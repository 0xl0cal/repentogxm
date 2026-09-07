# Stock vitaGL reference profile

This recipe builds the vitaGL that the shipped renderer links when
`ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON`; the perf builds recorded in
`../../STATUS.md` use it. It pins upstream vitaGL commit
`73dd57a8857f89f2353881c6de5891959c5c1983`. Patch 0001 changes only
deterministic archive/debug-path mechanics and backports the one-line
`f24ad3e66f7f34bebe70598d1302c34f4eff4a54` initialization out-of-bounds fix.
Mandatory patch 0028 refreshes the shader float-output selector immediately
after lazy color-attachment synchronization in `scene_reset`. It repairs an
RGBA8 <-> RGBA16F redefinition with changed backing storage; default-display,
RT/depth ownership, dirty flags, and recovery order are unchanged. It does
not address same-address redefinition, scissor resize, or fixed-function
float output. No new option or public ABI is introduced. Its hash is a
functional input to both native and shader-cache identities. The overlay
recipe is unaffected.

Mandatory patch `0029-invalidate-resized-fbo-scissor.patch` runs after the
optional source patches. Within the existing attachment-size-change branch,
it marks scissor dirty only for the current FBO with scissor enabled. Both
lazy synchronization and explicit reattachment then recompute the clipped box
through the existing update after BeginScene. No disabled-scissor mask work,
RT/depth ownership, flipped-Y replay, or same-address upload policy changes.
There is no knob: its patch hash belongs to both native and shader identities,
and the build contract reports `fbo_scissor_resize_invalidation=1` plus the hash.
The existing patch-chain check freshly extracts the full
attachment/reset/scissor bodies and compares baseline/fixed SDK argument
values with mocked SDK/memory boundaries.

Mandatory `0030-correct-flipped-scissor-replay.patch` then makes scene-reset
tile-clip replay use the same Y as `update_scissor_test`: for flipped FBOs,
`max(region.gl_y, 0)`, otherwise the existing `region.y`. The cached region,
mask, viewport and scene lifetime are unchanged. With existing valid-region
support, `0031-correct-flipped-scissor-replay-valid-region.patch` is used instead;
it corrects Y before the unchanged 0009 clamp. These are two source variants
of one fix, not options; both hashes are functional inputs and reported beside
`fbo_scissor_replay_sync=1`. The existing scissor fixture exercises no-resize
Flush/Finish replay, display/unflipped unchanged behavior, and the actual
valid-region clamp with SDK arguments.

The optional patches below are applied according to the environment
switches below:

| build.sh environment | CMake option | patch |
|---|---|---|
| `ISAAC_GPU_DRAW_OPTIMIZATIONS=1` | `ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS` | 0002 |
| `ISAAC_P8_SAFE_UPLOAD=1` | `ISAAC_VITA_VITAGL_P8_SAFE_UPLOAD` | 0010; prerequisites only, no atlas conversion |
| `ISAAC_CANONICAL_QUAD_ZERO_COPY=1` | `ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY` | compile flag on patch 0002 |
| `ISAAC_GXM_STATE_SHADOW=1` | `ISAAC_VITA_GXM_STATE_SHADOW` | compile flag on patch 0002 |
| `ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1` | `ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS` | 0003 |
| `ISAAC_SHADER_CACHE=1` | `ISAAC_VITA_VITAGL_SHADER_CACHE` | 0004 |
| `ISAAC_SHADER_CACHE_OBSERVER_COMPAT=1` | `ISAAC_VITA_SHADER_CACHE_OBSERVER_COMPAT` | shader-cache identity only; requires cache, default OFF |
| `ISAAC_PHASE_PROFILE=1` | `ISAAC_VITA_PHASE_PROFILE` | none (counters) |
| `ISAAC_GL_TIME_PROFILE=1` | `ISAAC_VITA_GL_TIME_PROFILE` | 0005, 0006 |
| `ISAAC_FBO_RT_SCENES=1` | `ISAAC_VITA_STOCK_FBO_RT_SCENES` | 0007 |
| `ISAAC_COLOROFFSET_FS_PROBE=1` (TRIVIAL) or `=2` (NODISCARD), diagnostic only | `ISAAC_VITA_COLOROFFSET_FS_PROBE` | 0008 |
| `ISAAC_FBO_VALID_REGION=1` | `ISAAC_VITA_STOCK_FBO_VALID_REGION` | 0009 |
| `ISAAC_COLOROFFSET_FS_PROBE=3` (NEUTRAL), all-vertex-gated production path | `ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH` | 0008 |
| `ISAAC_COLOROFFSET_STAGING_PROOF=1` | `ISAAC_VITA_COLOROFFSET_STAGING_PROOF` | 0012; requires neutral mode 3 |
| `ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=1` | `ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND` | 0014; requires staging proof and neutral mode 3 |
| `ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1` | `ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR` | 0015; isolated candidate requiring PLAIN and single-final-bind |
| `ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=1` | `ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM` | 0018; exact clean 64-byte Transform no-op suppression, requires P5, default OFF |
| `ISAAC_LASER_ATLAS_NEAREST=1` | `ISAAC_VITA_LASER_ATLAS_NEAREST` | 0019 plus reused 0016/0017 transport; authenticated atlas sampler, independent of halo clipping, default OFF |
| `ISAAC_COLOROFFSET_PLAIN_FASTPATH=1` | `ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH` | 0013; requires staging proof, default OFF |
| `ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION=1` | `ISAAC_VITA_COLOROFFSET_STAGING_PLAIN_FUSION` | Ordinary cached-chunk CPU proof only; requires staging proof + PLAIN fastpath, default OFF |
| `ISAAC_COLOROFFSET_STAGING_FINITE_NEON=1` | `ISAAC_VITA_COLOROFFSET_STAGING_FINITE_NEON` | Private integer finite check on complete ordinary cached records; requires stock + staging proof + PLAIN fusion, default OFF |
| `ISAAC_COLOROFFSET_STAGING_LIMITS_NEON=1` | `ISAAC_VITA_COLOROFFSET_STAGING_LIMITS_NEON` | Whole private per-lane predicate on the same owned records; requires finite NEON, supersedes its private scans, default OFF |
| `ISAAC_COLOROFFSET_STAGING_OUTLINE=1` | `ISAAC_VITA_COLOROFFSET_STAGING_OUTLINE` | Private noinline whole-copy entries; requires limits NEON, default OFF |
| `ISAAC_COLOROFFSET_PLAIN_FP16=1` | `ISAAC_VITA_COLOROFFSET_PLAIN_FP16` | Private 0013 helper sub-mode; requires PLAIN, rejects vertex pair, default OFF |
| `ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1` | `ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX` | Private 0016 helper sub-mode; requires HALO_CLIP, faint fringe color/depth approximation, default OFF |

The main CMake build runs this recipe in its binary directory with the
switches derived from those options.

The default-OFF `ISAAC_VITA_LASER_LIGHT_HALO_DEPTH_APPROX` removes only the
halo helper's depth-test veto. The normal guest renderer enables depth, so
the original halo/nearest path cannot engage on ordinary gameplay draws.
The option retains every exact atlas/program/layout/plain/UV/size/blend
guard and all existing clipping math, native depth state, shaders and gameplay.
With depth writes enabled, trimming the weak 416..421 and 444..448 fringes
also removes their depth coverage and can reveal later geometry there.
This is intentionally NOT image- or depth-equivalent. The original triangle's
retained core and interpolated Z are unchanged; stencil remains prohibited.

