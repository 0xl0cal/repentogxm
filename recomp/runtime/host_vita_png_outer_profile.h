#ifndef HOST_VITA_PNG_OUTER_PROFILE_H
#define HOST_VITA_PNG_OUTER_PROFILE_H
#include <stdint.h>
#define ISAAC_VITA_PNG_OUTER_PROFILE_ABI 1u

/* Owner-thread cumulative snapshot of the two existing ImagePng call-site
 * brackets. Completed outer calls, including libpng error returns, contribute
 * once; nested calls are included in their parent's time, not added twice.
 * A call crossing a profile boundary contributes on completion. These totals
 * INCLUDE native PNG decode/I/O/row service, premultiply, GL upload and cleanup;
 * they are not an exclusive CPU bucket. max_us is lifetime maximum, NOT a
 * window maximum. timed == completed is required for complete timing coverage.
 * depth/nested/bad_* and sticky saturated expose invalid or incomplete scopes.
 * The snapshot performs no clock reads, logging, locking or reset. */
typedef struct {
    uint32_t started, completed, timed, depth, nested;
    uint32_t bad_clock, bad_sequence, saturated;
    uint64_t total_us, max_us;
} IsaacVitaPngOuterSnapshot;

void isaac_vita_png_outer_snapshot(IsaacVitaPngOuterSnapshot *out);
void isaac_vita_png_outer_begin(void);
void isaac_vita_png_outer_end(void);
/* Shared timestamps from the explicitly enabled legacy sampled profiler avoid
 * a second outer clock pair. Its initial calibration and final reporting lie
 * outside these brackets, just as they do in the legacy sampled image time. */
void isaac_vita_png_outer_begin_at(uint64_t now);
void isaac_vita_png_outer_end_at(uint64_t now);

#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
void isaac_vita_png_outer_oracle_reset(void);
void isaac_vita_png_outer_oracle_seed(const IsaacVitaPngOuterSnapshot *value);
#endif
#endif
