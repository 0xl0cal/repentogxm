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

`ISAAC_VITA_ANM2_POOL_INIT=ON` is a default-OFF, runtime-only experiment requiring
translated KAGE and `ISAAC_VITA_ANM2_SCRATCH=ON`. It replaces the first Color
initializer call in each of the six live scratch pools with exact native loops
(four 84-byte and two 108-byte records, 5,000 records each), preserving padding
and the original caller's final iteration tail. Unknown sites, coverage mode,
wrong CPU/frame state, non-live scratch and ordinary guest-heap fallback use the
original helper. The existing scratch ownership lock covers the bulk writes;
no pool is retained longer and no gameplay or rendering policy changes.
Toggling it rebuilds two runtime objects and relinks, without regenerating
or recompiling the guest corpus.

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
`--generate-only` stops after the corpus is generated and verified. The
release build was configured directly with CMake; the complete option set of
v0.1.1-alpha is `release/v0.1.1-alpha.cmake` at the `v0.1.1-alpha` tag (a
CMake initial cache for `cmake -C`), whose header gives the build commands.
Since the tag that file is the living release profile: lines pin their new
knobs there (mostly OFF; `FBO_READBACK_FIX`, `RUNTIME_SETTINGS` and
`RENDER_SCALE_DEFAULT` ON) and the host tests listed in `release/README.md`
step 1 require those pins, so it is no longer a copy of the shipped cache.
`release/v0.1.2-alpha.cmake` is the draft for the next release (composed from
the B-all2 configure inputs of 2026-10-26 plus `ISAAC_VITA_CXX_THROW_DIAG=ON`;
its header lists the levers still without a device gate); at the tag its body
is replaced by `--emit-body` of the shipped build's cache
(`release/README.md` step 1).
`python tools/release/check_release_recipe.py <recipe or CMakeCache.txt>`
fails unless the eight player-facing fix knobs (`FBO_DEPTH_CARRY`,
`TEXEL_SCRATCH_32M`, `VITAGL_SCENE_GUARD`, `BACKDROP_WATERGFX_GUARD`,
`LUA_CALL_ERROR_CATCH`, `EFFECTS_PROFILE_WATER`, `AUDIO_STARVATION_SHIELD`,
`CRT_FREAD_COMPLETE`) and `GL_RECORD_ADMISSION_CACHE_STANDALONE` are ON;
`--against <CMakeCache.txt>` also requires the recipe to equal the cache and
`--emit-body` prints a cache as a recipe body. Its fixture test is
`tools/release/test_check_release_recipe.py` (with `cmake` on PATH it also
checks that `cmake -C` reads every recipe the way the checker does).

## Options the release and the measured device builds use

This section groups the options that matter and names the defaults. Defaults
are OFF unless noted.

- Renderer: `ISAAC_VITA_KAGE`, `ISAAC_VITA_DIRECT_DEFAULT`,
  `ISAAC_VITA_RENDER_SURFACE_NATIVE`, `ISAAC_VITA_VITAGL_STOCK_REFERENCE`,
  `ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS`, `ISAAC_VITA_VITAGL_SHADER_CACHE`,
  `ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY`, `ISAAC_VITA_GXM_STATE_SHADOW`,
  `ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS`, `ISAAC_VITA_DISPLAY_RASTER=960`,
  `ISAAC_VITA_VITAGL_RAM_RESERVE_MB=32`, `ISAAC_VITA_LOADING_PRESENTATION`
  (default ON), `ISAAC_VITA_CLOCK_BOOST` (default ON).
- Audio: `ISAAC_VITA_AUDIO` (default ON), `ISAAC_VITA_AUDIO_WORKER`,
  `ISAAC_VITA_NATIVE_VORBIS`, `ISAAC_VITA_NATIVE_VORBIS_ASYNC`,
  `ISAAC_VITA_OGG_READAHEAD` (with `_CHUNKS=4`, `_WAIT_US=20000`,
  `_PRIORITY=168`; needs `ISAAC_VITA_NATIVE_VORBIS`),
  `ISAAC_VITA_OPENAL_POOL` (default ON), `ISAAC_VITA_AUDIO_STARVATION_SHIELD`
  and the diagnostic `ISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT` (both need
  `ISAAC_VITA_AUDIO_WORKER` or `ISAAC_VITA_AUDIO_REFILL_RECEIPT`),
  `ISAAC_VITA_SFX_FLOOR_PRELOAD` with
  `ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB=8` (default) and
  `ISAAC_VITA_SFX_FIGHT_PRELOAD` (default OFF; needs
  `ISAAC_VITA_SFX_FLOOR_PRELOAD`).
