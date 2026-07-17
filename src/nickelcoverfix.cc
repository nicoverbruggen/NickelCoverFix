// NickelCoverFix — offline cover reliability, zero DRM, zero decryption.
//
// The bug: Kobo keys a book's cover on its mutable store CoverImageId and, for purchased books, re-fetches
// it from the NETWORK on every RAM-cache loss (reboot/eviction) — it never reads the on-disk copy. So
// offline (or after a sync changes the CoverImageId) the cover blanks to the title/author PLACEHOLDER even
// though a perfectly good cover exists. This mod keeps a copy of the cover Kobo itself legitimately
// obtained — keyed by the STABLE ContentID — and serves it back instead of the placeholder.
//
// Three things, all reactive / user-driven (no boot-time enumeration):
//   CAPTURE  (cover hooks)             — when a book is shown and our cache is missing or stale, queue its
//                                        cover: from the on-disk .parsed if present, or from the rendered QImage
//                                        as a fallback. The copy runs after the render hook returns.
//   SERVE    (hook generateDefaultCover)— when nickel would paint the placeholder, return our mirror instead.
//   REPAIR   (More > Repair book covers)— a chunked, progress-dialog pass that mirrors every book's on-disk
//                                        cover at once (covers you never scroll to; bulk backfill).
//
// Keyed by Content::getId() (stable) in .adds/nickel-cover-fix/covers/<sha1(ContentID)>.png. .kobo-images is
// never written; remove the mod (or its .adds folder) to fully revert. If a cover is missing on disk, the
// user runs Kobo's own "Repair your Kobo account" (re-fetches covers to .kobo-images) then Repair book covers.
//
// BOOT SAFETY (must never hang a boot): purely reactive (one cover at a time, only when nickel renders it —
// at cold boot that's just the last-read book); no network/zip/crypto/DB/foreign-lock; fail-open (worst case
// = the placeholder); bounded decode; capture is queued after the hook returns. Disable over USB with
// ncf_enabled:0. Repair runs only on a menu tap (post-boot), chunked on the event loop, so a fault there is
// failsafe-recoverable.
//
// Maintainer map:
//   - ncf_* helpers below own all file/image validation and cache accounting.
//   - the CAPTURE hooks only identify work and enqueue it; they must stay cheap and non-blocking.
//   - the SERVE hooks may synchronously read our already-bounded mirror because they are asked for an image.
//   - NcfBridge owns deferred capture and the explicitly user-triggered Repair workflow.
//   - the final NickelHook tables are the ABI boundary: every symbol name and C++ signature there must be
//     revalidated against a target firmware before adding a new hook or changing a private-object call.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utime.h>

#include <QString>
#include <QByteArray>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSize>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QHash>
#include <QVector>
#include <QList>
#include <QPair>
#include <QObject>
#include <QTimer>
#include <QWidget>
#include <QApplication>
#include <QPixmap>
#include <QCryptographicHash>
#include <QWidget>
#include <QBoxLayout>

#include <NickelHook.h>

#include "config.h"
#include "util.h"
#include "ncfbridge.h"

static const char *const NCF_LIBNICKEL  = "/usr/local/Kobo/libnickel.so.1.0.0";
static const char *const NCF_COVERS_DIR = NCF_CONFIG_DIR "/covers";     // only directory this mod writes to
static const char *const NCF_BLACKLIST_FILE = NCF_CONFIG_DIR "/blacklist.txt";
static const char *const NCF_LIST_FILE = NCF_CONFIG_DIR "/list.txt";
// These limits protect both storage and the render path from malformed or unexpectedly large image files.
// Keep the compressed-byte and decoded-pixel limits separate: a small compressed image can expand enormously.
static const qint64      NCF_MAX_BYTES  = 8 * 1024 * 1024;             // per-cover compressed-file sanity cap
static const qint64      NCF_MAX_CACHE_BYTES = 128 * 1024 * 1024;     // total mirror budget
static const qint64      NCF_MIN_FREE_BYTES  = 32 * 1024 * 1024;      // never consume the last 32 MiB
static const qint64      NCF_MAX_PIXELS = 12 * 1024 * 1024;           // decoded-image memory guard
static const int         NCF_MAX_DIMENSION = 8192;                    // reject oversized dimensions early
// Two cover sizes: library grid/list (small) and the lock/sleep screen (full device resolution).
static const char *const NCF_TYPES_LIB[]  = { "N3_LIBRARY_FULL", "N3_LIBRARY_GRID", "N3_LIBRARY_LIST" };
static const char *const NCF_TYPES_LOCK[] = { "N3_FULL", "N3_LIBRARY_FULL" };
static const int         NCF_LOCK_MIN     = 700;                       // requested max(w,h) above this = lock screen

// opaque Kobo types — we only ever pass their addresses through to libnickel
struct KVolume;
typedef std::function<void(const KVolume &)> NcfVolumeFn;

// ---- resolved symbols (real functions we wrap) -------------------------------------------
//
// These signatures are not ordinary public C++ APIs: they are libnickel ABI contracts. A wrong reference,
// const qualifier, hidden QImage return convention, or object layout can corrupt Nickel. Keep each pointer
// close to its use and document why it is hooked directly versus looked up with dlsym below.
static QImage (*real_getCoverImage)(const void *vol, const QString &imageId, bool b) = nullptr;
static QImage (*real_generateDefaultCover)(const void *vol, const QSize &size)        = nullptr;
static void   (*real_moreview_ctor)(void *self, void *parent)                         = nullptr;
static void   (*real_loadCover)(void *self)                                           = nullptr;  // VolumePixmapView::loadCover
static void   (*real_setContent)(void *self, const void *vol, const QString &s)        = nullptr;  // VolumePixmapView::setContent
static void   (*real_imageReady)(void *self, const QImage &img, const void *vol, const QString &s) = nullptr; // VolumePixmapView::imageReady
static void   (*real_sbhw_setContent)(void *self, const void *vol)                     = nullptr;  // SingleBookHomeWidget::setContent(Volume const&)
static void   (*real_homepageview_ctor)(void *self, void *parent)                      = nullptr;  // HomePageView ctor (arms the first-run auto-cache)
// Callables (dlsym). Optional symbols deliberately degrade one feature at a time instead of making the whole
// plugin fail to load. Functions used by a safety-critical path are checked again immediately before use.
static void   (*vpv_loadDefaultCover)(void *self)                                     = nullptr;  // VolumePixmapView::loadDefaultCover
static bool   (*content_isPurchaseable)(const void *self)                             = nullptr;  // Content::isPurchaseable() const
static const void *(*vpv_getVolume)(const void *self)                                 = nullptr;  // VolumePixmapView::getVolume() (this+0xac, by symbol)
static void   (*bcv_setImage)(void *self, const QImage &img, const QString &label)     = nullptr;  // BookCoverView::setImage (vtable-only -> can't hook, dlsym + call directly)
static QString      (*content_getId)(const void *self)                                = nullptr;  // Content::getId() const (stable)
// Use Content::getTitle(), not getTitleRaw(): the latter returns Kobo's raw internal metadata representation,
// while getTitle() performs the firmware's conversion to a normal QString for the Repair manifest.
static QString      (*content_getTitle)(const void *self)                             = nullptr;  // Content::getTitle() const (optional list label)
static const void  *(*device_current)()                                               = nullptr;  // Device::getCurrentDevice()
static QString      (*image_getFileName)(const void *dev, const void *vol, const QString &type) = nullptr; // Image::getFileName (static)
static void         (*vm_forEach)(const QString &filter, const NcfVolumeFn &fn)        = nullptr;  // VolumeManager::forEach
static void         (*iconbtn_ctor)(void *self, void *parent)                          = nullptr;
static void         (*iconbtn_setText)(void *self, const QString &s)                   = nullptr;
static void         (*iconbtn_setPixmap)(void *self, const QPixmap &p)                 = nullptr;
static void         (*qboxlayout_addWidget)(void *layout, void *w, int stretch, int align) = nullptr;
static void         (*qboxlayout_insertWidget)(void *layout, int index, void *w, int stretch, int align) = nullptr;
// N3ProgressDialog is Kobo's native post-boot progress UI. Its public methods are resolved by symbol, while
// the 0x78 allocation is the size used by Kobo's own 4.45.23697 caller. This is still a private C++ ABI
// assumption; missing symbols are safe, but a changed class size requires firmware validation.
static void         (*ncf_progress_ctor)(void *self)                                   = nullptr;
static void         (*ncf_progress_setTitle)(void *self, const QString &title)         = nullptr;
static void         (*ncf_progress_setText)(void *self, const QString &text)           = nullptr;
static void         (*ncf_progress_setProgress)(void *self, int value)                 = nullptr;
static void         (*ncf_progress_setRange)(void *self, int minimum, int maximum)     = nullptr;
static void         (*ncf_progress_setVisible)(void *self, bool visible)               = nullptr;
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
// ncf_firstrun (default ON): on a fresh install with no mirror cached yet, auto-run the cover backfill once,
//   ~3.5s after the user first lands on the home screen. Post-boot and one-shot, so it never slows startup.
static bool ncf_firstrun()    { return ncf_global_config_bool("ncf_firstrun", true);  }

