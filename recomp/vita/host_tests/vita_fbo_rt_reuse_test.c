#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../vitagl-stock-reference/isaac_fbo_rt_reuse.h"

/* Opt-in white-box build compiles this one TU, not a second policy object.
 * It seeds overflow/defensive-guard states without a production test API. */
#ifdef ISAAC_FBO_RT_REUSE_PROFILE_TEST
#ifndef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
#error "The white-box profile fixture requires the native resource profile"
#endif
#include "../vitagl-stock-reference/isaac_fbo_rt_reuse.c"
#endif

/* Exercise the production policy through its public boundary. A returned
 * expired handle has one native destroy owner; current handles stay owned by
 * the framebuffer and are deliberately never destroyed by this policy. */
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
#define OWNER1 ((uintptr_t)0x1110)
#define OWNER2 ((uintptr_t)0x2220)

static unsigned checks, destroyed[256];
static uintptr_t fresh_handle = 1;

#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
static unsigned profile_checks;
#define PROFILE_CHECK(x) do { CHECK(x); ++profile_checks; } while (0)
#define PROFILE_FIELDS(X) \
    X(switch_calls, 0) X(hits, 1) X(first_misses, 2) \
    X(expired_reusable, 3) X(expired_revoked, 4) \
    X(explicit_invalidations, 5) X(reject_owner, 6) X(reject_size, 7) \
    X(reject_policy, 8) X(reject_current, 9) X(reject_args, 10) \
    X(reject_spare, 11) \
    X(lease_grants, 12) X(lease_hits, 13) X(lease_drains, 14)

static void expect_profile(IsaacFboRtReuseStats expected)
{
    IsaacFboRtReuseStats actual, empty;
    vglIsaacFboRtReuseProfileTake(NULL); /* Must not discard the window. */
    vglIsaacFboRtReuseProfileTake(&actual);
#define EXPECT_FIELD(field, bit) PROFILE_CHECK(actual.field == expected.field);
    PROFILE_FIELDS(EXPECT_FIELD)
#undef EXPECT_FIELD
    PROFILE_CHECK(actual.saturation_mask == expected.saturation_mask);
    if (!actual.saturation_mask && actual.switch_calls != UINT32_MAX) {
        PROFILE_CHECK(actual.switch_calls == actual.hits + actual.first_misses +
            actual.reject_owner + actual.reject_size + actual.reject_policy +
            actual.reject_current + actual.reject_args + actual.reject_spare);
    }
    vglIsaacFboRtReuseProfileTake(&empty);
#define EXPECT_ZERO(field, bit) PROFILE_CHECK(empty.field == 0);
    PROFILE_FIELDS(EXPECT_ZERO)
#undef EXPECT_ZERO
    PROFILE_CHECK(empty.saturation_mask == 0);
}
#define EXPECT_PROFILE(...) expect_profile((IsaacFboRtReuseStats){__VA_ARGS__})
#else
#define EXPECT_PROFILE(...) ((void)0)
#endif

static uintptr_t fresh(void)
{
    CHECK(fresh_handle < 256);
    return fresh_handle++;
}

static void destroy_once(uintptr_t handle)
{
    CHECK(handle > 0 && handle < fresh_handle);
    CHECK(!destroyed[handle]);
    destroyed[handle] = 1;
    ++checks;
}

static void collect(int bucket, uintptr_t expected)
{
    uintptr_t got = isaacFboRtCollect(bucket);
    CHECK(got == expected);
    if (got) destroy_once(got);
    CHECK(isaacFboRtCollect(bucket) == 0);
    ++checks;
}

static void empty_policy(void)
{
    int bucket;
    isaacFboRtInvalidate(OWNER1);
    isaacFboRtInvalidate(OWNER2);
    for (bucket = 0; bucket != 8; ++bucket) collect(bucket, 0);
}

static void swap_round_trip(void)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 1, &next));
    CHECK(next == 0); /* First miss holds A exclusively, not a queue entry. */
    collect(0, 0);
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    CHECK(isaacFboRtSwitch(OWNER1, b, 784, 472, 480, 272, 1, 2, &next));
    CHECK(next == a && next != b);
    collect(1, 0); /* Reattached A cancelled its old retirement deadline. */
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 3, &next));
    CHECK(next == b && next != a);
    collect(2, 0); /* Reattached B likewise is no longer a spare. */
    collect(3, a);
    isaacFboRtInvalidate(OWNER1);
    destroy_once(b); /* Native framebuffer destruction owns current B. */
    empty_policy();
}

