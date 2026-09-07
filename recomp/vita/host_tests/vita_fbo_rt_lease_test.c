#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../vitagl-stock-reference/isaac_fbo_rt_reuse.h"
#include "../vitagl-stock-reference/isaac_fbo_rt_reuse.c"

#ifndef HAVE_ISAAC_FBO_RT_REUSE_LEASE
#error "This focused fixture exercises the experimental lease ON path"
#endif

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
#define OWNER ((uintptr_t)0x1110)
#define OTHER ((uintptr_t)0x2220)
static unsigned checks, next_handle = 1, destroyed[512];
/* Independent per-retirement expectation; never derive permission from the
 * policy's lease_started or from Finish. White-box access below is limited to
 * injecting otherwise impractical recovery-depth overflow. */
static unsigned retired[512], deadline_seen[512], retirement_epoch[512];
static int original_bucket[512];

static int test_switch(uintptr_t owner, uintptr_t current, int ow, int oh,
                       int nw, int nh, int compatible, int bucket, uintptr_t *next)
{
    int ok = isaacFboRtSwitch(owner, current, ow, oh, nw, nh, compatible, bucket, next);
    if (ok) {
        if (*next) retired[*next] = 0;
        retired[current] = 1;
        deadline_seen[current] = 0;
        original_bucket[current] = bucket;
        ++retirement_epoch[current];
    }
    return ok;
}

static uintptr_t test_collect(int bucket)
{
    for (unsigned i = 1; i < next_handle; ++i)
        if (retired[i] && original_bucket[i] == bucket) deadline_seen[i] = 1;
    uintptr_t target = isaacFboRtCollect(bucket);
    if (target) CHECK(retired[target] && deadline_seen[target]);
    return target;
}

static uintptr_t test_take(void)
{
    uintptr_t target = isaacFboRtRecoveryTake();
    if (target) CHECK(retired[target] && deadline_seen[target]);
    return target;
}
#define isaacFboRtSwitch test_switch
#define isaacFboRtCollect test_collect
#define isaacFboRtRecoveryTake test_take

static uintptr_t fresh(void)
{
    CHECK(next_handle < 512);
    return next_handle++;
}

static void destroy(uintptr_t target)
{
    CHECK(target > 0 && target < next_handle);
    CHECK(!destroyed[target]);
    CHECK(!retired[target] || deadline_seen[target]);
    retired[target] = 0;
    destroyed[target] = 1;
}

static void begin_pair(uintptr_t *a, uintptr_t *b, int bucket)
{
    uintptr_t out = 999;
    *a = fresh();
    *b = fresh();
    isaacFboRtCreated(OWNER, *a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER, *a, 480, 272, 784, 472, 1, bucket, &out));
    CHECK(out == 0);
    isaacFboRtCreated(OWNER, *b, 784, 472, 1);
    CHECK(isaacFboRtHasSpare());
}

static void finish_active(uintptr_t active)
{
    isaacFboRtInvalidate(OWNER);
    destroy(active); /* Framebuffer, not the policy, owns its current target. */
    CHECK(!isaacFboRtHasSpare());
}

static void bounded_rotation(void)
{
    uintptr_t a, b;
    begin_pair(&a, &b, 3);
    for (int bucket = 0; bucket < 3; ++bucket)
        CHECK(isaacFboRtCollect(bucket) == 0);
    CHECK(isaacFboRtCollect(3) == 0); /* First original deadline is retained. */
    for (int i = 0; i < 100; ++i)
        CHECK(isaacFboRtCollect(3) == 0); /* Idle repeats cannot age/reset it. */
    for (int bucket = 0; bucket < 3; ++bucket) {
        CHECK(isaacFboRtCollect(bucket) == 0);
        CHECK(isaacFboRtCollect(bucket) == 0);
        isaacFboRtCreated(OWNER, b, 784, 472, 1); /* Must not renew grace. */
    }
    CHECK(isaacFboRtCollect(3) == a);
    destroy(a);
    CHECK(isaacFboRtCollect(3) == 0);
    finish_active(b);
}

static void invalidation_lower_bound(int after_deadline)
{
    uintptr_t a, b;
    begin_pair(&a, &b, 2);
    if (after_deadline)
        CHECK(isaacFboRtCollect(2) == 0);
    isaacFboRtInvalidate(OWNER);
    /* A newly generated framebuffer at exactly the old address cannot adopt
     * the old generation's target or reset its deadline. */
    uintptr_t recycled = fresh(), out = 999;
    isaacFboRtCreated(OWNER, recycled, 480, 272, 1);
    CHECK(!isaacFboRtSwitch(OWNER, recycled, 480, 272, 784, 472, 1, 0, &out));
    CHECK(out == 999);
    if (after_deadline) {
        CHECK(isaacFboRtCollect(3) == a); /* Already past original grace. */
    } else {
        CHECK(isaacFboRtCollect(0) == 0);
        CHECK(isaacFboRtCollect(1) == 0);
        CHECK(isaacFboRtCollect(2) == a); /* Never earlier without a fence. */
    }
    destroy(a);
    destroy(b);
    destroy(recycled);
    CHECK(!isaacFboRtHasSpare());
}

