#include "isaac_fbo_rt_reuse.h"

#if !defined(HAVE_SINGLE_THREADED_GC) || defined(HAVE_PTHREAD) || \
    defined(HAVE_SHARED_RENDERTARGETS) || defined(RECYCLE_RENDERTARGETS)
#error "Isaac RT reuse requires the stock single-owner, single-threaded GC path"
#endif

typedef struct IsaacFboRtHandle {
    uintptr_t target;
    int w, h;
} IsaacFboRtHandle;

static struct {
    uintptr_t owner;
    IsaacFboRtHandle active, spare;
    int spare_bucket;
    int spare_reusable;
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
    int interlude;
#endif
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    int lease_started, lease_last_bucket;
    uint32_t recovery_depth;
    int recovery_poison;
#endif
} rt_reuse;

#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
static IsaacFboRtReuseStats rt_profile;

static void profile_increment(uint32_t *counter, unsigned bit)
{
    if (*counter != UINT32_MAX)
        ++*counter;
    else
        rt_profile.saturation_mask |= UINT32_C(1) << bit;
}

#define RT_PROFILE_INC(field, bit) profile_increment(&rt_profile.field, bit)

void vglIsaacFboRtReuseProfileTake(IsaacFboRtReuseStats *out)
{
    if (!out)
        return;
    *out = rt_profile;
    rt_profile = (IsaacFboRtReuseStats){0};
}
#else
#define RT_PROFILE_INC(field, bit) ((void)0)
#endif

static int observed_size(int w, int h)
{
    /* Physical backing sizes from ROOM_DIAG w12, not logical viewports.
     * Other dimensions retain the stock lifecycle until separately measured. */
    return (w == 480 && h == 272) || (w == 784 && h == 472);
}

#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
static IsaacFboRtInterludeStats interlude_profile;

static void interlude_increment(uint32_t *counter, unsigned bit)
{
    if (*counter != UINT32_MAX)
        ++*counter;
    else
        interlude_profile.saturation_mask |= UINT32_C(1) << bit;
}

void vglIsaacFboRtInterludeProfileTake(IsaacFboRtInterludeStats *out)
{
    if (!out)
        return;
    *out = interlude_profile;
    interlude_profile = (IsaacFboRtInterludeStats){0};
}

static int interlude_size(int w, int h)
{
    /* These are temporary ACTIVE shapes observed in the same broad room
     * windows, never additional spare/cache shapes. No arbitrary-size path. */
    return (w == 64 && h == 56) || (w == 232 && h == 208);
}
#endif

static void invalidate_owner(uintptr_t owner)
{
    if (owner && rt_reuse.owner == owner) {
        rt_reuse.owner = 0;
        rt_reuse.active.target = 0;
        rt_reuse.spare_reusable = 0;
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
        rt_reuse.interlude = 0;
#endif
        /* Do not enqueue the spare now: doing so would restart its grace
         * period. No lease extension is granted after revocation; an already
         * elapsed original grace needs no fresh destruction delay. */
    }
}

void isaacFboRtInvalidate(uintptr_t owner)
{
    if (owner && rt_reuse.owner == owner)
        RT_PROFILE_INC(explicit_invalidations, 5);
    invalidate_owner(owner);
}

void isaacFboRtCreated(uintptr_t owner, uintptr_t target, int w, int h,
                       int compatible)
{
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    if (rt_reuse.recovery_depth || rt_reuse.recovery_poison) {
        invalidate_owner(owner);
        return;
    }
#endif
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
    if (rt_reuse.interlude && owner == rt_reuse.owner) {
        /* The original scene-reset create must resolve the exact pending
         * current. Failure/fallback or an unexpected publication revokes it. */
        if (target && compatible && !rt_reuse.active.target &&
                rt_reuse.active.w == w && rt_reuse.active.h == h &&
                interlude_size(w, h)) {
            rt_reuse.active.target = target;
            return;
        }
        invalidate_owner(owner);
        return;
    }
#endif
    if (!owner || !target || !compatible || !observed_size(w, h)) {
        invalidate_owner(owner);
        return;
    }
    if (rt_reuse.owner && rt_reuse.owner != owner)
        return;
    /* A dead owner's spare still has its original deferred lifetime. Do not
     * transfer it to a recycled FBO address, or claim a second owner early. */
    if (!rt_reuse.owner && rt_reuse.spare.target)
        return;
    rt_reuse.owner = owner;
    rt_reuse.active.target = target;
    rt_reuse.active.w = w;
    rt_reuse.active.h = h;
}

