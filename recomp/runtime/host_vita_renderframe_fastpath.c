/* Fail-closed native acquisition for the frozen J835 RenderFrame seam. */
#include <stdint.h>

#include "guest.h"
#include "host_vita_renderframe_fastpath.h"

#if !defined(ISAAC_VITA_RENDERFRAME_FASTPATH_ORACLE) && \
    UINTPTR_MAX != UINT32_MAX
#error RenderFrame borrowing requires the 32-bit identity-mapped Vita runtime
#endif

enum {
    ISAAC_RENDERFRAME_STATE_MIN_FILTER = 0x20,
    ISAAC_RENDERFRAME_STATE_MAG_FILTER = 0x24,
    ISAAC_RENDERFRAME_STATE_WRAP_S = 0x28,
    ISAAC_RENDERFRAME_STATE_WRAP_T = 0x2c,
    ISAAC_RENDERFRAME_STATE_IMAGE = 0x94,
    ISAAC_RENDERFRAME_STATE_COUNTER = 0x98,
    ISAAC_RENDERFRAME_IMAGE_MIN_FILTER = 0x24,
    ISAAC_RENDERFRAME_IMAGE_MAG_FILTER = 0x28,
    ISAAC_RENDERFRAME_IMAGE_WRAP_S = 0x2c,
    ISAAC_RENDERFRAME_IMAGE_WRAP_T = 0x30,
    ISAAC_RENDERFRAME_COUNTER_VFTABLE = 0x00,
    ISAAC_RENDERFRAME_COUNTER_STRONG = 0x04,
    ISAAC_RENDERFRAME_COUNTER_OWNED_IMAGE = 0x14
};

static int renderframe_sampler_snapshot_matches(
    const isaac_vita_renderframe_snapshot *snapshot)
{
    return snapshot->desired_min_filter == snapshot->actual_min_filter &&
           snapshot->desired_mag_filter == snapshot->actual_mag_filter &&
           snapshot->desired_wrap_s == snapshot->actual_wrap_s &&
           snapshot->desired_wrap_t == snapshot->actual_wrap_t;
}

int isaac_vita_renderframe_try_borrow(
    uint32_t layer_state,
    uint32_t expected_counter_vftable,
    uint32_t release_observer_slot,
    uint32_t expected_release_observer,
    uint32_t *image_out)
{
    isaac_vita_renderframe_snapshot snapshot = {0};

    if (!image_out)
        return 0;
    *image_out = 0U;

    snapshot.image = ld32(layer_state + ISAAC_RENDERFRAME_STATE_IMAGE);
    if (snapshot.image == 0U) {
        snapshot.counter = ld32(
            layer_state + ISAAC_RENDERFRAME_STATE_COUNTER);
        return isaac_vita_renderframe_snapshot_is_borrowable(
            &snapshot, expected_counter_vftable,
            expected_release_observer, image_out);
    }

    /* Preserve GetSpriteSheet's only semantic work: if any sampler mode is
     * stale, let the original virtual setters and SmartPointer copy run. */
    snapshot.desired_min_filter = ld32(
        layer_state + ISAAC_RENDERFRAME_STATE_MIN_FILTER);
    snapshot.actual_min_filter = ld32(
        snapshot.image + ISAAC_RENDERFRAME_IMAGE_MIN_FILTER);
    snapshot.actual_mag_filter = ld32(
        snapshot.image + ISAAC_RENDERFRAME_IMAGE_MAG_FILTER);
    snapshot.desired_mag_filter = ld32(
        layer_state + ISAAC_RENDERFRAME_STATE_MAG_FILTER);
    snapshot.desired_wrap_s = ld32(
        layer_state + ISAAC_RENDERFRAME_STATE_WRAP_S);
    snapshot.actual_wrap_s = ld32(
        snapshot.image + ISAAC_RENDERFRAME_IMAGE_WRAP_S);
    snapshot.actual_wrap_t = ld32(
        snapshot.image + ISAAC_RENDERFRAME_IMAGE_WRAP_T);
    snapshot.desired_wrap_t = ld32(
        layer_state + ISAAC_RENDERFRAME_STATE_WRAP_T);
    if (!renderframe_sampler_snapshot_matches(&snapshot))
        return 0;

    snapshot.counter = ld32(
        layer_state + ISAAC_RENDERFRAME_STATE_COUNTER);
    if (snapshot.counter == 0U)
        return 0;
    snapshot.counter_vftable = ld32(
        snapshot.counter + ISAAC_RENDERFRAME_COUNTER_VFTABLE);
    snapshot.strong_count = ld16(
        snapshot.counter + ISAAC_RENDERFRAME_COUNTER_STRONG);
    snapshot.counter_owned_image = ld32(
        snapshot.counter + ISAAC_RENDERFRAME_COUNTER_OWNED_IMAGE);
    snapshot.release_observer = ld32(release_observer_slot);

    /* The frozen RenderFrame reads these owner fields without synchronization.
     * Take a second exact snapshot before exposing the pair anyway: any
     * re-entrant or corrupted mutation observed here falls back.  Codegen
     * limits borrowing to the NULL/size consumers; virtual Render calls retain
     * their original owning SmartPointer. */
    if (ld32(layer_state + ISAAC_RENDERFRAME_STATE_IMAGE) != snapshot.image ||
        ld32(layer_state + ISAAC_RENDERFRAME_STATE_COUNTER) != snapshot.counter ||
        ld32(layer_state + ISAAC_RENDERFRAME_STATE_MIN_FILTER) !=
            snapshot.desired_min_filter ||
        ld32(layer_state + ISAAC_RENDERFRAME_STATE_MAG_FILTER) !=
            snapshot.desired_mag_filter ||
        ld32(layer_state + ISAAC_RENDERFRAME_STATE_WRAP_S) !=
            snapshot.desired_wrap_s ||
        ld32(layer_state + ISAAC_RENDERFRAME_STATE_WRAP_T) !=
            snapshot.desired_wrap_t ||
        ld32(snapshot.image + ISAAC_RENDERFRAME_IMAGE_MIN_FILTER) !=
            snapshot.actual_min_filter ||
        ld32(snapshot.image + ISAAC_RENDERFRAME_IMAGE_MAG_FILTER) !=
            snapshot.actual_mag_filter ||
        ld32(snapshot.image + ISAAC_RENDERFRAME_IMAGE_WRAP_S) !=
            snapshot.actual_wrap_s ||
        ld32(snapshot.image + ISAAC_RENDERFRAME_IMAGE_WRAP_T) !=
            snapshot.actual_wrap_t ||
        ld32(snapshot.counter + ISAAC_RENDERFRAME_COUNTER_VFTABLE) !=
            snapshot.counter_vftable ||
        ld16(snapshot.counter + ISAAC_RENDERFRAME_COUNTER_STRONG) !=
            snapshot.strong_count ||
        ld32(snapshot.counter + ISAAC_RENDERFRAME_COUNTER_OWNED_IMAGE) !=
            snapshot.counter_owned_image ||
        ld32(release_observer_slot) != snapshot.release_observer)
        return 0;

    return isaac_vita_renderframe_snapshot_is_borrowable(
        &snapshot, expected_counter_vftable,
        expected_release_observer, image_out);
}
