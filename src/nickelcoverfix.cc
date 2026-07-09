// NickelCoverFix — offline cover reliability, zero DRM, zero decryption.
//
// The bug: Kobo keys a book's cover on its mutable store CoverImageId and, for purchased books, re-fetches
// it from the NETWORK on every RAM-cache loss (reboot/eviction) — it never reads the on-disk copy. So
// offline (or after a sync changes the CoverImageId) the cover blanks to the title/author PLACEHOLDER even
// though a perfectly good cover exists. This mod keeps a copy of the cover Kobo itself legitimately
// obtained — keyed by the STABLE ContentID — and serves it back instead of the placeholder.
//
// Three things, all reactive / user-driven (no boot-time enumeration):
//   CAPTURE  (hook getCoverImage)      — when a book is shown and our cache is missing for it, mirror its
//                                        cover: from the live QImage if real, else from the on-disk .parsed
//                                        if present. So browsing (online OR offline) backfills automatically.
//   SERVE    (hook generateDefaultCover)— when nickel would paint the placeholder, return our mirror instead.
//   REPAIR   (More > Repair book covers)— a chunked, progress-dialog pass that mirrors every book's on-disk
//                                        cover at once (covers you never scroll to; bulk backfill).
//
// Keyed by Content::getId() (stable) in .adds/nickelcoverfix/covers/<sha1(ContentID)>.png. .kobo-images is
// never written; remove the mod (or its .adds folder) to fully revert. If a cover is missing on disk, the
// user runs Kobo's own "Repair your Kobo account" (re-fetches covers to .kobo-images) then Repair book covers.
//
// BOOT SAFETY (must never hang a boot): purely reactive (one cover at a time, only when nickel renders it —
// at cold boot that's just the last-read book); no network/zip/crypto/DB/foreign-lock; fail-open (worst case
// = the placeholder); per-file size cap; and an ARM FLAG — every hook passes straight through until
// <config>/enabled exists, so installing it can't affect boot. Repair runs only on a menu tap (post-boot),
// chunked on the event loop, so it never blocks e-ink and a fault there is failsafe-recoverable.

#include <cstdint>
#include <functional>
#include <new>
#include <unistd.h>
#include <sys/stat.h>

#include <QString>
#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>
#include <QPair>
#include <QObject>
#include <QTimer>
#include <QPixmap>
#include <QCryptographicHash>

#include <NickelHook.h>

#include "config.h"
#include "util.h"
#include "ncfbridge.h"

static const char *const NCF_LIBNICKEL  = "/usr/local/Kobo/libnickel.so.1.0.0";
static const char *const NCF_COVERS_DIR = NCF_CONFIG_DIR "/covers";     // our ContentID-keyed mirror
static const char *const NCF_ARM_FLAG   = NCF_CONFIG_DIR "/enabled";    // presence = armed (boot-safety gate)
static const qint64      NCF_MAX_BYTES  = 8 * 1024 * 1024;             // per-cover sanity cap
static const char *const NCF_TYPES[]    = { "N3_LIBRARY_FULL", "N3_LIBRARY_GRID", "N3_LIBRARY_LIST" };

// opaque Kobo types — we only ever pass their addresses through to libnickel
struct KVolume;
typedef std::function<void(const KVolume &)> NcfVolumeFn;

// ---- resolved symbols (real functions we wrap) -------------------------------------------
static QImage (*real_getCoverImage)(const void *vol, const QString &imageId, bool b) = nullptr;
static QImage (*real_generateDefaultCover)(const void *vol, const QSize &size)        = nullptr;
static void   (*real_moreview_ctor)(void *self, void *parent)                         = nullptr;
// callables (dlsym)
static QString      (*content_getId)(const void *self)                                = nullptr;  // Content::getId() const (stable)
static const void  *(*device_current)()                                               = nullptr;  // Device::getCurrentDevice()
static QString      (*image_getFileName)(const void *dev, const void *vol, const QString &type) = nullptr; // Image::getFileName (static)
static void         (*vm_forEach)(const QString &filter, const NcfVolumeFn &fn)        = nullptr;  // VolumeManager::forEach
static void         (*n3pd_ctor)(void *self)                                           = nullptr;  // N3ProgressDialog()
static void         (*n3pd_setTitle)(void *self, const QString &s)                     = nullptr;
static void         (*n3pd_setProgress)(void *self, int v)                             = nullptr;
static void         (*n3pd_setRange)(void *self, int lo, int hi)                       = nullptr;
static void         (*n3pd_setBarVisible)(void *self, bool v)                          = nullptr;
static void         (*qwidget_show)(void *w)                                           = nullptr;
static void         (*qwidget_hide)(void *w)                                           = nullptr;
static void         (*iconbtn_ctor)(void *self, void *parent)                          = nullptr;
static void         (*iconbtn_setText)(void *self, const QString &s)                   = nullptr;
static void         (*iconbtn_setPixmap)(void *self, const QPixmap &p)                 = nullptr;
static void         (*qboxlayout_addWidget)(void *layout, void *w, int stretch, int align) = nullptr;
void               (*ncf_showOKDialog)(const QString &, const QString &)               = nullptr;  // extern (used by NcfBridge)

