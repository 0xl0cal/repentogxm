/* GPU frame profile (ISAAC_VITA_GPU_FRAME_PROFILE).  See the header for the
 * mechanism and the record.  The model functions are pure and shared with
 * the host oracle (kage_vita_gpu_frame_profile_oracle.c); only the poll
 * thread, the clock and the vitaGL fence differ on the device. */
#include "kage_vita_gpu_frame_profile.h"

#include <stddef.h>
#include <string.h>

#if defined(ISAAC_KAGE_VITA_GPU_FRAME_PROFILE_ORACLE)
# include <stdio.h>
# define KGFP_SNPRINTF snprintf
#else
# include <psp2/gxm.h>
# include <psp2/kernel/clib.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/threadmgr.h>
# define KGFP_SNPRINTF sceClibSnprintf
# if defined(ISAAC_VITA_LOG_ASYNC)
#  include "host_vita_log_async.h"
# else
#  define ISAAC_VITA_LOG_PRINTF sceClibPrintf
# endif
#endif

/* ------------------------------------------------------------- model ---- */

void kgfp_hist_add(kgfp_hist *h, uint32_t us)
{
    uint32_t b = us / KGFP_BUCKET_US;

    if (b >= KGFP_BUCKETS)
        b = KGFP_BUCKETS - 1u;
    ++h->count[b];
    ++h->n;
    if (us > h->max)
        h->max = us;
}

uint32_t kgfp_hist_quantile(const kgfp_hist *h, uint32_t q)
{
    uint32_t rank, seen = 0u, b;

    if (!h->n)
        return 0u;
    if (q >= 100u)
        return h->max;
    rank = (uint32_t)(((uint64_t)h->n * q + 99u) / 100u);
    if (!rank)
        rank = 1u;
    for (b = 0u; b < KGFP_BUCKETS; ++b) {
        seen += h->count[b];
        if (seen >= rank) {
            uint32_t edge = (b + 1u) * KGFP_BUCKET_US;
            if (b == KGFP_BUCKETS - 1u || edge > h->max)
                return h->max;
            return edge;
        }
    }
    return h->max;
}

void kgfp_poll_step(kgfp_poll_state *s, kgfp_window *w,
                    const kgfp_shared *sh, uint64_t now_us,
                    uint32_t completed, uint32_t submitted,
                    uint32_t period_us)
{
    uint32_t pending = submitted - completed;
    uint32_t busy = pending != 0u && pending < 0x80000000u;
    uint32_t presents = __atomic_load_n(&sh->presents, __ATOMIC_ACQUIRE);
    uint32_t render = sh->in_render != 0u;

    ++w->polls;
    if (busy && pending > w->pend_max)
        w->pend_max = pending;
    if (!s->have_prev) {
        s->have_prev = 1u;
        s->prev_presents = presents;
        s->lag_handled = presents;
    } else {
        uint64_t dt = now_us > s->prev_us ? now_us - s->prev_us : 0u;

        if (dt > 4u * (uint64_t)period_us)
            ++w->late;
        w->wall_us += dt;
        if (s->prev_busy) {
            w->busy_us += dt;
            s->frame_busy_us += dt;
        }
        if (s->prev_render) {
            w->rwall_us += dt;
            if (s->prev_busy)
                w->rbusy_us += dt;
        }
        if (presents != s->prev_presents) {
            kgfp_hist_add(&w->gf, s->frame_busy_us > UINT32_MAX
                                      ? UINT32_MAX
                                      : (uint32_t)s->frame_busy_us);
            ++w->frames;
            s->frame_busy_us = 0u;
            s->prev_presents = presents;
        }
        if (presents - s->lag_handled > KGFP_RING) {
            w->lag_lost += presents - s->lag_handled - KGFP_RING;
            s->lag_handled = presents - KGFP_RING;
        }
        while (s->lag_handled != presents) {
            uint32_t slot = s->lag_handled & (KGFP_RING - 1u); /* seq-1 */
            uint64_t at, lag;

            if ((int32_t)(completed - sh->target[slot]) < 0)
                break;
            at = sh->submit_us[slot];
            lag = now_us > at ? now_us - at : 0u;
            kgfp_hist_add(&w->lag, lag > UINT32_MAX ? UINT32_MAX
                                                    : (uint32_t)lag);
            ++s->lag_handled;
        }
    }
    s->prev_us = now_us;
    s->prev_busy = busy;
    s->prev_render = render;
}

