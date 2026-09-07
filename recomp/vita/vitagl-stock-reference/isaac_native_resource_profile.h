#ifndef ISAAC_NATIVE_RESOURCE_PROFILE_H
#define ISAAC_NATIVE_RESOURCE_PROFILE_H

#include <stdint.h>

/* CPU elapsed microseconds, not GPU execution time. Inclusive brackets overlap:
 * recovery can contain Finish + GC; GC can contain RT destruction; depth
 * allocation can contain recovery. Never add those totals as disjoint costs.
 * Owner-thread take-and-zero; the native build requires SINGLE_THREADED_GC.
 * Only window counters reset. Resource identity and active-scene size persist.
 * clock_clamp means a bracket exceeded uint32 (including clock rollback):
 * saturated times in that window are not measured multi-minute stalls.
 * EndScene's native return is ignored by stock: end_scene.failed stays zero
 * as unavailable, not as proof of successful completion. Finish returns void.
 */
#define ISAAC_NATIVE_RESOURCE_ABI 1U
#define ISAAC_NATIVE_RESOURCE_SIZE_SLOTS 8U
enum {
    ISAAC_NR_GC, ISAAC_NR_FINISH, ISAAC_NR_RT_CREATE, ISAAC_NR_RT_DESTROY,
    ISAAC_NR_DEPTH_CREATE, ISAAC_NR_RECOVER_CPU, ISAAC_NR_RECOVER_GPU,
    ISAAC_NR_METRIC_COUNT
};
typedef struct {
    uint32_t count, us, max_us, failed;
} IsaacNativeResourceMetric;
typedef struct {
    uint32_t width, height, rt_retired, depth_retired;
    IsaacNativeResourceMetric end_scene, rt_create, rt_destroy, depth_create;
} IsaacNativeResourceSize;
typedef struct {
    uint32_t abi, sizes_used, size_overflow, registry_overflow;
    uint32_t unknown_destroy, unknown_retire, clock_clamp, saturation;
    uint32_t gc_objects, rt_retired, depth_retired, scene_unknown;
    uint32_t recovery_cpu_bytes, recovery_gpu_bytes;
    /* Pool endpoints sampled only by Take, not minimum/maximum or whole RAM. */
    uint32_t ram_free, ram_total, vram_free, vram_total;
    IsaacNativeResourceMetric metric[ISAAC_NR_METRIC_COUNT];
    IsaacNativeResourceSize size[ISAAC_NATIVE_RESOURCE_SIZE_SLOTS];
} IsaacNativeResourceStats;

#ifdef __cplusplus
extern "C" {
#endif
void vglIsaacNativeResourceProfileTake(IsaacNativeResourceStats *out);
#ifdef __cplusplus
}
#endif

#endif