The option is native-only: its mode file and environment value select the
existing helper via `EXTRA_CFLAGS=-DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1`.
The helper's existing content hash and the option value are recipe inputs;
there is no new patch, public ABI, generated corpus change or counter family.
OFF omits the macro, retains the original rejection and success text.
ON appends `depth-approx=1 depth-test=1` (or `0` for a depth-disabled draw) to
the existing first-success halo line. If nearest is also
requested, its separate existing `nearest=point applied=1 restored=1` line
requires a successful override bind, draw and restore.

The default-off staging-proof option preserves the neutral fragment source,
blend/texture/layout requirements and the uncached GPU allocation. Only the
exact packed client-array path copies through a 704-byte CPU stack chunk,
validates every copied vertex there, and bulk-copies those same bytes into the
GPU pool. It never rereads that GPU upload for the neutral proof. A nonneutral
vertex selects the original fragment shader and the remainder is still copied
in full. Unsupported ranges use the original copy/fallback. Program identity
is checked once per draw and shared by the opaque and neutral selectors, never
cached by a reused client pointer. This option does not enable
`vglUseCachedMem`.

The additional default-off plain specialization proves that Colorize alpha
and all three ColorOffset lanes are exactly +/-0 at **every** copied vertex.
It retains every existing neutral/finite/bounded-arithmetic check, arbitrary
accepted vertex tint/alpha, texture sampling (including P8), and the draw's
blend/output/MSAA state. Its separate per-program fragment cache falls back
to the current neutral shader when compilation, registration or a cache
entry is unavailable. Colorized/offset draws continue using neutral. The
88-byte stream and original VS remain unchanged. No plain proof rereads GPU or
mutable client bytes.

The independent default-off staging PLAIN fusion option reuses the integer
color-bound words already loaded by the ordinary per-vertex neutral proof.
Finite checks, bound checks and flag checks retain their original order;
PLAIN is decided only after all have passed. After the first non-PLAIN vertex,
the rest of that chunk and all later chunks use the original neutral-only
predicate. Rejection still uploads the same cached chunk and copies the whole
remaining input once. UNKNOWN still copies nothing. This changes no shader,
halo admission, texture, allocation or draw byte. The standalone policy helpers
are unchanged.
The mode and existing staging-header hash are functional inputs to both native
and shader-cache identities, including observer-compatible mode. Editing
`build.sh` also changes both keys versus the prior revision even with fusion
OFF.

The separate default-OFF `ISAAC_VITA_COLOROFFSET_STAGING_FINITE_NEON` requires
stock vitaGL, staging proof and PLAIN fusion; it never enables those options.
On ARM NEON only, the private ordinary staging predicates replace the scalar
22-word finite scan with five 16-byte loads and one final 8-byte load of the
already-copied 88-byte record. IEEE exponent equality is checked per integer
lane before Boolean reduction; no FP operation or 96-byte overread is used.
Bounds/flags retain their prior order and bytes, including raw WHITE checks.
After PLAIN loss, private neutral-only predicates use the same finite helper
without PLAIN/WHITE accumulation. Shared standalone and halo predicates stay
unchanged. Early NaN may now read the rest of its complete owned record,
never more client/GPU bytes. Non-ARM hosts retain the scalar path. Observer
OFF does not gain WHITE tracking.
The native mode `HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON=1`, contract field
`coloroffset_staging_finite_neon`, existing staging-header hash and build-script
hash are functional in both recipes. Neither mode
nor header is normalized out of observer-compatible shader keys.

The separate default-OFF `ISAAC_VITA_COLOROFFSET_STAGING_LIMITS_NEON` requires
finite NEON without enabling it implicitly. When both are ON on ARM NEON,
limits mode supersedes the private finite/bounds/flags scans; it does not run
both predicates. OFF/explicit0/non-ARM retains the prior finite-only path.
Only complete owned 88-byte records use this reordered predicate. F is
`0x7f7fffff` (largest finite magnitude), C is `0x41800000`, Z is zero.
Inclusive unsigned magnitude limits
in load order are F/F/F/C, C/C/C/F, F/C/C/C, C/C/C/C, F/F/Z/Z, then Z/F
in the eight-byte tail. Raw84 separately retains the sign-or-zero condition.
PLAIN and optional raw WHITE reductions reuse loaded vectors; there is no
second scalar snapshot read. Neutral-only paths omit both accumulations;
observer OFF compiles out WHITE arithmetic/access. Partial private WHITE
accumulation on rejected records need not match: no token escapes rejection.
Shared/client/GPU/halo predicates, ingress gates, copies, remainder and final
live-float/publication rules are untouched.

The separate default-OFF `ISAAC_VITA_COLOROFFSET_STAGING_OUTLINE` requires
limits NEON without enabling it implicitly. It changes only the declarations
of the two complete ordinary copy/proof functions from `static inline` to
private noinline functions. Undefined or explicit zero retains the original
declarations; unsupported compilers fail loudly when ON. GCC/Clang and MSVC
use their existing noinline spelling. The native owner enters once per admitted
ordinary-copy attempt, not per vertex; halo success never enters this copy.
The existing WHITE null-token fallback remains, although native calls supply
the draw-local token.

Native `HAVE_ISAAC_COLOROFFSET_STAGING_OUTLINE=1`, contract field
`coloroffset_staging_outline`, the staging-header hash and build-script hash
remain functional in both identities (94-field common prefix).

With phase profiling, existing `ph120.kp draw(q,b,o)` remains the aggregate
neutral family (neutral + proven plain), while appended `plain(q,b,f)` counts
eligible plain requests, successful plain selections and requests falling
back (`q=b+f`) for that window. The original `gxp/src` fields still identify
the neutral program; `KAGE VITA COLOROFFSET PLAIN LINK` reports the separate
plain identity. The optional private getter is weak and take-and-zero, with
three uint32 words and ABI return value 1; no new public vitaGL ABI or global
guest compile definition is needed.

The private archive is built with exactly these behavioral flags:

```text
SOFTFP_ABI=1
NO_DEBUG=1
NO_SPLASHSCREEN=1
SINGLE_THREADED_GC=1
```

`SHARED_RENDERTARGETS` is deliberately absent. The recipe invokes make with an
empty environment plus only the recorded tool paths and flags, builds only
`libvitaGL.a`, never installs into the SDK, and records the source, patch,
flags and output hashes under its output root.

The optional `ISAAC_GPU_DRAW_OPTIMIZATIONS=1` variant remains on that same
stock commit and adds only `0002-exact-gpu-draw-optimizations.patch`. It
coalesces an indexed custom-shader draw from N identical physical vertex
streams to one only when every resolved base address, stride, index source and
identity stream mapping is exactly equal. It also retains up to four patched
vertex programs per GL program under an explicit field-by-field shader/layout
key. Program deletion finishes the GXM context and releases every retained
reference once. Any failed check follows the original draw path.

Two independent, default-off switches extend that exact variant. They both
require `ISAAC_GPU_DRAW_OPTIMIZATIONS=1`:

