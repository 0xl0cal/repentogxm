/* ISAAC_VITA_GXM_RT_CREATE_CENSUS (measurement only, default OFF).
 *
 * Counts and times every sceGxmCreateRenderTarget and
 * sceGxmDestroyRenderTarget call of the process, whoever makes it (vitaGL's
 * display target at vglInit, its offscreen FBO targets, the render-target
 * reuse pool of the stock reference patches).  The calls are intercepted at
 * link time (GNU ld --wrap=sceGxmCreateRenderTarget
 * --wrap=sceGxmDestroyRenderTarget), so libvitaGL.a is byte-identical to the
 * build without the knob; each wrapper passes its arguments through
 * unchanged and returns the real function's return value.
 *
 * Why it exists: on a build with the GL render thread
 * (ISAAC_VITA_GL_THREAD_WORKER) no other receipt reports render-target
 * creates.  ph120.gx rt(c,x,z) needs ISAAC_VITA_GL_TIME_PROFILE, which
 * CMake refuses together with the render thread (CMakeLists.txt, the
 * ISAAC_GL_WORKER_CONFLICT list), and the wait probe's ph120.wr / ph120.ft
 * live on another line of development.
 *
 * One record per ph120 window (kage_vita_phase_profile.c, after ph120.da):
 *   ph120.gr bid= win= loops= create(c,x,us,max)=C,X,US,MAX destroy(n,us)=N,US
 *     c    sceGxmCreateRenderTarget calls that returned >= 0
 *     x    calls that returned < 0 (vitaGL's 0007 site retries with one
 *          scene slot: that retry is one more call)
 *     us   process-clock microseconds inside all create calls, failed ones
 *          included
 *     max  the longest single create call of the window, us
 *     n,us sceGxmDestroyRenderTarget calls and the time inside them
 * The wrappers run on whichever thread calls GXM (the GL render thread when
 * it exists) and the record is taken on the game thread: every counter is a
 * 32-bit atomic, taken with an exchange.  A call that straddles the take is
 * counted in the next window; max can be attributed one window late for the
 * same reason.
 *
 * The first record is preceded by the receipt
 *   KAGE VITA GXM RT CREATE CENSUS: on wrap=sceGxmCreateRenderTarget,
 *   sceGxmDestroyRenderTarget seen(c,x,d)=C,X,D receipt=ph120.gr
 * where seen() are the totals since process start at that moment: c >= 1
 * shows that the wrapper is linked in front of vitaGL's own calls (the
 * display target is created before the first window).
 *
 * KAGE_VITA_GXM_RT_CENSUS_WRAPS defines the two wrappers in the including
 * translation unit (exactly one: kage_vita_phase_profile.c; the host oracle
 * recomp/vita/host_tests/gxm_rt_census_oracle.c for the test). */
#ifndef KAGE_VITA_GXM_RT_CENSUS_H
#define KAGE_VITA_GXM_RT_CENSUS_H

#include <stdint.h>

typedef struct KageVitaGxmRtCensus {
    uint32_t created;
    uint32_t failed;
    uint32_t create_us;
    uint32_t create_max_us;
    uint32_t destroyed;
    uint32_t destroy_us;
} KageVitaGxmRtCensus;

#define KAGE_VITA_GXM_RT_CENSUS_FORMAT \
    "[kage-vita] ph120.gr bid=%.32s win=%u loops=%u " \
    "create(c,x,us,max)=%u,%u,%u,%u destroy(n,us)=%u,%u\n"
#define KAGE_VITA_GXM_RT_CENSUS_ARGS(take) \
    (take)->created, (take)->failed, (take)->create_us, \
    (take)->create_max_us, (take)->destroyed, (take)->destroy_us
#define KAGE_VITA_GXM_RT_CENSUS_RECEIPT_FORMAT \
    "[kage-vita] KAGE VITA GXM RT CREATE CENSUS: on " \
    "wrap=sceGxmCreateRenderTarget,sceGxmDestroyRenderTarget " \
    "seen(c,x,d)=%u,%u,%u receipt=ph120.gr\n"

/* Window counters (taken and zeroed) and totals since start (never reset). */
static volatile uint32_t s_gxm_rt_census_window[6];
static volatile uint32_t s_gxm_rt_census_seen[3];