static void expiry_and_forced_gc(void)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    int bucket;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 3, &next));
    CHECK(!next);
    /* The API sees cleanup buckets, not frame timestamps. Several forced GC
     * calls within one outer frame must expire on the same original bucket. */
    for (bucket = 0; bucket < 3; ++bucket) collect(bucket, 0);
    collect(3, a);
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    CHECK(isaacFboRtSwitch(OWNER1, b, 784, 472, 480, 272, 1, 0, &next));
    CHECK(!next); /* Expired A is never recovered from the native purge. */
    collect(1, 0);
    collect(2, 0);
    collect(3, 0);
    collect(0, b); /* Wraparound does not extend the recorded deadline. */
    empty_policy();
}

static void repeated_swaps_in_one_bucket(void)
{
    uintptr_t a = fresh(), b = fresh(), current = b, next = 999;
    int i, width = 784, height = 472;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 0, &next));
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    for (i = 0; i != 100; ++i) {
        int new_width = width == 784 ? 480 : 784;
        int new_height = height == 472 ? 272 : 472;
        CHECK(isaacFboRtSwitch(OWNER1, current, width, height,
                              new_width, new_height, 1, 0, &next));
        CHECK(next == (current == a ? b : a));
        CHECK(next != current && !destroyed[next]);
        current = next;
        width = new_width;
        height = new_height;
        ++checks;
    }
    /* Only the non-active handle can expire, even after repeated exchanges. */
    collect(0, current == a ? b : a);
    isaacFboRtInvalidate(OWNER1);
    destroy_once(current);
    empty_policy();
}

static void resize_without_intervening_draw(void)
{
    uintptr_t a = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 1, &next));
    CHECK(next == 0);
    next = 999;
    CHECK(!isaacFboRtSwitch(OWNER1, 0, 784, 472, 480, 272, 1, 3, &next));
    CHECK(next == 999);
    collect(0, 0);
    collect(1, a);
    empty_policy();
}

static void one_owner_and_recycled_address(void)
{
    uintptr_t a = fresh(), b = fresh(), foreign = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 2, &next));
    isaacFboRtCreated(OWNER2, foreign, 480, 272, 1);
    next = 999;
    CHECK(!isaacFboRtSwitch(OWNER2, foreign, 480, 272, 784, 472, 1, 3, &next));
    CHECK(next == 999);
    isaacFboRtInvalidate(OWNER2); /* Foreign invalidation cannot touch A. */
    collect(1, 0);
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    CHECK(isaacFboRtSwitch(OWNER1, b, 784, 472, 480, 272, 1, 3, &next));
    CHECK(next == a);
    isaacFboRtInvalidate(OWNER1); /* Delete/detach/texture-delete contract. */
    destroy_once(a);
    /* A new FBO may reuse the same native address, but cannot adopt B. */
    isaacFboRtCreated(OWNER1, foreign, 480, 272, 1);
    next = 999;
    CHECK(!isaacFboRtSwitch(OWNER1, foreign, 480, 272, 784, 472, 1, 0, &next));
    CHECK(next == 999);
    collect(2, 0);
    collect(3, b); /* Invalidation did not requeue B in bucket 0. */
    isaacFboRtCreated(OWNER1, foreign, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, foreign, 480, 272, 784, 472, 1, 1, &next));
    CHECK(next == 0);
    collect(1, foreign);
    empty_policy();
}

static void switch_rejects(int reason)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    int old_w = 784, old_h = 472, new_w = 480, new_h = 272;
    int compatible = 1, bucket = 3;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 1, &next));
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    next = 999;
    switch (reason) {
    case 0: new_w = 1024; new_h = 1024; break;
    case 1: compatible = 0; break; /* MSAA/scene-policy escape. */
    case 2: old_w = 480; old_h = 272; break; /* Stale dimensions. */
    case 3: new_w = 784; new_h = 472; break; /* No resize. */
    case 4: bucket = -1; break;
    default: break;
    }
    CHECK(!isaacFboRtSwitch(OWNER1, b, old_w, old_h, new_w, new_h,
                            compatible, bucket, reason == 5 ? NULL : &next));
    CHECK(next == 999);
    collect(0, 0);
    collect(1, a); /* Rejection cannot extend already-held A's deadline. */
    destroy_once(b); /* Caller uses the ordinary retirement path. */
    empty_policy();
}

