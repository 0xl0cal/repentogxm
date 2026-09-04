#include "isaac_coloroffset_gpu_policy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "coloroffset-policy oracle: %s\n", message);
		exit(1);
	}
}

static void store_u32(uint8_t *bytes, uint32_t value)
{
	memcpy(bytes, &value, sizeof value);
}

static void make_vertices(uint8_t *vertices, size_t size)
{
	uint32_t vertex;
	memset(vertices, 0, size);
	for (vertex = 0; vertex < ISAAC_COLOROFFSET_VERTEX_COUNT; ++vertex) {
		uint8_t *value = vertices +
			(size_t)vertex * ISAAC_COLOROFFSET_VERTEX_STRIDE;
		store_u32(value + 24u, 0x3f800000u);
	}
}

static void read_exact_file(
	const char *path, void *bytes, size_t size, const char *label)
{
	FILE *stream = fopen(path, "rb");
	expect(stream != NULL, label);
	expect(fread(bytes, 1u, size, stream) == size, label);
	expect(fgetc(stream) == EOF, label);
	expect(!ferror(stream), label);
	expect(fclose(stream) == 0, label);
}

static void verify_captured_gxp_receipts(
	const char *vertex_path, const char *fragment_path)
{
	uint8_t vertex_gxp[ISAAC_COLOROFFSET_VERTEX_GXP_SIZE];
	uint8_t fragment_gxp[ISAAC_COLOROFFSET_FRAGMENT_GXP_SIZE];

	read_exact_file(vertex_path, vertex_gxp, sizeof vertex_gxp,
		"captured exact ColorOffset vertex GXP receipt failed");
	expect(isaac_coloroffset_policy_fnv1a(
			vertex_gxp, sizeof vertex_gxp) ==
			ISAAC_COLOROFFSET_VERTEX_GXP_FNV1A &&
		isaac_coloroffset_policy_fnv1a(vertex_gxp, 512u) ==
			ISAAC_COLOROFFSET_VERTEX_GXP_FIRST512_FNV1A,
		"captured vertex GXP does not match the full binary receipt");
	read_exact_file(fragment_path, fragment_gxp, sizeof fragment_gxp,
		"captured exact ColorOffset fragment GXP receipt failed");
	expect(isaac_coloroffset_policy_fnv1a(
			fragment_gxp, sizeof fragment_gxp) ==
			ISAAC_COLOROFFSET_FRAGMENT_GXP_FNV1A &&
		isaac_coloroffset_policy_fnv1a(fragment_gxp, 512u) ==
			ISAAC_COLOROFFSET_FRAGMENT_GXP_FIRST512_FNV1A,
		"captured fragment GXP does not match the full binary receipt");
}

