# Contributing to NickelCoverFix

Technical guide for building, testing, and changing the mod. For how cover capture and serving
actually work, the private-object construction behind the optional Repair UI, and the firmware
assumptions each feature makes, read the source comments in `src/nickelcoverfix.cc`, which document
the hooks, private ABI assumptions, and safety decisions alongside the (deliberately non-technical)
README. These mods follow the shared conventions in
[NickelGuidance](https://github.com/nicoverbruggen/NickelGuidance).

## Building

Needs [podman](https://podman.io) (or Docker); the ARM cross-toolchain runs in a container, so
your host never needs it:

```sh
git clone --recursive https://github.com/nicoverbruggen/NickelCoverFix   # --recursive: NickelHook is a submodule
cd NickelCoverFix
./build.sh                                                               # make clean all strip koboroot in ghcr.io/pgaskin/nickeltc:1.0
```

This produces `KoboRoot.tgz` at the repo root. `./build.sh <targets>` passes other make targets
through; `NICKELTC_IMAGE` overrides the toolchain image.

Version stamping: NickelHook.mk bakes `git describe --tags --always --dirty` into `NH_VERSION`:
the git **tag** when you're on one, otherwise a **commit hash**. `build.sh` excludes `.git`, so
local container builds are unstamped (`dev`); CI (checkout with `fetch-depth: 0`) produces the
authoritative stamped artifacts.

## Testing on a device

1. Copy `KoboRoot.tgz` into the Kobo's hidden `.kobo` folder over USB.
2. Eject and reboot; the firmware installs it and deletes the tgz.
3. The mod's folder is `KOBOeReader/.adds/nickel-cover-fix/` (`config`, `default`, `doc`,
   `uninstall`, and the `nickel-cover-fix.log`). The `config` file is seeded from `default` on
   first boot.

**Boot safety / recovery**: NickelHook's failsafe (`failsafe_delay = 3`) uninstalls the mod if
Nickel crashes within ~3 s of boot. Power off within that window to recover a bad build. Deleting
the `uninstall` file in the mod folder and rebooting also removes it. The mod never writes Kobo's
own `.kobo-images` cache.

## Logs & debugging

The mod logs to `KOBOeReader/.adds/nickel-cover-fix/nickel-cover-fix.log` (and to syslog via
`nh_log`, viewable with `logread` on a shell-enabled device). Every message carries the mod
version; the startup block logs the mod version, the firmware version, the effective config, and
the resolved-symbol map (which optional features attached on this firmware).

- A healthy boot writes only that startup block.
- `ncf_log:1` (the default) keeps the on-device log file; `ncf_log:0` sends messages to syslog
  only. Per-cover messages are rate-limited so a large library can't grow the log without bound.
- The log is size-capped and rotates once to `nickel-cover-fix.log.old`.

## Firmware compatibility

Every hooked `libnickel` symbol carries a `//libnickel <first> <last|*> <symbol>` annotation. The
`test/syms` checker (CI job `syms`, also runnable locally with Go:
`cd test/syms && go build -o ../../test.syms . && cd ../src && ../test.syms`) verifies them
against ~70 real firmware dumps (4.6 → 4.45).

All hooks are `.optional = true` and null-checked at the use site: a missing symbol makes a
feature inert, never fatal. Keep it that way. The optional Repair UI constructs a few private
Kobo widgets whose class sizes were validated on firmware **4.45.23697**; it over-allocates and
fails closed, and stays inert where its symbols don't resolve. Targets Kobo **4.x** firmware only;
5.x (Qt 6 / Chromium) is out of scope and the mod stays inert there.

## Pull requests

- Add a `## Unreleased` entry to `CHANGELOG.md` for any user-visible change (release notes are
  generated from it).
- Annotate any new `libnickel` symbol with `//libnickel …`; CI verifies it.
- State the device + firmware you tested on, and attach the relevant `nickel-cover-fix.log`
  excerpt (the PR template asks for both).

## Releases (maintainers)

Rename `## Unreleased` in `CHANGELOG.md` to the new `## vX.Y`, tag the commit `vX.Y`, and push the
tag. CI builds, extracts that section as the release notes, attaches `KoboRoot.tgz`, and fails if
the CHANGELOG section is missing.