static void failed_or_incompatible_creation(int reason)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 2, &next));
    /* Failed first creation is reported before the native one-scene fallback;
     * that fallback must not gain eligible metadata. */
    isaacFboRtCreated(OWNER1, reason == 0 ? 0 : b,
                      reason == 2 ? 1024 : 784,
                      reason == 2 ? 1024 : 472, reason == 1 ? 0 : 1);
    next = 999;
    CHECK(!isaacFboRtSwitch(OWNER1, b, 784, 472, 480, 272, 1, 3, &next));
    CHECK(next == 999);
    collect(1, 0);
    collect(2, a);
    destroy_once(b);
    empty_policy();
}

#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
static void snapshot_preserves_lifetime(void)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 1, &next));
    EXPECT_PROFILE(.switch_calls = 1, .first_misses = 1);
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    CHECK(isaacFboRtSwitch(OWNER1, b, 784, 472, 480, 272, 1, 3, &next));
    CHECK(next == a);
    EXPECT_PROFILE(.switch_calls = 1, .hits = 1);
    collect(1, 0);
    isaacFboRtInvalidate(OWNER2);
    isaacFboRtInvalidate(0);
    isaacFboRtInvalidate(OWNER1);
    isaacFboRtInvalidate(OWNER1); /* Repeated invalidation is not an event. */
    EXPECT_PROFILE(.explicit_invalidations = 1);
    collect(2, 0);
    collect(3, b); /* All snapshots preserved B's original bucket. */
    EXPECT_PROFILE(.expired_revoked = 1);
    destroy_once(a);
    empty_policy();
    EXPECT_PROFILE(0);
}

static void reject_precedence_and_current_handle(void)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    /* Multiple bad inputs still produce exactly one documented reason. */
    CHECK(!isaacFboRtSwitch(0, 0, 1, 1, 1, 1, 0, -1, NULL));
    CHECK(!isaacFboRtSwitch(OWNER2, 0, 1, 1, 1, 1, 0, 0, &next));
    CHECK(!isaacFboRtSwitch(OWNER2, 0, 1, 1, 1, 1, 1, 0, &next));
    CHECK(!isaacFboRtSwitch(OWNER1, b, 480, 272, 1, 1, 1, 0, &next));
    CHECK(next == 999);
    destroy_once(a);
    destroy_once(b);
    empty_policy();
}
#endif

#ifdef ISAAC_FBO_RT_REUSE_PROFILE_TEST
static void defensive_spare_rejects(int reason)
{
    uintptr_t a = fresh(), b = fresh(), next = 999;
    isaacFboRtCreated(OWNER1, a, 480, 272, 1);
    CHECK(isaacFboRtSwitch(OWNER1, a, 480, 272, 784, 472, 1, 2, &next));
    isaacFboRtCreated(OWNER1, b, 784, 472, 1);
    /* These unreachable-through-valid-hooks states exercise the final safety
     * guard and its reason bit, not additional permitted ownership patterns. */
    if (reason == 0) rt_reuse.spare_reusable = 0;
    if (reason == 1) rt_reuse.spare.w = 1024;
    if (reason == 2) rt_reuse.spare.target = b;
    next = 999;
    CHECK(!isaacFboRtSwitch(OWNER1, b, 784, 472, 480, 272, 1, 3, &next));
    CHECK(next == 999);
    if (reason == 2) rt_reuse.spare.target = a; /* Undo fixture corruption. */
    collect(1, 0);
    collect(2, a);
    destroy_once(b);
    empty_policy();
}