int isaacFboRtSwitch(uintptr_t owner, uintptr_t current,
                     int old_w, int old_h, int new_w, int new_h,
                     int compatible, int purge_bucket, uintptr_t *next)
{
    IsaacFboRtHandle previous, replacement = {0, 0, 0};
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    /* A nested scene_reset during glFinish must not repopulate the lease. */
    if (rt_reuse.recovery_depth || rt_reuse.recovery_poison)
        compatible = 0;
#endif
    RT_PROFILE_INC(switch_calls, 0);
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
    /* Keep only an already owned, pre-original-deadline spare. The current
     * intermediate is always retired by the native caller, never cached. */
    if (next && owner && current && compatible &&
            purge_bucket >= 0 && purge_bucket < 4 &&
            rt_reuse.owner == owner && rt_reuse.active.target == current &&
            rt_reuse.active.w == old_w && rt_reuse.active.h == old_h &&
            (old_w != new_w || old_h != new_h) &&
            rt_reuse.spare.target && rt_reuse.spare.target != current &&
            rt_reuse.spare_reusable &&
            observed_size(rt_reuse.spare.w, rt_reuse.spare.h)
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
            && !rt_reuse.lease_started
#endif
            ) {
        if (rt_reuse.interlude && rt_reuse.spare.w == new_w &&
                rt_reuse.spare.h == new_h) {
            rt_reuse.active = rt_reuse.spare;
            rt_reuse.spare.target = 0;
            rt_reuse.spare_reusable = 0;
            rt_reuse.interlude = 0;
            *next = rt_reuse.active.target;
            RT_PROFILE_INC(hits, 1);
            interlude_increment(&interlude_profile.restores, 1);
            return ISAAC_FBO_RT_RETIRES_CURRENT;
        }
        if (interlude_size(new_w, new_h) &&
                (rt_reuse.interlude || observed_size(old_w, old_h))) {
            rt_reuse.active = (IsaacFboRtHandle){0, new_w, new_h};
            rt_reuse.interlude = 1;
            *next = 0;
            RT_PROFILE_INC(first_misses, 2);
            interlude_increment(&interlude_profile.holds, 0);
            return ISAAC_FBO_RT_RETIRES_CURRENT;
        }
    }
#endif
    if (!next || !owner || !current || !compatible || purge_bucket < 0 ||
            rt_reuse.owner != owner || rt_reuse.active.target != current ||
            rt_reuse.active.w != old_w || rt_reuse.active.h != old_h ||
            !observed_size(old_w, old_h) || !observed_size(new_w, new_h) ||
            (old_w == new_w && old_h == new_h)) {
#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
        /* Classify only after the unchanged acceptance predicate rejects. The
         * first reason owns the call even if several supplied fields disagree. */
        if (!next || !owner || purge_bucket < 0)
            RT_PROFILE_INC(reject_args, 10);
        else if (!compatible)
            RT_PROFILE_INC(reject_policy, 8);
        else if (rt_reuse.owner != owner)
            RT_PROFILE_INC(reject_owner, 6);
        else if (!current || rt_reuse.active.target != current ||
                rt_reuse.active.w != old_w || rt_reuse.active.h != old_h)
            RT_PROFILE_INC(reject_current, 9);
        else
            RT_PROFILE_INC(reject_size, 7);
#endif
        invalidate_owner(owner);
        return 0;
    }
    if (rt_reuse.spare.target) {
        if (!rt_reuse.spare_reusable || rt_reuse.spare.w != new_w ||
                rt_reuse.spare.h != new_h || rt_reuse.spare.target == current) {
            RT_PROFILE_INC(reject_spare, 11);
            invalidate_owner(owner);
            return 0;
        }
        replacement = rt_reuse.spare;
    }

    if (replacement.target)
        RT_PROFILE_INC(hits, 1);
    else
        RT_PROFILE_INC(first_misses, 2);
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    if (replacement.target && rt_reuse.lease_started)
        RT_PROFILE_INC(lease_hits, 13);
#endif
    previous = rt_reuse.active;
    rt_reuse.active = replacement;
    rt_reuse.spare = previous;
    rt_reuse.spare_bucket = purge_bucket;
    rt_reuse.spare_reusable = 1;
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    /* Only real handle transfer renews retirement, never idle collection. */
    rt_reuse.lease_started = 0;
#endif
    *next = replacement.target;
    return 1;
}

