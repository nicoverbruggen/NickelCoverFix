# Changelog

## v0.1

### Added

- **Offline cover mirror:** preserves Kobo-obtained covers under stable ContentID-based filenames and serves them when Nickel would otherwise show the title/author placeholder.
- **Blacklist support**: add `blacklist.txt` and make covers exempt from being replaced. You can replace existing images with your own to effectively get custom covers.
- **Multiple cover paths:** supports library covers in both list and grid views, lock/sleep-screen covers, and the home screen tiles and carousels. With `ncf_force_serve:1` the scoped override now reaches the grid too, which is the default cover layout on newer Colour devices.
- **First-run auto-cache:** on a fresh install with nothing cached yet, mirrors the library's covers once, automatically, ~3.5s after the first home screen. Post-boot and one-shot, so it never slows startup. It only runs when Kobo's own cover cache already has covers to copy; on an unsynced device it does nothing and retries on a later boot instead of marking itself done. Shows a "Preparing covers for first-time use..." dialog, and the covers already on screen refresh in place when it finishes, so no tab switch is needed.
- **Repair Book Covers:** adds a post-boot, user-triggered backfill using Kobo's native progress dialog, placed just above the More page's Settings row. Uses a "Copying covers again..." dialog to distinguish it from the first-run pass.

### Safety

- **Bounded capture and storage:** render hooks only queue work; image decoding, cache writes, and Repair run
  outside the render call stack with image, cache-size, and free-space limits.
- **Fail-closed optional UI:** the manual Repair row is omitted unless Kobo's complete `N3ProgressDialog` and
  `IconLeftButton` APIs are available and the More-page layout can be identified unambiguously.
- **Maintainer documentation:** source comments explain the hooks, private ABI assumptions,
  firmware validation, and safety decisions.
