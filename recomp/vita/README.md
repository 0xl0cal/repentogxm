# recomp/vita: the PS Vita target

This is the PS Vita target of repentogxm, a port of The Binding of Isaac:
Repentance. Overview, status and credits: [`../../README.md`](../../README.md).

This directory holds the CMake target, the Vita runtime glue, the pinned
vitaGL and OpenAL Soft build recipes, and the Vita-specific tests. The game's
x86 code is translated to C ahead of time and compiled for the Vita CPU; that
code is the guest, the Vita-side runtime is the host. KAGE is the game's own
engine (Nicalis); `ISAAC_VITA_KAGE` names the renderer and input glue for it.

Dated device measurements and open problems: [`../../STATUS.md`](../../STATUS.md).
Installation: [`../../INSTALL.md`](../../INSTALL.md). This file is for builders.

## What the build produces

- A VPK with title ID `ISAACR001`. It packages the unpacked PE as
  `app0:/isaac-ng.exe.unpacked.exe` next to `eboot.bin`; game resources are
  not packaged.
- The PE must be exactly 8,650,240 bytes with SHA-256
  `31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404`; anything
  else is rejected as `unsupported unpacked PE`.
- The translated image is mapped at the fixed address `0x98000000` through
  kubridge (`kuKernelAllocMemBlock`), so `eboot.bin` is built as an unsafe
  SELF (the Vita executable format) with address randomization off, and
  kubridge is a hard requirement.
- Everything is compiled for the soft-float ABI (`-mcpu=cortex-a9 -mfpu=neon
  -mfloat-abi=softfp -mthumb`); a hard-float VitaSDK is rejected by
  `tools/build_vita.py`.
- The guest heap is 81 MiB below the image. CMake accepts 1-63, 65-79 and
  81 MiB; 64 and 80 are rejected because their alignment overlaps the fixed
  image.
- The corpus (the translated C for the whole PE plus its `manifest.json`) is
  checked at configure time: image base, sizes, SHA-256s and recipe id. A
  mismatch fails the build or aborts before the PE loads.

## Building

The public entry point is `tools/build_vita.py`. Run it from the repository
root with a softfp VitaSDK:

```text
python -m pip install -r requirements.txt
python tools/build_vita.py --pe /path/to/isaac-ng.exe.unpacked.exe \
  --vitasdk /path/to/vitasdk-softfp
```

`python tools/build_vita.py --self-test` (host-only checks of the wrapper)
currently fails at this commit on a stale source-count assertion; it is not
part of the build.

Generated C goes outside the checkout by default
(`../repentogxm-build/vita-generated`); `--generated-dir`, `--analysis-dir` and
`--build-dir` change the locations. The wrapper regenerates the corpus,
verifies the PE and manifest, configures and builds with Ninja, and prints
artifact hashes. It never downloads or publishes game data. `--release` is a
fail-closed verification mode and publishes nothing.
`python tools/build_vita.py --help` is the authority for flags and defaults.

`build_vita.py` builds a conservative configuration: it forces
`ISAAC_VITA_FULLSPEED_SCHEDULER=OFF`, leaves Lua off unless `--lua
--lua-source` is given, selects the stock vitaGL profile only under
`--texture-churn-profile`, and has no switch for most options listed below.
The device builds measured in September 2026 were configured directly with CMake.

## Options the measured device builds use

Reconstructed from the 2026-09-04 build logs; the exact cmake command lives on
the build machine, not in the repo, so this list is UNVERIFIED in-repo.
Defaults are OFF unless noted.

- Renderer: `ISAAC_VITA_KAGE`, `ISAAC_VITA_DIRECT_DEFAULT`,
  `ISAAC_VITA_RENDER_SURFACE_NATIVE`, `ISAAC_VITA_VITAGL_STOCK_REFERENCE`,
  `ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS`, `ISAAC_VITA_VITAGL_SHADER_CACHE`,
  `ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY`, `ISAAC_VITA_GXM_STATE_SHADOW`,
  `ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS`, `ISAAC_VITA_DISPLAY_RASTER=960`,
  `ISAAC_VITA_VITAGL_RAM_RESERVE_MB=32`, `ISAAC_VITA_LOADING_PRESENTATION`
  (default ON), `ISAAC_VITA_CLOCK_BOOST` (default ON).
- Audio: `ISAAC_VITA_AUDIO` (default ON), `ISAAC_VITA_AUDIO_WORKER`,
  `ISAAC_VITA_NATIVE_VORBIS`, `ISAAC_VITA_NATIVE_VORBIS_ASYNC`,
  `ISAAC_VITA_OPENAL_POOL` (default ON).
- Lua: `ISAAC_VITA_LUA` with `ISAAC_LUA53_SOURCE_DIR`,
  `ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=128` (default),
  `ISAAC_VITA_LUA_GCCOLLECT_CLAMP`, `ISAAC_VITA_LUA_SCOPE_FASTPATH` (default
  ON), `ISAAC_VITA_LUA_IMPORT_FASTDISPATCH`, `ISAAC_VITA_LUA_NATIVE_INDEX`,
  `ISAAC_VITA_LUA_NATIVE_GETCLASS`.
