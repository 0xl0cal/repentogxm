#ifndef ISAAC_LASER_LIGHT_HALO_POLICY_H
#define ISAAC_LASER_LIGHT_HALO_POLICY_H

#include "isaac_coloroffset_plain_policy.h"

#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
#if !defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) || HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV != 1
#error "Proven halo emission requires producer UV and its combined output/float recovery guards"
#endif
#endif

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
#include "isaac_laser_halo_profile.h"
#define ISAAC_HALO_REASON(value) do { if (halo_reason) *halo_reason = (value); } while (0)
#define ISAAC_HALO_REASON_DECL , uint32_t *halo_reason
#define ISAAC_HALO_REASON_ARG(value) , value
#else
#define ISAAC_HALO_REASON(value)
#define ISAAC_HALO_REASON_DECL
#define ISAAC_HALO_REASON_ARG(value)
#endif

/* Visual-quality option, not an image-equivalence optimization. The exact
 * LaserEffects atlas columns 424..440 contain the opaque light-beam core.
 * Keep 421..443 plus the boundary at 444; trim only its weak outer halo.
 * Both 007.005_lightbeam and 007.008_light ring use this 416..448 strip.
 * Positions are clipped per ORIGINAL triangle. Never rebuild a quad across
 * its diagonal, alter a guest vertex, or move the opaque core/centerline.
 */
#define ISAAC_LIGHT_HALO_MAX_VERTICES 256u
#define ISAAC_LIGHT_HALO_MAX_INDICES 768u
#define ISAAC_LIGHT_HALO_LANES 22u

typedef struct { float lane[ISAAC_LIGHT_HALO_LANES]; } isaac_light_halo_vertex;

#if defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1
/* Exact logical-448 ImagePng body producer, not x/storage_width. The actual
 * guest ARM keeps f32 reciprocal, pixel multiply, then logical/storage scale.
 * At storage 448 the x416 endpoint is one ULP above direct division. */
static inline float isaac_light_halo_producer_endpoint(uint32_t width, uint32_t index)
{
    static const uint32_t bits[2][2] = {
        {0x3f6db6dcu, 0x3f800000u}, {0x3f500000u, 0x3f600000u}
    };
    float value;
    memcpy(&value, &bits[width == 512u][index], sizeof value);
    return value;
}
#endif

#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
/* Frozen ImagePng producer: f32(x * f32(1/448)) followed by
 * f32(uv * f32(448/storage_width)). Do NOT replace with x/storage_width:
 * 352 and 384 differ by one ULP. Literal bits also survive native fast-math.
 * Only this logical-448 atlas class is admitted; no epsilon/alternate encoding.
 */
static inline float isaac_red_cap_side_endpoint(uint32_t width, uint32_t index)
{
    static const uint32_t bits[2][3] = {
        {0x3f36db6eu, 0x3f492493u, 0x3f5b6db8u},
        {0x3f200000u, 0x3f300001u, 0x3f400001u}
    };
    float value;
    memcpy(&value, &bits[width == 512u][index], sizeof value);
    return value;
}
#endif

