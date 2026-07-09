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
// = the placeholder); per-file size cap; work is a cheap raw copy or a guarded encode. Disable over USB with
// ncf_enabled:0. Repair runs only on a menu tap (post-boot), chunked on the event loop, so it never blocks
// e-ink and a fault there is failsafe-recoverable.

#include <cstdint>
#include <functional>
#include <new>
#include <unistd.h>
#include <sys/stat.h>

#include <QString>
#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <QSize>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QHash>
#include <QVector>
#include <QPair>
#include <QObject>
#include <QTimer>
#include <QPixmap>
#include <QCryptographicHash>
#include <QApplication>
#include <QRegularExpression>
#include <QWidget>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QBoxLayout>

#include <NickelHook.h>

#include "config.h"
#include "util.h"
#include "ncfbridge.h"

static const char *const NCF_LIBNICKEL  = "/usr/local/Kobo/libnickel.so.1.0.0";
static const char *const NCF_COVERS_DIR = NCF_CONFIG_DIR "/covers";     // our ContentID-keyed mirror
static const qint64      NCF_MAX_BYTES  = 8 * 1024 * 1024;             // per-cover sanity cap
// two cover sizes: library grid/list (small) and the lock/sleep screen (full device res)
static const char *const NCF_TYPES_LIB[]  = { "N3_LIBRARY_FULL", "N3_LIBRARY_GRID", "N3_LIBRARY_LIST" };
static const char *const NCF_TYPES_LOCK[] = { "N3_FULL", "N3_LIBRARY_FULL" };
static const int         NCF_LOCK_MIN     = 700;                       // requested max(w,h) above this = lock screen

// opaque Kobo types — we only ever pass their addresses through to libnickel
struct KVolume;
typedef std::function<void(const KVolume &)> NcfVolumeFn;

// ---- resolved symbols (real functions we wrap) -------------------------------------------
static QImage (*real_getCoverImage)(const void *vol, const QString &imageId, bool b) = nullptr;
static QImage (*real_generateDefaultCover)(const void *vol, const QSize &size)        = nullptr;
static void   (*real_moreview_ctor)(void *self, void *parent)                         = nullptr;
static void   (*real_loadCover)(void *self)                                           = nullptr;  // VolumePixmapView::loadCover
static void   (*real_setContent)(void *self, const void *vol, const QString &s)        = nullptr;  // VolumePixmapView::setContent
static void   (*real_imageReady)(void *self, const QImage &img, const void *vol, const QString &s) = nullptr; // VolumePixmapView::imageReady
static void   (*real_sbhw_setContent)(void *self, const void *vol)                     = nullptr;  // SingleBookHomeWidget::setContent(Volume const&)
// callables (dlsym)
static void   (*vpv_loadDefaultCover)(void *self)                                     = nullptr;  // VolumePixmapView::loadDefaultCover
static bool   (*content_isPurchaseable)(const void *self)                             = nullptr;  // Content::isPurchaseable() const
static const void *(*vpv_getVolume)(const void *self)                                 = nullptr;  // VolumePixmapView::getVolume() (this+0xac, by symbol)
static void   (*bcv_setImage)(void *self, const QImage &img, const QString &label)     = nullptr;  // BookCoverView::setImage (vtable-only -> can't hook, dlsym + call directly)
static QString      (*content_getId)(const void *self)                                = nullptr;  // Content::getId() const (stable)
static const void  *(*device_current)()                                               = nullptr;  // Device::getCurrentDevice()
static QString      (*image_getFileName)(const void *dev, const void *vol, const QString &type) = nullptr; // Image::getFileName (static)
static void         (*vm_forEach)(const QString &filter, const NcfVolumeFn &fn)        = nullptr;  // VolumeManager::forEach
static void         (*iconbtn_ctor)(void *self, void *parent)                          = nullptr;
static void         (*iconbtn_setText)(void *self, const QString &s)                   = nullptr;
static void         (*iconbtn_setPixmap)(void *self, const QPixmap &p)                 = nullptr;
static void         (*qboxlayout_addWidget)(void *layout, void *w, int stretch, int align) = nullptr;
void               (*ncf_showOKDialog)(const QString &, const QString &)               = nullptr;  // extern (used by NcfBridge)