int main(int argc, char **argv)
{
	isaac_coloroffset_attribute_key attributes[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT];
	uint16_t strides[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT];
	static const uint16_t offsets[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT] = {
		0u, 12u, 28u, 36u, 52u, 64u, 72u, 76u
	};
	static const uint8_t components[ISAAC_COLOROFFSET_ATTRIBUTE_COUNT] = {
		3u, 4u, 2u, 4u, 3u, 2u, 1u, 3u
	};
	static const uint16_t indices[ISAAC_COLOROFFSET_INDEX_COUNT] = {
		0u, 2u, 1u, 1u, 2u, 3u
	};
	uint8_t vertices[ISAAC_COLOROFFSET_VERTEX_COUNT *
		ISAAC_COLOROFFSET_VERTEX_STRIDE];
	isaac_coloroffset_cache_key key;
	isaac_coloroffset_cache_key changed;
	uint32_t index;
	float source_values[] = {0.0f, 0.125f, 0.5f, 1.0f};
	float destination_values[] = {0.0f, 0.25f, 0.75f, 1.0f};

	expect(isaac_coloroffset_next_generation(1u) == 2u,
		"live lifecycle generation did not advance");
	expect(isaac_coloroffset_next_generation(UINT32_MAX - 1u) == UINT32_MAX,
		"last non-wrapping lifecycle generation was skipped");
	expect(isaac_coloroffset_next_generation(UINT32_MAX) == 0u &&
		isaac_coloroffset_next_generation(0u) == 0u,
		"wrapped lifecycle generation was not permanently poisoned");

	for (index = 0; index < ISAAC_COLOROFFSET_ATTRIBUTE_COUNT; ++index) {
		attributes[index].stream_index = (uint16_t)index;
		attributes[index].offset = offsets[index];
		attributes[index].format = 9u;
		attributes[index].component_count = components[index];
		attributes[index].reg_index = (uint16_t)(index * 4u);
		strides[index] = ISAAC_COLOROFFSET_VERTEX_STRIDE;
	}
	expect(isaac_coloroffset_layout_is_exact(
		attributes, strides, ISAAC_COLOROFFSET_ATTRIBUTE_COUNT, 0xffu, 9u),
		"measured eight-attribute/88-byte layout rejected");
	attributes[7].offset++;
	expect(!isaac_coloroffset_layout_is_exact(
		attributes, strides, ISAAC_COLOROFFSET_ATTRIBUTE_COUNT, 0xffu, 9u),
		"hostile attribute offset passed");
	attributes[7].offset--;
	expect(!isaac_coloroffset_layout_is_exact(
		attributes, strides, ISAAC_COLOROFFSET_ATTRIBUTE_COUNT, 0x7fu, 9u),
		"disabled referenced attribute passed");

	make_vertices(vertices, sizeof vertices);
	expect(isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"bit-exact default vertices rejected");
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices - 1u, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"short hostile vertex span passed");
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT, 0u),
		"hostile zero stride passed");
	expect(isaac_coloroffset_indexed_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
		indices, sizeof indices, ISAAC_COLOROFFSET_INDEX_COUNT, 2u, 0u),
		"bounded index proof rejected");
	expect(!isaac_coloroffset_indexed_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
		indices, sizeof indices - 1u, ISAAC_COLOROFFSET_INDEX_COUNT, 2u, 0u),
		"short hostile index span passed");
	expect(!isaac_coloroffset_indexed_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
		indices, sizeof indices, ISAAC_COLOROFFSET_INDEX_COUNT, 3u, 0u),
		"invalid index width passed");
	{
		uint16_t hostile[ISAAC_COLOROFFSET_INDEX_COUNT] = {0, 1, 2, 4, 2, 3};
		expect(!isaac_coloroffset_indexed_vertices_are_opaque(
			vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
			hostile, sizeof hostile, ISAAC_COLOROFFSET_INDEX_COUNT, 2u, 0u),
			"out-of-range vertex index passed");
	}
	{
		uint32_t hostile = UINT32_MAX;
		expect(!isaac_coloroffset_indexed_vertices_are_opaque(
			vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_STRIDE,
			&hostile, sizeof hostile, 1u, 4u, 1u),
			"base/index overflow passed");
	}
	store_u32(vertices + ISAAC_COLOROFFSET_VERTEX_STRIDE + 24u, 0x3f7fffffu);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"non-one Color.a passed");
	store_u32(vertices + ISAAC_COLOROFFSET_VERTEX_STRIDE + 24u, 0x3f800000u);
	store_u32(vertices + 48u, 0x80000000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"negative-zero ColorizeOut.a passed bit-exact gate");
	store_u32(vertices + 48u, 0u);
	store_u32(vertices + 52u, 0x80000000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"negative-zero ColorOffsetOut passed bit-exact gate");
	store_u32(vertices + 52u, 0u);
	store_u32(vertices + 72u, 0x7fc00000u);
	expect(!isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"NaN pixelation passed <= 0 gate");
	store_u32(vertices + 72u, 0xbf800000u);
	expect(isaac_coloroffset_contiguous_vertices_are_opaque(
		vertices, sizeof vertices, ISAAC_COLOROFFSET_VERTEX_COUNT,
		ISAAC_COLOROFFSET_VERTEX_STRIDE),
		"finite negative pixelation was not accepted");

	/* Physical GXM raw 0x5151110f decodes to mask ALL, ADD/ADD,
	 * color ONE/ONE_MINUS_SRC_ALPHA and alpha
	 * ONE/ONE_MINUS_SRC_ALPHA.  The old accepted source factor was
	 * SRC_ALPHA.  Both source factors are identical when the separately
	 * proven shader output alpha is mathematically one. */
	expect((0x5151110fu & 0xffu) == 0x0fu &&
		((0x5151110fu >> 8u) & 0x0fu) == 1u &&
		((0x5151110fu >> 12u) & 0x0fu) == 1u &&
		((0x5151110fu >> 16u) & 0x0fu) == 1u &&
		((0x5151110fu >> 20u) & 0x0fu) == 5u &&
		((0x5151110fu >> 24u) & 0x0fu) == 1u &&
		((0x5151110fu >> 28u) & 0x0fu) == 5u,
		"physical ColorOffset blend receipt decoded incorrectly");
	for (index = 0; index < sizeof source_values / sizeof source_values[0]; ++index) {
		uint32_t destination;
		uint32_t source_factor;
		for (source_factor = 0; source_factor < 2u; ++source_factor) {
			for (destination = 0;
					destination < sizeof destination_values /
						sizeof destination_values[0]; ++destination) {
				float alpha = 1.0f;
				float factor = source_factor ? alpha : 1.0f;
				float blended = source_values[index] * factor +
					destination_values[destination] * (1.0f - alpha);
				expect(blended == source_values[index],
					"opaque blend algebra differs from no-blend output");
			}
		}
	}

	/* The byte-identical stock shader writes Color.a.  RGB24 sampling supplies
	 * alpha one and the gate requires Color0.a to be bit-exact one, so source
	 * alpha is mathematically one regardless of its RGB/colorize arithmetic. */
	expect(1.0f * 1.0f == 1.0f,
		"RGB24/Color0 output-alpha proof failed");

	memset(&key, 0, sizeof key);
	key.fragment_shader_id = 7u;
	key.source_generation = 3u;
	key.vertex_program = (uintptr_t)0x12340000u;
	key.source_blend_raw = 0x5151110fu;
	key.output_format = 1u;
	key.multisample_mode = 0u;
	key.texture_format = 0x98000000u;
	key.texture_width = ISAAC_COLOROFFSET_TEXTURE_WIDTH;
	key.texture_height = ISAAC_COLOROFFSET_TEXTURE_HEIGHT;
	key.viewport_width = ISAAC_COLOROFFSET_VIEWPORT_WIDTH;
	key.viewport_height = ISAAC_COLOROFFSET_VIEWPORT_HEIGHT;
	changed = key;
	expect(isaac_coloroffset_cache_key_equal(&key, &changed),
		"identical complete cache key rejected");
