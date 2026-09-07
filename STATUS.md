# Status

Numbers come from one PS Vita at 960x544 with the External Item Descriptions
(EID) mod loaded. Every number names the build it was measured on. Release
builds carry no profiler; their evidence is the fault log, the seam receipts
and the play session. A "twin" is the release source rebuilt with the phase
profiler, the sampler and sparse GL timers on. Older builds are named by their
log id (`perf:wf-opt-v13` is written `opt-v13`; `prof-N` was the profiling
build of `opt-N`).

## Words

- guest: the translated x86 game code. host: the native Vita runtime around it.
  KAGE: the game engine's platform and rendering layer. corpus: the C code
  generated from the game executable (15,116 functions).
- seam: a native replacement for one translated function. Every seam checks
  its inputs and falls back to the translated code when anything is unexpected.
  VERIFY: a seam mode that runs both the native and the translated code and
  compares the results; a mismatch turns the seam off.
- oracle: a host-side test comparing native and translated results.
  receipt: a log line that proves a mechanism ran on the device.
- UPS and FPS: the game simulates 30 times per second (UPS); the port redraws
  the screen up to 60 times per second (FPS).

## Current build

```text
build id:  rel:v0.1.1-alpha-rc3
source:    the v0.1.1-alpha snapshot, configured from release/v0.1.1-alpha.cmake
           in a fresh build directory
eboot.bin: 13,360,635 bytes
sha256:    87802d3fd987fc680472584b065a95fa4cc5b1953d54e611c7a4229fcb9e4886
VPK:       17,529,342 bytes
sha256:    3a1d6399d47938c591d63c4bed1ec296d46d6455944300720861605fbabeb927
           (members identical to v0.1.0-alpha except eboot.bin)
sessions:  2026-09-07: rc3 (rc2 plus the file-handle recovery) 4.9 min:
           Continue into a Utero run, four rooms, saves written, no fault,
           every native seam at fallbacks=0, no handle loss (enodev=0);
           earlier the same day at the main menu with music, PS button to
           the home screen and back: both music streams recovered their
           handles, no fault. rc2 about 2.5 min of play and
           rc1 (quad fast path for batched quads only) 4.5 min including a
           save in a dense room: no fault record, every native seam at
           fallbacks=0, picture correct
```

The complete option set is `release/v0.1.1-alpha.cmake` (option names are
defined in `recomp/vita/CMakeLists.txt`). In groups:

- Display 960x544 (`ISAAC_VITA_DISPLAY_RASTER=960`). The world is drawn on the
  game's own 480x270 render surface, as on PC, and upscaled once; the game's
  HQX filter and colour-correction passes are bypassed
  (`ISAAC_VITA_RENDER_SURFACE_NATIVE`). Lower display resolutions exist as
  build options only.
- 60 Hz presentation around the 30 Hz game update
  (`ISAAC_VITA_FULLSPEED_SCHEDULER`, `ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES`).
- Audio: OpenAL Soft 1.19.1 with the mixer on CPU 2 (`ISAAC_VITA_AUDIO`,
  `_WORKER`); music decoded by a native stb_vorbis 1.04 worker on CPU 2
  (`ISAAC_VITA_NATIVE_VORBIS`, `_WORKER`, `_ASYNC`). Game threads on cores 0, 1.
- Lua: pinned Lua 5.3.3 bridge (`ISAAC_VITA_LUA`); GC step clamped to 128 KB
  per frame and the per-floor full GC to one 256 KB step
  (`ISAAC_VITA_LUA_GCSTEP_CLAMP_KB`, `ISAAC_VITA_LUA_GCCOLLECT_CLAMP`); typed
  import endpoints (`ISAAC_VITA_LUA_IMPORT_FASTDISPATCH`); native `__index`
  and `getClass` seams (`ISAAC_VITA_LUA_NATIVE_INDEX`, `_GETCLASS`).