/* Return the exact crop's left atlas column, or zero on any uncertainty. */
#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
static inline uint32_t isaac_light_halo_classify_reason(
#else
static inline uint32_t isaac_light_halo_classify(
#endif
        const isaac_light_halo_vertex *vertices, uint32_t vertex_count,
        const uint16_t *indices, uint32_t index_count, uint32_t texture_width
        ISAAC_HALO_REASON_DECL)
{
    uint32_t i, lane, crop = 416u;
    float left, right;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    float cap_top = 0.0f;
#endif
    ISAAC_HALO_REASON(ISAAC_HALO_LIMITS);
    if (!vertices || !indices || !vertex_count ||
            vertex_count > ISAAC_LIGHT_HALO_MAX_VERTICES || !index_count ||
            index_count > ISAAC_LIGHT_HALO_MAX_INDICES || index_count % 3u ||
            (texture_width != 448u && texture_width != 512u))
        return 0;
    ISAAC_HALO_REASON(ISAAC_HALO_PLAIN);
    if (!isaac_coloroffset_contiguous_vertices_are_neutral(vertices,
            (size_t)vertex_count * sizeof *vertices, vertex_count,
            ISAAC_COLOROFFSET_VERTEX_STRIDE) ||
            !isaac_coloroffset_neutral_vertices_have_plain_colors(
                (const uint8_t *)vertices, vertex_count))
        return 0;
    ISAAC_HALO_REASON(ISAAC_HALO_UV);
#if defined(HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV) && HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV == 1
    left = isaac_light_halo_producer_endpoint(texture_width, 0u);
    right = isaac_light_halo_producer_endpoint(texture_width, 1u);
#else
    left = 416.0f / (float)texture_width;
    right = 448.0f / (float)texture_width;
#endif
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    if (vertices[0].lane[7] != left && vertices[0].lane[7] != right) {
        left = vertices[0].lane[7];
        cap_top = vertices[0].lane[8];
        for (i = 1u; i < vertex_count; ++i) {
            if (vertices[i].lane[7] < left) left = vertices[i].lane[7];
            if (vertices[i].lane[8] < cap_top) cap_top = vertices[i].lane[8];
        }
        if (left == isaac_red_cap_side_endpoint(texture_width, 0u)) crop = 320u;
        else if (left == isaac_red_cap_side_endpoint(texture_width, 1u)) crop = 352u;
        else return 0;
        if (cap_top != 0.0f && cap_top != 0.5f) return 0;
        right = isaac_red_cap_side_endpoint(texture_width, crop == 320u ? 1u : 2u);
    }
#endif
    for (i = 0; i < vertex_count; ++i) {
        const float *v = vertices[i].lane;
        /* The exact crop endpoints, not a UV bounding-box heuristic.
         * Inset, repeated-U and other atlas crops retain the stock draw. */
        if (v[7] != left && v[7] != right)
            return 0;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
        /* One whole 32x32 cap frame, not a batch-wide bounding box. */
        if (crop != 416u && v[8] != cap_top && v[8] != cap_top + 0.5f)
            return 0;
#endif
        for (lane = 0; lane < ISAAC_LIGHT_HALO_LANES; ++lane)
            if (v[lane] < -1048576.0f || v[lane] > 1048576.0f)
                return 0; /* Bounded interpolation, including positions. */
        for (lane = 3; lane <= 6; ++lane)
            if (v[lane] < 0.0f || v[lane] > 1.0f)
                return 0; /* Do not amplify the discarded faint halo. */
    }
    for (i = 0; i < index_count; i += 3u) {
        uint32_t a = indices[i], b = indices[i + 1u], c = indices[i + 2u];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count ||
                a == b || a == c || b == c)
            return 0;
        if (vertices[a].lane[7] == vertices[b].lane[7] &&
                vertices[a].lane[7] == vertices[c].lane[7])
            return 0;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
        if (crop != 416u && vertices[a].lane[8] == vertices[b].lane[8] &&
                vertices[a].lane[8] == vertices[c].lane[8])
            return 0;
#endif
    }
    return crop;
}

#if defined(HAVE_ISAAC_LASER_HALO_PROFILE) && HAVE_ISAAC_LASER_HALO_PROFILE
/* Checked emission retains its original classification, with no observation.
 * Only the first classifier in try_stage returns a local terminal reason. */
static inline uint32_t isaac_light_halo_classify(
        const isaac_light_halo_vertex *vertices, uint32_t vertex_count,
        const uint16_t *indices, uint32_t index_count, uint32_t texture_width)
{
    return isaac_light_halo_classify_reason(vertices, vertex_count, indices,
        index_count, texture_width, NULL);
}
#endif

static inline int isaac_light_halo_input_is_exact(
        const isaac_light_halo_vertex *vertices, uint32_t vertex_count,
        const uint16_t *indices, uint32_t index_count, uint32_t texture_width)
{
    return isaac_light_halo_classify(vertices, vertex_count, indices,
        index_count, texture_width) != 0u;
}

static inline uint32_t isaac_light_halo_clip_plane(
        isaac_light_halo_vertex *out, const isaac_light_halo_vertex *in,
        uint32_t count, float bound, int keep_greater)
{
    uint32_t size = 0u, i, lane;
    const isaac_light_halo_vertex *previous = &in[count - 1u];
    int previous_inside = keep_greater ? previous->lane[7] >= bound :
        previous->lane[7] <= bound;
    for (i = 0u; i < count; ++i) {
        const isaac_light_halo_vertex *current = &in[i];
        int inside = keep_greater ? current->lane[7] >= bound :
            current->lane[7] <= bound;
        if (inside != previous_inside) {
            float weight = (bound - previous->lane[7]) /
                (current->lane[7] - previous->lane[7]);
            if (size >= 5u || weight < 0.0f || weight > 1.0f)
                return 0u;
            for (lane = 0u; lane < ISAAC_LIGHT_HALO_LANES; ++lane)
                out[size].lane[lane] = previous->lane[lane] * (1.0f - weight)
                    + current->lane[lane] * weight;
            out[size++].lane[7] = bound;
        }
        if (inside) {
            if (size >= 5u)
                return 0u;
            out[size++] = *current;
        }
        previous = current;
        previous_inside = inside;
    }
    return size;
}

/* Each admitted triangle spans the TWO exact U endpoints. Intersecting with
 * the interior U band produces four vertices, in original winding order.
 * Emit two triangles for that polygon; each remains inside one original
 * triangle and retains all 22 interpolants. Output capacity is explicit. */
