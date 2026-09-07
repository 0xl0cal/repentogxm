/* Native pressure/create bodies are freshly extracted by test_vitagl_rt_lease.py.
 * Boundaries below model bucket order, not SDK/OOM or EndScene safety. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../vitagl-stock-reference/isaac_fbo_rt_reuse.h"

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
#define HAVE_ISAAC_FBO_RT_REUSE 1
#define HAVE_ISAAC_FBO_RT_SCENES 1
#define FRAME_PURGE_FREQ 4
#define SCE_GXM_MULTISAMPLE_NONE 0
#define ISAAC_FBO_RT_SCENES() gxm_fbo_rt_size
#if defined(HAVE_ISAAC_FBO_RT_REUSE_LEASE) && HAVE_ISAAC_FBO_RT_REUSE_LEASE
#define LEASE_ON 1
#else
#define LEASE_ON 0
#endif
typedef struct SceGxmRenderTarget { unsigned id; } SceGxmRenderTarget;
typedef struct framebuffer { SceGxmRenderTarget *target; int width, height; } framebuffer;
static unsigned checks, finishes, destroys, collections, retries, scene_count;
static unsigned scene_history[4], destroyed[256], next_target = 1;
static unsigned retired[256], deadline_seen[256], epoch[256];
static int original_bucket[256], clean_idx, mark_idx;
static int fail_inner, fail_create, create_with_pressure, nested_pressure;
static int native_retry_pending, finish_collects, double_take;
static SceGxmRenderTarget targets[256], *held, *active, *open_scene;
static framebuffer fb;
static framebuffer *active_write_fb = &fb;
static void *gxm_context = (void *)(uintptr_t)0x1234;
static int gxm_fbo_rt_size = 8, msaa_mode = SCE_GXM_MULTISAMPLE_NONE;
static void glFinish(void);
static void sceGxmFinish(void *context);
static int sceGxmDestroyRenderTarget(SceGxmRenderTarget *target);
static int garbage_collector(unsigned args, void *arg);
static void *gpu_alloc_mapped_aligned_for_cpu_inner(size_t alignment, size_t size);
static void *gpu_alloc_mapped_aligned_for_gpu_inner(size_t alignment, size_t size);
static int setup_render_target(SceGxmRenderTarget **target, int w, int h, int scenes);
#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
static unsigned retirements;
static void isaacNativeResourceRetireRT(SceGxmRenderTarget *target) {
    CHECK(target == held);
    ++retirements;
}
#endif
#include "native_rt_lease.h"

static SceGxmRenderTarget *fresh(void) {
    CHECK(next_target < 256);
    targets[next_target].id = next_target;
    return &targets[next_target++];
}

static int switch_target(SceGxmRenderTarget *current, int ow, int oh,
                         int nw, int nh, SceGxmRenderTarget **next) {
    uintptr_t out = 99;
    int ok = isaacFboRtSwitch((uintptr_t)&fb, (uintptr_t)current,
                             ow, oh, nw, nh, 1, mark_idx, &out);
    if (ok) {
        *next = (SceGxmRenderTarget *)out;
        if (*next) retired[(*next)->id] = 0;
        held = current;
        retired[current->id] = 1;
        deadline_seen[current->id] = 0;
        original_bucket[current->id] = mark_idx;
        ++epoch[current->id];
    }
    return ok;
}

static void reset_counts(void) {
    finishes = destroys = collections = retries = scene_count = 0;
    native_retry_pending = fail_inner = fail_create = create_with_pressure = 0;
    nested_pressure = finish_collects = double_take = 0;
    unsafe_allocator_counter = 0;
}

static void pair(void) {
    CHECK(!held);
    reset_counts();
    mark_idx = 0; clean_idx = 1;
    SceGxmRenderTarget *a = fresh(), *b = fresh(), *out = NULL;
    isaacFboRtCreated((uintptr_t)&fb, (uintptr_t)a, 480, 272, 1);
    open_scene = a; /* Attachment changes fb->target, not this scene identity. */
    CHECK(switch_target(a, 480, 272, 784, 472, &out));
    CHECK(!out);
    isaacFboRtCreated((uintptr_t)&fb, (uintptr_t)b, 784, 472, 1);
    active = b;
    fb = (framebuffer){b, 784, 472};
    CHECK(held == a && !deadline_seen[a->id]);
}

static void sceGxmFinish(void *context) {
    CHECK(context == gxm_context);
    ++finishes; /* Deliberately does NOT clear open_scene. */
    if (finish_collects) {
        finish_collects = 0; /* Adversarial reentry, not a claim about SDK callbacks. */
        garbage_collector(0, NULL);
    }
}

