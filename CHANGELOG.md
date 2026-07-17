# Changelog

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
