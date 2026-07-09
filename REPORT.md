# NickelCoverFix safety and build report

Date: 2026-07-09

Firmware reference: `kobo-update-4.45.23697/KoboRoot/`

## Build setup

The `NickelHook` submodule was initialized successfully at:

- URL: `https://github.com/pgaskin/NickelHook.git`
- pinned checkout: `de76894806e4f8a26048b4c9a7370493e68d671d`

The containerized ARM build completed successfully with Qt 5.2.1 and `-Wall -Wextra -Werror`.
The shared library linked with `--no-undefined`, was stripped, and was packaged into `KoboRoot.tgz`.
`git diff --check` passes.

The build still reports non-fatal environment warnings because the container does not detect a local NickelTC
prefix and cannot determine the source version through Git. The ARM build itself succeeds.

## Safety changes applied

- Render hooks only queue capture work; copying and encoding happen after the hook returns.
- Image decoding is bounded to 8192 pixels per side, 12 MP, and 8 MiB per file.
- The mirror has a 128 MiB total budget and preserves 32 MiB of free storage.
- Temporary files are validated before replacement and cleaned up on failure.
- Source timestamps are refreshed when a Kobo cache file is newer.
- Missing `content_isPurchaseable` fails closed instead of treating unknown content as owned content.
- `VolumePixmapView::getVolume` is used when available to avoid stale widget-pointer decisions.
- Persistent logging can be disabled with `ncf_log:0`.

## Manual Repair UI

Repair uses Kobo's native `N3ProgressDialog` rather than a custom Qt dialog. The More-page row is discovered
through the live Qt object tree rather than MoreView private UI offsets.

The entire manual feature is fail-closed: if the complete `N3ProgressDialog` or `IconLeftButton` API is not
available, the row is omitted and Repair does not enumerate or process the library. Cover capture and serving
remain independent.

The native progress dialog allocation size (`0x78`) and the `IconLeftButton` allocation size (`0x58`) remain
private Kobo C++ ABI assumptions validated against firmware 4.45.23697. These require revalidation on other
firmware families.

## Firmware validation

Against the supplied `libnickel.so.1.0.0`:

- BuildID: `f043a6aed1c782880be5d6075a22619bd8e49dcb`.
- Required hook and dlsym symbols were present.
- `getCoverImage` and `generateDefaultCover` signatures matched the wrappers, including the QImage hidden
  structure-return ABI.
- The native progress-dialog API and its `0x78` allocation were confirmed from Kobo's own
  `ManageDictionariesWorkflow::showProgressDialog` caller.

## Conclusion

The modified source builds cleanly for the Kobo ARM target and packages correctly. The changes reduce boot-path
risk, bound storage and image work, and make the optional manual Repair UI fail closed when its private Kobo
APIs are unavailable. A device smoke test of More > Repair Book Covers remains advisable after UI changes.