static int sceGxmDestroyRenderTarget(SceGxmRenderTarget *target) {
    CHECK(target == held && target != active && !destroyed[target->id]);
    CHECK(retired[target->id] && epoch[target->id] && deadline_seen[target->id]);
    /* Permission is the independent current-retirement Collect witness, NOT
     * finishes>0 or a mutable framebuffer target. Original GC safety is not proved. */
#if LEASE_ON
    CHECK(!isaacFboRtHasSpare());
    if (double_take) CHECK(isaacFboRtRecoveryTake() == 0);
#endif
    destroyed[target->id] = 1;
    retired[target->id] = 0;
    held = NULL;
    ++destroys;
    return 0;
}

static int garbage_collector(unsigned args, void *arg) {
    CHECK(args == 0 && arg == NULL);
    /* Native order: ordinary bucket objects, policy Collect, destroy, then
     * advance BOTH native indices. Ordinary bucket queues are empty here. */
    if (held && original_bucket[held->id] == clean_idx)
        deadline_seen[held->id] = 1;
    uintptr_t target = isaacFboRtCollect(clean_idx);
    if (target) {
#ifdef HAVE_ISAAC_NATIVE_RESOURCE_PROFILE
        isaacNativeResourceRetireRT((SceGxmRenderTarget *)target);
#endif
        sceGxmDestroyRenderTarget((SceGxmRenderTarget *)target);
    }
    clean_idx = (clean_idx + 1) % FRAME_PURGE_FREQ;
    mark_idx = (mark_idx + 1) % FRAME_PURGE_FREQ;
    ++collections;
    return 0;
}

static void *inner(size_t alignment, size_t size) {
    CHECK(alignment == 16 && size == 128 && finishes > 0);
    ++retries;
    return fail_inner ? NULL : (void *)(uintptr_t)0x8000;
}
static void *gpu_alloc_mapped_aligned_for_cpu_inner(size_t a, size_t s) { return inner(a, s); }
static void *gpu_alloc_mapped_aligned_for_gpu_inner(size_t a, size_t s) { return inner(a, s); }

static void glFinish(void) {
    open_scene = NULL; /* Model CPU scene reset; never used as destroy permission. */
    if (nested_pressure) {
        nested_pressure = 0;
        unsafe_allocator_counter = 0; /* Actual full allocation wrapper behavior. */
        CHECK(gpu_alloc_mapped_aligned_unsafe_for_gpu(16, 128) != NULL);
#if LEASE_ON
        uintptr_t out = 99;
        isaacFboRtCreated((uintptr_t)&fb, (uintptr_t)active, 784, 472, 1);
        CHECK(!isaacFboRtSwitch((uintptr_t)&fb, (uintptr_t)active,
                               784, 472, 480, 272, 1, mark_idx, &out));
        CHECK(out == 99); /* Nested End cannot reopen outer admission. */
#endif
    }
    sceGxmFinish(gxm_context);
}

static int setup_render_target(SceGxmRenderTarget **target, int w, int h, int scenes) {
    CHECK(w == 784 && h == 472 && scene_count < 4);
    scene_history[scene_count++] = (unsigned)scenes;
    if (native_retry_pending) CHECK(*target == NULL);
    if (create_with_pressure) {
        create_with_pressure = 0;
        unsafe_allocator_counter = 0;
        CHECK(gpu_alloc_mapped_aligned_unsafe_for_gpu(16, 128) != NULL);
    }
    if (fail_create && scenes == 8) {
        native_retry_pending = 1;
        *target = NULL;
        return -1;
    }
    *target = fresh();
    return 0;
}

static void cleanup(void) {
    isaacFboRtInvalidate((uintptr_t)&fb);
    for (int i = 0; held && i < 8; ++i) garbage_collector(0, NULL);
    CHECK(!held);
    open_scene = NULL;
}

static void allocation(int cpu, int failure, int nested, int nested_deadline) {
    pair();
    SceGxmRenderTarget *original = held;
    if (nested_deadline) {
        for (int i = 0; i < 3; ++i) garbage_collector(0, NULL);
        CHECK(clean_idx == 0 && held == original && !deadline_seen[original->id]);
        reset_counts();
    }
    fail_inner = failure; nested_pressure = nested;
    void *result = cpu ? gpu_alloc_mapped_aligned_unsafe_for_cpu(16, 128)
                       : gpu_alloc_mapped_aligned_unsafe_for_gpu(16, 128);
    CHECK((result == NULL) == failure);
    CHECK(finishes == (unsigned)(nested ? 2 : 1));
    CHECK(retries == (unsigned)(failure ? 4 : nested ? 2 : 1));
    CHECK(collections == retries);
    CHECK(destroys == (unsigned)(failure || nested_deadline));
    if (!failure && !nested_deadline) {
        CHECK(held == original && !deadline_seen[original->id]);
        if (!cpu) CHECK(open_scene == original && !destroyed[original->id]);
    }
#if LEASE_ON
    CHECK(isaacFboRtRecoveryTake() == 0);
#endif
    cleanup();
}

