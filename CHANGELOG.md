# Changelog

## v0.1

### Added

- **Offline cover mirror:** preserves Kobo-obtained covers under stable ContentID-based filenames and serves
  them when Nickel would otherwise show the title/author placeholder.
- **Multiple cover paths:** supports library covers, lock/sleep-screen covers, and the home screen's
  **Now Reading** shelf.
- **Repair Book Covers:** adds a post-boot, user-triggered backfill using Kobo's native progress dialog.

### Safety

- **Bounded capture and storage:** render hooks only queue work; image decoding, cache writes, and Repair run
  outside the render call stack with image, cache-size, and free-space limits.
- **Fail-closed optional UI:** the manual Repair row is omitted unless Kobo's complete `N3ProgressDialog` and
  `IconLeftButton` APIs are available and the More-page layout can be identified unambiguously.
- **Maintainer documentation:** source comments and `REPORT.md` explain the hooks, private ABI assumptions,
  firmware validation, and safety decisions.

The current implementation was validated against Kobo firmware 4.45.23697. Other firmware families require
validation before deployment, especially where private Kobo C++ object sizes are involved.