static inline uint32_t kage_vita_gxm_rt_census_clamp(uint64_t us)
{
    return us > 0xffffffffull ? 0xffffffffu : (uint32_t)us;
}

static inline void kage_vita_gxm_rt_census_note_create(int rc, uint64_t us)
{
    uint32_t spent = kage_vita_gxm_rt_census_clamp(us);
    unsigned slot = rc < 0 ? 1u : 0u;

    __atomic_fetch_add(&s_gxm_rt_census_window[slot], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_gxm_rt_census_seen[slot], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_gxm_rt_census_window[2], spent, __ATOMIC_RELAXED);
    /* Single writer per call site thread; the game thread only exchanges
     * the slot with 0, after which this store opens the next window. */
    if (spent > __atomic_load_n(&s_gxm_rt_census_window[3], __ATOMIC_RELAXED))
        __atomic_store_n(&s_gxm_rt_census_window[3], spent, __ATOMIC_RELAXED);
}

static inline void kage_vita_gxm_rt_census_note_destroy(uint64_t us)
{
    __atomic_fetch_add(&s_gxm_rt_census_window[4], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_gxm_rt_census_seen[2], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_gxm_rt_census_window[5],
                       kage_vita_gxm_rt_census_clamp(us), __ATOMIC_RELAXED);
}

/* Copy the window counters out and zero them. */
static inline void kage_vita_gxm_rt_census_take(KageVitaGxmRtCensus *out)
{
    out->created =
        __atomic_exchange_n(&s_gxm_rt_census_window[0], 0u, __ATOMIC_RELAXED);
    out->failed =
        __atomic_exchange_n(&s_gxm_rt_census_window[1], 0u, __ATOMIC_RELAXED);
    out->create_us =
        __atomic_exchange_n(&s_gxm_rt_census_window[2], 0u, __ATOMIC_RELAXED);
    out->create_max_us =
        __atomic_exchange_n(&s_gxm_rt_census_window[3], 0u, __ATOMIC_RELAXED);
    out->destroyed =
        __atomic_exchange_n(&s_gxm_rt_census_window[4], 0u, __ATOMIC_RELAXED);
    out->destroy_us =
        __atomic_exchange_n(&s_gxm_rt_census_window[5], 0u, __ATOMIC_RELAXED);
}

/* Totals since process start: creates that succeeded, failed, destroys. */
static inline void kage_vita_gxm_rt_census_seen(uint32_t out[3])
{
    out[0] = __atomic_load_n(&s_gxm_rt_census_seen[0], __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&s_gxm_rt_census_seen[1], __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&s_gxm_rt_census_seen[2], __ATOMIC_RELAXED);
}

#if defined(KAGE_VITA_GXM_RT_CENSUS_WRAPS)
/* GNU ld --wrap: every undefined reference to sceGxmCreateRenderTarget in
 * the link (libvitaGL.a included) resolves to __wrap_..., and __real_...
 * is the SceGxm import.  The parameter types are spelled as untyped
 * pointers on purpose: both functions take pointers only and return int
 * (vitasdk psp2/gxm.h: int sceGxmCreateRenderTarget(const
 * SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget);
 * int sceGxmDestroyRenderTarget(SceGxmRenderTarget *renderTarget)), which
 * is the same AAPCS call, and this header then needs no GXM header. */
uint64_t sceKernelGetProcessTimeWide(void);
int __real_sceGxmCreateRenderTarget(const void *params, void **target);
int __real_sceGxmDestroyRenderTarget(void *target);
int __wrap_sceGxmCreateRenderTarget(const void *params, void **target);
int __wrap_sceGxmDestroyRenderTarget(void *target);

int __wrap_sceGxmCreateRenderTarget(const void *params, void **target)
{
    uint64_t start = sceKernelGetProcessTimeWide();
    int rc = __real_sceGxmCreateRenderTarget(params, target);

    kage_vita_gxm_rt_census_note_create(
        rc, sceKernelGetProcessTimeWide() - start);
    return rc;
}

int __wrap_sceGxmDestroyRenderTarget(void *target)
{
    uint64_t start = sceKernelGetProcessTimeWide();
    int rc = __real_sceGxmDestroyRenderTarget(target);

    kage_vita_gxm_rt_census_note_destroy(
        sceKernelGetProcessTimeWide() - start);
    return rc;
}
#endif

#endif