- Loading, I/O and memory: `ISAAC_VITA_NATIVE_PNG` (`_RESERVE_MB=16`),
  `ISAAC_VITA_NATIVE_INFLATE`, `ISAAC_VITA_ARCHIVE_FILE_CACHE`,
  `ISAAC_VITA_CRT_SEEK_SHADOW` (needs the archive cache),
  `ISAAC_VITA_ASYNC_SAVE_WRITE`, `ISAAC_VITA_LOG_ASYNC`,
  `ISAAC_VITA_HEAP_MB=81`, `ISAAC_VITA_HEAP_OVERFLOW_MSPACE`,
  `ISAAC_VITA_TEXEL_SCRATCH`.
- Translated code: `ISAAC_VITA_TRANSLATED_CPU` with `_GPR_LOCAL`,
  `_LEAF_INLINE`, `_SSE_LOWER` and the four default-ON sub-options
  (`_COVERAGE_HOOKS_OFF`, `_FS_BASE_INLINE`, `_HOT_LAYOUT`, `_FLAGS_LOCAL`);
  `_F32_VFP` and `_COLD_REST` stayed OFF;
  `ISAAC_VITA_DISPATCH_TABLE`, `ISAAC_VITA_IAT_DIRECT`, `ISAAC_VITA_LAYOUT_HUB`,
  `ISAAC_VITA_SYNC_IMPORT_FASTPATH`, `ISAAC_VITA_SYNC_INLINE_FASTPATH`,
  `ISAAC_VITA_KAGE_MUTEX_SEAM`, `ISAAC_VITA_FLOOR_THUNK_FASTPATH`,
  `ISAAC_VITA_GL_SHIM_FASTDISPATCH`, `ISAAC_VITA_GL_SHIM_TABLE_TOKENS`,
  `ISAAC_VITA_GL_SHIM_RAW_ARGS`, `ISAAC_VITA_SHADER_ATTRIB_FASTPATH`.
- Scheduler: `ISAAC_VITA_FULLSPEED_SCHEDULER` and
  `ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES`. The game simulates at 30 Hz;
  the scheduler presents completed phases at 60 Hz.

Three options need the corpus generated with an environment variable set for
the `recomp/gen_all.py` run (`tools/build_vita.py` imports `gen_all`, so set
it before running the wrapper): `GUEST_FLOOR_THUNK_FASTPATH=1` for
`ISAAC_VITA_FLOOR_THUNK_FASTPATH`, `GUEST_LEAF_INLINE=1` for
`ISAAC_VITA_TRANSLATED_CPU_LEAF_INLINE` and `GUEST_SSE_LOWER=1` for
`ISAAC_VITA_TRANSLATED_CPU_SSE_LOWER`. The production corpus is generated with
all three; configure-time checks on the generated files fail otherwise.
Profiling options (`ISAAC_VITA_PHASE_PROFILE`, `ISAAC_VITA_GL_TIME_PROFILE`,
`ISAAC_VITA_GUEST_SAMPLER`, and the `*_RECEIPT` and `*_VERIFY` options) are
for diagnostic builds; the CMake cache keeps them between configures, so
switch them OFF explicitly for a play build.

## Renderer

- One process-wide vitaGL context with the runtime shader compiler, which is
  why `ur0:data/libshacccg.suprx` is required. The game keeps its 960x540
  logical viewport inside the 960x544 panel.
- `ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON` links the pinned upstream vitaGL
  `73dd57a` built by [`vitagl-stock-reference/`](vitagl-stock-reference/README.md)
  with the patches selected by the options above. This is the renderer of the
  perf builds recorded in STATUS.md. It requires
  `ISAAC_VITA_DIRECT_DEFAULT=ON` and rejects the diagnostic `*_PROBE` options
  (first-frame, raw-GXM, known-colour, raster, screenshot),
  `ISAAC_VITA_IO_PROFILE` and `ISAAC_VITA_STALL_PROBE`; audio and the loading
  screen are compatible.
- With it OFF the build links the older pinned overlay `f4b23b6` from
  [`vitagl-overlay/`](vitagl-overlay/README.md).
- `ISAAC_VITA_RENDER_SURFACE_NATIVE`: the world is drawn at its native 480x270
  and upscaled once. `ISAAC_VITA_DISPLAY_RASTER` is 960 (native); 720 (720x408)
  and 480 (480x272) exist but are not the supported mode.
- `ISAAC_VITA_VITAGL_SHADER_CACHE=ON` stores compiled shaders under
  `ux0:data/isaacr001/shader-cache`. `ISAAC_VITA_CLOCK_BOOST` sets the
  CPU/bus/GPU/XBAR clocks to 444/222/222/166 MHz at startup.