* `ISAAC_CANONICAL_QUAD_ZERO_COPY=1` allocates one process-lifetime,
  CPU/GXM-mapped `0xC000`-entry U16 index buffer with KAGE's exact
  `0,2,1 / 1,2,3` quad winding. It is used only at the two call sites in the
  supported PE that draw quads, and only when the arguments are exactly what
  those sites always pass; anything else takes the normal draw path.
* `ISAAC_GXM_STATE_SHADOW=1` tracks vertex program, fragment program and every
  complete 16-byte fragment-texture descriptor independently for each GXM
  context and successful scene. Begin/end/reset, framebuffer, clear,
  fixed-function, shader rebuild/release and context changes invalidate the
  applicable state. Exact repeats alone suppress a setter.

With `ISAAC_VITA_PHASE_PROFILE=ON` the
phase profiler prints counters for both switches (log lines `ph120.q` and
`ph120.f`). Neither switch enables vitaGL's unsafe draw speedhacks or changes
the full-resolution display path.

A host-side check (`gpu_draw_policy_oracle.c`) verifies that the supported PE
builds its quad index buffer exactly the way the shortcut assumes. A different
PE fails that check and the shortcut stays off.

`ISAAC_GPU_DRAW_OPTIMIZATIONS` defaults to `0`, in which case the optimization
patch and policy header are not applied at all. Both modes are recorded in
`build-contract.txt`; the
optimized archive exports `vglGetIsaacGpuDrawStats`, while the default archive
is required not to export it.

The further default-off `ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1` profile requires
the exact GPU draw profile. It keeps the game's `resources/shaders/coloroffset.fs`
byte-for-byte unchanged: the typed GL boundary authenticates the complete
source (the 2,052-byte CRLF asset, FNV-1a `2b3ccf4a`), calls stock
`glShaderSource` once with the original arguments, and the linked programs
must match the captured vertex (562 bytes) and fragment (809 bytes) binaries.
Only the fragment-program blend object changes: a draw may select the cached
`SCE_GXM_BLEND_FUNC_NONE` variant when the exact source/compile generation,
linked VS, sampler parameter and 8-attribute/88-byte layout match, blending is
ADD with `ONE` or `SRC_ALPHA` as the colour source and `ONE_MINUS_SRC_ALPHA` as
its destination under a complete colour mask, the texture is an owned live
432x240 `U8U8U8_BGR` descriptor (RGB24 sampling supplies alpha one), every
referenced `Color.a` in the four-vertex staged range is bit-exact 1.0 with
default effect lanes and finite `PixelationAmount <= 0`, the output is UCHAR4
and the logical viewport is exactly 0,0,960,540. Under those proofs the
captured ADD blend is mathematically no blending; any uncertain field follows
the already-bound stock fragment path. It does not replace GLSL or reduce
resolution. The proposed GLSL early return was excluded (a texture lookup
inside a branch driven by interpolated varyings makes implicit derivatives
undefined), and the static-default VS/FS pair stays deferred, so
`static_eligible`, `static_hits`, and `static_pixels` remain zero by construction.
With `ISAAC_VITA_PHASE_PROFILE=ON`, `ph120.k` prints the 24-word
counter ABI once per 120 guest loops (source selection, compile results, exact
draw requests, opaque eligibility and hits, the eight failure counts, the three
cache counts and the three static counts); its `opaque_viewport_pixels` value
is a CPU-visible viewport upper bound, not a GPU fragment counter (clip, depth
and scissor can reduce actual fragments). `test_coloroffset_gpu_bundle.ps1`
runs without game data; an owned source asset plus the two captured GXP paths
add the exact production receipts, none of which is copied into the repository.

The CMake profile requires `ISAAC_VITA_KAGE=ON` and
`ISAAC_VITA_DIRECT_DEFAULT=ON` (the generated `Game::Render` override that
draws straight into the screen framebuffer). It rejects the
first-frame/raw-GXM/known-color/raster/screenshot probes,
`ISAAC_VITA_IO_PROFILE` and `ISAAC_VITA_STALL_PROBE`, so the first submitted
frame uses stock vitaGL display and scene handling. Audio, the CPU loading
screen and the phase profiler are compatible; only
`ISAAC_VITA_TEXTURE_CHURN_PROFILE` requires `ISAAC_VITA_AUDIO=OFF`. The rules
live in `kage_vita_stock_reference_selection.cmake`.

On a Vita build host:

```sh
export VITASDK=/opt/vitasdk-softfp
bash recomp/vita/vitagl-stock-reference/build.sh \
  /tmp/isaac-vitagl-stock-reference
```

For the opt-in variant:

```sh
export ISAAC_GPU_DRAW_OPTIMIZATIONS=1
bash recomp/vita/vitagl-stock-reference/build.sh \
  /tmp/isaac-vitagl-stock-reference-gpu-draw
```

The production CMake switch is
`-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=ON`; it is accepted only together
with `-DISAAC_VITA_VITAGL_STOCK_REFERENCE=ON`.

The host oracle compiles without VitaSDK and exercises the captured physical
8-attribute/88-byte layout, address equivalence, every hostile coalescing
fallback, every cache-key field, capacity fallback, and one release per stored
program:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  recomp/vita/vitagl-stock-reference/gpu_draw_policy_oracle.c \
  -o /tmp/isaac-gpu-draw-policy
