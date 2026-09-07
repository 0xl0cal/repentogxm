/* Optional aggregate timing, not a resource cache or GPU synchronization policy.
 * The stock single-owner GL contract and SINGLE_THREADED_GC are required. */
#include "shared.h"
#undef sceGxmFinish
#undef sceGxmCreateRenderTarget
#undef sceGxmDestroyRenderTarget

#define ISAAC_NR_RECORDS 128U
typedef struct {
    void *pointer;
    uint32_t width, height, kind;
} IsaacNativeResourceIdentity;
static IsaacNativeResourceStats nr;
static IsaacNativeResourceIdentity identities[ISAAC_NR_RECORDS];
static uint32_t scene_width, scene_height, scene_known;

static void nr_add(uint32_t *value, uint32_t increment)
{
    if (increment > UINT32_MAX - *value) {
        *value = UINT32_MAX;
        if (nr.saturation != UINT32_MAX)
            ++nr.saturation;
    } else {
        *value += increment;
    }
}

static uint32_t nr_time(uint64_t elapsed)
{
    if (elapsed > UINT32_MAX) {
        nr_add(&nr.clock_clamp, 1U);
        return UINT32_MAX;
    }
    return (uint32_t)elapsed;
}

static void nr_metric(IsaacNativeResourceMetric *metric, uint32_t us, int failed)
{
    nr_add(&metric->count, 1U);
    nr_add(&metric->us, us);
    if (us > metric->max_us)
        metric->max_us = us;
    if (failed)
        nr_add(&metric->failed, 1U);
}

static IsaacNativeResourceSize *nr_size(uint32_t w, uint32_t h)
{
    uint32_t i;
    for (i = 0; i < nr.sizes_used; ++i)
        if (nr.size[i].width == w && nr.size[i].height == h)
            return &nr.size[i];
    if (nr.sizes_used == ISAAC_NATIVE_RESOURCE_SIZE_SLOTS) {
        nr_add(&nr.size_overflow, 1U);
        return NULL;
    }
    i = nr.sizes_used++;
    nr.size[i].width = w;
    nr.size[i].height = h;
    return &nr.size[i];
}

static IsaacNativeResourceIdentity *nr_find(void *pointer, uint32_t kind)
{
    uint32_t i;
    if (!pointer)
        return NULL;
    for (i = 0; i < ISAAC_NR_RECORDS; ++i)
        if (identities[i].pointer == pointer && identities[i].kind == kind)
            return &identities[i];
    return NULL;
}

static void nr_register(void *pointer, uint32_t kind, uint32_t w, uint32_t h)
{
    uint32_t i;
    IsaacNativeResourceIdentity *record;
    if (!pointer)
        return;
    record = nr_find(pointer, kind);
    if (!record) {
        for (i = 0; i < ISAAC_NR_RECORDS; ++i) {
            if (!identities[i].pointer) {
                record = &identities[i];
                break;
            }
        }
    }
    if (!record) {
        nr_add(&nr.registry_overflow, 1U);
        return;
    }
    record->pointer = pointer;
    record->kind = kind;
    record->width = w;
    record->height = h;
}

void isaacNativeResourceEvent(unsigned kind, uint32_t w, uint32_t h,
                              uint64_t elapsed, int failed)
{
    uint32_t us = nr_time(elapsed);
    IsaacNativeResourceSize *size;
    if (kind >= ISAAC_NR_METRIC_COUNT)
        return;
    nr_metric(&nr.metric[kind], us, failed);
    if (kind != ISAAC_NR_RT_CREATE && kind != ISAAC_NR_RT_DESTROY &&
            kind != ISAAC_NR_DEPTH_CREATE)
        return;
    size = nr_size(w, h);
    if (!size)
        return;
    if (kind == ISAAC_NR_RT_CREATE)
        nr_metric(&size->rt_create, us, failed);
    else if (kind == ISAAC_NR_RT_DESTROY)
        nr_metric(&size->rt_destroy, us, failed);
    else
        nr_metric(&size->depth_create, us, failed);
}

void isaacNativeResourceSceneBegin(uint32_t w, uint32_t h, int result)
{
    scene_known = result == 0;
    scene_width = w;
    scene_height = h;
}

void isaacNativeResourceSceneEnd(uint32_t elapsed)
{
    IsaacNativeResourceSize *size;
    if (!scene_known) {
        nr_add(&nr.scene_unknown, 1U);
        return;
    }
    size = nr_size(scene_width, scene_height);
    if (size)
        nr_metric(&size->end_scene, elapsed, 0);
    scene_known = 0U;
}