static NcfBridge *g_bridge = nullptr;

// ---- config / arming ---------------------------------------------------------------------
static bool ncf_armed()   { return access(NCF_ARM_FLAG, F_OK) == 0; }                       // live: touch/delete over USB
static bool ncf_enabled() { return ncf_global_config_bool("ncf_enabled", true); }           // master kill-switch
static bool ncf_capture() { return ncf_global_config_bool("ncf_capture", true);  }
static bool ncf_serve()   { return ncf_global_config_bool("ncf_serve",   true);  }
static bool ncf_menu()    { return ncf_global_config_bool("ncf_menu",    true);  }
static bool ncf_active()  { return ncf_armed() && ncf_enabled(); }

static int ncf_logbudget = 300;
#define NCF_TLOG(...) do { if (ncf_logbudget > 0) { ncf_logbudget--; NCF_LOG(__VA_ARGS__); } } while (0)

// getCoverImage sets this false, calls the real fn; a placeholder render (hooked generateDefaultCover)
// flips it true. Thread-local so nested/concurrent renders don't race.
static thread_local bool ncf_placeholder_path = false;

// once-per-session capture guard (mod-owned mutex only — never a nickel lock)
static QMutex        ncf_seen_mutex;
static QSet<QString> ncf_seen;

// ---- helpers -----------------------------------------------------------------------------
static QString ncf_mirror_path(const QString &cid) {
    const QByteArray h = QCryptographicHash::hash(cid.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromUtf8(NCF_COVERS_DIR) + QLatin1Char('/') + QString::fromUtf8(h) + QStringLiteral(".png");
}

static QImage ncf_load_mirror(const QString &cid) {
    const QString path = ncf_mirror_path(cid);
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() <= 0 || fi.size() > NCF_MAX_BYTES)
        return QImage();
    QImage img;
    if (!img.load(path))
        return QImage();
    return img;
}

// Load a source cover file and (re)write our mirror as PNG, atomically. Returns true on write.
static bool ncf_write_mirror_from(const QString &src, const QString &dst) {
    QFileInfo si(src);
    if (!si.exists() || si.size() <= 0 || si.size() > NCF_MAX_BYTES)
        return false;
    QImage img;
    if (!img.load(src) || img.isNull())
        return false;
    const QString tmp = dst + QStringLiteral(".tmp");
    if (!img.save(tmp, "PNG"))
        return false;
    QFile::remove(dst);
    if (!QFile::rename(tmp, dst)) { QFile::remove(tmp); return false; }
    return true;
}

// Find a book's on-disk cover (largest type first) via Kobo's own path builder.
static QString ncf_disk_cover_path(const void *dev, const void *vol) {
    if (!dev || !vol || !image_getFileName)
        return QString();
    for (const char *t : NCF_TYPES) {
        const QString p = image_getFileName(dev, vol, QString::fromUtf8(t));
        if (!p.isEmpty() && QFile::exists(p))
            return p;
    }
    return QString();
}