static NcfBridge *g_bridge = nullptr;

// ---- config ------------------------------------------------------------------------------
static bool ncf_enabled() { return ncf_global_config_bool("ncf_enabled", true); }           // master kill-switch
static bool ncf_capture() { return ncf_global_config_bool("ncf_capture", true);  }
static bool ncf_serve()   { return ncf_global_config_bool("ncf_serve",   true);  }
static bool ncf_menu()    { return ncf_global_config_bool("ncf_menu",    true);  }
static bool ncf_active()  { return ncf_enabled(); }
// ncf_force_serve (default ON): "always our copy or placeholder" — generate + show our cover for every book,
//   bypassing Kobo's live path. Set 0 for the graceful fallback (Kobo's real cover when available, our copy
//   only to rescue placeholders). ncf_debug_dot (default off): stamp served covers with a bullseye.
static bool ncf_force_serve() { return ncf_global_config_bool("ncf_force_serve", true);  }
static bool ncf_debug_dot()   { return ncf_global_config_bool("ncf_debug_dot",   false); }

static int ncf_logbudget = 300;
#define NCF_TLOG(...) do { if (ncf_logbudget > 0) { ncf_logbudget--; NCF_LOG(__VA_ARGS__); } } while (0)

// once-per-session capture guard (mod-owned mutex only — never a nickel lock)
static QMutex        ncf_seen_mutex;
static QSet<QString> ncf_seen;

// per-cover-widget override decision: does this widget's book have a cover we can produce? (set in
// setContent, read in loadCover). Keyed by widget pointer; widgets are recycled so the map stays small.
static QMutex             ncf_ov_mutex;
static QHash<void *, bool> ncf_override;

// ---- helpers -----------------------------------------------------------------------------
static QString ncf_mirror_base(const QString &cid) {
    const QByteArray h = QCryptographicHash::hash(cid.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromUtf8(NCF_COVERS_DIR) + QLatin1Char('/') + QString::fromUtf8(h);
}
static QString ncf_mirror_path(const QString &cid)      { return ncf_mirror_base(cid) + QStringLiteral(".png"); }       // library
static QString ncf_mirror_path_lock(const QString &cid) { return ncf_mirror_base(cid) + QStringLiteral("-lock.jpg"); } // lock screen

static QImage ncf_load_mirror_file(const QString &path) {
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() <= 0 || fi.size() > NCF_MAX_BYTES)
        return QImage();
    QImage img;
    if (!img.load(path))
        return QImage();
    return img;
}
static QImage ncf_load_mirror(const QString &cid) { return ncf_load_mirror_file(ncf_mirror_path(cid)); }

// Pick the right mirror for the requested size: the big lock-screen cover for large requests, the library
// cover otherwise — falling back to whichever exists.
static QImage ncf_serve_mirror(const QString &cid, const QSize &size) {
    const bool wantLock = qMax(size.width(), size.height()) > NCF_LOCK_MIN;
    QImage m = wantLock ? ncf_load_mirror_file(ncf_mirror_path_lock(cid)) : ncf_load_mirror(cid);
    if (!m.isNull()) return m;
    return wantLock ? ncf_load_mirror(cid) : ncf_load_mirror_file(ncf_mirror_path_lock(cid));
}

// Debug marker: a big black/white/black bullseye in the center — visible on light AND dark covers.
static void ncf_draw_dot(QImage &img) {
    if (img.isNull()) return;
    if (img.format() != QImage::Format_ARGB32 && img.format() != QImage::Format_RGB32)
        img = img.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int r = qMin(img.width(), img.height()) / 4;
    const QPoint c(img.width() / 2, img.height() / 2);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);  p.drawEllipse(c, r, r);
    p.setBrush(Qt::white);  p.drawEllipse(c, r * 3 / 5, r * 3 / 5);
    p.setBrush(Qt::black);  p.drawEllipse(c, r / 4, r / 4);
    p.end();
}

