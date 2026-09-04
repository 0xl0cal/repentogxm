#ifndef ISAAC_HOST_VITA_RENDERFRAME_FASTPATH_H
#define ISAAC_HOST_VITA_RENDERFRAME_FASTPATH_H

#include <stdint.h>

/* A value-only snapshot keeps the equivalence predicate executable on a
 * hostile 64-bit host.  Production gathers these fields from exact guest
 * offsets; the policy itself neither dereferences nor mutates guest memory. */
typedef struct isaac_vita_renderframe_snapshot {
    uint32_t image;
    uint32_t counter;
    uint32_t counter_vftable;
    uint32_t counter_owned_image;
    uint32_t release_observer;
    uint32_t desired_min_filter;
    uint32_t desired_mag_filter;
    uint32_t desired_wrap_s;
    uint32_t desired_wrap_t;
    uint32_t actual_min_filter;
    uint32_t actual_mag_filter;
    uint32_t actual_wrap_s;
    uint32_t actual_wrap_t;
    uint16_t strong_count;
} isaac_vita_renderframe_snapshot;

static inline int isaac_vita_renderframe_snapshot_is_borrowable(
    const isaac_vita_renderframe_snapshot *snapshot,
    uint32_t expected_counter_vftable,
    uint32_t expected_release_observer,
    uint32_t *image_out)
{
    if (!image_out)
        return 0;
    *image_out = 0U;
    if (!snapshot)
        return 0;

    /* GetSpriteSheet returns an empty SmartPointer without synchronization,
     * reference traffic or an observer callback in exactly this state. */
    if (snapshot->image == 0U)
        return snapshot->counter == 0U;

    if (expected_counter_vftable == 0U ||
        snapshot->counter == 0U ||
        snapshot->counter_vftable != expected_counter_vftable ||
        snapshot->counter_owned_image != snapshot->image ||
        snapshot->strong_count == 0U ||
        snapshot->strong_count == UINT16_MAX ||
        snapshot->desired_min_filter != snapshot->actual_min_filter ||
        snapshot->desired_mag_filter != snapshot->actual_mag_filter ||
        snapshot->desired_wrap_s != snapshot->actual_wrap_s ||
        snapshot->desired_wrap_t != snapshot->actual_wrap_t)
        return 0;

    /* The frozen KAGE observer's only external effect occurs when a temporary
     * release leaves exactly one strong reference.  Preserve that edge by
     * falling back; reject every unknown observer.  A disabled observer is
     * always unobservable. */
    if (snapshot->release_observer != 0U &&
        (expected_release_observer == 0U ||
         snapshot->release_observer != expected_release_observer ||
         snapshot->strong_count == 1U))
        return 0;

    *image_out = snapshot->image;
    return 1;
}

int isaac_vita_renderframe_try_borrow(
    uint32_t layer_state,
    uint32_t expected_counter_vftable,
    uint32_t release_observer_slot,
    uint32_t expected_release_observer,
    uint32_t *image_out);

#endif
