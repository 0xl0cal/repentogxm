# Exact ColorOffset GPU optimization

This default-off profile keeps the game's `resources/shaders/coloroffset.fs`
byte-for-byte unchanged.  The typed GL boundary only authenticates the complete
OpenGL `count`/`length` concatenation, calls stock `glShaderSource` once with the
original arguments, and then asks vitaGL to recheck its owned source copy.

The production identity is the 2,052-byte CRLF asset with full FNV-1a
`2b3ccf4a` and first-512-byte FNV-1a `75ef2322`.  The linked programs must also
match the complete captured binaries:

- VS: 562 bytes, FNV-1a `4f7dce51`, first 512 `338d5584`, SHA-256
  `ae60775149e43dad58943fe2d000358d3fb8dadd2bc7051557d3ca0d3b88be8a`;
- FS: 809 bytes, FNV-1a `b6517f58`, first 512 `c26317f4`, SHA-256
  `e9e6df192297ff4fdc723dd0d5f62e9ad79e902e13842a3fb07784c6dabdc574`.

Only the fragment-program blend object changes.  A draw may select the cached
`SCE_GXM_BLEND_FUNC_NONE` variant when all of these runtime proofs hold:

- exact source/compile generation, compiled FS, linked VS generation, sampler
  parameter and full 8-attribute/88-byte layout identity;
- ADD blending with `ONE` or `SRC_ALPHA` as the color source,
  `ONE_MINUS_SRC_ALPHA` as its destination, complete color mask, and `ONE` or
  `SRC_ALPHA` as the alpha source;
- an owned `TEX_VALID`, live 432x240 `U8U8U8_BGR` descriptor confirmed by both
  vitaGL's format field and the GXM descriptor getter;
- the complete four-vertex staged range and the bounded original 16-bit,
  six-index payload (before vitaGL's index-width downgrade),
  with every referenced `Color.a` bit-exact 1.0, default effect lanes, and
  finite `PixelationAmount <= 0`;
- UCHAR4 output, current MSAA mode, and the exact 0,0,960,540 logical viewport.

RGB24 sampling supplies alpha one, the stock shader writes `Color.a`, and the
vertex gate proves that value is one.  Therefore both accepted color-source
factors evaluate to one and the captured ADD blend is mathematically identical
to no blending.  The physical ColorOffset receipt is raw `0x5151110f`: complete
mask, ADD/ADD, `ONE`/`ONE_MINUS_SRC_ALPHA` for color and alpha.  Clip/discard
and every stock shader instruction remain present.  Any uncertain field
follows the already-bound stock fragment path.

The cache key contains the fragment ID/source generation, linked VS pointer,
raw blend state, output format, MSAA, texture format/dimensions, and viewport.
Every completed shader compile or cache load advances its lifecycle generation;
the FS receipt is rechecked there and again at link, and the VS receipt at link,
so recompilation cannot reuse an old identity bit.  Direct GXP replacement also
invalidates the generation.  There are at most four variants per GL program.  Program deletion
finishes the GXM context, releases each variant once, and only then permits
attached shader objects to be unregistered.

The physical display remains 960x544, the logical viewport remains 960x540,
and the source texture remains 432x240.  This profile performs no raster,
filtering, resolution, or visual-quality downgrade.

## Deliberately deferred paths

The proposed GLSL early return was excluded.  A texture lookup inside a branch
driven by interpolated varyings can be dynamically divergent, making implicit
texture derivatives undefined for draws outside the measured default case.
The stock source is therefore never rewritten.

A separate simple FS and the stronger static-default VS/FS pair are also
deferred.  There is no independently compiled matching GXP receipt or proven
shader-pair/lifecycle path yet, so this bundle does not remove five attributes,
varyings, or discard.  `static_eligible`, `static_hits`, and `static_pixels`
remain zero by construction.  Because no alternate source is compiled, a
replacement-compile fail-open is not exercised; any future alternate FS must
retain and retry the authenticated original before it can enter this profile.

## Selection and counters

Enable all required default-off switches:

```text
-DISAAC_VITA_VITAGL_STOCK_REFERENCE=ON
-DISAAC_VITA_VITAGL_GPU_DRAW_OPTIMIZATIONS=ON
-DISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS=ON
```

Add `-DISAAC_VITA_PHASE_PROFILE=ON` only for measurement builds.  It enables
the ColorOffset and common GPU-draw proof counters; production `OFF` keeps the
same authenticated shader/cache/fallback behavior without their hot updates.

The separate ABI is 24 consecutive `uint32_t` values, in order:

```text
abi_version, enabled, source_selected, source_mark_rejected,
compile_successes, compile_failures, exact_draw_requests, opaque_eligible,
opaque_hits, opaque_viewport_pixels, fail_program, fail_blend, fail_sampler,
fail_layout, fail_vertex_range, fail_vertex_value, fail_output, fail_cache,
cache_hits, cache_creates, cache_releases, static_eligible, static_hits,
static_pixels
```

Once per 120 guest loops, `ph120.k` emits the same deltas as
`src=selected/rejected/compile-ok/compile-fail`,
`opq=requests/eligible/hits/viewport-pixel-upper-bound`, the eight failure
counts, three cache counts, and the three static counts.  The fourth opaque
value is explicitly a CPU-visible viewport upper bound, not a GPU fragment counter;
clip, depth, and scissor can reduce actual fragments.  There are no
per-draw prints.

`test_coloroffset_gpu_bundle.ps1` runs without game data.  Supplying an owned
source asset plus the two captured GXP paths additionally verifies the exact
production receipts; none of those owned files is copied into the repository.
