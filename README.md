# NickelCoverFix

A [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo e-readers that stops book covers from
blanking to the title/author **placeholder** when Kobo can't fetch them.

## The problem
Kobo keys a book's cover on its (mutable) store **CoverImageId**. When the file for the *current* imageId
isn't reachable, nickel paints the placeholder even though a perfectly good cover exists:
- **offline** — the cover was downloaded/shown before, but the RAM image cache missed and nickel prefers
  the network worker, which fails with no connection;
- **after a sync** — the CoverImageId changed, so the new id's file is absent while the old cover file is
  orphaned on disk.

## What it does
It mirrors the cover **Kobo itself already had** into `.adds/nickel-cover-fix/covers/`, keyed by the book's
**stable ContentID** (`sha1(ContentID).png` for the library, `sha1(ContentID)-lock.jpg` full-res for the
lock screen), and shows that when Kobo would otherwise draw a placeholder.

- **Capture** — as each of *your* books renders, its on-disk cover is raw-copied from Kobo's cache
  (`.kobo-images`, via `Image::getFileName`) — no re-encode. Covers that were only ever in RAM are captured
  from the rendered image as a fallback. Store/recommendation covers are never touched.
- **Serve** — when a cover would be a placeholder, the mirror is loaded and scaled in its place. This covers
  the **library grid**, the **lock/sleep screen**, and the home screen's **"Now Reading"** shelf.
- **Repair Book Covers** (More menu) — mirror your whole library at once, with a progress bar.

Because the mirror is keyed by the stable ContentID, it survives both offline gaps and imageId changes.

**No DRM, no decryption, no book files** — it only ever keeps a copy of a cover nickel already obtained
legitimately. `.kobo-images` is never written; removing the mod (or its `.adds` folder) fully reverts.

## The home "Now Reading" shelf
The library grid renders covers through `VolumePixmapView` (reached via the PLT, which NickelHook's GOT
patch intercepts), but the home "Now Reading" widget (`SingleBookHomeWidget` → `BookCoverView`) dispatches
its cover calls through the **vtable**, which a GOT patch cannot catch — so it needs its own seam. The mod
hooks `SingleBookHomeWidget::setContent` (which *is* PLT-reached), finds the embedded `BookCoverView` via the
Qt object tree (no struct offsets), and pushes the mirror in through `BookCoverView::setImage` (dlsym'd and
called directly, since that function is vtable-only and can't be hooked).

## Boot safety
Covers render on the boot/sleep-screen path, so the mod is built to never hang a boot: no enumeration
(purely reactive, one cover at a time), no blocking work (no network/zip/crypto/DB), fail-open on everything,
a per-cover size cap, and capture that's a cheap raw copy. Repair runs only on a menu tap (post-boot,
chunked on the event loop).

## Cover mode (`ncf_force_serve`)
- `1` (default) — **scoped override**: your library books show our own copy (offline-consistent); store,
  recommendations, previews and new/cloud books fall through to Kobo untouched.
- `0` — **graceful fallback**: Kobo's real cover when available, our copy only to rescue a placeholder.

## Config (`.adds/nickel-cover-fix/config`, seeded from `default`)
- `ncf_enabled` — master kill-switch (`0` = works like stock).
- `ncf_capture` — mirror covers as books are shown (else only Repair populates).
- `ncf_serve` — show a mirrored cover instead of the placeholder.
- `ncf_force_serve` — cover mode (see above).
- `ncf_menu` — add the Repair Book Covers row to the More page.
- `ncf_log` — write `nickelcoverfix.log`.
- `ncf_debug_dot` — stamp served covers with a bullseye (debugging).

## Build
```
./build.sh          # podman + ghcr.io/pgaskin/nickeltc:1.0 -> KoboRoot.tgz
```

## Install
Copy `KoboRoot.tgz` to `KOBOeReader/.kobo/` and reboot. It's active on install — covers are mirrored as
books are shown, or all at once via **More > Repair Book Covers**. See `res/doc` for details.

## Uninstall
Delete `KOBOeReader/.adds/nickel-cover-fix/uninstall` and reboot (removes the mod and the mirror folder;
`.kobo-images` is untouched).