- Translated code: `ISAAC_VITA_TRANSLATED_CPU` (x86 flags and registers in C
  locals, tiny-leaf inlining, SSE to NEON lowering), `ISAAC_VITA_LAYOUT_HUB`,
  `ISAAC_VITA_DISPATCH_TABLE`, `ISAAC_VITA_IAT_DIRECT`,
  `ISAAC_VITA_SYNC_INLINE_FASTPATH`.
- Native seams: PNG decode with a 16 MiB reserve (`ISAAC_VITA_NATIVE_PNG`) and
  its exact fast paths (NEON premultiply, row batching, bounded reuse, strict
  inflate; `ISAAC_VITA_NATIVE_PNG_REUSE_LARGE` OFF), zlib inflate
  (`ISAAC_VITA_NATIVE_INFLATE`), the file-size cache for the game's redundant
  seek/tell calls (`ISAAC_VITA_CRT_SEEK_SHADOW`), recovery of file handles the
  kernel invalidates across an app suspend
  (`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER`), KAGE mutex lock/unlock
  (`ISAAC_VITA_KAGE_MUTEX_SEAM`) and its reference-count helpers
  (`ISAAC_VITA_KAGE_REFCOUNT_SEAM`), the inline `floor()` import
  (`ISAAC_VITA_FLOOR_THUNK_FASTPATH`), host replay of the shader attribute
  enable/disable calls (`ISAAC_VITA_SHADER_ATTRIB_FASTPATH`) and of
  `Image::PushQuad` for every quad (`ISAAC_VITA_KAGE_QUAD_FASTPATH`), GL calls
  through the dispatch table with plain argument loads
  (`ISAAC_VITA_GL_SHIM_TABLE_TOKENS`, `ISAAC_VITA_GL_SHIM_RAW_ARGS`).
- Renderer: the stock vitaGL profile (`ISAAC_VITA_VITAGL_STOCK_REFERENCE`),
  the neutral ColorOffset fragment path
  (`ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH`), no sampled/ring laser shadow
  pass (`ISAAC_VITA_LASER_RING_SHADOW_SKIP`), 8 GXM scene slots for the
  offscreen render target (`ISAAC_VITA_STOCK_FBO_RT_SCENES`).
- Heap: 81 MiB guest heap (`ISAAC_VITA_HEAP_MB`), 32 MiB kept outside the
  vitaGL pool (`ISAAC_VITA_VITAGL_RAM_RESERVE_MB`), a one-slot emergency
  allocation for the music stream queue (`ISAAC_VITA_OGG_QUEUE_EMERGENCY`),
  lock-free read of the allocator's terminal state
  (`ISAAC_VITA_HEAP_TERMINAL_FASTPATH`).
- Asynchronous logger (`ISAAC_VITA_LOG_ASYNC`) and save writer
  (`ISAAC_VITA_ASYNC_SAVE_WRITE`). Every profiling and observer option OFF.

`tools/build_vita.py` exposes none of the translated-CPU, dispatch, native
decoder, async-log, Lua-seam or GL-shim options and builds a slower baseline
with Lua off. The release build is configured directly with CMake from the
file above; options that change the generated code (leaf inlining, the floor
thunk fast path) need the corpus generated with the matching `GUEST_*`
switches, as the file's header says.

## How the numbers are measured

A profiling build prints one block of timing records to the kernel debug log
every 120 frames. Each 2-second window is classified by its draw calls per
presented frame into menu, rooms with enemies (about 65-70 draws) and dense
rooms (about 78 draws). FPS is presented frames per 120 possible, because a
window with 120 presents prints "60 FPS" even when frames took 30-70 ms.
"Render median" is the median render phase per frame (16.7 ms is the 60 FPS
budget); p95 the 95th percentile.

## Measured performance

Twins of the release source, EID loaded, game at 29.3-30.0 updates per second.
The rc1 twin had the quad fast path for batched quads only; the rc2 twin
(246 windows, 46 in rooms) has it for every quad. Both 2026-09-06/07.

