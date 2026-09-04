#ifndef KAGE_VITA_PRELOOP_PHASE_H
#define KAGE_VITA_PRELOOP_PHASE_H

#include <stdint.h>

/* Coarse translated-call boundaries reached between KAGE initialization and
 * the first game-loop iteration.  GLFW_ERROR starts at sub_0059fa30, the sole
 * cross-TU parent of the same-TU sub_0059b7a0 error body, so it remains active
 * throughout a stall in that body.  The order is also the bit/nibble order in
 * active_mask and packed_depths. */
enum kage_vita_preloop_phase_slot {
    KAGE_VITA_PRELOOP_GLFW_ERROR = 0,
    KAGE_VITA_PRELOOP_PLATFORM_INIT,
    KAGE_VITA_PRELOOP_ISAAC_STARTUP,
    KAGE_VITA_PRELOOP_LOAD_ARCHIVE,
    KAGE_VITA_PRELOOP_MANAGER_CONSTRUCT,
    KAGE_VITA_PRELOOP_MINIZ_STATE,
    KAGE_VITA_PRELOOP_INFLATE_STATE,
    KAGE_VITA_PRELOOP_INFLATE_BLOCKS,
    KAGE_VITA_PRELOOP_PHASE_COUNT
};

typedef struct kage_vita_preloop_phase_slot_snapshot {
    uint32_t enter_count;
    uint32_t exit_count;
    uint32_t current_depth;
    uint32_t max_depth;
} kage_vita_preloop_phase_slot_snapshot;

typedef struct kage_vita_preloop_phase_snapshot {
    uint32_t active_mask;
    /* Four saturated depth bits per slot, low slot first. */
    uint32_t packed_depths;
    uint32_t total_enter_count;
    uint32_t total_exit_count;
    kage_vita_preloop_phase_slot_snapshot
        slots[KAGE_VITA_PRELOOP_PHASE_COUNT];
} kage_vita_preloop_phase_snapshot;

/* Begin a new watchdog epoch on the sole translated-guest thread, before the
 * watchdog starts.  Backend initialization is itself reached inside the
 * PLATFORM_INIT wrapper, so an already-live depth is seeded into the new
 * epoch rather than cleared; its eventual leave therefore stays balanced. */
void kage_vita_preloop_phase_reset(void);

/* This getter performs only lock-free 32-bit atomic loads.  A watchdog may
 * observe one transition in flight, but it never waits for the guest thread
 * or dereferences guest memory. */
void kage_vita_preloop_phase_get_snapshot(
    kage_vita_preloop_phase_snapshot *out);

#endif