// Mirror a source cover to dst, atomically. Normal mode = a raw byte copy of Kobo's .parsed (which is
// already a JPEG) — no decode/re-encode, no quality loss, fast. Debug-dot mode decodes to stamp the marker.
static bool ncf_write_mirror_from(const QString &src, const QString &dst) {
    QFileInfo si(src);
    if (!si.exists() || si.size() <= 0 || si.size() > NCF_MAX_BYTES)
        return false;
    const QString tmp = dst + QStringLiteral(".tmp");
    QFile::remove(tmp);
    bool ok;
    if (ncf_debug_dot()) {                                      // decode -> stamp bullseye -> re-encode
        QImage img;
        if (!img.load(src) || img.isNull())
            return false;
        ncf_draw_dot(img);
        const bool jpg = dst.endsWith(QLatin1String(".jpg"));
        ok = img.save(tmp, jpg ? "JPG" : "PNG", jpg ? 88 : -1);
    } else {                                                    // fast path: straight copy, no re-encode
        ok = QFile::copy(src, tmp);
    }
    if (!ok)
        return false;
    QFile::remove(dst);
    if (!QFile::rename(tmp, dst)) { QFile::remove(tmp); return false; }
    return true;
}

// Find a book's best on-disk cover from a preference-ordered type list (biggest first).
static QString ncf_disk_cover_path(const void *dev, const void *vol, const char *const *types, int ntypes) {
    if (!dev || !vol || !image_getFileName)
        return QString();
    for (int i = 0; i < ntypes; ++i) {
        const QString p = image_getFileName(dev, vol, QString::fromUtf8(types[i]));
        if (!p.isEmpty() && QFile::exists(p))
            return p;
    }
    return QString();
}

// Auto-mirror a book's on-disk cover as it's rendered — once per book+size per session, skip if we have it.
// lock=false -> library size (grid path); lock=true -> full lock-screen size (sleep/read path).
static void ncf_autocapture(const void *vol, bool lock) {
    if (!vol || !content_getId || !device_current || !image_getFileName)
        return;
    const QString cid = content_getId(vol);
    if (cid.isEmpty())
        return;
    const QString key = cid + (lock ? QLatin1String("#L") : QLatin1String("#G"));
    { QMutexLocker lk(&ncf_seen_mutex); if (ncf_seen.contains(key)) return; ncf_seen.insert(key); }
    const QString dst = lock ? ncf_mirror_path_lock(cid) : ncf_mirror_path(cid);
    if (QFile::exists(dst))
        return;
    const QString src = lock ? ncf_disk_cover_path(device_current(), vol, NCF_TYPES_LOCK, 2)
                             : ncf_disk_cover_path(device_current(), vol, NCF_TYPES_LIB, 3);
    if (!src.isEmpty() && ncf_write_mirror_from(src, dst))
        NCF_TLOG("autocapture cid=%s -> %s", qPrintable(cid), lock ? "lock" : "lib");
}

// ---- CAPTURE: mirror the cover Kobo has on disk as books render ---------------------------
extern "C" __attribute__((visibility("default")))
QImage _ncf_getCoverImage(const void *vol, const QString &imageId, bool b) {
    if (!real_getCoverImage)
        return QImage();
    if (!ncf_active())
        return real_getCoverImage(vol, imageId, b);

    QImage img = real_getCoverImage(vol, imageId, b);

    if (ncf_capture()) ncf_autocapture(vol, /*lock*/ true);   // sleep/read context -> full lock-screen mirror
    // debug/verification: force our mirror onto every book that has one, so coverage is visible everywhere
    // (books with a dot = mirrored; books without = not yet). Overrides the real cover — debug only.
    if (ncf_force_serve() && content_getId && vol) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            QImage m = ncf_serve_mirror(cid, img.size());
            if (!m.isNull()) {
                QImage out = (!img.isNull() && img.size().isValid() && !img.size().isEmpty() && m.size() != img.size())
                    ? m.scaled(img.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    : m;
                return out;
            }
        }
    }
    return img;
}