| Scene | Metric | Value | Build |
|---|---|---|---|
| Menus | FPS; render per frame | 60; 10.0-10.3 ms | rc1 and rc2 twins |
| Rooms with enemies | render median; p95 | 9.9-10.3 ms; 10.9-11.4 ms | rc2 twin |
| Rooms with enemies | presents per 120 possible | 116-118 median (55-59 FPS); 56.8 FPS over the 46 room windows | rc2 twin |
| Rooms with enemies | render median; FPS | 11.7-13.1 ms; 48-52 | rc1 twin |
| Quad fast path | quads replayed natively | 100 % (1,161,348 of 1,161,348 in room windows, no declines) | rc2 twin |
| Dense rooms | render median; FPS | 15.4-15.9 ms; 33-34 | rc1 twin (no dense room in the rc2 session) |
| Whole play windows | FPS | 45.7-50.8 depending on the rooms | rc1 twin |
| Door transition with resource loads | wall time | 0.9-1.1 s; ordinary revisits far less | batch deep-profile build |
| Continue from the main menu | time | about 6.9 s | batch deep-profile build |
| Process start to the main menu | time | about 4 s | batch deep-profile build |
| Stability | faults | none in 4.9 min (rc3), 2.5 min (rc2) and 4.5 min (rc1); fallbacks=0 on every seam | rc1, rc2, rc3 |
| Stability | PS button to the home screen and back at the menu | both music-stream handles recovered, no fault | rc3 |

Where the room render time goes per frame (rc1 twin): translated game code
about 5.7 ms, of which sprite quad building 1.0-1.3 ms before the quad fast
path; native code called from it about 4.3 ms, of which vitaGL draw bodies
about 1.0 ms (14-27 us per draw, `sceGxmDraw` itself 3.3-3.9 us), scene
begin/end and clears 0.5-0.7 ms (4 scenes per frame), Lua callbacks about
0.8 ms, sprite attribute state about 0.7 ms. The quad fast path handled
55-57 % of room quads on the rc1 twin and 100 % on the rc2 twin; the room
render median fell from 11.7-13.1 ms to 9.9-10.3 ms. Menu quads are 98.5 %
culled before any vertex is built, so the menu's 10.2 ms are spent elsewhere.

A door transition runs the room state reset (`00520160`, 73 ms per call,
53 ms of it the function's own work) and the room snapshot (`00314fc0`,
40 ms), sometimes both on two consecutive frames, plus the resource loads.

## Known problems

1. Rooms below 60 FPS: rooms with enemies 55-59 FPS, dense rooms 33-34 FPS.
   Cause: the CPU render phase, 9.9-15.9 ms median per frame, split as above.
2. Load stalls: door transitions with resource loads 0.9-1.1 s, Continue about
   6.9 s. On the 2026-09-04 builds 45-50 % of a room entry was PNG
   sprite-sheet reload plus archive reads and 10 % a full Lua GC at level
   init; the room state reset's own 53 ms is the next target.
3. Long sessions may exhaust the game heap. Seen once (prof-v11, 27 minutes):
   a 307,200-byte music-stream allocation on "Exit game" failed with
   84,425,616 bytes allocated of the 81 MiB heap and 504,944 bytes free in
   3,294 chunks; the game threw `std::bad_alloc`, which the recompiled code
   cannot handle, so the process stopped in a controlled way after the saves
   were written. `ISAAC_VITA_OGG_QUEUE_EMERGENCY` admits that one allocation
   to a reserve slot; it is not a general fragmentation cure.
4. Laser-heavy rooms (opt-v13 and prof-v11, 2026-09-04): Circle of Protection
   alone locked the game to 30 FPS (render 22-26 ms); two Brimstone monsters
   plus Circle of Protection plus Azazel's Brimstone gave 6-7 FPS: 72 ms per
   frame of CPU waiting for the GPU at the end of each GXM scene, because all
   offscreen passes of a frame shared one render target with one scene slot,
   plus more than 100 ms per frame of GPU fill on the laser layers. The
   8-slot render target and the laser shadow skip ship since the 2026-09-05
   batch; no laser room has been measured on them. Skipping redundant clears
   of offscreen buffers (`ISAAC_VITA_FBO_CLEAR_ELISION`) saved about 2.2 ms
   per frame but lost shading layers together with framebuffer down-scaling,
   so it stays off.