#if LEASE_ON
static void mature_pair(void) {
    pair();
    SceGxmRenderTarget *original = held;
    for (int i = 0; i < 4; ++i) garbage_collector(0, NULL);
    CHECK(held == original && deadline_seen[original->id]);
    CHECK(isaacFboRtHasMatureSpare());
    reset_counts();
}
static void mature_allocation(int cpu, int nested_collect) {
    mature_pair();
    finish_collects = nested_collect;
    double_take = 1;
    CHECK((cpu ? gpu_alloc_mapped_aligned_unsafe_for_cpu(16, 128)
               : gpu_alloc_mapped_aligned_unsafe_for_gpu(16, 128)) != NULL);
    CHECK(destroys == 1 && finishes == 1 && retries == 1);
    CHECK(collections == (unsigned)(1 + nested_collect));
    CHECK(!held && isaacFboRtRecoveryTake() == 0);
    cleanup();
}
static void new_retirement_pressure(int same_pointer_again) {
    mature_pair();
    SceGxmRenderTarget *a = held, *b = active, *out = NULL;
    CHECK(switch_target(b, 784, 472, 480, 272, &out) && out == a);
    active = out; open_scene = b;
    if (same_pointer_again) {
        CHECK(switch_target(a, 480, 272, 784, 472, &out) && out == b);
        active = out; open_scene = a;
        CHECK(epoch[a->id] == 2);
    }
    SceGxmRenderTarget *new_spare = held;
    CHECK(!deadline_seen[new_spare->id] && !isaacFboRtHasMatureSpare());
    reset_counts();
    CHECK(gpu_alloc_mapped_aligned_unsafe_for_gpu(16, 128) != NULL);
    CHECK(held == new_spare && destroys == 0 && open_scene == new_spare);
    cleanup();
}
#endif

static void create(int with_spare, int mature, int pressure, int failure, int reenter) {
#if LEASE_ON
    if (mature) mature_pair(); else pair();
#else
    CHECK(!mature); pair();
#endif
    if (!with_spare) { cleanup(); reset_counts(); }
    fail_create = failure; create_with_pressure = pressure; finish_collects = reenter;
    fb.target = NULL;
    CHECK(native_create() == 0);
    CHECK(scene_count == (unsigned)(failure ? 2 : 1) && scene_history[0] == 8);
    if (failure) CHECK(scene_history[1] == 1);
    unsigned expected_finish = (unsigned)(pressure || (LEASE_ON && mature && with_spare && failure));
    CHECK(finishes == expected_finish);
    CHECK(destroys == (unsigned)(LEASE_ON && mature && with_spare && (failure || pressure)));
    CHECK(collections == (unsigned)(pressure + reenter));
    CHECK(retries == (unsigned)pressure);
    if (with_spare && !mature) CHECK(held && !deadline_seen[held->id]);
    if (failure) {
        uintptr_t out = 99;
        CHECK(!isaacFboRtSwitch((uintptr_t)&fb, (uintptr_t)fb.target,
                               784, 472, 480, 272, 1, mark_idx, &out));
    }
    cleanup();
}

int main(void) {
    allocation(0, 0, 0, 0); allocation(1, 0, 0, 0);
    allocation(0, 1, 0, 0); allocation(1, 1, 0, 0);
    allocation(1, 0, 1, 0); allocation(1, 0, 1, 1);
    create(1, 0, 0, 1, 0); create(0, 0, 0, 1, 0);
    create(1, 0, 1, 0, 0); create(1, 0, 1, 1, 0);
#if LEASE_ON
    mature_allocation(0, 0); mature_allocation(1, 0); mature_allocation(1, 1);
    new_retirement_pressure(0); new_retirement_pressure(1);
    create(1, 1, 0, 1, 0); create(1, 1, 0, 1, 1);
    create(1, 1, 1, 0, 0); create(1, 1, 1, 1, 0);
#endif
    printf("native RT retirement deadline/pressure/fallback: %u checks PASS\n", checks);
    return 0;
}