/tmp/isaac-gpu-draw-policy
```

`ISAAC_GL_TIME_PROFILE=1` (CMake `-DISAAC_VITA_GL_TIME_PROFILE=ON`, which
also requires `ISAAC_VITA_PHASE_PROFILE=ON`) adds
`0005-isaac-scene-timer.patch` and `0006-isaac-scene-split.patch` on top of
the 0002 `gxm.c`. 0005 brackets `sceGxmBeginScene` and `sceGxmEndScene` in
`scene_reset`/`scene_end` with two `sceKernelGetProcessTimeWide` reads and
exposes the window sums through
`vglIsaacSceneTimes(begin_us, end_us, count, begin_max_us)` (take-and-zero).
0006 splits the EndScene sum by the framebuffer whose scene is ended (FBO or
display, from `old_framebuffer`) and by the ordinal of the FBO scene end since
the last `vglSwapBuffers` (1st, 2nd, 3rd, later), counts EndScenes longer than
500 us and the longest one, and counts offscreen render targets and depth
buffers created plus `sceGxmCreateRenderTarget` failures (otherwise silent
under `NO_DEBUG=1`); it exposes them through
`vglIsaacSceneWaits(fbo_us, display_us, max_us, slow, ordinal_us[4],
rt_created, rt_failed, depth_created)` (take-and-zero). Neither hook is
declared in `vitaGL.h`, so the shader-cache whole-file hashes of
`custom_shaders.c`/`vitaGL.h` are unchanged; the consumer
(`kage_vita_phase_profile.c`) declares them and prints `ph120.gt` and, directly
after it, `ph120.gx bid= win= loops= end(f,d,max,slow)=… ord(1,2,3,4+)=…
rt(c,x,z)=…` (f + d == scene e of `ph120.gt` within the same take; rt(c)
counts every offscreen `setup_render_target` attempt, so a refused N-slot
target plus its 1-slot retry adds 2 to c and 1 to x; rt(x) also counts display
render-target failures; `slow` includes display EndScenes; rt(c) and rt(z)
must be 0 in a steady window and rt(x) always). Measured on the device the CPU
blocks inside `sceGxmEndScene`, not `sceGxmBeginScene` (BeginScene stays under
0.1 ms), so the EndScene sum is the GPU back-pressure the CPU sees on a
draw-target switch, a clear or a present.

`ISAAC_FBO_RT_SCENES=1` (CMake `-DISAAC_VITA_STOCK_FBO_RT_SCENES=N`, N in
1..8) adds `0007-isaac-fbo-rt-scenes.patch`, the `gxm.c`-only backport of the
upstream f142f14 knob: render targets created for framebuffer objects use
`gxm_fbo_rt_size` scenes per frame instead of the literal 1, set once at
start-up through `uint8_t vglIsaacSetupFboRenderTargetScenes(uint8_t size)`
(consumer-declared in `kage_vita_backend.c`; values outside
1..`MAX_SCENES_PER_FRAME` are ignored and the effective value is returned for
the "vitaGL ready" banner). If `sceGxmCreateRenderTarget` refuses the larger
target, the creation site retries with the stock single slot (0006 counts the
failure in `rt_failed`), so a framebuffer never stays without a target. 0006
and 0007 edit disjoint `gxm.c` hunks: every GL_TIME_PROFILE x FBO_RT_SCENES
combination applies in build.sh order (0005, 0006, 0007), and the archive must
export `vglIsaacSetupFboRenderTargetScenes` iff the flag is 1. Like 0005 it
requires `ISAAC_GPU_DRAW_OPTIMIZATIONS=1`. A 1024x1024 target at 8 scenes
costs about 0.95 MB more GXM driver memory than at 1 (measured on the device).

`ISAAC_COLOROFFSET_FS_PROBE=1|2` (CMake
`-DISAAC_VITA_COLOROFFSET_FS_PROBE=TRIVIAL|NODISCARD`, default `OFF`; `ON` is
rejected, the mode must be named) adds `0008-isaac-coloroffset-fs-probe.patch`,
a DIAGNOSTIC that visibly changes the picture and never ships. It exists to
measure the fill cost of the stock ColorOffset fragment shader in the GPU-bound
rooms. It requires `ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1` (CMake also
`ISAAC_VITA_PHASE_PROFILE=ON`) and edits `custom_shaders.c` only, between 0003
and 0004 (its hunks are disjoint from 0004's; `vitaGL.h` stays byte-identical,
so the shader-cache matrix gains the third key `1:1:1` with the same header
hash). When `glLinkProgram` links the exact ColorOffset pair (the 0003
`record_link` proof), the probe compiles a second fragment program through the
same GLSL translator (pair mode seeded from the exact vertex shader, translator
globals restored afterwards) and vitaShaRK, and registers it next to the stock
one: mode 1 (TRIVIAL) is `Color0 * texture(Texture0, TexCoord0)` with no
colorize/offset/pixelate lanes and no discard; mode 2 (NODISCARD) is the stock
source with its `gl_FragCoord` clip/discard statement removed, derived at
runtime from the authenticated 2052-byte source (span 903+73 bytes, FNV-1a
`eed28c60`; result 1979 bytes, FNV-1a `fc8c5c9b`; nothing from the game asset
is embedded in the patch). Exact draws then bind a probe fragment program
created with the draw's own blend, output format and MSAA mode (up to four per
GL program) instead of the stock or the 0003 opaque one. The probe fails
closed to the stock fragment program unless the compiler is online, the
compile succeeds, `sceGxmProgramCheck` passes, the program is a fragment
program with exactly one parameter, the sampler `Texture0` at resource index
0 and the stock default uniform buffer size, and the patcher accepts it; the
stock shader object, its reflection, the uniform upload and the 0003 link
record are never touched. The archive exports
`vglGetIsaacColorOffsetFsProbeStats` (21 `uint32_t`, consumer-declared in
`recomp/runtime/gl_vita_coloroffset_fs_probe.h`) iff the switch is non-zero;
`kage_vita_phase_profile.c` prints it as `ph120.kp` directly after `ph120.k`
(mode, link attempts/ready, link failures by reason, program handle, GXP and
source receipts as state; draws that consulted/bound/overrode the opaque
variant and the probe cache as window deltas), and `gl_vita_backend.c` logs
`KAGE VITA COLOROFFSET FS PROBE LINK: prog= mode= ...` once per exact link.
`build-contract.txt` records `coloroffset_fs_probe=`,
`coloroffset_fs_probe_mode=` and `coloroffset_fs_probe_patch_sha256=`.

`ISAAC_VITA_COLOROFFSET_NEUTRAL_FASTPATH=ON` is a separate production option
(default `OFF`), internally `ISAAC_COLOROFFSET_FS_PROBE=3`. It preserves the
Colorize/ColorOffset equations and blend state, specializing only draws whose
entire bounded staged vertex span proves pixelation=0 and inactive clipping.
Unknown/nonneutral layouts, VBOs, floating textures/targets and compile/link
failures keep the stock shader. It cannot be combined with either diagnostic
mode and does not require phase profiling. Eligibility is checked on every
vertex of the complete staged range through max(index)+1: the authenticated
source/GXP pair, the eight-attribute/88-byte F32 layout with identity
attribute mapping, packed single-stream client data, 16-bit non-instanced
indices with zero base vertex, `PixelationAmount` exactly +/-0, `ClipPlane.xy`
+/-0 and `ClipPlane.z <= 0`, finite vertex lanes with the retained colour
lanes bounded to [-16,16], normalized RGBA8/RGB8/P8 textures and non-floating
outputs; spans over 4096 vertices, VBOs and unknown layouts decline. Tints,
colorize alpha and colour offsets need not be defaults and stay in the shader.

`ISAAC_FBO_VALID_REGION=1` (CMake `-DISAAC_VITA_STOCK_FBO_VALID_REGION=OBSERVE`
or `ON`, default `OFF`) adds `0009-isaac-fbo-valid-region.patch` (`gxm.c`,
`misc.c`, `tests.c`). The game's offscreen colour attachments are
power-of-two textures larger than what it draws into them (1024x1024 for the
960x540 surface, 512x512 for 480x270) and its RenderTarget::Bind sets
`glViewport(0, 0, w, h)` right after `glBindFramebuffer` +
`glFramebufferTexture2D`; stock vitaGL begins every FBO scene with a NULL
`SceGxmValidRegion` and `update_scissor_test` clips to the whole texture, so
the padding rows and columns take the clear quad, the mask quads and the
colour store of every offscreen pass. The patch derives the logical rect from
`gl_viewport` (the guest's last `glViewport`, recorded in `misc.c` together
with the write framebuffer, its attachment and the attachment size it saw) at
the FBO `sceGxmBeginScene` of `scene_reset`. Fail closed: the rect is used
only when it was set after the current bind/attach, is anchored at (0, 0) and
fits inside the attachment; a rect equal to the attachment, a stale, an
unanchored or an oversized one keeps NULL; a region refused by
`sceGxmBeginScene` is retried with NULL; a guest `glViewport` that leaves the
rect derived for the open FBO scene counts a violation (`v`) in both modes
and, in apply mode, also stops applying for the rest of the run (the open
scene stays clipped, which is why OBSERVE must read `v == 0` before ON is
run at all). In observe mode (`vglIsaacSetupFboValidRegion(0)`, CMake
`OBSERVE`) nothing is passed and the scenes and violations are only
counted; in apply mode (`vglIsaacSetupFboValidRegion(1)`,
CMake `ON`) the rect is passed as the valid region and the three
`sceGxmSetRegionClip` sites (the `update_scissor_test` full-framebuffer and
scissor clips in `tests.c`, the `scene_reset` re-apply) are clamped to it
through `isaacFboRegionClampClip`; vitaGL's own viewport re-applies never
refresh the record. The display render target is untouched. Counters and a
four-entry size census come back through
`vglIsaacFboValidRegionStats(counters[8], census[20])` (take-and-zero: s FBO
scenes begun, p rect known and smaller than the attachment, u rect
unknown/unanchored/oversized, a begun with the region, e refused and retried
with NULL, v viewport violations (both modes), c clips clamped, o census
overflow; the
consumer `kage_vita_phase_profile.c` prints them as `ph120.gx
vr(s,p,u,a,e,v,c)` and the census as `ph120.vz o= sz=TWxTH/RWxRH/N,...`).
Neither hook is declared in `vitaGL.h` or `shared.h`. The patch needs the
0005/0006 BeginScene brackets as context, so it requires
`ISAAC_GL_TIME_PROFILE=1` and is refused on a tree without them; it commutes
with 0007 (both orders leave `gxm.c`/`misc.c`/`tests.c` identical), and the
archive must export both hooks iff the flag is 1. The mode hook is called
once before `vglInit` by `kage_vita_backend.c`, whose "vitaGL ready" banner
prints ` fbo-valid-region=observe|on` from its return. OBSERVE gate: `p > 0`
in every game window, `u == 0`, `o == 0`, `v == 0` everywhere and the two
game sizes in `ph120.vz`; `v > 0` means the game grows a viewport inside an
open offscreen scene and ON would clip those fragments, so ON stays blocked
until that sequence is understood. ON gate: `a == p`, `e == 0`, `v == 0`,
then the visual gate below.

What ON changes on screen. With the region applied, the padding of the pow2
attachment (columns 480..511 and rows 270..511 of the 512x512 surface,
960..1023 and 540..1023 of the 1024x1024 one) is no longer written by any
pass. Stock `glClear` runs `invalidate_viewport()` (`misc.c` glClear ->
`tests.c` `glViewport(0, 0, fbW, fbH)` under `skip_viewport_override`), so
under OFF the clear quad rewrote the padding with the clear colour on every
pass; under ON the clamped region clip and the valid region confine it to
the logical rect and the padding keeps whatever the game's construction fill
left there (the manager surface gets a (0,0,0,0) fill at creation,
`guest_0108.c` sub_003a91c0 -> `guest_0176.c` sub_005b5e40). Anything that
samples the edge reads it: the 2x bilinear upscale of the 480x270 surface
samples texel column 480 / row 270 at 25 % weight in the last output
column/row, blur and pixelation kernels reach further, and sprites or beams
crossing the 960/540 (480/270) edge used to land in the padding and be
sampled back by that bleed; ON drops them (the GXM viewport is a transform,
not a clip). Visual gate for ON against OFF: screenshots of the title, the
start room, the pause menu and a laser-ring room with the game's Filter
(bilinear) option ON and OFF, plus one room with bloom or pixelation,
pixel-diffed over the 2-px right and bottom band of the world composite
(ideally with a brimstone beam crossing the right wall); a change there is a
fringe, not "no visual change".

Reading the counters against the shim. `ISAAC_VITA_GL_TYPED_STATE_CACHE` (ON
in the perf builds) makes `vita_glViewport` return before vitaGL on an exact
duplicate of the last viewport; `vita_glBindFramebuffer` invalidates that
shadow, so the game's Bind sequence always reaches `misc.c`, but a
`glFramebufferTexture2D` attachment swap without a re-bind followed by the
same viewport leaves the 0009 record stale (attachment mismatch), counted as
`u` with a NULL region (fail closed): check a device `u > 0` against this
before blaming the game's GL sequence. `isaacFboRegionClampClip` collapses a
clip wholly outside the region to a one-pixel sliver rather than an empty
clip (unreachable: the guest never scissors outside its viewport).

The GL fill census (`ISAAC_VITA_GL_FILL_CENSUS`, records `ph120.fa` and
`ph120.fp`) is shim-side only: it lives in `gl_vita_backend.c`, adds no vitaGL
patch and does not change the archive. Its pass ordinal (`p0..p3`, `disp` in
`ph120.fp`) counts offscreen framebuffer passes since the last present the way
0006 counts FBO scene ends (`ord(1,2,3,4+)` in `ph120.gx`), so with both
options ON the kpx of pass k can be read against the EndScene wait of ordinal
k+1 in the same window. The alignment mirrors vitaGL's `scene_reset`
predicate on the guest surface (framebuffer switch, colour attach to the
in-use framebuffer, `glReadPixels` of the in-use framebuffer, delete, swap),
including the owed depth clears `ISAAC_VITA_FBO_CLEAR_ELISION` issues
natively on the owing framebuffer (counted there, in that framebuffer's open
pass). Not modelled: `glFinish`/`glFlush` (not on the guest surface) and the
empty scene vitaGL opens after a `glReadPixels` that no draw follows.

`ISAAC_LASER_ATLAS_P8=1` (CMake `ISAAC_VITA_LASER_ATLAS_P8=ON`, default OFF)
adds `0011-isaac-exact-laser-p8.patch` after the required P8 safety patch 0010.
It opts in only exact LaserEffects pixels, retains a mapped RGBA failover for
allocation-free restoration, and closes image/mip/attachment/sampler/pointer
and deletion boundaries in the native driver. It edits textures.c,
framebuffers.c and the private gpu_utils.h free hook, not public headers or
shaders. Eligibility: only a fresh, unreferenced, non-overridden
RGBA/UNSIGNED_BYTE level-0 upload whose every pixel matches the exact 448x64
`Effect_018_LaserEffects.png` reference (or either exact premultiplied output
of the game's PNG loader, or the 512x64 layout with all-zero padding columns)
is converted; one differing byte declines, and the policy never transforms an
uploaded pixel. Memory cost: at most two textures at a time, each keeping a
mapped RGBA backup; the native P8 storage is 29,696 B per 448x64 copy or
33,792 B per 512x64 copy (33,792 B with the swizzled layout). A successful
conversion logs `Isaac laser P8: exact=448x64 variant=1 sampled=29696
rgba-backup=114688`; no such line means nothing was converted.

`ISAAC_LASER_ATLAS_NEAREST=1` (CMake
`ISAAC_VITA_LASER_ATLAS_NEAREST=ON`, default OFF) is an independent visual-quality
candidate: draw-local nearest sampling for authenticated laser atlases. It
requires stock vitaGL with shader cache, exact laser P8, staging proof, the neutral fastpath and
the current P5 vertex-pair configuration (which also requires PLAIN/P3).
It does **not** require or enable `LASER_LIGHT_HALO_CLIP` or
`LASER_LIGHT_NEAREST`, does not change geometry, and leaves rejected draws on
the original sampler path.

For this option the recipe materializes existing 0016/0017 as shared draw
transport even when their options are OFF; their `HAVE_*` flags stay OFF.
Patch 0019 follows them and optional 0018, with the existing draw helper
`isaac_laser_light_nearest.h` and new metadata/descriptor helper
`isaac_laser_atlas_nearest.h`. The option, patch and helper hashes enter the
native recipe and CMake dependencies, and the actual native compile receives
`-DHAVE_ISAAC_LASER_ATLAS_NEAREST=1`. No generated guest source or global
application definition is changed. The fresh pinned-source matrix covers both
Transform modes; OFF omits 0019 and its helper from the materialized source.

Admission reuses this draw's authenticated ColorOffset source/link-generation
proof, exactly one 2D fragment sampler at native slot 0, no vertex sampler,
and the live exact-owned canonical P8 texture. Sampler objects, overrides,
mipmaps, float FBOs and unsupported native filtering are rejected. This is
independent of whether P2/P5 is selected for the actual vertex values. It adds
no vertex scan, crop, geometry change or batch-count limit. Supported callers
are canonical quads and ordinary U16 client-index TRIANGLES; U8/U32, indexed
buffer, range/base-vertex and instanced paths retain their existing behavior.

Only min/mag filtering in a draw-local descriptor changes. The original GPU
indices are submitted unless the separately enabled halo option already
staged clipped geometry. POINT may make laser edges/patterns sharper, jagged
or shimmery; sampled alpha and any shader-discard edge can change too. Positions,
UVs, tint, blend state and gameplay/collisions are not modified.
`Isaac laser atlas: nearest=point applied=1 restored=1 indices=... halo=...`
appears once only after successful native bind, draw and original-descriptor
restore. Failure lines report checked status and disable the shared nearest
path; a failed POINT bind may draw baseline only after a successful original
bind.

The source-only patch-chain regression requires either the GitHub source
tarball of the pinned commit or a local Git repository which contains it. It
applies all four supported shader-cache combinations (0008 between 0003
and 0004), the GL_TIME_PROFILE x FBO_RT_SCENES combinations and the 0009
legs with/without 0007 in both orders. Unsupported prerequisite combinations
are refused. The source-only matrix does not build an ARM archive:

```sh
python3 recomp/vita/test_vitagl_stock_patch_chain.py \
  --stock-tar /path/to/vitaGL-73dd57a.tar.gz
