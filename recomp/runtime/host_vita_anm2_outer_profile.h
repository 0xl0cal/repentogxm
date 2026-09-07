#ifndef HOST_VITA_ANM2_OUTER_PROFILE_H
#define HOST_VITA_ANM2_OUTER_PROFILE_H
#include <stdint.h>
#define ISAAC_VITA_ANM2_OUTER_PROFILE_ABI 1u

/* Owner-thread cumulative inclusive ANM2 loader scopes, NOT semantic success.
 * Only completed outer calls contribute time; nested calls are already inside
 * that time. Calls crossing a window contribute on completion. max_us is a
 * lifetime maximum. This includes XML, I/O, images, allocation and cleanup:
 * do not add those nested measurements to this total. Snapshot reads no clock.
 * Fatal nonlocal escapes bypass C cleanup and leave depth nonzero. Subsequent
 * calls remain nested/incomplete, not new valid completed intervals. There is
 * deliberately no production reset or retained pointer to an automatic token.
 * bad_sequence/depth/saturated expose invalid or incomplete scope state.
 * Every bad_sequence permanently disables pairing. Its cumulative value
 * must stay visible even when later window deltas are zero (badseqever in
 * ph120.anm2o); it is not a window-only error count. Snapshot ABI unchanged. */
typedef struct {
    uint32_t started, completed, timed, depth, nested;
    uint32_t bad_clock, bad_sequence, saturated;
    uint64_t total_us, max_us;
} IsaacVitaAnm2OuterSnapshot;

uint64_t isaac_vita_anm2_outer_begin(void);
void isaac_vita_anm2_outer_cleanup(uint64_t *token);
void isaac_vita_anm2_outer_snapshot(IsaacVitaAnm2OuterSnapshot *out);

#if defined(ISAAC_VITA_ANM2_OUTER_PROFILE_ORACLE)
void isaac_vita_anm2_outer_oracle_reset(void);
void isaac_vita_anm2_outer_oracle_seed(const IsaacVitaAnm2OuterSnapshot *value,
                                     uint32_t generation);
#endif
#endif