// ---- SERVE: return our mirror instead of the placeholder ---------------------------------
extern "C" __attribute__((visibility("default")))
QImage _ncf_generateDefaultCover(const void *vol, const QSize &size) {
    if (ncf_active() && ncf_serve() && content_getId && vol) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            // generate on demand (cheap raw copy) if we don't have it yet, then serve. Always ensure the
            // library mirror (any disk type), plus the lock mirror for big requests — so a book with ANY
            // on-disk cover always yields a mirror. (The home "Now Reading" reaches us only here, via
            // loadDefaultCover@plt->generateDefaultCover@plt; its setContent/loadCover are vtable-dispatched
            // and NickelHook's GOT patch can't catch those, so this is where we must produce the cover.)
            if (ncf_capture()) {
                ncf_autocapture(vol, /*lock*/ false);
                if (qMax(size.width(), size.height()) > NCF_LOCK_MIN)
                    ncf_autocapture(vol, /*lock*/ true);
            }
            QImage m = ncf_serve_mirror(cid, size);
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

// ---- OVERRIDE (scoped): show our copy for library books we can produce; else let Kobo's path run -------
// The grid gets real covers async via onImageReady (virtual, unhookable). With force_serve on we route
// loadCover -> loadDefaultCover -> our serve, BUT only for books setContent flagged "we can produce a cover"
// — so store items, recommendations, previews and new/cloud books fall through to Kobo untouched.
extern "C" __attribute__((visibility("default")))
void _ncf_loadCover(void *self) {
    if (ncf_active() && ncf_force_serve() && vpv_loadDefaultCover && self) {
        bool ov = false;
        { QMutexLocker lk(&ncf_ov_mutex); ov = ncf_override.value(self, false); }
        if (ov) { vpv_loadDefaultCover(self); return; }
    }
    if (real_loadCover) real_loadCover(self);
}

// ---- CAPTURE + SCOPE (grid): mirror the book's cover, and decide whether we may override its widget -----
// setContent(Volume const&, QString const&) hands us the book by-symbol — no struct offset needed.
extern "C" __attribute__((visibility("default")))
void _ncf_setContent(void *self, const void *vol, const QString &shortcoverId) {
    if (real_setContent) real_setContent(self, vol, shortcoverId);
    bool canProduce = false;
    if (ncf_active() && vol && content_getId) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            if (ncf_capture()) ncf_autocapture(vol, /*lock*/ false);   // generate the library mirror if we can
            // "can we produce a cover?" — we already have a mirror, or Kobo has one on disk to copy.
            // False for store/recommendation/preview/new items (nothing in .kobo-images) -> they fall through.
            canProduce = QFile::exists(ncf_mirror_path(cid))
                      || (device_current && image_getFileName
                          && !ncf_disk_cover_path(device_current(), vol, NCF_TYPES_LIB, 3).isEmpty());
        }
    }
    if (self) { QMutexLocker lk(&ncf_ov_mutex); ncf_override.insert(self, canProduce); }
}

// ---- CAPTURE (rendered): mirror the cover Kobo actually drew, so RAM-only covers (no .parsed on disk)
// get saved too. Scoped to books you OWN (isPurchaseable == false) so store/recommendation covers are never
// cached. This is the "fallback" for covers we can't raw-copy from disk.
extern "C" __attribute__((visibility("default")))
void _ncf_imageReady(void *self, const QImage &img, const void *vol, const QString &s) {
    if (real_imageReady) real_imageReady(self, img, vol, s);
    if (!(ncf_active() && ncf_capture()) || img.isNull() || !vol || !content_getId)
        return;
    if (content_isPurchaseable && content_isPurchaseable(vol))   // store/recommendation item -> leave alone
        return;
    const QString cid = content_getId(vol);
    if (cid.isEmpty())
        return;
    const QString dst = ncf_mirror_path(cid);
    bool fresh = false;
    { QMutexLocker lk(&ncf_seen_mutex); const QString k = cid + QLatin1String("#R");
      fresh = !ncf_seen.contains(k); if (fresh) ncf_seen.insert(k); }
    if (!fresh || QFile::exists(dst))                            // already mirrored (raw copy or earlier)
        return;
    QImage out = img;                                           // rendered cover — encode it (no disk source to copy)
    if (ncf_debug_dot()) ncf_draw_dot(out);
    const QString tmp = dst + QStringLiteral(".tmp");
    QFile::remove(tmp);
    if (out.save(tmp, "JPG", 85)) {
        QFile::remove(dst);
        if (!QFile::rename(tmp, dst)) QFile::remove(tmp);
        else NCF_TLOG("ram-capture cid=%s (%dx%d)", qPrintable(cid), img.width(), img.height());
    }
}

