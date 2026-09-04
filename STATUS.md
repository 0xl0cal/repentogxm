# Status

Evidence date: 2026-09-04. Every number below names the build it was measured
on and the date. Builds are named by their log id; `perf:wf-opt-v13` is
written `opt-v13`. `opt-N` is a production-style build. `prof-N` is a
profiling build: the production options plus a sampler and timing counters.
`opt-v11f` is opt-v11 with 32-bit hardware float loads on
(`ISAAC_VITA_TRANSLATED_CPU_F32_VFP`). Items marked UNVERIFIED have no device
evidence yet.

## Words used below

- guest: the translated x86 game code. host: the native Vita runtime around it.
  KAGE: the game engine's own platform and rendering layer. corpus: the C code
  generated from the game executable (15,116 functions).
- seam: a native replacement for one translated function. Every seam checks
  its inputs and falls back to the translated code when anything is unexpected.
  VERIFY: a seam mode that runs both the native and the translated code and
  compares the results; a mismatch turns the seam off.
- oracle: a host-side test comparing native and translated results.
  receipt: a log line that proves a mechanism ran on the device.
- UPS and FPS: the game simulates 30 times per second (UPS); the port redraws
  the screen up to 60 times per second (FPS).
- EID: External Item Descriptions, a widely used Lua mod. It is the
  mod-compatibility benchmark and was loaded in every session of the
  performance table below.

## Current build

```text
build id:  perf:wf-opt-v13
source:    branch wf/flags-local, commit 594a10b (2026-09-04 11:43 +0300)
eboot.bin: 13,322,070 bytes
sha256:    1440b4e4e486603a5e6634b4bd9363838b497497539a395cf0f5d37179ab3e07
built:     2026-09-04 12:06, deployed to the test Vita 12:19
```

What this build runs (option names are in `recomp/vita/CMakeLists.txt`):

- Display 960x544 (`ISAAC_VITA_DISPLAY_RASTER=960`). The world is drawn on
  the game's own 480x270 render surface, as on PC, and upscaled once; the
  game's HQX upscaling filter and colour-correction passes are bypassed
  (`ISAAC_VITA_RENDER_SURFACE_NATIVE`); the picture is sharper than the PC
  default. Lower display resolutions exist as build options only.
- 60 Hz presentation around the 30 Hz game update
  (`ISAAC_VITA_FULLSPEED_SCHEDULER`, `ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES`).
- Audio on: OpenAL Soft 1.19.1, mixer pinned to CPU 2 (`ISAAC_VITA_AUDIO`,
  `ISAAC_VITA_AUDIO_WORKER`); music decoded by a native stb_vorbis 1.04 worker
  on CPU 2 (`ISAAC_VITA_NATIVE_VORBIS`, `_WORKER`, `_ASYNC`). Game threads: cores 0, 1.
- Lua on: pinned Lua 5.3.3 bridge (`ISAAC_VITA_LUA`); GC step clamped to
  128 KB per frame and the per-floor full GC to one 256 KB step
  (`ISAAC_VITA_LUA_GCSTEP_CLAMP_KB`, `ISAAC_VITA_LUA_GCCOLLECT_CLAMP`); typed
  import endpoints (`ISAAC_VITA_LUA_IMPORT_FASTDISPATCH`); native `__index` and
  `getClass` seams with VERIFY on (`ISAAC_VITA_LUA_NATIVE_INDEX`, `_GETCLASS`).
- Translated-code generation: `ISAAC_VITA_TRANSLATED_CPU` (x86 flags and
  general registers in C locals, tiny-leaf inlining, SSE to NEON lowering),
  `ISAAC_VITA_LAYOUT_HUB` (0 long-branch veneers), `ISAAC_VITA_DISPATCH_TABLE`,
  `ISAAC_VITA_IAT_DIRECT` (direct calls through the import table, IAT),
  `ISAAC_VITA_SYNC_INLINE_FASTPATH`.
