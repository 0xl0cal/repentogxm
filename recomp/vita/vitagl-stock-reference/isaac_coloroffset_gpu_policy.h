#ifndef ISAAC_COLOROFFSET_GPU_POLICY_H
#define ISAAC_COLOROFFSET_GPU_POLICY_H

/* Pure, host-testable policy for the one measured ColorOffset full-screen
 * draw.  The vitaGL patch owns GXM objects; this header owns every arithmetic
 * and byte-range decision that permits the no-blend variant. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ISAAC_COLOROFFSET_ATTRIBUTE_COUNT 8u
#define ISAAC_COLOROFFSET_VERTEX_STRIDE 88u
#define ISAAC_COLOROFFSET_VERTEX_COUNT 4u
#define ISAAC_COLOROFFSET_INDEX_COUNT 6u
#define ISAAC_COLOROFFSET_TEXTURE_WIDTH 432u
#define ISAAC_COLOROFFSET_TEXTURE_HEIGHT 240u
#define ISAAC_COLOROFFSET_VIEWPORT_WIDTH 960u
#define ISAAC_COLOROFFSET_VIEWPORT_HEIGHT 540u
#define ISAAC_COLOROFFSET_SOURCE_SIZE 2052u
#define ISAAC_COLOROFFSET_SOURCE_FNV1A 0x2b3ccf4au
#define ISAAC_COLOROFFSET_SOURCE_FIRST512_FNV1A 0x75ef2322u
#define ISAAC_COLOROFFSET_VERTEX_GXP_SIZE 562u
#define ISAAC_COLOROFFSET_VERTEX_GXP_FNV1A 0x4f7dce51u
#define ISAAC_COLOROFFSET_VERTEX_GXP_FIRST512_FNV1A 0x338d5584u
#define ISAAC_COLOROFFSET_FRAGMENT_GXP_SIZE 809u
#define ISAAC_COLOROFFSET_FRAGMENT_GXP_FNV1A 0xb6517f58u
#define ISAAC_COLOROFFSET_FRAGMENT_GXP_FIRST512_FNV1A 0xc26317f4u

/* Captured complete GXP receipts.  SHA-256:
 * VS ae60775149e43dad58943fe2d000358d3fb8dadd2bc7051557d3ca0d3b88be8a
 * FS e9e6df192297ff4fdc723dd0d5f62e9ad79e902e13842a3fb07784c6dabdc574 */

enum isaac_coloroffset_failure {
	ISAAC_COLOROFFSET_OK = 0,
	ISAAC_COLOROFFSET_FAIL_PROGRAM,
	ISAAC_COLOROFFSET_FAIL_BLEND,
	ISAAC_COLOROFFSET_FAIL_SAMPLER,
	ISAAC_COLOROFFSET_FAIL_LAYOUT,
	ISAAC_COLOROFFSET_FAIL_VERTEX_RANGE,
	ISAAC_COLOROFFSET_FAIL_VERTEX_VALUE,
	ISAAC_COLOROFFSET_FAIL_OUTPUT,
	ISAAC_COLOROFFSET_FAIL_CACHE
};

typedef struct isaac_coloroffset_attribute_key {
	uint16_t stream_index;
	uint16_t offset;
	uint8_t format;
	uint8_t component_count;
	uint16_t reg_index;
} isaac_coloroffset_attribute_key;

typedef struct isaac_coloroffset_blend_key {
	uint8_t color_mask;
	uint8_t color_func;
	uint8_t alpha_func;
	uint8_t color_src;
	uint8_t color_dst;
	uint8_t alpha_src;
	uint8_t alpha_dst;
} isaac_coloroffset_blend_key;

typedef struct isaac_coloroffset_cache_key {
	uint32_t fragment_shader_id;
	uint32_t source_generation;
	uintptr_t vertex_program;
	uint32_t source_blend_raw;
	uint32_t output_format;
	uint32_t multisample_mode;
	uint32_t texture_format;
	uint16_t texture_width;
	uint16_t texture_height;
	int16_t viewport_x;
	int16_t viewport_y;
	uint16_t viewport_width;
	uint16_t viewport_height;
} isaac_coloroffset_cache_key;

static inline uint32_t isaac_coloroffset_policy_fnv1a(
		const void *data, size_t size) {
	const uint8_t *bytes = (const uint8_t *)data;
	uint32_t value = 0x811c9dc5u;
	size_t index;

	if (!bytes && size)
		return 0u;
	for (index = 0; index < size; ++index) {
		value ^= bytes[index];
		value *= 0x01000193u;
	}
	return value;
}

/* Zero is a permanent fail-closed poison value, preventing generation ABA
 * after wrap or use before glCreateShader initialization. */
static inline uint32_t isaac_coloroffset_next_generation(uint32_t current) {
	return current && current != UINT32_MAX ? current + 1u : 0u;
}

