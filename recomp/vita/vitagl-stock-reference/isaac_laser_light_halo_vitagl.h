#ifndef ISAAC_LASER_LIGHT_HALO_VITAGL_H
#define ISAAC_LASER_LIGHT_HALO_VITAGL_H

#if !defined(HAVE_ISAAC_LASER_ATLAS_P8) || \
    !defined(HAVE_ISAAC_COLOROFFSET_STAGING_PROOF) || \
    !defined(HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH)
#error "Light halo clip requires exact owned P8 and the plain staging proof"
#endif
#include "isaac_laser_light_halo_policy.h"

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
static vglIsaacLaserHaloStats isaac_halo_stats = {
    .abi_version = ISAAC_LASER_HALO_STATS_ABI,
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
    .white_enabled = 1u,
#endif
};
uint32_t vglGetIsaacLaserHaloStats(vglIsaacLaserHaloStats *out, uint32_t words)
{
    if (!out || (words != ISAAC_LASER_HALO_STATS_WORDS &&
                 words != ISAAC_LASER_HALO_STATS_LEGACY_WORDS)) return 0u;
    memcpy(out, &isaac_halo_stats, words * sizeof(uint32_t));
    if (words == ISAAC_LASER_HALO_STATS_LEGACY_WORDS) {
        const uint32_t legacy_abi = 1u;
        memcpy(out, &legacy_abi, sizeof legacy_abi);
    }
    return 1u;
}
static inline void isaac_halo_profile_commit(uint32_t outcome)
{
    ++isaac_halo_stats.attempts;
    if (outcome == ISAAC_HALO_TOO_BIG) {
        ++isaac_halo_stats.too_big;
        outcome = ISAAC_HALO_LIMITS;
    }
    ++isaac_halo_stats.outcome[outcome];
}
#if defined(HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION) && HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION
/* Called once at the final ordinary PLAIN request gate, never by a helper
 * or the halo shortcut. This measures eligible requests, not successful
 * shader selection, SDK submission, fragments or GPU time. */
static inline void isaac_white_profile_commit(uint32_t ordinary_proof)
{
    if (ordinary_proof == 1u || ordinary_proof == 2u) {
        ++isaac_halo_stats.ordinary_plain_requests;
        isaac_halo_stats.ordinary_white_requests += ordinary_proof == 2u;
    }
}
#endif
/* Preserve the original short-circuit expression and evaluate each operand
 * once. A failed operand records its existing reason, never probes later
 * pointers/state to infer why an aggregate returned false. */
#define ISAAC_HALO_REJECT(condition, reason) \
    ((condition) ? (ISAAC_HALO_SET_LOCAL(reason), 1) : 0)
#define ISAAC_HALO_SET_LOCAL(reason) (halo_reason ? (void)(*halo_reason = (reason)) : (void)0)
#else
#define ISAAC_HALO_REJECT(condition, reason) (condition)
#endif

/* Called only at the non-VBO, packed 88-byte copy boundary, after exact
 * shader/layout metadata admission. Client vertices and indices are copied
 * ONCE to cached stack storage; classification and emission consume that same
 * snapshot. No GPU readback, guest mutation, retained client pointer or
 * per-draw heap allocation is used. Temporary outputs have normal GPU pool
 * lifetime. Publication to the caller happens only after complete emission.
 */