// ---- HOME "Now Reading" (SingleBookHomeWidget): force our cover onto the embedded BookCoverView ---------
// The home widget dispatches its cover leaf-calls (setContent/loadCover/setImage) through the vtable, which
// NickelHook's GOT patch can't intercept — so none of the VolumePixmapView hooks above fire for it, and the
// home stays a placeholder while the grid works (confirmed on-device). But SingleBookHomeWidget::setContent
// IS reached via PLT (from FourBookHomeWidget::configure), so we catch it here. After Kobo configures the
// widget we locate its BookCoverView child through the Qt object tree (no struct offset) and push our mirror
// straight in via BookCoverView::setImage — which is itself unhookable (vtable-only, no GOT entry), so we
// dlsym it and call it directly. Note: home books report isPurchaseable==true, so we do NOT gate on that.
extern "C" __attribute__((visibility("default")))
void _ncf_SingleBookHomeWidget_setContent(void *self, const void *vol) {
    if (real_sbhw_setContent) real_sbhw_setContent(self, vol);
    if (!(ncf_active() && ncf_serve()) || !self || !vol || !content_getId || !bcv_setImage)
        return;
    const QString cid = content_getId(vol);
    if (cid.isEmpty())
        return;
    if (ncf_capture()) { ncf_autocapture(vol, /*lock*/ false); ncf_autocapture(vol, /*lock*/ true); }
    const QImage m = ncf_serve_mirror(cid, QSize(4096, 4096));         // prefer the full lock mirror, fall back to library
    if (m.isNull())
        return;
    // locate the embedded BookCoverView via the Qt object tree — offset-free, robust to layout changes
    QObject *bcv = nullptr;
    for (QObject *c : reinterpret_cast<QObject *>(self)->findChildren<QObject *>())
        if (c && qstrcmp(c->metaObject()->className(), "BookCoverView") == 0) { bcv = c; break; }
    if (!bcv)
        return;
    bcv_setImage(bcv, m, cid);
    NCF_TLOG("home-serve cid=%s -> BookCoverView (%dx%d)", qPrintable(cid), m.width(), m.height());
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
    if (iconbtn_setText)   iconbtn_setText(row, QStringLiteral("Repair Book Covers"));
    if (iconbtn_setPixmap) { QPixmap icon(QStringLiteral(":/images/menu/info.png")); iconbtn_setPixmap(row, icon); }
    qboxlayout_addWidget(inner, row, 0, 0);
    static_cast<QWidget *>(row)->show();
    QObject::connect((QObject *)row, "2tapped()", g_bridge, "1onRepairTapped()");
    NCF_LOG("MoreView: appended 'Repair Book Covers' row");
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

    // 1) enumerate the library (main thread, warm cache) -> work-list of
    //    [ librarySrc, libraryDst, lockSrc, lockDst ] per book
    m_work.clear(); m_idx = 0; m_copied = 0;
    const void *dev = device_current();
    NcfVolumeFn fn = [dev, this](const KVolume &v) {
        const void *vol = reinterpret_cast<const void *>(&v);
        const QString cid = content_getId(vol);
        if (cid.isEmpty()) return;
        const QString libSrc  = ncf_disk_cover_path(dev, vol, NCF_TYPES_LIB, 3);
        const QString lockSrc = ncf_disk_cover_path(dev, vol, NCF_TYPES_LOCK, 2);
        if (libSrc.isEmpty() && lockSrc.isEmpty()) return;     // nothing on disk (Repair-my-account can fill it)
        m_work.append(QStringList{ libSrc, ncf_mirror_path(cid), lockSrc, ncf_mirror_path_lock(cid) });
    };
    vm_forEach(QString(), fn);

    // 2) our own compact progress dialog (QtWidgets) — small + centered, no full-screen white takeover
    m_dlg = nullptr; m_bar = nullptr;
    {
        QWidget *parent = QApplication::activeWindow();
        QDialog *dlg = new QDialog(parent);
        dlg->setObjectName(QStringLiteral("ncfRepairDialog"));
        QVBoxLayout *lay = new QVBoxLayout(dlg);
        lay->setContentsMargins(52, 46, 52, 56);
        lay->setSpacing(26);
        QLabel *lbl = new QLabel(QStringLiteral("Repairing Book Covers…"), dlg);
        lbl->setObjectName(QStringLiteral("ncfTitle"));
        lbl->setAlignment(Qt::AlignCenter);
        QProgressBar *bar = new QProgressBar(dlg);
        bar->setRange(0, m_work.isEmpty() ? 1 : m_work.size());
        bar->setValue(0);
        bar->setTextVisible(true);
        QLabel *desc = new QLabel(QStringLiteral(
            "Caching your book covers so they still show when you're offline. This can take a while."), dlg);
        desc->setObjectName(QStringLiteral("ncfDesc"));
        desc->setAlignment(Qt::AlignCenter);
        desc->setWordWrap(true);
        lay->addWidget(lbl);
        lay->addWidget(bar);
        lay->addWidget(desc);
        // match the Kobo UI font: Kobo sets it via the app stylesheet (* { font-family: ... }); read it back.
        QString ff = qApp->font().family();
        {
            const QRegularExpressionMatch m =
                QRegularExpression(QStringLiteral("font-family\\s*:\\s*([^;}\\r\\n]+)")).match(qApp->styleSheet());
            if (m.hasMatch()) ff = m.captured(1).trimmed();
        }
        if (ff.isEmpty()) ff = QStringLiteral("sans-serif");
        dlg->setStyleSheet(QString(QStringLiteral(
            "#ncfRepairDialog{background:#ffffff;border:3px solid #000000;}"
            "#ncfRepairDialog, #ncfRepairDialog *{font-family:%1;}"
            "#ncfTitle{color:#000000;font-size:40px;font-weight:bold;}"
            "#ncfDesc{color:#000000;font-size:27px;}"
            "QProgressBar{border:2px solid #000000;background:#ffffff;color:#000000;"
            "font-size:30px;min-height:54px;text-align:center;}"
            "QProgressBar::chunk{background:#000000;}")).arg(ff));
        NCF_LOG("repair dialog font-family: %s", qPrintable(ff));
        dlg->resize(860, 360);
        if (parent) dlg->move(parent->geometry().center() - dlg->rect().center());
        dlg->show();
        dlg->raise();
        m_dlg = dlg;
        m_bar = bar;
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
        const QStringList &e = m_work[m_idx];              // [ librarySrc, libraryDst, lockSrc, lockDst ]
        bool any = false;
        if (!e[0].isEmpty() && ncf_write_mirror_from(e[0], e[1])) any = true;   // library mirror
        if (!e[2].isEmpty() && ncf_write_mirror_from(e[2], e[3])) any = true;   // lock-screen mirror
        if (any) m_copied++;                               // Repair always regenerates both
    }
    if (m_bar) static_cast<QProgressBar *>(m_bar)->setValue(m_idx);

    if (m_idx >= m_work.size()) {
        if (m_timer) { static_cast<QTimer *>(m_timer)->stop(); m_timer->deleteLater(); m_timer = nullptr; }
        if (m_dlg) {
            static_cast<QDialog *>(m_dlg)->hide();
            static_cast<QDialog *>(m_dlg)->deleteLater();     // deletes child label/bar/layout too
            m_dlg = nullptr; m_bar = nullptr;
        }
        const int total = m_work.size();
        m_running = false;
        NCF_LOG("repair: done, mirrored %d of %d", m_copied, total);
        if (ncf_showOKDialog)
            ncf_showOKDialog(QStringLiteral("Repair Book Covers"),
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
    NCF_LOG("startup(v1): enabled=%d capture=%d serve=%d menu=%d force=%d",
            ncf_enabled(), ncf_capture(), ncf_serve(), ncf_menu(), ncf_force_serve());
    NCF_LOG("startup(v1): getCoverImage=%p generateDefaultCover=%p moreview=%p bridge=%p sbhw_setContent=%p bcv_setImage=%p",
            (void*)real_getCoverImage, (void*)real_generateDefaultCover, (void*)real_moreview_ctor, (void*)g_bridge,
            (void*)real_sbhw_setContent, (void*)bcv_setImage);
    NCF_LOG("startup(v1): getId=%p device=%p getFileName=%p forEach=%p showOK=%p",
            (void*)content_getId, (void*)device_current, (void*)image_getFileName, (void*)vm_forEach,
            (void*)ncf_showOKDialog);
    return 0;
}

static bool ncf_del(const char *p) { return access(p, F_OK) != 0 ? true : nh_delete_file(p); }
static bool ncf_uninstall() {
    NCF_LOG("uninstall: removing NickelCoverFix files + our cover mirror (.kobo-images never touched)");
    bool ok = true;
    QDir(QString::fromUtf8(NCF_COVERS_DIR)).removeRecursively();
    ok = ncf_del(NCF_CONFIG_DIR "/enabled") && ok;   // remove any leftover arm flag from older builds
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
    { .sym = "_ZN16VolumePixmapView9loadCoverEv", .sym_new = "_ncf_loadCover",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_loadCover),
      .desc = "debug force_serve: route grid covers through the default path", .optional = true },
    { .sym = "_ZN16VolumePixmapView10setContentERK6VolumeRK7QString", .sym_new = "_ncf_setContent",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_setContent),
      .desc = "auto-mirror library cover as the grid populates (Volume by-symbol)", .optional = true },
    { .sym = "_ZN16VolumePixmapView10imageReadyERK6QImageRK6VolumeRK7QString", .sym_new = "_ncf_imageReady",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_imageReady),
      .desc = "mirror the rendered cover (RAM-only covers), owned books only", .optional = true },
    { .sym = "_ZN20SingleBookHomeWidget10setContentERK6Volume", .sym_new = "_ncf_SingleBookHomeWidget_setContent",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_sbhw_setContent),
      .desc = "home Now-Reading: push our mirror onto the widget's BookCoverView (vtable blind spot)", .optional = true },
    {0},
};