// ---- CAPTURE: mirror the cover Kobo already had (live QImage, else on-disk .parsed) ------
extern "C" __attribute__((visibility("default")))
QImage _ncf_getCoverImage(const void *vol, const QString &imageId, bool b) {
    if (!real_getCoverImage)
        return QImage();
    if (!ncf_active())
        return real_getCoverImage(vol, imageId, b);

    ncf_placeholder_path = false;
    QImage img = real_getCoverImage(vol, imageId, b);

    if (ncf_capture() && content_getId && vol) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            bool fresh = false;
            { QMutexLocker lock(&ncf_seen_mutex); fresh = !ncf_seen.contains(cid); if (fresh) ncf_seen.insert(cid); }
            if (fresh) {
                const QString dst = ncf_mirror_path(cid);
                if (!QFile::exists(dst)) {
                    bool ok = false;
                    if (!ncf_placeholder_path && !img.isNull()) {
                        ok = img.save(dst, "PNG");                      // live real cover
                    } else if (device_current) {
                        const QString src = ncf_disk_cover_path(device_current(), vol);  // else grab from disk
                        if (!src.isEmpty()) ok = ncf_write_mirror_from(src, dst);
                    }
                    if (ok) NCF_TLOG("capture cid=%s -> mirrored", qPrintable(cid));
                }
            }
        }
    }
    return img;
}

// ---- SERVE: return our mirror instead of the placeholder ---------------------------------
extern "C" __attribute__((visibility("default")))
QImage _ncf_generateDefaultCover(const void *vol, const QSize &size) {
    ncf_placeholder_path = true;                               // tell the capture hook this render was a placeholder

    if (ncf_active() && ncf_serve() && content_getId && vol) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            QImage m = ncf_load_mirror(cid);
            if (!m.isNull()) {
                QImage out = (size.isValid() && !size.isEmpty() && m.size() != size)
                    ? m.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    : m;
                NCF_TLOG("serve cid=%s -> mirror (%dx%d)", qPrintable(cid), out.width(), out.height());
                return out;
            }
        }
    }
    return real_generateDefaultCover ? real_generateDefaultCover(vol, size) : QImage();
}

// ---- More > "Repair book covers" row -----------------------------------------------------
extern "C" __attribute__((visibility("default")))
void _ncf_MoreView_ctor(void *self, void *parent) {
    if (real_moreview_ctor) real_moreview_ctor(self, parent);
    if (!(ncf_active() && ncf_menu()) || !self || !g_bridge) return;
    void *ui = *(void **)((char *)self + 0x18);
    if (!ui) return;
    void *container = *(void **)((char *)ui + 0x04);
    void *inner     = *(void **)((char *)ui + 0x08);
    if (!container || !inner || !iconbtn_ctor || !qboxlayout_addWidget) return;
    void *row = ::operator new(0x58);
    if (!row) return;
    iconbtn_ctor(row, container);
    if (iconbtn_setText)   iconbtn_setText(row, QStringLiteral("Repair book covers"));
    if (iconbtn_setPixmap) { QPixmap icon(QStringLiteral(":/images/fte/missingbooks_repair.png")); iconbtn_setPixmap(row, icon); }
    qboxlayout_addWidget(inner, row, 0, 0);
    if (qwidget_show) qwidget_show(row);
    QObject::connect((QObject *)row, "2tapped()", g_bridge, "1onRepairTapped()");
    NCF_LOG("MoreView: appended 'Repair book covers' row");
}

// ---- NcfBridge slots (defined here so they can reach the resolved symbols) ----------------
void NcfBridge::onRepairTapped() {
    if (m_running) return;
    if (!(content_getId && image_getFileName && device_current && vm_forEach)) {
        if (ncf_showOKDialog) ncf_showOKDialog(QStringLiteral("Repair book covers"),
            QStringLiteral("This isn't available on your firmware."));
        return;
    }
    QDir().mkpath(QString::fromUtf8(NCF_COVERS_DIR));

    // 1) enumerate the library (main thread, warm cache) -> work-list of (on-disk cover, our mirror)
    m_work.clear(); m_idx = 0; m_copied = 0;
    const void *dev = device_current();
    NcfVolumeFn fn = [dev, this](const KVolume &v) {
        const void *vol = reinterpret_cast<const void *>(&v);
        const QString cid = content_getId(vol);
        if (cid.isEmpty()) return;
        const QString src = ncf_disk_cover_path(dev, vol);
        if (src.isEmpty()) return;                              // nothing on disk (Repair-my-account can fill it)
        m_work.append(qMakePair(src, ncf_mirror_path(cid)));
    };
    vm_forEach(QString(), fn);

    // 2) progress dialog
    m_dlg = nullptr;
    if (n3pd_ctor) {
        m_dlg = ::operator new(0x78);                          // sizeof(N3ProgressDialog)
        n3pd_ctor(m_dlg);
        if (n3pd_setTitle)      n3pd_setTitle(m_dlg, QStringLiteral("Repairing book covers…"));
        if (n3pd_setBarVisible) n3pd_setBarVisible(m_dlg, true);
        if (n3pd_setRange)      n3pd_setRange(m_dlg, 0, m_work.isEmpty() ? 1 : m_work.size());
        if (n3pd_setProgress)   n3pd_setProgress(m_dlg, 0);
        if (qwidget_show)       qwidget_show(m_dlg);
    }

    // 3) drive the copy in chunks on the event loop
    m_running = true;
    QTimer *t = new QTimer(this);
    m_timer = t;
    t->setInterval(0);
    QObject::connect(t, SIGNAL(timeout()), this, SLOT(onTick()));
    t->start();
    NCF_LOG("repair: %d book(s) with an on-disk cover to mirror", m_work.size());
}

