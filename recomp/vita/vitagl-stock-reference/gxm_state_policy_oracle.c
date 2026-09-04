#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "isaac_gxm_state_policy.h"

static void fill(uint8_t descriptor[ISAAC_GXM_STATE_TEXTURE_BYTES],
	uint8_t seed)
{
	uint32_t index;

	for (index = 0u; index < ISAAC_GXM_STATE_TEXTURE_BYTES; ++index)
		descriptor[index] = (uint8_t)(seed + index * 13u);
}

int main(void)
{
	isaac_gxm_state_policy policy;
	uint8_t texture_a[ISAAC_GXM_STATE_TEXTURE_BYTES];
	uint8_t texture_b[ISAAC_GXM_STATE_TEXTURE_BYTES];
	uint8_t texture_changed[ISAAC_GXM_STATE_TEXTURE_BYTES];
	uint32_t context;

	memset(&policy, 0, sizeof policy);
	fill(texture_a, 7u);
	fill(texture_b, 19u);
	memcpy(texture_changed, texture_a, sizeof texture_a);
	texture_changed[ISAAC_GXM_STATE_TEXTURE_BYTES - 1u] ^= 0x80u;

	/* Nothing is cacheable before a successful BeginScene. */
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2000u));
	assert(!isaac_gxm_state_fragment_program(&policy, 0x1000u, 0x3000u));
	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, 0u, texture_a));
	isaac_gxm_state_begin_scene(&policy, 0x1000u, 0);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2000u));

	isaac_gxm_state_begin_scene(&policy, 0x1000u, 1);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2000u));
	assert(isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2000u));
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));
	assert(isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0u));
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0u));

	assert(!isaac_gxm_state_fragment_program(&policy, 0x1000u, 0x3000u));
	assert(isaac_gxm_state_fragment_program(&policy, 0x1000u, 0x3000u));
	assert(!isaac_gxm_state_fragment_program(&policy, 0x1000u, 0x3001u));

	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, 3u, texture_a));
	assert(isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, 3u, texture_a));
	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, 3u, texture_changed));
	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, 4u, texture_changed));
	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, ISAAC_GXM_STATE_TEXTURE_UNITS, texture_a));
	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1000u, 0u, NULL));

	/* A second context cannot inherit any state from the first. */
	isaac_gxm_state_begin_scene(&policy, 0x1100u, 1);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1100u, 0x2001u));
	assert(isaac_gxm_state_vertex_program(&policy, 0x1100u, 0x2001u));
	assert(isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));
	assert(!isaac_gxm_state_fragment_texture(
		&policy, 0x1100u, 3u, texture_b));
	assert(isaac_gxm_state_fragment_texture(
		&policy, 0x1100u, 3u, texture_b));

	/* A clear/pipeline invalidation forgets values without ending the scene. */
	isaac_gxm_state_invalidate_values(&policy, 0x1100u);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1100u, 0x2001u));
	assert(isaac_gxm_state_vertex_program(&policy, 0x1100u, 0x2001u));

	/* End/reset and every new scene force the first setter through again. */
	isaac_gxm_state_invalidate(&policy, 0x1000u);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));
	isaac_gxm_state_begin_scene(&policy, 0x1000u, 1);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));
	assert(isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));
	isaac_gxm_state_begin_scene(&policy, 0x1000u, 1);
	assert(!isaac_gxm_state_vertex_program(&policy, 0x1000u, 0x2001u));

	/* Bounded replacement also starts each new context cold. */
	for (context = 0u; context < ISAAC_GXM_STATE_CONTEXT_CAPACITY + 3u;
			++context) {
		uintptr_t key = 0x4000u + context * 0x100u;
		isaac_gxm_state_begin_scene(&policy, key, 1);
		assert(!isaac_gxm_state_fragment_program(
			&policy, key, 0x5000u));
		assert(isaac_gxm_state_fragment_program(
			&policy, key, 0x5000u));
	}

#ifdef HAVE_ISAAC_PHASE_PROFILE
	assert(policy.counters.vertex_program_hits == 6u);
	assert(policy.counters.vertex_program_misses == 11u);
	assert(policy.counters.fragment_program_hits == 8u);
	assert(policy.counters.fragment_program_misses == 10u);
	assert(policy.counters.fragment_texture_hits == 2u);
	assert(policy.counters.fragment_texture_misses == 7u);
#else
	assert(policy.counters.vertex_program_hits == 0u);
	assert(policy.counters.vertex_program_misses == 0u);
	assert(policy.counters.fragment_program_hits == 0u);
	assert(policy.counters.fragment_program_misses == 0u);
	assert(policy.counters.fragment_texture_hits == 0u);
	assert(policy.counters.fragment_texture_misses == 0u);
#endif
	puts("GXM per-context/per-scene state policy oracle: PASS");
	return 0;
}