- Native seams: PNG decode with a 16 MiB reserve (`ISAAC_VITA_NATIVE_PNG`),
  zlib inflate (`ISAAC_VITA_NATIVE_INFLATE`), a file-size cache that answers
  the game's redundant seek/tell calls (`ISAAC_VITA_CRT_SEEK_SHADOW`), KAGE
  mutex lock/unlock (`ISAAC_VITA_KAGE_MUTEX_SEAM`), the `floor()` import
  executed inline (`ISAAC_VITA_FLOOR_THUNK_FASTPATH`), host-side replay of the
  shader attribute enable/disable calls (`ISAAC_VITA_SHADER_ATTRIB_FASTPATH`),
  and GL calls routed through the dispatch table with plain argument loads
  (`ISAAC_VITA_GL_SHIM_TABLE_TOKENS`, `ISAAC_VITA_GL_SHIM_RAW_ARGS`).
- Asynchronous logger (`ISAAC_VITA_LOG_ASYNC`) and save writer
  (`ISAAC_VITA_ASYNC_SAVE_WRITE`); 81 MiB guest heap (`ISAAC_VITA_HEAP_MB`);
  32 MiB kept outside the vitaGL pool (`ISAAC_VITA_VITAGL_RAM_RESERVE_MB`);
  phase profiler on (`ISAAC_VITA_PHASE_PROFILE`).

Build reproducibility gap: `tools/build_vita.py` exposes none of the
translated-CPU, dispatch, IAT, layout, native Vorbis, PNG, inflate, async-log,
Lua-seam or GL-shim options; it builds a slower baseline VPK with Lua off by
default. Production builds pass `-D` options straight to CMake from a script
outside the repo; the list above is reconstructed from build logs (UNVERIFIED
in-repo). Three options need a corpus regenerated with `GUEST_LEAF_INLINE=1
GUEST_SSE_LOWER=1 GUEST_FLOOR_THUNK_FASTPATH=1` set.

## How the numbers are measured

Every device build prints one block of timing records (`ph120.*`) to the kernel
debug log every 120 frames. They are captured over the network with a debug-log
plugin, filtered by build id (`ISAAC_VITA_GUEST_LINK_ID`) and read with developer
scripts outside this repo. The summary script prints "60.00 FPS" for any window
with 120 presented frames, so it hides hitches and load stalls. "Real FPS" is 120
frames divided by the window wall time; "render p50" is the median render phase
per frame (16.7 ms is the 60 FPS budget); "loop" is the whole frame (update,
render and waiting); p95 is the 95th percentile.

## Measured performance