static GLboolean isaac_light_halo_try_stage(texture *tex, uint32_t sampler_unit,
        const void *source, uint32_t vertex_count, const uint16_t *indices,
        GLsizei index_count, IsaacLightHaloDraw *submission, void **destination,
        uint32_t *new_vertex_count ISAAC_HALO_REASON_DECL)
{
    static uint32_t logged;
    isaac_light_halo_vertex snapshot[ISAAC_LIGHT_HALO_MAX_VERTICES];
    uint16_t index_snapshot[ISAAC_LIGHT_HALO_MAX_INDICES];
    isaac_light_halo_vertex *output;
    uint16_t *output_indices;
    uint32_t width, premultiplied, vertices, count, crop;
    size_t vertex_bytes, index_bytes;
    uintptr_t source_address = (uintptr_t)source;
    uintptr_t index_address = (uintptr_t)indices;
    ISAAC_HALO_REASON(ISAAC_HALO_STATE);
    if (ISAAC_HALO_REJECT(!submission || submission->primitive != SCE_GXM_PRIMITIVE_TRIANGLES ||
            !destination || !new_vertex_count || !source || !indices ||
            !vertex_count, ISAAC_HALO_LIMITS) ||
            ISAAC_HALO_REJECT(vertex_count > ISAAC_LIGHT_HALO_MAX_VERTICES, ISAAC_HALO_TOO_BIG) ||
            ISAAC_HALO_REJECT(index_count <= 0, ISAAC_HALO_LIMITS) ||
            ISAAC_HALO_REJECT(index_count > (GLsizei)ISAAC_LIGHT_HALO_MAX_INDICES, ISAAC_HALO_TOO_BIG) ||
            ISAAC_HALO_REJECT(index_count % 3, ISAAC_HALO_LIMITS) ||
            !tex || sampler_unit >= COMBINED_TEXTURE_IMAGE_UNITS_NUM ||
            samplers[sampler_unit] || tex->overridden || tex->use_mips ||
            tex->mip_count != 1 ||
            (tex->min_filter != SCE_GXM_TEXTURE_FILTER_POINT &&
             tex->min_filter != SCE_GXM_TEXTURE_FILTER_LINEAR) ||
            (tex->mag_filter != SCE_GXM_TEXTURE_FILTER_POINT &&
             tex->mag_filter != SCE_GXM_TEXTURE_FILTER_LINEAR) ||
#if !defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) || HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX != 1
            depth_test_state ||
#endif
            stencil_test_state || !blend_state ||
            polygon_mode_front != SCE_GXM_POLYGON_MODE_TRIANGLE_FILL ||
            polygon_mode_back != SCE_GXM_POLYGON_MODE_TRIANGLE_FILL ||
            blend_func_rgb != SCE_GXM_BLEND_FUNC_ADD ||
            blend_func_a != SCE_GXM_BLEND_FUNC_ADD ||
            blend_color_mask != SCE_GXM_COLOR_MASK_ALL ||
            (blend_dfactor_rgb != SCE_GXM_BLEND_FACTOR_ONE &&
             blend_dfactor_rgb != SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) ||
            (blend_dfactor_a != SCE_GXM_BLEND_FACTOR_ONE &&
             blend_dfactor_a != SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) ||
            (blend_sfactor_a != SCE_GXM_BLEND_FACTOR_ONE &&
             blend_sfactor_a != SCE_GXM_BLEND_FACTOR_SRC_ALPHA))
        return GL_FALSE;
    /* The separate opt-in removes ONLY the depth-test veto above. Clipping
     * still retains each original triangle's interpolated Z and attributes,
     * but its discarded faint fringe also stops covering/writing depth.
     * That can expose later geometry: this is an explicit visual tradeoff,
     * not depth-equivalent rendering. No native depth state is changed. */
    width = isaacLaserLightHaloAtlasInfo(tex, &premultiplied);
    if (ISAAC_HALO_REJECT(!width || (blend_sfactor_rgb != SCE_GXM_BLEND_FACTOR_SRC_ALPHA &&
            !(premultiplied && blend_sfactor_rgb == SCE_GXM_BLEND_FACTOR_ONE)), ISAAC_HALO_STATE))
        return GL_FALSE; /* Raw RGB-white + ONE is NOT a faint alpha halo. */
    vertex_bytes = (size_t)vertex_count * sizeof snapshot[0];
    index_bytes = (size_t)index_count * sizeof index_snapshot[0];
    if (ISAAC_HALO_REJECT(vertex_bytes > UINTPTR_MAX - source_address ||
            index_bytes > UINTPTR_MAX - index_address, ISAAC_HALO_LIMITS))
        return GL_FALSE;
    vgl_fast_memcpy(snapshot, source, vertex_bytes);
    vgl_fast_memcpy(index_snapshot, indices, index_bytes);
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
    crop = isaac_light_halo_classify_reason(snapshot, vertex_count,
        index_snapshot, (uint32_t)index_count, width, halo_reason);
#else
    crop = isaac_light_halo_classify(snapshot, vertex_count,
        index_snapshot, (uint32_t)index_count, width);
#endif
    if (!crop)
        return GL_FALSE;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    /* This option never changes sampling, even with LIGHT_NEAREST enabled.
     * Require BOTH semantic filters and the actual live descriptor. */
    if (ISAAC_HALO_REJECT(crop != 416u &&
            (tex->min_filter != SCE_GXM_TEXTURE_FILTER_POINT ||
             tex->mag_filter != SCE_GXM_TEXTURE_FILTER_POINT ||
             sceGxmTextureGetMinFilter(&tex->gxm_tex) != SCE_GXM_TEXTURE_FILTER_POINT ||
             sceGxmTextureGetMagFilter(&tex->gxm_tex) != SCE_GXM_TEXTURE_FILTER_POINT), ISAAC_HALO_STATE))
        return GL_FALSE;
#endif
    vertices = ((uint32_t)index_count / 3u) * 4u;
    count = (uint32_t)index_count * 2u;
    ISAAC_HALO_REASON(ISAAC_HALO_OUTPUT);
#if defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1
    const GLboolean combined_output = GL_TRUE; /* Body opt-in AND existing caps. */
#elif defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    const GLboolean combined_output = crop != 416u;
#endif
#if (defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1) || \
    (defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1)
    if (combined_output) {
        /* A heap-backed pool overflow is immediately marked for retirement.
         * A SECOND reservation's recovery could collect that first buffer.
         * Reserve admitted vertices+indices together: no recovery between them.
         * vertices=4*T, stride=88, so the index offset=352*T is 32-aligned.
         * Existing bounds cap the entire reservation at 93184 bytes. */
        size_t combined_vertex_bytes = (size_t)vertices * sizeof *output;
        output = gpu_alloc_mapped_temp(combined_vertex_bytes +
            (size_t)count * sizeof *output_indices);
        if (!output)
            return GL_FALSE;
        output_indices = (uint16_t *)((uint8_t *)output + combined_vertex_bytes);
    } else
#endif
    {
        output = gpu_alloc_mapped_temp((size_t)vertices * sizeof *output);
        if (!output)
            return GL_FALSE;
        output_indices = gpu_alloc_mapped_temp((size_t)count * sizeof *output_indices);
        if (!output_indices)
            return GL_FALSE; /* Unpublished pool space expires normally. */
    }
#if (defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1) || \
    (defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1)
    /* Successful temporary-pool recovery may run glFinish/scene_reset and
     * synchronize a lazily respecified float FBO. Never publish opt-in geometry
     * under that changed output format; the original caller falls back. */
    if (combined_output && is_fbo_float)
        return GL_FALSE;
#endif
#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
    /* Only these private snapshots carry the first classification proof.
     * Recovery may mutate client storage/live FBO state, not our stack copies;
     * the live float veto above is still required before emission. */
    if (!isaac_light_halo_emit_proven(output, vertices, output_indices, count,
            snapshot, index_snapshot, (uint32_t)index_count, width, crop))
#else
    if (!isaac_light_halo_emit(output, vertices, output_indices, count,
            snapshot, vertex_count, index_snapshot, (uint32_t)index_count, width))
#endif
        return GL_FALSE;
    *destination = output;
    *new_vertex_count = vertices;
    submission->indices = output_indices;
    submission->count = (GLsizei)count;
#ifdef HAVE_ISAAC_LASER_LIGHT_NEAREST
    /* Companion quality option: admission is this SAME successful clipped
     * snapshot. No second vertex read, ownership change or shared sampler edit.
     * Preserve every descriptor bit except checked min/mag filtering. */
    submission->nearest_ready = GL_FALSE;
    if (
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
            crop == 416u &&
#endif
            (sceGxmTextureGetMinFilter(&tex->gxm_tex) == SCE_GXM_TEXTURE_FILTER_LINEAR ||
             sceGxmTextureGetMagFilter(&tex->gxm_tex) == SCE_GXM_TEXTURE_FILTER_LINEAR)) {
        SceGxmTexture point = tex->gxm_tex;
        if (sceGxmTextureSetMinFilter(&point, SCE_GXM_TEXTURE_FILTER_POINT) == 0 &&
                sceGxmTextureSetMagFilter(&point, SCE_GXM_TEXTURE_FILTER_POINT) == 0) {
            submission->nearest_original = tex->gxm_tex;
            submission->nearest_point = point;
            submission->nearest_ready = GL_TRUE;
        }
    }
#endif
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    if (crop != 416u) {
        static uint32_t cap_logged;
        if (!cap_logged) {
            cap_logged = 1u;
            sceClibPrintf("Isaac red cap side: atlas=%ux64 premul=%u vertices=%u->%u indices=%u->%u crop=%u..%u keep=%u..%u v=unchanged filter=point depth-test=%u\n",
                width, premultiplied, vertex_count, vertices, (uint32_t)index_count, count,
                crop, crop + 32u, crop + 3u, crop + 29u, (unsigned)(depth_test_state != 0));
        }
        ISAAC_HALO_REASON(ISAAC_HALO_CAP);
        return GL_TRUE;
    }
#endif
    if (!logged) {
        logged = 1u;
#if defined(HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX) && HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX == 1
        sceClibPrintf("Isaac light halo: atlas=%ux64 premul=%u vertices=%u->%u indices=%u->%u crop=416..448 keep=421..444"
#if defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1
            " producer-uv=1 combined=1"
#endif
            " depth-approx=1 depth-test=%u\n",
            width, premultiplied, vertex_count, vertices, (uint32_t)index_count, count,
            (unsigned)(depth_test_state != 0));
#else
        sceClibPrintf("Isaac light halo: atlas=%ux64 premul=%u vertices=%u->%u indices=%u->%u crop=416..448 keep=421..444"
#if defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1
            " producer-uv=1 combined=1"
#endif
            "\n",
            width, premultiplied, vertex_count, vertices, (uint32_t)index_count, count);
#endif
    }
    ISAAC_HALO_REASON(ISAAC_HALO_LIGHT);
    return GL_TRUE;
}
#endif