// Avoid turning a frequently-called cover hook into an unbounded syslog producer. Persistent file logging is
// separately controlled by ncf_log in config.c; this budget only limits the most verbose per-cover messages.
static int ncf_logbudget = 300;
#define NCF_TLOG(...) do { if (ncf_logbudget > 0) { ncf_logbudget--; NCF_LOG(__VA_ARGS__); } } while (0)

// Once-per-session capture guard. The mutex protects only mod-owned Qt containers; never hold it while calling
// into Nickel or doing file I/O, because either could re-enter a hook or wait on a Nickel lock.
static QMutex        ncf_seen_mutex;
static QSet<QString> ncf_seen;

// Cache accounting is deliberately conservative. A failed or externally deleted cache file may leave the
// counter high (which only suppresses future writes), while statvfs() independently protects free storage.
static QMutex        ncf_cache_mutex;
static qint64        ncf_cache_bytes = -1;

// Per-cover-widget override decision: does this widget's book have a cover we can produce? (set in
// setContent, read in loadCover). Keyed by widget pointer; widgets are recycled so the map stays small.
// When getVolume() is available, the current Volume is safer than this fallback map and the map is not used.
static QMutex             ncf_ov_mutex;
static QHash<void *, bool> ncf_override;

// ---- helpers -----------------------------------------------------------------------------
static bool ncf_image_size_allowed(const QSize &size) {
    // Check dimensions before decoding. QImageReader::read() can allocate from the advertised dimensions,
    // so a compressed-byte limit alone is not a sufficient memory bound.
    if (!size.isValid() || size.isEmpty() || size.width() > NCF_MAX_DIMENSION ||
        size.height() > NCF_MAX_DIMENSION)
        return false;
    return static_cast<qint64>(size.width()) * static_cast<qint64>(size.height()) <= NCF_MAX_PIXELS;
}

static bool ncf_read_image_size(const QString &path, QSize *size_out) {
    // This is the cheap validation pass used by the raw-copy path. It confirms the file is an image and lets
    // us reject hostile dimensions without materializing the image in memory.
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    if (!ncf_image_size_allowed(size))
        return false;
    if (size_out)
        *size_out = size;
    return true;
}

static QImage ncf_load_image_file(const QString &path) {
    // All image loads funnel through this function so the same byte, dimension, and pixel limits apply to
    // mirrors, Kobo cache files, and rendered-image fallbacks.
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile() || fi.size() <= 0 || fi.size() > NCF_MAX_BYTES)
        return QImage();

    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (!ncf_image_size_allowed(reader.size()))
        return QImage();

    QImage img = reader.read();
    return ncf_image_size_allowed(img.size()) ? img : QImage();
}

static bool ncf_is_cache_artifact(const QFileInfo &fi) {
    const QString name = fi.fileName();
    return name.endsWith(QLatin1String(".png")) || name.endsWith(QLatin1String("-lock.jpg")) ||
           name.endsWith(QLatin1String(".tmp"));
}

static qint64 ncf_scan_cache_bytes() {
    // Re-scan lazily instead of trusting a counter across restarts or USB sessions. Stale .tmp files are
    // cleanup debris from an interrupted write and are safe to remove because they are mod-owned.
    qint64 total = 0;
    const QFileInfoList files = QDir(QString::fromUtf8(NCF_COVERS_DIR)).entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &fi : files) {
        if (!ncf_is_cache_artifact(fi))
            continue;
        if (fi.fileName().endsWith(QLatin1String(".tmp"))) {
            QFile::remove(fi.absoluteFilePath());
            continue;
        }
        if (fi.size() > 0 && total <= NCF_MAX_CACHE_BYTES - fi.size())
            total += fi.size();
        else
            return NCF_MAX_CACHE_BYTES;
    }
    return total;
}

static bool ncf_has_free_space(qint64 incoming) {
    // f_bavail describes space available to the current user, which is the relevant measure on /mnt/onboard.
    struct statvfs st = {};
    if (statvfs(NCF_CONFIG_DIR, &st) != 0 || incoming < 0)
        return false;
    const quint64 available = static_cast<quint64>(st.f_bavail) * static_cast<quint64>(st.f_frsize);
    const quint64 needed = static_cast<quint64>(incoming) + static_cast<quint64>(NCF_MIN_FREE_BYTES);
    return available >= needed;
}

static bool ncf_cache_can_write(const QString &dst, qint64 upper_bound) {
    // Reserve the existing destination size when replacing a mirror; otherwise a refresh would incorrectly
    // count the old file and reject a write that remains within the total budget.
    if (upper_bound <= 0 || upper_bound > NCF_MAX_BYTES || !ncf_has_free_space(upper_bound))
        return false;

    QMutexLocker lk(&ncf_cache_mutex);
    if (ncf_cache_bytes < 0)
        ncf_cache_bytes = ncf_scan_cache_bytes();

    const QFileInfo dst_info(dst);
    const qint64 old_size = dst_info.exists() && dst_info.isFile() ? dst_info.size() : 0;
    const qint64 base = ncf_cache_bytes >= old_size ? ncf_cache_bytes - old_size : ncf_cache_bytes;
    return base <= NCF_MAX_CACHE_BYTES - upper_bound;
}

static void ncf_cache_commit(const QString &dst, qint64 old_size, qint64 new_size) {
    // This is accounting only; the filesystem rename has already succeeded before this function is called.
    QMutexLocker lk(&ncf_cache_mutex);
    if (ncf_cache_bytes < 0)
        ncf_cache_bytes = ncf_scan_cache_bytes();
    if (old_size < 0)
        old_size = 0;
    if (new_size < 0)
        new_size = 0;
    const qint64 base = ncf_cache_bytes >= old_size ? ncf_cache_bytes - old_size : ncf_cache_bytes;
    ncf_cache_bytes = base + new_size;
    (void)dst;
}

static void ncf_set_source_time(const QString &dst, const QFileInfo &src_info) {
    // Mirrored files inherit the source mtime so the next render can cheaply tell whether Kobo's cache changed.
    if (!src_info.lastModified().isValid())
        return;
    const time_t seconds = static_cast<time_t>(src_info.lastModified().toTime_t());
    struct utimbuf times = { seconds, seconds };
    const QByteArray path = dst.toLocal8Bit();
    (void)utime(path.constData(), &times);
}

static bool ncf_commit_temp(const QString &tmp, const QString &dst, qint64 old_size,
                            const QFileInfo *source_info = nullptr) {
    // Validate the completed temp file before it can become visible. Qt 5.2's QFile::rename() cannot replace
    // an existing file, so the old destination is removed first; this is a validated replacement, not a fully
    // atomic rename-overwrite. Losing the stale mirror is preferable to exposing a partial new file.
    const QFileInfo tmp_info(tmp);
    if (!tmp_info.exists() || !tmp_info.isFile() || tmp_info.size() <= 0 || tmp_info.size() > NCF_MAX_BYTES) {
        QFile::remove(tmp);
        return false;
    }
    if (QFile::exists(dst) && !QFile::remove(dst)) {
        QFile::remove(tmp);
        return false;
    }
    if (!QFile::rename(tmp, dst)) {
        QFile::remove(tmp);
        return false;
    }
    const qint64 new_size = QFileInfo(dst).size();
    ncf_cache_commit(dst, old_size, new_size);
    if (source_info)
        ncf_set_source_time(dst, *source_info);
    return true;
}

