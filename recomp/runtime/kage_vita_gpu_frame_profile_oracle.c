/* Host oracle for kage_vita_gpu_frame_profile.c (model half).
 *
 * Simulates an in-order GPU executing scenes submitted by a game thread,
 * polls the model exactly as the device thread does (completed first, then
 * submitted) and compares every aggregate against the true value integrated
 * from the same timeline.  Build:
 *   cc -std=c11 -O1 -DISAAC_KAGE_VITA_GPU_FRAME_PROFILE_ORACLE \
 *      kage_vita_gpu_frame_profile.c kage_vita_gpu_frame_profile_oracle.c */
#include "kage_vita_gpu_frame_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCENES 8192
static uint64_t s_submit[MAX_SCENES], s_end[MAX_SCENES];
static uint32_t s_n;
static int s_fail;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); s_fail = 1; } } while (0)

static void scene(uint64_t submit, uint64_t dur)
{
    uint64_t start = submit;
    if (s_n && s_end[s_n - 1] > start)
        start = s_end[s_n - 1];
    s_submit[s_n] = submit;
    s_end[s_n] = start + dur;
    ++s_n;
}

static uint32_t completed_at(uint64_t t)
{
    uint32_t i, c = 0;
    for (i = 0; i < s_n; ++i)
        if (s_end[i] <= t)
            ++c;
    return c;
}

static uint32_t submitted_at(uint64_t t)
{
    uint32_t i, c = 0;
    for (i = 0; i < s_n; ++i)
        if (s_submit[i] <= t)
            ++c;
    return c;
}

/* True GPU busy time in [a, b): union of [submit, end) of pending scenes. */
static uint64_t true_busy(uint64_t a, uint64_t b)
{
    uint64_t t, busy = 0;
    for (t = a; t < b; t += 10)
        if (submitted_at(t) != completed_at(t))
            busy += 10;
    return busy;
}

typedef struct frame_plan {
    uint64_t render_begin, present_enter, present_return;
    uint32_t scenes;
    uint64_t scene_dur, last_dur;
} frame_plan;

typedef struct result {
    kgfp_window w;
    uint64_t true_busy, true_rbusy, true_rwall, wall;
    uint32_t lag_true_p50;
} result;

/* Runs frames back to back with the given CPU frame period. */
static void run(const char *name, uint32_t frames, uint64_t period,
                uint64_t update, uint64_t render, uint32_t scenes,
                uint64_t scene_dur, uint64_t last_dur, uint32_t poll,
                result *r)
{
    static frame_plan plan[512];
    kgfp_poll_state ps;
    kgfp_present_state pr;
    kgfp_shared sh;
    uint64_t t, end;
    uint32_t f, next_frame = 0, phase = 0;

    memset(r, 0, sizeof *r);
    memset(&ps, 0, sizeof ps);
    memset(&pr, 0, sizeof pr);
    memset(&sh, 0, sizeof sh);
    s_n = 0;
    {
        uint64_t t0 = 1000;
        uint64_t last_scene_end[512];
        for (f = 0; f < frames; ++f) {
            uint64_t k, ret;
            plan[f].render_begin = t0 + update;
            plan[f].present_enter = t0 + update + render;
            ret = plan[f].present_enter + 100;
            for (k = 0; k < scenes; ++k)
                scene(plan[f].render_begin + (k + 1) * render / (scenes + 1), scene_dur);
            /* vitaGL 73dd57a: 3 display buffers, displayQueueMaxPendingCount
             * 2, so AddEntry returns only once frame f-2 has completed. */
            scene(plan[f].present_enter + 50, last_dur); /* scene_end first */
            if (f >= 2 && last_scene_end[f - 2] > ret)
                ret = last_scene_end[f - 2];
            plan[f].present_return = ret;
            last_scene_end[f] = s_end[s_n - 1];
            t0 = ret > t0 + period ? ret : t0 + period;
        }
        end = t0;
    }
    /* Interleave game events and polls in time order; game events at the
     * same instant run first (they are what the poll observes). */
    for (t = 0; t <= end; t += 10) {
        if (next_frame < frames) {
            frame_plan *p = &plan[next_frame];
            if (phase == 0 && t >= p->render_begin) { sh.in_render = 1; phase = 1; }
            if (phase == 1 && t >= p->present_enter) {
                kgfp_present_enter_model(&pr, &r->w, t, completed_at(t)); phase = 2; }
            if (phase == 2 && t >= p->present_return) {
                kgfp_present_return_model(&pr, &r->w, &sh, t, submitted_at(t));
                sh.in_render = 0; phase = 0; ++next_frame; }
        }
        if (t % poll == 0) {
            uint32_t c = completed_at(t), s = submitted_at(t);
            kgfp_poll_step(&ps, &r->w, &sh, t, c, s, poll);
        }
    }
    r->wall = end;
    r->true_busy = true_busy(0, end);
    for (f = 0; f < frames; ++f) {
        r->true_rwall += plan[f].present_return - plan[f].render_begin;
        r->true_rbusy += true_busy(plan[f].render_begin, plan[f].present_return);
    }
    {
        char line[512];
        int n = kgfp_format(line, sizeof line, "oracle", 1, 120, poll, &r->w);
        printf("%s: %s", name, line);
        CHECK(n > 0 && n < 384, "record length %d", n);
    }
}

static int near(uint64_t got, uint64_t want, uint64_t tol)
{
    return got + tol >= want && got <= want + tol;
}

