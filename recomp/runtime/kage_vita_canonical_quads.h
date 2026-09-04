#ifndef KAGE_VITA_CANONICAL_QUADS_H
#define KAGE_VITA_CANONICAL_QUADS_H

#include <stdint.h>

#define KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE  0x0056039du
#define KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_GLOBAL 0x0056d794u
#define KAGE_VITA_CANONICAL_QUAD_MAX_INDICES       0x0000c000u
#define KAGE_VITA_GL_TRIANGLES                     0x00000004u
#define KAGE_VITA_GL_UNSIGNED_SHORT                0x00001403u

/* test_kage_canonical_quad_callsites.py pins the sole frozen producer and its
 * +6-index/+4-vertex recurrence, not merely these two consumer return RVAs. */

typedef enum KageVitaCanonicalQuadResult {
    KAGE_VITA_CANONICAL_QUAD_OK = 0,
    KAGE_VITA_CANONICAL_QUAD_REJECT_CALLSITE,
    KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE,
    KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS,
    KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER
} KageVitaCanonicalQuadResult;

static inline KageVitaCanonicalQuadResult
kage_vita_canonical_quad_classify(
    uint32_t return_rva, uint32_t mode, int32_t count,
    uint32_t type, uint32_t indices)
{
    uint32_t byte_count;

    if (return_rva != KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE &&
            return_rva != KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_GLOBAL)
        return KAGE_VITA_CANONICAL_QUAD_REJECT_CALLSITE;
    if (mode != KAGE_VITA_GL_TRIANGLES ||
            type != KAGE_VITA_GL_UNSIGNED_SHORT)
        return KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE;
    if (count <= 0 || (uint32_t)count > KAGE_VITA_CANONICAL_QUAD_MAX_INDICES)
        return KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS;
    if ((uint32_t)count % 6u)
        return KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE;
    if (!indices || (indices & 1u))
        return KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER;
    byte_count = (uint32_t)count * (uint32_t)sizeof(uint16_t);
    if (indices > UINT32_MAX - byte_count)
        return KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER;
    return KAGE_VITA_CANONICAL_QUAD_OK;
}

static inline uint32_t kage_vita_canonical_quad_top_idx(uint32_t count)
{
    return (count / 6u) * 4u;
}

static inline int kage_vita_canonical_quad_fill(
    uint16_t *indices, uint32_t capacity)
{
    uint32_t quad;

    if (!indices || capacity < KAGE_VITA_CANONICAL_QUAD_MAX_INDICES)
        return 0;
    for (quad = 0u; quad < KAGE_VITA_CANONICAL_QUAD_MAX_INDICES / 6u;
            ++quad) {
        uint16_t base = (uint16_t)(quad * 4u);
        indices[quad * 6u] = base;
        indices[quad * 6u + 1u] = (uint16_t)(base + 2u);
        indices[quad * 6u + 2u] = (uint16_t)(base + 1u);
        indices[quad * 6u + 3u] = (uint16_t)(base + 1u);
        indices[quad * 6u + 4u] = (uint16_t)(base + 2u);
        indices[quad * 6u + 5u] = (uint16_t)(base + 3u);
    }
    return 1;
}

#endif