static struct nh_dlsym NickelCoverFixDlsym[] = {
    { .name = "_ZNK7Content5getIdEv", .out = nh_symoutptr(content_getId), .desc = "Content::getId (stable ContentID)", .optional = true },
    { .name = "_ZN6Device16getCurrentDeviceEv", .out = nh_symoutptr(device_current), .desc = "Device::getCurrentDevice", .optional = true },
    { .name = "_ZN5Image11getFileNameERK6DeviceRK6VolumeRK7QString", .out = nh_symoutptr(image_getFileName), .desc = "Image::getFileName", .optional = true },
    { .name = "_ZN13VolumeManager7forEachERK7QStringRKSt8functionIFvRK6VolumeEE", .out = nh_symoutptr(vm_forEach), .desc = "VolumeManager::forEach", .optional = true },
    { .name = "_ZN16VolumePixmapView16loadDefaultCoverEv", .out = nh_symoutptr(vpv_loadDefaultCover), .desc = "VolumePixmapView::loadDefaultCover", .optional = true },
    { .name = "_ZNK7Content14isPurchaseableEv", .out = nh_symoutptr(content_isPurchaseable), .desc = "Content::isPurchaseable (owned-book gate)", .optional = true },
    { .name = "_ZNK16VolumePixmapView9getVolumeEv", .out = nh_symoutptr(vpv_getVolume), .desc = "VolumePixmapView::getVolume (by-symbol Volume access)", .optional = true },
    { .name = "_ZN13BookCoverView8setImageERK6QImageRK7QString", .out = nh_symoutptr(bcv_setImage), .desc = "BookCoverView::setImage (call directly; vtable-only, unhookable)", .optional = true },
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
