# Release staging

This directory tracks text only: the release notes and, per release, the
CMake initial cache with the complete option set of its build. Binaries (VPK,
`eboot.bin`, ELF, VELF, SFO), generated guest code and the PC executable are
excluded from Git; the VPK of each release is attached to the GitHub Releases
page together with its SHA-256 and the eboot SHA-256.

`RELEASE_NOTES.md` has one section per release plus the state of the
development line. Steps for a release:

1. Build from a clean checkout of the tagged revision, configured from the
   release's CMake initial cache (`release/<version>.cmake`, copied from the
   build's `CMakeCache.txt`), and run
   `python tools/release_audit.py --strict --include-untracked` there; it must
   end with `release audit: GO`.
2. Check the VPK members against the previous release: only `eboot.bin` is
   expected to change unless the notes say otherwise; resources, saves and
   keys are never packaged.
3. Play a hardware session on a real Vita with the exact package (boot,
   Continue, rooms, a save) and record in the notes the build id, that no
   fault was logged and that every native seam reports fallbacks=0.
4. Attach the VPK and its SHA-256 to the GitHub Release; hashes live there,
   not in the notes.