static QString ncf_mirror_key(const QString &cid) {
    // ContentID is user/database data, so hash it before using it in a path. This prevents path separators or
    // unusual IDs from escaping the single mod-owned cache directory.
    const QByteArray h = QCryptographicHash::hash(cid.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromUtf8(h);
}
static QString ncf_mirror_base(const QString &cid) {
    return QString::fromUtf8(NCF_COVERS_DIR) + QLatin1Char('/') + ncf_mirror_key(cid);
}
static QString ncf_mirror_path(const QString &cid)      { return ncf_mirror_base(cid) + QStringLiteral(".png"); }       // library
static QString ncf_mirror_path_lock(const QString &cid) { return ncf_mirror_base(cid) + QStringLiteral("-lock.jpg"); } // lock screen

static QSet<QString> ncf_read_repair_blacklist() {
    // This list is intentionally read only when Repair starts. It is a user-controlled exception for custom
    // covers, not a global serving rule, so ordinary capture and fallback behavior remain unchanged.
    QSet<QString> ids;
    QFile file(QString::fromUtf8(NCF_BLACKLIST_FILE));
    if (!file.exists())
        return ids;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        NCF_LOG("repair: could not read blacklist.txt");
        return ids;
    }

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        const int comment = line.indexOf(QLatin1Char('#'));
        if (comment >= 0)
            line = line.left(comment).trimmed();
        if (!line.isEmpty())
            ids.insert(line);
    }
    NCF_LOG("repair: loaded %d blacklist ID(s)", ids.size());
    return ids;
}

static bool ncf_write_repair_list(const QStringList &entries) {
    // Write a complete replacement after Repair finishes. A temporary file prevents an interrupted run from
    // leaving a partially written manifest that could mislead someone preparing custom covers.
    const QString path = QString::fromUtf8(NCF_LIST_FILE);
    const QString tmp = path + QStringLiteral(".tmp");
    QFile::remove(tmp);
    QFile file(tmp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        NCF_LOG("repair: could not write list.txt");
        return false;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    for (const QString &entry : entries)
        stream << entry << QLatin1Char('\n');
    stream.flush();
    if (!file.flush()) {
        file.close();
        QFile::remove(tmp);
        NCF_LOG("repair: could not flush list.txt");
        return false;
    }
    file.close();

    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        QFile::remove(tmp);
        NCF_LOG("repair: could not replace list.txt");
        return false;
    }
    return true;
}

static QImage ncf_load_mirror_file(const QString &path) {
    return ncf_load_image_file(path);
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

static QImage ncf_scale_for_request(const QImage &img, const QSize &requested) {
    // The requested size comes from Nickel. Clamp it too: callers must not be able to turn a valid mirror into
    // an oversized scaling allocation.
    if (img.isNull() || !ncf_image_size_allowed(img.size()) || !requested.isValid() || requested.isEmpty())
        return img;

    QSize target = requested.boundedTo(QSize(NCF_MAX_DIMENSION, NCF_MAX_DIMENSION));
    const qint64 pixels = static_cast<qint64>(target.width()) * static_cast<qint64>(target.height());
    if (pixels > NCF_MAX_PIXELS) {
        const qreal factor = static_cast<qreal>(std::sqrt(static_cast<double>(NCF_MAX_PIXELS) /
                                                          static_cast<double>(pixels)));
        target = QSize(qMax(1, static_cast<int>(target.width() * factor)),
                       qMax(1, static_cast<int>(target.height() * factor)));
    }
    return img.size() == target ? img : img.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Debug marker: a big black/white/black bullseye in the center — visible on light AND dark covers.
static void ncf_draw_dot(QImage &img) {
    // Debug-only mutation is intentionally isolated here. Normal operation preserves Kobo's original bytes,
    // which avoids both quality loss and an unnecessary decode/encode on the render path.
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
    // Prefer copying Kobo's already-encoded .parsed file. The reader pass above validates its dimensions and
    // format without paying the cost of decoding it; only ncf_debug_dot requests a real decode.
    QFileInfo si(src);
    if (!si.exists() || si.size() <= 0 || si.size() > NCF_MAX_BYTES)
        return false;
    if (!ncf_read_image_size(src, nullptr))
        return false;
    if (!ncf_cache_can_write(dst, si.size()))
        return false;
    const QString tmp = dst + QStringLiteral(".tmp");
    QFile::remove(tmp);
    const QFileInfo dst_info(dst);
    const qint64 old_size = dst_info.exists() && dst_info.isFile() ? dst_info.size() : 0;
    bool ok;
    if (ncf_debug_dot()) {                                      // decode -> stamp bullseye -> re-encode
        QImage img = ncf_load_image_file(src);
        if (img.isNull())
            return false;
        ncf_draw_dot(img);
        const bool jpg = dst.endsWith(QLatin1String(".jpg"));
        ok = img.save(tmp, jpg ? "JPG" : "PNG", jpg ? 88 : -1);
    } else {                                                    // fast path: straight copy, no re-encode
        ok = QFile::copy(src, tmp);
    }
    if (!ok) {
        QFile::remove(tmp);
        return false;
    }
    return ncf_commit_temp(tmp, dst, old_size, &si);
}

static bool ncf_write_mirror_image(const QImage &image, const QString &dst) {
    // Rendered-image fallback has no source file to copy, so it must be encoded. It is queued outside the
    // render hook and always uses the same bounded/temporary-file commit path as disk captures.
    if (image.isNull() || !ncf_image_size_allowed(image.size()) || !ncf_cache_can_write(dst, NCF_MAX_BYTES))
        return false;
    QImage out = image;
    if (ncf_debug_dot())
        ncf_draw_dot(out);
    const QString tmp = dst + QStringLiteral(".tmp");
    QFile::remove(tmp);
    const QFileInfo dst_info(dst);
    const qint64 old_size = dst_info.exists() && dst_info.isFile() ? dst_info.size() : 0;
    if (!out.save(tmp, "JPG", 85)) {
        QFile::remove(tmp);
        return false;
    }
    return ncf_commit_temp(tmp, dst, old_size);
}

// Find a book's best on-disk cover from a preference-ordered type list (biggest first).
static QString ncf_disk_cover_path(const void *dev, const void *vol, const char *const *types, int ntypes) {
    // Image::getFileName is Kobo's own mapping from Volume + cover role to .kobo-images. Do not glob the
    // directory or infer image IDs here: the library database remains the authority for which cover to use.
    if (!dev || !vol || !image_getFileName)
        return QString();
    for (int i = 0; i < ntypes; ++i) {
        const QString p = image_getFileName(dev, vol, QString::fromUtf8(types[i]));
        if (!p.isEmpty() && QFile::exists(p))
            return p;
    }
    return QString();
}

// Queue a book's on-disk cover for capture after the render hook returns. The hook only resolves the current
// source path; all copying, validation and optional decoding happen on the event loop.
static void ncf_autocapture(const void *vol, bool lock) {
    // This function is deliberately limited to metadata lookup and queueing. It may be called from a Nickel
    // render hook, including sleep/boot rendering, so it must not copy, decode, enumerate, or wait on I/O.
    if (!vol || !g_bridge || !content_getId || !device_current || !image_getFileName)
        return;
    const QString cid = content_getId(vol);
    if (cid.isEmpty())
        return;
    const QString dst = lock ? ncf_mirror_path_lock(cid) : ncf_mirror_path(cid);
    const QString src = lock ? ncf_disk_cover_path(device_current(), vol, NCF_TYPES_LOCK, 2)
                             : ncf_disk_cover_path(device_current(), vol, NCF_TYPES_LIB, 3);
    if (src.isEmpty())
        return;

    const QFileInfo source_info(src);
    const QFileInfo destination_info(dst);
    if (destination_info.exists() && source_info.lastModified().toTime_t() <= destination_info.lastModified().toTime_t())
        return;

    // Include source mtime and size in the deduplication key: a changed Kobo cache file must be eligible for
    // refresh even if the stable ContentID has not changed.
    const QString key = cid + (lock ? QLatin1String("#L:") : QLatin1String("#G:")) +
                        QString::number(source_info.lastModified().toMSecsSinceEpoch()) + QLatin1Char(':') +
                        QString::number(source_info.size());
    { QMutexLocker lk(&ncf_seen_mutex); if (ncf_seen.contains(key)) return; }
    g_bridge->queueCapture(src, dst, key);
}

// ---- CAPTURE: mirror the cover Kobo has on disk as books render ---------------------------
extern "C" __attribute__((visibility("default")))
QImage _ncf_getCoverImage(const void *vol, const QString &imageId, bool b) {
    // Call the original first. Capture is an observation of Kobo's result, and serving is only allowed after
    // the original has supplied a request-sized image from which we can choose a compatible fallback.
    if (!real_getCoverImage)
        return QImage();
    if (!ncf_active())
        return real_getCoverImage(vol, imageId, b);

    QImage img = real_getCoverImage(vol, imageId, b);

    if (ncf_capture()) ncf_autocapture(vol, /*lock*/ true);   // sleep/read context -> full lock-screen mirror
    // force_serve (default on, the aggressive mode): show our stable-ContentID mirror for every book we've
    // captured, overriding Kobo's live cover. This keeps a book's cover immune to Kobo's post-sync
    // CoverImageId churn, not just to outright fetch failures. Set ncf_force_serve:0 for fallback mode
    // (only substitute the mirror when Kobo would otherwise draw the title/author placeholder).
    if (ncf_serve() && ncf_force_serve() && content_getId && vol) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            QImage m = ncf_serve_mirror(cid, img.size());
            if (!m.isNull()) {
                return ncf_scale_for_request(m, img.size());
            }
        }
    }
    return img;
}