- The loading screen shows the Specialist Dance animation when
  `ISAAC_VITA_LOADING_SPECIALIST=ON` (frames in
  `recomp/runtime/kage_vita_loading_specialist.inc`; `tools/build_vita.py`
  passes it by default). OFF draws a procedural fallback.

## Audio

OpenAL Soft 1.19.1 is built as a project-local static archive by
[`openal-overlay/`](openal-overlay/README.md) with two patches (voice limit,
allocator redirect into the `ISAAC_VITA_OPENAL_POOL` pool). `alsoft.conf`
(`sources = 80`) is installed as `app0:/alsoft.conf` when `ISAAC_VITA_AUDIO=ON`.
`ISAAC_VITA_AUDIO_WORKER` pins the mixer thread to CPU 2.
`ISAAC_VITA_NATIVE_VORBIS` replaces the translated stb_vorbis decoder with the
vendored v1.04 compiled natively on a CPU 2 worker; `_ASYNC` returns the PCM
the worker has ready and caps the game thread's own decoding at a small time
budget.

## Lua and mods

`ISAAC_VITA_LUA=ON` builds the game's Lua 5.3.3 bridge from a user-supplied
pristine `lua-5.3.3` source tree (`ISAAC_LUA53_SOURCE_DIR`, or `--lua
--lua-source` in the wrapper); every file is pinned by SHA-256. Mods live at
`ux0:data/isaacr001/mods/<workshop id>/`; a `disable.it` file inside a mod
directory disables it; the Save/Mod Manager app (see INSTALL.md; not yet
tested on a real Vita) toggles that file. A build with
`ISAAC_VITA_LUA=OFF` crashes at startup when a mod is enabled (observed
2026-09-03), so ship Lua ON whenever mods are present.

## Controls

The Vita pad and front touch are presented to the game as one XInput (Xbox)
controller; the game keeps its own menu/gameplay context and analog handling.

| Vita | XInput | Default action |
|---|---|---|
| D-pad | D-pad | menu navigation / movement |
| Left stick | left stick | movement |
| Right stick | right stick | shooting |
| Cross | A | confirm / shoot down |
| Circle | B | cancel / shoot right |
| Square | X | shoot left |
| Triangle | Y | shoot up |
| L | left shoulder | bomb |
| R | left trigger | active item |
| Select | right trigger | tap: swap Schoolbag/pocket items, hold: drop |
| Start (tap) | Start | pause |
| Start + L | right shoulder | pill/card |
| Start + R | Back | map |
| Start (hold 0.8 s) | keyboard R | restart (the game's own hold time then applies) |
| Touch: active-item icon | left trigger | active item |
| Touch: minimap | Back | map while held |
| Touch: pocket icon | right shoulder | pill/card/pocket item |

A touch stays in the zone where it began. Stick axes reach the game at full
range; only the keyboard-emulation fallback uses a fixed 64-of-128 threshold.
There is no remapping UI. The Manager (untested on hardware) has a read-only
Controls page with this table. Touch zones were not re-verified in the
September 2026 sessions.

## Data and logs

- Everything writable is under `ux0:data/isaacr001/`: `resources/`,
  `Documents/` (saves; back them up before replacing a build, see INSTALL.md;
  `tools/isaac_vita_sync.py pull` fetches dated copies), `mods/`,
  `shader-cache/`.
- Port log: `ux0:data/isaacr001/first-arm-fault.log` (clock settings, which
  native replacements were activated, `FAULT` lines). Game log:
  `Documents/My Games/Binding of Isaac Repentance/log.txt` below the same
  root (Lua and mod errors).
- The phase profiler (`ISAAC_VITA_PHASE_PROFILE`) prints its records, lines
  starting with `ph120.`, through the kernel debug printf; they are visible
  only with a debug-log plugin, not in a file.

## Tests

- `python tools/build_vita.py --self-test` (host-only wrapper checks; currently
  failing on a stale source-count assertion, not part of the build).
- `recomp/vita/test_*.sh` and `recomp/vita/test_*.py`; most need a softfp
  VitaSDK, none need a console.
- Stock vitaGL: `test_vitagl_stock_reference_recipe.py`,
  `test_kage_vita_stock_reference.py`, `test_vitagl_stock_patch_chain.py`,
  `cmake -P test_kage_vita_stock_reference_cmake.cmake`.
- `vitagl-overlay/test.sh`, `openal-overlay/test.sh`,
  `openal-overlay/test_deterministic_archive.py`.

Vita3K is useful for correctness and crash reproduction only; never quote
emulator numbers as performance. Its paths are in `INSTALL.md`.

## Legal

The project's own code is GPL-2.0-or-later; third-party parts keep their own
terms. See [`DISTRIBUTION.md`](../../DISTRIBUTION.md),
[`NOTICE.md`](../../NOTICE.md), [`THIRD_PARTY.md`](../../THIRD_PARTY.md), [`LICENSE`](../../LICENSE).