static void profile_saturation(void)
{
    int reason;
    /* Check the actual saturating primitive immediately around the boundary. */
#define SATURATION_BOUNDARY(field, bit) do { \
    rt_profile = (IsaacFboRtReuseStats){0}; \
    rt_profile.field = UINT32_MAX - 1; \
    profile_increment(&rt_profile.field, bit); \
    PROFILE_CHECK(rt_profile.field == UINT32_MAX); \
    PROFILE_CHECK(rt_profile.saturation_mask == 0); \
    profile_increment(&rt_profile.field, bit); \
    profile_increment(&rt_profile.field, bit); \
    PROFILE_CHECK(rt_profile.field == UINT32_MAX); \
    PROFILE_CHECK(rt_profile.saturation_mask == (UINT32_C(1) << bit)); \
} while (0);
    PROFILE_FIELDS(SATURATION_BOUNDARY)
#undef SATURATION_BOUNDARY
    rt_profile = (IsaacFboRtReuseStats){0};
#define SEED_MAX(field, bit) rt_profile.field = UINT32_MAX;
    PROFILE_FIELDS(SEED_MAX)
#undef SEED_MAX
    /* Exercise every real event site at saturation. The same lifetime tests
     * must still pass; no overflow may alter a handle, reject, or deadline. */
    swap_round_trip();
    resize_without_intervening_draw();
    one_owner_and_recycled_address();
    for (reason = 0; reason != 6; ++reason) switch_rejects(reason);
    defensive_spare_rejects(0);
#define EXPECT_MAX(field, bit) .field = UINT32_MAX,
    expect_profile((IsaacFboRtReuseStats){
        PROFILE_FIELDS(EXPECT_MAX) .saturation_mask = UINT32_C(0xfff)
    });
#undef EXPECT_MAX
}
#endif

int main(void)
{
    int reason;
    empty_policy();
    swap_round_trip();
    EXPECT_PROFILE(.switch_calls = 3, .hits = 2, .first_misses = 1,
                   .expired_reusable = 1, .explicit_invalidations = 1);
    expiry_and_forced_gc();
    EXPECT_PROFILE(.switch_calls = 2, .first_misses = 2,
                   .expired_reusable = 2, .explicit_invalidations = 1);
    repeated_swaps_in_one_bucket();
    EXPECT_PROFILE(.switch_calls = 101, .hits = 100, .first_misses = 1,
                   .expired_reusable = 1, .explicit_invalidations = 1);
    resize_without_intervening_draw();
    EXPECT_PROFILE(.switch_calls = 2, .first_misses = 1,
                   .reject_current = 1, .expired_revoked = 1);
    one_owner_and_recycled_address();
    EXPECT_PROFILE(.switch_calls = 5, .hits = 1, .first_misses = 2,
                   .reject_owner = 2, .expired_reusable = 1,
                   .expired_revoked = 1, .explicit_invalidations = 2);
    for (reason = 0; reason != 6; ++reason) {
        switch_rejects(reason);
        EXPECT_PROFILE(.switch_calls = 2, .first_misses = 1,
                       .expired_revoked = 1, .reject_size = reason == 0 || reason == 3,
                       .reject_policy = reason == 1, .reject_current = reason == 2,
                       .reject_args = reason >= 4);
    }
    for (reason = 0; reason != 3; ++reason) {
        failed_or_incompatible_creation(reason);
        EXPECT_PROFILE(.switch_calls = 2, .first_misses = 1,
                       .reject_owner = 1, .expired_revoked = 1);
    }
    printf("vita_fbo_rt_reuse_test: PASS (%u ownership/collection checks)\n", checks);
#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
    PROFILE_CHECK(ISAAC_FBO_RT_REUSE_PROFILE_ABI == 2);
    snapshot_preserves_lifetime();
    reject_precedence_and_current_handle();
    EXPECT_PROFILE(.switch_calls = 4, .reject_args = 1, .reject_policy = 1,
                   .reject_owner = 1, .reject_current = 1);
#ifdef ISAAC_FBO_RT_REUSE_PROFILE_TEST
    for (reason = 0; reason != 3; ++reason) {
        defensive_spare_rejects(reason);
        EXPECT_PROFILE(.switch_calls = 2, .first_misses = 1,
                       .reject_spare = 1, .expired_revoked = 1);
    }
    profile_saturation();
#endif
    printf("vita_fbo_rt_reuse_profile: PASS (%u profile checks, "
           "%u total ownership/collection checks)\n", profile_checks, checks);
#endif
    return 0;
}