// ---- SERVE: return our mirror instead of the placeholder ---------------------------------
extern "C" __attribute__((visibility("default")))
QImage _ncf_generateDefaultCover(const void *vol, const QSize &size) {
    // This is the placeholder path: returning a non-null mirror here prevents Nickel from drawing its
    // title/author placeholder. If anything is uncertain, call the original implementation unchanged.
    if (ncf_active() && ncf_serve() && content_getId && vol) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            // queue a capture if we don't have a current mirror yet, then serve an existing mirror. Always ensure the
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
                QImage out = ncf_scale_for_request(m, size);
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
static bool ncf_widget_can_produce(void *self) {
    // force_serve is intentionally scoped to books for which we can prove a mirror exists or a Kobo source is
    // available. This prevents recommendations, previews, store items, and cloud-only books from being
    // replaced by an unrelated or stale mirror.
    if (vpv_getVolume && content_getId && self) {
        const void *vol = vpv_getVolume(self);
        if (!vol)
            return false;
        const QString cid = content_getId(vol);
        if (cid.isEmpty())
            return false;
        return QFile::exists(ncf_mirror_path(cid)) ||
               (device_current && image_getFileName &&
                !ncf_disk_cover_path(device_current(), vol, NCF_TYPES_LIB, 3).isEmpty());
    }
    QMutexLocker lk(&ncf_ov_mutex);
    return ncf_override.value(self, false);
}

extern "C" __attribute__((visibility("default")))
void _ncf_loadCover(void *self) {
    // loadCover is a dispatch point, not an image-producing function. Route only eligible library widgets to
    // Kobo's default-cover path; all other widgets retain Nickel's normal loading behavior.
    if (ncf_active() && ncf_serve() && ncf_force_serve() && vpv_loadDefaultCover && self) {
        if (ncf_widget_can_produce(self)) { vpv_loadDefaultCover(self); return; }
    }
    if (real_loadCover) real_loadCover(self);
}

// ---- CAPTURE + SCOPE (grid): mirror the book's cover, and decide whether we may override its widget -----
// setContent(Volume const&, QString const&) hands us the book by-symbol — no struct offset needed.
extern "C" __attribute__((visibility("default")))
void _ncf_setContent(void *self, const void *vol, const QString &shortcoverId) {
    // The original call must happen first so Nickel finishes assigning the Volume to the widget before any
    // later loadCover hook consults the decision.
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
    if (self && !vpv_getVolume) { QMutexLocker lk(&ncf_ov_mutex); ncf_override.insert(self, canProduce); }

    // BookCoverView force_serve. Grid view (the default cover layout on newer Colour devices), the home tiles,
    // and carousels all draw through a BookCoverView whose real cover arrives asynchronously via
    // CoverImageProvider, bypassing loadCover and getCoverImage — so without this, force_serve never reached
    // them and the grid showed Kobo's cover regardless of the setting. VolumePixmapView::setContent (this hook)
    // DOES fire on that BookCoverView (it inherits the method), so when self is itself a BookCoverView we push
    // our mirror straight in via the resolved setImage. Confirmed on-device: the pushed mirror survives the
    // later async setImage, so force_serve is consistent across list and grid. Scoped by mirror existence, which
    // only owned books have, so store/preview items (no mirror) fall through to Kobo untouched.
    if (ncf_active() && ncf_serve() && ncf_force_serve() && self && vol && content_getId && bcv_setImage
        && qstrcmp(reinterpret_cast<QObject *>(self)->metaObject()->className(), "BookCoverView") == 0) {
        const QString cid = content_getId(vol);
        if (!cid.isEmpty()) {
            const QImage m = ncf_serve_mirror(cid, QSize(4096, 4096));   // full mirror; BookCoverView scales it
            if (!m.isNull()) {
                bcv_setImage(self, m, cid);
                NCF_TLOG("bcv-serve cid=%s -> BookCoverView (%dx%d)", qPrintable(cid), m.width(), m.height());
            }
        }
    }
}

// ---- CAPTURE (rendered): mirror the cover Kobo actually drew, so RAM-only covers (no .parsed on disk)
// get saved too. Scoped to books you OWN (isPurchaseable == false) so store/recommendation covers are never
// cached. This is the "fallback" for covers we can't raw-copy from disk.
extern "C" __attribute__((visibility("default")))
void _ncf_imageReady(void *self, const QImage &img, const void *vol, const QString &s) {
    // This hook observes a rendered result. It is the only way to preserve a RAM-only cover, but it must be
    // fail-closed when the ownership classifier is missing: unknown content must never enter our cache.
    if (real_imageReady) real_imageReady(self, img, vol, s);
    if (!(ncf_active() && ncf_capture()) || img.isNull() || !vol || !content_getId)
        return;
    if (!content_isPurchaseable || content_isPurchaseable(vol))  // unknown classification is fail-closed
        return;
    const QString cid = content_getId(vol);
    if (cid.isEmpty())
        return;
    const QString dst = ncf_mirror_path(cid);
    const QString key = cid + QLatin1String("#R");
    if (QFile::exists(dst))                                      // raw copy already wins over rendered fallback
        return;
    { QMutexLocker lk(&ncf_seen_mutex); if (ncf_seen.contains(key)) return; }
    if (g_bridge)
        g_bridge->queueRenderedCapture(img, dst, key);
}

// ---- HOME first-run trigger (SingleBookHomeWidget::setContent) -----------------------------------------
// A reliable "the home screen is up" signal reached through the PLT (from FourBookHomeWidget::configure), unlike
// the HomePageView ctor which is a direct call the GOT patch may miss. Used only to arm the first-run auto-cache.
// The home tile's own cover no longer needs a bespoke push here: VolumePixmapView::setContent fires on the home's
// BookCoverView too, so the BookCoverView force_serve path in _ncf_setContent already serves it (and captures it).
extern "C" __attribute__((visibility("default")))
void _ncf_SingleBookHomeWidget_setContent(void *self, const void *vol) {
    if (real_sbhw_setContent) real_sbhw_setContent(self, vol);
    if (g_bridge && self) g_bridge->maybeArmFirstRun(reinterpret_cast<QWidget *>(self));
}

void NcfBridge::queueCapture(const QString &src, const QString &dst, const QString &key) {
    // Multiple cover hooks can observe the same book before the event loop gets a chance to run. Deduplicate
    // by destination while queued; the key is retained for the post-write session guard.
    if (src.isEmpty() || dst.isEmpty() || m_capture_pending.contains(dst))
        return;
    CaptureJob job;
    job.src = src;
    job.dst = dst;
    job.key = key;
    m_capture_pending.insert(dst);
    m_capture_work.append(job);
    if (!m_capture_timer) {
        QTimer *timer = new QTimer(this);
        timer->setInterval(0);
        QObject::connect(timer, SIGNAL(timeout()), this, SLOT(onCaptureTick()));
        m_capture_timer = timer;
    }
    static_cast<QTimer *>(m_capture_timer)->start();
}

void NcfBridge::queueRenderedCapture(const QImage &img, const QString &dst, const QString &key) {
    // QImage is implicitly shared, so queuing this value avoids copying pixels immediately while still keeping
    // the rendered fallback alive until the deferred write runs.
    if (img.isNull() || dst.isEmpty() || m_capture_pending.contains(dst))
        return;
    CaptureJob job;
    job.dst = dst;
    job.key = key;
    job.image = img;
    job.fromImage = true;
    m_capture_pending.insert(dst);
    m_capture_work.append(job);
    if (!m_capture_timer) {
        QTimer *timer = new QTimer(this);
        timer->setInterval(0);
        QObject::connect(timer, SIGNAL(timeout()), this, SLOT(onCaptureTick()));
        m_capture_timer = timer;
    }
    static_cast<QTimer *>(m_capture_timer)->start();
}

void NcfBridge::onCaptureTick() {
    // One job per event-loop turn keeps capture work out of Nickel's call stack and gives UI/sleep processing
    // opportunities between writes. A failed job is simply dropped; the next render may retry it.
    if (m_capture_work.isEmpty()) {
        if (m_capture_timer) {
            static_cast<QTimer *>(m_capture_timer)->stop();
            m_capture_timer->deleteLater();
            m_capture_timer = nullptr;
        }
        return;
    }

    const CaptureJob job = m_capture_work.takeLast();
    m_capture_pending.remove(job.dst);
    const bool ok = job.fromImage ? ncf_write_mirror_image(job.image, job.dst)
                                  : ncf_write_mirror_from(job.src, job.dst);
    if (ok) {
        QMutexLocker lk(&ncf_seen_mutex);
        ncf_seen.insert(job.key);
        NCF_TLOG("capture key=%s", qPrintable(job.key));
    }
}

// ---- More > "Repair book covers" row -----------------------------------------------------
// Kobo's IconLeftButton and N3ProgressDialog are private classes with no public factory, so we must provide
// their storage ourselves. The exact class sizes were validated on firmware 4.45.23697, but a later firmware
// could keep the same symbols while adding members. Over-allocate a generous, zero-initialized block so a
// constructor written for a larger layout still writes inside our buffer instead of corrupting the heap. The
// object is later freed by Kobo/Qt through the global operator delete(void*), which uses the allocator's own
// chunk size rather than our requested size, so handing off the larger allocation is safe.
static const size_t NCF_PRIVATE_OBJ_ALLOC = 4096;
static void *ncf_alloc_private_object(size_t validated_size) {
    if (validated_size == 0 || validated_size > NCF_PRIVATE_OBJ_ALLOC)
        return nullptr;                    // margin cannot cover this size; refuse rather than under-allocate
    void *p = ::operator new(NCF_PRIVATE_OBJ_ALLOC, std::nothrow);
    if (p)
        memset(p, 0, NCF_PRIVATE_OBJ_ALLOC);
    return p;
}

static bool ncf_repair_ui_available() {
    // The manual Repair path constructs two private Kobo widgets. Require every symbol that the row and the
    // progress dialog genuinely need to function; a dialog-less action is less safe than omitting the feature.
    // iconbtn_setPixmap is deliberately NOT required: the row icon is cosmetic, its call site is guarded, and
    // the symbol is absent on some firmwares (e.g. 4.29.18730) where the rest of the Repair UI is fine.
    return iconbtn_ctor && iconbtn_setText && qboxlayout_addWidget &&
           ncf_progress_ctor && ncf_progress_setTitle && ncf_progress_setText &&
           ncf_progress_setProgress && ncf_progress_setRange && ncf_progress_setVisible;
}

// ---- first-run auto-cache gates ----------------------------------------------------------
static const char *const NCF_FIRSTRUN_MARKER = NCF_CONFIG_DIR "/first-run-done";
static bool ncf_firstrun_done()      { return access(NCF_FIRSTRUN_MARKER, F_OK) == 0; }
static void ncf_mark_firstrun_done() { FILE *f = fopen(NCF_FIRSTRUN_MARKER, "w"); if (f) fclose(f); }
// True when the mirror cache holds no cover yet — i.e. a fresh install that hasn't cached anything.
static bool ncf_covers_empty() {
    return QDir(QString::fromUtf8(NCF_COVERS_DIR)).entryInfoList(QDir::Files | QDir::NoDotAndDotDot).isEmpty();
}
// Cheap bounded probe of Kobo's own cover cache. First-run must not run (and mark itself done) on a device whose
// cache is still empty, e.g. a brand-new Kobo that has not synced yet: there would be nothing to copy and we
// would then never auto-run again. When covers do exist, the first shard resolves this immediately, so it stays
// cheap even on a large cache. The scan stops as soon as it has seen a couple of parsed covers.
static bool ncf_kobo_cache_has_covers() {
    QDirIterator it(QStringLiteral("/mnt/onboard/.kobo-images"),
                    QStringList() << QStringLiteral("*.parsed"),
                    QDir::Files, QDirIterator::Subdirectories);
    int n = 0;
    while (it.hasNext() && n < 2) { it.next(); ++n; }
    return n >= 2;
}

// True if o's class is base, or derives from it (walks the Qt meta-object super-class chain). Cover refresh uses
// this so it catches every VolumePixmapView subclass (BookCoverView, ThreeBookVolumeView, VolumePackCoverView),
// not just the exact class name.
static bool ncf_isa(const QObject *o, const char *base) {
    for (const QMetaObject *mo = o ? o->metaObject() : nullptr; mo; mo = mo->superClass())
        if (qstrcmp(mo->className(), base) == 0)
            return true;
    return false;
}

// After a backfill finishes, re-fetch covers for the currently-visible cover views so freshly-mirrored covers
// replace any placeholders that were drawn before the mirror existed, without the user having to navigate away
// and back (e.g. right after the first-run backfill closes its dialog). Two widget families draw covers:
//   - VolumePixmapView (list view, lock/sleep): loadCover() re-runs the fetch, which now hits our serve hooks.
//   - BookCoverView (grid view, home tiles, carousels): its cover comes from the async provider, so loadCover
//     won't re-serve our mirror; push it straight in via the resolved setImage, exactly like _ncf_setContent.
// Offset-free (class name from the Qt meta-object) and cheap: a page holds only a handful of cover views.
static void ncf_refresh_visible_covers() {
    if (!ncf_serve())
        return;
    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget *w : widgets) {
        if (!w || !w->isVisible())
            continue;
        // Check BookCoverView first: it derives from VolumePixmapView but its cover comes from the async
        // provider, so loadCover() would not re-serve our mirror.
        if (ncf_isa(w, "BookCoverView")) {
            if (!bcv_setImage || !vpv_getVolume || !content_getId)
                continue;
            const void *vol = vpv_getVolume(w);            // BookCoverView inherits VolumePixmapView::getVolume
            if (!vol)
                continue;
            const QString cid = content_getId(vol);
            if (cid.isEmpty())
                continue;
            const QImage m = ncf_serve_mirror(cid, QSize(4096, 4096));
            if (!m.isNull())
                bcv_setImage(w, m, cid);
        } else if (ncf_isa(w, "VolumePixmapView")) {
            // list view, My Books (ThreeBookVolumeView), lock screen, etc.: re-run the fetch; a missing cover
            // now routes through our generateDefaultCover serve with the mirror in place.
            if (real_loadCover)
                real_loadCover(w);
        }
    }
}