static void hist_tests(void)
{
    kgfp_hist h;
    uint32_t i;

    memset(&h, 0, sizeof h);
    CHECK(kgfp_hist_quantile(&h, 50) == 0, "empty");
    for (i = 1; i <= 100; ++i)
        kgfp_hist_add(&h, i * 100);           /* 0.1 .. 10 ms */
    CHECK(kgfp_hist_quantile(&h, 50) == 5250, "p50 %u", kgfp_hist_quantile(&h, 50));
    CHECK(kgfp_hist_quantile(&h, 95) == 9750, "p95 %u", kgfp_hist_quantile(&h, 95));
    CHECK(kgfp_hist_quantile(&h, 100) == 10000, "max");
    kgfp_hist_add(&h, 200000);                /* open bucket */
    CHECK(h.max == 200000 && kgfp_hist_quantile(&h, 100) == 200000, "open");
    memset(&h, 0, sizeof h);
    kgfp_hist_add(&h, 130);
    CHECK(kgfp_hist_quantile(&h, 50) == 130, "edge clamps to max %u",
          kgfp_hist_quantile(&h, 50));
}

int main(void)
{
    result r;
    uint32_t frames = 60, poll = 500;

    hist_tests();

    /* A: water-like, CPU-bound.  35-ms CPU frames, 16-ms render with 12
     * offscreen scenes of 1.2 ms + a 2-ms display scene: GPU 16.4 ms/frame,
     * finishes shortly after the present, never two frames behind. */
    run("cpu-bound", frames, 35000, 12000, 16000, 12, 1200, 2000, poll, &r);
    CHECK(r.w.frames >= frames - 1, "frames %u", r.w.frames);
    CHECK(near(r.w.busy_us, r.true_busy, r.true_busy / 20),
          "busy %llu true %llu", (unsigned long long)r.w.busy_us,
          (unsigned long long)r.true_busy);
    CHECK(near(r.true_busy, (uint64_t)frames * 16400, frames * 50), "model busy");
    CHECK(near(kgfp_hist_quantile(&r.w.gf, 50), 16400, 2 * poll + KGFP_BUCKET_US),
          "gf p50 %u", kgfp_hist_quantile(&r.w.gf, 50));
    CHECK(near(r.w.rwall_us, r.true_rwall, (uint64_t)frames * 2 * poll), "rwall");
    CHECK(near(r.w.rbusy_us, r.true_rbusy, r.true_rbusy / 20), "rbusy");
    CHECK(r.w.q[2] == 0 && r.w.swp_q2_us == 0, "no queue-full presents");
    CHECK(kgfp_hist_quantile(&r.w.lag, 50) <= 2000 + 2 * poll + KGFP_BUCKET_US,
          "lag p50 %u", kgfp_hist_quantile(&r.w.lag, 50));
    CHECK(r.w.pend_max >= 1 && r.w.pend_max <= 3, "pend %u", r.w.pend_max);

    CHECK(r.w.lag.n >= frames - 1 && r.w.lag_lost == 0, "lag n %u", r.w.lag.n);

    /* B: GPU-bound.  20-ms CPU frames, GPU 30 ms/frame: the GPU falls
     * behind until the display queue (2 pending) blocks the present; busy
     * ~ wall, presents find 2 earlier frames pending and the swap waits. */
    run("gpu-bound", frames, 20000, 4000, 15000, 10, 2800, 2000, poll, &r);
    CHECK(r.w.busy_us * 100 >= r.w.wall_us * 95, "busy %llu wall %llu",
          (unsigned long long)r.w.busy_us, (unsigned long long)r.w.wall_us);
    CHECK(r.w.q[2] > frames / 2, "q2 %u", r.w.q[2]);
    CHECK(r.w.swp_q2_us > r.w.swp_other_us * 10, "swap wait is queue-full");
    CHECK(r.w.lag.n >= frames - 3, "lag n %u", r.w.lag.n);
    CHECK(kgfp_hist_quantile(&r.w.lag, 50) > 20000, "lag p50 %u",
          kgfp_hist_quantile(&r.w.lag, 50));
    /* While GPU-bound the per-frame busy is the GPU frame time. */
    CHECK(near(kgfp_hist_quantile(&r.w.gf, 50), 30000, 2 * poll + KGFP_BUCKET_US),
          "gf p50 %u", kgfp_hist_quantile(&r.w.gf, 50));

    /* C: idle GPU (menu): 16.7-ms frames, 2 ms of GPU work. */
    run("gpu-idle", frames, 16667, 1000, 13000, 2, 500, 1000, poll, &r);
    CHECK(r.w.busy_us * 100 <= r.w.wall_us * 20, "busy %llu wall %llu",
          (unsigned long long)r.w.busy_us, (unsigned long long)r.w.wall_us);
    CHECK(r.w.q[0] >= frames - 1, "q0 %u", r.w.q[0]);

    /* D: one scene per frame (display only), finished long before the next
     * present: completed == previous target exactly, which is NOT pending. */
    run("display-only", frames, 16667, 1000, 13000, 0, 500, 1000, poll, &r);
    CHECK(r.w.q[0] == frames && r.w.q[1] == 0, "q %u,%u", r.w.q[0], r.w.q[1]);

    printf(s_fail ? "GPU frame profile oracle: FAIL\n"
                  : "GPU frame profile oracle: PASS\n");
    return s_fail;
}