void isaacNativeResourceDepth(void *data, uint32_t w, uint32_t h,
                              uint64_t elapsed, int failed)
{
    isaacNativeResourceEvent(ISAAC_NR_DEPTH_CREATE, w, h, elapsed, failed);
    if (!failed)
        nr_register(data, 2U, w, h);
}

void isaacNativeResourceGcObject(void)
{
    nr_add(&nr.gc_objects, 1U);
}

void isaacNativeResourceRecovery(unsigned kind, uint32_t bytes,
                                 uint64_t elapsed, int failed)
{
    if (kind != ISAAC_NR_RECOVER_CPU && kind != ISAAC_NR_RECOVER_GPU)
        return;
    nr_add(kind == ISAAC_NR_RECOVER_CPU ? &nr.recovery_cpu_bytes :
           &nr.recovery_gpu_bytes, bytes);
    isaacNativeResourceEvent(kind, 0U, 0U, elapsed, failed);
}

static void *nr_retire(void *pointer, uint32_t kind)
{
    IsaacNativeResourceIdentity *record;
    IsaacNativeResourceSize *size;
    if (!pointer)
        return pointer;
    nr_add(kind == 1U ? &nr.rt_retired : &nr.depth_retired, 1U);
    record = nr_find(pointer, kind);
    if (!record) {
        nr_add(&nr.unknown_retire, 1U);
        return pointer;
    }
    size = nr_size(record->width, record->height);
    if (size)
        nr_add(kind == 1U ? &size->rt_retired : &size->depth_retired, 1U);
    /* Depth retirement is measured, not its later generic free. RT identity
     * must survive the rotating purge list until actual native destruction. */
    if (kind == 2U)
        record->pointer = NULL;
    return pointer;
}

void *isaacNativeResourceRetireRT(void *rt) { return nr_retire(rt, 1U); }
void *isaacNativeResourceRetireDepth(void *data) { return nr_retire(data, 2U); }

void isaacNativeResourceFinish(SceGxmContext *context)
{
    uint64_t start = sceKernelGetProcessTimeWide();
    sceGxmFinish(context); /* SDK returns void, despite stale @return prose. */
    isaacNativeResourceEvent(ISAAC_NR_FINISH, 0U, 0U,
                             sceKernelGetProcessTimeWide() - start, 0);
}

int isaacNativeResourceCreateRT(const SceGxmRenderTargetParams *params,
                                SceGxmRenderTarget **target)
{
    uint32_t w = params ? params->width : 0U;
    uint32_t h = params ? params->height : 0U;
    uint64_t start = sceKernelGetProcessTimeWide();
    int result = sceGxmCreateRenderTarget(params, target);
    isaacNativeResourceEvent(ISAAC_NR_RT_CREATE, w, h,
                             sceKernelGetProcessTimeWide() - start, result != 0);
    if (!result && target)
        nr_register(*target, 1U, w, h);
    return result;
}

int isaacNativeResourceDestroyRT(SceGxmRenderTarget *target)
{
    IsaacNativeResourceIdentity *record = nr_find(target, 1U);
    uint32_t w = record ? record->width : 0U;
    uint32_t h = record ? record->height : 0U;
    uint64_t start = sceKernelGetProcessTimeWide();
    int result = sceGxmDestroyRenderTarget(target);
    isaacNativeResourceEvent(ISAAC_NR_RT_DESTROY, w, h,
                             sceKernelGetProcessTimeWide() - start, result != 0);
    if (!record)
        nr_add(&nr.unknown_destroy, 1U);
    else if (!result)
        record->pointer = NULL;
    return result;
}

void vglIsaacNativeResourceProfileTake(IsaacNativeResourceStats *out)
{
    if (!out)
        return;
    nr.abi = ISAAC_NATIVE_RESOURCE_ABI;
    nr.ram_free = (uint32_t)vglMemFree(VGL_MEM_RAM);
    nr.ram_total = (uint32_t)vglMemTotal(VGL_MEM_RAM);
    nr.vram_free = (uint32_t)vglMemFree(VGL_MEM_VRAM);
    nr.vram_total = (uint32_t)vglMemTotal(VGL_MEM_VRAM);
    *out = nr;
    vgl_memset(&nr, 0, sizeof nr);
}