void NcfBridge::onTick() {
    const int BATCH = 4;
    for (int n = 0; n < BATCH && m_idx < m_work.size(); ++n, ++m_idx) {
        const QString &src = m_work[m_idx].first;
        const QString &dst = m_work[m_idx].second;
        QFileInfo di(dst), si(src);
        if (!di.exists() || si.lastModified() > di.lastModified()) {   // repair: create or refresh
            if (ncf_write_mirror_from(src, dst)) m_copied++;
        }
    }
    if (n3pd_setProgress && m_dlg) n3pd_setProgress(m_dlg, m_idx);

    if (m_idx >= m_work.size()) {
        if (m_timer) { static_cast<QTimer *>(m_timer)->stop(); m_timer->deleteLater(); m_timer = nullptr; }
        if (m_dlg) {
            if (qwidget_hide) qwidget_hide(m_dlg);
            static_cast<QObject *>(m_dlg)->deleteLater();
            m_dlg = nullptr;
        }
        const int total = m_work.size();
        m_running = false;
        NCF_LOG("repair: done, mirrored %d of %d", m_copied, total);
        if (ncf_showOKDialog)
            ncf_showOKDialog(QStringLiteral("Repair book covers"),
                QString(QStringLiteral("Cached %1 of %2 cover(s). They'll now show offline. If books are still "
                        "missing a cover, go online and run Settings > Device information > \"Repair your Kobo "
                        "account\", then tap Repair book covers again.")).arg(m_copied).arg(total));
    }
}

// ---- init / uninstall --------------------------------------------------------------------
static int ncf_init() {
    ncf_global_config_get("");                                // force config parse (writes default on first run)
    mkdir(NCF_CONFIG_DIR, 0755);
    QDir().mkpath(QString::fromUtf8(NCF_COVERS_DIR));
    g_bridge = new NcfBridge(nullptr);
    NCF_LOG("startup(v1): armed=%d enabled=%d capture=%d serve=%d menu=%d",
            ncf_armed(), ncf_enabled(), ncf_capture(), ncf_serve(), ncf_menu());
    NCF_LOG("startup(v1): getCoverImage=%p generateDefaultCover=%p moreview=%p bridge=%p",
            (void*)real_getCoverImage, (void*)real_generateDefaultCover, (void*)real_moreview_ctor, (void*)g_bridge);
    NCF_LOG("startup(v1): getId=%p device=%p getFileName=%p forEach=%p n3pd=%p showOK=%p",
            (void*)content_getId, (void*)device_current, (void*)image_getFileName, (void*)vm_forEach,
            (void*)n3pd_ctor, (void*)ncf_showOKDialog);
    if (!ncf_armed())
        NCF_LOG("NOT ARMED: passing through. Create %s/enabled and reboot to activate.", NCF_CONFIG_DIR_DISP);
    return 0;
}

static bool ncf_del(const char *p) { return access(p, F_OK) != 0 ? true : nh_delete_file(p); }
static bool ncf_uninstall() {
    NCF_LOG("uninstall: removing NickelCoverFix files + our cover mirror (.kobo-images never touched)");
    bool ok = true;
    QDir(QString::fromUtf8(NCF_COVERS_DIR)).removeRecursively();
    ok = ncf_del(NCF_ARM_FLAG) && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/config") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/default") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/doc") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/nickelcoverfix.log") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/uninstall") && ok;
    if (access(NCF_CONFIG_DIR, F_OK) == 0) ok = nh_delete_dir(NCF_CONFIG_DIR) && ok;
    return ok;
}

