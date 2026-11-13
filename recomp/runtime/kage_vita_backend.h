#ifndef KAGE_VITA_BACKEND_H
#define KAGE_VITA_BACKEND_H

#include <stdint.h>

/* Process-wide vitaGL owner used by the high-level KAGE boundary.  The
 * physical display is 960x544; KAGE keeps its measured 960x540 logical size. */
int         kage_vita_backend_initialize(uint32_t width, uint32_t height);
void        kage_vita_backend_deactivate(void);
int         kage_vita_backend_present(void);
/* Records a request but always applies GL_FALSE: guest software pacing owns
 * the Vita session's actual 60 Hz policy. */
int         kage_vita_backend_set_vsync(int enabled);
int         kage_vita_backend_time_seconds(double *seconds);
int         kage_vita_backend_ready(void);
uint32_t    kage_vita_backend_width(void);
uint32_t    kage_vita_backend_height(void);
unsigned    kage_vita_backend_present_count(void);
unsigned    kage_vita_backend_vsync_change_count(void);
int         kage_vita_backend_vsync_enabled(void);
const char *kage_vita_backend_gl_version(void);
const char *kage_vita_backend_last_error(void);

#define KAGE_VITA_MEMORY_SNAPSHOT_MAX_BODY 366u

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* GL render thread: vitaGL free space as published by the render thread
 * (ISAAC_VITA_GL_THREAD_VGL_MEM_SNAPSHOT, kage_vita_backend.c). */
#define KAGE_VITA_VGL_SNAPSHOT_RAM_FREE     0u
#define KAGE_VITA_VGL_SNAPSHOT_VRAM_FREE    1u
#define KAGE_VITA_VGL_SNAPSHOT_PHYCONT_FREE 2u
#define KAGE_VITA_VGL_SNAPSHOT_RAM_TOTAL    3u
#define KAGE_VITA_VGL_SNAPSHOT_WORDS        4u
uint32_t    kage_vita_backend_vgl_mem(unsigned index);
#endif
#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
/* 0 = decline this interpolated render: the render thread is a frame behind. */
int         kage_vita_backend_render_admit(void);
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
/* GL admit wait (R60-DRYADMIT-SPEC-20261028.md), game thread only.
 * behind: the last present closure has not finished (never waits, counts
 * nothing).  refused: the gate declined while behind; counts ph120.rw rs
 * and, with latest_us != 0, arms the fit check at the next Present.
 * wait_until (ON): sleep until that closure finished (1) or the process
 * clock reached deadline_us (0). */
int         kage_vita_backend_render_behind(void);
void        kage_vita_backend_render_refused(uint64_t latest_us);
# if ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
int         kage_vita_backend_render_wait_until(uint64_t deadline_us);
/* The frame just admitted followed a wait: the next Present that holds on
 * its closure counts hd(n,us) and leaves the hold for admit_hold_take, which
 * returns it (us, 0 = none since the last call) to the scheduler's next loop
 * head. */
void        kage_vita_backend_render_admitted_after_wait(void);
uint32_t    kage_vita_backend_admit_hold_take(void);
# endif
/* e, a, miss_us, miss_max_us, hd_n, hd_us. */
void        kage_vita_backend_gl_admit_fit_take(uint32_t out[6]);
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
/* GL admit behind (R60-FABLE-PLAN.md lane B), game thread only.
 * gate: the stock admission predicate without its rs count; when behind (1)
 * *outstanding receives the ring bytes not yet replayed.  Never waits.
 * refused: the gate declined while behind; counts ph120.rw rs and arms the
 * closure check at the next Present with the instants (process clock, us)
 * the frame's Present and Render return were predicted at, the due time of
 * the loop head after it, and whether ON would have rendered it. */
int         kage_vita_backend_admit_behind_gate(uint32_t *outstanding);
void        kage_vita_backend_admit_behind_refused(
                uint64_t present_us, uint64_t return_us, uint64_t head_due_us,
                uint32_t would_admit);
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
/* The frame just admitted was behind: its own Present, and the Present after
 * it, count hd(n,us) when they hold on a closure and leave the hold for
 * hold_take, which returns it (us, 0 = none since the last call) to the
 * scheduler's next loop head. */
void        kage_vita_backend_admit_behind_admitted(void);
uint32_t    kage_vita_backend_admit_behind_hold_take(void);
# endif
/* pm(c,ok,us,max), wp(c,ok,us,max), phl, hd(n,us). */
#define KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS 11u
void        kage_vita_backend_gl_admit_behind_take(
                uint32_t out[KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS]);
#endif

#if defined(ISAAC_VITA_IO_PROFILE)
/* Called by the project-side vitaGL render-target event endpoint at the first
 * 1024x1024 FBO acquisition.  The implementation is diagnostic-only: query
 * failures are recorded and never change rendering or startup. */
void kage_vita_backend_memory_snapshot_at_first_fbo(
    int32_t event_result, const void *target);
#endif

#endif