# Or: --stock-repo /path/to/vitaGL.git
```

Builds with this profile are configured directly with CMake/Ninja.
`tools/build_vita.py` selects `ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON` only
under `--texture-churn-profile`, so the public wrapper cannot reproduce the
shipped renderer configuration; see [`../README.md`](../README.md) for the
option set. Only outputs rebuilt by the exact CMake configuration may be
attributed to it: an older VPK or `build-manifest.json` in the build tree is
stale and is not a stock-reference artifact.
## Single final fragment bind (P3, default OFF)

`ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND=ON` (recipe environment
`ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=1`) requires staging proof and the neutral
fastpath. Optional patch 0014 follows 0012 and, when present, 0013. Only custom
DrawElements delays the initial stock fragment-program bind: blend/float state,
shader rebuild and invalidation still happen before texture/vertex preparation.
The existing variant selector then binds its winner, or explicitly binds
`p->fprog` on rejection, before the unchanged vertex-program and uniform uploads.
Other draw paths retain their original prepare-plus-bind operation. OFF does not
apply 0014 and leaves the generated vitaGL source byte-identical.

This removes a redundant state transition, not shader work. Existing `ph120.q`
fragment set hit/miss counts measure engagement.

## Plain fragment FP16 (isolated candidate, default OFF)

`ISAAC_VITA_COLOROFFSET_PLAIN_FP16=ON` (recipe environment
`ISAAC_COLOROFFSET_PLAIN_FP16=1`) requires the plain fastpath and its existing
staging/neutral prerequisites. It is mutually exclusive with
`ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR`: both CMake and the native recipe reject
that combination. It changes only the optional private 0013 helper;
there is no new vitaGL source patch or public shader-interface change. The
recipe copies and hashes `isaac_coloroffset_plain_fp16.h` and passes
`-DHAVE_ISAAC_COLOROFFSET_PLAIN_FP16=1` through native `EXTRA_CFLAGS`. The
option and header participate in the archive dependency key; translated guest
units receive no new definition.

The helper must retain the ordinary plain shader when the candidate
compilation or baseline-interface comparison fails. OFF keeps the ordinary
plain fragment path.

Only the texture sample and tint locals become `half4`; their multiplication
uses native `*=`, which the pinned GLSL translator does not rewrite through
its float-only `vglMul` overload. Varying declarations, UV arithmetic, sampler,
original vertex program, blend and output format stay unchanged. This can round
colour/alpha before the existing UCHAR4 output; floating-point render targets
always retain baseline P2. Existing all-vertex plain admission is unchanged.

Baseline P2 is compiled/registered and retained independently. Before registering
the candidate, the helper checks the actual compiled baseline and candidate GXP
program flags, complete 32-byte varying/output block (except the relocated table
pointer), and every byte of every 16-byte iterator descriptor. A changed iterator
width, precision or register map rejects the candidate. Candidate compile,
validation, registration, native variant creation or bounded-cache failure uses
the unchanged baseline P2 selector.

`KAGE VITA PLAIN FP16: ... ready=... ... fail=...` reports link acceptance.
The existing `ph120.kp` record appends optional `half(q,b)=...`: q counts proven
plain requests consulting the option, b counts half fragments selected for final
binding, and q-b is baseline-selection/fallback requests. Existing `plain(q,b,f)`
semantics are unchanged. The half suffix is absent when its private endpoint is
not linked.

## Paired plain vertex program (isolated candidate, default OFF)

`ISAAC_VITA_COLOROFFSET_TRANSFORM_UNIFORM=ON` additionally opts into patch 0018,
requiring the stock profile and the private vertex pair. It compares exactly
64 bytes only for a clean, owned, F32 Transform uniform with no fragment alias;
already-dirty/unsupported/changed writes retain the native setter. This avoids
redundant shadow writes and potentially the following pool upload, **not** P5's
native uniform-buffer restore. No shader math or gameplay changes. Only native
vitaGL receives `HAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=1` through real
`EXTRA_CFLAGS`; OFF adds no patch/header to the materialized source. Existing
counters do not count these skips or uniform-pool reserves.

`ISAAC_VITA_COLOROFFSET_PLAIN_VERTEX_PAIR=ON` opts into patch 0015 only after
0014. It requires `ISAAC_VITA_COLOROFFSET_PLAIN_FASTPATH=ON` and
`ISAAC_VITA_COLOROFFSET_SINGLE_FINAL_BIND=ON`, including their existing neutral
and staging prerequisites. The private integration header is
`isaac_coloroffset_plain_vertex_pair.h`. Only the native vitaGL archive receives
the compile definition; translated guest units and simulation are unchanged.
OFF omits the patch.

The fixed native layout reads Position/Color/UV at offsets 0/12/28, keeps the
88-byte staged input and the original public reflection, and preserves the
source matrix expression. Native metadata must prove a 64-byte F32 Transform
buffer at the original offset/container and the original Color0/UV output
width/precision. Both private stages are selected together; failure retains
P2 with the original VS. No vertex-layout cache or draw-time compile is added.
Uniform reuse restores the existing buffer on private-path draws and the
first return to stock when it is not dirty; this introduces a state call,
not a new allocation or upload. Startup `KAGE VITA PLAIN VERTEX PAIR` reports
link readiness. Existing `plain(q,b,f)` covers both plain variants unchanged.
When the private-pair option is ON, the same `ph120.kp` line also contains
`pair(q,b,f)`: proven PLAIN requests reaching pair selection, both stages
selected for final binds, and fallback to P2/original VS, respectively.
`q=b+f`; `b` measures selection, not native bind API return status. The private
getter takes and zeros exactly three uint32 words (ABI 1); invalid requests
do not consume. OFF omits the endpoint and suffix. Worst-case `ph120.kp` is
469 bytes, below the existing 512-byte durable formatter limit.
# Optional immutable ColorOffset sampler link proof (0022)

`ISAAC_COLOROFFSET_LINK_PROOF=1` is a native-only, default-OFF build option;
the CMake-facing name is `ISAAC_VITA_COLOROFFSET_LINK_PROOF`. It requires the
existing exact ColorOffset GPU optimizations, but not profiling, staging or a
particular fragment-probe mode. It does not change a shader, GL state, draw
order, render-target lifetime, uploaded bytes or image quality.

The existing draw proof still checks every original source/compile/link
generation, exact-source flag, compile result, fragment patcher id, vertex GXP
pointer and vertex generation. Only its five immutable `Texture0` reflection
calls move to authenticated link completion. A positive proof remembers the
exact owned fragment GXP pointer. The new pointer is cleared at program create,
delete, before **every** link attempt (including early failures), and before
recording any new link result. Native shader source/binary/compile paths retain
their existing generation invalidation; attached deleted shaders remain owned
until their existing reference count reaches zero. No GXP allocation is kept
alive by this non-owning pointer.

With 32-bit pointers this adds one four-byte member to each of the existing
1,024 native program slots (4 KiB of field storage; no allocation or public ABI
change). An admitted draw replaces five metadata calls with a pointer
comparison. Existing `ph120.k/kp/q` counts keep their original meanings.
Build identity records `coloroffset_link_proof` and
`coloroffset_link_proof_patch_sha256`.

Focused local validation regenerates the pinned source, applies 0022 after
each supported ColorOffset source chain, and compiles the actual extracted
native link/draw/generation helpers with controlled GXM reflection responses:

```sh
python recomp/vita/test_vitagl_coloroffset_link_proof.py \
  --stock-tar /path/to/vitaGL-73dd57a8857f89f2353881c6de5891959c5c1983.tar.gz \
  --cc clang --output /path/to/host-output