static void reuse_after_original_deadline(void)
{
    uintptr_t a, b, out = 999;
    begin_pair(&a, &b, 0);
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtCollect(1) == 0);
    CHECK(isaacFboRtSwitch(OWNER, b, 784, 472, 480, 272, 1, 2, &out));
    CHECK(out == a && !destroyed[out]);
    CHECK(isaacFboRtCollect(1) == 0); /* B has its own new original deadline. */
    CHECK(isaacFboRtCollect(2) == 0);
    CHECK(isaacFboRtCollect(3) == 0);
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtCollect(1) == 0);
    CHECK(isaacFboRtCollect(2) == b);
    destroy(b);
    finish_active(a);
}

static void unknown_sequence_expires(void)
{
    uintptr_t a, b;
    begin_pair(&a, &b, 0);
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtCollect(2) == a); /* Fail closed after old deadline. */
    destroy(a);
    finish_active(b);
}

static void no_active_no_extension(void)
{
    uintptr_t a = fresh(), out;
    isaacFboRtCreated(OWNER, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER, a, 480, 272, 784, 472, 1, 1, &out));
    CHECK(!out);
    CHECK(isaacFboRtCollect(1) == a); /* No new scene/target was created. */
    destroy(a);
    isaacFboRtInvalidate(OWNER);
}

static void nested_pressure(void)
{
    uintptr_t a, b, c = fresh(), d = fresh(), out = 999;
    begin_pair(&a, &b, 3);
    CHECK(isaacFboRtRecoveryTake() == 0); /* Cannot bypass normal grace. */
    isaacFboRtRecoveryBegin();
    isaacFboRtCreated(OTHER, c, 480, 272, 1);
    CHECK(!isaacFboRtSwitch(OTHER, c, 480, 272, 784, 472, 1, 0, &out));
    CHECK(out == 999);
    isaacFboRtRecoveryBegin(); /* Re-entry through glFinish/scene_reset. */
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtRecoveryTake() == 0); /* Finish cannot create a deadline. */
    CHECK(!isaacFboRtHasMatureSpare());
    CHECK(isaacFboRtCollect(3) == a); /* Nested original collection consumes it. */
    destroy(a);
    CHECK(isaacFboRtRecoveryTake() == 0);
    isaacFboRtRecoveryEnd();
    isaacFboRtCreated(OTHER, c, 480, 272, 1);
    CHECK(!isaacFboRtSwitch(OTHER, c, 480, 272, 784, 472, 1, 0, &out));
    isaacFboRtRecoveryEnd();
    isaacFboRtCreated(OTHER, c, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OTHER, c, 480, 272, 784, 472, 1, 0, &out));
    CHECK(!out);
    isaacFboRtCreated(OTHER, d, 784, 472, 1);
    isaacFboRtInvalidate(OTHER);
    CHECK(isaacFboRtCollect(0) == c);
    destroy(c);
    destroy(d);
    destroy(b);
}

static void recovery_with_original_collection(void)
{
    uintptr_t a, b;
    begin_pair(&a, &b, 1);
    isaacFboRtRecoveryBegin();
    CHECK(isaacFboRtCollect(1) == a); /* Forced GC during the outer Finish. */
    destroy(a);
    CHECK(isaacFboRtRecoveryTake() == 0); /* Never destroy it twice. */
    isaacFboRtRecoveryEnd();
    destroy(b);
}

static void pressure_after_deadline(void)
{
    uintptr_t a, b;
    begin_pair(&a, &b, 0);
    CHECK(!isaacFboRtHasMatureSpare());
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtHasMatureSpare());
    CHECK(isaacFboRtRecoveryTake() == 0); /* A witness is not a Begin. */
    isaacFboRtRecoveryBegin();
    CHECK(isaacFboRtRecoveryTake() == a);
    destroy(a);
    CHECK(!isaacFboRtHasMatureSpare());
    CHECK(isaacFboRtRecoveryTake() == 0);
    isaacFboRtRecoveryEnd();
    finish_active(b);
}

static void reuse_cannot_inherit_witness(int same_handle_again)
{
    uintptr_t a, b, out;
    begin_pair(&a, &b, 0);
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtSwitch(OWNER, b, 784, 472, 480, 272, 1, 2, &out));
    CHECK(out == a && retirement_epoch[a] == 1);
    uintptr_t retiring = b, current = a;
    int bucket = 2;
    if (same_handle_again) {
        CHECK(isaacFboRtSwitch(OWNER, a, 480, 272, 784, 472, 1, 3, &out));
        CHECK(out == b && retirement_epoch[a] == 2);
        retiring = a; current = b; bucket = 3;
    }
    CHECK(!isaacFboRtHasMatureSpare());
    isaacFboRtRecoveryBegin();
    CHECK(isaacFboRtRecoveryTake() == 0);
    CHECK(isaacFboRtCollect(1) == 0);
    CHECK(isaacFboRtCollect(bucket) == retiring);
    destroy(retiring);
    isaacFboRtRecoveryEnd();
    finish_active(current);
}

