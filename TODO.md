# NickelCoverFix — TODO / v2 ideas

## Where v1 stands (built, this session)
Two reactive, boot-safe hooks (static `VolumePixmapView` helpers):
- **Capture** `getCoverImage` — mirror the real cover nickel yields (a cache hit) into
  `.adds/nickelcoverfix/covers/<sha1(ContentID)>.png`, once per book per session.
- **Serve** `generateDefaultCover` — return that mirror instead of the placeholder.
Keyed by the **stable ContentID** (not the mutable imageId), so it survives offline gaps and imageId changes.
No DRM, no book files, no enumeration; arm-flag gated, fail-open, size-capped. `.kobo-images` never written.

**v1 limitation → what v2 fixes:** v1 only mirrors a cover once you view it online (so the real cover lands in
the RAM cache). Books already on the device at install time, never re-viewed online, aren't mirrored yet.

---

## v2 — Backfill existing covers (the main ask)
Proactively mirror covers for books already on the device, so they work offline without viewing each online.

- **Source = Kobo's own `.kobo-images` cache** (never the book file → no DRM). If a cover isn't there, the user
  runs Kobo's stock **"Repair my Kobo account"**, which re-fetches all covers into `.kobo-images`; the backfill
  then picks them up. Copying only what's on disk is acceptable by design (Repair fills the gaps).
- **Mechanism:** for each book, map ContentID → find its `.parsed` in `.kobo-images` → copy bytes to
  `.adds/nickelcoverfix/covers/<sha1(ContentID)>.png` (the same key the serve hook reads). `QImage::load`
  sniffs the format at serve time.

### Trigger — pick one (recommend the menu item: safest, never near boot)
- **"Repair covers" More-menu item (RECOMMENDED)** — user-initiated, runs fully post-boot, zero boot-path
  risk, pairs naturally with the Repair-my-account flow. Use the proven More-row pattern: hook `MoreView` ctor
  (`_ZN8MoreViewC1EP7QWidget`) → append an `IconLeftButton` → moc'd QObject bridge for the tap slot (copy the
  NacBridge pattern from `/Users/nico/OSS/kobo-investigation/mod/NickelAltCover`).
- **Auto once-per-install, post-boot** — `QTimer::singleShot` after the home screen is up, guarded by a
  `.adds/nickelcoverfix/backfilled` flag. Needs careful post-boot timing; only if we don't want a menu.

### Progress UI — native `N3ProgressDialog` (API confirmed in libnickel; don't build a custom Qt view)
"Copying covers…" with a real progress bar. Symbols/addresses (Libra 2 libnickel, portable by name):
- ctor `N3ProgressDialog()` @0x10e684c (`_ZN16N3ProgressDialogC1Ev`)
- `setTitle` @0x10e66f4 · `setText` @0x10e671c · `setProgress(int)` @0x10e679c
- `setProgressBarRange(int,int)` @0x10e67ac · `setProgressBarMaximum(int)` @0x10e67bc ·
  `setProgressBarVisible(bool)` @0x10e676c
- `showCancelButton` @0x10e681c · `setCancelButtonText` @0x10e682c · `setCancelButtonEnabled` @0x10e683c
- **Reference usage to copy:** `ManageDictionariesWorkflow::showProgressDialog(int)` @0xffdd2c — does
  `operator new` + ctor, `setProgressBarRange`, `QWidget::setAttribute`, `setTitle`, `setProgressBarVisible`,
  then shows it. Non-blocking: drive `setProgress` from our own loop; do NOT `exec()`.

### Execution / boot-safety
- Enumerate: `VolumeManager::forEach(QString(), std::function<void(Volume const&)>)` @0xa6bbec (main thread,
  warm cache); also `forEachBook` @0xa6bdf0.
- With a visible progress dialog, run **MAIN-THREAD CHUNKED** (QTimer, N books per tick, update `setProgress`
  each tick) — simpler than a background thread (Qt GUI is main-thread-only anyway) and keeps e-ink responsive.
- Per book: source via `Image::getFileName(Device,Volume,type)` @0x9b3d5c (or glob `.kobo-images` for the
  imageId); dest = `sha1(ContentID)`; atomic write (temp + rename); skip if dest exists. Fail-open, capped,
  arm-flag + `ncf_backfill` gated, once-flag.

---

## v2 idea — fetch missing covers online during Repair (if Wi-Fi on)
Today Repair is local-only (copies on-disk covers; skips books with none) and directs the user to Kobo's
Settings > Device information > "Repair your Kobo account" for the rest. Make Repair fetch the missing ones
itself when online.
- **Lever (RE'd):** `ImageProvider::retrieveImage(Device,Volume,QString,InternetProvider*,ImageReceiver*)`
  @0x8cc03c → for a purchased book this runs the `ImageWorker` network path (`WebRequester::onestoreImage`)
  and populates the RAM ImageCache; our capture hook then mirrors it on the next render. Alt: fetch
  `Content::imageUrl()` @0x690c28 ourselves via QNetworkAccessManager and save straight to the mirror.
- **Why it's v2, not v1:** async — Repair's progress bar would advance on fetch completions (per-book
  ImageReceiver callbacks / QNAM finished signals), with queueing, timeouts, and error handling; online-only.
  Owner's note (correct): once the placeholder is shown nickel doesn't auto-replace it with the live web
  cover, so forcing `retrieveImage` during Repair is the way to trigger it.
- Also: prime-on-onboarding (do this automatically on first run if online). Interim shipped in v1: the Repair
  summary dialog names the exact path to run Kobo's own repair.

---

## Verify on device before building v2 (needs USB)
- `.parsed` files are a `QImage`-loadable format (so a raw byte-copy is servable):
  `find /Volumes/KOBOeReader/.kobo-images -name '*.parsed' | head -1 | xargs xxd -l16`
  (FF D8 FF = JPEG, 89 50 4E 47 = PNG).
- "Repair my Kobo account" writes covers to `.kobo-images` on disk (not just RAM).
- Rough on-disk cover count vs library size.

## Decided — not doing
- No desktop backfill script (backfill must be on-device).
- No DRM / de-DRM cover extraction from book files (legal; see kobo-investigation report 65).

## Reference
Full RE + rationale: `/Users/nico/OSS/kobo-investigation/reports/66-kobocoverfix-scope.md` (and report 65 for
why the book-file/DRM route is off the table).