static inline int isaac_light_halo_emit(
        isaac_light_halo_vertex *output_vertices, uint32_t vertex_capacity,
        uint16_t *output_indices, uint32_t index_capacity,
        const isaac_light_halo_vertex *vertices, uint32_t vertex_count,
        const uint16_t *indices, uint32_t index_count, uint32_t texture_width)
{
    uint32_t i, out = 0u, index = 0u, crop;
    float left, right;
    if (!output_vertices || !output_indices)
        return 0;
    crop = isaac_light_halo_classify(vertices, vertex_count, indices,
        index_count, texture_width);
    if (!crop ||
            vertex_capacity < (index_count / 3u) * 4u ||
            index_capacity < index_count * 2u)
        return 0;
    left = 421.0f / (float)texture_width;
    right = 444.0f / (float)texture_width;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    if (crop != 416u) {
        /* Exact red caps have alpha support at local U [4,28). Keep one
         * blank texel on each side, and ALL V: rows outside the cap can be
         * visible under filtering. This path separately requires POINT. */
        left = (float)(crop + 3u) / (float)texture_width;
        right = (float)(crop + 29u) / (float)texture_width;
    }
#endif
    for (i = 0u; i < index_count; i += 3u) {
        isaac_light_halo_vertex polygon[5], clipped[5];
        uint32_t count;
        polygon[0] = vertices[indices[i]];
        polygon[1] = vertices[indices[i + 1u]];
        polygon[2] = vertices[indices[i + 2u]];
        count = isaac_light_halo_clip_plane(clipped, polygon, 3u, left, 1);
        if ((count != 3u && count != 4u) ||
                isaac_light_halo_clip_plane(polygon, clipped, count, right, 0) != 4u)
            return 0;
        memcpy(output_vertices + out, polygon, 4u * sizeof *polygon);
        output_indices[index++] = (uint16_t)out;
        output_indices[index++] = (uint16_t)(out + 1u);
        output_indices[index++] = (uint16_t)(out + 2u);
        output_indices[index++] = (uint16_t)out;
        output_indices[index++] = (uint16_t)(out + 2u);
        output_indices[index++] = (uint16_t)(out + 3u);
        out += 4u;
    }
    return 1;
}

#if defined(HAVE_ISAAC_LASER_HALO_PROVEN_EMIT) && HAVE_ISAAC_LASER_HALO_PROVEN_EMIT == 1
/* Private native seam: vertices/indices/count/width/crop must be the SAME
 * immutable stack snapshot accepted by classify before the reservation.
 * The allocator receives only a byte count, never these snapshot addresses.
 * Keep checked emit above literally unchanged, including in OFF builds.
 * This intentionally duplicates its emission tail: the checked API remains
 * an independent validation boundary, not a way to forge a proven crop.
 */
static inline int isaac_light_halo_emit_proven(
        isaac_light_halo_vertex *output_vertices, uint32_t vertex_capacity,
        uint16_t *output_indices, uint32_t index_capacity,
        const isaac_light_halo_vertex *vertices, const uint16_t *indices,
        uint32_t index_count, uint32_t texture_width, uint32_t crop)
{
    uint32_t i, out = 0u, index = 0u;
    float left, right;
    if (!output_vertices || !output_indices ||
            (crop != 416u
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
             && crop != 320u && crop != 352u
#endif
            ) || vertex_capacity < (index_count / 3u) * 4u ||
            index_capacity < index_count * 2u)
        return 0;
    left = 421.0f / (float)texture_width;
    right = 444.0f / (float)texture_width;
#if defined(HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP) && HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP == 1
    if (crop != 416u) {
        left = (float)(crop + 3u) / (float)texture_width;
        right = (float)(crop + 29u) / (float)texture_width;
    }
#endif
    for (i = 0u; i < index_count; i += 3u) {
        isaac_light_halo_vertex polygon[5], clipped[5];
        uint32_t count;
        polygon[0] = vertices[indices[i]];
        polygon[1] = vertices[indices[i + 1u]];
        polygon[2] = vertices[indices[i + 2u]];
        count = isaac_light_halo_clip_plane(clipped, polygon, 3u, left, 1);
        if ((count != 3u && count != 4u) ||
                isaac_light_halo_clip_plane(polygon, clipped, count, right, 0) != 4u)
            return 0;
        memcpy(output_vertices + out, polygon, 4u * sizeof *polygon);
        output_indices[index++] = (uint16_t)out;
        output_indices[index++] = (uint16_t)(out + 1u);
        output_indices[index++] = (uint16_t)(out + 2u);
        output_indices[index++] = (uint16_t)out;
        output_indices[index++] = (uint16_t)(out + 2u);
        output_indices[index++] = (uint16_t)(out + 3u);
        out += 4u;
    }
    return 1;
}
#endif
#endif