// ---- NickelHook wiring -------------------------------------------------------------------
static struct nh_info NickelCoverFixInfo = {
    .name            = "NickelCoverFix",
    .desc            = "v1: serve a cached cover instead of the placeholder + More>Repair book covers (no DRM).",
    .uninstall_flag  = NCF_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NCF_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 3,
};

static struct nh_hook NickelCoverFixHooks[] = {
    { .sym = "_ZN16VolumePixmapView13getCoverImageERK6VolumeRK7QStringb", .sym_new = "_ncf_getCoverImage",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_getCoverImage),
      .desc = "capture the real cover into .adds", .optional = true },
    { .sym = "_ZN16VolumePixmapView20generateDefaultCoverERK6VolumeRK5QSize", .sym_new = "_ncf_generateDefaultCover",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_generateDefaultCover),
      .desc = "serve mirror instead of placeholder", .optional = true },
    { .sym = "_ZN8MoreViewC1EP7QWidget", .sym_new = "_ncf_MoreView_ctor",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_moreview_ctor),
      .desc = "append Repair book covers row", .optional = true },
    {0},
};

static struct nh_dlsym NickelCoverFixDlsym[] = {
    { .name = "_ZNK7Content5getIdEv", .out = nh_symoutptr(content_getId), .desc = "Content::getId (stable ContentID)", .optional = true },
    { .name = "_ZN6Device16getCurrentDeviceEv", .out = nh_symoutptr(device_current), .desc = "Device::getCurrentDevice", .optional = true },
    { .name = "_ZN5Image11getFileNameERK6DeviceRK6VolumeRK7QString", .out = nh_symoutptr(image_getFileName), .desc = "Image::getFileName", .optional = true },
    { .name = "_ZN13VolumeManager7forEachERK7QStringRKSt8functionIFvRK6VolumeEE", .out = nh_symoutptr(vm_forEach), .desc = "VolumeManager::forEach", .optional = true },
    { .name = "_ZN16N3ProgressDialogC1Ev", .out = nh_symoutptr(n3pd_ctor), .desc = "N3ProgressDialog ctor", .optional = true },
    { .name = "_ZN16N3ProgressDialog8setTitleERK7QString", .out = nh_symoutptr(n3pd_setTitle), .desc = "N3ProgressDialog::setTitle", .optional = true },
    { .name = "_ZN16N3ProgressDialog11setProgressEi", .out = nh_symoutptr(n3pd_setProgress), .desc = "N3ProgressDialog::setProgress", .optional = true },
    { .name = "_ZN16N3ProgressDialog19setProgressBarRangeEii", .out = nh_symoutptr(n3pd_setRange), .desc = "N3ProgressDialog::setProgressBarRange", .optional = true },
    { .name = "_ZN16N3ProgressDialog21setProgressBarVisibleEb", .out = nh_symoutptr(n3pd_setBarVisible), .desc = "N3ProgressDialog::setProgressBarVisible", .optional = true },
    { .name = "_ZN7QWidget4showEv", .out = nh_symoutptr(qwidget_show), .desc = "QWidget::show", .optional = true },
    { .name = "_ZN7QWidget4hideEv", .out = nh_symoutptr(qwidget_hide), .desc = "QWidget::hide", .optional = true },
    { .name = "_ZN14IconLeftButtonC1EP7QWidget", .out = nh_symoutptr(iconbtn_ctor), .desc = "IconLeftButton ctor", .optional = true },
    { .name = "_ZN14IconLeftButton7setTextERK7QString", .out = nh_symoutptr(iconbtn_setText), .desc = "IconLeftButton::setText", .optional = true },
    { .name = "_ZN14IconLeftButton9setPixmapERK7QPixmap", .out = nh_symoutptr(iconbtn_setPixmap), .desc = "IconLeftButton::setPixmap", .optional = true },
    { .name = "_ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE", .out = nh_symoutptr(qboxlayout_addWidget), .desc = "QBoxLayout::addWidget", .optional = true },
    { .name = "_ZN25ConfirmationDialogFactory12showOKDialogERK7QStringS2_", .out = nh_symoutptr(ncf_showOKDialog), .desc = "showOKDialog", .optional = true },
    {0},
};

NickelHook(
    .init      = &ncf_init,
    .info      = &NickelCoverFixInfo,
    .hook      = NickelCoverFixHooks,
    .dlsym     = NickelCoverFixDlsym,
    .uninstall = &ncf_uninstall,
)
