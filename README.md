# NickelCoverFix

A [NickelHook](https://github.com/pgaskin/NickelHook) mod for Kobo e-readers that stops book covers from blanking to the title/author **placeholder** when Kobo cannot fetch them. It can also be used with custom cover images if you want to replace selected covers.

## The problem

Kobo keys a book's cover on its mutable store **CoverImageId**. When the file for the current imageId is not reachable, Nickel paints the placeholder even though a perfectly good cover exists:

- **Offline:** the cover was downloaded and shown before, but the RAM image cache missed and Nickel prefers the network worker, which fails with no connection.
- **After a sync:** the CoverImageId changed, so the new ID's file is absent while the old cover file is orphaned on disk.

## What it does

It mirrors the cover **Kobo itself already had** into `.adds/nickel-cover-fix/covers/`, keyed by the book's **stable ContentID** (`sha1(ContentID).png` for the library, `sha1(ContentID)-lock.jpg` for the lock screen), and shows that copy when Kobo would otherwise draw a placeholder.

- **Capture:** as each of *your* books renders, its on-disk cover is copied from Kobo's cache (`.kobo-images`, via `Image::getFileName`) without re-encoding. Covers that were only ever in RAM are captured from the rendered image as a fallback. Store and recommendation covers are never touched.
- **Serve:** when a cover would be a placeholder, the mirror is loaded and scaled in its place. This covers the **library grid**, the **lock/sleep screen**, and the home screen's **Now Reading** shelf.
- **First-run caching:** on a fresh install with nothing cached yet, the mod caches your library's covers once, automatically, about a second after you first reach the home screen. It runs post-boot (so it never slows startup) and only this one time, with a "Caching covers for first-time use" progress bar.
- **Repair Book Covers:** mirror your whole library at once from the More menu, with a progress bar — the same pass, available any time (for example after a later sync).

The first-run cache above handles most people automatically. If you'd rather run it yourself, or need it again after syncing new books, open **More > Repair Book Covers**. It prepares the available cached covers in one post-boot pass, so the fallback is ready much sooner. Without either, covers are prepared automatically as books are shown, which can take a while. Placeholders may appear while valid covers are being copied. That is expected; the mirror will be used on a later render once it is ready.

Because the mirror is keyed by the stable ContentID, it survives offline gaps and image ID changes.

**No DRM, no decryption, no book files:** the mod only keeps a copy of a cover Nickel already obtained legitimately. `.kobo-images` is never written. Removing the mod or its `.adds` folder fully reverts.

## Boot safety

Covers render on the boot and sleep-screen paths, so the mod avoids work that could hang startup. It does not enumerate the library, use the network, open archives, access the database, or hold foreign locks from a render hook. Image decoding is bounded, and capture is queued after the hook returns. Repair runs only after a menu tap, in small event-loop batches. The mirror has a 128 MiB total budget, preserves at least 32 MiB of free storage, and removes failed temporary files.

The Repair row is located through Kobo's live Qt object tree without using MoreView's private UI offsets. Repair uses Kobo's native `N3ProgressDialog` and `IconLeftButton`. It is optional: if either API is unavailable, the row is not added and the Repair workflow is skipped. Cover capture and serving continue independently.

## Custom covers

You can replace a mirrored cover with your own image. Put it in `.adds/nickel-cover-fix/covers/` using the exact filename generated for that book: `sha1(ContentID).png` for the library cover or `sha1(ContentID)-lock.jpg` for the lock screen. The image must be readable by Qt and stay within the same size limits as any other mirror. It is used while `ncf_serve:1` is enabled. With `ncf_force_serve:0`, it is used only when Kobo would otherwise show a placeholder.

### Recommended workflow

1. Install the mod.
2. Connect to Wi-Fi and sync, or run **Settings > Device information > Repair your Kobo account**. Wait for Kobo to download the covers. NickelCoverFix cannot download covers; it copies the covers Kobo has already downloaded.
3. Open **More > Repair Book Covers** to copy those downloaded covers into the mod's storage.
4. Connect the Kobo by USB and open `.adds/nickel-cover-fix/list.txt` to see which title, ContentID, and cover hash belong together.
5. Use the ContentID or hash to find the corresponding files in `.adds/nickel-cover-fix/covers/`, then replace them with the covers you want to use.

For good results, use a portrait image with the same aspect ratio as the cover it replaces. Around 600 pixels on the long edge is usually enough for a library cover. For the lock screen, use the device's native display resolution or the dimensions of the original lock-screen cover. Larger images are scaled down without cropping, while small images may look soft when enlarged.

The safety limits are 8 MiB per file, 8192 pixels on either side, and 12 megapixels total. These are maximums, not recommended target sizes. Images outside those limits or images that Qt cannot read are ignored.

Run **More > Repair Book Covers** before copying custom images. Repair rebuilds mirror files from Kobo's cache, so running it afterward can replace your custom covers.

To keep custom covers out of a later Repair run, create `.adds/nickel-cover-fix/blacklist.txt` and add one stable ContentID or 40-character SHA-1 cover filename stem per line. For example, `0963c26c758a65db4ab0691a30abee91a795e106` matches the `.png` and `-lock.jpg` files with that name. Blank lines and lines beginning with `#` are ignored. Repair skips matching books that are present in the library. The list does not affect automatic capture or serving, and old IDs that are no longer in the library are harmless.

After Repair finishes, it writes `.adds/nickel-cover-fix/list.txt` with one successfully mirrored book per line in this format: `Title - ContentID - cover-hash`. The hash is the filename stem used for the library `.png` cover and the lock-screen `-lock.jpg` cover. The file is UTF-8 text and can be searched with Ctrl-F or Cmd-F. If Kobo does not provide a title, the line uses `(unknown title)`.

## Cover mode (`ncf_force_serve`)

- `1` (default): **scoped override**. Your library books show our own copy for consistent offline behavior. Store, recommendation, preview, and new or cloud books fall through to Kobo untouched.
- `0`: **graceful fallback**. Kobo's real cover is used when available, and our copy is used only to replace a placeholder.

## Config (`.adds/nickel-cover-fix/config`, seeded from `default`)

| Key | Default | Description |
|---|---:|---|
| `ncf_enabled` | `1` | Master kill-switch. `0` behaves like stock. |
| `ncf_capture` | `1` | Mirror covers as books are shown. If disabled, only Repair populates the mirror. |
| `ncf_serve` | `1` | Show a mirrored cover instead of the placeholder. |
| `ncf_force_serve` | `1` | Select the cover mode described above. |
| `ncf_menu` | `1` | Add the Repair Book Covers row to the More page. |
| `ncf_log` | `1` | Write `nickel-cover-fix.log`. `0` disables persistent file logging. |
| `ncf_debug_dot` | `0` | Stamp served covers with a bullseye for debugging. |

## Build

```sh
./build.sh          # podman + ghcr.io/pgaskin/nickeltc:1.0 -> KoboRoot.tgz
```

GitHub Actions runs the same NickelTC build for pushes, pull requests, and tags. Tagging a release builds the artifacts, uploads them, and publishes release notes from the matching section in `CHANGELOG.md`.

## Install

Copy `KoboRoot.tgz` to `KOBOeReader/.kobo/` and reboot. It is active on install. Covers are mirrored as books are shown, or all at once through **More > Repair Book Covers**. See `res/doc` for details.

## Uninstall

Delete `KOBOeReader/.adds/nickel-cover-fix/uninstall` and reboot. This removes the mod and its mirror folder; `.kobo-images` is untouched.
