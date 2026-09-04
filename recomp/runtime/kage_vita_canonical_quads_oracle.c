#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "kage_vita_canonical_quads.h"

static int winding(uint16_t a, uint16_t b, uint16_t c)
{
    static const int x[4] = {0, 1, 0, 1};
    static const int y[4] = {0, 0, 1, 1};
    return (x[b] - x[a]) * (y[c] - y[a]) -
        (y[b] - y[a]) * (x[c] - x[a]);
}

int main(void)
{
    static uint16_t indices[KAGE_VITA_CANONICAL_QUAD_MAX_INDICES];
    static const uint32_t sites[] = {
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_GLOBAL
    };
    uint32_t quad;
    uint32_t count;
    uint32_t site;

    assert(!kage_vita_canonical_quad_fill(NULL,
        KAGE_VITA_CANONICAL_QUAD_MAX_INDICES));
    assert(!kage_vita_canonical_quad_fill(indices,
        KAGE_VITA_CANONICAL_QUAD_MAX_INDICES - 1u));
    assert(kage_vita_canonical_quad_fill(indices,
        KAGE_VITA_CANONICAL_QUAD_MAX_INDICES));
    for (quad = 0u; quad < KAGE_VITA_CANONICAL_QUAD_MAX_INDICES / 6u;
            ++quad) {
        uint32_t at = quad * 6u;
        uint16_t base = (uint16_t)(quad * 4u);
        assert(indices[at] == base);
        assert(indices[at + 1u] == base + 2u);
        assert(indices[at + 2u] == base + 1u);
        assert(indices[at + 3u] == base + 1u);
        assert(indices[at + 4u] == base + 2u);
        assert(indices[at + 5u] == base + 3u);
        assert(winding(
            indices[at] - base, indices[at + 1u] - base,
            indices[at + 2u] - base) == -1);
        assert(winding(
            indices[at + 3u] - base, indices[at + 4u] - base,
            indices[at + 5u] - base) == -1);
    }
    assert(indices[KAGE_VITA_CANONICAL_QUAD_MAX_INDICES - 1u] == 32767u);

    for (site = 0u; site < sizeof sites / sizeof sites[0]; ++site) {
        for (count = 1u; count <= KAGE_VITA_CANONICAL_QUAD_MAX_INDICES + 6u;
                ++count) {
            KageVitaCanonicalQuadResult expected =
                count > KAGE_VITA_CANONICAL_QUAD_MAX_INDICES ?
                    KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS :
                count % 6u ? KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE :
                    KAGE_VITA_CANONICAL_QUAD_OK;
            assert(kage_vita_canonical_quad_classify(
                sites[site], KAGE_VITA_GL_TRIANGLES, (int32_t)count,
                KAGE_VITA_GL_UNSIGNED_SHORT, 0x1000u) == expected);
        }
    }
    assert(kage_vita_canonical_quad_classify(
        0u, KAGE_VITA_GL_TRIANGLES, 6, KAGE_VITA_GL_UNSIGNED_SHORT,
        0x1000u) == KAGE_VITA_CANONICAL_QUAD_REJECT_CALLSITE);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE - 1u,
        KAGE_VITA_GL_TRIANGLES, 6, KAGE_VITA_GL_UNSIGNED_SHORT,
        0x1000u) == KAGE_VITA_CANONICAL_QUAD_REJECT_CALLSITE);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE, 5u, 6,
        KAGE_VITA_GL_UNSIGNED_SHORT, 0x1000u) ==
        KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, 6, 0x1405u, 0x1000u) ==
        KAGE_VITA_CANONICAL_QUAD_REJECT_SHAPE);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, 0, KAGE_VITA_GL_UNSIGNED_SHORT, 0x1000u) ==
        KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, INT_MIN, KAGE_VITA_GL_UNSIGNED_SHORT,
        0x1000u) == KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, INT_MAX, KAGE_VITA_GL_UNSIGNED_SHORT,
        0x1000u) == KAGE_VITA_CANONICAL_QUAD_REJECT_BOUNDS);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, 6, KAGE_VITA_GL_UNSIGNED_SHORT, 0u) ==
        KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, 6, KAGE_VITA_GL_UNSIGNED_SHORT, 0x1001u) ==
        KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER);
    assert(kage_vita_canonical_quad_classify(
        KAGE_VITA_CANONICAL_QUAD_RETURN_RVA_IMAGE,
        KAGE_VITA_GL_TRIANGLES, 6, KAGE_VITA_GL_UNSIGNED_SHORT,
        UINT32_MAX - 9u) == KAGE_VITA_CANONICAL_QUAD_REJECT_POINTER);
    assert(kage_vita_canonical_quad_top_idx(6u) == 4u);
    assert(kage_vita_canonical_quad_top_idx(
        KAGE_VITA_CANONICAL_QUAD_MAX_INDICES) == 32768u);
    puts("KAGE canonical quad zero-copy policy oracle: PASS");
    return 0;
}
