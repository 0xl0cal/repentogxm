#ifndef ISAAC_COLOROFFSET_PLAIN_POLICY_H
#define ISAAC_COLOROFFSET_PLAIN_POLICY_H

#include "isaac_coloroffset_gpu_policy.h"

/* Additional proof ONLY AFTER the existing neutral policy has accepted the
 * same immutable cached chunk. That policy bounds all color arithmetic and
 * rejects every Inf/NaN lane: mix(x,y,0) is not x when y is non-finite.
 * The paired exact VS copies these attributes. No Color RGBA lane is changed
 * or required to be opaque/white; Colorize RGB may be any accepted value. */
static inline int isaac_coloroffset_neutral_vertices_have_plain_colors(
		const uint8_t *vertices, uint32_t vertex_count) {
	uint32_t vertex;
	if (!vertices || !vertex_count || vertex_count > 4096u)
		return 0;
	for (vertex = 0u; vertex < vertex_count; ++vertex) {
		const uint8_t *v = vertices +
			(size_t)vertex * ISAAC_COLOROFFSET_VERTEX_STRIDE;
		/* ColorizeIn.a and ColorOffsetIn.xyz must all be +/-0, not
		 * "close enough". With normalized textures and the retained
		 * abs(color)<=16 guard, even the unused luminance branch is finite. */
		uint32_t nonzero = isaac_coloroffset_load_u32(v + 48u) |
			isaac_coloroffset_load_u32(v + 52u) |
			isaac_coloroffset_load_u32(v + 56u) |
			isaac_coloroffset_load_u32(v + 60u);
		if ((nonzero & 0x7fffffffu) != 0u)
			return 0;
	}
	return 1;
}

#endif
