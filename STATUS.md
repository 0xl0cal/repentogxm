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
build id:  rel:v0.2.0-alpha-rc1
source:    the v0.2.0-alpha snapshot, configured from release/v0.2.0-alpha.cmake
           (-DISAAC_VITA_GUEST_LINK_ID=rel:v0.2.0-alpha-rc1) in a fresh build
           directory
eboot.bin: 11,948,474 bytes
sha256:    ce218f355fc910b4cd03e4241f0fcc68ee8789b2ff2ffc1255b5539dfaa10ee1
VPK:       36,485,239 bytes (about 20 MB of it are the six trophy packs)
sha256:    b35cc22b99095783b7b984db01530183c122d3058feda708b3ebbd35de2e6418
sessions:  2026-11-13: boot smoke, 90 s to the title screen with music, every
           speed-up banner present, no fault. The same build with only
           ISAAC_VITA_LAUNCH_ARGS_FILE=ON added (every other object identical
           but the build id) played the seeded bench through all eight phases
           (Downpour with water, a floor change, Basement I): no fault, no Lua
           error, game updates per phase within 0.15 per second of the
           measured development build. FPS numbers come from that
           development build (README); the release build carries no
           measurement code.
```

The complete option set is `release/v0.2.0-alpha.cmake` (option names are
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
  vitaGL pool (`ISAAC_VITA_VITAGL_RAM_RESERVE_MB`), a second-tier pool after
  newlib exhaustion (`ISAAC_VITA_HEAP_OVERFLOW_MSPACE`, size
  `ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=0x007d5000`, 8 MiB in this
  release), a one-slot emergency allocation for the music stream queue
  (`ISAAC_VITA_OGG_QUEUE_EMERGENCY`), lock-free read of the allocator's
  terminal state (`ISAAC_VITA_HEAP_TERMINAL_FASTPATH`).
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

v0.2.0-alpha (2026-11-13): the development build with the same speed-ups as
the release plus the measurement code, two seeded bench runs (EID loaded, the
same rooms every run). FPS is presented frames per second over the whole
phase, not the 120-frame windows above; game updates 29.1-29.8 per second in
every phase but the floor transition (24.7 while loading).

| bench phase | FPS (two runs) |
|---|---|
| Downpour, quiet room | 59.6 |
| Downpour, fight | 57.0 |
| Downpour, room with a big spawn | 49.0-52.3 |
| floor transition, including the loads | 40.3-40.5 |
| Downpour, another quiet room | 59.6 |
| Basement I, quiet room | 59.6-59.7 |
| Basement I, fight | 58.9 |
| Basement I, room with a big spawn | 56.7-58.4 |

The rest of this section is v0.1.1-alpha.

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
    front-touch zones; update and rollback; sessions longer than 27 minutes;
    standby (power button) with a run open. The
    Manager's FTP has no authentication; any LAN device can modify
    `ux0:/data/isaacr001` while it runs. No remapping exists.
11. Chinese language crashes on load (issue #4; v0.1.0 and v0.1.1-alpha;
    fixed in v0.2.0-alpha).
    The Chinese font atlases in `repentance_zh.a` decode to 16 MiB
    (`TeamMeatFontExtended12/16bold`, 2048x2048 RGBA) and 32 MiB
    (`UpheavalExtended_1`, 4096x2048); the shipped transient texel scratch
    admits at most 12,410,880 bytes, the fallback guest heap cannot hold the
    request either, and the font renderer dereferences the NULL page. The fix
    is in the tree behind `ISAAC_VITA_TEXEL_SCRATCH_32M` (a 33,558,528-byte
    scratch reserved before vitaGL, which shrinks the vitaGL RAM pool by the
    same amount) and is in v0.2.0-alpha; a second, unconditional
    fix keeps guest `malloc(0)` alive when newlib is exhausted, which stopped
    the follow-up `bad_alloc` seen on a cold start with Chinese saved.
12. The Daily challenge needs Steam. On the port the paper appears and nothing
    follows (reported in GitHub issue 5). Not fixable without a Steam client.
13. Endings stop the game on v0.1.1-alpha (fixed in v0.2.0-alpha; found 2026-09-09 jumping into the
    chest after Ultra Greed on the test device): every cutscene with a video
    part opens its `.ogv` through libtheora, whose CPU-feature probe toggles
    EFLAGS.ID with `pushfd`/`popfd`, and `popfd` was the one instruction the
    translator did not emit (`guest stop: addr=0x005e0050 fault=untranslated:
    popfd return=0x005dfe43`, controlled exit, no dump). Fixed in the tree:
    the emitter translates `popfd` and `pushfd` carries the ID bit; the
    corpus was regenerated. Behind it the video ran out of memory: theoraplayer
    builds a queue of 8-16 RGBA frames of 816x480 (1.5 MiB each) while the
    guest heap after a run is ~98 % full, so the fifth frame died in
    `bad_alloc` (`request=1566720 owner=0x005bf75e`). The knob
    `ISAAC_VITA_THEORA_PRECACHE_FRAMES` (default 0 = untouched, release
    builds use 3) wraps `TheoraFrameQueue::setSize` and caps the queue; the
    decoder just reads ahead less. Third layer: the first decoded frame goes
    through libyuv, whose CPU-dispatch init asks `getenv` for twelve
    `LIBYUV_DISABLE_*` switches and the CRT shim faulted on the unmeasured
    names (`fault=getenv received an unmeasured variable name
    return=0x005d652a`). The shim now answers NULL for all twelve (unset on a
    PC), so libyuv dispatches on the port's CPUID profile (X86 + SSE2, C row
    functions; its SSSE3/SSE4/AVX variants stay untranslated and
    unreachable). Fourth layer: with the three above fixed the clip loads and
    the screen stays black at 60 FPS while the game waits for frames.
    theoraplayer decodes on a worker thread created with
    `KERNEL32!CreateThread`, which the port leaves unresolved; the generator's
    `THEORAPLAYER_PC_WORKER_BYPASS` skips `createWorkerThreads` whenever the
    KAGE backend is up, so the manager has zero workers and nothing ever
    decodes. The knob `ISAAC_VITA_THEORA_WORKER_PUMP` (default 0 = untouched,
    release builds use 1) keeps a host-resident `TheoraWorkerThread` and runs
    the untouched worker loop (`sub_005c2dc0`) as one bounded slice per game
    frame on the main thread, entered from the game's own per-frame
    `TheoraVideoManager::update` (`sub_00598d60`, the first call of every
    main-loop iteration); the `decodeNextFrame` seam caps the decodes per
    slice and the Sleep shim ends a slice instead of delaying. A slice is skipped while the main thread holds
    a theora mutex (hltypes treats a re-lock as a deadlock spin). Pinned by
    `recomp/test_theora_worker_pump_sites.py`. Device 2026-09-10: the first
    pump build hung after three slices (the Sleep intercept compared a based
    return address while the guest stack holds bare RVAs; fixed), then the
    ending video played to the end and the run continued, but at 1-2 frames
    per second with bursty audio: the translated libtheora costs about 25 ms
    per 816x480 packet, so the clip timer runs ahead and theoraplayer keeps
    one frame in sixteen. Fifth layer: `ISAAC_VITA_NATIVE_THEORA` (default
    OFF) decodes with the VitaSDK `libtheoradec` compiled natively behind
    thirteen `th_*` `--wrap` seams (`th_info` converted field by field for
    the one-byte enums, the rest shared in place) and times the translated
    YUV->RGB conversion. Device 2026-09-10 (nt1, full run to Mom's Heart):
    `th_decode_alloc` answered NULL because libtheoradec's `malloc` is the
    raw newlib arena, i.e. the guest heap with no 2.5 MiB left at the end of
    a run, and theoraplayer's unchecked NULL reached `th_granule_frame`
    (C2-12828-1). Fixed: the archive is linked as an objcopy copy whose
    malloc/calloc/realloc/free go to the seam's pool allocator (private
    8 MiB memblock mspace per video, guest-heap fallback) and every seam
    refuses a NULL decoder. Device 2026-09-10 (nt2): the pool was created at
    6 MiB (8 MiB refused), the decoder used 4.7 MiB, no fallback, the video
    played to the end, no crash; the native packet decode takes ~18 ms, the
    translated YUV->RGB conversion 86 ms per kept frame, a slice 390-800 ms,
    59 of 712 frames kept. Sixth layer: `ISAAC_VITA_NATIVE_THEORA_RGB`
    (default OFF) converts YUV->RGB natively (one `--wrap` on libyuv's
    `I420ToRGB24Matrix`, its C reference math in NEON with the coefficients
    read from the caller's table; bit-identical, the cpuid seam reports no
    SSSE3 so the translated build ran the C row) and the theora seam times
    the clip's Vorbis decode. Device 2026-09-10 (nt3): conversion 86 ->
    18.6 ms per frame, packet decode 16.3 ms, Vorbis 1.5 ms per packet,
    about 4-5 frames per second shown at real time. Seventh layer: the row
    in int16 lanes, sixteen pixels per step, behind a range guard on the
    loaded constants (32-bit lanes otherwise; exhaustive proof
    `recomp/test_native_yuv_s16.py`), loop counters moved out of the row;
    device measurement pending. Remaining: `th_decode_packetin` at 16 ms per
    packet (libtheora's ARM assembly is the next lever). Eighth layer (source
    only, ARM build and device gates pending): `ISAAC_VITA_THEORADEC_SOURCE=master`
    links a libtheora 1.2.0 ARM/NEON archive from `ISAAC_VITA_THEORADEC_PREFIX`
    (vendor string in the configure log and in the alloc receipt, `nm -u`
    import audit of the pooled copy), and `ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST`
    logs a CRC32 per plane per packet to be diffed against the host oracle
    `recomp/vita/theora_plane_digest.c` (`theora_plane_digest_compare.py`). Pinned by `recomp/test_native_theora_sites.py`.
14. Checked and not applicable (2026-09-10): the reference recompilation of
    Repentance+ 1.9.7.17 pays a per-layer `SetShaderUniform("ChampionColor")`
    with a strcmp table scan and a malloc/memcpy/free per value change. In
    the frozen 1.7.9b PE the champion tint is the `ChampionColorIn` vertex
    attribute of `shaders/coloroffset_champion` (attribute table 0x7aa558);
    the per-layer draw `sub_00003e50` only swaps the current-shader pointer,
    and the KAGE `Shader::Bind` uniform-record loop (`sub_005671d0`) walks a
    vector nothing ever appends to. The two real uniform setters
    (`sub_00567490`, `sub_00567430`) neither scan nor allocate, and their
    2-3 location queries per batch flush already hit the shim location cache.
    The diagnostic knob `ISAAC_VITA_UNIFORM_SET_RECEIPT` (default OFF)
    counts the path per ph120 window (including the values the per-frame
    `Shader::Collapse` `sub_005679e0` is about to free) so a device build can
    confirm `r=0`, `it=0`, `c(f)=0`; no fast path was added because there is
    nothing to bypass.
15. Borrowed and parked OFF (2026-09-10): the reference recompilation keeps
    the archive inflater's Huffman tables across blocks instead of rebuilding
    them per block. `ISAAC_VITA_INFLATE_TABLE_MEMO` (default OFF, requires
    `ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH`) copies the fixed tables after the
    first fixed block and memoises dynamic tables by their exact code-length
    header in `ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS` (16) slots; only the
    archive tinfl translation unit takes the definition, the PNG entry is
    fenced. On the shipped 1.7.9b archives it is neutral: the host oracle
    sees 14-22 fixed blocks per whole archive, 0 dynamic hits on
    afterbirth.a/afterbirthp.a (the dynamic lookups switch themselves off
    after 4096 misses) and 645 hits out of 59,507 dynamic blocks on
    repentance.a. The two-table build is 34-37 % of the per-piece decode on
    the host, so the gain only materialises with the static-Huffman archive
    repack and the ON measurement must be paired with it. Pinned by
    `recomp/test_vita_inflate_table_memo.py` (OFF identity, PNG fence,
    differential over the full coroutine state on the real archives) and the
    raw allocator gate; no device number.
16. Borrowed and parked OFF (2026-09-10): a packed-archive read trail for
    read-ahead and layout planning. `ISAAC_VITA_ARCHIVE_READ_TRAIL` (default
    OFF, diagnostic; requires `ISAAC_VITA_ARCHIVE_FILE_CACHE`,
    `ISAAC_VITA_PHASE_PROFILE` and the translated KAGE runtime) records one
    run per packed-archive seek (basename, offset, bytes, freads, process
    time) inside the CRT hooks and prints them per phase window as one
    `ph120.rts` summary and up to `ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP` (64;
    256 for device traces) `ph120.rt` records; OFF adds no object, define or
    link option. A log analyzer that is not part of this repository turns
    the records into per-room read lists, backward-seek counts and repeat
    ratios. Pinned by `recomp/test_archive_read_trail.py` (frozen-PE seams,
    CMake wiring, OFF identity, record shape) and the host oracle
    `recomp/vita/test_archive_trail.sh`; no device trace yet.
17. Borrowed and parked OFF (2026-09-10): the game's `log.txt` written behind
    the game thread. At the Mom's Heart death (nt2p, ph120 windows 90/91) the
    main loop stalled 128 ms and 317 ms; 45.5 ms and 28.6 ms of that were the
    frozen logger's own `log.txt` writes, one synchronous `sceIoWrite` of
    15-25 ms per record, because the logger flushes after every record and
    the Vita CRT hands the flush to a 1 KiB-buffered newlib FILE.
    `ISAAC_VITA_GAME_LOG_ASYNC` (default OFF; needs
    `ISAAC_VITA_ARCHIVE_FILE_CACHE` and `ISAAC_VITA_LOG_ASYNC`) adopts that
    FILE as a host byte ring (`ISAAC_VITA_GAME_LOG_ASYNC_RING_KB=64`) appended
    by one worker thread: INFO flushes return without storage I/O,
    WARN/ERROR/ASSERT flushes (`ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY=1`)
    wait for the append in flight and then append synchronously, every
    controlled end drains, and any read, seek, stat, reopen or directory scan
    of `log.txt` first drains, so the file stays byte-identical to the
    synchronous result; a worker wedged for 10 s is abandoned and the lane
    goes synchronous. Receipts `[isaac-gamelog] open/refuse/append/wait/summary`
    (`refuse ... mode=` = a write-mode open of `log.txt` the lane did not take,
    so an inert lane is visible on device).
    OFF adds no object, define or link option (the six edited translation
    units are byte-identical once the guarded blocks are stripped). Pinned by
    `recomp/test_vita_game_log_async.py` and the host oracles
    `recomp/vita/test_game_log_async.sh` (module at ring 64/32 and every
    durable severity; the production CRT TU with and without
    `ISAAC_VITA_GAME_LOG_BATCH`); no device number yet. The frozen PE's
    `fopen` mode of `log.txt` is asserted by the pin test's `--pe` run (every
    `call [fopen]` import site is decoded to its mode literal; every
    write/append site must use a mode the lane accepts and no site may pass
    the mode in a register); that leg needs the frozen PE and is skipped
    without `--pe`.
18. Vita error C2-12828-1 at Ultra Greed's death (GitHub issue 1). The crash
    dump is a data abort inside `glClear`: stock vitaGL ignores a failed
    `sceGxmCreateRenderTarget`/`sceGxmBeginScene` (GXM driver memory runs out
    under the boss's offscreen load), so no scene is open and the clear writes
    its uniforms through an uninitialised pointer. The fix is in the tree
    behind `ISAAC_VITA_VITAGL_SCENE_GUARD` (stock-reference patch 0034: the
    clear and scissor-mask writes are skipped and counted while no scene is
    open, `KAGE VITA SCENE GUARD` receipts in `first-arm-fault.log`) and is
    not in the released build. Its first revision froze the display when a
    `sceGxmEndScene` failed (2026-09-27, Continue with the Chinese font under
    VRAM pressure): it marked the scene closed and skipped every later end,
    while GXM kept the scene open and refused each new begin with
    `WITHIN_SCENE`. The guard now retries the end as stock vitaGL does and
    logs `dangling`, `end_retry`, `recovered` and `end_err`.

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
- Game log write-behind lane (`ISAAC_VITA_GAME_LOG_ASYNC`,
  `recomp/runtime/host_vita_game_log.c`, `recomp/vita/test_game_log_async.sh`,
  `recomp/test_vita_game_log_async.py`); parked OFF.
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
  Tests: `recomp/vita/test_heap_*.sh`. `ISAAC_VITA_GUEST_STACK_MEMBLOCK`
  (OFF) moves the 4 MiB guest stack out of that heap into its own USER_RW
  memblock; the entry tail stops the guest sampler and the Lua beat thread
  before the stack is released; unmeasured on device. Tests:
  `recomp/test_guest_stack_memblock.py`,
  `recomp/vita/test_guest_stack_memblock.sh`.
- On-device Manager (`recomp/vita/manager_storage.c`, `manager_ftp_server.c`):
  a second button on the game's LiveArea page; mod on/off through the game's
  `disable.it` marker, SHA-256-verified save backup and restore (optionally
  of the `persistentgamedataN.dat` + `gamestateN.dat` pair under one index:
  `ISAAC_VITA_MANAGER_SAVE_GAMESTATE` / `tools/build_vita.py
  --manager-save-gamestate`, OFF by default and in the release profile, host
  tests only), passive-mode FTP on port 1337 rooted at
  `ux0:/data/isaacr001`, no authentication.
  The manager draws into a RAM canvas and presents through two CDRAM
  buffers (`manager_present`, flip on `SCE_DISPLAY_SETBUF_NEXTFRAME`); the
  first builds redrew the sub-pages every frame straight into the buffer the
  display was scanning, so the LCD showed the cleared background above the
  row where the panel and text writes overtook the beam and the header never
  landed (issue #1 photos) while pngshot screenshots, which read memory,
  looked complete.
- Port settings file `ux0:/data/isaacr001/port.ini` (knobs
  `ISAAC_VITA_RUNTIME_SETTINGS` for the game and
  `ISAAC_VITA_MANAGER_SETTINGS_UI` for the Manager, both OFF by default; the
  release profile turns the game-side knob on, since the render-scale
  default needs it, and leaves the Manager page OFF): one pure-C parser TU shared
  by both SELFs (`recomp/runtime/host_vita_settings_parse.c`), a boot-time
  loader with the `ISAAC VITA SETTINGS:` receipt line, `fps_lock`
  60/45/40/30 as an admission duty on the fullspeed scheduler's interpolated
  phase, and the Manager's `ISAAC SETTINGS` page (`recomp/vita/
  manager_settings.c`, `manager_settings_ui.c`: FPS LOCK and ROOM RENDER
  rows, SAVE / DEFAULTS / UNDO through `.manager-backups/port-NNNN.ini`,
  PORT.INI and LAST BOOT status rows). 2026-09-16 (owner decision): the
  port default for the game's `MaxRenderScale` is 1 (480x270 world
  raster); the port.ini key `render_scale=1|2` selects it and
  `ISAAC_VITA_RENDER_SCALE_DEFAULT` (`recomp/runtime/
  host_vita_render_scale.c`, called by the loader) rewrites the one
  `MaxRenderScale` line of the game's `options.ini` at every boot with a
  rename-aside publish protocol and the `ISAAC VITA RENDER SCALE:`
  receipt. Untested on hardware; the device gates of the
  design note are open. Tests: `recomp/vita/test_vita_runtime_settings.py`,
  `recomp/vita/test_manager_host.py`,
  `recomp/vita/host_tests/manager_settings*_host_test.c`.
- PC sync tools (`tools/isaac_vita_sync.py`, `tools/isaac_vita_sync_gui.py`):
  `plan | push | pull` for saves and Workshop mods over that FTP server; push
  never deletes user content; pull downloads the three save slots into a new
  directory. Tests: `tools/test_isaac_vita_sync.py` (fake FTP).
- SFX halving tool (`tools/isaac_vita_halve_sfx.py`): offline, opt-in rewrite
  of a resources copy that stores treble-free sound effects at half their
  sample rate (127-tap Kaiser sinc, 2:1), with an ARCH000 codec mirroring the
  engine's mount checksum, raw XOR and MiniZ/ISAAC stored pieces. v1.7.9b
  with the default archive set (`afterbirthp.a`, `repentance.a`, `sfx.a`):
  591 of 922 catalogued samples, PCM 185.5 to 110.9 MiB; `afterbirth.a`
  (149 more samples, 12.0 MiB) only with `--include-afterbirth`, because its
  trusted validation receipt is pinned to the shipped file and a rewritten
  copy would pay full mount-time validation on every boot: 63-66 s, measured
  on the Vita for the shipped `afterbirth.a` validated entry by entry (same
  2647 entries; perf build before its receipt existed) as the proxy.
  Every rewritten archive gets a `<archive>.receipt` sidecar (written after
  the tool verified all entries); the runtime knob
  `ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR` (default OFF, not in the release
  set) trusts it and skips the per-entry validation the rewritten entries
  would otherwise pay on every boot. `--game-log log.txt` reports the
  resident-PCM saving for one session. Never in place; unmeasured on a Vita.
  Tests: `tools/test_isaac_vita_halve_sfx.py`,
  `recomp/vita/test_archive_validation_sidecar.sh` (host oracle of the C
  module against the tool's sidecars).
- Archive repacker (`tools/isaac_vita_repack.py`, standard library only):
  offline, opt-in re-encoding of the MiniZ (mode 2) archives' deflate pieces
  as static Huffman, keeping the 0x400-byte piece cadence, keys, sizes, mount
  checksums and entry order, so the engine's inflater no longer builds a
  Huffman table for almost every piece it decodes; stored pieces are copied
  through the keystream, `afterbirth.a` / `animations.a` (pinned by the
  runtime receipt) and every non-MiniZ archive are copied unchanged.
  `repack | repack-dir | verify | info`; before success every entry of the
  repacked archive is decoded (zlib, the engine-model inflater, and in the
  test the runtime's own C decoder through ctypes) and byte-compared with the
  original. Shipped v1.7.9b: `afterbirthp.a` 56197 dynamic blocks to 0,
  134.2 to 128.9 MB (-3.9 %); `repentance.a` 59507 to 0, 385.0 to 381.6 MB
  (-0.9 %). Pairs with `ISAAC_VITA_INFLATE_TABLE_MEMO` (host x64 runs of the
  runtime's own tinfl port: about -30 % inflate time from the repack alone,
  -76..-78 % with the memo; the Vita is unmeasured). Run it before
  `isaac_vita_halve_sfx.py` (that tool copies untouched entries byte for byte
  and its `.receipt` sidecar pins the final bytes; `repack-dir` does not copy
  the sidecar of an archive it changed). Never in place; no repacked archive
  has been loaded on a Vita yet. Tests: `tools/test_isaac_vita_repack.py`.
- Champion tint uniform, checked and not applicable (2026-09-10): the
  reference recompilation of Repentance+ 1.9.7.17 pays a per-layer
  `SetShaderUniform("ChampionColor")` with a strcmp table scan and a
  malloc/memcpy/free per value change. In the frozen 1.7.9b PE the champion
  tint is the `ChampionColorIn` vertex attribute of
  `shaders/coloroffset_champion` (attribute table 0x7aa558); the per-layer
  draw `sub_00003e50` only swaps the current-shader pointer, and the KAGE
  `Shader::Bind` uniform-record loop (`sub_005671d0`) walks a vector nothing
  ever appends to. The two real uniform setters (`sub_00567490`,
  `sub_00567430`) neither scan nor allocate, and their 2-3 location queries
  per batch flush already hit the shim location cache. The diagnostic knob
  `ISAAC_VITA_UNIFORM_SET_RECEIPT` (default OFF) counts the path per ph120
  window (including the values the per-frame `Shader::Collapse`
  `sub_005679e0` is about to free) so a device build can confirm `r=0`,
  `it=0`, `c(f)=0`; no fast path was added because there is nothing to
  bypass. Tests: `recomp/test_uniform_set_sites.py`.
- Inflate table memo (`ISAAC_VITA_INFLATE_TABLE_MEMO`, default OFF, requires
  `ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH`; borrowed and parked OFF 2026-09-10):
  the reference recompilation keeps the archive inflater's Huffman tables
  across blocks instead of rebuilding them per block. The knob copies the
  fixed tables after the first fixed block and memoises dynamic tables by
  their exact code-length header in `ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS`
  (16) slots; only the archive tinfl translation unit takes the definition,
  the PNG entry is fenced. On the shipped 1.7.9b archives it is neutral: the
  host oracle sees 14-22 fixed blocks per whole archive, 0 dynamic hits on
  afterbirth.a/afterbirthp.a (the dynamic lookups switch themselves off
  after 4096 misses) and 645 hits out of 59,507 dynamic blocks on
  repentance.a. The two-table build is 34-37 % of the per-piece decode on
  the host, so the gain only materialises with the static-Huffman archive
  repack and the ON measurement must be paired with it. Tests:
  `recomp/test_vita_inflate_table_memo.py` (OFF identity, PNG fence,
  differential over the full coroutine state on the real archives) and the
  raw allocator gate; no device number.
- Archive read trail (`ISAAC_VITA_ARCHIVE_READ_TRAIL`, default OFF,
  diagnostic; requires `ISAAC_VITA_ARCHIVE_FILE_CACHE`,
  `ISAAC_VITA_PHASE_PROFILE` and the translated KAGE runtime; borrowed and
  parked OFF 2026-09-10): for read-ahead and layout planning, records one
  run per packed-archive seek (basename, offset, bytes, freads, process
  time) inside the CRT hooks and prints them per phase window as one
  `ph120.rts` summary and up to `ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP` (64;
  256 for device traces) `ph120.rt` records; OFF adds no object, define or
  link option. A log analyzer that is not part of this repository turns
  the records into per-room read lists, backward-seek counts and repeat
  ratios; `recomp/runtime/host_vita_archive_prefetch.h` is the design note
  for a trail-driven read-ahead (not compiled, not wired; measure with the
  trail first). Tests: `recomp/test_archive_read_trail.py` (frozen-PE seams,
  CMake wiring, OFF identity, record shape) and the host oracle
  `recomp/vita/test_archive_trail.sh` (run at the script's cap of 8 and at
  the header default of 64); no device trace yet.

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

114 `recomp/test_*.py`, 27 `recomp/vita/test_*.py`, 71 `recomp/vita/test_*.sh`,
10 `tools/test_*.py`. Release gates, all passing on 2026-09-07:
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
| 2026-11-13 | rel:v0.2.0-alpha-rc1 | v0.2.0-alpha: GL thread, admission behind the frame, FBO deferred attach, Lua GC pacing, BulletAtlas preload, texel scratch, release recipe release/v0.2.0-alpha.cmake | boot smoke; seeded bench (all eight phases) on the same build plus launch arguments: no fault, updates per phase as on the measured build |

The order of work is in [PLAN.md](PLAN.md).