| Scene | Metric | Value | Build | Date |
|---|---|---|---|---|
| Menu | render p50 | 10.2 ms | opt-v12 | 2026-09-04 |
| Menu | real FPS p50 | 59.6 | opt-v11 | 2026-09-04 |
| Light rooms | render p50 | 7.8 ms | opt-v12 | 2026-09-04 |
| Light rooms | real FPS p50 | 42.4 (opt-v11), 51.1 (opt-v5) | opt-v11, opt-v5 | 2026-09-04 |
| Mid rooms | render p50 | 11.5 ms | opt-v12 | 2026-09-04 |
| Mid rooms | real FPS p50 | 48.8 (opt-v11), 45.9 (opt-v5) | opt-v11, opt-v5 | 2026-09-04 |
| Dense rooms (many entities) | render p50 / p95 | 22.3 / 35.6 ms, real FPS 36 | opt-v12 | 2026-09-04 |
| Dense room, no lasers (owner's) | FPS | 35-37, render p50 20.5 ms | opt-v11f | 2026-09-04 |
| Boss fight with Brimstone | loop p50 | 18-21 ms (about 50 FPS) | opt-v11 | 2026-09-04 |
| Circle of Protection (laser ring) | render p50 | 22-26 ms, locks to 30 FPS | prof-v11 | 2026-09-04 |
| Two Brimstone monsters + Circle of Protection + Azazel | FPS | 6-7 (frame 133 ms, render 123 ms) | opt-v13 | 2026-09-04 |
| Game update rate | UPS | 30.00 (opt-v1), 29.85 (opt-v5) | opt-v1, opt-v5 | 2026-09-03/04 |
| Room entry | update stall | 0.9-1.4 s (opt-v11), 1.06-1.72 s (opt-v12; 13 in the session) | opt-v11, opt-v12 | 2026-09-04 |
| Floor change | update stall | 1.5-4.3 s | opt-v11 | 2026-09-04 |
| Run start, Continue | update stall | 5.3-5.5 s | opt-v11, opt-v12 | 2026-09-04 |
| Boot to title screen | time | 26 s | gpr-v3 | 2026-09-03 |
| Session stability | faults | FAULT 0, EID ERROR 0 over 540 + 343 windows | opt-v11, opt-v12 | 2026-09-04 |

Summary of the opt-v5 session (99 game windows): 94-100 % of room windows had
a frame over 33 ms, mid rooms had a loop p95 over 20 ms in 98 % of windows, 19
room changes stalled 0.8-2.6 s. The light-room real FPS values come from
different sessions and room mixes; the drop from opt-v5 to opt-v11 is
unexplained. opt-v13 has only the worst-room number.

## Known problems

1. Dense rooms run at 35-37 FPS. Cause (prof-v11, opt-v11f, 2026-09-04):
   per-object reference counting under a KAGE mutex; 7,790 lock/unlock pairs
   per frame versus 700 in a normal room, 35,692 translated calls per frame
   versus 10,867; draws, GL and Lua work stay flat. Fix: native seam, being
   implemented (design reviewed 2026-09-04), not on device.
2. Laser-heavy rooms. Circle of Protection alone locks the game to 30 FPS
   (render 22-26 ms). One profile (prof-v11) found the CPU busy in render; a
   later one (prof-v12) attributed the same 30 FPS cadence to the CPU waiting
   for the GPU at vitaGL clears, so the cause is under investigation. With two
   Brimstone monsters plus Circle of Protection plus Azazel's Brimstone a
   frame takes 133 ms: the CPU waits for the GPU inside vitaGL `glClear` and
   at the end of each GXM scene (GXM is the Vita graphics API): 72 ms per
   frame in 7 clears, where a normal room spends 0.48 ms. Two experiments on
   2026-09-04 (8 GXM scenes per display frame instead of 1; skipping
   redundant clears of offscreen buffers, `ISAAC_VITA_FBO_CLEAR_ELISION`) did
   not remove the bursts; the second saved about 2.2 ms per frame. Combined
   with framebuffer down-scaling that option lost shading layers on device in
   an earlier build, so it stays off. Neither is merged. Design review
   (2026-09-04 13:35): 99.8 % of the clear time is `sceGxmEndScene` inside
   vitaGL's scene switch; all offscreen passes of a frame use one render
   target created with one scene slot, so pass k cannot end before pass k-1
   finishes on the GPU. Fix being implemented: give that render target 8
   slots (vitaGL patch, build option) plus a per-target scene-end timer to
   confirm; not on device. The worst room also spends more than 100 ms per
   frame of GPU fill on the laser layers, which needs a separate fix.
3. Load stalls at room entries (0.9-1.7 s on opt-v11/v12, 0.8-2.6 s on
   opt-v5; the report calls them unchanged from opt-v5 to opt-v12), floor
   changes (5.6-5.7 s on opt-v5/v6, 1.5-4.3 s on opt-v11; no controlled A/B
   between them) and run start or Continue (5.3-5.5 s). Attribution (prof-v10):
   PNG sprite-sheet reload plus archive reads 45-50 % of room entry, a full
   Lua GC at level init 10 %. The per-floor GC clamp is in opt-v13 but
   unmeasured; sprite retention across rooms is implemented on a branch, not
   merged and not measured. The seek cache (`ISAAC_VITA_CRT_SEEK_SHADOW`) is
   active on opt-v11; its gain is UNVERIFIED.
4. Guest heap exhaustion on "Exit game" after a 27-minute session (prof-v11,
   2026-09-04): a 307,200-byte music-stream allocation failed with 84,425,616
   bytes allocated of the 81 MiB heap and 504,944 bytes free in 3,294 chunks.
   The game throws `std::bad_alloc`; C++ exceptions are not supported by the
   recompiled code, so the process stops in a controlled way (result 1) after
   the saves were written (rc=0). Heap use grows with play time. Fix in progress.
5. EID description box open: Lua costs 8-10 ms per frame and breaks 60 FPS
   (2.7 ms per frame with the box closed). Not fixed.
6. `KAGE VITA AUDIO REPLAY` (music source ran dry) appears only inside load
   stalls over 1 s, 10 times in a 20-minute opt-v11 session; the decoder logs
   `underruns=0 fallbacks=0`. Whether it is audible is UNVERIFIED.
7. The native `__index` seam with VERIFY on logs one mismatch on the `Vector`
   `Position` getter and then disables itself silently. The mismatch is a false
   alarm by construction (the userdata holds a self-relative pointer); the
   comparator and the silent shutdown are defects. No visible effect: the seam
   turns itself off and its speed gain is lost. `getClass` VERIFY: 110k
   checks, 0 mismatches (prof-v12).
8. Saves over 64 KiB: a 2026-09-03 note says the async save read path hung at
   boot on such a file; the owner's save is 56,352 bytes, so the path was not
   exercised since. Whether the hang still exists is UNVERIFIED. Keep a backup.
9. Exit-to-menu latency has not been measured since 2026-08-31.
10. Game rule, not a port bug: a run saved without a mod cannot be continued
    after enabling one (`Cannot continue a game that doesn't have the same
    modding state` in the game's `log.txt`). Lua-off builds crash at startup
    when a mod is enabled.
11. The on-device Manager (mod on/off, save backup and restore, FTP) has never
    been used on hardware. Its FTP has no authentication; any LAN device can
    modify `ux0:/data/isaacr001` while it runs.

## Not tested

No 2026-09 evidence either way: Utero II world rendering (an earlier black world
report predates the colour-correction bypass), suspend and resume, PS button
behaviour, PS TV, front-touch zones on current builds, language switching, update
and rollback, sessions longer than 27 minutes, full-run beatability. No remapping exists.

## What is in the tree

Most options are off by default; production builds turn them on.

- Translated-code generation (`ISAAC_VITA_TRANSLATED_CPU`, `_LAYOUT_HUB`,
  `_DISPATCH_TABLE`, `_IAT_DIRECT`); `.text` 22.28 MB after SSE lowering.
  Tests: `recomp/test_guest_dispatch_table.py`, `recomp/vita/test_import_id_dispatch.sh`.
  Device: opt-v1 cut menu render 14.9 to 12.3 ms (2026-09-03).
- Audio: OpenAL Soft 1.19.1 statically linked, mixer on CPU 2. Native
  stb_vorbis 1.04 replaces the translated decoder; a CPU 2 worker decodes
  ahead. Tests: `recomp/vita/test_native_vorbis.sh`,
  `recomp/vita/test_audio_imports.sh`. Device (perf:wf-native-vorbis-v1, 2026-09-03):
  UPS 30.00, 24/24 VERIFY slots MATCH, audio service p95 3-4 ms (was 80-180 ms).
- Native PNG decode (`ISAAC_VITA_NATIVE_PNG`): replaces `png_read_row`, serving
  rows from a 16 MiB block reserved before vitaGL starts.
  Tests: `recomp/vita/test_native_png.sh`, `recomp/test_vita_native_png.py`
  (210 PNGs byte-identical to a reference). Device: the first run fell back to
  the translated decoder on the large 1024x1024 sheets (2026-09-03); fixed by
  the reserve (5fbf53f, 0dbbb2a).
- Native inflate (`ISAAC_VITA_NATIVE_INFLATE`): inert on opt-v6, fixed on
  opt-v7 (`STATS calls=1 native=1 fallbacks=0`, 2026-09-04); did not change
  load stalls by itself. Seek cache (`ISAAC_VITA_CRT_SEEK_SHADOW`: answers the
  game's file-size seek/tell pairs from a cached size;
  `recomp/vita/test_crt_seek_shadow.sh`): receipt `elided=528 apply_fail=0` on
  opt-v11, gain UNVERIFIED.
- Asynchronous logger (`ISAAC_VITA_LOG_ASYNC`, `recomp/runtime/host_vita_log_async.c`,
  `recomp/vita/test_log_async.sh`): a 256 KiB ring drained by a low-priority
  thread on CPU 2. Device (2026-09-04): `KAGE VITA LOG ASYNC: started=1 rc=0`,
  no drops; a 60-100 ms stall every 2 s is gone; menu real FPS 58.0 (opt-v7)
  to 59.2-59.6 (opt-v11, which also carried three other seams).
- Asynchronous save writer (`ISAAC_VITA_ASYNC_SAVE_WRITE`,
  `recomp/vita/test_async_write.sh`): on since gpr-v3 (2026-09-03); writes
  returned rc=0 before the Exit crash. See problem 8.
- Lua bridge (`ISAAC_VITA_LUA`, pinned vanilla 5.3.3 from a user-supplied
  source tree): GC clamps, typed import endpoints (`recomp/vita/test_lua_import_fastdispatch.sh`),
  native `__index` and `getClass` seams (`recomp/runtime/host_vita_lua.c`,
  `host_vita_lua_getclass.c`). Device: EID loads (95 scripts, 19 MB) and draws
  descriptions since perf:wf-flags-v2 (2026-09-03). Test mod: `tools/isaac_vita_lua_sentinel/`.
- KAGE and GL seams: mutex lock/unlock (`recomp/test_vita_kage_mutex_seam.py`),
  inline `floor()` import (`recomp/test_vita_floor_thunk_fastpath.py`),
  host-side replay of shader attribute enable/disable
  (`recomp/test_vita_shader_attrib_fastpath.py`), GL calls dispatched straight
  to the GL adapter (`recomp/vita/test_gl_shim_fastdispatch.sh`). Device:
  opt-v11/v12 receipts,
  FAULT 0; menu render 10.7 to 10.2 ms, light rooms 8.6 to 7.8 ms (2026-09-04).
- Heap: 81 MiB newlib heap below the fixed image; an ownership ledger tracks
  which allocator owns every block (`ISAAC_VITA_HEAP_LEDGER_*`, on by default).
  Tests: `recomp/vita/test_heap_*.sh`. See problem 4.
- On-device Manager (`recomp/vita/manager_storage.c`, `manager_ftp_server.c`):
  a second button on the game's LiveArea page; mod on/off through the game's
  `disable.it` marker,
  SHA-256-verified save backup and restore, passive-mode FTP on port 1337
  rooted at `ux0:/data/isaacr001`, no authentication. Never used on hardware.
- PC sync tools (`tools/isaac_vita_sync.py`, `tools/isaac_vita_sync_gui.py`):
  `plan | push | pull` for saves and Workshop mods over that FTP server; push
  never deletes user content; pull downloads the three save slots into a new
  directory. Tests: `tools/test_isaac_vita_sync.py` (fake FTP). Use against
  the Manager on hardware is UNVERIFIED.

## Safety and provenance

- The translated image is fixed at `0x98000000`, built ARM EABI5 softfp with
  Thumb-2 generated units; the executable is built with address randomization
  off (NOASLR), so kubridge is required to map it.
- Every native fast path checks the exact caller, target and layout it was
  written for; anything else runs the original translated code.
- Every Windows API the game imports goes through one validated 413-entry
  table (the count is checked in `recomp/runtime/host_vita_import_id.c`).
- Generated guest C, the original executable, resources, Sony modules, VPK and
  saves stay outside the tracked source tree.
- Source is on GitHub and the VPK on its Releases page; game resources are
  never published. The project's own code is GPL-2.0-or-later. See
  `DISTRIBUTION.md`.

## Tests

At 594a10b: 87 `recomp/test_*.py`, 66 `recomp/vita/test_*.sh`, 8 `tools/test_*.py`
(the 8 tools files: 59 cases, all pass, run 2026-09-04 at 594a10b).
Two failures were pre-existing at 2cd6417 (2026-09-04): `python tools/build_vita.py
--self-test` (miniz_native count assert) and `recomp/vita/test_kage_vita_io_profile.sh`
(host shadow compile); whether they pass at 594a10b is UNVERIFIED.

## History

| Date | Build | Change | Headline |
|---|---|---|---|
| 2026-08-31 | perf:bundle19-stablefps-lua-v1 (12,423,488 B, sha256 5e419f5d...) | audio + first Lua bridge on device, original frame limiter | 59.83 FPS average over 8,040 presents after transitions; transitions 9-40 FPS; EID did not start |
| 2026-09-02 | perf:bundle35 (720x408, audio off, Lua off) | 60 Hz wrapper, phase profiler | start room 60 FPS at 7.0 ms; pause menu 45 FPS; with audio on UPS fell to 20.5 |
| 2026-09-03 | wf/integration-v3 b19c535, perf:wf-native-vorbis-v1 | 480x270 render surface at 960x544, native Vorbis on CPU 2, Lua on, translated-CPU, exit fix | pause menu 60 FPS at 7.4-7.7 ms; UPS 30.00; audio service p95 3-4 ms |
| 2026-09-03 | perf:wf-flags-v1 to v4 | Lua vararg formatter (v1), 80 extra code-pointer roots (v2, v3), GC step clamp 128 KB (v4) | EID runs and draws descriptions; 14 FPS before the clamp |
| 2026-09-03 | perf:wf-flags-v5-png | native PNG first device run | sheets over 1 MiB fell back; fixed by the 16 MiB reserve |
| 2026-09-03 | perf:wf-gpr-v1 | general registers in locals (audio off) | 60 FPS in every game window of the owner's session |
| 2026-09-03 | perf:wf-gpr-v3 (0dbbb2a) | everything on | boot to title 26 s; title render 14.5 ms; start room 11.8 ms p50 |
| 2026-09-03 | perf:wf-opt-v1 (69a7492) | layout hub, dispatch table, sync inline, IAT direct | menu 14.9 to 12.3 ms; UPS 30.00 |
| 2026-09-04 | perf:wf-opt-v5 (95e9ddb) | leaf inline, SSE to NEON, GL shim fast dispatch | render p50 menu 11.2, light 9.4, mid 11.5, heavy 16.7 ms; real FPS light 51.1, mid 45.9, menu 57.9 |
| 2026-09-04 | perf:wf-opt-v7 | native inflate (v6 inert, v7 fixed) | load stalls unchanged |
| 2026-09-04 | perf:wf-opt-v8, v9 | Vorbis partial-slot async, CRT seek shadow | receipts confirmed on opt-v11 |
| 2026-09-04 | perf:wf-opt-v10 (007f0ab) | asynchronous logger | built, not run on device; the logger was first measured on opt-v11 (menu real FPS 58.0 to 59.2) |
| 2026-09-04 | perf:wf-opt-v11 | KAGE mutex seam, floor thunk fast path, typed Lua endpoints | menu real FPS 59.6; light 42.4, mid 48.8; boss with Brimstone about 50 FPS |
| 2026-09-04 | perf:wf-opt-v12 (c0e0292) | shader attribute replay, GL tokens, raw args | render p50 menu 10.2, light 7.8, mid 11.5, heavy 22.3 ms |
| 2026-09-04 | perf:wf-opt-v13 (594a10b) | per-floor GC clamp, native `__index` and `getClass` seams | worst room 6-7 FPS; no other numbers yet |

The order of work is in [PLAN.md](PLAN.md).