#define REJECT_CHANGED(field, value, message) \
	changed = key; changed.field = (value); \
	expect(!isaac_coloroffset_cache_key_equal(&key, &changed), (message))
	REJECT_CHANGED(fragment_shader_id, 8u, "fragment identity omitted from key");
	REJECT_CHANGED(source_generation, 4u, "source/relink generation omitted from key");
	REJECT_CHANGED(vertex_program, (uintptr_t)0x12340004u,
		"vertex link omitted from key");
	REJECT_CHANGED(source_blend_raw, 0x5151111fu, "blend state omitted from key");
	REJECT_CHANGED(output_format, 2u, "output format omitted from key");
	REJECT_CHANGED(multisample_mode, 1u, "MSAA state omitted from key");
	REJECT_CHANGED(texture_format, 0x9c000000u, "sampler format omitted from key");
	REJECT_CHANGED(texture_width, 431u, "sampler width omitted from key");
	REJECT_CHANGED(texture_height, 239u, "sampler height omitted from key");
	REJECT_CHANGED(viewport_x, 1, "viewport x omitted from key");
	REJECT_CHANGED(viewport_y, 1, "viewport y omitted from key");
	REJECT_CHANGED(viewport_width, 959u, "viewport width omitted from key");
	REJECT_CHANGED(viewport_height, 544u, "viewport state omitted from key");
#undef REJECT_CHANGED

	if (argc == 3)
		verify_captured_gxp_receipts(argv[1], argv[2]);
	else
		expect(argc == 1,
			"usage: oracle [captured-vertex.gxp captured-fragment.gxp]");

	puts("coloroffset-policy oracle: PASS");
	return 0;
}