```

The test checks both feature states, every existing runtime gate, link
reflection failures, failed/repeated compile and link, same-address reuse,
generation overflow poison, reattachment and slot clearing. It verifies the
normal, vanilla-cache and authenticated-cache source invalidation wiring.

## Skip empty fragment default-uniform uploads (0023, default OFF)

`ISAAC_VITA_SKIP_ZERO_FRAGMENT_UNIFORM=ON` selects the native-only build option
`ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1`, passed to the compiler as
`HAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1`. Only the stock vitaGL profile is
required; this is independent of ColorOffset, P5, caches and profiling.
OFF does not apply the patch. No generated guest source or public ABI changes.

vitaGL includes samplers in `frag_uniforms` even when the compiled fragment
program reports a zero-byte default uniform buffer. In both native
`upload_uniforms` macro variants, 0023 skips only that empty buffer's reserve,
default-buffer setter and zero-byte copy. Dirty clearing and the named-UBO loop
remain in their original positions. Nonzero uploads, vertex uniforms, shader
switches, temporary clear/scissor buffers and existing restore calls are
unchanged. The last nonempty `vgl_def_frag_buf` is not cleared or replaced by
an empty upload; it owns no new memory and does not prolong any allocation.

The boundary is supported by two primary implementations, not just a GXP
header inference:

- [Vita3K SceGxm at ea0795c](https://github.com/Vita3K/Vita3K/blob/ea0795ce3cd371894a4a22c3624d8e402b6afbdc/vita3k/modules/SceGxm/SceGxm.cpp):
  reserving a zero-size fragment default buffer succeeds with NULL; draw-time
  uniform submission ignores size-zero slots regardless of the previous
  pointer. Selecting a fragment program does not reset the buffer pointer.
- [libvita2d texture draws at a8f15ab](https://github.com/xerpi/libvita2d/blob/a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed/libvita2d/source/vita2d_texture.c):
  ordinary texture draws set no fragment default buffer; tint draws reserve
  and write one. Both public paths select their program independently and
  support ordinary/tint switching without binding a dummy empty buffer.

Thus first use with no prior buffer and nonzero-to-zero switching consume no
default-buffer bytes. On zero-to-nonzero switching, existing `glUseProgram`
dirty handling installs the new nonempty buffer before drawing.
The P5 draw count is **not** an empty-upload count. Existing profile
records retain their meanings; no per-draw counters, clocks or logs are added.
Build identity records `skip_zero_fragment_uniform` and its patch SHA256.

The focused fixture regenerates pinned native source and compiles the actual
fragment branches from both macro variants, their unchanged named-UBO loop,
and the actual circular-pool reserve/default-buffer restore helpers. It checks
OFF/ON, first use, clean/dirty state, zero and nonzero sizes, repeated shader
switches, pool wrap, temporary clear restoration and changed named bindings:

```sh
python recomp/vita/test_vitagl_zero_fragment_uniform.py \
  --stock-tar /path/to/vitaGL-73dd57a8857f89f2353881c6de5891959c5c1983.tar.gz \
  --cc clang --output /path/to/host-output