static inline int isaac_coloroffset_layout_is_exact(
		const isaac_coloroffset_attribute_key *attributes,
		const uint16_t *strides, uint32_t attribute_count,
		uint32_t active_mask, uint8_t f32_format) {
	static const uint16_t offsets[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT] = {
		0u, 12u, 28u, 36u, 52u, 64u, 72u, 76u
	};
	static const uint8_t components[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT] = {
		3u, 4u, 2u, 4u, 3u, 2u, 1u, 3u
	};
	uint32_t index;

	if (!attributes || !strides ||
			attribute_count != ISAAC_COLOROFFSET_ATTRIBUTE_COUNT ||
			(active_mask & 0xffu) != 0xffu)
		return 0;
	for (index = 0; index < ISAAC_COLOROFFSET_ATTRIBUTE_COUNT; ++index) {
		if (attributes[index].stream_index != index ||
				attributes[index].offset != offsets[index] ||
				attributes[index].format != f32_format ||
				attributes[index].component_count != components[index] ||
				attributes[index].reg_index != index * 4u ||
				strides[index] != ISAAC_COLOROFFSET_VERTEX_STRIDE)
			return 0;
	}
	return 1;
}

static inline uint32_t isaac_coloroffset_load_u32(const uint8_t *bytes) {
	uint32_t value;
	memcpy(&value, bytes, sizeof value);
	return value;
}

static inline int isaac_coloroffset_vertex_is_default_opaque(
		const uint8_t *vertex, size_t available) {
	float pixelation;
	uint32_t index;

	if (!vertex || available < ISAAC_COLOROFFSET_VERTEX_STRIDE)
		return 0;
	/* Color.a is bit-exact 1.0. */
	if (isaac_coloroffset_load_u32(vertex + 24u) != 0x3f800000u)
		return 0;
	/* ColorizeOut.a and every ColorOffsetOut lane are bit-exact +0. */
	if (isaac_coloroffset_load_u32(vertex + 48u) != 0u)
		return 0;
	for (index = 0; index < 3u; ++index) {
		if (isaac_coloroffset_load_u32(vertex + 52u + index * 4u) != 0u)
			return 0;
	}
	memcpy(&pixelation, vertex + 72u, sizeof pixelation);
	/* This also rejects NaN.  Negative values are legal: the exact shader's
	 * fast branch and original sampling expression both use TexCoord0. */
	return pixelation <= 0.0f;
}

static inline int isaac_coloroffset_contiguous_vertices_are_opaque(
		const void *vertices, size_t vertex_bytes, uint32_t vertex_count,
		uint32_t stride) {
	const uint8_t *bytes = (const uint8_t *)vertices;
	uint32_t index;

	if (!bytes || stride != ISAAC_COLOROFFSET_VERTEX_STRIDE ||
			vertex_count != ISAAC_COLOROFFSET_VERTEX_COUNT ||
			vertex_count > SIZE_MAX / stride ||
			vertex_bytes < (size_t)vertex_count * stride)
		return 0;
	for (index = 0; index < vertex_count; ++index) {
		size_t offset = (size_t)index * stride;
		if (!isaac_coloroffset_vertex_is_default_opaque(
				bytes + offset, vertex_bytes - offset))
			return 0;
	}
	return 1;
}

/* General indexed form used by the hostile-range oracle.  Production uses
 * the contiguous form only after vitaGL itself has found max(index)+1 and
 * copied that complete range into its bounded mapped staging allocation. */
static inline int isaac_coloroffset_indexed_vertices_are_opaque(
		const void *vertices, size_t vertex_bytes, uint32_t stride,
		const void *indices, size_t index_bytes, uint32_t index_count,
		uint32_t index_width, uint32_t base_vertex) {
	const uint8_t *vertex_data = (const uint8_t *)vertices;
	const uint8_t *index_data = (const uint8_t *)indices;
	uint32_t item;

	if (!vertex_data || !index_data ||
			stride != ISAAC_COLOROFFSET_VERTEX_STRIDE ||
			(index_width != 2u && index_width != 4u) ||
			index_count > SIZE_MAX / index_width ||
			index_bytes < (size_t)index_count * index_width)
		return 0;
	for (item = 0; item < index_count; ++item) {
		uint32_t index;
		size_t offset;
		if (index_width == 2u) {
			uint16_t value;
			memcpy(&value, index_data + (size_t)item * 2u, sizeof value);
			index = value;
		} else {
			memcpy(&index, index_data + (size_t)item * 4u, sizeof index);
		}
		if (index > UINT32_MAX - base_vertex)
			return 0;
		index += base_vertex;
		if (index > SIZE_MAX / stride)
			return 0;
		offset = (size_t)index * stride;
		if (offset > vertex_bytes ||
				vertex_bytes - offset < ISAAC_COLOROFFSET_VERTEX_STRIDE ||
				!isaac_coloroffset_vertex_is_default_opaque(
					vertex_data + offset, vertex_bytes - offset))
			return 0;
	}
	return 1;
}

static inline int isaac_coloroffset_cache_key_equal(
		const isaac_coloroffset_cache_key *left,
		const isaac_coloroffset_cache_key *right) {
	return left && right &&
		left->fragment_shader_id == right->fragment_shader_id &&
		left->source_generation == right->source_generation &&
		left->vertex_program == right->vertex_program &&
		left->source_blend_raw == right->source_blend_raw &&
		left->output_format == right->output_format &&
		left->multisample_mode == right->multisample_mode &&
		left->texture_format == right->texture_format &&
		left->texture_width == right->texture_width &&
		left->texture_height == right->texture_height &&
		left->viewport_x == right->viewport_x &&
		left->viewport_y == right->viewport_y &&
		left->viewport_width == right->viewport_width &&
		left->viewport_height == right->viewport_height;
}

#endif
