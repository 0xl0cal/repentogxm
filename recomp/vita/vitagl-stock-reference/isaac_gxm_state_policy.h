#ifndef ISAAC_VITAGL_GXM_STATE_POLICY_H
#define ISAAC_VITAGL_GXM_STATE_POLICY_H

#include <stddef.h>
#include <stdint.h>

#define ISAAC_GXM_STATE_CONTEXT_CAPACITY 4u
#define ISAAC_GXM_STATE_TEXTURE_UNITS 16u
#define ISAAC_GXM_STATE_TEXTURE_BYTES 16u

typedef struct isaac_gxm_state_counters {
	uint32_t vertex_program_hits;
	uint32_t vertex_program_misses;
	uint32_t fragment_program_hits;
	uint32_t fragment_program_misses;
	uint32_t fragment_texture_hits;
	uint32_t fragment_texture_misses;
} isaac_gxm_state_counters;

typedef struct isaac_gxm_context_state {
	uintptr_t context;
	uintptr_t vertex_program;
	uintptr_t fragment_program;
	uint8_t texture[ISAAC_GXM_STATE_TEXTURE_UNITS]
		[ISAAC_GXM_STATE_TEXTURE_BYTES];
	uint16_t texture_valid;
	uint8_t active_scene;
	uint8_t vertex_program_valid;
	uint8_t fragment_program_valid;
} isaac_gxm_context_state;

typedef struct isaac_gxm_state_policy {
	isaac_gxm_context_state contexts[ISAAC_GXM_STATE_CONTEXT_CAPACITY];
	isaac_gxm_state_counters counters;
	uint32_t replacement_cursor;
} isaac_gxm_state_policy;

#ifdef HAVE_ISAAC_PHASE_PROFILE
#define ISAAC_GXM_STATE_COUNT(policy, field) \
	((policy)->counters.field++)
#else
#define ISAAC_GXM_STATE_COUNT(policy, field) ((void)0)
#endif

static inline void isaac_gxm_state_clear(isaac_gxm_context_state *state)
{
	state->vertex_program = 0u;
	state->fragment_program = 0u;
	state->texture_valid = 0u;
	state->vertex_program_valid = 0u;
	state->fragment_program_valid = 0u;
}

static inline void isaac_gxm_state_forget(isaac_gxm_context_state *state)
{
	isaac_gxm_state_clear(state);
	state->active_scene = 0u;
}

static inline isaac_gxm_context_state *isaac_gxm_state_find(
	isaac_gxm_state_policy *policy, uintptr_t context, int create)
{
	uint32_t index;
	isaac_gxm_context_state *empty = NULL;

	if (!context)
		return NULL;
	for (index = 0u; index < ISAAC_GXM_STATE_CONTEXT_CAPACITY; ++index) {
		isaac_gxm_context_state *state = &policy->contexts[index];
		if (state->context == context)
			return state;
		if (!state->context && !empty)
			empty = state;
	}
	if (!create)
		return NULL;
	if (!empty) {
		empty = &policy->contexts[
			policy->replacement_cursor % ISAAC_GXM_STATE_CONTEXT_CAPACITY];
		policy->replacement_cursor++;
	}
	empty->context = context;
	isaac_gxm_state_forget(empty);
	return empty;
}

static inline void isaac_gxm_state_begin_scene(
	isaac_gxm_state_policy *policy, uintptr_t context, int success)
{
	isaac_gxm_context_state *state =
		isaac_gxm_state_find(policy, context, success != 0);

	if (!state)
		return;
	isaac_gxm_state_forget(state);
	state->active_scene = success ? 1u : 0u;
}

static inline void isaac_gxm_state_invalidate(
	isaac_gxm_state_policy *policy, uintptr_t context)
{
	isaac_gxm_context_state *state =
		isaac_gxm_state_find(policy, context, 0);

	if (state)
		isaac_gxm_state_forget(state);
}

static inline void isaac_gxm_state_invalidate_values(
	isaac_gxm_state_policy *policy, uintptr_t context)
{
	isaac_gxm_context_state *state =
		isaac_gxm_state_find(policy, context, 0);

	if (state)
		isaac_gxm_state_clear(state);
}

static inline int isaac_gxm_state_vertex_program(
	isaac_gxm_state_policy *policy, uintptr_t context, uintptr_t program)
{
	isaac_gxm_context_state *state =
		isaac_gxm_state_find(policy, context, 0);

	if (state && state->active_scene && program &&
			state->vertex_program_valid &&
			state->vertex_program == program) {
		ISAAC_GXM_STATE_COUNT(policy, vertex_program_hits);
		return 1;
	}
	ISAAC_GXM_STATE_COUNT(policy, vertex_program_misses);
	if (state && state->active_scene && program) {
		state->vertex_program = program;
		state->vertex_program_valid = 1u;
	}
	return 0;
}

static inline int isaac_gxm_state_fragment_program(
	isaac_gxm_state_policy *policy, uintptr_t context, uintptr_t program)
{
	isaac_gxm_context_state *state =
		isaac_gxm_state_find(policy, context, 0);

	if (state && state->active_scene && program &&
			state->fragment_program_valid &&
			state->fragment_program == program) {
		ISAAC_GXM_STATE_COUNT(policy, fragment_program_hits);
		return 1;
	}
	ISAAC_GXM_STATE_COUNT(policy, fragment_program_misses);
	if (state && state->active_scene && program) {
		state->fragment_program = program;
		state->fragment_program_valid = 1u;
	}
	return 0;
}

static inline int isaac_gxm_state_bytes_equal(
	const uint8_t *left, const uint8_t *right)
{
	uint32_t index;

	for (index = 0u; index < ISAAC_GXM_STATE_TEXTURE_BYTES; ++index)
		if (left[index] != right[index])
			return 0;
	return 1;
}

static inline int isaac_gxm_state_fragment_texture(
	isaac_gxm_state_policy *policy, uintptr_t context, uint32_t unit,
	const void *descriptor)
{
	isaac_gxm_context_state *state =
		isaac_gxm_state_find(policy, context, 0);
	const uint8_t *bytes = (const uint8_t *)descriptor;
	uint32_t index;

	if (state && state->active_scene && bytes &&
			unit < ISAAC_GXM_STATE_TEXTURE_UNITS &&
			(state->texture_valid & (uint16_t)(1u << unit)) &&
			isaac_gxm_state_bytes_equal(state->texture[unit], bytes)) {
		ISAAC_GXM_STATE_COUNT(policy, fragment_texture_hits);
		return 1;
	}
	ISAAC_GXM_STATE_COUNT(policy, fragment_texture_misses);
	if (state && state->active_scene && bytes &&
			unit < ISAAC_GXM_STATE_TEXTURE_UNITS) {
		for (index = 0u; index < ISAAC_GXM_STATE_TEXTURE_BYTES; ++index)
			state->texture[unit][index] = bytes[index];
		state->texture_valid |= (uint16_t)(1u << unit);
	}
	return 0;
}

#endif
