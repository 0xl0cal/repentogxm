# Stock vitaGL reference profile

This recipe builds the vitaGL that the shipped renderer links when
`ISAAC_VITA_VITAGL_STOCK_REFERENCE=ON`; the perf builds recorded in
`../../STATUS.md` use it. It pins upstream vitaGL commit
`73dd57a8857f89f2353881c6de5891959c5c1983`. Patch 0001 changes only
deterministic archive/debug-path mechanics and backports the one-line
`f24ad3e66f7f34bebe70598d1302c34f4eff4a54` initialization out-of-bounds fix.
The optional patches 0002 to 0005 are applied according to the environment
switches below:

| build.sh environment | CMake option | patch |
|---|---|---|
| `ISAAC_GPU_DRAW_OPTIMIZATIONS=1` | `ISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS` | 0002 |
| `ISAAC_CANONICAL_QUAD_ZERO_COPY=1` | `ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY` | compile flag on patch 0002 |
| `ISAAC_GXM_STATE_SHADOW=1` | `ISAAC_VITA_GXM_STATE_SHADOW` | compile flag on patch 0002 |
| `ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1` | `ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS` | 0003 |
| `ISAAC_SHADER_CACHE=1` | `ISAAC_VITA_VITAGL_SHADER_CACHE` | 0004 |
| `ISAAC_PHASE_PROFILE=1` | `ISAAC_VITA_PHASE_PROFILE` | none (counters) |
| `ISAAC_GL_TIME_PROFILE=1` | `ISAAC_VITA_GL_TIME_PROFILE` | 0005 |

The main CMake build runs this recipe in its binary directory with the
switches derived from those options.

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

The corresponding CMake switches are
`-DISAAC_VITA_CANONICAL_QUAD_ZERO_COPY=ON` and
`-DISAAC_VITA_GXM_STATE_SHADOW=ON`. With `ISAAC_VITA_PHASE_PROFILE=ON` the
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
the exact GPU draw profile. It checks the byte-identical stock ColorOffset
shader source and the captured vertex and fragment shader binaries, then
permits only an opaque no-blend fragment-program variant after a full
draw-state check. It does not replace GLSL or reduce resolution. The check,
counter ABI, and intentionally deferred simple/static shader paths are
documented in
[`COLOROFFSET_GPU_OPTIMIZATION.md`](COLOROFFSET_GPU_OPTIMIZATION.md).

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

For the measured opt-in variant:

```sh
export ISAAC_GPU_DRAW_OPTIMIZATIONS=1
bash recomp/vita/vitagl-stock-reference/build.sh \
  /tmp/isaac-vitagl-stock-reference-gpu-draw
```

The production CMake switch is
`-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=ON`; it is accepted only together
with `-DISAAC_VITA_VITAGL_STOCK_REFERENCE=ON`.

The ColorOffset switch is
`-DISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=ON` and remains off unless
explicitly selected. `-DISAAC_VITA_PHASE_PROFILE=ON` is optional: it enables
the aggregate counters but does not select or change the optimization.

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
`0005-isaac-scene-timer.patch` on top of the 0002 `gxm.c`. It brackets
`sceGxmBeginScene` and `sceGxmEndScene` in `scene_reset`/`scene_end` with two
`sceKernelGetProcessTimeWide` reads and exposes the window sums through
`vglIsaacSceneTimes(begin_us, end_us, count, begin_max_us)` (take-and-zero).
The hook is deliberately not declared in `vitaGL.h`, so the shader-cache
whole-file hashes of `custom_shaders.c`/`vitaGL.h` are unchanged; the consumer
(`kage_vita_phase_profile.c`) declares it and prints the scene-timer line
`ph120.gt`. The BeginScene sum is the CPU time blocked on the GPU (the previous
job on that render target), which is what a tile-based deferred renderer
charges a draw-target switch.

This recipe builds an ARM archive but does not run an executable or touch a
console, emulator, remote, or VitaSDK installation.

The source-only patch-chain regression requires either the GitHub source
tarball of the pinned commit or a local Git repository which contains it. It
applies the patch chain with and without `ISAAC_SHADER_CACHE=1` (patch 0004)
without building:

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