// Find the actual More-page button layout through Qt's object tree. This avoids relying on MoreView's private
// Ui_MoreView pointer/container/layout offsets, which were the reason this optional feature was firmware-gated.
static QBoxLayout *ncf_moreview_layout(QWidget *more) {
    // MoreView's generated Ui_* member is private and firmware-specific. Qt's object tree is a safer seam,
    // but it can still be ambiguous when Kobo changes the page, so this helper refuses to guess.
    if (!more)
        return nullptr;

    QBoxLayout *best = nullptr;
    int best_score = -1;
    bool ambiguous = false;
    const QList<QBoxLayout *> layouts = more->findChildren<QBoxLayout *>();
    for (QBoxLayout *layout : layouts) {
        if (!layout || (layout->direction() != QBoxLayout::TopToBottom &&
                        layout->direction() != QBoxLayout::BottomToTop) || !layout->parent())
            continue;
        int icon_buttons = 0;
        int widgets = 0;
        for (int i = 0; i < layout->count(); ++i) {
            QWidget *widget = layout->itemAt(i) ? layout->itemAt(i)->widget() : nullptr;
            if (!widget)
                continue;
            ++widgets;
            if (qstrcmp(widget->metaObject()->className(), "IconLeftButton") == 0)
                ++icon_buttons;
        }
        // More's menu is the vertical layout with the existing IconLeftButton rows. Requiring at least one
        // known row and a unique best candidate makes an unexpected UI tree fail closed.
        if (icon_buttons == 0)
            continue;
        const int score = icon_buttons * 100 + widgets;
        if (score > best_score) {
            best = layout;
            best_score = score;
            ambiguous = false;
        } else if (score == best_score) {
            ambiguous = true;
        }
    }
    return ambiguous ? nullptr : best;
}

