# NickelCoverFix — TODO / v2 ideas

## Current v1

The current build provides reactive cover capture and placeholder replacement, plus a user-triggered Repair
Book Covers backfill. It is keyed by stable ContentID, never reads book files, never writes `.kobo-images`,
and keeps capture bounded and off the render-hook call path.

Repair enumerates the library on the main thread and copies Kobo's on-disk cover cache in small event-loop
chunks. Its UI uses Kobo's native `N3ProgressDialog`. The More-page row is located through Qt's object tree;
there are no MoreView private layout offsets in the current implementation.

## Remaining v2 ideas

- Fetch missing covers during Repair through Kobo's own image-provider path after the user has repaired the Kobo
  account. This remains deliberately out of scope for v1 because it introduces asynchronous network state,
  timeouts, and additional failure handling.
- Add device-side verification on additional firmware families before widening the native-dialog ABI support.
- Consider replacing the private `IconLeftButton` constructor path with a public Qt widget if Kobo's native
  button ABI changes or if a fully portable menu insertion path becomes necessary.

## Decided — not doing

- No desktop backfill script; backfill must run on-device using Kobo's own cache.
- No DRM or book-file cover extraction.