- Lua: `ISAAC_VITA_LUA` with `ISAAC_LUA53_SOURCE_DIR`,
  `ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=128` (default),
  `ISAAC_VITA_LUA_GCCOLLECT_CLAMP`, `ISAAC_VITA_LUA_GC_FINALIZER_PACE`
  (default OFF; with `ISAAC_VITA_LUA_GC_FINALIZER_MAX=8` and
  `ISAAC_VITA_LUA_GC_FINALIZER_COST=512` the Lua archive compiles a two-line
  derivation of `lgc.c`, `lua53_gc_finalizer_pace.py`, so a paced step runs at
  most ~K*1024/COST + 16 `__gc` finalizers instead of a whole cycle's
  ~1,400-1,700; `ISAAC_VITA_LUA_GC_SWEEP_COST` (default 5 = pristine, line
  untouched; 32 proposed) is a third derived line, lgc.c:39, so a step sweeps
  ~K*1024/SWEEP objects instead of ~26,000 per 128 KB; meant to pair with
  `ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=40`, and a clamp below 128 *without* the
  pace makes the finalizer hitch worse; the sub-knobs refuse a non-default
  value while PACE is OFF; OFF keeps the pristine Lua closure byte-identical),
  `ISAAC_VITA_LUA_SCOPE_FASTPATH` (default
  ON), `ISAAC_VITA_LUA_IMPORT_FASTDISPATCH`, `ISAAC_VITA_LUA_NATIVE_INDEX`,
  `ISAAC_VITA_LUA_NATIVE_GETCLASS`, `ISAAC_VITA_LUA_ARENA_MB=0` (default =
  Lua state on the guest heap; the release cache sets 48, a private USER_RW
  mspace reserved before vitaGL init; 33 with the 24 MiB overflow pool).
  `ISAAC_VITA_LUA_CORE_ARM` (default OFF, unmeasured): compile the pristine
  Lua 5.3.3 archive with `-marm` instead of `-mthumb`, nothing else; the
  bridge logs `[isaac-lua] core isa=arm|thumb` per state and the raw
  allocator gate pins the ARM option sequence and relocation encoding.
  `ISAAC_VITA_LUA_BINDING_REPLAY` (default OFF, device A/B pending):
  serve 54 LuaBridge binding thunks natively, one `--wrap` per body: the 51
  template-A scalar getters, CppBindClass GetData `0x446d90`,
  bool(int,int) `0x433870` and the `__propget`/`__parent` walk `0x3f90b0`.
  `host_vita_lua_binding_replay.c` compiles the bodies' own corpus text
  (`host_vita_lua_binding_mirror.h`) and emulates each Lua import crossing in
  place behind pinned IAT slots, endpoints and a stack window. Any pin, owner
  or frame miss runs the translated body. Neither replay header is tracked:
  with the knob ON, configure generates `host_vita_lua_binding_mirror.h` and
  `host_vita_lua_binding_table.h` into the build directory with
  `recomp/vita/lua_binding_replay_table.py`, from the frozen PE and the corpus
  being built. The TU gets the owner units' `GUEST_*`
  definitions. The elided crossings leave the ph120 import census and are
  counted in `[isaac-lua] binding replay stats:`. It needs
  `ISAAC_VITA_LUA_NATIVE_GETCLASS`, `TRANSLATED_CPU` with `GPR_LOCAL` and
  `FLAGS_LOCAL`, `ISAAC_VITA_GUEST_STACK_GUARD=OFF` and
  `ISAAC_VITA_STALL_PROBE=OFF`.
  `ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES` (default
  `scalar;getdata;boolii;index2`) picks the families to wrap.
  `ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY` (proof build) re-runs the replayed
  const scalar getters translated and logs any mismatch. The host oracle is
  `recomp/test_vita_lua_binding_replay.py`; with the knob OFF every object is
  unchanged.
  `ISAAC_VITA_LUA_INTEGER2STR` (default OFF, device A/B pending): the Lua
  archive compiles a derivation of `lobject.c` (`lua53_integer2str.py`: the
  prototype on blank line 366, the call on line 376, one function appended)
  in which `luaO_tostring` formats integers with a digit loop instead of
  `snprintf(buff, 50, "%lld")`. It covers `..`, `tostring`,
  `lua_tolstring` and `lua_pushfstring` `%d`/`%I`; `io.write` and
  `string.format` keep the C library. The text and the length are
  byte-identical (`recomp/test_vita_lua_integer2str.py`: about 20 million
  values against `snprintf`, plus a pristine-vs-derived Lua transcript).
  The raw allocator gate re-derives the file and refuses any other bytes.
  With `ISAAC_VITA_LUA_COST_PROFILE` also ON,
  `host_vita_lua_integer2str_selftest.c` runs once at the first LuaEngine
  state. It compares and times both on the console's newlib and logs
  `[isaac-lua] i2s selftest ... mismatches=0 small ... snprintf_ns= itoa_ns=`.
  OFF keeps the pristine Lua closure byte-identical.
  `ISAAC_VITA_LUA_GETCLASS_DIRECT` (default OFF, device A/B pending; needs
  `ISAAC_VITA_LUA_NATIVE_GETCLASS`): the getClass/getExact wraps run the
  same replay without Lua C API calls. Each call is the pristine `lapi.c`
  body of that API function, inlined, and the two `lua_rawgetp` lookups
  (registry[classKey], mt[identityKey]) come from a validated node memo. A
  memo node is used only while it sits on a node boundary inside the table's
  current node array and still holds the key, which is exactly what
  `luaH_get` returns. The path keeps `luaS_new` on the game's `.rdata`
  pointers, every `luaC_checkGC` point and debt, every stack slot and
  `L->top`, and re-derives stack addresses after each GC point. The
  `ISAAC_VITA_LUA_BINDING_REPLAY` bodies call the same wrap.
  `[isaac-lua] getclass stats:` gains
  `direct=1 memo(class hit/miss/stale,identity hit/miss/stale)=`. The host
  oracle is `recomp/test_vita_lua_getclass.py` (direct variants with GC debt,
  string-cache and dead-slot snapshots, stack-moving `__gc` stress). OFF
  compiles the API path only. CMake stops at the first FATAL, so a negative
  configure of either knob (one expected to fail at that knob's FATAL) must
  also switch off every lever of its initial cache that fails earlier:
  `cmake_knob_probe.py` configures only this
  file's knob block on a host without the SDK (`-C <init> -S recomp/vita
  -B <dir> -D KEY=VAL ...`), and `test_cmake_knob_probe.py` checks that
  both knobs' FATALs are the first ones their negatives reach.
  `ISAAC_VITA_LUA_LOADFILE_RETRY` (default OFF, needs `ISAAC_VITA_LUA`;
  device checks pending): one `-Wl,--wrap=luaL_loadfilex` and one new unit,
  `recomp/runtime/host_vita_lua_loadfile.c`, retry a `luaL_loadfilex` that
  failed with lauxlib's own `cannot read <path>: ...` `LUA_ERRFILE` -- bench
  run D4A4Pc lost EID for a whole run to one such read of
  `descriptions/ab+/cs_cz.lua`, printed as `cannot read ...: Success`
  because lauxlib reads `errno` after `fclose` and newlib's `_close_r` zeroes
  it. The wrap covers every caller in another object: the bridge import
  (`require`, `include`, `LuaEngine::RunScript`), `loadfile`/`dofile` and
  `package.searchers[2]`. Attempt 1 is the unchanged pristine call; a read
  error is retried up to `ISAAC_VITA_LUA_LOADFILE_RETRY_MAX` times (default
  3, 0..3; 0 = count and log only) after 2, 16 and 128 ms, a transient
  `cannot open` (any `strerror` but those of `ENOENT`, `ENOTDIR`, `EACCES`,
  `ENAMETOOLONG`, computed at run time) once, never a directory or a
  `cannot reopen`; a per-boot budget of 16 retries and 1 s of sleep bounds
  the time the game thread holds the Lua owner slot. `recovered` means the
  final status is `LUA_OK`; a final lauxlib `cannot read`/`cannot open` is
  `unrecovered`; any other final status, or a final `cannot reopen`, is
  `changed`. One `KAGE VITA LUA LOADFILE STATS: why=first-load max= delay_us=
  budget= probe= inject= loads= read_err= open_transient= open_err= isdir=
  retries= recovered= unrecovered= changed= observed= budget_hit= sleep_us=
  logged=` line at the first wrapped call and again at `why=loading-complete`
  (`kage_vita_loading.c`), and one `KAGE VITA LUA LOADFILE RETRY: n= result=
  kind= attempts= status= fail_us= retry_us= sce= sce_at= sce_len= sce_us=
  close= inj= path= why=` receipt for each of the first eight events (`why=`,
  the `strerror` text of the first failure, comes last; there is no
  `errno=` field). `ISAAC_VITA_LUA_LOADFILE_IO_PROBE` (test eboots only,
  refused with `ISAAC_VITA_IO_PROFILE`/`_IO_WINDOW_PROFILE`) adds
  thread-armed `sceIoOpen`/`sceIoRead`/`sceIoClose` wraps that fill `sce=`
  (the full 32-bit code of the first failed read; `none` means newlib's fd
  layer failed without a kernel error) and `close=` (the arm is per thread
  and per stack frame: a load that longjmps out of `__real_luaL_loadfilex`,
  out of memory or on a `__gc` error say, leaves it set until a later
  wrapped call or hook from a shallower or equal frame clears it; until then
  that thread's `sceIo` calls from deeper frames outside a load land in the
  dead arm's unreported record,
  and with `IO_INJECT` a matching file opened there can still be injected;
  later loads get their own record and stay clean);
  `ISAAC_VITA_LUA_LOADFILE_IO_INJECT=<path-suffix>@<n>` (needs the probe)
  makes the n-th `sceIoRead` of a matching file return
  `__vita_make_sce_errno(EIO)` on attempt 1 of each load, so the real
  newlib path runs end to end. Neither goes into a release or timing recipe;
  `release/v0.1.1-alpha.cmake` pins all four OFF/empty. OFF adds no source,
  definition or link option; with the knob ON `host_vita_lua.c` and every
  `isaac_lua53` member are unchanged and only `kage_vita_loading.c` gains
  the guarded report call. Host oracle: `recomp/test_vita_lua_loadfile_retry.py`
  (MSVC x86; the pinned Lua sources with `--wrap` simulated by renaming, a
  newlib-shaped stdio over a fault-injecting fake `sceIo` layer, mutants,
  the CMake negatives through `cmake_knob_probe.py`); the bridge variant is
  `loadfile-retry` in `recomp/test_vita_lua_runtime.py`.
- Video: `ISAAC_VITA_THEORA_PRECACHE_FRAMES=0` (default, untouched); a value
  of 1..64 caps theoraplayer's per-clip RGBA frame queue so the 816x480
  ending videos (1.5 MiB per frame, 8-16 frames requested) fit the heap that
  is left after a run; 3 is the release setting.
  `ISAAC_VITA_THEORA_WORKER_PUMP=0` (default, untouched): the Vita build has
  no `CreateThread`, so theoraplayer's decoder worker is never created and a
  video clip stays black; a value of 1..8 runs the untouched worker loop as
  one slice per game frame on the main thread, decoding at most that many
  frames per slice (one `--wrap` seam on the game's per-frame
  `TheoraVideoManager::update` and one on `decodeNextFrame`; the Sleep shim
  ends a slice instead of delaying); 1 is the release setting.
  `ISAAC_VITA_NATIVE_THEORA` (default OFF): decode the video with the
  VitaSDK `libtheoradec` compiled natively instead of the translated
  libtheora 1.2.0alpha (about 25 ms of main-thread time per 960x544 packet,
  which is why the pumped clip ran at 1-2 frames per second with bursty
  audio); thirteen `--wrap` seams on the cross-unit `th_*` calls from
  `TheoraVideoClip_Theora` read the cdecl arguments from the guest stack and
  call the native function on the same memory (`ogg_packet` and
  `th_ycbcr_buffer` are shared in place, `th_info` is converted field by
  field because the VitaSDK toolchain uses one-byte enums), plus a timed
  pass-through on `TheoraVideoFrame::decode` that reports the YUV->RGB cost
  (`KAGE VITA NATIVE THEORA:` receipts). The archive is linked as a copy whose
  malloc/calloc/realloc/free are renamed by objcopy to the seam's pool
  allocator: a private 8 MiB (else 6 MiB) memblock mspace created at the
  first allocation of a video and released with its last free, with the guest
  heap's public API (arena, then overflow pool) as fallback and NULL-decoder
  guards on every seam (`KAGE VITA NATIVE THEORA POOL:` receipts). The raw
  newlib arena is the guest heap, full to within a megabyte by the end of a
  run, which is where the first native build's `th_decode_alloc` failed. The
  seam also times the clip's Vorbis audio decode (a pass-through on
  `vorbis_synthesis`, which `decodeNextFrame` runs on the translated libvorbis
  for every queued audio packet before the video packet). OFF keeps every
  object byte-identical and links no libtheoradec.
  `ISAAC_VITA_NATIVE_THEORA_RGB` (default OFF, requires
  `ISAAC_VITA_NATIVE_THEORA`): theoraplayer's YUV->RGB conversion of a decoded
  frame runs natively. One `--wrap` seam on libyuv's `I420ToRGB24Matrix` (two
  direct callers, the RGB and BGR conversion-table entries; no address-taken
  reference) reads the eleven arguments (ecx, edx and nine stack words),
  reproduces libyuv's C reference row math in NEON with the coefficients read
  from the constants table the caller passes (the RGB entry hands the YVU
  table with the u/v planes swapped, the BGR entry the YUV table), honours the
  negative-height flip and answers 0 / -1 like the original (`KAGE VITA NATIVE
  YUV:` receipts). The Vita cpuid seam reports no SSSE3, so the translated
  build always ran libyuv's C row and the native frame is bit-identical to it.
  The row runs in int16 lanes, sixteen pixels per step, when the loaded
  constants pass a range guard (both frozen tables do; exhaustive proof in
  `recomp/test_native_yuv_s16.py`), else in 32-bit lanes. On the device the
  translated conversion cost 86 ms per 960x544 frame (nt2); the first native
  row (32-bit lanes, eight pixels per step) 18.6 ms (nt3), with the packet
  decode at 16.3 ms and the clip's Vorbis decode 1.5 ms per packet, so about
  4-5 frames per second reached the screen. OFF keeps every object
  byte-identical.
  `ISAAC_VITA_THEORADEC_SOURCE` (`vitasdk` default | `master`, with
  `ISAAC_VITA_NATIVE_THEORA`): which `libtheoradec.a` sits behind the seams.
  `vitasdk` is the sysroot's archive (libtheora 1.1.1, C only) with the
  sysroot header, exactly as before; `master` takes
  `ISAAC_VITA_THEORADEC_PREFIX/lib/libtheoradec.a` (no default prefix: the
  libtheora 1.2.0 release with its ARM/NEON assembly, built with the same
  softfp VitaSDK toolchain and installed to a prefix holding `lib/` and
  `include/`) and adds that prefix's `include` to
  the seam source only, so its `_Static_assert`s see the header that belongs
  to the archive; an empty prefix or one without `theora/theoradec.h` is a
  configure error, and so is a misspelled source or `master` while
  `ISAAC_VITA_NATIVE_THEORA` is OFF (checked after the knob block, so an
  A/B build cannot be mislabelled silently). Either way only the objcopy
  copy with the four allocator imports renamed is linked, and a second step
  of the same custom command (`recomp/vita/vita_theoradec_import_audit.py`,
  `nm -u`) fails the build unless the copy imports
  `isaac_vita_theora_{malloc,calloc,realloc,free}` and none of the raw
  allocator names in the script's `FORBIDDEN` tuple (`malloc calloc realloc
  free memalign posix_memalign aligned_alloc strdup valloc pvalloc
  _malloc_r` plus the newlib/BSD spellings) or any symbol starting with
  `ogg` (the raw allocator gate does not scan link archives). The configure
  log prints `source= archive= vendor='Xiph.Org libtheora ...'` read from
  the archive at configure time (the archive is a configure dependency, so
  rebuilding it in place reconfigures), and the seam logs `KAGE VITA NATIVE
  THEORA: archive=<native
  th_version_string()> native_th_info=<sizeof>` at every `th_decode_alloc`,
  because the guest's own `th_version_string` is not wrapped and keeps
  saying 1.2.0alpha. 1.2.0 answers `TH_DUPFRAME` for inter frames with no
  coded blocks (1.1.1 only for empty packets), which is what the guest's
  embedded 1.2.0alpha did, so `dup=` rises with `master`.
  `ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST` (default OFF, diagnostic, requires
  `ISAAC_VITA_NATIVE_THEORA`): after every successful `th_decode_packetin`
  (0 or `TH_DUPFRAME`) the seam reads the planes back with
  `th_decode_ycbcr_out` and logs `KAGE VITA NATIVE THEORA DIGEST: packet=N
  ret=R gp=G y=.. cb=.. cr=..`, a CRC32 (IEEE, 0xEDB88320) per plane over
  exactly width x height bytes through the stride, keyed by the packet
  ordinal since the last alloc (roughly 7-11 ms per 960x544 frame through
  a byte-serial CRC, outside the `packetin_us` timing, so frame timings
  taken with it ON are not representative; the ordinal assumes linear
  playback, which the cutscenes are). `recomp/vita/theora_plane_digest.c`
  (host only: libogg demux, three headers, packetin then ycbcr_out after
  every 0/`TH_DUPFRAME` result, same crc functions) prints the same lines
  from the same `.ogv`, and `recomp/vita/theora_plane_digest_compare.py`
  diffs two streams (oracle vs oracle, or a device log vs the oracle; a
  run ends at an alloc receipt, at an ordinal that went backwards or at a
  repeated ordinal with other content, so a second play whose first lines
  the async logger dropped is never folded into the first): planes and
  granule positions must agree on every packet, a `(no digest)` receipt
  fails, `ret` may differ only 0 -> 1 (a dup frame gained) and only without
  `--strict-ret`; 1 -> 0 or any other delta fails. The exactness gate for a
  replacement archive is device stream == host oracle stream == the 1.1.1
  device stream, packet by packet, on the same clip, with
  `--require-complete`; the two same-version legs (1.2.0 device vs the 1.2.0
  host oracle, two device runs of one eboot) run with `--strict-ret`, only
  the 1.1.1-vs-1.2.0 leg takes the lenient default. OFF keeps every object
  byte-identical.
  `ISAAC_VITA_THEORA_WORKER_PUMP_WATCHDOG` (default OFF, diagnostic): with the
  pump on, a reader thread logs the pump, clip and guest-stack state when no
  slice completes for five seconds.
- Shader uniform census: `ISAAC_VITA_UNIFORM_SET_RECEIPT` (default OFF,
  diagnostic; FATAL with `ISAAC_VITA_SCAFFOLD_ONLY` or without
  `ISAAC_VITA_KAGE`) wraps KAGE `Shader::Bind` (`sub_005671d0`),
  `Shader::ApplyUniforms` (`sub_00567b50`), `Shader::SetUniform`
  (`sub_00567490`), `Shader::SetUniformInt` (`sub_00567430`) and the
  per-frame `Shader::Collapse` (`sub_005679e0`, the only place a uniform
  value is ever freed) with GNU ld `--wrap` and counts entries, the Shader's
  uniform-record count, the loop iterations, the name literals ("Transform",
  "Texture0", "Texture1", other) and the values Collapse is about to free.
  With `ISAAC_VITA_PHASE_PROFILE` the counters come out per window as
  `ph120.us b(n,a,r,rn,it,im)=... s(n,i,t,x0,x1,o)=... c(n,v,f)=... bad=`;
  every build also logs a cumulative `KAGE VITA UNIFORM SET:` line at binds
  1,2,4..4096 and then every 4096 binds. Expected on this PE: `r=0`, `it=0`,
  `s(n)=0` and `c(v)=c(f)=0` always (the record vector is never appended; the
  champion tint is the `ChampionColorIn` vertex attribute of
  `shaders/coloroffset_champion`, not a uniform), so the reference project's
  per-layer uniform name scan and malloc/free churn do not exist here and no
  fast path is offered. Pinned by `recomp/test_uniform_set_sites.py`.
- Loading, I/O and memory: `ISAAC_VITA_NATIVE_PNG` (`_RESERVE_MB=16`),
  `ISAAC_VITA_NATIVE_INFLATE`, `ISAAC_VITA_ARCHIVE_FILE_CACHE`,
  `ISAAC_VITA_CRT_SEEK_SHADOW` and `ISAAC_VITA_CRT_DESCRIPTOR_RECOVER` (both
  need the archive cache; the recovery reopens a read-only FILE token whose
  descriptor the kernel invalidated across an app suspend or PS-button
  interception, errno `ENODEV` or `EBADF`, at its logical cursor and redoes
  the failed `fread`/`fseek` once; with the option every guest FILE token is
  its slot address, so the FILE a recovery releases cannot reappear as a
  second live token; one `arcdiag v1 tag=descriptor-recover-v1` line per
  recovery and a `KAGE VITA CRT DESCRIPTOR RECOVER` counter line with every
  seek-shadow report and at shutdown; write and update streams are never
  recovered),
  `ISAAC_VITA_CRT_FREAD_COMPLETE` (default OFF, needs the archive cache: an
  `fread` on a packed-archive FILE token (`resources*/*.a`) that returns
  fewer elements than requested while the stream carries neither the EOF
  nor the error flag is completed in place -- the remainder is read again
  from the cursor the first read left, at most eight more times, until the
  request is whole, a real EOF or an error; the ArchivedFile block reader
  ignores `fread`'s count, so one such short read shifts its cursor and the
  next header read dies on `ArchivedFile block header is invalid` (tour 1,
  2026-09-17, Mines music from `repentance.a`, 48 bytes short); loose files,
  write streams and the raw SceIo lane are never touched; one `KAGE VITA CRT
  FREAD SHORT: tok= want= got= loops= rc=` receipt for each of the first
  eight short reads and a `KAGE VITA CRT FREAD COMPLETE` counter line with
  every seek-shadow report and at shutdown; the OFF build is byte-identical;
  `recomp/vita/test_crt_fread_complete.sh`),
  `ISAAC_VITA_ARCHIVE_DIAG_VERBOSE` (diagnostic, needs the archive cache:
  every `fread`/`fseek`/`ftell`/`fclose` on every packed-archive FILE token
  is retained in a 1024-event `arcdiag` ring, with the ArchivedFile
  block-reader site in `call=` and its caller in `off=` for reads or `word=`
  for seeks, and dumped at ArchivedFile fatals; short reads on every archive
  are written immediately, not only on afterbirth*.a; pair it with a larger
  `ISAAC_VITA_LOG_ASYNC_RING_KB` so the dump burst is not dropped),
  `ISAAC_VITA_ASYNC_SAVE_WRITE`, `ISAAC_VITA_LOG_ASYNC`,
  `ISAAC_VITA_HEAP_MB=81`, `ISAAC_VITA_HEAP_OVERFLOW_MSPACE`,
  `ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES=0x007d5000` (default; the size of
  that second-tier USER_RW pool: `0x` + 8 hex digits with lowest set bit
  0x1000, 0x007d5000..0x03ffd000, retained span +4 KiB; the combined
  ledger+pool budgets and the RoomConfig slab remainder derive from it; the
  default emits no definition, so the default build is byte-identical; only
  meaningful with `ISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON`; boot receipt
  `guest heap overflow PASS: ledger_capacity=524288 pool_bytes=<n>`;
  `0x017fd000` (24 MiB class, +16.9 MB second tier) together with
  `ISAAC_VITA_LUA_ARENA_MB=33` is the pair proposed for the next release --
  33 not 32 keeps the arena's low set bit at 1 MiB so one 1 MiB quantum is
  retained, not 32 MiB; device verification pending),
  `ISAAC_VITA_GUEST_STACK_MEMBLOCK` (default OFF; ON backs the 4 MiB guest
  stack with a dedicated USER_RW memblock `isaac_guest_stack`, request
  0x401000 so the low set bit stays 4 KiB, instead of newlib `memalign`:
  frees 4,194,304 B of the 81 MiB guest heap, costs the vitaGL RAM pool
  4 MiB + 4 KiB, one `guest stack reserve: status=ready path=memblock`
  receipt at boot, fail-closed with no `memalign` fallback; the entry tail
  unbinds the cross-core guest stack readers before `guest_stack_free`
  (`kage_vita_guest_sampler_stop` under `ISAAC_VITA_GUEST_SAMPLER`,
  `isaac_vita_lua_beat_stop` under `ISAAC_VITA_LUA_RECEIPT`) and the free
  publishes the unbinding before the unmap; the define reaches only
  `guest.c` and `entry_vita.c`, and OFF keeps both byte-identical;
  `recomp/test_guest_stack_memblock.py`,
  `recomp/vita/test_guest_stack_memblock.sh`),
  `ISAAC_VITA_TEXEL_SCRATCH`; `ISAAC_VITA_INFLATE_TABLE_MEMO` (default OFF,
  needs `ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH`) keeps the native archive
  inflater's built Huffman tables across the 1 KiB pieces of a mode-2 entry:
  the fixed tables are memcpy'd after the first fixed block, dynamic tables
  are memoised by their exact code-length header in
  `ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS=16` slots (about 6.75 KiB of .bss
  each; the fixed copy is 6.25 KiB more), the decoded bytes and the guest
  tinfl state stay byte-identical, and a bounded
  `KAGE VITA INFLATE MEMO: blocks= fixed_hits= dyn_hits= dyn_miss=
  build_us_saved~=` line is logged for blocks 1..8 and every 256th block.
  The shipped archives are almost
  entirely dynamic blocks with unique headers (afterbirthp.a: 20 fixed of
  56,217 Huffman blocks, 3 repeated headers in 4096), so on them the knob is
  neutral (dynamic lookups switch themselves off after 4096 misses); it pays
  with a static-Huffman repack, where every block after the first is served
  by memcpy instead of the table build (35 % of the host decode time per
  piece).  Proof and census:
  `python recomp/test_vita_inflate_table_memo.py --pe <exe> --archive <.a> --measure`;
  diagnostic `ISAAC_VITA_ARCHIVE_READ_TRAIL`
  (needs the archive cache and `ISAAC_VITA_PHASE_PROFILE`): one `ph120.rts`
  summary and up to `ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP=64` `ph120.rt a= o= n=
  c= k= at=` records per window, one per packed-archive seek run (the exact
  archive window ArchivedFile read); a log analyzer that is not part of this
  repository turns them into per-room read lists, backward-seek counts and
  repeat ratios;
  `ISAAC_VITA_ARCHIVE_VALIDATION_SKIP` and
  `ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT` (mount-time entry validation is
  skipped for PE-baked checksum matches and for the receipt-proven
  animations.a/afterbirth.a), `ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR`
  (default OFF, needs the receipt; a packed archive rewritten by
  `tools/isaac_vita_halve_sfx.py` is trusted when the `<archive>.receipt`
  sidecar the tool wrote matches its size, ARCH000 header, index SHA-256 and
  64 sampled 4 KiB windows; one new runtime source behind
  `--wrap=isaac_vita_archive_receipt_begin`, host gate
  `recomp/vita/test_archive_validation_sidecar.sh`; without it every
  rewritten entry is validated the original way on every boot; afterbirth.a
  alone would pay 63-66 s, measured on the shipped file validated entry by
  entry as the proxy, same 2647 entries).
- `ISAAC_VITA_GAME_LOG_ASYNC` (default OFF, needs `ISAAC_VITA_ARCHIVE_FILE_CACHE`
  and `ISAAC_VITA_LOG_ASYNC`) holds the guest's
  `Documents/My Games/Binding of Isaac Repentance/log.txt` FILE as a host byte
  ring (`ISAAC_VITA_GAME_LOG_ASYNC_RING_KB=64`) appended by one worker thread
  instead of a 1 KiB-buffered newlib FILE flushed on the game thread: every
  `fwrite` is a memcpy, an INFO `fflush` returns without storage I/O, and a
  WARN/ERROR/ASSERT `fflush` (severity read from the frozen logger chain,
  `ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY=1`) waits for the in-flight
  append, then appends the pending bytes itself before control returns, so the
  file content and line order stay byte-identical to the synchronous result on
  every controlled end (`exit`, `_exit`, guest stop, abort) and the
  crash-relevant lines are on the card before the game continues.
  `ftell`/`fseek` to the current end answer from the tracked position; any
  other seek, a second `fopen`, `remove`, `_access`, `GetFileAttributesA` or a
  `FindFirstFileW` of that directory first drains the lane (read-your-writes).
  Measured need: the Mom's Heart death (2026-09-10, nt2p) paid 45.5 ms and
  28.6 ms of `log.txt` writes on the game thread in two consecutive 2 s
  windows, 15-25 ms per record. Receipts `[isaac-gamelog] open ...` and
  `[isaac-gamelog] summary ... demote= late= lost_bytes= fail=` (the device
  gate is all four zero); a write-mode `fopen` of `log.txt` the lane refuses
  (a mode outside `w`/`wb`/`a`/`ab`) leaves `[isaac-gamelog] refuse ... mode=
  reason=`, so a lane that never adopts the log is visible rather than
  silently inert; with the lane live `ISAAC_VITA_GAME_LOG_BATCH` is inert
  for `log.txt`. OFF adds no object, define or link option. Pinned by
  `recomp/test_vita_game_log_async.py` and the host oracles
  `recomp/vita/test_game_log_async.sh`.
- Guest `free` route costs (all default OFF; OFF keeps every
  touched object byte-identical). `ISAAC_VITA_HEAP_FREE_PREFILTER` (needs the
  translated runtime) makes the OGG emergency, ANM2 scratch and texel scratch
  modules publish `{base, end}` of their fixed block
  (`host_vita_heap_free_prefilter.h`; `end` is stored before the base is
  published and cleared after it is unpublished) and lets the free import
  skip their three free routes with one range test when the pointer is
  outside every block -- each route would have answered NOT_HANDLED with no
  side effect. A pointer inside any block still runs all three in the
  original order, so routes, results, faults and owner state are unchanged.
  On the device the three routes handled 0.060 % of frees (A2S). Pinned by
  `recomp/vita/test_heap_free_prefilter.sh`: the real heap import and the
  three real owners at the device layout run an 810,932-import trace
  (405,704 frees, default capacity) OFF/ON for the default, 8M and 32M texel
  capacities (identical digests in 6 phases; ON calls no owner for the
  405,380 outside frees), five mutations
  caught, OFF token-stream and object identity vs the revision before the
  change, and the
  Thumb symbol legs with `VITASDK`.
  `ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH_MASK` (needs
  `ISAAC_VITA_NATIVE_VORBIS_FREE_WATCH`) keeps a live bit per Vorbis mirror
  slot, set at bind before the slot's only `active=1` store and cleared
  after each of its `active=0` stores; the guest free observer loads the
  mask once and visits only set bits, ascending, instead of all 20 slots
  (same slots retired in the same order). Host oracle legs in
  `recomp/vita/test_native_vorbis.sh` (sync and async; the mask equals the
  active set between files; on the real game OGGs every observer call saw
  exactly one live slot). `ISAAC_VITA_HEAP_FREE_COST_PROFILE` (profiling
  only; needs `ISAAC_VITA_HEAP_STAGE_PROFILE`, and the heap TU needs the
  range lease the native PNG/inflate/archive/WAV owners enable -- `#error`
  otherwise) adds `ph120.hg` (the ledger span `g` split into lookup `k`,
  lease/domain check `e` and remove `r`, 1-in-64 on calls whose `a` and `g`
  are not timed, plus exact lookup probes, remove scans and backshift moves)
  and `ph120.hv` (prefilter skip/maybe counts and the Vorbis observer's
  live-slot census with the mask) after `ph120.hm`. Per call:
  `us / (n - over) - z`. Pinned by the heap-free-cost legs of
  `test_kage_vita_phase_profile.py` and the cost legs of
  `recomp/test_heap_stage_profile.py` (trace identical to OFF; exactly 100
  `k`/`e`/`r` spans and 6400 lookups/removes per 6400 import pairs).
  Device read-out: `tools/bench/heap_stage_costs.py` prints the per-call
  stage costs of each bench phase, the `hg`/`hv` split, B - A verdicts
  against a measured A/A floor and a prefilter tripwire (every free import
  counted once by the prefilter, never fewer handed to the block routes than
  they handled); `tools/bench/heap_stage_costs_test.py` pins it.
- Translated code: `ISAAC_VITA_TRANSLATED_CPU` with `_GPR_LOCAL`,
  `_LEAF_INLINE`, `_SSE_LOWER` and the four default-ON sub-options
  (`_COVERAGE_HOOKS_OFF`, `_FS_BASE_INLINE`, `_HOT_LAYOUT`, `_FLAGS_LOCAL`);
  `_F32_VFP` and `_COLD_REST` stayed OFF;
  `ISAAC_VITA_DISPATCH_TABLE`, `ISAAC_VITA_IAT_DIRECT`, `ISAAC_VITA_LAYOUT_HUB`,
  `ISAAC_VITA_SYNC_IMPORT_FASTPATH`, `ISAAC_VITA_SYNC_INLINE_FASTPATH`,
  `ISAAC_VITA_KAGE_MUTEX_SEAM`, `ISAAC_VITA_FLOOR_THUNK_FASTPATH`,
  `ISAAC_VITA_GL_SHIM_FASTDISPATCH`, `ISAAC_VITA_GL_SHIM_TABLE_TOKENS`,
  `ISAAC_VITA_GL_SHIM_RAW_ARGS`, `ISAAC_VITA_SHADER_ATTRIB_FASTPATH`,
  `ISAAC_VITA_KAGE_QUAD_FASTPATH`.
- `ISAAC_VITA_HASCOLLECTIBLE_NATIVE` (default OFF, not in the release recipe;
  requires `ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL`): answers
  `Entity_Player::HasCollectible` (`sub_0033af30`, 413 calls per game tick
  from other units on the console) natively through a `--wrap` seam, with its
  `HasCollectible(0xbf)` call and the callees of its common paths (G FUEL
  mode/query, effect list, item tags, the collectible map lookup, byte vector,
  item predicate, slots) in one host function. Co-op babies, the ids
  0x147/0x148/0x139/9/0x3b and negative ids, G FUEL bytes above 7, the twin
  call, nulls, long walks and a short guest stack go to the translated body
  (`__real_`). With `ISAAC_VITA_GFUEL_SEED_NATIVE` and
  `ISAAC_VITA_PHASE_PROFILE` the G FUEL profiling wrapper keeps
  `--wrap=sub_0033af30` and calls the replay, so ph120.gu `us(h)` times it.
  Counters: `ph120.hn` (`n` entries, `h` answered, `b` native bodies, `y`
  true answers, `d(s,c,i,g,t,n,w,x)` declines by reason). OFF adds no object,
  define or link option. Pinned by `recomp/test_vita_hascoll_native.py`
  (differential oracle against the translated bodies, branch coverage,
  mutation test of every copied constant); `recomp/hascoll_arm_cost.py`
  counts executed Thumb-2 instructions per call, native vs translated
  (clang build, an estimate, not a gate).
- `ISAAC_VITA_TIMEGETTIME_RECIPROCAL` (default OFF, not in the release
  recipe): the guest `timeGetTime` import handler
  (`vita_post_com_timeGetTime`, `host_vita_post_com.c`) narrows the 64-bit
  microsecond process time with four 32x32 multiplies instead of
  `(uint32_t)(t / 1000)`, which is a `__aeabi_uldivmod` call on the
  Cortex-A9. The result is the same integer for every 64-bit `t` (proof in
  `host_vita_tgt_reciprocal.h`; exhaustive and randomized host test in
  `recomp/test_vita_clear_expiry_latch.py`). The first call prints
  `KAGE VITA TGT RECIPROCAL: selftest n=4096 mismatch=0 use=reciprocal`
  (plus `div_us/mul_us` timings with `ISAAC_VITA_PHASE_PROFILE`); a mismatch
  on the device makes the handler keep the division. OFF adds no define.
- `ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE|ON` (default OFF, not in the
  release recipe; OBSERVE requires `ISAAC_VITA_PHASE_PROFILE`): the KAGE
  Clear expiry loops (`sub_00564f50` and its twin `sub_00561f40`) call
  `timeGetTime` once per pending-release image to test `now - stamp > 5000`.
  ON answers every *keep* iteration (the element pointer is the previous
  call's plus 4, same call site, same CPU, vector bounds unchanged) from the
  clock read that started its keep run, so a keep run costs one clock read.
  This is timing-legal, not identical: it is the run the original program
  makes when the keep run takes 0 ms, and an image that reaches 5 s inside a
  run whose clock ticks is released by the next Clear instead (heap only, no
  GL call on the release path). ON ships only with the maintainer's sign-off.
  OBSERVE returns every fresh read (OFF behaviour) and counts what ON would
  do: `ph120.tl` directly after `ph120.a`, `site(n,h,f,s)` (site calls,
  latch hits, re-arming reads, calls from another CPU), `obs(t,x,d,w)`
  (ticks inside keep runs, expiries, decision flips, stamps newer than the
  latch) and `clk` (clock reads by the handler). `d == 0` over a run means
  ON would have run the same trajectory. `tools/bench/bench_run.py` prints
  the per-phase sums and per-render rates (`Clear expiry clock latch per
  phase`) with a verdict line that checks `s == w == 0`, `n == h + f` and
  `clk == tgt` (OBSERVE) or `clk == tgt - h` (ON). The first loop call prints
  `KAGE VITA CLEAR LATCH: mode=<1|2> sites=0x00564fd6,0x00561f6b ...`.
  Only `host_vita_post_com.c` (and `kage_vita_phase_profile.c` for the
  record) receive the define; OFF adds no define, object or link option.
  Pinned by `recomp/test_vita_clear_expiry_latch.py` (frozen-PE and corpus
  pins, a differential oracle against the translated loops, mutants).
- ON in v0.1.1-alpha: `ISAAC_VITA_KAGE_REFCOUNT_SEAM`,
  `ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH`, `ISAAC_VITA_LASER_RING_SHADOW_SKIP`,
  `ISAAC_VITA_STOCK_FBO_RT_SCENES`, `ISAAC_VITA_OGG_QUEUE_EMERGENCY`,
  `ISAAC_VITA_HEAP_TERMINAL_FASTPATH`; `ISAAC_VITA_NATIVE_PNG_REUSE_LARGE` OFF;
  every profiling and observer option OFF in the release build.
- Effects profile: `ISAAC_VITA_ORIGINAL_EFFECTS_PROFILE` (ON in the release
  recipe; requires translated KAGE) makes the generator inject, at the end of
  `OptionsConfig::Load` (`sub_0047fbe0`, the only generated unit that receives
  the define), stores that zero six option bytes after the loader's own:
  shockwave `+0x7e`, caustics `+0x7f`, filter `+0x80`, `EnableWaterSurface`
  `+0x83` and its copied latch `+0x84`, `EnableColorModifier` `+0x85` -- the
  PCSB00676 settings plus no Repentance water/ColorModifier passes.
  `ISAAC_VITA_EFFECTS_PROFILE_WATER` (default OFF; FATAL without the profile)
  adds `ISAAC_VITA_EFFECTS_PROFILE_KEEP_WATER` to that same unit, which
  compiles out only the two water stores, so the Repentance water surface
  (Flooded Caves `water.fs`, Downpour/Dross `water_v2.fs` plus the overlay
  pass: one extra FBO pass and one or two full-screen ALU-heavy passes per
  frame) renders as on PC while the other four bytes are still zeroed. OFF
  leaves the preprocessed owner unit byte-identical; no runtime banner
  reports it -- verify ON on the owner object (no stores to `+0x83`/`+0x84`)
  and on screen.
  `ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER` (default OFF; FATAL
  without the profile; issue #5 item 15-i) adds the define of the same name
  to the same unit, which compiles out only the `EnableColorModifier`
  (`+0x85`) store, alone or together with the water knob, so the per-stage
  floor/room colour tint (the `ColorModifier` whole-room capture) is drawn as
  on PC while shockwave, caustics and filter stay zeroed. The kept branch
  logs once per boot (one line per seam actually executed; on hardware only
  the default seam at `0x47fc22` runs, because there is no
  `resources/config.ini`): `KAGE VITA EFFECTS PROFILE: color_modifier=kept
  shockwave=%u caustics=%u filter=%u water=%u latch=%u color_modifier=%u`,
  reading the six bytes back after the stores (`water=1 latch=1` only with
  `ISAAC_VITA_EFFECTS_PROFILE_WATER`, `color_modifier=1` when the loader
  enabled it). The store was removed in the first place because the
  whole-room capture "can replace the world with a black sampled surface on
  Vita" (Utero II, 2026-08-30); check that stage on the device before turning
  it on in a release. OFF preprocesses to the pre-knob owner byte-for-byte;
  the generator's unit-packing budget is unchanged (the knob text is
  subtracted through `legacy_size_delta`), so no function changes unit, and
  with `GUEST_GPR_FLUSH_MASK=1` the owner's other masks are unchanged (the
  receipt's `extern` declaration sits inside its once-only branch, because
  the mask pass reads it as host code across every `#if` branch).
  ON needs a corpus generated with the knob's fence: configure counts the
  owner's `#if !defined(ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER)`
  lines against its profile blocks and stops with `... needs a corpus
  generated with its fence` when they differ (an older corpus would build,
  still clear the byte and print no receipt).
  Test: `python test_vita_original_effects_profile.py --pe <isaac-ng.exe>`
  (also runs that configure check with `cmake -P` on generated owners).
- Startup Intro (GitHub issue #14): `ISAAC_VITA_INTRO_PLAY` (default OFF,
  not in the release recipe; FATAL in a scaffold-only build). The generator
  injects, after `IsActionTriggered(ACTION_MENUCONFIRM)` (`call 0x004b1170`)
  in the cutscene update `sub_004b78d0`, a forced "confirm pressed" for
  cutscene 1 (Intro) while the startup flags hold. It was written for the PC
  discovery path, which has no decoder worker; on Vita its guard is true as
  soon as the KAGE backend is ready, so the Intro vanishes on every boot. The
  guard is spelled `guest_kage_intro_skip_ready()` (same length as
  `guest_kage_pc_handoff_ready()`, so unit packing is unchanged); `guest.h`
  aliases it to the handoff predicate unless `ISAAC_VITA_INTRO_PLAY` is
  defined, so OFF keeps today's auto-skip with the same preprocessed tokens.
  ON defines the macro for the one owner unit (found by its
  `void sub_004b78d0(...)` definition, configure FATAL unless it calls the new
  guard exactly once, i.e. the corpus came from this generator),
  `manual_kage_vita.c` and `manual_kage_disabled.c`; the Vita function
  returns 0, so the stock Intro plays with its music and Cross skips it
  through the game's own path. First cutscene update prints
  `KAGE VITA INTRO: autoskip=0 backend_ready=<0|1>` once (`backend_ready` is
  informational; the function returns 0 either way). Pinned by
  `recomp/test_vita_intro_play.py --pe <frozen PE> [--generated-dir DIR
  [--base-generated-dir OLD]]`, which translates `sub_004b78d0` with this
  tree's generator and runs the seam oracle on that fresh text; a given
  corpus must carry the same seam (modulo `GUEST_GPR_FLUSH_MASK`).
- Scheduler: `ISAAC_VITA_FULLSPEED_SCHEDULER` and
  `ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES`. The game simulates at 30 Hz;
  the scheduler presents completed phases at 60 Hz.
  `ISAAC_VITA_FULLSPEED_SERVICE_RESERVE` (default OFF) is an isolated
  interpolation-budget experiment: reserve the preceding advancing full
  tick's measured head-to-Game-entry work before the next Game start, instead
  of reserving the Game update body that runs after that start. It adds no
  clock reads, does not change the 30-Hz floor, and falls back to the original
  estimate when the sample is stale, invalid or from a different game.
  `ISAAC_VITA_FULLSPEED_HEAD_ADVANCE` (default OFF) additionally requires both
  fullspeed cadence and service-reserve. It prepares only the predicted next
  full head up to an experimental 4500 us early, leaving published deadlines
  and the 30-Hz Game floor unchanged. Actual early preparation is credited
  against floor realignment; an accepted zero-frame return rolls that credit
  back. Current Manager/Game/counter/frame mismatch discards the prediction,
  not a new guest fault. Service can already have started early once when
  entering pause/menu, but Game still obeys its floor. This runtime-only switch
  adds no clock-read sites, although an extra existing floor wait can execute
  its clock reads more often.
  `ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT` (default OFF; requires
  fullspeed cadence; experimental, in no release recipe) changes only the
  interpolated-frame admission test. The predicted Render cost becomes the
  last Render body (entry to the Present seam) plus the part of its Present
  wait that the Render before it also paid, instead of the whole duration:
  an isolated swap wait no longer refuses the next interpolated frame, a
  wait on every Present still does. Motivation: int2j tour 3 light rooms sat
  at 50 FPS with `skip(a,l,n,f)=20,0,20,0` per 120 loops at a 6.4-ms body;
  every fifth swap waited ~10.7 ms, the inclusive cost refused the next
  interpolated frame, and the refusal kept the wait on a full-phase frame.
  Host oracle (`kage_vita_fullspeed_scheduler_oracle.c`, 39 pinned
  Present-wait cases): a wait that ends on the 60-Hz edge gives 120 presents
  at exactly 30 UPS (OFF: 100); a fixed 10.6-ms wait, which the tour's
  constant rnd95 - rnd50 fits better, gives 120 presents at 29.73 UPS (OFF:
  100 at 30.00); a GPU-bound two-deep vblank queue at 19 ms gives 27.99 UPS
  and fewer real FPS (OFF: 30.00). It is a trade, not a fix, until the
  ~10.7-ms swap wait itself is explained. Adds one clock read per Present;
  the 30-Hz floor, the floor realignment and the render-bound full-phase
  test keep the inclusive duration. The CADENCE30 banner carries
  ` render-cost=excludes-present` when ON. Device gate in light rooms:
  `skip(a,l,n,f)=0,0,0,0`, `p=120`, `g=60` **and** `ph120.s` `at=` deltas of
  2000 ms per 120 loops (2018 ms is 29.7 UPS; `g=60` per 120 loops holds by
  construction and proves nothing about UPS).
  `ISAAC_VITA_FULLSPEED_FLOOR_DEBT` (`OFF|OBSERVE|ON`, default OFF; requires
  fullspeed cadence; FATAL with `ISAAC_VITA_FULLSPEED_HEAD_ADVANCE`) targets
  the UPS lost to
  late Game::Update starts (today 29.3-29.7 UPS on the device). Stock, a
  Game::Update that starts late re-anchors the next 30-Hz floor at the
  clock, so every late start is lost simulation time. ON keeps up to
  `ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US` (default 8000, 1..16666) of that
  lateness: the next floor is max(old floor, now - cap, this full head's
  60-Hz deadline) + 33.333 ms (the grid clamp keeps the floor from moving
  ahead of the next full head's deadline after a large wake overshoot). The
  stale cut keeps nothing when the start is one Game tick (33.333 ms) or
  more late, or when the 60-Hz head dropped stale ticks, reset its clock or
  had its wait fail since the previous start (menus, loads, Continue,
  stalls), so stale time is still dropped exactly as stock. This changes the
  start contract: start[j] - start[i] >= (j - i) * 33.333 ms - cap, one gap
  may be as short as 25.3 ms at cap 8000, at most 31 starts in a sliding
  second, long-run rate still <= 30. The per-tick wrapper sequence is
  unchanged, but ON removes most floor waits, so wrapper ticks and
  Manager::Update calls per second rise from about 59 toward 60, and the
  wall-clock time that Game::Update and Lua mods read at each start moves
  (366 guest `timeGetTime` call sites, EID's `Isaac.GetTime()`). ON
  therefore needs the maintainer's sign-off and is in no recipe. The cap and the
  slack are validated in every mode: a bad value is FATAL even with the knob
  OFF. `ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US`
  (default 0; FATAL without the debt and when >= the cap; acts only with ON)
  admits an interpolated frame predicted to end up to that much after the
  floor, so the overrun becomes kept lateness instead of a refused frame.
  OBSERVE keeps every decision stock (host oracle: behaviour digests equal
  OFF in all 68-71 scenarios of every combination) and both modes print,
  right after `ph120.w`, `ph120.fd` (paces, lateness count/sum/max, kept,
  kept below lateness by the cap or the grid clamp, `clamp(c,us)` = the part
  of those the grid clamp cut below min(late, cap), stale cut
  count/grid-rule count/sum, and gf = next full head's grid deadline minus
  the next floor: after a waited pace that is the pace wake overshoot minus
  the head wake overshoot minus the service, so small positive values are
  normal; after a late pace it is <= 0 in every mode) and `ph120.fs`
  (lateness histogram, slack band/admitted-late/sum, `floor=` the next Game
  floor in us, hex). Per window, the floor stamp advances by p * 33.333 ms +
  late us + stale us under OBSERVE (over us in place of late us under ON),
  to 1 us of rounding per pace (scheduler and phase oracles: exact). Use the
  stamps, not `ph120.s at=` deltas: `at=` is read before the loop head's
  wait, so its deltas also carry the boundary tick's work. Under OBSERVE
  `kept` is ON's rule applied on stock's trajectory, where the grid clamp
  binds far more often than under ON; the cap decision reads
  (kept + clamp) / late. The CADENCE30 banner carries
  ` floor-debt=observe|on cap=<N>us[ admit-slack=<N>us]`. Host oracle (MODEL,
  deterministic clock): a jitter room goes from 29.62 to 30.00 UPS, the
  fps_lock 30/40/45 jitter rooms from 29.63-29.64 to 30.00, a slack room
  from 120 to 200 presents per 240 ticks; one pinned Present-wait case (C2
  fixed 18-ms wait) trades 30.00 for 29.83 UPS for 200 more presents.
  Device gates: OBSERVE first (the `ph120.fd`/`ph120.fs` records above), ON
  only after the maintainer signs off.
- Runtime settings: `ISAAC_VITA_RUNTIME_SETTINGS` (default OFF; requires
  `ISAAC_VITA_KAGE`; refused with `ISAAC_VITA_SCAFFOLD_ONLY` or
  `ISAAC_VITA_IO_PROFILE`; `tools/build_vita.py --runtime-settings`). The
  eboot reads `ux0:/data/isaacr001/port.ini` once at boot
  (`recomp/runtime/host_vita_runtime_settings.c`; shared parser
  `host_vita_settings_parse.c`; grammar and keys in the header comment of
  `recomp/runtime/host_vita_settings_parse.h`: ASCII `key=value`
  lines, `schema=1` mandatory, 4096-byte cap) and logs one receipt right
  after `guest boundary begin`:
  `ISAAC VITA SETTINGS: path=ux0:/data/isaacr001/port.ini
  status=absent|ok|schema|garbage|too-large|io-error(0x........) bytes=N
  lines=N rejected=N unknown=N dup=N fps_lock=N[(ignored:no-fullspeed-scheduler)]
  raster=WxH(ignored:not-compiled) remap=default|custom[(guard)] chords=L/R
  restart_hold=0|1 stick_swap=0|1`, followed by
  `ISAAC VITA SETTINGS REJECTED: lines=a,b,c,d unknown=k1,k2,k3,k4` when any
  line was rejected or any key unknown (first four of each). `dup` counts
  accepted repeats of a key (last wins); a repeat whose value is rejected is
  counted under `rejected` and keeps the earlier accepted value. `fps_lock` =
  `60|45|40|30` is an admission duty on the scheduler's interpolated
  (non-full) phase: 60 admits every one (today's behaviour), 45 every
  second, 40 every third, 30 none; the game keeps simulating at 30 UPS and
  full phases are never dropped for it, so per 120 wrapper ticks the port
  presents 120/90/80/60 frames. 45 and 40 are an uneven cadence (present
  intervals alternate 16.7 and 33.3 ms). It applies only when
  `ISAAC_VITA_FULLSPEED_SCHEDULER` is compiled (the `KAGE VITA CADENCE30:`
  banner then ends with ` fps_lock=N`, and a `CADENCE30 DISABLE` fail-open
  ends the lock with the scheduler); a `STABLE_30` or scheduler-less build
  reports `fps_lock=N(ignored:no-fullspeed-scheduler)`. `display_raster`
  and the `button.*` / `chord.*` / `touch.*` / `stick.swap` keys are parsed,
  guarded (at least one button on `a` and one on `b`, else
  `remap=default(guard)`) and reported only; they take effect in later work
  packages. Absent file, garbage, more than 4096 bytes, a missing `schema=1`
  or an I/O error all mean the compile-time defaults. With the option OFF no
  new object is linked and every existing object is byte-identical
  (per-source definitions; gates `recomp/vita/test_vita_runtime_settings.py`,
  `test_kage_vita_fullspeed_scheduler.py`, `test_vita_raw_allocator_gate.py`).
- Render scale default: `ISAAC_VITA_RENDER_SCALE_DEFAULT` (default OFF;
  FATAL unless `ISAAC_VITA_RUNTIME_SETTINGS` is ON; owner decision
  2026-09-16: `MaxRenderScale=1` is the port default). The port.ini key
  `render_scale` = `1|2` (default 1; 1 = the 480x270 world raster, 2 =
  the game's stock 960x540) is written into the game's own
  `ux0:/data/isaacr001/Documents/My Games/Binding of Isaac Repentance/
  options.ini` by `recomp/runtime/host_vita_render_scale.c`, called by
  the port.ini loader right after its receipt (nested
  `#if defined(ISAAC_VITA_RENDER_SCALE_DEFAULT)` fences in
  `host_vita_runtime_settings.c`; the definition reaches only that TU and
  the normalizer, a nested block of the runtime-settings CMake block).
  The transform replaces the value token of the single `MaxRenderScale`
  line (`1` for 1, `99` for 2; key matched ignoring case; blanks, the
  line's spelling and every other byte incl. LF/CRLF endings kept) or
  appends the line at the end of `[Options]`; a duplicate key, no
  `[Options]`, a NUL byte, an empty file, more than 16 KiB or a missing
  file leave everything alone. Publish protocol: `options.ini.rs-tmp`
  O_EXCL + write + `sceIoSyncByFd`, `options.ini` renamed aside to
  `options.ini.rs-bak`, temp renamed into place, aside copy removed,
  `sceIoSync("ux0:")`; every failure puts the previous file back; a
  stranded aside copy is moved back at the next boot (`recovered=1`).
  Receipt: `ISAAC VITA RENDER SCALE: mode=N target=1|99
  status=unchanged|rewritten|appended|absent|empty|too-large|binary|
  no-section|duplicate|read-error|write-error|rename-error old=<previous
  value|-> bytes=N rc=0x........ recovered=0|1`. Oracle
  `host_vita_render_scale_oracle.c` (transform corpus, publish protocol,
  every failure seam, recovery, receipt goldens; run by
  `test_vita_runtime_settings.py` and `host_tests`). OFF: no new object,
  the loader object byte-identical.
- Debug overlay: `ISAAC_VITA_DEBUG_OVERLAY` (default OFF, debug builds
  only; the release recipe pins it OFF; FATAL unless `ISAAC_VITA_KAGE` and
  `ISAAC_VITA_RUNTIME_SETTINGS` are ON, and together with
  `ISAAC_VITA_CONTINUE_OVERLAY`, which owns the same display callback). The
  port.ini key `debug_overlay` = `off|compact|full` (default off; the key
  exists in the shared parser only in this build) picks what
  `recomp/runtime/host_vita_debug_overlay.c` draws, top-right, into the final
  scanout buffer from vitaGL's display callback (stippled backing, built-in
  5x7 font, doubled from 720 pixels up; no guest GL state is touched, so
  render scale and FBO paths do not matter). `compact`: presents in the last
  second, frame ms p50/max, free user RAM / CDRAM, the overlay's own cost.
  `full`: frame p50/p95/max; Game::Update / Render / vglSwapBuffers p50/max
  (the fullspeed scheduler's own clocks and the present bracket; Render
  includes Present); per-core load of cores 0-2 (`sceKernelGetSystemInfo`
  idle clocks, system-wide) and the game thread's share of one core; the
  three busiest other native threads; one row per RAM consumer (used /
  reserved / peak MiB, K below 1 MiB, `*` = peak of 1 Hz samples, `-` = the
  owner counts no in-use figure) sorted by reservation, then FREE,
  UNACCOUNTED and BASE (BOOT) for user RAM and one line each for CDRAM,
  phycont and the common-dialog pool. The rows come from a ledger of every
  memblock and thread stack the eboot creates: the link wraps
  `sceKernelAllocMemBlock`, `kuKernelAllocMemBlock`, `sceKernelFreeMemBlock`,
  `sceKernelCreateThread`, `sceKernelDeleteThread` and
  `sceKernelExitDeleteThread` (`-Wl,--wrap`), so newlib's heap (crt0),
  vitaGL's pools and every `isaac_*` reservation are booked by name without
  touching their owners; eboot segments come from `sceKernelGetModuleInfo`
  (`SceKernelModulemgr_stub` is linked). BASE = the free size at the first
  wrapped call (before newlib's heap exists) + eboot + main stack, so
  BASE = rows + FREE + UNACCOUNTED per domain; UNACCOUNTED is memory taken
  after crt0 outside the ledger. Memory and threads are sampled at 1 Hz
  (full only; compact reads the two free sizes), text and frame figures at
  4 Hz, the page is republished only when its text changes; with the mode
  off the two present hooks are one branch each and the callback is never
  installed. Receipts: the settings line gains ` debug_overlay=<mode>`,
  then `KAGE VITA DEBUG OVERLAY: mode=... boot=2 rc=... free(u/c/p)=...
  ledger(allocs,frees,dropped)=...`, and in full mode the table goes to the
  log at the first memory sample and every 120 s (`KAGE VITA DEBUG OVERLAY
  MEM/ROW/SUM:` lines). Gate `recomp/vita/test_vita_debug_overlay.py
  [--base <commit>] [--mockup <dir>]` (oracle, fences, OFF token identity of
  every touched TU against the base, WSL gcc object identity of the parser).
  OFF: no new object, every touched object byte-identical (each edit sits
  under `#if defined(ISAAC_VITA_DEBUG_OVERLAY)`; touched units:
  `host_vita_settings_parse.c`, `host_vita_runtime_settings.c`,
  `kage_vita_backend.c`, `kage_vita_fullspeed_scheduler.c`,
  `host_vita_lua.c`, plus includers of the touched headers
  `host_vita_settings_parse.h`, `kage_vita_fullspeed_scheduler.h`,
  `host_vita_lua_gc.h`).
- GL recorder admission cache without the overlay:
  `ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE` (default OFF; FATAL
  unless `ISAAC_VITA_GL_THREAD_WORKER`). The recorder checks every client
  span it copies with `sceKernelGetMemBlockInfoByAddr` and caches the last
  four readable blocks, keyed on a free generation that the link's one
  `sceKernelFreeMemBlock` wrap bumps before each real free. Until now only
  the debug overlay linked that wrap, so a release build (overlay OFF) had
  no cache and queried the kernel on every check. With this knob ON and
  the overlay OFF, CMake adds `recomp/runtime/gl_record_free_wrap.c` (bump,
  then `__real_sceKernelFreeMemBlock`; no lock, log or allocation) and
  `-Wl,--wrap=sceKernelFreeMemBlock`; with the overlay ON the overlay keeps
  the wrap and nothing is added (two definitions would not link). Either
  way `gl_record_backend.c` gets `ISAAC_GL_RECORD_ADMISSION_CACHE=1` from
  one statement, so its object is the same in the measurement and the
  release shapes. The premise, stated narrowly: cached entries come only
  from the game thread's own client spans, and those blocks stop being
  mapped only through a wrapped free; unmaps the wrap cannot see (a deleted
  thread's stack, frees inside system modules) are outside it. Receipt at
  render-thread start: `KAGE VITA GL RECORD ADMISSION: cache=on
  owner=overlay|recorder wrap=linked|MISSING verify=0|1` (only with the knob
  ON: `kage_vita_backend.c` is unchanged with it OFF).
  `ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_VERIFY` (default OFF, measurement
  only; FATAL unless `..._STANDALONE` and `ISAAC_VITA_PHASE_PROFILE`): every
  cache hit is re-queried and a disagreement is a fault; a boot self-test
  caches one 4 KiB page, frees it through the wrap and requires
  `selftest(cached,bumped,stale,after)=1,1,0,0 ok=1` (then empties the
  cache and restores the ph120.rw `mq` count); every 1024 presents
  `KAGE VITA GL RECORD ADMISSION VERIFY: presents=N wraps=W misses=M
  hits=H` (gate: `M <= 4 x W + 1`). `ph120.rw mq` keeps its production
  meaning (queries a cached build makes). Gate
  `recomp/vita/test_gl_record_free_wrap.py [--plant]` (wrap oracle with a
  late-bump mutant on clang and MSVC, the wrap's text, the CMake blocks run
  under `cmake -P` for every recipe shape, clang -E token identity against
  the base with the knobs OFF, and `elf_ref_census.py` on an ld.lld
  fixture); `recomp/vita/elf_ref_census.py --gate wrapped|unwrapped` is the
  ELF census of call and address-taking references on an ARM build. OFF: no new
  object, every object byte-identical.
- C++ throw diagnostic: `ISAAC_VITA_CXX_THROW_DIAG` (default OFF; ON in
  `release/v0.1.2-alpha.cmake`). C++ throws are not implemented
  (`_CxxThrowException` and `__std_exception_copy` stay unresolved), so a
  throw still stops the guest; with the knob ON, `guest_call_slow` first
  calls `guest_cxx_throw_diag`, which reads the thrown object through the
  MSVC CompleteObjectLocator and logs once, right before the stop,
  `KAGE VITA CXX THROW: via=<import> obj=%08x vt=%08x type=<RTTI name>
  L=%08x what="..." luamsg(size=%u cap=%u)="..." ret=%08x`
  (`recomp/runtime/guest.c`, the only unit that gets the define). Configure
  prints `Isaac Vita C++ throw diagnostic: on (guest.c only; logs before the
  stop on an unresolved C++ throw import)` from the same block that applies
  the define, so the line appears exactly when `guest.c` gets it. There is
  no boot receipt: the line is the receipt, and it exists only on the fatal
  path. Per-frame cost is zero by construction and by measurement: the call
  sits in the unresolved-import branch after `guest_host_import_id` failed,
  immediately before `guest_fault`; arm-none-eabi-gcc 10.3.1 with the
  release-shape `guest.c` defines (`-O2 -mthumb -mcpu=cortex-a9`) leaves the
  other 72 functions instruction-identical OFF vs ON (branch targets masked),
  adds the out-of-line `guest_cxx_throw_diag` (0x238 bytes, not inlined)
  and grows `guest_call_slow` from 0x194 to 0x19c bytes by exactly
  `ldr r1,[r7,#4]; mov r0,r5; bl guest_cxx_throw_diag`, placed before the
  argument setup of its one `bl guest_fault`, with an unchanged prologue. Device receipt (build
  `rel:v0.1.2-alpha-int4-cxx-diag-20261012`, EID `json.lua:125` error):
  `type=.?AVLuaException@luabridge@@ ... luamsg(size=108 cap=111)=...`.
  With `ISAAC_VITA_LUA_CALL_ERROR_CATCH` ON that Lua error is caught before
  any throw, so in a release the line only names throws the catch does not
  cover. `type=` and `what=` hold for any thrown object; `luamsg(...)`
  decodes the object as a `luabridge::LuaException` (`std::string` at
  +0x10, size +0x20, capacity +0x24) and means something only when
  `type=.?AVLuaException@luabridge@@`; for other types it prints whatever
  lies there. Its pointers are range-checked (0x80000000-0xC0000000), not
  mapping-checked, so a foreign type could in principle fault inside the
  diagnostic, on a path that is already fatal. For the likeliest release
  throw, `std::bad_alloc` from RVA 0x5ebd99 (return
  `ISAAC_VITA_BAD_ALLOC_THROW_RETURN_RVA` 0x5ebdb5 in `entry_vita.c`), the
  object sits at `[ebp-0xc]`: read as a long-string data pointer, +0x10 is
  the bare return RVA at `[ebp+4]`, which the range check rejects, and a
  short-string read (at most 399 bytes from +0x10) stays inside the guest
  stack, so the bad_alloc decode in `entry_vita.c` still runs after the stop.
  OFF: `guest.c` byte-identical (every edit is under
  `#if defined(ISAAC_VITA_CXX_THROW_DIAG)`).
- GL recorder opcode census: `ISAAC_VITA_GL_RECORD_OPCODE_CENSUS` (default
  OFF, measurement only; FATAL unless `ISAAC_VITA_PHASE_PROFILE` and
  `ISAAC_VITA_GL_THREAD_WORKER`). Defines `ISAAC_GL_RECORD_OPCODE_CENSUS=1`
  on `gl_record_backend.c` and `kage_vita_phase_profile.c` only. Per
  phase-profile window and recorder opcode (slot = opcode, 78 =
  closures, 79/80 = the attribute batches `ATTRIBS_ENABLE` /
  `ATTRIBS_DISABLE` when `ISAAC_VITA_GL_RECORD_ATTRIB_BATCH` is ON, which then
  also defines itself on `kage_vita_phase_profile.c` so the reader's copy
  has 81 slots; slot 0 = out of range): render thread `n` records replayed,
  `b` payload bytes, `e/eus` exact replay timings (the first 32 of each
  opcode in the window, and every closure), `rs/rus` random ones (1/64 of
  the rest); game thread `ps/pus` 1/64-sampled appends and their
  microseconds from the admission check (`pst` of them) or the append
  start to the append end, `sy/sw/swus` S waits charged to the opcode
  before them (closures: worker calls). In this build both 1/64 samplers
  (the game thread's appends, which are also the `ph120.rw ap` samples,
  and the random replay timings) count down a gap drawn uniform 1..127
  from an LCG instead of the `ap_tick` stride (`ph120.rw ap` keeps its
  meaning). After `ph120.rw`: `[kage-vita] ph120.ro bid= win=
  loops= ops= lines= k=64 x=32 ck16= f=n,b,e,eus,rs,rus,ps,pst,pus,sy,sw,swus`
  and `lines` records `ph120.rop bid= win= part= op=slot:fields;...` (three
  slots each, at most 490 bytes). `tools/bench/bench_run.py analyze` prints
  per phase the opcodes by render-thread microseconds per present (clock
  cost removed, 1/64 samples scaled, 4-sigma sampler check) and drops any
  window whose census disagrees with the same take's `ph120.rt`/`rw`
  (sum n = r, sum ps = ap samples, sum sy = s). Cost (estimated, not
  measured): about 0.02-0.06 ms per render on the game thread, about
  0.2 ms per present on the render thread. Gates:
  `test_kage_vita_phase_profile.py` (gl-worker-census leg),
  `test_gl_vita_backend.ps1` with `ISAAC_GL_RECORD=1` (the `census`
  scenario variant: same trace, census checked against the window),
  `recomp/vita/test_gl_record_opcode_census.py [--plant]` (CMake shapes,
  define scope, knobs-OFF token identity). OFF: every object
  byte-identical.
- Manager settings page: `ISAAC_VITA_MANAGER_SETTINGS_UI` (default OFF;
  FATAL unless `ISAAC_VITA_RUNTIME_SETTINGS` is ON; `tools/build_vita.py
  --manager-settings-ui`). Adds `manager_settings.c`, `manager_settings_ui.c`
  and the shared parser TU to `isaac_manager` through `target_sources()`
  with the per-target definition `MANAGER_SETTINGS_UI=1`; OFF leaves every
  manager object byte-identical (every edit in `manager_main.c` and
  `manager_storage.c` sits under `#if defined(MANAGER_SETTINGS_UI)`). The
  page (Start on the Manager's home screen) edits `fps_lock` (FPS LOCK row)
  and `render_scale` (ROOM RENDER row: `480X270 FAST` / `960X540 SHARP`,
  applied by the game build's normalizer at its next start) and writes
  `port.ini` through an O_EXCL temp, SHA-256 readback and rename protocol
  that archives the previous version as `.manager-backups/port-NNNN.ini`;
  UNDO restores the newest archive (twice = redo when the undone write
  replaced a live file; after DEFAULTS + UNDO nothing was replaced and the
  row hides), DEFAULTS archives and removes the file, a `port.ini.rollback`
  stranded by a power loss is promoted by the manager's start-up recovery
  (which runs even when a save-slot repair fails). The page reads the first
  16 KiB of `first-arm-fault.log` for its `LAST BOOT` row (`FPS N REMAP ...`
  from the receipt, `FPS N IGNORED REMAP ...` when the receipt carried
  `(ignored:no-fullspeed-scheduler)`, `FILE <STATUS> - DEFAULTS USED` for a
  `status=` other than ok/absent; `GAME BUILD IGNORES PORT.INI` when the
  last game start logged a `build` line but no receipt). Host tests:
  `host_tests/manager_settings_host_test`,
  `host_tests/manager_settings_ui_host_test`;
  `recomp/vita/test_manager_host.py` builds and runs the whole `host_tests`
  tree (through WSL on a Windows checkout without a C compiler) and then
  proves the knob-OFF object identity of the manager against a base commit
  (`--base`, `ISAAC_MANAGER_IDENTITY_BASE`, or the merge-base with
  `origin/main`): each manager TU is compiled from the base tree and from
  the checkout with the production flags and the objects are `cmp`'d - the
  SDK-free TUs with the host compiler always, all eight `isaac_manager` TUs
  with `arm-vita-eabi-gcc` when `VITASDK` is set (loud skip otherwise), plus
  a `-Werror` compile of every TU with each manager knob ON.
- Manager UI2: `ISAAC_VITA_MANAGER_UI2` (default OFF; FATAL unless
  `ISAAC_VITA_MANAGER_SETTINGS_UI` is ON; `tools/build_vita.py
  --manager-ui2`). Adds
  `manager_font.c`, `manager_canvas.c`, `manager_theme.c`,
  `manager_strings.c`, `manager_ui.c`, `manager_ui_draw.c`,
  `manager_mods_meta.c`, `manager_ini.c` and `manager_assets_vita.c` to
  `isaac_manager` through `target_sources()` with `MANAGER_UI2=1` and the
  build id, links `SceAppUtil_stub` (system language), and packages
  `manager/font-{title-24,body-16,small-12}.imf` (DejaVu IMF1 atlases baked by
  `tools/manager_font_bake.py`) plus `licenses/dejavu.txt`; OFF adds no VPK
  member and every `manager_main.c` edit sits under `#if defined(MANAGER_UI2)`
  / `#if !defined(MANAGER_UI2)`. `manager_ui.c` is an SDK-free page
  controller (HOME, MODS, SAVES, SETTINGS, ACHIEVEMENTS, CONTROLS, ABOUT,
  confirmation box, key repeat, FTP read-only mode, auto-launch countdown)
  that emits a display list; `manager_ui_draw.c` paints it through a theme
  (`manager_theme.h`: `dark`, `light`, and `game`, which a separate game-skin
  knob provides through `manager_skin_game_init()` and which falls back to
  `light` with a status line). `ux0:data/isaacr001/manager.ini` holds
  `lang`, `theme` and `autolaunch`. Host tests: `manager_font_host_test`,
  `manager_strings_host_test`, `manager_ini_host_test`,
  `manager_mods_meta_host_test`, `manager_ui_host_test` (display-list
  checks) and `manager_ui_render_host_test` (FNV-1a frame goldens in
  `host_tests/manager_ui_render_goldens.txt`; `MANAGER_UI_RENDER_UPDATE=1`
  rewrites them, `MANAGER_UI_FRAMES_DIR=<dir>` writes PPM frames).
- Manager UI2 game skin: `ISAAC_VITA_MANAGER_UI2_GAME_SKIN` (default OFF;
  FATAL unless `ISAAC_VITA_MANAGER_UI2` is ON; `tools/build_vita.py
  --manager-ui2-game-skin`, which requires `--manager-ui2`;
  `release/v0.1.1-alpha.cmake` pins it OFF). The `game` theme of UI2 (`theme=game`
  in `manager.ini`, the default) drawn with the game's own art, read at run
  time from the user's `ux0:data/isaacr001/resources/packed` archives
  (`repentance.a`, `afterbirthp.a`, `afterbirth.a`, first archive holding a
  path wins): `gfx/ui/main menu/EmptyScreen.png` as the backdrop, the paper
  sheet and strip of `gfx/ui/main menu/KeyConfigMenu.png` under the page and
  the SAVES cards, the paper and cursor of `gfx/ui/GenericPopup.png` for the
  confirmation box, the `Vita` button frames of `gfx/ui/Buttons.png` on the
  hint bar, and the Team Meat faces `font/TeamMeatFont12.fnt` (body, and the
  title pixel-doubled) and `font/TeamMeatFont10.fnt` (small text) with their
  pages. The Team Meat faces carry the Russian alphabet, so Russian text is
  drawn in them; a code point they lack (`×`, `—`, `▸`, `«»`, ...) is drawn
  per glyph by the DejaVu face of the same role on the same baseline.
  Nothing of the game is packaged: the VPK member set is the UI2 one.
  Adds `manager_skin_game.c` and the readers `manager_arch.c` (ARCH000),
  `manager_png.c`, `manager_bmfont.c`, `manager_tinfl.c` and
  `manager_blob_vita.c` (sceIo) to `isaac_manager` with
  `MANAGER_UI2_GAME_SKIN=1`; the `manager_main.c` wiring (the skin context,
  a USER_RW memblock of `MANAGER_SKIN_GAME_ARENA_BYTES` = 3 MiB requested
  the first time the game theme is selected) sits under
  `#if defined(MANAGER_UI2_GAME_SKIN)`. Decoding happens once; about 0.8 MB
  of it stays in use (the backdrop and paper composed at half resolution,
  the crops, two font pages), the high-water mark is about 1.7 MB.
  Fallback: archives missing, a file absent (another game revision), a
  decode error, a sheet of an unexpected size or too little memory make
  the skin release everything and the manager draw the `light` theme with
  the status line "Game theme could not be read from the game files —
  using Light"; never an error dialog. The eboot never sees the knob:
  `vita_raw_allocator_gate.py` fails when `MANAGER_UI2_GAME_SKIN` or one of
  the skin units reaches an eboot compile record. Host tests:
  `manager_arch_host_test`, `manager_png_host_test`,
  `manager_bmfont_host_test` and `manager_skin_game_host_test`, the last
  over a synthetic install written by `host_tests/manager_gameskin_fixtures.py`
  (every page in en/ru against `host_tests/manager_skin_game_goldens.txt`,
  `MANAGER_SKIN_GAME_UPDATE=1` rewrites it, `MANAGER_SKIN_FRAMES_DIR=<dir>`
  writes PPM frames; the BMFont/DejaVu baseline; every failure path back to
  `light`, every arena size below the high-water mark, every pread failing).
- Manager trophy import row: `ISAAC_VITA_MANAGER_TROPHY_IMPORT` (default
  OFF; FATAL unless `ISAAC_VITA_MANAGER_UI2` is ON; CMake only, no
  `build_vita.py` flag - trophy builds are configured with `cmake -C`;
  `release/v0.1.1-alpha.cmake` pins it OFF). The UI2 ACHIEVEMENTS page gets
  "Grant trophies from save" / "Выдать трофеи из сейва" with File 1-3 on
  ←/→; × writes the game runtime's request file
  `ux0:data/isaacr001/trophy-import.req` (exactly one digit, through
  `manager_storage_write_text_atomic`, so a replaced request is archived as
  `.manager-backups/trophy-import-NNNN.req`). A game built with
  `ISAAC_VITA_TROPHIES` consumes it at its next start (`TROPHIES.md`, manager
  contract) and writes one line to `trophy-import.res`; the page shows the waiting
  request and that line (`done`, `partial`, `unavailable`, `no-save`, the
  save-format errors), and a missing, garbled or oversized file as such -
  never an error dialog. Repeating a request is safe (the runtime reports
  `unlocked=0 already=<n>`). While FTP listens the row is read-only. No unit
  is added; the code sits in `manager_ui.c` / `manager_strings.h` under
  `#if defined(MANAGER_TROPHY_IMPORT)`. Host tests:
  `manager_ui_trophy_host_test` (parsers, the row over the fake FS, frames
  against `host_tests/manager_ui_trophy_goldens.txt`) and
  `manager_strings_knobs_host_test` (the string table with the row's
  strings).
- Manager debug overlay row: `ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW` (default
  OFF; FATAL unless `ISAAC_VITA_MANAGER_UI2` is ON; CMake only;
  `release/v0.1.1-alpha.cmake` pins it OFF; for debug/test builds - only
  debug builds of the game draw the overlay). The UI2 SETTINGS page gets
  "Debug overlay" / "Отладочный оверлей" (off / compact / full) in the Game
  section after EID, saved with the page's × as the flat `port.ini` key
  `debug_overlay = off|compact|full`. The row goes through the shared
  parser's generic key API (`isaac_vita_settings_key_from_text`,
  `get_text`, `set_text`; pending count, save, UNDO and DEFAULTS then cover
  it like every other key), so it needs `host_vita_settings_parse.c` to
  know the key: an entry `debug_overlay` in the key table and in
  `enum isaac_vita_settings_key` (before `ISAAC_VITA_SETTINGS_KEY_COUNT`),
  a field in `struct isaac_vita_settings`, the values `off`, `compact`,
  `full` accepted by `set_text` and produced by `get_text` / `serialize`,
  default `off` from `isaac_vita_settings_defaults`, the field compared by
  `isaac_vita_settings_equal`, and `ISAAC_VITA_SETTINGS_SERIALIZED_MAX`
  raised by the longest line (`debug_overlay=compact\n`, 22 bytes). The
  manager probes exactly that at start; with a parser that lacks the key the
  row is absent and nothing else changes. No unit is added; the code sits in
  `manager_ui.c` / `manager_ui.h` / `manager_strings.h` under
  `#if defined(MANAGER_DEBUG_OVERLAY_ROW)`. Host test:
  `manager_ui_overlay_host_test` (with today's parser: the row absent; with
  a parser that knows the key: steps, save, reopen, DEFAULTS, FTP read-only).

Two options need the corpus generated with an environment variable set for
the `recomp/gen_all.py` run (`tools/build_vita.py` imports `gen_all`, so set
it before running the wrapper): `GUEST_FLOOR_THUNK_FASTPATH=1` for
`ISAAC_VITA_FLOOR_THUNK_FASTPATH` and `GUEST_LEAF_INLINE=1` for
`ISAAC_VITA_TRANSLATED_CPU_LEAF_INLINE`; SSE lowering is a generation default.
The release corpus is generated with both variables set. The generation
manifest records the switches, and a corpus generated without them compiles to
different code, so keep the two in step with the options.

`ISAAC_VITA_TRANSLATED_CPU_GPR_FLUSH_MASK` (default OFF, requires
`ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL=ON`) needs a corpus generated with
`GUEST_GPR_FLUSH_MASK=1` as well: `recomp/gpr_flush_mask.py` rewrites each
`GUEST_GPR_FLUSH(c);` it can prove partial to `GUEST_GPR_FLUSH_MASK(c, 0xMMU);`
(the registers possibly written since the last sync), and the option makes
guest.h store only those. `guest_0144.c` is never masked (its identity is
pinned for the direct-default derivation). Configure fails if the option is ON
and `guest_0000.c` has no mask. With the option OFF a masked corpus compiles to
the unmasked objects (only the recipe-stamped `guest_table.c`, manifest and
`guest.c` generation symbol differ). `recomp/test_gpr_flush_mask_proof.py`
proves the masks on a generated corpus (Linux gcc; WSL works).

`ISAAC_VITA_ARCHIVE_XOR_FASTPATH` (default OFF, requires the translated KAGE
build and overflow mspace) word-XORs the frozen stored-archive block through
checked allocation leases; it retains the translated PRNG refresh by default.
The separate `ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH` (default OFF, requires
`ISAAC_VITA_ARCHIVE_XOR_FASTPATH=ON`) runs that exact refresh natively only
inside the leased seam. Every refresh first checks all four live guest switch
destinations; a mismatch executes the original translated function. Neither
option changes the decoder, global/thread PRNGs, save PRNG or generated
corpus. The native-refresh define applies only to `host_vita_archive_xor.c`.
`recomp/test_vita_archive_miniz_fastpath.py --xor-only --pe <frozen PE>`
regenerates and executes the original byte loop and refresh, comparing the
entire CPU and arena in ORIGINAL/NATIVE configurations, including unaligned
states, repeated calls, tails, integer wraparound and changed valid switch
destinations.

`ISAAC_VITA_LAUNCH_ARGS_FILE` (default OFF; bench builds only, refused with
`ISAAC_VITA_SCAFFOLD_ONLY`) appends the whitespace-separated tokens of
`ux0:/data/isaacr001/launch-args.txt` (512 B, 8 tokens, `#` comments) to the
guest argv, so `--set-stage=1 --set-stage-type=4` starts a Downpour run with
no menu and no input (`tools/bench/`). Receipt:
`ISAAC VITA LAUNCH ARGS: path=... status=ok|absent|io-error|too-large|too-many|no-slot|bad-slot
io=0x... bytes=N tokens=N slot=N argc=N cmdline=...`. A `--set-stage` run
never visits the save menus, so the game's save slot (Manager+0x10, cleared
by Manager::Init at `004ad7fa`) stays 0, which the mod-data bindings map to
`save1.dat` = File 1. The host-only token `--vita-save-slot=N` (N = 1..3,
never passed to the guest) makes the CRT fopen owner store N there at the
first guest fopen after Manager::Init stored the Game pointer (`004ad841`),
before any mod script, and put it back if it changes:
`ISAAC VITA LAUNCH SAVE SLOT: event=applied|restored slot=N was=0x...
manager=0x... waited|restored=N trigger=<path>`. Fail-safe: `--set-stage=`
without a slot token, a slot outside 1..3 or two slot tokens rejects the whole
file (`status=no-slot` / `bad-slot`, unchanged argv = normal menu boot,
nothing stored). With the option OFF no object changes (the module is not
linked and every CRT line sits under `#if defined(ISAAC_VITA_LAUNCH_ARGS_FILE)`).
Gates: `python recomp/test_vita_launch_args.py --pe <frozen PE>` (instruction
pins of the slot stores, build-gate scan, host oracle with branch coverage, 20
mutations) and `python tools/bench/bench_run_test.py`.

Profiling options (`ISAAC_VITA_PHASE_PROFILE`, `ISAAC_VITA_GL_TIME_PROFILE`,
`ISAAC_VITA_GL_FILL_CENSUS` with `ISAAC_VITA_GL_FILL_CENSUS_DUMP`,
`ISAAC_VITA_GUEST_SAMPLER`, and the `*_RECEIPT` and `*_VERIFY` options) are
for diagnostic builds; the CMake cache keeps them between configures, so
switch them OFF explicitly for a play build. `ISAAC_VITA_COLOROFFSET_FS_PROBE`
(`OFF`, `TRIVIAL` or `NODISCARD`; default `OFF`) is a GPU fill-cost probe that
deliberately draws the ColorOffset sprites with a cheaper fragment shader; the
picture is wrong by design, so it is never part of a play build (see
[`vitagl-stock-reference/README.md`](vitagl-stock-reference/README.md)).
The GL fill census (needs
`ISAAC_VITA_PHASE_PROFILE=ON`; `GL_TIME_PROFILE=ON` recommended so the
`ph120.gx` GPU waits sit next to it) projects every `glDrawElements` through
the shadowed `Position` attribute, `Transform` matrix and viewport and counts
the fill in kpx (1024 px) per program class, blend class, pass ordinal and
FBO attachment; the DUMP option additionally logs one frame's draw list
(`KAGE VITA FILL DUMP` lines) once the render median exceeds
`ISAAC_VITA_GL_FILL_CENSUS_DUMP_RND_US` (default 60000, or at window
`..._DUMP_WIN` when set). Both are OFF by default and compile to nothing.
Census fine print: kpx are rounded per draw (a draw under 512 px counts 0;
the class sums `prog e+o`, `blend a+d+o` and the `ph120.fp` pass kpx each
equal `kpx(c)` exactly); `kpx(clr)` is the level-0 size of the attached
colour texture, or the native 960x544 panel for framebuffer 0 (not the
viewport); clears whose target size the census does not know count 0 kpx and
one `cunk` in `ph120.fp`; the owed depth clears `ISAAC_VITA_FBO_CLEAR_ELISION`
replays natively are counted on the framebuffer that owed them, absorbed and
dropped ones are not (they never reach vitaGL); `big` counts triangles and
`vp n` counts viewport changes. The census measures the native 960x544
display and fails to configure with `ISAAC_VITA_DISPLAY_RASTER` 720/480. The
boot receipt is one `[kage-vita] GL fill census: on records=ph120.fa,ph120.fp
dump=1 rnd_us=60000 min_win=30 win=0 frames=2` line after the `vitaGL ready`
banner; a dump flush writes up to 193 `isaac_vita_log` lines from one report
call (the `ISAAC_VITA_LOG_ASYNC` ring absorbs the burst; without it that
report is a one-shot hitch, at most `..._DUMP_FRAMES` times per launch).
`ISAAC_VITA_GL_TIME_SDK_SPARSE` with `ISAAC_VITA_SHADER_ATTRIB_FASTPATH`
(no extra option; CMake hands `ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB` to
`host_vita_shader_attrib_fastpath.c` and `kage_vita_phase_profile.c` only when
both are ON) adds a fifth `ph120.gt mode=sdk32` row, `scope=attrib`, directly
after `scope=queue`: `res=<residue> replays(en,di)=<all HANDLED
EnableAttribs,DisableAttribs replays of the window> en(n,us,max)=...
di(n,us,max)=... bad=<clock went backwards>`.  It is the sdk32 scheme applied
to the two Shader attribute replays: the ordinal is the kind's HANDLED count,
the residue is the phase-profile window & 31 (re-derived at every take, like
the native sdk32 sites), the replay whose `(ordinal & 31) == residue` reads
`sceKernelGetProcessTimeWide` at `_try` entry and at its `ret 0xc` retirement
(after the last wrapper or batch call, so the bracket covers the decline
checks, the memo, every wrapper/batch and the register retirement), the other
31 pay one load, one compare and one increment; a rejected replay is neither
counted nor timed (its translated body runs).  `n * 32 ~ replays` per kind,
`us / n` is the per-replay cost.  Take-and-zero at the same loop head as the
other sdk32 rows, the pre-window accumulation discarded at the first loop
head; the row is 273 bytes all-ones (pinned by the `[sdk-sparse-attrib]` leg
of `test_kage_vita_phase_profile.py`).  Without the define the replay TU is
byte-identical (no clock, no counters); `ISAAC_VITA_GL_WRAPPER_TIME` already
refuses `GL_TIME_SDK_SPARSE`, so the two replay timers never coexist.
`ISAAC_VITA_GL_WRAPPER_TIME` (OFF; needs `ISAAC_VITA_PHASE_PROFILE`,
`ISAAC_VITA_GL_TIME_PROFILE` and `ISAAC_VITA_GL_SHIM_FASTDISPATCH`, refuses
`GL_TIME_SDK_SPARSE`) adds one `ph120.gw` record directly after `ph120.gt`:
`w(d,c,b,t,s,p,a,u,o)` is the wall time from GL shim wrapper entry (the
fast-dispatch adapter run in `gl_bridge.c`, or the KAGE Present method for
`p`) to its return, on the same `sceKernelGetProcessTimeWide` clock and with
the same bucket assignment as `ph120.gt`, so `w - gt.us` per bucket is the
shim's own cost (argument decode, attribute coalescing, location cache, fill
census, coloroffset staging); `a` (Enable/DisableVertexAttribArray +
VertexAttribPointer) and `u` (glUniform*) have their vitaGL bodies inside
`gt` `s`, `o` is every other registry entry; `n=` are the calls in the same
order; `k(en,di)=us,n/us,n` are the handled `ISAAC_VITA_SHADER_ATTRIB_FASTPATH`
replays of `Shader::EnableAttribs` / `DisableAttribs` (the only host-native
callees the KAGE batch flush `sub_0056d500` reaches through its vtable
slots; zero without that option), `bad=` reversed clock reads. Two clock
reads per wrapper when ON, nothing when OFF.
The same knob emits `ph120.gd` and `ph120.gu` directly after `ph120.gw`.
`gd` splits the glDrawElements wrapper along `vita_glDrawElements`
(`gl_vita_backend.c`), one clock read per boundary and sharing the `gt`
bracket's own reads: `d(disp,sync,cls,cen,fbo,gl,tail)` = bridge entry to
body start (adapter decode, deep-scope enter), `gl_vita_backend_attrib_sync`
(deferred attrib toggles incl. their native bodies), world-seam note +
canonical-quad classify + first-frame probe, `GL_VITA_PHASE_COUNT` + fusion
profile + fill census, `gl_vita_fbo_note_draw` (owed clear materialize) up to
the `gt` BEGIN read, the `gt` draw bracket itself (`gl == gt d` by
construction), and the `gt` END read to the bridge exit read; `n(dr,cq,ea,da)`
= draws, canonical zero-copy hits, attribs replayed by the handled
EnableAttribs / DisableAttribs replays; `en/di(own,loc,gl)` split `k(en,di)`
into the replay's own checks and register retirement, the per-attrib
`glGetAttribLocation` memo, and the per-attrib Enable+Pointer / Disable
wrapper calls (`own + loc + gl == k`).  `gu` `u(m4,4f,2f,1i,o)=us,n/...`
keys the `u` bucket by entry point (glUniformMatrix4fv, glUniform4fv,
glUniform2fv, glUniform1i, other) from the same two reads.  All-ones sizes
379/370/232 bytes.

Two profile-only options measure the render residual the sprite chain does not
explain; both are OFF by default and leave every object byte-identical when
OFF.
`ISAAC_VITA_GUEST_SAMPLER_RESIDUAL` (FATAL unless
`ISAAC_VITA_GUEST_SAMPLER_CHAIN`; defines `ISAAC_VITA_GUEST_SAMPLER_RESIDUAL`
for `kage_vita_guest_sampler.c` and `host_vita_lua.c` only) lets the chain walk
cross the native Lua boundary: a `0xfff15333` callback-return word
(`host_vita_lua.c:71`) is linked to the translated caller's marker 8 bytes
above it (C function) or 20 bytes above it (guest allocator), so samples inside
translated LuaBridge bindings keep their render root.  Only a word at an
address the Lua bridge published as live is linked (the bridge stores the
word's guest address after each push and clears it when the frame ends); a
stale copy left below ESP by a callback that returned is counted (`lbx`) and
skipped.  It adds bit 23 (`+800000`) to `ph120.hotr` keys when the walk crossed
a live boundary at or below the root (on root 0 the bit means "crossed, then
broke", still no root) and four render-phase records:
`ph120.hotw ... w=rva:self/caller/incl/root/root_n/lua_root/lua_root_n` for
every function of `recomp/runtime/kage_vita_render_watch.h` (whatever its
rank), `ph120.hotl ... inv=40eef0 o=outer_root:t/n/lt/ln` (samples rooted in
the mod-callback invoker, by the pass that ran it), `ph120.hotu` (rootless
samples by the outermost function reached, `+1000000` = native) and
`ph120.hcl lb= lbm= lbs= lbx= watch=` (boundaries linked, boundaries whose
caller was not found, samples inside a binding, stale boundary words skipped).  `ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS`
(FATAL unless `ISAAC_VITA_GUEST_SAMPLER`; refuses
`ISAAC_VITA_KAGE_REFCOUNT_SEAM_VERIFY`, which would count 562e00/562ec0
twice) counts translated body entries of the same watch list per sampler
phase.  At configure time `vita_render_watch.py` turns the corpus's
`guest_coverage.json` into `<build>/isaac-generated-overrides/entry-census/guest_entry_census_ids.h`
(a constant-id switch, so a watched hook is one phase load and one increment
and every other hook folds to nothing) and CMake prints `Isaac Vita render
watch entry census: entries=64 ...` (the list is full at 64).  The sampler prints `KAGE VITA ENTRY
CENSUS: ... ids=ok|STALE` at start and `ph120.fe ... phase=<p> e=rva:entries`
per window (upd and rnd always, other phases when non-zero; take-and-zero;
lines split under 384 bytes); a header from another corpus fails to compile
on the function count and prints `ids=STALE` with no `e=` on an RVA-hash
mismatch.  A native seam that replaces a body without replaying its coverage
hook (`ISAAC_VITA_RENDER_CHAIN_NATIVE`) is not counted.  `python
tools/bench/bench_run.py analyze` folds these records into a `render residual
per phase` section (ms/render, calls/render, us/call per watched function);
its `no root (all)` line needs only `ph120.hotr`, so every
`ISAAC_VITA_GUEST_SAMPLER_CHAIN` log gets it, and `compare` prints it per phase.
Gates: `python recomp/vita/test_kage_vita_guest_sampler.py` (contract and
host oracle, scenarios 8, 8c and 9), `python recomp/vita/test_vita_render_watch.py
[--coverage <generated>/guest_coverage.json]` and `python
tools/bench/bench_run_test.py`.

`ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO` (default OFF; FATAL unless
`ISAAC_VITA_SHADER_ATTRIB_FASTPATH`) memoizes the per-attribute
`glGetAttribLocation` answers inside the `Shader::EnableAttribs` /
`DisableAttribs` replays (`host_vita_shader_attrib_fastpath.c`).
The memo is keyed on the backend's program-state generation
(`g_isaac_vita_gl_location_generation` in `gl_vita_backend.c`, bumped by the
same events that invalidate the shim location cache: `glAttachShader`,
`glCompileShader`, `glCreateProgram`, `glDeleteProgram` (name recycling),
`glDeleteShader`, `glLinkProgram`, `glShaderSource`, and every backend
install/uninstall; `glBindAttribLocation` is not in the guest GL surface and
`test_gl_vita_backend.ps1` fails if it ever appears), the Shader object
address, its program word, its attribute table address and count, and every
attribute's name address plus name bytes (compared through the current
string to its NUL).  Any mismatch, an unfilled slot, or a name the shim cache
would not memoize either (empty, >63 bytes, non-printable) takes the
unchanged per-attribute lookup path and refills, so the wrapper call sequence
is the per-attribute one minus exactly the skipped `glGetAttribLocation`
calls (the whole-object hit test lands in `ph120.gd` `own`), and every
decline check of the replay still runs before the memo is consulted.
`ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY` (default OFF; FATAL unless the
memo) still performs the wrapper lookup on every hit, counts a memo/wrapper
disagreement into `ph120.a loc(...,m)` (shared with the shim cache's own
VERIFY count; must stay 0) and uses the looked-up value, so a VERIFY build
issues exactly the unmemoized calls.  MEMO_VERIFY together with
`ISAAC_VITA_GL_LOCATION_CACHE_VERIFY` is FATAL: both count into the same
`ph120.a loc(...,m)` word from two threads (game and render), so a nonzero
value could not be attributed.  Cost: 16 direct-mapped entries, ~20 KiB
of BSS, one generation read and one name-bytes compare per replay.
Threads: under `ISAAC_VITA_GL_THREAD_RECORD` the generation word is written
on the render thread only (inside the recorded FENCE replays and the
synchronous install/uninstall worker calls) and read on the game thread; the
ring's `done_pos` release/acquire pair orders it, and every FENCE ends in
`record_sync()`, so the game thread reads a settled value before its next
replay.  The contract is spelled out at the end of `gl_vita_backend.h`.

`ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES` (STRING, default `16`;
accepted 16, 32, 64, 128; anything but 16 is FATAL without the memo) sets the
memo size.  16 keeps the original `((self >> 4) ^ (self >> 9)) & 15` fold and
compiles byte-identically to a build without the option; any other value
indexes by `(self >> 2) & (N - 1)` (a Shader object is 4-aligned) and is a
compile definition on `host_vita_shader_attrib_fastpath.c` only.  The census
in the test (every `mov dword [imm32], <Shader vtable>` in the frozen PE: 23
static Shader objects) shows why the fold cannot simply be widened: at 16
entries slot 4 holds four shaders (colormod, water_v2, ColorOffset and the
KAGE textured built-in), and at 64 entries the old fold still pairs
ColorOffset with the KAGE textured built-in.  The `(self >> 2)` index leaves
1 extra static occupant at 32 and none at 64 or 128 (heap-constructed Shader
objects are not in the census).  64 entries cost ~77 KiB of BSS.

`recomp/test_vita_shader_attrib_fastpath.py` runs the replay oracle in these
flavours: frozen, memo, memo+VERIFY, memo-64, and `memo-recorder`,
`memo-recorder-verify` and `memo-recorder-64`, which put the real typed
recorder, ring and consumer thread (`gl_record_backend.c`,
`gl_record_ring.c`, `gl_record_locations.c`, worker mode with jitter) in
front of the recording backend.  The recorder flavours prime the producer
location cache before every program-mutator case, and every location the
fake backend receives is checked against its link epoch (a location cached
before a relink must never reach the backend after it).  Four mutants must
FAIL: an async FENCE (the recorder's void program mutators generated as
FIRE), a memo without the generation compare, a memo without the name-bytes
compare, and a recorder whose FENCE bodies do not reset the producer
location cache.  Static pins in the same test: FENCES are the 7 program
mutators and disjoint from FIRE; every FENCE body resets the producer
location cache and ends in `record_sync()`; the backend functions that
invalidate the shim location cache are exactly the 7 FENCE wrappers; the 9
bump sites; install/uninstall go only through `isaac_gl_record_worker_call`,
and the only `isaac_gl_record_worker_post` is the KAGE present closure,
whose direct-call closure over every production source (recursive) reaches
no install, location cache, generation write or program-state wrapper; no
raw vitaGL program mutator outside its own wrapper.  The `coalesce-memo`
mode of `test_gl_vita_backend.ps1` also pins the backend bump sites.

The device timing gate for the memo is `tools/bench/ab_gate.py`, not a
single A-off/A pair: two seeded runs of the same build differ by 2.5 ms of
render time per render in P2, as much as the expected saving.  Bench recipe
A with the memo OFF (A-nomemo) and recipe A with it ON, at least three runs
each, interleaved in one session; the gated metric is render-phase sampler
time per draw, and a phase shows a SAVING only when the difference of the
medians exceeds the larger within-build range.

A bench run counts only when the mods loaded (bench validity gate; host
tools only, no runtime knob).
`bench_run.py analyze` FAILs a run whose `log.txt` has a Lua error (`There
was an error running the lua file`, lauxlib `cannot open/read/reopen`, `EID
ERROR`, a main-chunk `ERR:`), no `External Item Descriptions v... loaded.`
banner, an EID heap line other than the pinned one (`EID_HEAP_PIN`:
`11215 KB and 166 bytes` for v5.23_bc0551a; `--eid-heap-pin KB:BYTES` for a
build that changes Lua memory), an unrecovered `KAGE VITA LUA LOADFILE`
receipt, or a system intercept (`SCE_CTRL_INTERCEPTED`, any input-taking
overlay) other than the kill's: more than one in `native.log`, or one with
more than 5 s of log after it (timed by the first timestamp after it).  A
run that reached `MARK BENCH DONE` with no intercept at all, not even the
kill's, WARNs `INTERCEPT_UNSEEN`: the detector was blind on it.  `ab_gate.py`
re-diagnoses every run from its logs and refuses (exit 2) a set with any
such run, with EID versions that differ, or with EID heaps that differ within
a side.  Runs prepared from now on also need `bench_run.py killrecord` after
the kill (`NO_HARNESS_KILL` otherwise).  It finds the run's own deploy and
kill in the device tool's `audit.jsonl` from the run's audited `RUN/save2.dat` upload
(the first ok deploy after it, so an errored deploy that was retried is
skipped; the last kill before the next deploy).  It does not take a later
bench run's deploy or kill: an upload of any other file to the
repro_console `save2.dat` after this run's upload and before the kill is
refused (another run's, or a restore of a backup; also an upload that
errored, since the device tool can report an error after the new file is in place;
EID's `save2.dat` does not count), also when this run's own deploy
errored and was never retried (the first ok deploy is then the later
run's), and a later run's deploy ends the window.  It does take a
non-bench launch and kill of the title that falls after this run's kill
and before the next deploy; the time check then decides (`NO_HARNESS_KILL`
unless the time from the launch to that kill matches the length of
`native.log` within 2 s), and the nonce decides whether the downloaded logs
are this run's.  Of two runs whose `save2.dat` uploads both precede one
deploy, the earlier is refused (its `save2.dat` was overwritten before any
deploy) and the later binds to that deploy and kill.  A deploy that errors
must be repeated until it is ok before the launch.  A run prepared without
`--eboot` is not checked against the eboot its deploy installed, so an ok
non-bench deploy after an errored deploy of its own can be taken as its
deploy; `prepare` warns, and device runs are prepared with `--eboot`
(manifests from before the gate have no `kill_record` and are never
checked).  When a kill reports an error, check first whether the app is
still running and repeat the kill only while it is: a kill that errored but
stopped the app stays the run's kill and FAILs `NO_HARNESS_KILL` on its
status, where a retry would be bound instead and FAIL on its time; either
way the run is repeated.

The bench list (`tools/bench/bench-save.txt`) runs P1-P5 on Downpour I and
then the dry phases on Basement I of the same start seed and item build:
`MARK DRY START` and `@room 3` end P5 with the command
and room that ended it before, `stage 1` builds Basement I, and P6
dry-start-idle, P7 dry-fight-maxroom and P8 dry-spawn-heavy repeat P5, P2
and P3 without water.  Every `MARK` line ends with `backdrop=<n> water=?
haswater=<bool>` (`Room:GetBackdropType` and `Room:HasWater` of 1.7.9b;
`Room:GetWaterAmount` is REPENTOGON's and is never called; `?` = a binding
absent or failing).  `analyze` takes the expected phases from the run's
manifest (P1-P5 for older manifests; without a manifest list, P1-P8 when the
log has `MARK DRY START`), checks the Downpour floor (`SEED`) only against a
`Level::Init` before `MARK DRY START` and the Basement floor against the
start seed's stage seed (`SEED_DRY`), WARNs `DRY_PHASE_NOT_DRY` /
`DRY_PHASE_UNPROVEN` from the marker that closes each dry phase, WARNs
`DRY_ITEMS_CHANGED` / `DRY_ITEMS_UNPROVEN` unless the census's `familiars`
on the dry floor (P6-P8 and BENCH DONE) equals `MARK BENCH START`'s (the
item build survived `stage 1`), and prints mean presents, renders and
updates per second per phase.  The dry phases are information in every
decision set (R1.4, R8.2, R8.5, R8.9, `ab_gate.py`, the device-gate
scripts).  The lines that sum over every phase window (floor debt R1.2,
R1.3, R8.3, R8.7, and the R1.6b emit cost) do cover them, as they cover P1,
P4 and P5.  And a run that crashes or stalls in the dry section is not BENCH
OK (`INCOMPLETE` / `MISSING_PHASES`), so `ab_gate.py` and the device gates
lose its P2/P3 too.  The list runs about 65 s longer, and the
procedure's second sleep is 230 s instead of 150 s.

`ISAAC_VITA_GL_RECORD_ATTRIB_BATCH` (default OFF; FATAL unless
`ISAAC_VITA_GL_THREAD_RECORD`, `ISAAC_VITA_GL_THREAD_WORKER`,
`ISAAC_VITA_SHADER_ATTRIB_FASTPATH` and `ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO`
are ON; FATAL with `ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY` or
`ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE`; `GL_WRAPPER_TIME` and `STALL_PROBE`
are already refused under the worker) makes a `Shader::EnableAttribs` /
`DisableAttribs` replay that hits the location memo, while the installed GL
table's lookup/toggle/pointer members are the recorder's own, publish ONE
ring record (opcode 79 `REC_ATTRIBS_ENABLE` / 80 `REC_ATTRIBS_DISABLE`,
appended after `REC_WORKER_CALL` in `gl_record_backend.c`; the generated
surface is untouched) instead of a `glEnableVertexAttribArray` +
`glVertexAttribPointer` record per attribute (Disable: a
`glDisableVertexAttribArray` record per attribute).  The producer applies
exactly the attribute-shadow notes the per-call records would, in the same
order (the snapshot-draw sizing reads them); the render thread unpacks the
record into the same `s_target` wrapper calls with the same arguments in the
same order, updating its own shadow before each call as `record_replay` does
per record.  The guest-visible retire (`[ebp+8]`, EAX, ECX, ESP, the
ownership word) is the per-call one.  A memo miss, a decline or a foreign
table keeps the per-call records.  Payload: Enable `{count, stride, base,
loc[count], components[count] zero-padded to 4}` (48 bytes for 7
attributes), Disable `{count, loc[count]}`.  A one-shot
`KAGE VITA GL RECORD ATTRIB BATCH: first enable count= loc[0]= loc[last]=
stride= op=79,80` line, printed by the replay TU on its first batched
Enable, proves the path is live.  Compile definition on
`host_vita_shader_attrib_fastpath.c` and `gl_record_backend.c` only (plus
`kage_vita_phase_profile.c` when `ISAAC_VITA_GL_RECORD_OPCODE_CENSUS` is ON,
which sizes the census from `ISAAC_GL_RECORD_OPCODE_SLOTS`, 79 -> 81).
OFF: the preprocessed text of every touched unit is unchanged, so the
objects compare equal apart from the embedded build id.  Receipt deltas that are accounting, not
behaviour: `ph120.rt r`/`hw`/`fr`, `ph120.rw cb`/`ap`, census opcodes
23/26/72 fall to the memo-miss residue and 79/80 appear; compare A/B per
draw, never raw records.  Gates: `test_vita_shader_attrib_fastpath.py`
(flavours `record-batch-memo`, `record-batch-decline`,
`memo-recorder-batch`, `memo-recorder-batch-64`, the attribute-batch option checks under
`cmake -P`, and the producer/recorder must-FAIL mutants) and
`test_gl_vita_backend.ps1` with `ISAAC_GL_RECORD=1` and
`ISAAC_GL_RECORD_ATTRIB_BATCH=1` (the scenario's batch section, every
recorder mode, SHA-equal traces against the per-call direct variant),
`test_gl_record_opcode_census.py` (oracle `gl-worker-census-batch`: 81 census
slots in the same 27 `ph120.rop` records) and `tools/bench/bench_run_test.py`
(`bench_run.py` names 79/80 `ATTRIBS_ENABLE` / `ATTRIBS_DISABLE` from the
backend's enum).

`ISAAC_VITA_SHADER_SAMPLER_FASTPATH` (default OFF) link-wraps the frozen
`Shader::SetUniformInt` root `sub_00567430`. A bounded eligible call replays
the same ActiveTexture / optional BindTexture / GetUniformLocation /
Uniform1i calls, including the original secondary-texture/no-bind branches,
through the actual Vita wrappers and returns with the original ABI. It drops
three/four dynamic GL adapter crossings and synthetic stack/GPR glue, not GL
work. There is no sampler value/location/program/texture cache or shader math
change. Unknown/foreign/partial/busy backend, source shape, token or stack
conditions take the exact translated fallback. Requires KAGE,
GL_SHIM_FASTDISPATCH and SHADER_ATTRIB_FASTPATH; UNIFORM_SET_RECEIPT (same
wrapped root), GUEST_SAMPLER and STALL_PROBE must be OFF. The original generator
and shader-cache recipe are unchanged. `test_shader_sampler_fastpath.py --pe`
regenerates and executes the owned original body in both GPR/stack-guard modes;
host proof is not a device speed/image pass. Verify the final ARM guest-table
entry for RVA0x567430 targets the wrapper, not only that its symbol exists.

`ISAAC_VITA_KAGE_BIND_PACKET` (default OFF; bind packet
stage 1) link-wraps frozen KAGE `Shader::Bind` (`sub_005671d0`, reached only
through the Shader vtable, so the corpus is unchanged).  When the uniform
record list is empty - the shape the game binds - the seam
(`runtime/host_vita_kage_bind_packet.c`) replays Bind, its two
`Shader::SetUniformInt` calls ("Texture0"/"Texture1"), `Image::IsEmpty` and
`RenderManager::Transform` (the "Transform" matrix, transcribed statement by
statement from the corpus on the CPU's xmm registers) natively and stages
every GL call into one typed-recorder record, `REC_KAGE_BIND_PACKET` (opcode
81, `gl_record_backend.c`), instead of 6..14 guest_call crossings, adapters
and ring records.  The render thread replays the staged calls one by one
through the ordinary replay with each call's original return RVA, so vitaGL
sees the same calls, arguments, name and matrix bytes, order and RVAs.  A
uniform location is staged when a 4-slot memo (keyed on location-cache
epoch, program, name address and name bytes) or the producer location cache
answers; a cache miss flushes the packet and takes the ordinary synchronous
lookup, so the cache transitions are the translated ones.  Exit state is the
translated `ret 4`'s (EAX/ECX/EDX, ESP, xmm0..7, flags, sampler word, stack
low water, body-entry census); the dead stack below the final ESP is not
written.  Any unproven input (stack depth, objects on the stack, foreign
vtable/IsEmpty/getter, non-empty record list, slot tokens, an unrecorded or
foreign backend table, GL ownership) declines to the translated body before
the first side effect.  FATAL unless KAGE, GL_SHIM_FASTDISPATCH,
SHADER_ATTRIB_FASTPATH, GL_THREAD_RECORD and GL_THREAD_WORKER are ON;
UNIFORM_SET_RECEIPT (wraps the same root) and STALL_PROBE must be OFF.  The
define reaches only the seam TU and `gl_record_backend.c` (and
`kage_vita_phase_profile.c` with `ISAAC_VITA_GL_RECORD_OPCODE_CENSUS`, whose
census then has 82 slots); with the option OFF every object is unchanged.
Receipts that change by design with it ON: `ph120.d` / dispatch and dynamic
(`ik dyn`) counts drop by the bypassed crossings, ring record counts drop,
the opcode census shows slot 81 (one per packet), and the boot log prints
`KAGE VITA BIND PACKET: binds=... handled=... declined=... (frame= object=
records= image= target= tokens= recorder= owner=) cold=... packets=...
staged=... memo=... cache=... miss=...` at 1, 2, 4, ... 4096 binds and then
every 4096.  `test_vita_kage_bind_packet.py` pins the frozen PE bytes, the
corpus bodies and every seam constant, and runs the differential oracle
`runtime/host_vita_kage_bind_packet_oracle.c` (MSVC x86, the real recorder
and consumer thread, both GPR spellings, stack-guard / entry-census /
leaf-call / sampler / opcode-census flavours) plus must-FAIL mutants;
`test_gl_record_opcode_census.py` covers the census bound.

`ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE` (default OFF; FATAL unless
`ISAAC_VITA_SHADER_ATTRIB_FASTPATH`; FATAL with `ISAAC_VITA_GL_WRAPPER_TIME`
or `ISAAC_VITA_STALL_PROBE`, whose per-attribute split and breadcrumbs
describe crossings this path does not make) makes the `Shader::EnableAttribs`
/ `DisableAttribs` replays hand their whole attribute set to the typed
backend in one native call (`gl_vita_backend_attribs_replay_enable` /
`_disable` in `gl_vita_backend.c`) instead of calling the
`glEnableVertexAttribArray` + `glVertexAttribPointer` (Enable) or
`glDisableVertexAttribArray` (Disable) wrappers once per attribute through
the backend table.  What the per-call wrappers do (`gl_vita_backend.c`
`vita_glEnableVertexAttribArray`, `vita_glVertexAttribPointer`,
`vita_glDisableVertexAttribArray`): fill-census note (`GL_FILL_CENSUS`
only), the typed-state check (`index < 16`; enable/disable: hit if the bit is
known and already in that state, else the coalescer defers a known bit and
only an unknown bit reaches vitaGL; pointer: `size 1..4`, `stride >= 0`, type
in the float/short/byte set, hit if the recorded tuple is equal, else vitaGL
`glVertexAttribPointer` immediately - the KAGE flush changes the base every
draw, so the pointer calls do reach vitaGL), the phase counter and, in the
FULL GL-time build, the `state` bracket around the native call.  The batch
runs exactly those statements per attribute in the per-call order (lookup -
through `vita_glGetAttribLocation` when the replay has no memo hit, VERIFY
included - then enable then pointer; or lookup then disable) with only the
redundant work removed: the table-indirect call and its sampler
publish/restore pair become direct calls that publish the same tokens at the
same positions, and the pointer wrapper's validation is reduced to
`index < 16 && stride >= 0` because the components come from the frozen 1..4
table and the type is the bodies' constant `GL_FLOAT` (the batch re-checks
both once and declines otherwise).  vitaGL sees the same calls with the same
arguments in the same order for any interleaving with other GL calls; every
backend-only shadow (typed state, coalescer masks, fill census, location
cache) is written by the same statements in the same order; `ph120.c
gl(...)`, `ph120.a loc(...)`, `ph120.y coal(...)` and every other counter
stay identical.  The batch returns 0 with no side effect when the
installed table's members are not the Vita backend's own wrappers (a partial
bring-up table, an oracle recording backend), when count exceeds 16, a
component is outside 1..4 or a NULL argument is passed, and the replay then
runs its unchanged per-call loop; a replay decline never reaches the batch
(every check runs first).  The `direct-plain`, `typed-direct`,
`coalesce-direct`, `coalesce-direct-loc` and `coalesce-direct-fill` modes of
`test_gl_vita_backend.ps1` drive one scripted replay sequence through
the per-call wrappers and through the batch from a fresh install, with
locations supplied (memo hit) and with names looked up (memo miss/OFF/
VERIFY), and require the fake-vitaGL call trace, the typed-state shadow
bytes, the coalescer's pending count, the native mask and every phase
counter to be equal, plus the declines to leave all of them untouched;
`recomp/test_vita_shader_attrib_fastpath.py` runs the replay oracle in the
`direct`, `direct-decline` (batch forced to decline: per-call fallback),
`direct-memo` and `direct-memo-verify` flavours against the translated
reference bodies with the batch recorded as its per-call stream.

`ISAAC_VITA_KAGE_QUAD_FASTPATH` (default OFF; FATAL unless
`ISAAC_VITA_TRANSLATED_CPU` and `ISAAC_VITA_FLOOR_THUNK_FASTPATH`) replaces
the translated body of `Image::PushQuad` (`sub_0055f990`, 491 instructions,
with its callees `sub_0055ee80` ring reserve, `sub_00561e20` attribute size
switch and `sub_0055e590`) with a native host replay
(`host_vita_kage_quad_fastpath.c`) installed at the root with the attrib seam
shape - `GUEST_GPR_FLUSH; if (isaac_vita_kage_quad_try(c)) return;
GUEST_GPR_RELOAD` - so the exact translated body still follows every decline.
What the replay does, in the x86 order: reads the six stdcall
arguments and `this`, calls the render target's `GetWidth`/`GetHeight`
getters through `guest_call` exactly like the `call eax` sites (return words
0x55f9f5/0x55fa1e pushed at frame-4, `edx`/`esi`/`edi`/`ebp` in the values
the x86 has at that point, `esp` checked back to the frame), runs the two
cull chains with `minss`/`maxss`/`comiss` semantics spelled as the generated
C (`(float)((double)a < (double)b ? a : b)` operand order, unordered
compares) and takes the cull exit (`eax=0`, `ecx`=y0 bits, `edx`=colour[1],
`esp += 28` after `ret 0x18`), otherwise copies the quad, snaps y3,x3,y0,x0
through four floor imports accounted exactly like the floor thunk fast path
(`NOTE_CALL`, coverage import 313, `NOTE_IMPORT`, `g_host_import_calls`,
sampler slot publish/restore, stack low-water note, `(float)floor((double)t)`
with `t = (float)(s*v); t = (float)(t + 0.5f); r/s`), reserves six indices
then four vertices with the ring element sizes as the x86 (a record with
room: `count` word and pointer arithmetic natively, `(u16)base` wrap
included; a record that must grow, fresh `{0,0,0,0}` records included: the
translated `sub_0055ee80` entered by direct call with the site's registers,
argument slot and return word 0x55fd59/0x55fd6c, so the allocator vtable
calls, memcpy and free are the translated ones), stores the six u16 indices
in the x86 order (0,2,8,4,6,10), fills the four vertices attribute by
attribute (format 5 position + depth, 7 uv scaled by `[this+8]`/`[this+0xc]`,
6 colour with or without premultiplication, other formats as 16-byte copies
from the colour rows), steps the depth cursor, pushes the image onto the
dirty vector (at capacity: `[F+0x34] = this`, then the translated
`sub_00026fb0` by direct call with return word 0x5601f3) and rewrites the
flags shadow (`|= 4`).  An image without the batched flag (0x20) takes the
same path with the x86's two extra legs: before the snap the alpha sum
`((a0+a1)+a2)+a3 < 4.0` (`comiss`/`seta`) selects `bool` and the translated
blend/batch select `sub_005607a0` (blend descriptor at 0x7c7a34.. and
SetBlend through the Renderer vtable, group pointer 0x7c7a44, pair vector
0x7e7d28.., rebind words, `[this+0x60/0x6c/0x70/0x71]`) is entered by direct
call with `eax=bool ecx=esi=this edx=colour[1] edi=quad ebp=entry ESP-4`,
`[F-4]=bool` and return word 0x55fb74 - its effects (snap scale, batch,
records) are read back afterwards exactly where the x86 reads them - and
after the dirty push the tail re-reads the flag, calls the translated
`sub_00561ff0(0x7c7a30, 0)` when `[this+0x70]` is set (return word
0x560218) and performs the five resets (`[this+0x60]=0`, `[this+0x64/0x68]`
from 0x802fec/0x802ff0, `[this+0x6c]=0`, `[this+0x70]=0`).  It then retires
`eax`=vertex pointer, `ecx`/`edx` as the last attribute left them
(`(u16)base+1` / count) or as the last tail callee left them, callee-saved
registers and `esp` identical to the translated body (the xmm/x87 scratch
the body uses is volatile at the stdcall boundary and is not retired; the
x87 stack is left balanced, as the oracle checks).  Every guest memory
write, register retirement, import receipt and getter result is therefore
bit-identical to the translated body; float math uses the generated
spellings (no reassociation, no FMA).  The direct calls are census-identical
to the body's `call rel32` sites: a translated direct edge publishes nothing
(no dispatcher lookup, no `NOTE_CALL`, no sampler token; the callee's own
`guest_coverage_function` and the sampler's PC attribution are what they
were) and the callee's `ret N` is checked back to the frame (`guest_fault`
otherwise).  It declines (returns 0 with zero side effects, and the
translated body runs) on: stack frame not provable (`guest_stack_fast_bound`,
28-byte argument frame and the 0x160-byte local frame inside the bound; `f`),
a null `this`/quad/format table/uv/colour pointer the x86 dereferences (`oa`;
for an unbatched image all four colours, the alpha sum reads them), a
batched image's null batch or ring records (`ob`), a ring buffer the x86
stores through being null or of element size 0 (`on`), attribute count > 16
(`c`), a format outside 1..8 (`a`), a format-size sum that does not match
the image stride (`s`), the floor route not authenticated
(`g_isaac_vita_import_ids_ready` clear or the IAT slot 0x606524 rebound,
`g`; checked for every handled quad, since the snap predicate is only known
after the select), or a null render-target vtable / getter slot (`t`).
Every decline is taken before the first side effect; the states only the
select can produce (a null batch, hollow fresh records) fault with a message
after it, where the x86 takes an access violation.
`ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE` (default OFF; FATAL unless the fast
path) runs every check and counts the verdicts but never replays, so the
translated body executes unchanged.  Both modes emit `ph120.kq bid= win=
loops= mode=1|2 n= h= c(x,k,g,s)= p(b,t,f,r,d)= d(f,oa,ob,on,c,a,s,g,t)=`
(378 bytes at all-ones; take-and-zero at the window boundary, discarded once
at the first loop head): `n` entries, `h` handled (OBSERVE: would have been
replayed), `c(x,k,g,s)` the handled quads that took the cull exit / were
culled against the constant 480x270 / called the getters / snapped (OBSERVE:
candidates before the cull), `p(b,t,f,r,d)` the handled quads without the
batched flag that entered the select / of those translucent (`bool` 1) /
fresh ring records met / ring reserves delegated to the translated
Ring::Alloc / dirty pushes delegated to the translated growth (OBSERVE: b/t
candidates, f/r the entry snapshot of batched images, d the prediction of
the select's `[this+0x71]` write), and the nine decline reasons in the order
above.  Compile definitions are scoped to
the owning generated unit, `host_vita_kage_quad_fastpath.c` and
`kage_vita_phase_profile.c`; with the define absent the generated unit (its
only change is a guarded `extern` and the guarded seam; `guest_0166.c` is the
only unit whose text changes) and the phase profiler compile to
byte-identical sections, relocations and symbol tables.
`recomp/test_vita_kage_quad_fastpath.py` pins the PE, the
root/callee/tail-callee bodies and the 32-byte size table by hash, the rel32
callers, the call/`call eax`/`ret 0x18` sites, every frozen constant and the
three blend descriptors, extracts the ten bodies from the generated corpus,
and runs `host_vita_kage_quad_fastpath_oracle.c` (the translated reference
bodies vs the seamed bodies, in both GPR spellings) over hand cases, 160 LCG
cases and every decline reason, comparing the call/import trace, registers,
frame words, globals, arena and grown bytes, probing the null-pointer
declines and the post-select faults, plus the CMake option defaults, FATAL
gates and single-owner census.

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
- `ISAAC_VITA_STOCK_FBO_VALID_REGION=OBSERVE|ON` (default OFF; requires
  `ISAAC_VITA_GL_TIME_PROFILE=ON`) applies vitaGL patch 0009: the game's
  offscreen attachments are power-of-two textures (1024x1024 for the 960x540
  surface) and stock vitaGL begins every FBO scene with a NULL
  `SceGxmValidRegion`, so the padding is cleared, drawn and stored on every
  offscreen pass. OBSERVE only counts such scenes and the guest viewport
  violations ON would clip (`ph120.gx vr(...)`, `ph120.vz`); ON passes the
  logical rect (the guest's last `glViewport`, fail-closed to NULL when stale
  or oversized) to `sceGxmBeginScene` and clamps the tile-clipper region to
  it, so the pow2 padding is never written again and edge sampling (the 2x
  bilinear upscale, blur, pixelation, sprites crossing the surface edge) can
  change by up to one texel. The "vitaGL ready" banner prints
  `fbo-valid-region=observe|on`. OBSERVE first (`v == 0`), then ON (see
  [`vitagl-stock-reference/README.md`](vitagl-stock-reference/README.md)).
- `ISAAC_VITA_VITAGL_INTENSITY_TEXTURE` (default OFF; FATAL without
  `ISAAC_VITA_KAGE` and `ISAAC_VITA_VITAGL_STOCK_REFERENCE`) applies vitaGL
  patch 0037 for issue #4 (Chinese text). Stock vitaGL stores every RGBA page
  at 4 bytes per texel, and the eight Chinese font pages take 113 MiB that way;
  when a page does not fit, vitaGL drops it silently (no GL error), which is the
  missing floor-name glyphs and the black save-card boxes. With the knob ON, a
  level-0 `GL_RGBA`/`GL_UNSIGNED_BYTE` definition of exactly 512x512,
  2048x1024, 2048x2048 or 4096x2048 whose every texel (transparent ones
  included) has R=G=B is stored losslessly as `U8_RRRR` (alpha equal too) or
  `A8L8` (independent alpha): 113 MiB becomes 28.25 MiB (linear premultiply)
  or 48.25 MiB (gamma), and the 4096x2048 `UpheavalExtended_1` page 32 -> 8 MiB
  (host-reconstructed GPU payload, not process RAM). Pixel content, not a file
  name, decides; a coloured texel, another shape or mipmaps keep the stock RGBA
  path, and so does a texture object that already had an ordinary definition,
  a sub-image update, a non-filter/wrap parameter or an FBO attachment. That
  history belongs to the GL texture name: `glGenTextures` clears it when it
  hands the slot out again. This matters because vitaGL always reuses the
  lowest free slot, so after the Options language switch deletes the old
  textures the Chinese pages land in slots that held other textures. A
  sub-image update, an unsupported parameter or an FBO attachment of a compact
  page first expands it back to RGBA, transactionally (an allocation failure
  keeps the compact image and sets `GL_OUT_OF_MEMORY`). The 113 MiB is the
  full RGBA set, which was never resident on the device (pages that did not
  fit were dropped): against the measured cold-start residency (65 MiB, six
  2048x2048 pages) the gain is about -29 MiB with `U8_RRRR` and about -1 MiB
  with `A8L8`, and with `A8L8` a language switch (54 MiB free in the int2j
  log) can still end in `phase=upload-failed`. The premultiply mode decides
  which storage a page gets; read it from `storage=` on the 2048x1024 and
  2048x2048 lines. Configure prints
  `intensity texture=on (0037-isaac-intensity-texture.patch)`. vitaGL prints
  its lines with `sceClibPrintf`, so they reach only the Cat-A-Log tty stream
  (`vita_debug_log_tail` or the listener), never `native.log`: one
  `Isaac intensity: armed patch=0037 ...` on the first texture definition,
  then one `Isaac intensity: phase=upload size=WxH ...
  storage=U8_RRRR|A8L8 ... status=valid shadow=0` per compacted page.
  `phase=reject size=WxH pixel=i rgba=r,g,b,a n=k/32` marks a coloured page of
  an admitted shape, and `phase=skip size=WxH slot=N
  reason=excluded|attached|overridden|mips|layout|live n=k/32` an admitted
  page refused because of its texture object's own state. Both are capped at
  32 lines per shape. `phase=deopt`, `upload-failed`, `deopt-failed` and
  `redefine-rejected` report the other outcomes. The tty can drop lines, so
  the same events are also counted in the archive (every event, including
  those past the cap) and `gl_vita_backend.c`, which sees the knob as its only
  runtime TU, copies the counters into `native.log` after each
  `glTexImage2D`/`glTexSubImage2D`/`glTexParameteri`/`glFramebufferTexture2D`
  whose call moved them: one `KAGE VITA INTENSITY: armed patch=0037
  order=512x512/2048x1024/2048x2048/4096x2048 words=25`, then lines like
  `KAGE VITA INTENSITY: gen=N last=upload:2048x2048:A8L8 up=a/b/c/d
  fail=a/b/c/d skip=a/b/c/d rej=a/b/c/d u8=N a8l8=N deopt=N dfail=N redef=N
  live=pages/bytes` (per-shape counts in the `order=` order; `gen` = tty lines
  printed so far; `live` = compact storage held now). A run whose stream and
  `native.log` both lack the armed line is not the ON archive and counts for
  nothing. OFF neither applies the patch nor copies the header, and
  `gl_vita_backend.c` compiles none of the reader (five guarded blocks; host
  MSVC `/EP` tokens and object identical). Keep `ISAAC_VITA_TEXEL_SCRATCH_32M=ON`: this
  shrinks GPU storage, not the decoder's RGBA source scratch. Host gate:
  `python recomp/vita/test_intensity_texture.py --stock-tar <vitaGL 73dd57a tar>
  --clang <clang> [--sanitize] [--temp-dir <dir>]` (plus
  `--font-archive/--font-census/--pe` for the 16 reconstructed real pages, and
  `--before-patch <another line's 0036>` to run the oracle on that chain too);
  `test_vitagl_stock_patch_chain.py` proves the patch placement on all three
  chains. GXM filtering precision on hardware is a device check.
- With it OFF the build links the older pinned overlay from `vitagl-overlay/`:
  upstream vitaGL `f4b23b61c84e8ba59de542832f8e507b3660f994` built by its
  `build.sh` with `SOFTFP_ABI=1 NO_DEBUG=1 NO_SPLASHSCREEN=1
  SINGLE_THREADED_GC=1 SHARED_RENDERTARGETS=1` (no garbage-collector thread,
  small FBO render targets shared); its `test.sh` checks the archive's EABI5
  softfp attributes, symbol set and a GLSL/FBO link probe. Neither touches the
  SDK copy.
- `ISAAC_VITA_RENDER_SURFACE_NATIVE`: the world is drawn at its native 480x270
  and upscaled once. `ISAAC_VITA_DISPLAY_RASTER` is 960 (native); 720 (720x408)
  and 480 (480x272) exist but are not the supported mode.
- `ISAAC_VITA_DISPLAY_CENTER_Y` (default OFF; FATAL without translated KAGE
  or with a scaled `ISAAC_VITA_DISPLAY_RASTER`; GitHub issue #5 item 5.8b).
  KAGE centres the 960x540 image in the 960x544 panel only at init
  (`glViewport(0, KAGE_VITA_VIEWPORT_Y=2, ...)`); the game's own
  default-framebuffer `glViewport(0, 0, 960, 540)` replaces that, so on
  device rows 0-3 are black and the picture sits in rows 4-543. ON shadows
  the guest viewport in `gl_vita_backend.c` (the logical-viewport machinery
  of the scaled rasters: replayed when the draw framebuffer changes or the
  bound one is deleted, `glGetIntegerv(GL_VIEWPORT)` answered with the
  guest's values) and adds 2 rows to framebuffer 0's native Y origin only
  (saturating at `INT32_MAX`); offscreen viewports, X, width and height pass
  through exactly. Content then sits in rows 2-541 with two black rows top
  and bottom. Only that object receives the define; OFF leaves it
  byte-identical. Receipt, once per boot at the first default-framebuffer
  replay after install: `KAGE VITA DISPLAY CENTER Y: rows=2 fb0
  guest=0,0,960,540 native=0,2,960,540`. It does not clip: with
  `render_scale=2` the world still spills past the HUD into the panel's
  outer rows (now 2 at the top, up to 2 at the bottom). Host oracle modes
  `center-y`, `typed-center-y`, `prod-elision-drop-readback-center-y` and
  `prod-elision-drop-depth-carry-center-y` in `test_gl_vita_backend.ps1`
  (and the ARM compile in `.sh`). Only the viewport origin moves, which is
  exact while the guest names window rows no other way: both scripts fail
  closed if the guest GL surface gains `glScissor*`, `glBlit*Framebuffer` or
  `glCopyPixels` (`glCopyTex*` is already refused) or loses `glReadPixels`.
  The guest does call `glReadPixels`; a read of framebuffer 0 would get
  panel rows, 2 below the guest's. Device logs so far show none
  (`KAGE VITA FBO READBACK: ... display=0` with `ISAAC_VITA_FBO_READBACK_FIX`).
- `ISAAC_VITA_RENDER_SURFACE_RASTER_432` (experimental, default OFF) requires
  translated KAGE, `DIRECT_DEFAULT`, `RENDER_SURFACE_NATIVE` and
  `TEXTURE_ALIGN8_POLICY`. The exact outer Render Surface factory at
  `004acba8` still allocates original 480x272 storage from unchanged 480x270
  inputs. Before its result is published to retained owner `987fed7c`, the
  runtime wrapper checks the exact caller, live name, virtual dimensions,
  stack/output contract, original object layout and exact-base heap lease.
  It changes only logical size to **432x242** and UV to 432/480,242/272.
  Every validated recreation is treated, with no consumed-argument restore.
  The existing Light416 wrapper dispatch is shared, not duplicated; either
  option can be enabled independently, or both together.
  Projection, input coordinates, alpha/blend, texture ID, physical backing,
  auxiliary surfaces, display mode and generated corpus are unchanged. The
  image's original per-target pixel snapping follows the smaller viewport.
  **Game HUD is inside this capture and loses raster sharpness too**; other
  content using this outer capture is also affected. Full-target
  clear/storage costs do not shrink. The logical viewport contains 19.33%
  fewer pixels.
  `KAGE Render surface raster: applied ... logical=432x242 backing=480x272
  ... storage=unchanged hud=raster-scaled` is emitted once, after successful
  lease release; an exact-owner rejection logs one `not-applied reason=...`.
  Unrelated calls are silent and original. A lease-release failure after
  mutation is terminal and logs `applied-but-lease-release-failed`. Arbitrary
  scripts reading internal Render Surface pixels are not promised old logical
  size.
- `ISAAC_VITA_LIGHT_SURFACE_RASTER_416` (experimental, default OFF; requires
  KAGE and `ISAAC_VITA_TEXTURE_ALIGN8_POLICY`) keeps the exact Light Overlay
  Surface factory's original 480x270 inputs and real **480x272 backing**.
  After the original factory returns, an exact-base heap lease and original
  layout/UV checks admit four word writes: logical size 416x234 and UV extent
  416/480,234/272. The texture ID, allocation, stride and padded dimensions
  are unchanged. Keeping the backing equal to Shadow avoids introducing
  size-change RT/depth retirement on the shared manager FBO.
  Its screen-space projection and explicit full-size composite stay unchanged;
  Shadow, the world target, GUI, scanout and native texture lifetime are not
  modified. This runtime-only GNU `--wrap=sub_0056cf30` seam does
  not regenerate guest objects or introduce native texture virtualization.
  Admission checks the live name bytes, call frame, globals and align8 byte
  before the original call. Unknown postconditions leave the original object
  unchanged and emit one `not-applied reason=...`; no path retries the factory
  or restores its consumed arguments. Every new creation is checked and
  modified, while `KAGE Light surface raster: applied ... logical=416x234
  backing=480x272 ... storage=unchanged` is logged only once after successful
  lease release. A release failure after writes is terminal and explicitly
  reports `applied-but-lease-release-failed`.
  The reduced logical viewport has 24.89% fewer pixels for this effect;
  physical allocation and full-target clear size do **not** shrink. Unexpected
  scripts reading internal Light pixels are not promised the old resolution.
- `ISAAC_VITA_VITAGL_SHADER_CACHE=ON` stores compiled shaders under
  `ux0:data/isaacr001/shader-cache`. `ISAAC_VITA_CLOCK_BOOST` sets the
  CPU/bus/GPU/XBAR clocks to 444/222/222/166 MHz at startup.
- The loading screen shows the Specialist Dance animation when
  `ISAAC_VITA_LOADING_SPECIALIST=ON` (frames in
  `recomp/runtime/kage_vita_loading_specialist.inc`; `tools/build_vita.py`
  passes it by default). OFF draws a procedural fallback.

## Audio

OpenAL Soft 1.19.1 is built as a project-local static archive by
`openal-overlay/build.sh` (upstream `openal-soft-1.19.1` plus isage's Vita
backend patch, both pinned by SHA-256, never overwriting the SDK copy) with two
patches: `0001-limit-initial-voices.patch` bounds the 256-voice initial
reservation (8.13 MiB) by `sources = 80` (about 2.5 MiB), and
`0002-route-direct-allocators.patch` routes every direct allocator call in the
library into the `ISAAC_VITA_OPENAL_POOL` pool. The archive is built
reproducibly and `verify_deterministic_archive.py` checks it after every
build. `alsoft.conf`
(`sources = 80`) is installed as `app0:/alsoft.conf` when `ISAAC_VITA_AUDIO=ON`.
`ISAAC_VITA_AUDIO_WORKER` pins the mixer thread to CPU 2.
`ISAAC_VITA_NATIVE_VORBIS` replaces the translated stb_vorbis decoder with the
vendored v1.04 compiled natively on a CPU 2 worker; `_ASYNC` returns the PCM
the worker has ready and caps the game thread's own decoding at a small time
budget.

`ISAAC_VITA_OGG_READAHEAD` (default OFF) takes the music file reads off the
game thread as well. The decoder's OGG bytes still arrive through the
translated ArchivedFile reader, `vita_crt_fread` and newlib's 16 KiB FILE
buffer, whose refill is one blocking `sceIoRead`. The knob wraps newlib's
`_read_r`/`_lseek_r`/`_close_r` at link time (`-Wl,--wrap`) and answers those
calls for the descriptors the native Vorbis stream top-ups read (registered
by `vita_crt_fread` only inside `nv_guest_vcall`) from `_CHUNKS` x 16 KiB a
CPU 2 worker (`isaac_ogg_ra`, priority `_PRIORITY`) has already read with
`sceIoPread` on its own descriptor into a memblock, never the guest heap.
newlib's FILE state is a function of what those three calls return, so
returning the same bytes, counts and errno keeps every FILE field and every
guest byte identical; anything the chunks do not cover (stream starts,
`SEEK_END`, a chunk still being read after `_WAIT_US`) goes to the real call
after one `SEEK_SET` that brings the kernel position back to newlib's view.
The one difference is on a failure path: after a PS-button suspend the chunks
already read are still served, so the `ENODEV` that starts
`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER` arrives later: at most one window later
when the worker's descriptor predates the suspend, and only at the next
forwarded call or at close when the worker opened it after the resume (its
descriptor is then live and hits continue); the bytes are the file's bytes
either way. Receipts: `KAGE VITA OGG READAHEAD: worker=...`, then
`KAGE VITA OGG READAHEAD STATS: reads=... hits=... miss=...` and
`KAGE VITA OGG READAHEAD WORKER: fills=...` every 16 stream reads. Needs
`ISAAC_VITA_NATIVE_VORBIS`, `ISAAC_VITA_ARCHIVE_FILE_CACHE` and
`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER`. OFF adds no source, define or link
option; `host_vita_crt.c` and `host_vita_native_vorbis.c` keep the base text
outside their `#if defined(ISAAC_VITA_OGG_READAHEAD)` blocks, so their
objects differ from the base only in debug line numbers (identical after
`--strip-debug`). Proofs: `test_ogg_readahead.sh` streams generated OGGs
through both paths and compares every return value, errno, byte and the
decoded PCM, with injected delays, worker failures, a dead descriptor (with
the worker's descriptor dead or live), a held entry lock, descriptor reuse
and foreign threads, in six geometries, kills six seeded mutants, builds the
CRT hooks on the host and runs the device-gate evaluator on the oracle's
own receipts; `recomp/test_vita_ogg_readahead.py [--base REV]` checks the
build gate, the registries and that the OFF source is the base. Device A/B:
`ogg_readahead_gates.py ab A.log B.log` and `suspend S.log` compute gates
G1-G8 per window class (game, menu, transition, burst), tested by
`test_ogg_readahead_gates.py` against the P2b session.

Music streams are refilled from the main loop, so a load stall longer than the
queued PCM drains a stream's OpenAL source to `AL_STOPPED`; the game's
MusicManager then treats the slot as finished ("music stopped playing") and
drops the track. `ISAAC_VITA_AUDIO_STARVATION_SHIELD` answers that one
`IsPlaying` query with playing for a live stream (still in the sound manager's
active list, not at end of file, not stopped by the game) and restarts the
source when PCM was queued after it stopped; it logs
`KAGE VITA AUDIO SHIELD: n=... replays=... starved=...` (first 8 events, then
every 64th) and a final summary. `ISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT` is the
diagnostic counterpart: a linker `--wrap` of the translated slot reset logs
`KAGE VITA AUDIO MUSIC DROP: slot=... src=... state=... eof=... loop=...
last_refill_ms=...` at the moment of each drop (first 16, then every 16th).
Both default OFF, keep every object byte-identical when OFF, and need
`ISAAC_VITA_AUDIO_WORKER` or `ISAAC_VITA_AUDIO_REFILL_RECEIPT`; proofs in
`test_audio_starvation_shield.sh` and `recomp/test_audio_shield_sites.py`.

With the native audio manager active the game loads every sound effect on
its first Play (`[warn] Sound N was not preloaded.` in the game log), inside
a gameplay frame; the Downpour thunder alone is about 3.4 MiB and 570 ms.
`ISAAC_VITA_SFX_FLOOR_PRELOAD` (default OFF) wraps `Level::Init`
(`--wrap=sub_00311e60`) and, after it returns, replays Play's own lazy-load
branch (`SoundEffect::Load` with Play's return word, then the loaded-count
increment) for a fixed set: 13 sounds that were first-play loads in at
least half of 48 device sessions, and on Downpour (`m_Stage` 1-2,
`m_StageType` 4) the thunder, water flow, drip, wet-step and leech sounds.
Already loaded or invalid ids, the eager-load policy, audio off, a terminal
heap, guest heap headroom below `ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB`
plus the sound's size, and an OpenAL pool with less than 2 MiB plus the
sound's size free (the PCM is copied there by `alBufferData`) keep the
first-Play load.  Every register Level::Init returned with is restored.  It
logs `KAGE VITA SFX PRELOAD: armed ...` once, `KAGE VITA SFX PRELOAD:
sound=... samples=... us=... hr_kb=... pool_kb=...` per loaded sound (or
`sound=... declined hr_kb=|pool_kb=...`) and `KAGE VITA SFX PRELOAD:
level=... loaded=... skip(l,r,s,h,p)=... us=...` per Level::Init.  Needs
`ISAAC_VITA_TRANSLATED_CPU`, `ISAAC_VITA_KAGE` and `ISAAC_VITA_AUDIO`.
CMake refuses it together with
`ISAAC_VITA_UPDATE_PROBE` (same symbol).  OFF keeps every object
byte-identical; proof in `recomp/test_vita_sfx_floor_preload.py`
(differential MSVC x86 oracle against the translated Play).

`ISAAC_VITA_SFX_FIGHT_PRELOAD` (default OFF, needs
`ISAAC_VITA_SFX_FLOOR_PRELOAD`; CMake refuses it alone) adds a third list to
the same replay: 22 sounds, 2,670,450 stock bytes in 47 files, that the
seeded bench fights still loaded on their first Play (the P3 spawn window
203-224 ms of WAV loading, the large death burst 42-57 ms inside one update)
or that hand-played Downpour floors and at least a fifth of the real device
sessions first-played.  The ids, the selection rule and the evidence (each
log's sha256) are in `tools/bench/sfx_fight_evidence.json`, re-derived by
`tools/bench/sfx_fight_list.py derive`.  The list is walked after the floor
lists of a Level::Init that completed; it is declined as a whole, before any
load, when the guest heap headroom is below
`ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB` plus the bytes of the fight sounds
not yet loaded, or the OpenAL pool below its 2 MiB margin plus those bytes
(`KAGE VITA SFX PRELOAD: fight level=... declined hr_kb=|pool_kb=...
need_kb=... sounds=... mask=0x...`, the mask a bit per list index), and
every per-sound rule above still applies while it walks.  The reserve is
checked at that Level::Init only: the list then stays resident, so for the
rest of the run lazy loads of other sounds see up to 2,667 KiB less heap
headroom than with the knob OFF (the bytes of list sounds not played yet),
and the first Level::Init takes longer (about +0.7 s, estimated from the
per-file load cost).  Music streams are refilled from the main loop, which
that freeze blocks, so the run-start music runs dry for longer (it already
does with the floor lists; `ISAAC_VITA_AUDIO_STARVATION_SHIELD` restarts
it).  Receipts: the
banner gains `fight=22 fight_kb=2667`, and each walk logs `KAGE VITA SFX
PRELOAD: fight level=... loaded=... skip(l,r,s,h,p)=... us=... need_kb=...
hr_kb=...|... pool_kb=...` (or `fight level=... stopped ...`).
`tools/bench/sfx_fight_list.py phases NATIVE GAME` prints, per bench phase,
the WAV loads, the worst update loop, the lowest heap headroom and the
sounds first-played.  It adds no source, symbol or link option: the define
goes to `host_vita_sfx_floor_preload.c` only, and OFF keeps every object
byte-identical.  Same proof script (fight legs of the oracle, fight mutants,
the list against the evidence and, with `--packed`, the archives).

## Lua and mods

`ISAAC_VITA_LUA=ON` builds the game's Lua 5.3.3 bridge from a user-supplied
pristine `lua-5.3.3` source tree (`ISAAC_LUA53_SOURCE_DIR`, or `--lua
--lua-source` in the wrapper); every file is pinned by SHA-256. Mods live at
`ux0:data/isaacr001/mods/<workshop id>/`; a `disable.it` file inside a mod
directory disables it; the Save/Mod Manager app (see INSTALL.md) toggles that
file. A build with
`ISAAC_VITA_LUA=OFF` crashes at startup when a mod is enabled (observed
2026-09-03), so ship Lua ON whenever mods are present.

`recomp/test_vita_eid_startup.py`, with the fixtures in `eid_oracle/`, freezes
the PE's `RegisterClasses` body, EID 5.23 and the shipping core scripts, builds
a fresh 32-bit Lua 5.3.3, runs the real core bootstrap and EID's complete top
level with the game objects as inert recording proxies, proves the 102 module
paths and 35 callback registrations, and requires a byte-identical transcript
on a second run. It covers the pre-frame script/module/registration boundary
only, not callback execution or rendering.

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
There is no remapping UI. The Manager has a read-only Controls page with this
table.

## Data and logs

- Everything writable is under `ux0:data/isaacr001/`: `resources/`,
  `Documents/` (saves; back them up before replacing a build, see INSTALL.md;
  `tools/isaac_vita_sync.py pull` fetches dated copies), `mods/`,
  `shader-cache/`.
- Port log: `ux0:data/isaacr001/first-arm-fault.log` (clock settings, which
  native replacements were activated, `FAULT` lines). Game log:
  `Documents/My Games/Binding of Isaac Repentance/log.txt` below the same
  root (Lua and mod errors).
- With `ISAAC_VITA_GAME_LOG_ASYNC` the game log is written behind the game
  thread. What it can lose: nothing on a controlled end (normal exit, `_exit`,
  guest stop, controlled fault, abort/assert); on a native data abort, kernel
  kill or power loss the INFO lines since the worker's last completed append
  (typically the last 1-8 lines, at most the 64 KiB ring), never a
  WARN/ERROR/ASSERT line or anything before it (those flushes are
  synchronous). The OFF build's newlib FILE loses its 1 KiB buffer and, with
  `ISAAC_VITA_GAME_LOG_BATCH`, up to 7 INFO lines on `_exit`, guest stop and
  fault, because nothing flushes guest FILEs on that path; the lane closes
  that gap.
- The phase profiler (`ISAAC_VITA_PHASE_PROFILE`) prints its records, lines
  starting with `ph120.`, through the kernel debug printf; they are visible
  only with a debug-log plugin, not in a file. With `ISAAC_VITA_GL_FILL_CENSUS`
  the block gains `ph120.fa` (fill totals and classes) and `ph120.fp` (fill
  per pass ordinal since the present plus the last offscreen attachment).

## Tests

- `python tools/build_vita.py --self-test` (host-only wrapper checks, not part
  of the build).
- `recomp/vita/test_*.sh` and `recomp/vita/test_*.py`; most need a softfp
  VitaSDK, none need a console.
- Stock vitaGL: `test_vitagl_stock_reference_recipe.py`,
  `test_kage_vita_stock_reference.py`, `test_vitagl_stock_patch_chain.py`,
  `cmake -P test_kage_vita_stock_reference_cmake.cmake`.
- `vitagl-overlay/test.sh`, `openal-overlay/test.sh`,
  `openal-overlay/test_deterministic_archive.py`.
- Input-free bench: `python tools/bench/bench_run_test.py` (repro_console
  script via `lupa`, analyzer on the smoke logs, prepare/listing/verify) and
  `python recomp/test_vita_launch_args.py --pe <frozen PE>`.

Vita3K is useful for correctness and crash reproduction only; never quote
emulator numbers as performance. Its paths are in `INSTALL.md`.

## Legal

The project's own code is GPL-2.0-or-later; third-party parts keep their own
terms. See [`DISTRIBUTION.md`](../../DISTRIBUTION.md),
[`NOTICE.md`](../../NOTICE.md), [`THIRD_PARTY.md`](../../THIRD_PARTY.md), [`LICENSE`](../../LICENSE).