extern "C" __attribute__((visibility("default")))
void _ncf_MoreView_ctor(void *self, void *parent) {
    // The real constructor runs first so the Qt children/layout exist before we inspect them. Every failure
    // below only removes the optional Repair row; cover capture/serve hooks remain independent.
    if (real_moreview_ctor) real_moreview_ctor(self, parent);
    if (!(ncf_active() && ncf_menu()) || !self || !g_bridge || !ncf_repair_ui_available()) return;
    QBoxLayout *inner = ncf_moreview_layout(static_cast<QWidget *>(self));
    if (!inner) {
        NCF_LOG("MoreView: no unambiguous IconLeftButton layout; menu disabled for this UI");
        return;
    }
    QWidget *container = qobject_cast<QWidget *>(inner->parent());
    if (!container) return;
    // IconLeftButton is a private Kobo class with no usable public factory. Over-allocate a zeroed block (see
    // ncf_alloc_private_object) so a larger layout on a future firmware can't overflow; nothrow keeps an
    // allocation failure from taking down the More page.
    void *row = ncf_alloc_private_object(0x58);
    if (!row) return;
    iconbtn_ctor(row, container);
    if (iconbtn_setText)   iconbtn_setText(row, QStringLiteral("Repair Book Covers"));
    if (iconbtn_setPixmap) { QPixmap icon(QStringLiteral(":/images/menu/info.png")); iconbtn_setPixmap(row, icon); }
    // Only expose the row if its tap signal actually connects. A future IconLeftButton could rename or drop
    // tapped(); a visible but dead row is worse than omitting the feature, so tear it down and fail closed.
    if (!QObject::connect((QObject *)row, "2tapped()", g_bridge, "1onRepairTapped()")) {
        NCF_LOG("MoreView: IconLeftButton::tapped() did not connect; omitting Repair row");
        static_cast<QObject *>(row)->deleteLater();
        return;
    }
    // Place the row just above the Settings row rather than at the very bottom of the list. Ui_MoreView::setupUi
    // sets the Settings button's objectName to "settingsButton" (the Qt Designer widget name, set unconditionally
    // and independent of locale/translation, verified identical on 4.38 and 4.45). Hidden siblings stay in the
    // layout, so this finds the true index regardless of which rows a given device hides. If it can't be found or
    // insertWidget isn't resolved, fall back to appending; position is cosmetic.
    int settings_index = -1;
    for (int i = 0; i < inner->count(); ++i) {
        QWidget *w = inner->itemAt(i) ? inner->itemAt(i)->widget() : nullptr;
        if (w && w->objectName() == QLatin1String("settingsButton")) { settings_index = i; break; }
    }
    if (qboxlayout_insertWidget && settings_index >= 0)
        qboxlayout_insertWidget(inner, settings_index, row, 0, 0);
    else
        qboxlayout_addWidget(inner, row, 0, 0);
    static_cast<QWidget *>(row)->show();
    NCF_LOG("MoreView: added 'Repair Book Covers' row %s", settings_index >= 0 ? "above Settings" : "at end");
}

// Home screen shown: arm the one-shot first-run auto-cache. The real ctor runs first; the bridge decides
// whether to arm. This never does work here — it only schedules a post-boot timer — so the boot path is
// untouched. On firmware without HomePageView (pre-4.23) the optional hook simply never fires.
extern "C" __attribute__((visibility("default")))
void _ncf_HomePageView_ctor(void *self, void *parent) {
    if (real_homepageview_ctor) real_homepageview_ctor(self, parent);
    NCF_LOG("home-ctor: HomePageView constructed");
    if (g_bridge) g_bridge->maybeArmFirstRun(static_cast<QWidget *>(self));
}

// ---- NcfBridge slots (defined here so they can reach the resolved symbols) ----------------
// First-run auto-cache: on a fresh install (no mirror yet), cache the library's covers once, ~3.5s after the
// user first reaches the home screen. Post-boot and one-shot, so it never slows startup and never nags.
void NcfBridge::maybeArmFirstRun(QWidget *home) {
    // Arm at most once per session, and only for a genuine fresh install. The cheap pre-checks here avoid
    // scheduling a timer we would only no-op; the timer slot re-checks everything before doing any work.
    if (m_firstrun_armed) return;                   // called per-cover; decide exactly once, then stay quiet
    m_firstrun_armed = true;
    if (!ncf_active() || !ncf_firstrun()) { NCF_LOG("first-run: disabled (active=%d firstrun=%d)", ncf_active(), ncf_firstrun()); return; }
    if (ncf_firstrun_done())  { NCF_LOG("first-run: already done (marker present)"); return; }
    if (!ncf_covers_empty())  { NCF_LOG("first-run: covers already present; skipping"); return; }
    m_home = home;
    // 3.5s clears NickelHook's 3s crash-failsafe window (measured from plugin load, which precedes the home),
    // so the one-shot backfill can never be mistaken for a boot-time crash. Still effectively "just after home".
    QTimer::singleShot(3500, this, SLOT(onFirstRunTimer()));
    NCF_LOG("first-run: no cached covers; armed auto-cache (fires ~3.5s after home)");
}

void NcfBridge::onFirstRunTimer() {
    if (m_running) { NCF_LOG("first-run: skip, a repair is already running"); return; }
    if (m_home && !m_home->isVisible()) { NCF_LOG("first-run: skip, home not visible at timer"); return; }
    if (!ncf_active() || !ncf_firstrun() || !ncf_repair_ui_available()) {
        NCF_LOG("first-run: skip, inactive or repair UI unavailable (active=%d firstrun=%d ui=%d)",
                ncf_active(), ncf_firstrun(), ncf_repair_ui_available()); return; }
    if (!(content_getId && image_getFileName && device_current && vm_forEach)) {
        NCF_LOG("first-run: skip, core symbols missing"); return; }
    // Do NOT re-check ncf_covers_empty() here. maybeArmFirstRun already confirmed a fresh, empty mirror at arm
    // time, and our OWN auto-capture writes a few mirrors the instant the home renders (within the arm delay),
    // which would otherwise make the mirror non-empty and cancel first-run every time. The done-marker below is
    // the real "already ran once" guard.
    if (ncf_firstrun_done()) { NCF_LOG("first-run: skip, already done"); return; }
    // Nothing to copy if Kobo's own cache is empty. Skip WITHOUT marking done so a later boot (after the user
    // syncs and covers land in .kobo-images) will retry, instead of burning the one-shot on an empty library.
    if (!ncf_kobo_cache_has_covers()) { NCF_LOG("first-run: skip, Kobo cover cache empty; will retry after covers exist"); return; }
    ncf_mark_firstrun_done();                                  // one-shot: never auto-run again
    m_firstrun = true;
    NCF_LOG("first-run: auto-caching covers");
    onRepairTapped();                                          // reuse the full enumerate/progress/backfill flow
}