uintptr_t isaacFboRtCollect(int purge_bucket)
{
    uintptr_t target;
    if (!rt_reuse.spare.target)
        return 0;
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
    if (rt_reuse.interlude) {
        /* Neither pending nor resolved intermediates grant/renew a lease.
         * Lack of an exact return saves zero work at the original deadline. */
        if (rt_reuse.spare_bucket != purge_bucket)
            return 0;
        goto expire_interlude;
    }
#endif
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    if (rt_reuse.lease_started) {
        /* The original deadline has passed. Revocation can therefore collect
         * at this visit without adding a fresh retirement grace period. */
        if (rt_reuse.spare_reusable && rt_reuse.owner &&
                rt_reuse.active.target &&
                rt_reuse.active.target != rt_reuse.spare.target &&
                !rt_reuse.recovery_depth && !rt_reuse.recovery_poison) {
            if (purge_bucket == rt_reuse.lease_last_bucket)
                return 0; /* Repeated observation is not another rotation. */
            if (purge_bucket == ((rt_reuse.lease_last_bucket + 1) & 3)) {
                rt_reuse.lease_last_bucket = purge_bucket;
                if (purge_bucket != rt_reuse.spare_bucket)
                    return 0;
            }
            /* One full rotation, or an unexpected sequence: expire. */
        }
    } else {
        if (rt_reuse.spare_bucket != purge_bucket)
            return 0;
        if (purge_bucket >= 0 && purge_bucket < 4 &&
                rt_reuse.spare_reusable && rt_reuse.owner &&
                rt_reuse.active.target &&
                rt_reuse.active.target != rt_reuse.spare.target &&
                !rt_reuse.recovery_depth && !rt_reuse.recovery_poison) {
            rt_reuse.lease_started = 1;
            rt_reuse.lease_last_bucket = purge_bucket;
            RT_PROFILE_INC(lease_grants, 12);
            return 0;
        }
    }
#else
    if (rt_reuse.spare_bucket != purge_bucket)
        return 0;
#endif
#if defined(HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE) && HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE
expire_interlude:
    rt_reuse.interlude = 0;
#endif
    if (rt_reuse.spare_reusable)
        RT_PROFILE_INC(expired_reusable, 3);
    else
        RT_PROFILE_INC(expired_revoked, 4);
    target = rt_reuse.spare.target;
    rt_reuse.spare.target = 0;
    rt_reuse.spare_reusable = 0;
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
    rt_reuse.lease_started = 0;
#endif
    return target;
}

#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
void isaacFboRtRecoveryBegin(void)
{
    /* This precedes Finish, whose scene_reset may itself enter recovery. */
    if (rt_reuse.recovery_depth == UINT32_MAX)
        rt_reuse.recovery_poison = 1;
    else
        ++rt_reuse.recovery_depth;
    invalidate_owner(rt_reuse.owner);
}

uintptr_t isaacFboRtRecoveryTake(void)
{
    uintptr_t target;
    /* Finish is not an original-deadline witness or an EndScene. Only the
     * real Collect for this retirement can have granted the extra rotation.
     * Recheck AFTER Finish: nested recovery/collection may consume the spare. */
    if (!rt_reuse.recovery_depth || !rt_reuse.spare.target ||
            !rt_reuse.lease_started)
        return 0;
    target = rt_reuse.spare.target;
    RT_PROFILE_INC(lease_drains, 14);
    rt_reuse.spare.target = 0;
    rt_reuse.spare_reusable = 0;
    rt_reuse.lease_started = 0;
    return target;
}

void isaacFboRtRecoveryEnd(void)
{
    if (rt_reuse.recovery_depth)
        --rt_reuse.recovery_depth;
    else {
        /* An unmatched End must not accidentally re-enable lease admission. */
        rt_reuse.recovery_poison = 1;
        invalidate_owner(rt_reuse.owner);
    }
}

int isaacFboRtHasSpare(void)
{
    return rt_reuse.spare.target != 0;
}

int isaacFboRtHasMatureSpare(void)
{
    /* Read-only pre-Finish optimization, never permission to detach. */
    return rt_reuse.spare.target != 0 && rt_reuse.lease_started;
}
#endif