void kgfp_present_enter_model(kgfp_present_state *p, kgfp_window *w,
                              uint64_t now_us, uint32_t completed)
{
    uint32_t i, pending = 0u;

    for (i = 0u; i < p->ring_n; ++i)
        if ((int32_t)(p->ring[i] - completed) > 0)
            ++pending;
    p->entry_class = pending > 2u ? 2u : pending;
    ++w->q[p->entry_class];
    p->entry_us = now_us;
}

void kgfp_present_return_model(kgfp_present_state *p, kgfp_window *w,
                               kgfp_shared *sh, uint64_t now_us,
                               uint32_t submitted)
{
    uint64_t swp = now_us > p->entry_us ? now_us - p->entry_us : 0u;

    if (p->entry_class >= 2u)
        w->swp_q2_us += swp;
    else
        w->swp_other_us += swp;
    memmove(&p->ring[1], &p->ring[0], 3u * sizeof p->ring[0]);
    p->ring[0] = submitted;
    if (p->ring_n < 4u)
        ++p->ring_n;
    {
        uint32_t seq = sh->presents + 1u;
        uint32_t slot = (seq - 1u) & (KGFP_RING - 1u);

        sh->target[slot] = submitted;
        sh->submit_us[slot] = now_us;
        __atomic_store_n(&sh->presents, seq, __ATOMIC_RELEASE);
    }
}

int kgfp_format(char *out, unsigned size, const char *build_id,
                uint32_t window, uint32_t loops, uint32_t period_us,
                const kgfp_window *w)
{
    return KGFP_SNPRINTF(
        out, size,
        "[kage-vita] ph120.gpu bid=%.32s win=%u loops=%u period_us=%u "
        "poll(n,late)=%u,%u wall=%u busy=%u rwall=%u rbusy=%u f=%u "
        "gf(50,95,max)=%u,%u,%u lag(n,50,95,max,lost)=%u,%u,%u,%u,%u "
        "q(0,1,2)=%u,%u,%u "
        "swp(q2,oth)=%u,%u pend(max)=%u\n",
        build_id, (unsigned)window, (unsigned)loops, (unsigned)period_us,
        (unsigned)w->polls, (unsigned)w->late, (unsigned)w->wall_us,
        (unsigned)w->busy_us, (unsigned)w->rwall_us, (unsigned)w->rbusy_us,
        (unsigned)w->frames, (unsigned)kgfp_hist_quantile(&w->gf, 50u),
        (unsigned)kgfp_hist_quantile(&w->gf, 95u), (unsigned)w->gf.max,
        (unsigned)w->lag.n, (unsigned)kgfp_hist_quantile(&w->lag, 50u),
        (unsigned)kgfp_hist_quantile(&w->lag, 95u), (unsigned)w->lag.max,
        (unsigned)w->lag_lost,
        (unsigned)w->q[0], (unsigned)w->q[1], (unsigned)w->q[2],
        (unsigned)w->swp_q2_us, (unsigned)w->swp_other_us,
        (unsigned)w->pend_max);
}

/* ------------------------------------------------------------ device ---- */

#if defined(ISAAC_VITA_GPU_FRAME_PROFILE) && \
    !defined(ISAAC_KAGE_VITA_GPU_FRAME_PROFILE_ORACLE)

#ifndef ISAAC_VITA_GPU_FRAME_PROFILE_PERIOD_US
# define ISAAC_VITA_GPU_FRAME_PROFILE_PERIOD_US 500u
#endif
#define KGFP_PERIOD_US       ((uint32_t)ISAAC_VITA_GPU_FRAME_PROFILE_PERIOD_US)
#define KGFP_THREAD_PRIORITY 96
#define KGFP_THREAD_STACK    0x2000u
#define KGFP_THREAD_AFFINITY SCE_KERNEL_CPU_MASK_USER_2
#define KGFP_REPORT_SPIN     200000u

void isaac_vita_log(const char *format, ...);

/* vitaGL 73dd57a buffers.c:45, initialised in vglInit (buffers.c:82-84);
 * advanced once per scene in gxm.c scene_end. */
extern SceGxmNotification query_fence;