static void overflow_never_grants_deadline(void)
{
    uintptr_t a, b, out = 999;
    begin_pair(&a, &b, 2);
    rt_reuse.recovery_depth = UINT32_MAX; /* Fault injection, not a witness. */
    isaacFboRtRecoveryBegin();
    CHECK(rt_reuse.recovery_poison && rt_reuse.recovery_depth == UINT32_MAX);
    CHECK(isaacFboRtRecoveryTake() == 0);
    CHECK(isaacFboRtCollect(1) == 0);
    CHECK(isaacFboRtCollect(2) == a);
    destroy(a);
    isaacFboRtRecoveryEnd();
    CHECK(rt_reuse.recovery_depth == UINT32_MAX - 1);
    isaacFboRtCreated(OWNER, b, 784, 472, 1);
    CHECK(!isaacFboRtSwitch(OWNER, b, 784, 472, 480, 272, 1, 0, &out));
    destroy(b);
    CHECK(!isaacFboRtHasSpare());
    /* All handles are released; isolate the next independent poison case. */
    memset(&rt_reuse, 0, sizeof(rt_reuse));
}

#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
static void lease_event_profile(int saturated)
{
    IsaacFboRtReuseStats rr;
    vglIsaacFboRtReuseProfileTake(&rr);
    if (saturated) {
        rt_profile.lease_grants = UINT32_MAX;
        rt_profile.lease_hits = UINT32_MAX;
        rt_profile.lease_drains = UINT32_MAX;
    }
    uintptr_t a, b, out;
    begin_pair(&a, &b, 0);
    CHECK(isaacFboRtCollect(1) == 0);
    CHECK(isaacFboRtCollect(0) == 0);
    CHECK(isaacFboRtCollect(0) == 0); /* Repeated visit is not another grant. */
    if (!saturated) {
        vglIsaacFboRtReuseProfileTake(NULL);
        vglIsaacFboRtReuseProfileTake(&rr);
        CHECK(rr.lease_grants == 1 && !rr.lease_hits && !rr.lease_drains);
        CHECK(isaacFboRtHasMatureSpare()); /* Snapshot doesn't reset lifetime. */
    }
    CHECK(isaacFboRtSwitch(OWNER, b, 784, 472, 480, 272, 1, 2, &out));
    CHECK(out == a);
    if (!saturated) {
        vglIsaacFboRtReuseProfileTake(&rr);
        CHECK(rr.hits == 1 && rr.lease_hits == 1 && !rr.lease_grants && !rr.lease_drains);
    }
    CHECK(isaacFboRtCollect(2) == 0);
    if (!saturated) vglIsaacFboRtReuseProfileTake(&rr);
    isaacFboRtRecoveryBegin();
    CHECK(isaacFboRtRecoveryTake() == b);
    CHECK(isaacFboRtRecoveryTake() == 0);
    destroy(b);
    isaacFboRtRecoveryEnd();
    vglIsaacFboRtReuseProfileTake(&rr);
    if (saturated) {
        CHECK(rr.lease_grants == UINT32_MAX && rr.lease_hits == UINT32_MAX && rr.lease_drains == UINT32_MAX);
        CHECK(rr.saturation_mask == UINT32_C(0x7000));
    } else CHECK(rr.lease_drains == 1 && !rr.lease_grants && !rr.lease_hits);
    vglIsaacFboRtReuseProfileTake(&rr);
    CHECK(!rr.lease_grants && !rr.lease_hits && !rr.lease_drains && !rr.saturation_mask);
    finish_active(a);
}
#endif

static void unmatched_end_fails_closed(void)
{
    uintptr_t a = fresh(), out = 999;
    isaacFboRtRecoveryEnd();
    isaacFboRtCreated(OWNER, a, 480, 272, 1);
    CHECK(!isaacFboRtSwitch(OWNER, a, 480, 272, 784, 472, 1, 0, &out));
    isaacFboRtRecoveryBegin();
    isaacFboRtRecoveryEnd();
    isaacFboRtCreated(OWNER, a, 480, 272, 1);
    CHECK(!isaacFboRtSwitch(OWNER, a, 480, 272, 784, 472, 1, 0, &out));
    destroy(a);
}

int main(void)
{
    bounded_rotation();
    invalidation_lower_bound(0);
    invalidation_lower_bound(1);
    reuse_after_original_deadline();
    unknown_sequence_expires();
    no_active_no_extension();
    nested_pressure();
    recovery_with_original_collection();
    pressure_after_deadline();
    reuse_cannot_inherit_witness(0);
    reuse_cannot_inherit_witness(1);
#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
    lease_event_profile(0);
    lease_event_profile(1);
#endif
    overflow_never_grants_deadline();
    unmatched_end_fails_closed();
    for (unsigned i = 1; i < next_handle; ++i)
        CHECK(destroyed[i] == 1);
    printf("vita_fbo_rt_lease_test: PASS (%u checks)\n", checks);
    return 0;
}
