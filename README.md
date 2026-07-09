# NickelCoverFix

A tiny [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo e-readers that stops book covers
from blanking to the title/author **placeholder** when the cover is temporarily unreachable.

## The problem
Kobo keys a book's cover on its (mutable) store **CoverImageId**. When the file for the *current* imageId
isn't reachable, nickel paints the placeholder even though a perfectly good cover exists:
- **offline** — the cover was downloaded/shown before, but the RAM image cache missed and nickel prefers
  the network worker, which fails with no connection;
- **after a sync** — the CoverImageId changed, so the new id's file is absent while the old cover file is
  orphaned on disk.

## What it does
Two lightweight hooks on the (static) `VolumePixmapView` cover helpers:
- **Capture** `getCoverImage` — when nickel yields a real cover, mirror that image into
  `.adds/nickelcoverfix/covers/<sha1(ContentID)>.png` (once per book per session).
- **Serve** `generateDefaultCover` — when nickel would show the placeholder, load+scale the mirror and
  return it instead.

The mirror is keyed by the **stable ContentID**, so it survives both offline gaps and imageId changes.

**No DRM, no decryption, no book files** — it only ever keeps a copy of a cover nickel already obtained
legitimately. `.kobo-images` is never written; removing the mod (or its `.adds` folder) fully reverts.

## Boot safety
Covers render on the boot/sleep-screen path, so the mod is built to never hang a boot:
no enumeration (purely reactive, one cover at a time), no blocking work (no network/zip/crypto/DB),
fail-open on everything, a per-cover size cap, and work that's a cheap raw copy (or a guarded encode).
Disable over USB with `ncf_enabled:0`; Repair runs only on a menu tap (post-boot, chunked).

## Build
```
./build.sh          # podman + ghcr.io/pgaskin/nickeltc:1.0 -> KoboRoot.tgz
```

## Install
Copy `KoboRoot.tgz` to `KOBOeReader/.kobo/` and reboot. It's active on install — covers are mirrored as
books are shown, or all at once via **More > Repair Book Covers**. See `res/doc` for details.

## Uninstall
Delete `KOBOeReader/.adds/nickelcoverfix/uninstall` and reboot.
