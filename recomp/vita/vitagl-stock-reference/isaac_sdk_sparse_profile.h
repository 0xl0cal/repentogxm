#ifndef ISAAC_SDK_SPARSE_PROFILE_H
#define ISAAC_SDK_SPARSE_PROFILE_H

#include <stdint.h>

/* A mode of the existing native GL_TIME observer. Every duration is caller
 * process-clock wall time, NOT GPU execution time. Canonical outer/draw cover
 * the same selected, single-SDK-call, nonnested spans; clear/queue cover all
 * actual calls. Selected spans that cannot be paired increment unpaired;
 * neither duration is published, but all their clock reads remain counted.
 * Other SDK calls and noncanonical paths are not sampled; other_draw_calls
 * counts only SDK Draw calls compiled within draw.c outside a canonical span.
 * Owner-thread only; Begin/End keep their legacy unchecked arithmetic.
 * Counts include bad-clock samples; bad/saturation invalidate affected totals.
 */
enum {
    ISAAC_SDK_CANONICAL, ISAAC_SDK_DRAW, ISAAC_SDK_CLEAR,
    ISAAC_SDK_CLEAR_VERTEX, ISAAC_SDK_CLEAR_FRAGMENT, ISAAC_SDK_CLEAR_DRAW,
    ISAAC_SDK_QUEUE, ISAAC_SDK_CONTROL, ISAAC_SDK_BUCKET_COUNT
};
typedef struct {
    uint32_t calls, us, max_us;
} IsaacSdkSparseBucket;
typedef struct {
    uint32_t residue, canonical_seen, canonical_selected, canonical_issued;
    uint32_t canonical_unpaired, canonical_draw_errors, other_draw_calls;
    uint32_t begin_calls, end_calls, clock_reads;
    uint32_t bad_clock, saturation;
    IsaacSdkSparseBucket bucket[ISAAC_SDK_BUCKET_COUNT];
} IsaacSdkSparseProfile;

/* NULL discards pre-window counters without a calibration clock. A non-NULL
 * take includes one empty bracket, copies counters, then starts next_window.
 * Window id is supplied by the existing phase consumer, not a second clock.
 * Take/reset is only valid between native operations, never inside a span. */
void vglIsaacSdkSparseTake(IsaacSdkSparseProfile *out, uint32_t next_window);

#endif