static kgfp_window s_windows[2];
static uint32_t s_active;
static uint32_t s_busy;
static kgfp_shared s_shared;
static kgfp_poll_state s_poll;
static kgfp_present_state s_present;
static SceUID s_thread = -1;
static uint32_t s_started;

static inline uint32_t kgfp_completed(void)
{
    return *(volatile const uint32_t *)query_fence.address;
}

static inline uint32_t kgfp_submitted(void)
{
    return *(volatile const uint32_t *)&query_fence.value;
}

static int kgfp_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        uint32_t completed, submitted;
        kgfp_window *w;

        __atomic_store_n(&s_busy, 1u, __ATOMIC_SEQ_CST);
        w = &s_windows[__atomic_load_n(&s_active, __ATOMIC_SEQ_CST) & 1u];
        completed = kgfp_completed();   /* first: completed <= submitted */
        submitted = kgfp_submitted();
        kgfp_poll_step(&s_poll, w, &s_shared, sceKernelGetProcessTimeWide(),
                       completed, submitted, KGFP_PERIOD_US);
        __atomic_store_n(&s_busy, 0u, __ATOMIC_SEQ_CST);
        (void)sceKernelDelayThread(KGFP_PERIOD_US);
    }
    return 0;
}

static void kgfp_start(void)
{
    int started;

    s_started = 1u;
    if (!query_fence.address) {
        isaac_vita_log("KAGE VITA GPU FRAME PROFILE: state=FAILED no query_fence");
        return;
    }
    s_thread = sceKernelCreateThread("isaac_gpu_frame", kgfp_thread,
                                     KGFP_THREAD_PRIORITY, KGFP_THREAD_STACK,
                                     0u, KGFP_THREAD_AFFINITY, NULL);
    started = s_thread < 0 ? (int)s_thread
                           : sceKernelStartThread(s_thread, 0u, NULL);
    if (s_thread < 0 || started < 0) {
        if (s_thread >= 0)
            (void)sceKernelDeleteThread(s_thread);
        isaac_vita_log("KAGE VITA GPU FRAME PROFILE: state=FAILED "
                       "create=0x%08x start=0x%08x",
                       (unsigned)s_thread, (unsigned)started);
        s_thread = -1;
        return;
    }
    isaac_vita_log("KAGE VITA GPU FRAME PROFILE: thread=0x%08x affinity=0x%08x "
                   "prio=%d period_us=%u record=ph120.gpu",
                   (unsigned)s_thread, (unsigned)KGFP_THREAD_AFFINITY,
                   (int)KGFP_THREAD_PRIORITY, (unsigned)KGFP_PERIOD_US);
}

void kage_vita_gpu_frame_profile_present_enter(void)
{
    if (!s_started)
        kgfp_start();
    if (s_thread < 0)
        return;
    kgfp_present_enter_model(&s_present, &s_windows[s_active & 1u],
                             sceKernelGetProcessTimeWide(), kgfp_completed());
}

void kage_vita_gpu_frame_profile_present_return(void)
{
    if (s_thread < 0)
        return;
    kgfp_present_return_model(&s_present, &s_windows[s_active & 1u],
                              &s_shared, sceKernelGetProcessTimeWide(),
                              kgfp_submitted());
}

void kage_vita_gpu_frame_profile_render(uint32_t inside)
{
    s_shared.in_render = inside;
}

void kage_vita_gpu_frame_profile_report(
    const char *build_id, uint32_t window, uint32_t last_outer_loop)
{
    char line[384];
    kgfp_window *retired;
    uint32_t old, spins = 0u;

    if (s_thread < 0)
        return;
    old = s_active & 1u;
    __atomic_store_n(&s_active, old ^ 1u, __ATOMIC_SEQ_CST);
    /* Same Dekker as the guest sampler: busy is published before the index
     * is read, so once it is clear the poller is off the retired buffer. */
    while (__atomic_load_n(&s_busy, __ATOMIC_SEQ_CST) && spins < KGFP_REPORT_SPIN)
        ++spins;
    retired = &s_windows[old];
    if (kgfp_format(line, sizeof line, build_id, window, last_outer_loop,
                    KGFP_PERIOD_US, retired) > 0)
        ISAAC_VITA_LOG_PRINTF("%s", line);
    memset(retired, 0, sizeof *retired);
}

#endif
