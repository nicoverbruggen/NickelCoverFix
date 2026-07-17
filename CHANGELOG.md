# Changelog

## v0.2

### Added

- **Grid-view cover serving:** covers are now served in the library's **grid view** (the default cover layout on newer Colour devices), not just list view. With `ncf_force_serve:1` the aggressive override reaches the grid, home tiles, and carousels, which all draw through Kobo's async `BookCoverView` and previously showed Kobo's cover regardless of the setting. The "serve a mirror instead of the placeholder" path was already universal.
- **First-run auto-cache:** on a fresh install with nothing cached yet, and once Kobo's own cover cache has covers to copy, the whole library is mirrored once, automatically, about 3.5s after the first home screen (post-boot and one-shot, so it never slows startup). It shows a "Preparing covers for first-time use..." dialog; if Kobo's cache is still empty it does nothing and retries on a later boot instead of marking itself done. When it finishes, the covers already on screen refresh in place, so no tab switch is needed.

### Changed

- **Repair Book Covers placement:** the More-page row is now inserted just above **Settings** instead of at the very bottom of the list.
- **Manual Repair dialog** is now titled "Copying covers again...". The first-run pass uses the "Preparing covers for first-time use..." title.

### Compatibility

- **Supported firmware floor: Kobo 4.30.18838 and newer.** Every libnickel symbol the mod binds is annotated and symbol-checked from that version, and the private-object sizes and the grid/`BookCoverView` paths were verified on it. Hooks and lookups remain optional, so an older build degrades feature by feature rather than failing to load.

## v0.1

### Added

- **Offline cover mirror:** preserves Kobo-obtained covers under stable ContentID-based filenames and serves them when Nickel would otherwise show the title/author placeholder.
- **Blacklist support**: add `blacklist.txt` and make covers exempt from being replaced. You can replace existing images with your own to effectively get custom covers.
- **Multiple cover paths:** supports library covers, lock/sleep-screen covers, and the home screen.
- **Repair Book Covers:** adds a post-boot, user-triggered backfill using Kobo's native progress dialog.

### Safety

- **Bounded capture and storage:** render hooks only queue work; image decoding, cache writes, and Repair run
  outside the render call stack with image, cache-size, and free-space limits.
- **Fail-closed optional UI:** the manual Repair row is omitted unless Kobo's complete `N3ProgressDialog` and
  `IconLeftButton` APIs are available and the More-page layout can be identified unambiguously.
- **Maintainer documentation:** source comments explain the hooks, private ABI assumptions,
  firmware validation, and safety decisions.
