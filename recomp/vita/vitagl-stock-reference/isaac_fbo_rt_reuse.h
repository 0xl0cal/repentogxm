#ifndef ISAAC_FBO_RT_REUSE_H
#define ISAAC_FBO_RT_REUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0007 deliberately keeps this declaration out of the public vitaGL.h. */
uint8_t vglIsaacSetupFboRenderTargetScenes(uint8_t size);

/* Single owner, one spare, no pixels/depth and no allocation. Handles use
 * uintptr_t so the same ownership implementation is exercised by host tests.
 * A spare is NOT in the native purge queue. Its original purge bucket remains
 * its destruction deadline unless the experimental LEASE extension below is
 * compiled in; it always remains the earliest destruction boundary. */
void isaacFboRtCreated(uintptr_t owner, uintptr_t target, int w, int h,
                       int compatible);
/* INTERLUDE-only result bit: the native caller must enqueue its unchanged
 * current target at the original retirement site/bucket before assigning
 * *next. Original 0/1 returns retain their original ownership meaning. */
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
#define ISAAC_FBO_RT_RETIRES_CURRENT 2
#endif
int isaacFboRtSwitch(uintptr_t owner, uintptr_t current,
                     int old_w, int old_h, int new_w, int new_h,
                     int compatible, int purge_bucket, uintptr_t *next);
void isaacFboRtInvalidate(uintptr_t owner);
uintptr_t isaacFboRtCollect(int purge_bucket);

/* Experimental one-additional-rotation lease, native build only. The native
 * integration asserts FRAME_PURGE_FREQ == 4. No additional handle is owned.
 * RecoveryBegin revokes admission before a potentially recursive Finish;
 * RecoveryTake is legal ONLY AFTER Finish, and detaches only when an actual
 * Collect already reached THIS retirement's original bucket. Before then,
 * pressure revokes reuse but preserves the spare and its original bucket.
 * Finish alone is not an EndScene or permission to destroy early.
 * Begin/End must nest; an unmatched End permanently disables new admission.
 * OFF builds expose none of these endpoints and retain original Collect. */
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
void isaacFboRtRecoveryBegin(void);
uintptr_t isaacFboRtRecoveryTake(void);
void isaacFboRtRecoveryEnd(void);
int isaacFboRtHasSpare(void);
int isaacFboRtHasMatureSpare(void);
void vglIsaacFboRtDrainAfterFinish(void);
#endif

/* Independent of IsaacNativeResourceStats/nr ABI. The type/declaration are
 * shared with the app, whose feature defines differ from vitaGL's HAVE_ flags.
 * The producer exists only with HAVE_ISAAC_NATIVE_RESOURCE_PROFILE. Call only
 * when both RT reuse and native resource profiling are linked into vitaGL. */
#define ISAAC_FBO_RT_REUSE_PROFILE_ABI 2
typedef struct IsaacFboRtReuseStats {
    uint32_t switch_calls, hits, first_misses;
    uint32_t expired_reusable, expired_revoked, explicit_invalidations;
    uint32_t reject_owner, reject_size, reject_policy, reject_current;
    uint32_t reject_args, reject_spare;
    uint32_t saturation_mask;
    uint32_t lease_grants, lease_hits, lease_drains;
} IsaacFboRtReuseStats;

/* Each Switch increments calls and exactly one of hit, first_miss, or reject.
 * A first_miss is an accepted switch with no spare, including after expiry;
 * it is not restricted to the first switch of the owner or launch. With the
 * optional interlude path, first_miss also counts an accepted intermediate
 * with no replacement output; its existing spare stays unchanged. A restored
 * exact spare is a hit, not an intermediate-size cache admission.
 * Reject precedence: args (NULL out/owner, negative bucket), incompatible policy,
 * owner, current handle/old dimensions, unsupported or unchanged size, spare.
 * Expiry counts only handles returned by Collect (original bucket with LEASE
 * OFF; possibly one rotation later with LEASE ON). Post-deadline pressure draining is
 * a native retirement/destruction, not Collect expiry. Revoked includes internal
 * rejection cleanup as well as explicit invalidation.
 * Explicit invalidation counts effective public Invalidate calls, not ignored
 * foreign/repeated calls or internal Created/Switch rejection cleanup.
 * Counters saturate at UINT32_MAX. Losing an increment sets that field's bit in
 * saturation_mask (old counters bits 0..11, lease counters bits 12..14); no wrap is silent.
 * ABI2 appends grants (actual extension), hits (mature spare accepted before
 * Switch resets its retirement), and drains (nonzero guarded RecoveryTake).
 * LEASE OFF records zero for all three; profile OFF has no counter producer.
 * Windows are event-local: a grant, later reuse or drain can fall in different
 * windows. None is elapsed time or proof of avoided creation/allocation cost.
 * Take copies and resets counters only, never ownership/deadlines. NULL is a
 * no-op. All calls retain the existing single-threaded GC contract. */
void vglIsaacFboRtReuseProfileTake(IsaacFboRtReuseStats *out);

/* Narrow event-only observer, independent of the broad resource profiler.
 * Producer exists ONLY with HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1. Holds count
 * accepted intermediate switches (possibly several per tour); restores count
 * exact saved-target returns that bypass native creation. Neither is time.
 * Take resets counters, not ownership. NULL is a no-op. Saturation bits 0/1.
 * The consumer must use this exact header and match the native feature flag. */
typedef struct IsaacFboRtInterludeStats {
    uint32_t holds, restores, saturation_mask;
} IsaacFboRtInterludeStats;
void vglIsaacFboRtInterludeProfileTake(IsaacFboRtInterludeStats *out);

#ifdef __cplusplus
}
#endif

#endif