5. An open EID description box costs 8-10 ms of Lua per frame (2.7 ms closed,
   2026-09-04 profiles) and breaks 60 FPS. With the box closed Lua callbacks
   cost about 0.8 ms per frame (rc1 twin).
6. The native `__index` seam with VERIFY on logs one false mismatch on the
   `Vector` `Position` getter (the userdata holds a self-relative pointer)
   and then disables itself silently; the seam's gain is lost, nothing
   visible happens. `getClass` VERIFY: 110k checks, 0 mismatches.
7. Saves over 64 KiB: the async save read path hung at boot once on such a
   file (2026-09-03); not reproduced since. Keep a backup.
8. `KAGE VITA AUDIO REPLAY` (music source ran dry) appears only inside load
   stalls over 1 s; the decoder logs `underruns=0 fallbacks=0`.
9. Game rule, not a port bug: a run saved without a mod cannot be continued
    after enabling one (`Cannot continue a game that doesn't have the same
    modding state` in the game's `log.txt`). Lua-off builds crash at startup
    when a mod is enabled.
10. Untested on hardware: the on-device Save / Mod Manager (mod on/off, save
    backup and restore, FTP) and the PC sync tool; PS TV; language switching;
    front-touch zones; update and rollback; sessions longer than 27 minutes; a
    full run to an ending; standby (power button) with a run open. The
    Manager's FTP has no authentication; any LAN device can modify
    `ux0:/data/isaacr001` while it runs. No remapping exists.

Fixed in v0.1.1-alpha: the crash `ArchivedFile block header is invalid` after
a PS-button press or a console lock. The Vita kernel invalidates the
process's open `ux0:` file handles across an app suspend (the behaviour the
FdFix plugin exists for); the next read of a music stream from
`repentance.a` failed with errno 19 and the archive reader stopped the game.
All five occurrences on record followed such an interruption. The CRT now
retains the path of every read-only handle and, on that error, reopens it at
the same cursor and redoes the read (`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER`, one
`descriptor-recover-v1` receipt per recovery); verified on the rc3 build with
the PS button at the main menu.

## What is in the tree

Most options are off by default; the release configuration turns them on.

- Translated-code generation (`ISAAC_VITA_TRANSLATED_CPU`, `_LAYOUT_HUB`,
  `_DISPATCH_TABLE`, `_IAT_DIRECT`); `.text` 22.28 MB after SSE lowering.
  Tests: `recomp/test_guest_dispatch_table.py`, `recomp/vita/test_import_id_dispatch.sh`.
- Audio: OpenAL Soft 1.19.1 statically linked, mixer on CPU 2; native
  stb_vorbis 1.04 replaces the translated decoder, a CPU 2 worker decodes
  ahead. Tests: `recomp/vita/test_native_vorbis.sh`, `recomp/vita/test_audio_imports.sh`.
- Native PNG decode (`ISAAC_VITA_NATIVE_PNG`): replaces `png_read_row`, serving
  rows from a 16 MiB block reserved before vitaGL starts; the exact fast paths
  (`ISAAC_VITA_NATIVE_PNG_*`, `ISAAC_VITA_PNG_PREMULTIPLY_*`) each keep the
  original fallback. Tests: `recomp/vita/test_native_png.sh`,
  `recomp/test_vita_native_png.py` (210 PNGs byte-identical to a reference),
  `recomp/test_vita_native_png_*.py`.
- Native inflate (`ISAAC_VITA_NATIVE_INFLATE`), the seek cache
  (`ISAAC_VITA_CRT_SEEK_SHADOW`) and the file-handle recovery
  (`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER`); tests
  `recomp/vita/test_crt_seek_shadow.sh`, `recomp/vita/test_crt_raw_archive.sh`,
  `recomp/vita/test_crt_imports.sh`.
