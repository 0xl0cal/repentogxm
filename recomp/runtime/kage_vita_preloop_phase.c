#include "kage_vita_preloop_phase.h"

#include <stdint.h>
#include <string.h>

#include "guest.h"

typedef struct kage_vita_preloop_phase_slot_state {
    uint32_t enter_count;
    uint32_t exit_count;
    uint32_t current_depth;
    uint32_t max_depth;
} kage_vita_preloop_phase_slot_state;

static kage_vita_preloop_phase_slot_state
    s_phase_slots[KAGE_VITA_PRELOOP_PHASE_COUNT];

static uint32_t phase_load(const uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static void phase_store(uint32_t *value, uint32_t stored)
{
    __atomic_store_n(value, stored, __ATOMIC_RELAXED);
}

static uint32_t phase_increment(uint32_t *value)
{
    uint32_t incremented = phase_load(value) + 1u;

    /* All wrappers execute on the one translated-guest writer.  A relaxed
     * atomic load/store keeps watchdog reads race-free without ARM exclusive
     * loops or barriers on these potentially hot archive/inflate edges. */
    phase_store(value, incremented);
    return incremented;
}

static void phase_note_max(uint32_t *value, uint32_t candidate)
{
    uint32_t observed = phase_load(value);

    if (observed < candidate)
        phase_store(value, candidate);
}

static void phase_enter(enum kage_vita_preloop_phase_slot slot)
{
    kage_vita_preloop_phase_slot_state *state = &s_phase_slots[slot];
    uint32_t depth = phase_increment(&state->current_depth);

    (void)phase_increment(&state->enter_count);
    phase_note_max(&state->max_depth, depth);
}

static void phase_leave(enum kage_vita_preloop_phase_slot slot)
{
    kage_vita_preloop_phase_slot_state *state = &s_phase_slots[slot];

    (void)phase_increment(&state->exit_count);
    phase_store(&state->current_depth,
                phase_load(&state->current_depth) - 1u);
}

void kage_vita_preloop_phase_reset(void)
{
    unsigned slot;

    for (slot = 0u; slot < KAGE_VITA_PRELOOP_PHASE_COUNT; ++slot) {
        kage_vita_preloop_phase_slot_state *state = &s_phase_slots[slot];
        uint32_t depth = phase_load(&state->current_depth);

        /* Do not clear a live wrapper: backend initialization is nested under
         * PLATFORM_INIT.  The single guest-thread lifecycle guarantees no
         * wrapper transition can race these epoch stores. */
        phase_store(&state->enter_count, depth);
        phase_store(&state->exit_count, 0u);
        phase_store(&state->max_depth, depth);
    }
}

void kage_vita_preloop_phase_get_snapshot(
    kage_vita_preloop_phase_snapshot *out)
{
    unsigned slot;

    if (!out)
        return;
    memset(out, 0, sizeof *out);
    for (slot = 0u; slot < KAGE_VITA_PRELOOP_PHASE_COUNT; ++slot) {
        kage_vita_preloop_phase_slot_snapshot *destination =
            &out->slots[slot];
        const kage_vita_preloop_phase_slot_state *source =
            &s_phase_slots[slot];
        uint32_t depth;

        destination->enter_count = phase_load(&source->enter_count);
        destination->exit_count = phase_load(&source->exit_count);
        depth = phase_load(&source->current_depth);
        destination->current_depth = depth;
        destination->max_depth = phase_load(&source->max_depth);
        out->total_enter_count += destination->enter_count;
        out->total_exit_count += destination->exit_count;
        if (depth != 0u)
            out->active_mask |= 1u << slot;
        if (depth > 15u)
            depth = 15u;
        out->packed_depths |= depth << (slot * 4u);
    }
}

#define DEFINE_PHASE_WRAPPER(symbol, slot) \
    extern void __real_##symbol(CPU *__restrict c); \
    void __wrap_##symbol(CPU *__restrict c) \
    { \
        phase_enter(slot); \
        __real_##symbol(c); \
        phase_leave(slot); \
    }

DEFINE_PHASE_WRAPPER(
    sub_0059fa30, KAGE_VITA_PRELOOP_GLFW_ERROR)
DEFINE_PHASE_WRAPPER(
    sub_00598c80, KAGE_VITA_PRELOOP_PLATFORM_INIT)
DEFINE_PHASE_WRAPPER(
    sub_0050b240, KAGE_VITA_PRELOOP_ISAAC_STARTUP)
DEFINE_PHASE_WRAPPER(
    sub_00563880, KAGE_VITA_PRELOOP_LOAD_ARCHIVE)
DEFINE_PHASE_WRAPPER(
    sub_004ab7a0, KAGE_VITA_PRELOOP_MANAGER_CONSTRUCT)
DEFINE_PHASE_WRAPPER(
    sub_005aeb00, KAGE_VITA_PRELOOP_MINIZ_STATE)
DEFINE_PHASE_WRAPPER(
    sub_005c2fd0, KAGE_VITA_PRELOOP_INFLATE_STATE)
DEFINE_PHASE_WRAPPER(
    sub_005ce7d0, KAGE_VITA_PRELOOP_INFLATE_BLOCKS)

#undef DEFINE_PHASE_WRAPPER