void NcfBridge::onRepairTapped() {
    // Repair is intentionally user-triggered and post-boot. It is the only path allowed to enumerate the
    // library, and it remains chunked so a large library cannot monopolize the e-ink UI event loop.
    if (m_running) return;
    if (!(content_getId && image_getFileName && device_current && vm_forEach)) {
        if (ncf_showOKDialog) ncf_showOKDialog(QStringLiteral("Repair Book Covers"),
            QStringLiteral("This isn't available on your firmware."));
        return;
    }
    if (!ncf_repair_ui_available()) {
        NCF_LOG("repair: manual Repair UI unavailable; skipping menu action");
        return;
    }
    QDir().mkpath(QString::fromUtf8(NCF_COVERS_DIR));

    // 1) enumerate the library (main thread, warm cache) -> work-list of
    //    [ librarySrc, libraryDst, lockSrc, lockDst ] per book
    m_work.clear(); m_repair_list.clear(); m_repair_blacklist = ncf_read_repair_blacklist(); m_idx = 0; m_copied = 0;
    const void *dev = device_current();
    NcfVolumeFn fn = [dev, this](const KVolume &v) {
        const void *vol = reinterpret_cast<const void *>(&v);
        const QString cid = content_getId(vol);
        if (cid.isEmpty()) return;
        // Accept both forms users may have available: the stable ContentID, or the hash used in the mirror
        // filename. The latter is useful when a custom cover was copied by looking at the covers directory.
        if (m_repair_blacklist.contains(cid) || m_repair_blacklist.contains(ncf_mirror_key(cid))) return;
        const QString title = content_getTitle ? content_getTitle(vol).simplified() : QStringLiteral("(unknown title)");
        const QString label = title.isEmpty() ? QStringLiteral("(unknown title)") : title;
        const QString hash = ncf_mirror_key(cid);
        const QString libSrc  = ncf_disk_cover_path(dev, vol, NCF_TYPES_LIB, 3);
        const QString lockSrc = ncf_disk_cover_path(dev, vol, NCF_TYPES_LOCK, 2);
        if (libSrc.isEmpty() && lockSrc.isEmpty()) return;     // nothing on disk (Repair-my-account can fill it)
        m_work.append(QStringList{ libSrc, ncf_mirror_path(cid), lockSrc, ncf_mirror_path_lock(cid), label, cid, hash });
    };
    vm_forEach(QString(), fn);

    // 2) use Kobo's native progress dialog. Its 0x78 storage size is taken from Kobo's own
    // ManageDictionariesWorkflow caller in firmware 4.45.23697; we over-allocate a zeroed block so a larger
    // layout on a future firmware can't overflow. All methods are symbol-resolved.
    m_dlg = ncf_alloc_private_object(0x78);
    if (!m_dlg) {
        NCF_LOG("repair: native progress dialog allocation failed");
        return;
    }
    ncf_progress_ctor(m_dlg);
    ncf_progress_setRange(m_dlg, 0, m_work.isEmpty() ? 1 : m_work.size());
    ncf_progress_setTitle(m_dlg, m_firstrun ? QStringLiteral("Preparing covers for first-time use...")
                                            : QStringLiteral("Copying covers again..."));
    ncf_progress_setText(m_dlg, m_firstrun
        ? QStringLiteral("Your existing book covers will be copied over so that they will always work offline. "
                         "This only needs to happen once, but it can take a bit. Please be patient.")
        : QStringLiteral("Caching your book covers so they still show when you're offline."));
    ncf_progress_setVisible(m_dlg, true);
    static_cast<QWidget *>(m_dlg)->show();

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
    // Four books per tick bounds the time spent in file validation/copying while keeping progress updates
    // responsive. Do not move this work to a worker thread: these libnickel Volume APIs are main-thread APIs.
    const int BATCH = 4;
    for (int n = 0; n < BATCH && m_idx < m_work.size(); ++n, ++m_idx) {
        const QStringList &e = m_work[m_idx];              // [ librarySrc, libraryDst, lockSrc, lockDst, title, cid, hash ]
        bool any = false;
        if (!e[0].isEmpty() && ncf_write_mirror_from(e[0], e[1])) any = true;   // library mirror
        if (!e[2].isEmpty() && ncf_write_mirror_from(e[2], e[3])) any = true;   // lock-screen mirror
        if (any) {
            m_copied++;                                     // count books with at least one successful mirror
            m_repair_list.append(QStringLiteral("%1 - %2 - %3").arg(e[4], e[5], e[6]));
        }
    }
    if (m_dlg && ncf_progress_setProgress) ncf_progress_setProgress(m_dlg, m_idx);

    if (m_idx >= m_work.size()) {
        if (m_timer) { static_cast<QTimer *>(m_timer)->stop(); m_timer->deleteLater(); m_timer = nullptr; }
        if (m_dlg) {
            static_cast<QWidget *>(m_dlg)->hide();
            static_cast<QObject *>(m_dlg)->deleteLater();
            m_dlg = nullptr;
        }
        const int total = m_work.size();
        const bool wasFirstRun = m_firstrun;
        m_firstrun = false;
        ncf_write_repair_list(m_repair_list);
        m_running = false;
        NCF_LOG("repair: done, mirrored %d of %d%s", m_copied, total, wasFirstRun ? " (first-run)" : "");
        // Repaint any covers currently on screen so the newly-mirrored ones show immediately.
        if (m_copied > 0) ncf_refresh_visible_covers();
        // The first-run backfill is unattended, so it just closes its progress bar — no dialog to dismiss.
        if (!wasFirstRun && ncf_showOKDialog)
            ncf_showOKDialog(QStringLiteral("Repair Book Covers"),
                QString(QStringLiteral("Cached %1 of %2 cover(s). They'll now show offline. If books are still "
                        "missing a cover, go online and run Settings > Device information > \"Repair your Kobo "
                        "account\", then try this again.")).arg(m_copied).arg(total));
    }
}

// ---- init / uninstall --------------------------------------------------------------------
static void ncf_log_firmware() {
    // Record the firmware version once at startup so a future-firmware breakage report shows which version ran
    // next to the resolved-symbol map below. The serial number (field 0 of the version file) is deliberately
    // dropped. Diagnostics only: any failure is silent and never affects startup.
    FILE *f = fopen("/mnt/onboard/.kobo/version", "r");
    if (!f) {
        NCF_LOG("startup(v1): firmware version unavailable");
        return;
    }
    char line[512];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got)
        return;
    line[strcspn(line, "\r\n")] = '\0';
    // Format: <serial>,<...>,<firmware>,<model>,... — log everything after the serial.
    const char *comma = strchr(line, ',');
    NCF_LOG("startup(v1): firmware %s", comma ? comma + 1 : line);
}

static int ncf_init() {
    // NickelHook calls init only after its symbol resolution and GOT patches succeed. Keep this function fast:
    // it runs on Nickel's startup path, before the failsafe is disarmed.
    ncf_global_config_get("");                                // force config parse (writes default on first run)
    mkdir(NCF_CONFIG_DIR, 0755);
    QDir().mkpath(QString::fromUtf8(NCF_COVERS_DIR));
    g_bridge = new NcfBridge(nullptr);
    ncf_log_firmware();
    NCF_LOG("startup(v1): enabled=%d capture=%d serve=%d menu=%d force=%d firstrun=%d",
            ncf_enabled(), ncf_capture(), ncf_serve(), ncf_menu(), ncf_force_serve(), ncf_firstrun());
    NCF_LOG("startup(v1): getCoverImage=%p generateDefaultCover=%p moreview=%p bridge=%p sbhw_setContent=%p bcv_setImage=%p homepageview_ctor=%p",
            (void*)real_getCoverImage, (void*)real_generateDefaultCover, (void*)real_moreview_ctor, (void*)g_bridge,
            (void*)real_sbhw_setContent, (void*)bcv_setImage, (void*)real_homepageview_ctor);
    NCF_LOG("startup(v1): getId=%p device=%p getFileName=%p forEach=%p showOK=%p",
            (void*)content_getId, (void*)device_current, (void*)image_getFileName, (void*)vm_forEach,
            (void*)ncf_showOKDialog);
    return 0;
}

static bool ncf_del(const char *p) { return access(p, F_OK) != 0 ? true : nh_delete_file(p); }
static bool ncf_uninstall() {
    // NickelHook has already removed this plugin from future loads when this callback runs. Delete only the
    // mod-owned mirror/configuration paths; in particular, never touch Kobo's .kobo-images cache.
    NCF_LOG("uninstall: removing NickelCoverFix files + our cover mirror (.kobo-images never touched)");
    bool ok = true;
    QDir(QString::fromUtf8(NCF_COVERS_DIR)).removeRecursively();
    ok = ncf_del(NCF_CONFIG_DIR "/enabled") && ok;   // remove any leftover arm flag from older builds
    ok = ncf_del(NCF_CONFIG_DIR "/config") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/default") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/doc") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/nickel-cover-fix.log") && ok;
    ok = ncf_del(NCF_CONFIG_DIR "/uninstall") && ok;
    if (access(NCF_CONFIG_DIR, F_OK) == 0) ok = nh_delete_dir(NCF_CONFIG_DIR) && ok;
    return ok;
}

// ---- NickelHook wiring -------------------------------------------------------------------
// Hook entries are optional so a missing symbol disables only the corresponding feature. The core plugin can
// therefore load safely across nearby firmware revisions, while each feature still checks its prerequisites
// before dereferencing a null or ABI-incompatible pointer.
static struct nh_info NickelCoverFixInfo = {
    .name            = "NickelCoverFix",
    .desc            = "v1: serve a cached cover instead of the placeholder + More>Repair book covers (no DRM).",
    .uninstall_flag  = NCF_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NCF_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 3,
};

