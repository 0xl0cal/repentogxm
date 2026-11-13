# Release staging

This directory tracks text only: the release notes and, per release, the
CMake initial cache with the complete option set of its build. Binaries (VPK,
`eboot.bin`, ELF, VELF, SFO), generated guest code and the PC executable are
excluded from Git; the VPK of each release is attached to the GitHub Releases
page together with its SHA-256 and the eboot SHA-256.

`RELEASE_NOTES.md` has one section per release plus the state of the
development line. Steps for a release:

1. Build from a clean checkout of the tagged revision, configured from the
   release's CMake initial cache (`release/<version>.cmake`), and run
   `python tools/release_audit.py --strict --include-untracked` there; it must
   end with `release audit: GO`. **At the tag, the body of the new recipe
   (every line after its header comment) is the output of**
   `python tools/release/check_release_recipe.py --emit-body <build>/CMakeCache.txt --link-id rel:<version>`
   **for the build that ships**, not a set composed from build scripts or kept
   up by hand (every `ISAAC_VITA_*` BOOL and STRING entry plus
   `ISAAC_GUEST_BASE`, `SOURCE_DATE_EPOCH` and `CMAKE_BUILD_TYPE`; per-machine
   paths stay on the command line and fail the check if they appear in a
   recipe). Keep the build commands of the header and drop any DRAFT
   paragraph. Then
   `python tools/release/check_release_recipe.py release/<version>.cmake --against <build>/CMakeCache.txt`
   must end with `release recipe check: PASS`: every required fix knob (the
   eight issue fixes, the GL recorder admission cache and the five fixes of
   2026-11-06) ON, and no entry
   that differs from the cache. A draft recipe that is not yet a cache copy
   says so in its header (`v0.1.2-alpha.cmake`, never shipped).

   After its tag a recipe is a living file, not a frozen copy. A line that
   adds a knob pins it in the release profile, and host tests check those
   pins in `release/v0.1.1-alpha.cmake` (a pinned value, or that the knob
   stays out of the file): `recomp/test_vita_trophies.py`,
   `recomp/vita/test_vita_debug_overlay.py`,
   `recomp/test_gpr_flush_mask_proof.py`,
   `recomp/test_guest_stack_memblock.py`, `recomp/test_uniform_set_sites.py`,
   `recomp/test_vita_game_log_async.py` and
   `recomp/vita/test_vita_runtime_settings.py`. So on this line
   `v0.1.1-alpha.cmake` is no longer the shipped option set: on 2026-10-26 it
   held 14 entries the release build's cache never had, 11 later knobs pinned
   OFF and three ON (`FBO_READBACK_FIX` and `RENDER_SCALE_DEFAULT`, both
   approved changes of the release shape, and `RUNTIME_SETTINGS`, which the
   render scale default requires). The shipped set is the file at the
   `v0.1.1-alpha` tag (249 entries, equal to the release build's cache).
   `--against` an older cache reports such later pins as `[cache-absent]`,
   and keys the cache has but the recipe lacks as `[recipe-absent]`, apart
   from `[value]` differences.
2. Check the VPK members against the previous release: only `eboot.bin` is
   expected to change unless the notes say otherwise; resources, saves and
   keys are never packaged.
3. Play a hardware session on a real Vita with the exact package (boot,
   Continue, rooms, a save) and record in the notes the build id, that no
   fault was logged and that every native seam reports fallbacks=0.
4. Attach the VPK and its SHA-256 to the GitHub Release; hashes live there,
   not in the notes.
