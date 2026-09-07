# Distribution

Provenance-Status: complete

`Provenance-Status` is read by `tools/release_audit.py`; `complete` means every
external component and asset is listed in THIRD_PARTY.md and the project
licence (GPL-2.0-or-later, see LICENSE) is in place. Set it back to
`incomplete` when you add something not yet listed.

## What is published

- Source: this repository.
- Builds: the VPK is attached to the GitHub Releases page together with its
  SHA-256. It contains the recompiled game code and the Vita runtime.
- Never published: the game's resources (`resources/packed/*.a`, about 1 GiB),
  saves, keys, Sony modules or firmware, `libshacccg.suprx`. You copy the
  resources from your own PC installation (see INSTALL.md); without them the
  app does not start.

## What stays out of Git

`.gitignore` already excludes these; do not force-add them:

- the PC executable, resources, saves, keys and Workshop content; the five
  LiveArea images under `recomp/vita/assets/sce_sys/` are the only tracked
  artwork;
- translated game code (`guest_####.c`) and other generated sources;
- build outputs: ELF, VELF, SELF, `eboot.bin`, VPK, SFO, static archives;
- Sony SUPRX/SPRX/SKPRX modules, firmware, SDK binaries, console keys;
- logs, dumps, screenshots and other local diagnostics.

## Before a release

1. Build from a clean checkout of the tagged revision with the release's CMake
   initial cache (`release/<version>.cmake`).
2. Run `python tools/release_audit.py --strict --include-untracked` on that
   checkout; it must end with `release audit: GO`.
3. Describe the tested build configuration and the hardware session in
   `release/RELEASE_NOTES.md` (steps in `release/README.md`), then attach the
   VPK to the GitHub Release together with its SHA-256 and the eboot SHA-256.