static struct nh_hook NickelCoverFixHooks[] = {
    // These are PLT/GOT hooks: NickelHook redirects calls made through libnickel's relocation table, then
    // stores the original target in the matching real_* pointer for an exact call-through.
    //libnickel 4.30.18838 * _ZN16VolumePixmapView13getCoverImageERK6VolumeRK7QStringb
    { .sym = "_ZN16VolumePixmapView13getCoverImageERK6VolumeRK7QStringb", .sym_new = "_ncf_getCoverImage",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_getCoverImage),
      .desc = "capture the real cover into .adds", .optional = true },
    //libnickel 4.30.18838 * _ZN16VolumePixmapView20generateDefaultCoverERK6VolumeRK5QSize
    { .sym = "_ZN16VolumePixmapView20generateDefaultCoverERK6VolumeRK5QSize", .sym_new = "_ncf_generateDefaultCover",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_generateDefaultCover),
      .desc = "serve mirror instead of placeholder", .optional = true },
    //libnickel 4.30.18838 * _ZN8MoreViewC1EP7QWidget
    { .sym = "_ZN8MoreViewC1EP7QWidget", .sym_new = "_ncf_MoreView_ctor",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_moreview_ctor),
      .desc = "append Repair book covers row", .optional = true },
    //libnickel 4.30.18838 * _ZN16VolumePixmapView9loadCoverEv
    { .sym = "_ZN16VolumePixmapView9loadCoverEv", .sym_new = "_ncf_loadCover",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_loadCover),
      .desc = "debug force_serve: route grid covers through the default path", .optional = true },
    //libnickel 4.30.18838 * _ZN16VolumePixmapView10setContentERK6VolumeRK7QString
    { .sym = "_ZN16VolumePixmapView10setContentERK6VolumeRK7QString", .sym_new = "_ncf_setContent",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_setContent),
      .desc = "auto-mirror library cover as the grid populates (Volume by-symbol)", .optional = true },
    //libnickel 4.30.18838 * _ZN16VolumePixmapView10imageReadyERK6QImageRK6VolumeRK7QString
    { .sym = "_ZN16VolumePixmapView10imageReadyERK6QImageRK6VolumeRK7QString", .sym_new = "_ncf_imageReady",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_imageReady),
      .desc = "mirror the rendered cover (RAM-only covers), owned books only", .optional = true },
    //libnickel 4.30.18838 * _ZN20SingleBookHomeWidget10setContentERK6Volume
    { .sym = "_ZN20SingleBookHomeWidget10setContentERK6Volume", .sym_new = "_ncf_SingleBookHomeWidget_setContent",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_sbhw_setContent),
      .desc = "home Now-Reading: push our mirror onto the widget's BookCoverView (vtable blind spot)", .optional = true },
    //libnickel 4.30.18838 * _ZN12HomePageViewC1EP7QWidget
    { .sym = "_ZN12HomePageViewC1EP7QWidget", .sym_new = "_ncf_HomePageView_ctor",
      .lib = NCF_LIBNICKEL, .out = nh_symoutptr(real_homepageview_ctor),
      .desc = "first-run: auto-cache covers on the first home screen (4.23+)", .optional = true },
    {0},
};

static struct nh_dlsym NickelCoverFixDlsym[] = {
    // These functions are not safely hookable through a PLT entry in the relevant call path, or are helpers
    // needed by the deferred workflow. Resolve them by exported mangled name and treat them as optional.
    //libnickel 4.30.18838 * _ZNK7Content5getIdEv
    { .name = "_ZNK7Content5getIdEv", .out = nh_symoutptr(content_getId), .desc = "Content::getId (stable ContentID)", .optional = true },
    //libnickel 4.30.18838 * _ZNK7Content8getTitleEv
    { .name = "_ZNK7Content8getTitleEv", .out = nh_symoutptr(content_getTitle), .desc = "Content::getTitle (Repair list label)", .optional = true },
    //libnickel 4.30.18838 * _ZN6Device16getCurrentDeviceEv
    { .name = "_ZN6Device16getCurrentDeviceEv", .out = nh_symoutptr(device_current), .desc = "Device::getCurrentDevice", .optional = true },
    //libnickel 4.30.18838 * _ZN5Image11getFileNameERK6DeviceRK6VolumeRK7QString
    { .name = "_ZN5Image11getFileNameERK6DeviceRK6VolumeRK7QString", .out = nh_symoutptr(image_getFileName), .desc = "Image::getFileName", .optional = true },
    //libnickel 4.30.18838 * _ZN13VolumeManager7forEachERK7QStringRKSt8functionIFvRK6VolumeEE
    { .name = "_ZN13VolumeManager7forEachERK7QStringRKSt8functionIFvRK6VolumeEE", .out = nh_symoutptr(vm_forEach), .desc = "VolumeManager::forEach", .optional = true },
    //libnickel 4.30.18838 * _ZN16VolumePixmapView16loadDefaultCoverEv
    { .name = "_ZN16VolumePixmapView16loadDefaultCoverEv", .out = nh_symoutptr(vpv_loadDefaultCover), .desc = "VolumePixmapView::loadDefaultCover", .optional = true },
    //libnickel 4.30.18838 * _ZNK7Content14isPurchaseableEv
    { .name = "_ZNK7Content14isPurchaseableEv", .out = nh_symoutptr(content_isPurchaseable), .desc = "Content::isPurchaseable (owned-book gate)", .optional = true },
    //libnickel 4.30.18838 * _ZNK16VolumePixmapView9getVolumeEv
    { .name = "_ZNK16VolumePixmapView9getVolumeEv", .out = nh_symoutptr(vpv_getVolume), .desc = "VolumePixmapView::getVolume (by-symbol Volume access)", .optional = true },
    //libnickel 4.30.18838 * _ZN13BookCoverView8setImageERK6QImageRK7QString
    { .name = "_ZN13BookCoverView8setImageERK6QImageRK7QString", .out = nh_symoutptr(bcv_setImage), .desc = "BookCoverView::setImage (call directly; vtable-only, unhookable)", .optional = true },
    //libnickel 4.30.18838 * _ZN14IconLeftButtonC1EP7QWidget
    { .name = "_ZN14IconLeftButtonC1EP7QWidget", .out = nh_symoutptr(iconbtn_ctor), .desc = "IconLeftButton ctor", .optional = true },
    //libnickel 4.30.18838 * _ZN14IconLeftButton7setTextERK7QString
    { .name = "_ZN14IconLeftButton7setTextERK7QString", .out = nh_symoutptr(iconbtn_setText), .desc = "IconLeftButton::setText", .optional = true },
    //libnickel 4.30.18838 * _ZN14IconLeftButton9setPixmapERK7QPixmap
    { .name = "_ZN14IconLeftButton9setPixmapERK7QPixmap", .out = nh_symoutptr(iconbtn_setPixmap), .desc = "IconLeftButton::setPixmap", .optional = true },
    //libnickel 4.30.18838 * _ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE
    { .name = "_ZN10QBoxLayout9addWidgetEP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE", .out = nh_symoutptr(qboxlayout_addWidget), .desc = "QBoxLayout::addWidget", .optional = true },
    //libnickel 4.30.18838 * _ZN10QBoxLayout12insertWidgetEiP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE
    { .name = "_ZN10QBoxLayout12insertWidgetEiP7QWidgeti6QFlagsIN2Qt13AlignmentFlagEE", .out = nh_symoutptr(qboxlayout_insertWidget), .desc = "QBoxLayout::insertWidget (place Repair above Settings)", .optional = true },
    //libnickel 4.30.18838 * _ZN16N3ProgressDialogC1Ev
    { .name = "_ZN16N3ProgressDialogC1Ev", .out = nh_symoutptr(ncf_progress_ctor), .desc = "native progress dialog ctor", .optional = true },
    //libnickel 4.30.18838 * _ZN16N3ProgressDialog8setTitleERK7QString
    { .name = "_ZN16N3ProgressDialog8setTitleERK7QString", .out = nh_symoutptr(ncf_progress_setTitle), .desc = "native progress dialog title", .optional = true },
    //libnickel 4.30.18838 * _ZN16N3ProgressDialog7setTextERK7QString
    { .name = "_ZN16N3ProgressDialog7setTextERK7QString", .out = nh_symoutptr(ncf_progress_setText), .desc = "native progress dialog text", .optional = true },
    //libnickel 4.30.18838 * _ZN16N3ProgressDialog11setProgressEi
    { .name = "_ZN16N3ProgressDialog11setProgressEi", .out = nh_symoutptr(ncf_progress_setProgress), .desc = "native progress dialog value", .optional = true },
    //libnickel 4.30.18838 * _ZN16N3ProgressDialog19setProgressBarRangeEii
    { .name = "_ZN16N3ProgressDialog19setProgressBarRangeEii", .out = nh_symoutptr(ncf_progress_setRange), .desc = "native progress dialog range", .optional = true },
    //libnickel 4.30.18838 * _ZN16N3ProgressDialog21setProgressBarVisibleEb
    { .name = "_ZN16N3ProgressDialog21setProgressBarVisibleEb", .out = nh_symoutptr(ncf_progress_setVisible), .desc = "native progress bar visibility", .optional = true },
    //libnickel 4.30.18838 * _ZN25ConfirmationDialogFactory12showOKDialogERK7QStringS2_
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