- Asynchronous logger (`ISAAC_VITA_LOG_ASYNC`, `recomp/runtime/host_vita_log_async.c`,
  `recomp/vita/test_log_async.sh`): a 256 KiB ring drained by a low-priority
  thread on CPU 2; removed a 60-100 ms stall every 2 s.
- Asynchronous save writer (`ISAAC_VITA_ASYNC_SAVE_WRITE`, `recomp/vita/test_async_write.sh`).
- Lua bridge (`ISAAC_VITA_LUA`, pinned vanilla 5.3.3 from a user-supplied
  source tree): GC clamps, typed import endpoints
  (`recomp/vita/test_lua_import_fastdispatch.sh`), native `__index` and
  `getClass` seams (`recomp/runtime/host_vita_lua.c`, `host_vita_lua_getclass.c`).
  EID loads (95 scripts, 19 MB) and draws descriptions. Test mod:
  `tools/isaac_vita_lua_sentinel/`.
- KAGE and GL seams: mutex lock/unlock (`recomp/test_vita_kage_mutex_seam.py`),
  reference-count helpers (`recomp/test_vita_kage_refcount_seam.py`), inline
  `floor()` (`recomp/test_vita_floor_thunk_fastpath.py`), shader attribute
  replay (`recomp/test_vita_shader_attrib_fastpath.py`), `Image::PushQuad`
  replay (`recomp/test_vita_kage_quad_fastpath.py`), GL calls dispatched
  straight to the GL adapter (`recomp/vita/test_gl_shim_fastdispatch.sh`).
- Stock vitaGL profile (`recomp/vita/vitagl-stock-reference/`): pinned
  upstream vitaGL plus tracked patches (exact GPU draw and ColorOffset
  optimizations, the neutral ColorOffset fragment path, shader cache, 8-slot
  offscreen render target, laser ring shadow skip, valid-region and scissor
  fixes). Tests: `recomp/vita/test_vitagl_stock_reference_recipe.py`,
  `recomp/vita/test_vitagl_stock_patch_chain.py`.
- Sprite-sheet retention across rooms (`ISAAC_VITA_IMAGE_RETAIN`, OFF;
  `recomp/test_vita_image_retain.py`): unmeasured on device.
- Heap: 81 MiB newlib heap below the fixed image; an ownership ledger tracks
  which allocator owns every block (`ISAAC_VITA_HEAP_LEDGER_*`, on by default).
  Tests: `recomp/vita/test_heap_*.sh`.
- On-device Manager (`recomp/vita/manager_storage.c`, `manager_ftp_server.c`):
  a second button on the game's LiveArea page; mod on/off through the game's
  `disable.it` marker, SHA-256-verified save backup and restore, passive-mode
  FTP on port 1337 rooted at `ux0:/data/isaacr001`, no authentication.
- PC sync tools (`tools/isaac_vita_sync.py`, `tools/isaac_vita_sync_gui.py`):
  `plan | push | pull` for saves and Workshop mods over that FTP server; push
  never deletes user content; pull downloads the three save slots into a new
  directory. Tests: `tools/test_isaac_vita_sync.py` (fake FTP).

## Safety and provenance

- The translated image is fixed at `0x98000000`, built ARM EABI5 softfp with
  Thumb-2 generated units; the executable is built with address randomization
  off, so kubridge is required to map it.
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

105 `recomp/test_*.py`, 27 `recomp/vita/test_*.py`, 67 `recomp/vita/test_*.sh`,
8 `tools/test_*.py`. Release gates, all passing on 2026-09-07:
`recomp/vita/test_vitagl_stock_reference_recipe.py`,
`cmake -P recomp/vita/test_kage_vita_stock_reference_cmake.cmake`,
`python -m compileall tools recomp`, `tools/release_audit.py --strict
--include-untracked` (GO), `python tools/build_vita.py --self-test`,
`recomp/vita/test_kage_vita_io_profile.sh` (needs a softfp VitaSDK).

## History

