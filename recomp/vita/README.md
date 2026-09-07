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
v0.1.1-alpha is `release/v0.1.1-alpha.cmake` (a CMake initial cache for
`cmake -C`), whose header gives the build commands.

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
  `ISAAC_VITA_OPENAL_POOL` (default ON).
- Lua: `ISAAC_VITA_LUA` with `ISAAC_LUA53_SOURCE_DIR`,
  `ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=128` (default),
  `ISAAC_VITA_LUA_GCCOLLECT_CLAMP`, `ISAAC_VITA_LUA_SCOPE_FASTPATH` (default
  ON), `ISAAC_VITA_LUA_IMPORT_FASTDISPATCH`, `ISAAC_VITA_LUA_NATIVE_INDEX`,
  `ISAAC_VITA_LUA_NATIVE_GETCLASS`.
- Loading, I/O and memory: `ISAAC_VITA_NATIVE_PNG` (`_RESERVE_MB=16`),
  `ISAAC_VITA_NATIVE_INFLATE`, `ISAAC_VITA_ARCHIVE_FILE_CACHE`,
  `ISAAC_VITA_CRT_SEEK_SHADOW` and `ISAAC_VITA_CRT_DESCRIPTOR_RECOVER` (both
  need the archive cache; the recovery reopens a read-only FILE token whose
  descriptor the kernel invalidated across an app suspend or PS-button
  interception, errno `ENODEV`, at its logical cursor and redoes the failed
  `fread`/`fseek` once; one `arcdiag v1 tag=descriptor-recover-v1` line per
  recovery and a `KAGE VITA CRT DESCRIPTOR RECOVER` counter line with every
  seek-shadow report and at shutdown; write and update streams are never
  recovered),
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
  `ISAAC_VITA_GL_SHIM_RAW_ARGS`, `ISAAC_VITA_SHADER_ATTRIB_FASTPATH`,
  `ISAAC_VITA_KAGE_QUAD_FASTPATH`.
- ON in v0.1.1-alpha: `ISAAC_VITA_KAGE_REFCOUNT_SEAM`,
  `ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH`, `ISAAC_VITA_LASER_RING_SHADOW_SKIP`,
  `ISAAC_VITA_STOCK_FBO_RT_SCENES`, `ISAAC_VITA_OGG_QUEUE_EMERGENCY`,
  `ISAAC_VITA_HEAP_TERMINAL_FASTPATH`; `ISAAC_VITA_NATIVE_PNG_REUSE_LARGE` OFF;
  every profiling and observer option OFF in the release build.
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

Two options need the corpus generated with an environment variable set for
the `recomp/gen_all.py` run (`tools/build_vita.py` imports `gen_all`, so set
it before running the wrapper): `GUEST_FLOOR_THUNK_FASTPATH=1` for
`ISAAC_VITA_FLOOR_THUNK_FASTPATH` and `GUEST_LEAF_INLINE=1` for
`ISAAC_VITA_TRANSLATED_CPU_LEAF_INLINE`; SSE lowering is a generation default.
The release corpus is generated with both variables set. The generation
manifest records the switches, and a corpus generated without them compiles to
different code, so keep the two in step with the options.

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
issues exactly the unmemoized calls.  Cost: 16 direct-mapped entries, ~20 KiB
of BSS, one generation read and one name-bytes compare per replay.
`recomp/test_vita_shader_attrib_fastpath.py` runs the replay oracle in three
flavours (frozen, memo, memo+VERIFY), and the `coalesce-memo` mode of
`test_gl_vita_backend.ps1` pins the backend bump sites.

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

Vita3K is useful for correctness and crash reproduction only; never quote
emulator numbers as performance. Its paths are in `INSTALL.md`.

## Legal

The project's own code is GPL-2.0-or-later; third-party parts keep their own
terms. See [`DISTRIBUTION.md`](../../DISTRIBUTION.md),
[`NOTICE.md`](../../NOTICE.md), [`THIRD_PARTY.md`](../../THIRD_PARTY.md), [`LICENSE`](../../LICENSE).