```

## Optional shader-cache observer compatibility

`ISAAC_SHADER_CACHE_OBSERVER_COMPAT=1` is a default-OFF experiment allowing
persistent custom-shader records to be shared between otherwise identical
`GL_TIME_PROFILE`/`NATIVE_RESOURCE_PROFILE` variants. It requires
`ISAAC_SHADER_CACHE=1`. It does not share arbitrary native recipes or import
old cache records into a new namespace; the first compatibility-enabled run
can still be cold.

The **full native recipe** retains both real observer settings and all existing
inputs. Enabling compatibility appends `shader-cache-observer-compat-v1=1`,
so toggling it changes the native source directory/marker and cannot silently
reuse an archive containing the wrong cache key. A separate shader-cache digest
uses the same `emit_recipe_inputs` list, normalizing exactly those two boolean
values and their two complete `HAVE_ISAAC_*` compiler tokens. PHASE_PROFILE,
all other options/defines, and every source/patch/helper/build-script hash
(including disabled patches) remain exact. OFF emits the previous 79-input
sequence unchanged.

`recipe.txt` and the native marker remain the full artifact identity. The build
contract additionally records `shader_cache_observer_compat`,
`shader_cache_identity` and `shader_cache_build_sha256`; only the last digest
feeds `ISAAC_SHADER_CACHE_BUILD_HEX`. Identity policy is either
`full-native-recipe` or `observer-gl-time-native-resource-v1`.

The narrow compatibility proof is source-based:

- 0005/0006 touch only GXM scene/RT/depth timing and counters in `gxm.c`.
  They do not change shader strings, GLSL translation, compiler arguments,
  `binds_map`, matrix/UBO metadata, or shader publication/load branches.
- 0020 touches RT/depth lifetime observations, allocator recovery and GXM
  timing. Its `shared.h` include redirects only `sceGxmFinish`,
  `sceGxmCreateRenderTarget`, and `sceGxmDestroyRenderTarget`; it does not
  redirect shader compilation, GXM program checks/register, or change existing
  shader/metadata layouts. The 0021 NATIVE_RESOURCE conditional adds RT-reuse
  reason counters and retirement observations only.
- Neither observer macro occurs in cache policy/integration/block-list code
  or pinned translator/source strings. Existing downloaded DIAG/LEAN
  `custom_shaders.c` are byte-identical.

All source+peer hashes, stage/translation/compiler-option inputs, bounded file
and metadata validation, GXM Check/Size/Type/Register checks, source-preserving
fallback, exclusive temporary files, readback, fd/device sync and rename
behavior are unchanged.

The existing `test_vitagl_shader_cache.py` includes
`prove_observer_recipe_identity()`, which executes the actual pure shell
recipe fragment without downloading or building. It proves all four observer
combinations share only the ON cache digest, keep distinct native recipes,
OFF cache digest equals native, ON/OFF artifact identities differ, original
input byte ordering is preserved, and all 76 other inputs plus lookalike
compiler tokens remain significant.

## Optional draw-local ColorOffset metadata reuse

`ISAAC_COLOROFFSET_METADATA_ONCE=1` (CMake:
`ISAAC_VITA_COLOROFFSET_METADATA_ONCE=ON`) is default OFF. It reuses the first
metadata-only admission result when halo staging fails and normal staging is
entered in the same draw. Vertex bytes are still read and copied in their
original order, and normal neutral/PLAIN proof is unchanged. There is no
cross-draw cache or new timer.

Reuse additionally requires unchanged `is_fbo_float`. A lazy framebuffer
attachment update followed by allocation recovery can refresh this value
between the two original checks. On either 0-to-1 or 1-to-0 transition, the
second full predicate runs exactly as before. This change does not correct
the underlying lazy-attachment ordering or alter its stale-at-ingress result.

The supported recipe requires HALO_CLIP, PLAIN_VERTEX_PAIR, TRANSFORM_UNIFORM,
LINK_PROOF and SKIP_ZERO_FRAGMENT_UNIFORM. Both supported complete source
identities (atlas-nearest OFF/ON) are derived from their previously accepted
full-file hashes; other combinations fail closed. Native explicit zero,
HALO-OFF and `HAVE_TEX_CACHE` retain the original evaluation sequence. The
TEX_CACHE fixture checks that exclusion only, not support for the otherwise
incompatible HALO+P8+TEX_CACHE recipe.

The new functional flag and patch hash participate in both native and shader
cache identities, including when OFF; they are not observer-normalized.

Focused host tests freshly apply both complete patch chains and execute the
actual native predicate/staging block. They cover halo success/rejection,
allocation failure/recovery, lazy attachment changes in both directions,
unchanged stale state, live vertex reads and cross-draw changes. The excluded
modes preserve baseline host objects (apart from the COFF timestamp).

## Optional counter-only halo admission profile

`ISAAC_LASER_HALO_PROFILE=1` (CMake `ISAAC_VITA_LASER_HALO_PROFILE=ON`) is
default OFF; native `HAVE_ISAAC_LASER_HALO_PROFILE=0` is also OFF. This mode
requires PHASE_PROFILE, HALO_CLIP and the current authenticated METADATA_ONCE
recipes. GL_TIME and NATIVE_RESOURCE are not
required, and this observer adds no clocks.

`vglGetIsaacLaserHaloStats` is a separate strong private ABI-2 endpoint, 14
uint32 words. An old request for 11 words still writes exactly the legacy
44-byte prefix with ABI=1; both forms are nonconsuming. It never extends
vitaGL.h or the 31-word GPU draw ABI/inner laser
snapshots. Counters are cumulative modulo 2^32. The phase owner reads them at
the initial baseline and once per completed 120-loop window, then appends one
`ph120.ha` record **after all legacy records**:

```
ph120.ha bid=... win=... loops=... abi=2 from=... attempt=... reject(meta,limits,state,plain,uv,output)=... staged(light,cap)=... big=... white_on=1 ordinary(p,w)=...
```

`attempt` counts the existing packed non-VBO client-copy opportunities just
before the first metadata predicate: not all draws, not yet proven 88-byte
layout, and not known to be lasers. The eight terminal outcomes partition
attempts modulo 2^32. They mean first rejection at metadata; input/size/shape
limits; GL/sampler/owned-atlas state; neutral/PLAIN values; remaining exact
UV/crop/lane-range/topology proof; unpublished output allocation/float-veto/
emission; or complete staged Light/cap publication. `big` is only the subset
of limits whose already-reached first failing check was vertex_count > 256 or
index_count > 768. It is not another terminal bucket, or a measurement of
oversized laser batches. `uv` does not exclusively mean mixed UV. `output`
does not exclusively mean OOM. Staged geometry is not a successful SDK draw
or GPU execution/time measurement.

The WHITE census runs only when both this observer and the default-OFF
`ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION` are enabled; no new option enables
either one implicitly. Its private traced ordinary-copy entry reuses the
already loaded raw Color RGBA bound words before sign masking. Every copied
vertex, including any unused indices below top_idx, must contain exact +1 in
all four Color lanes. The 704-byte cached chunks supply both proof and upload;
no client reread, GPU readback, allocation, per-draw clock or drawing change.
The original nontracking functions/signatures remain unchanged. Any later
non-PLAIN/rejected/unknown result cancels the whole token. Halo shortcut draws
do not inherit ordinary WHITE proof; failed halo followed by ordinary copying
can produce one. The token is draw-local, not a cross-draw/cache state.

`ordinary(p,w)` counts ordinary PLAIN requests and their all-white subset,
once at the final staged/single-stream/PLAIN request gate before shader
selection, with the current float-output veto (after possible opaque-selector
recovery). Ordinary copying remains behind its existing post-allocation
metadata admission/recheck. Shader/cache refusal does not turn a request into
success.
`white_on=0` omits `ordinary`: disabled/rebaseline intervals and legacy ABI1
records have UNKNOWN white metrics, never measured zeros. A mode transition
rebaselines white independently of the unchanged halo population. ABI2's
maximal healthy record is 341 bytes including newline, below 512.

The old short-circuit checks, client snapshot, allocation order, both
classifications (first admission and emission recheck), geometry, publication
and nearest behavior remain intact. The first classifier returns a local
reason; emission's second classifier does not report one. The owner commits
once, including metadata failure; a fallback metadata recheck is not another
attempt. No failed guard probes later pointers to classify its rejection.

Invalid getter/ABI/baseline produces `abi=0 from=...` without count fields;
the next valid snapshot only rebaselines. Missing old-log records are unknown,
not zero. A requested profile with no native endpoint fails linking.

The observer's flag/0027/API/0032 hashes have a native-only recipe suffix, and its
exact compiler define is normalized only by observer-compatible shader keys.
The functional prefix, including staging and both halo helper hashes and the
build-script hash, remains significant in both identities.
Whole-source shader gates map the two known post-0026 hashes to freshly
derived post-0027 and post-0032 hashes; no identity is bypassed.

Existing P8, freshly regenerated metadata-owner, phase-profile and shader
cache fixtures cover reason accounting/poison exits, size subset, modular
wrap, recovery/publication, OFF/zero, ABI loss/rebaseline, record bounds and
same-revision identity.