| Date | Build | Change | Headline |
|---|---|---|---|
| 2026-08-31 | perf:bundle19-stablefps-lua-v1 | audio + first Lua bridge on device, original frame limiter | 59.83 FPS average after transitions; transitions 9-40 FPS; EID did not start |
| 2026-09-02 | perf:bundle35 (720x408, audio off, Lua off) | 60 Hz wrapper, phase profiler | start room 60 FPS at 7.0 ms; pause menu 45 FPS; with audio on UPS fell to 20.5 |
| 2026-09-03 | perf:wf-native-vorbis-v1 | 480x270 render surface at 960x544, native Vorbis on CPU 2, Lua on, translated-CPU, exit fix | pause menu 60 FPS at 7.4-7.7 ms; UPS 30.00; audio service p95 3-4 ms |
| 2026-09-03 | perf:wf-flags-v1 to v4 | Lua vararg formatter, 80 extra code-pointer roots, GC step clamp 128 KB | EID runs and draws descriptions; 14 FPS before the clamp |
| 2026-09-03 | perf:wf-flags-v5-png | native PNG first device run | sheets over 1 MiB fell back; fixed by the 16 MiB reserve |
| 2026-09-03 | perf:wf-gpr-v1 | general registers in locals (audio off) | 60 FPS in every game window |
| 2026-09-03 | perf:wf-gpr-v3 | everything on | boot to title 26 s; title render 14.5 ms; start room 11.8 ms p50 |
| 2026-09-03 | perf:wf-opt-v1 | layout hub, dispatch table, sync inline, IAT direct | menu 14.9 to 12.3 ms; UPS 30.00 |
| 2026-09-04 | perf:wf-opt-v5 | leaf inline, SSE to NEON, GL shim fast dispatch | render p50 menu 11.2, light 9.4, mid 11.5, heavy 16.7 ms |
| 2026-09-04 | perf:wf-opt-v7 | native inflate | load stalls unchanged |
| 2026-09-04 | perf:wf-opt-v8, v9 | Vorbis partial-slot async, CRT seek shadow | receipts confirmed on opt-v11 |
| 2026-09-04 | perf:wf-opt-v10 | asynchronous logger | menu real FPS 58.0 to 59.2 (measured on opt-v11) |
| 2026-09-04 | perf:wf-opt-v11 | KAGE mutex seam, floor thunk fast path, typed Lua endpoints | menu real FPS 59.6; light 42.4, mid 48.8; boss with Brimstone about 50 FPS |
| 2026-09-04 | perf:wf-opt-v12 | shader attribute replay, GL tokens, raw args | render p50 menu 10.2, light 7.8, mid 11.5, heavy 22.3 ms |
| 2026-09-04 | perf:wf-opt-v13 | per-floor GC clamp, native `__index` and `getClass` seams | worst laser room 6-7 FPS |
| 2026-09-04 | v0.1.0-alpha | first public VPK | the 2026-09-04 rows above |
| 2026-09-05/06 | batch deep-profile build | neutral ColorOffset path, laser ring shadow skip, OGG queue emergency slot, heap terminal observer, render-skip observer fix, PNG fast paths | door transitions 0.9-1.1 s, Continue 6.9 s, start to menu 4 s |
| 2026-09-07 | rel:v0.1.1-alpha-rc1 | batch plus render-line options; quad fast path for batched quads | menus 60 FPS at 10.0-10.3 ms; rooms 48-52 FPS; dense rooms 33-34 FPS (twin); 4.5 min session, no fault |
| 2026-09-07 | rc2 twin | quad fast path for every quad | 100 % of quads handled; rooms 55-59 FPS at 9.9-10.3 ms; one `ArchivedFile` crash after a PS-button interception |
| 2026-09-07 | rel:v0.1.1-alpha-rc2 | rc1 plus the complete quad fast path, configured from release/v0.1.1-alpha.cmake | 2.5 min session, no fault, fallbacks=0 |
| 2026-09-07 | rel:v0.1.1-alpha-rc3 | rc2 plus recovery of file handles invalidated across an app suspend | PS button at the menu and back: 2 handles recovered, no fault; 4.9 min Utero run (Continue, four rooms, saves), no fault, fallbacks=0 |

The order of work is in [PLAN.md](PLAN.md).
