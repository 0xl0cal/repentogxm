#ifndef KAGE_VITA_GPU_FRAME_PROFILE_H
#define KAGE_VITA_GPU_FRAME_PROFILE_H

/* GPU frame profile (ISAAC_VITA_GPU_FRAME_PROFILE, profile-only, default OFF).
 *
 * Question it answers: is a frame bounded by the game thread's CPU, by the
 * GPU, or by the display queue?  Pinned vitaGL 73dd57a already gives every
 * GXM scene a fragment notification (gxm.c scene_end:
 * `query_fence.value++; sceGxmEndScene(gxm_context, NULL, &query_fence)`),
 * so `query_fence.value` counts submitted scenes and `*query_fence.address`
 * (GPU-written) counts scenes whose fragment work has completed.  No vitaGL
 * patch is needed: a poll thread on CPU 2 reads both every period and
 * integrates
 *   busy   = wall time with at least one submitted scene not yet complete
 *            (GXM executes scenes in order, so this is GPU busy time, plus
 *            any GPU-side wait for a display buffer, which only happens when
 *            the queue is full and then shows up in q2 below);
 *   rbusy  = the part of busy while the game thread is inside Render.  The
 *            game thread can only be blocked on the GPU while a scene is
 *            pending, so rwall-rbusy is render time that is certainly CPU.
 * Per frame (between consecutive present returns): GPU busy time gf, and
 * lag = present return -> that frame's last scene completed.  At present
 * entry the game thread counts its own earlier frames still pending on the
 * GPU (q0/q1/q2+; vitaGL 73dd57a: 3 display buffers, display queue max
 * pending 2, so q2 means sceGxmDisplayQueueAddEntry may block) and splits
 * swap time by that class.
 *
 * Record, one per phase window, right after ph120.t / the sampler records:
 *   ph120.gpu bid= win= loops= period_us= poll(n,late)= wall= busy= rwall=
 *             rbusy= f= gf(50,95,max)= lag(n,50,95,max,lost)= q(0,1,2)=
 *             swp(q2,oth)= pend(max)=
 * all times in us.  Parsed by a host-side log analyzer (not in this repo).
 *
 * With the GL worker thread (GL_THREAD_WORKER) scenes are submitted on the
 * render thread; busy/rbusy/gf stay exact, lag/q/swp then describe the
 * game-side present post, not the swap. */

#include <stdint.h>

#if defined(ISAAC_VITA_GPU_FRAME_PROFILE) || \
    defined(ISAAC_KAGE_VITA_GPU_FRAME_PROFILE_ORACLE)

#define KGFP_BUCKET_US 250u
#define KGFP_BUCKETS   256u /* 0..64 ms, last bucket open-ended */

typedef struct kgfp_hist {
    uint32_t count[KGFP_BUCKETS];
    uint32_t n;
    uint32_t max;
} kgfp_hist;

/* One window of aggregates.  Poll-thread fields and game-thread fields are
 * disjoint; the reporter (game thread) swaps buffers. */
typedef struct kgfp_window {
    /* poll thread */
    uint32_t polls, late;
    uint64_t wall_us, busy_us, rwall_us, rbusy_us;
    uint32_t frames;
    kgfp_hist gf, lag;
    uint32_t lag_lost;   /* presents more than KGFP_RING behind the GPU */
    uint32_t pend_max;
    /* game thread */
    uint32_t q[3];
    uint64_t swp_q2_us, swp_other_us;
} kgfp_window;

/* Poll-thread model state (pure; the oracle drives it directly). */
typedef struct kgfp_poll_state {
    uint32_t have_prev;
    uint64_t prev_us;
    uint32_t prev_busy, prev_render;
    uint64_t frame_busy_us;
    uint32_t prev_presents;
    uint32_t lag_handled;
} kgfp_poll_state;

/* Values the game thread publishes for the poll thread.  Present seq (1..)
 * owns ring slot (seq - 1) & 3: the scene index of its display scene and
 * its return time; presents is written last (release). */
#define KGFP_RING 4u
typedef struct kgfp_shared {
    volatile uint32_t in_render;
    volatile uint32_t presents;
    volatile uint32_t target[KGFP_RING];
    volatile uint64_t submit_us[KGFP_RING];
} kgfp_shared;

void kgfp_hist_add(kgfp_hist *h, uint32_t us);
/* Upper edge of the bucket holding the q-th quantile (q in percent); max
 * for the open bucket and for q == 100. */
uint32_t kgfp_hist_quantile(const kgfp_hist *h, uint32_t q);

/* One poll: completed/submitted read in that order (completed <= submitted
 * at every instant, so pending never goes negative), period for "late". */
void kgfp_poll_step(kgfp_poll_state *s, kgfp_window *w,
                    const kgfp_shared *sh, uint64_t now_us,
                    uint32_t completed, uint32_t submitted,
                    uint32_t period_us);

/* Game-thread side of a present, pure: frames still pending at entry from
 * the ring of the last targets, and the swap time split. */
typedef struct kgfp_present_state {
    uint32_t ring[4];
    uint32_t ring_n;
    uint32_t entry_class;
    uint64_t entry_us;
} kgfp_present_state;

void kgfp_present_enter_model(kgfp_present_state *p, kgfp_window *w,
                              uint64_t now_us, uint32_t completed);
void kgfp_present_return_model(kgfp_present_state *p, kgfp_window *w,
                               kgfp_shared *sh, uint64_t now_us,
                               uint32_t submitted);

/* Formats the record (no newline split: one call, one line). */
int kgfp_format(char *out, unsigned size, const char *build_id,
                uint32_t window, uint32_t loops, uint32_t period_us,
                const kgfp_window *w);

#endif

#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)
/* Runtime hooks (device).  Start is lazy at the first present, when vitaGL
 * has initialised query_fence. */
void kage_vita_gpu_frame_profile_present_enter(void);
void kage_vita_gpu_frame_profile_present_return(void);
void kage_vita_gpu_frame_profile_render(uint32_t inside);
void kage_vita_gpu_frame_profile_report(
    const char *build_id, uint32_t window, uint32_t last_outer_loop);
# define KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_ENTER() \
    kage_vita_gpu_frame_profile_present_enter()
# define KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_RETURN() \
    kage_vita_gpu_frame_profile_present_return()
# define KAGE_VITA_GPU_FRAME_PROFILE_RENDER(inside) \
    kage_vita_gpu_frame_profile_render((inside))
# define KAGE_VITA_GPU_FRAME_PROFILE_REPORT(build_id, window, loops) \
    kage_vita_gpu_frame_profile_report((build_id), (window), (loops))
#else
# define KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_ENTER() ((void)0)
# define KAGE_VITA_GPU_FRAME_PROFILE_PRESENT_RETURN() ((void)0)
# define KAGE_VITA_GPU_FRAME_PROFILE_RENDER(inside) ((void)0)
# define KAGE_VITA_GPU_FRAME_PROFILE_REPORT(build_id, window, loops) ((void)0)
#endif

#endif
