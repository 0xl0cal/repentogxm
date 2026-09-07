#ifndef GL_VITA_COLOROFFSET_FS_PROBE_H
#define GL_VITA_COLOROFFSET_FS_PROBE_H

/* Consumer-declared ABI of the 0008 ColorOffset
 * fragment-shader probe (recomp/vita/vitagl-stock-reference/
 * 0008-isaac-coloroffset-fs-probe.patch, custom_shaders.c).  Like the gxm.c
 * scene hooks it is deliberately not declared in vitaGL.h, so the
 * shader-cache whole-file hash of that header stays valid; custom_shaders.c
 * defines the identical struct and exports the endpoint.
 *
 * Mode 1 = TRIVIAL: the exact ColorOffset program draws with
 * `Color0 * texture(Texture0, TexCoord0)` (no colorize/offset/pixelate lanes,
 * no discard).  Mode 2 = NODISCARD: the stock ColorOffset source with its
 * gl_FragCoord clip/discard statement removed, derived at runtime from the
 * authenticated source.  Both visibly change the picture; they measure the
 * fill cost of the stock ColorOffset fragment shader and never ship.
 * Mode 3 = NEUTRAL: production-eligible specialization, preserving color
 * lanes and admitted only for bounded staged batches with neutral
 * pixelation and inactive clip at every vertex.  It uses these existing
 * counters. With optional 0013, draw(q,b,o) covers the neutral family
 * (neutral plus proven plain); plain(q,b,f) reports the plain-only subset.
 * gxp/src remain the neutral program's identity; the plain identity is in
 * its startup link line, never substituted into these fields.
 *
 * Every counter is cumulative for the process.  The link fields
 * (link_attempts .. source_fnv1a) describe the probe link, which happens once
 * per exact ColorOffset program; the draw and cache fields advance per exact
 * draw and are reported as 120-loop window deltas (ph120.kp). */

#include <stdint.h>

#if !defined(ISAAC_VITA_COLOROFFSET_FS_PROBE)
# error gl_vita_coloroffset_fs_probe.h is only for ISAAC_VITA_COLOROFFSET_FS_PROBE builds
#endif
#if ISAAC_VITA_COLOROFFSET_FS_PROBE < 1 || ISAAC_VITA_COLOROFFSET_FS_PROBE > 3
# error ISAAC_VITA_COLOROFFSET_FS_PROBE must be 1 (TRIVIAL), 2 (NODISCARD) or 3 (NEUTRAL)
#endif

typedef struct vglIsaacColorOffsetFsProbeStats {
    uint32_t abi_version;      /* 1 */
    uint32_t mode;             /* HAVE_ISAAC_COLOROFFSET_FS_PROBE of the archive */
    uint32_t link_attempts;    /* exact ColorOffset programs seen at glLinkProgram */
    uint32_t link_ready;       /* probe fragment programs registered */
    uint32_t fail_source;      /* variant source unavailable or its copy failed */
    uint32_t fail_compiler;    /* shader compiler offline and could not start */
    uint32_t fail_compile;     /* vitaShaRK returned no program */
    uint32_t fail_check;       /* compiled program is not the stock interface */
    uint32_t fail_register;    /* sceGxmShaderPatcherRegisterProgram failed */
    uint32_t last_program;     /* GL program handle of the last probe link */
    uint32_t gxp_size;         /* registered probe GXP bytes */
    uint32_t gxp_fnv1a;        /* FNV-1a of the registered probe GXP */
    uint32_t source_size;      /* variant GLSL source bytes given to the translator */
    uint32_t source_fnv1a;     /* FNV-1a of that source */
    uint32_t draw_requests;    /* exact draws that consulted the probe */
    uint32_t draws_bound;      /* exact draws that bound the probe fragment program */
    uint32_t draws_opaque_overridden; /* of those, draws the 0003 opaque variant had selected */
    uint32_t fail_cache;       /* probe fragment-program cache full or allocation failed */
    uint32_t fail_create;      /* sceGxmShaderPatcherCreateFragmentProgram failed */
    uint32_t cache_creates;
    uint32_t cache_releases;
} vglIsaacColorOffsetFsProbeStats;

void vglGetIsaacColorOffsetFsProbeStats(vglIsaacColorOffsetFsProbeStats *stats);

/* Optional private 0013 endpoint, ABI 1: caller passes exactly 3 uint32_t
 * words. Take-and-zero q=eligible plain requests, b=selected plain fragments,
 * f=requests returning fallback, q=b+f. Returns 1 on success, 0 for invalid
 * arguments without consuming. The archive may omit the endpoint entirely.
 * No global target definition is needed to use the optional archive. */
#if defined(__GNUC__)
uint32_t vglTakeIsaacColorOffsetPlainStats(uint32_t *out, uint32_t words)
    __attribute__((weak));
#elif defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
uint32_t vglTakeIsaacColorOffsetPlainStats(uint32_t *out, uint32_t words);
#endif

/* Optional private 0015 endpoint, ABI 1: same three-word take-and-zero
 * contract. q = proven PLAIN requests consulting the private VS+FS pair;
 * b = both stages selected for the final binds; f = P2/original VS fallback.
 * q=b+f. No native-bind return status is implied. Absent when 0015 is OFF. */
#if defined(__GNUC__)
uint32_t vglTakeIsaacColorOffsetPlainVertexPairStats(uint32_t *out, uint32_t words)
    __attribute__((weak));
#elif defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
uint32_t vglTakeIsaacColorOffsetPlainVertexPairStats(uint32_t *out, uint32_t words);
#endif

/* Optional private FP16 endpoint, ABI 1: caller passes exactly two words.
 * Take-and-zero q = proven PLAIN requests consulting the half fragment;
 * b = half fragment selected, so q-b is fallback. Returns 1 on success,
 * 0 for invalid arguments without consuming. Absent when FP16 is OFF. */
#if defined(__GNUC__)
uint32_t vglTakeIsaacColorOffsetPlainFp16Stats(uint32_t *out, uint32_t words)
    __attribute__((weak));
#elif defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
uint32_t vglTakeIsaacColorOffsetPlainFp16Stats(uint32_t *out, uint32_t words);
#endif

_Static_assert(sizeof(vglIsaacColorOffsetFsProbeStats) == 21u * sizeof(uint32_t),
               "vitaGL ColorOffset FS probe statistics ABI drifted");

#endif
