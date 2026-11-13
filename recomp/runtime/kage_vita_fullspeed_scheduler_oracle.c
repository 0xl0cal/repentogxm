/* Deterministic host oracle for parity-aware cadence, debt and fail-open. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_fullspeed_scheduler.h"
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
# include "kage_vita_backend.h"
#endif

static uint64_t s_now;
static uint32_t s_log_calls;
static uint32_t s_disable_log_calls;
static uint32_t s_gate_log_calls;
static uint32_t s_gate_log_truncated;
static char s_gate_record[384];
static uint32_t s_delay_fail;
/* CADENCE30 banners carrying " render-cost=excludes-present". */
static uint32_t s_render_cost_banner_calls;
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
static uint32_t s_test_single_clip;
#endif
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
/* Last CADENCE30 banner, for the " fps_lock=%u" suffix pin (as long as the
 * log record: the floor-debt suffixes push it past 256 bytes). */
static char s_banner[384];
#endif
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
static uint32_t s_test_service_us = 500u;
static uint32_t s_test_overshoot_us, s_test_variable_wake, s_test_waits;
static uint32_t s_test_backward_wait;
#else
/* Floor-debt scenarios only (run_debt and its wake-overshoot cases):
 * run_debt_tick names the scheduler wait that is sleeping, so a
 * scenario can give the loop-head wait and the Game::Update pace wait
 * different wake overshoots, add one extra overshoot to the next pace wait,
 * or make the next head sleep return with the clock 1 us before it started
 * (wait_until then returns 2).  The site is NONE everywhere else, so every
 * other transcript keeps its exact sleep. */
# define DEBT_WAIT_NONE 0u
# define DEBT_WAIT_HEAD 1u
# define DEBT_WAIT_PACE 2u
static uint32_t s_debt_wait_site;
static uint32_t s_debt_head_os_us, s_debt_pace_os_us;
static uint32_t s_debt_pace_extra_us, s_debt_pace_extra_hits;
static uint32_t s_debt_head_backward, s_debt_head_backward_hits;
/* pace sleeps so far, and the overshoot (+ extra) the last one added */
static uint32_t s_debt_pace_sleeps, s_debt_pace_wake_us;
#endif

#define ORACLE_MAX_GAME_UPDATES 1024u
#define KAGE_ORACLE_STR2(x) #x
#define KAGE_ORACLE_STR(x) KAGE_ORACLE_STR2(x)

/* Game::Update start contract the checks below hold the scheduler to.
 * Stock (and FLOOR_DEBT=OBSERVE): starts at least one Game floor period
 * (33,333 1/3 us) apart, at most 30 in any sliding second.  FLOOR_DEBT=ON
 * (debt kept): the floors are still one period apart, but a start
 * may come up to the cap earlier than one period after the previous start,
 * so the per-pair gap bound drops by the cap and 31 starts fit in a sliding
 * second (31 * 33,333 - 16,666 > 1,000,000 for any legal cap; long-run UPS
 * stays <= 30).  The cumulative form, oracle_start_spacing_holds, is the
 * claim the per-pair and per-second bounds follow from. */
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2
# define ORACLE_FLOOR_DEBT_CAP_US ((uint32_t)ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US)
# define ORACLE_MAX_UPDATES_PER_SECOND 31u
#else
# define ORACLE_FLOOR_DEBT_CAP_US 0u
# define ORACLE_MAX_UPDATES_PER_SECOND 30u
#endif
#define ORACLE_MIN_UPDATE_GAP_US (33333u - ORACLE_FLOOR_DEBT_CAP_US)

uint64_t isaac_vita_get_process_time(void) { return s_now; }
int sceKernelDelayThread(unsigned int delay_us)
{
    if (s_delay_fail)
        return -1;
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    if (s_test_backward_wait) {
        s_test_backward_wait = 0u;
        --s_now;
        return 0;
    }
    {
        const uint32_t multipliers[] = {0u, 2u, 1u, 3u, 0u, 1u};
        s_now += (uint64_t)s_test_overshoot_us *
            (s_test_variable_wake ? multipliers[s_test_waits % 6u] : 1u);
        ++s_test_waits;
    }
#else
    if (s_debt_wait_site == DEBT_WAIT_HEAD) {
        if (s_debt_head_backward) {
            s_debt_head_backward = 0u;
            ++s_debt_head_backward_hits;
            --s_now;
            return 0;
        }
        s_now += s_debt_head_os_us;
    } else if (s_debt_wait_site == DEBT_WAIT_PACE) {
        ++s_debt_pace_sleeps;
        s_debt_pace_wake_us = s_debt_pace_os_us;
        s_now += s_debt_pace_os_us;
        if (s_debt_pace_extra_us) {
            s_now += s_debt_pace_extra_us;
            s_debt_pace_wake_us += s_debt_pace_extra_us;
            s_debt_pace_extra_us = 0u;
            ++s_debt_pace_extra_hits;
        }
    }
#endif
    s_now += delay_us;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char record[384];
    int length;

    va_start(arguments, format);
    length = vsnprintf(record, sizeof record, format, arguments);
    va_end(arguments);
    if (strstr(record, "KAGE VITA CADENCE30:") == record &&
            strstr(record,
                   "tick_units=50000 game_tick_units=100000 "
                   "max_catchup=4 stale_catchup=1 max_full_skip=4 "
                   "parity=4a264 frame=1a30dc") != NULL)
        ++s_log_calls;
    if (strstr(record, "KAGE VITA CADENCE30:") == record &&
            strstr(record, " render-cost=excludes-present") != NULL)
        ++s_render_cost_banner_calls;
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
    if (strstr(record, "KAGE VITA CADENCE30:") == record)
        memcpy(s_banner, record, sizeof s_banner);
#endif
    if (strstr(record, "KAGE VITA CADENCE30 DISABLE:") == record)
        ++s_disable_log_calls;
    if (strstr(record, "KAGE VITA CADENCE30 GATE:") == record) {
        ++s_gate_log_calls;
        if (length < 0 || (size_t)length >= sizeof record)
            ++s_gate_log_truncated;
        memcpy(s_gate_record, record, sizeof s_gate_record);
    }
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "cadence30 oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 0; \
    } \
} while (0)

typedef struct ScenarioResult {
    KageVitaFullspeedSnapshot scheduler;
    uint32_t limiter_bypasses;
    uint32_t limiter_fallbacks;
    uint32_t maximum_wrapper_ticks_between_presents;
    uint32_t game_update_time_count;
    uint32_t maximum_game_updates_per_second;
    uint64_t game_update_times[ORACLE_MAX_GAME_UPDATES];
    uint64_t elapsed_us;
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    uint32_t present_time_count;
    uint64_t present_times[ORACLE_MAX_GAME_UPDATES * 2u];
#endif
} ScenarioResult;

typedef struct OracleGame {
    uint32_t manager_pointer;
    uint32_t manager_counter;
    uint32_t game_pointer;
    uint32_t game_frame;
    uint32_t wrapper_ticks_since_present;
} OracleGame;

/* Display wait inside Present: it lands between the Present seam and Render
 * return, as vglSwapBuffers does inside kage_pc_backend_present.  NONE (the
 * default) adds nothing, so every other transcript here is unchanged. */
#define PRESENT_WAIT_NONE 0u
/* Every fifth Present waits for the next 60-Hz edge; the others take 100 us. */
#define PRESENT_WAIT_EDGE 1u
/* Every fifth Present waits s_present_wait_us; the others take 100 us. */
#define PRESENT_WAIT_FIXED 2u
/* No vblank: the GPU runs frames back to back at s_present_wait_us each and
 * Present N waits until frame N-2 has finished (two frames in flight). */
#define PRESENT_WAIT_QUEUE 3u
/* As QUEUE, but a frame flips on the first 60-Hz edge after it finishes, one
 * flip per edge, and Present N waits until frame N-2 has flipped. */
#define PRESENT_WAIT_QUEUE_VBLANK 4u

static uint32_t s_present_wait_kind;
static uint32_t s_present_wait_edge_us;
static uint32_t s_present_wait_us;
static uint32_t s_present_wait_count;
static uint64_t s_present_wait_gpu_done[4];
static uint64_t s_present_wait_flip[4];
static uint64_t s_present_wait_max_us;

#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
/* GL render thread timeline (R60-DRYADMIT-SPEC-20261028.md).  Present waits
 * for the previous present closure (kage_vita_backend_present: worker_wait
 * on the last position, the hold), then posts closure k, which completes at
 *   WORKER:       start + base, or start + tail when k % period == period-1
 *                 (and one stall closure), start = post time after the hold;
 *   WORKER_QUEUE: as QUEUE_VBLANK on the render thread: the swap of closure
 *                 k returns once frame k-2 has flipped (2 pending), frames
 *                 flip one per 60-Hz edge after their GPU work (gpu_us).
 * The admission stand-ins below read it: behind = closure k-1 not done. */
#define PRESENT_WAIT_WORKER 5u
#define PRESENT_WAIT_WORKER_QUEUE 6u
static uint32_t s_wk_period, s_wk_tail_us, s_wk_base_us = 300u, s_wk_gpu_us;
static uint32_t s_wk_stall_at = UINT32_MAX, s_wk_stall_us;
static uint64_t s_wk_done_last;
static uint32_t s_wk_posts, s_wk_holds, s_wk_rs;
static uint64_t s_wk_hold_us;
/* Backend fit check stand-in (kvb_admit_fit_check): e, a, miss_us, max. */
static uint32_t s_wk_fit_armed, s_wk_fit_pos;
static uint64_t s_wk_fit_latest;
/* e, a, miss_us, miss_max_us, hd n, hd us (kage_vita_backend.c). */
static uint32_t s_wk_fit[6];
/* ON: Present hold on a closure admitted after a wait (kage_vita_backend.c
 * s_admit_waited_*): arm at the admission, position = post index + 1 of
 * that render's closure, the hold left for the next loop head. */
static uint32_t s_wk_waited_arm, s_wk_waited_pos, s_wk_hold_take_us;
# if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
/* GL admit behind (R60-FABLE-PLAN.md lane B).  What the gate stand-in
 * reports as the ring's outstanding bytes: OVER = UINT32_MAX (the default:
 * every scenario of this file then runs the stock refusal), FIXED =
 * s_ab_out_fixed, RATE = 8 bytes per microsecond the last closure still
 * needs (8 ms left = 64000 bytes, inside the default bound; 9 ms = 72000,
 * over it). */
#  define ORACLE_AB_OUT_OVER 0u
#  define ORACLE_AB_OUT_FIXED 1u
#  define ORACLE_AB_OUT_RATE 2u
static uint32_t s_ab_out_model, s_ab_out_fixed;
/* Backend stand-ins (kage_vita_backend.c s_ab_*): the closure check armed
 * by a refusal and read at the next Present, the pm/wp/phl/hd words, and
 * the ON hold arm / chain / hold left for the next loop head. */
static uint32_t s_ab_check_armed, s_ab_check_pos, s_ab_check_would;
static uint64_t s_ab_check_present, s_ab_check_return, s_ab_check_head;
static uint32_t s_ab_be[KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS];
/* What the scheduler has to predict at a behind gate, measured here on the
 * seams themselves, not read from the scheduler: the last Render's body
 * (entry to Present) and cost (entry to return), in us.  The refused
 * stand-in holds the two instants it is handed against gate + these; the
 * ordinal of the first refusal that disagrees is kept for the caller
 * (ab_account, the directed runs).  split = refusals whose two instants
 * differ, i.e. the ones that tell a Present from a Render return. */
static uint64_t s_ab_ref_entry, s_ab_ref_body, s_ab_ref_cost;
static uint32_t s_ab_ref_have, s_ab_ref_bad, s_ab_ref_checked, s_ab_ref_split;
/* Directed runs: no Game::Update at all (a menu; run_debt_tick's menu). */
static uint32_t s_ab_menu;
#  if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
static uint32_t s_ab_arm, s_ab_chain, s_ab_hold_take_us;
#  endif
# endif
# if defined(ORACLE_GL_ADMIT_BEHIND_TWIN)
/* The knob-OFF twin of the ON "within" pass: the stock gate told that the
 * render thread is never behind (Present still holds on it). */
static uint32_t s_wk_gate_never_behind;
# endif
# if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE) && \
    !(defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && \
    !defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT) && \
    !(defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2)
/* The worker cases (check_worker_cases) run only in the combination the
 * admit wait is proven with: service reserve, stock render cost, no head
 * advance, floor debt OFF or OBSERVE.  With floor debt ON gpuq-19-fight
 * changes 1000 -> 600 presents: not this knob's contract, and the knob
 * refuses that combination at compile time. */
#  define ORACLE_GL_WORKER_CASES 1
# endif
# if defined(ORACLE_GL_WORKER_CASES) || \
    (defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2)
/* Render-thread wake latency after the closure completes (ON wait). */
static uint32_t s_wk_wake_us;
# endif
# if defined(ORACLE_GL_WORKER_CASES)
/* Seeded variation (review round 1 finding 2, round 2 findings 1-3): the
 * loop's pre-Game service 500 us and the Render body each drawn uniformly
 * in [x - J, x + J], and a closure tail on one post in N drawn at random
 * instead of every period-th.  All three are 0 outside the rows that set
 * them, so no other transcript moves.  xorshift32, one stream each. */
static uint32_t s_wk_svc_jit_us, s_wk_body_jit_us, s_wk_tail_one_in;
static uint32_t s_wk_rng[3];

static uint32_t worker_rand(uint32_t stream)
{
    uint32_t x = s_wk_rng[stream];
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_wk_rng[stream] = x;
    return x;
}

static uint32_t worker_jittered(uint32_t base, uint32_t jitter, uint32_t stream)
{
    if (!jitter || jitter > base)
        return base;
    return base - jitter + worker_rand(stream) % (2u * jitter + 1u);
}
# endif

static int worker_behind_now(void)
{
    return s_wk_posts != 0u && s_wk_done_last > s_now;
}

static uint64_t present_wait_edge_at_or_after(uint64_t at_us);

static uint64_t worker_present(void)
{
    uint64_t hold = 0u, start, done;
    uint32_t k = s_wk_posts;

    if (worker_behind_now()) {
        hold = s_wk_done_last - s_now;
        ++s_wk_holds;
        s_wk_hold_us += hold;
    }
    if (s_wk_waited_pos && s_wk_waited_pos == k && hold) {
        s_wk_hold_take_us = (uint32_t)hold;
        ++s_wk_fit[4];
        s_wk_fit[5] += (uint32_t)hold;
    }
    s_wk_waited_pos = 0u;
# if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
#  if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    /* kvb_admit_behind_hold_begin / _end. */
    if ((s_ab_arm || (s_ab_chain && s_ab_chain == k)) && hold) {
        s_ab_hold_take_us += (uint32_t)hold;
        ++s_ab_be[9];
        s_ab_be[10] += (uint32_t)hold;
    }
    s_ab_chain = 0u;
#  endif
    /* kvb_admit_behind_check: after worker_wait, before the post. */
    if (s_ab_check_armed) {
        s_ab_check_armed = 0u;
        if (k != 0u && s_ab_check_pos == k) {
            uint64_t miss = s_wk_done_last > s_ab_check_present ?
                s_wk_done_last - s_ab_check_present : 0u;
            ++s_ab_be[0];
            if (s_ab_check_would)
                ++s_ab_be[4];
            if (!miss) {
                ++s_ab_be[1];
                if (s_ab_check_would)
                    ++s_ab_be[5];
            } else {
                s_ab_be[2] += (uint32_t)miss;
                if (miss > s_ab_be[3]) s_ab_be[3] = (uint32_t)miss;
                if (s_ab_check_would) {
                    s_ab_be[6] += (uint32_t)miss;
                    if (miss > s_ab_be[7]) s_ab_be[7] = (uint32_t)miss;
                }
                if (s_ab_check_return + miss >= s_ab_check_head)
                    ++s_ab_be[8];
            }
        }
    }
# endif
    start = s_now + hold;
    /* Backend fit check: after worker_wait, before the post. */
    if (s_wk_fit_armed) {
        if (s_wk_fit_pos == k) {
            ++s_wk_fit[0];
            if (s_wk_done_last <= s_wk_fit_latest) {
                ++s_wk_fit[1];
            } else {
                uint64_t miss = s_wk_done_last - s_wk_fit_latest;
                s_wk_fit[2] += (uint32_t)miss;
                if (miss > s_wk_fit[3]) s_wk_fit[3] = (uint32_t)miss;
            }
        }
        s_wk_fit_armed = 0u;
    }
    if (s_present_wait_kind == PRESENT_WAIT_WORKER) {
        uint32_t cost = s_wk_base_us;
        if (s_wk_period && k % s_wk_period == s_wk_period - 1u)
            cost = s_wk_tail_us;
#if defined(ORACLE_GL_WORKER_CASES)
        if (s_wk_tail_one_in && worker_rand(2u) % s_wk_tail_one_in == 0u)
            cost = s_wk_tail_us;
#endif
        if (k == s_wk_stall_at)
            cost = s_wk_stall_us;
        done = start + cost;
    } else {
        uint64_t replayed = start + s_wk_base_us, gpu_start, gpu_done, flip;
        done = replayed;
        if (k >= 2u && s_present_wait_flip[(k - 2u) & 3u] > done)
            done = s_present_wait_flip[(k - 2u) & 3u];
        gpu_start = replayed;
        if (k != 0u && s_present_wait_gpu_done[(k - 1u) & 3u] > gpu_start)
            gpu_start = s_present_wait_gpu_done[(k - 1u) & 3u];
        gpu_done = gpu_start + s_wk_gpu_us;
        flip = present_wait_edge_at_or_after(gpu_done);
        if (k != 0u && flip <= s_present_wait_flip[(k - 1u) & 3u])
            flip = present_wait_edge_at_or_after(
                s_present_wait_flip[(k - 1u) & 3u] + 1u);
        s_present_wait_gpu_done[k & 3u] = gpu_done;
        s_present_wait_flip[k & 3u] = flip;
    }
    s_wk_done_last = done;
    ++s_wk_posts;
    if (s_wk_waited_arm) {
        s_wk_waited_arm = 0u;
        s_wk_waited_pos = s_wk_posts;
    }
# if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    if (s_ab_arm) {
        s_ab_arm = 0u;
        s_ab_chain = s_wk_posts;
    }
# endif
    ++s_present_wait_count;
    if (hold > s_present_wait_max_us)
        s_present_wait_max_us = hold;
    return hold;
}
#endif

/* First 60-Hz edge (50000 units of 1/3 us, at phase edge_us) at or after
 * at_us, rounded up to the next microsecond. */
static uint64_t present_wait_edge_at_or_after(uint64_t at_us)
{
    uint64_t offset = (uint64_t)s_present_wait_edge_us * 3u;
    uint64_t at = at_us * 3u;
    uint64_t k = 0u;

    if (at > offset)
        k = (at - offset + 50000u - 1u) / 50000u;
    return (offset + k * 50000u + 2u) / 3u;
}

static uint64_t present_wait_us(void)
{
    uint32_t n = s_present_wait_count;
    uint64_t wait = 100u;

    if (s_present_wait_kind == PRESENT_WAIT_NONE)
        return 0u;
#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
    if (s_present_wait_kind == PRESENT_WAIT_WORKER ||
            s_present_wait_kind == PRESENT_WAIT_WORKER_QUEUE)
        return worker_present();
#endif
    if (s_present_wait_kind == PRESENT_WAIT_QUEUE ||
            s_present_wait_kind == PRESENT_WAIT_QUEUE_VBLANK) {
        uint64_t start, done, flip;

        wait = 0u;
        if (n >= 2u && s_present_wait_flip[(n - 2u) & 3u] > s_now)
            wait = s_present_wait_flip[(n - 2u) & 3u] - s_now;
        start = s_now + wait;
        if (n != 0u && s_present_wait_gpu_done[(n - 1u) & 3u] > start)
            start = s_present_wait_gpu_done[(n - 1u) & 3u];
        done = start + s_present_wait_us;
        flip = done;
        if (s_present_wait_kind == PRESENT_WAIT_QUEUE_VBLANK) {
            flip = present_wait_edge_at_or_after(done);
            if (n != 0u && flip <= s_present_wait_flip[(n - 1u) & 3u])
                flip = present_wait_edge_at_or_after(
                    s_present_wait_flip[(n - 1u) & 3u] + 1u);
        }
        s_present_wait_gpu_done[n & 3u] = done;
        s_present_wait_flip[n & 3u] = flip;
    } else if (n % 5u == 4u) {
        wait = s_present_wait_kind == PRESENT_WAIT_EDGE ?
            present_wait_edge_at_or_after(s_now + 1u) - s_now :
            s_present_wait_us;
    }
    ++s_present_wait_count;
    if (wait > s_present_wait_max_us)
        s_present_wait_max_us = wait;
    return wait;
}

static int run_one_tick(
    OracleGame *game, uint32_t full_update_us, uint32_t nonfull_update_us,
    uint32_t render_us, uint32_t interpolation_enabled,
    ScenarioResult *result)
{
    int render;

    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    if (s_test_single_clip)
        kage_vita_fullspeed_scheduler_note_video_service();
#endif
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    s_now += s_test_service_us;
#elif defined(ORACLE_GL_WORKER_CASES)
    s_now += worker_jittered(500u, s_wk_svc_jit_us, 0u);
#else
    s_now += 500u;
#endif
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        game->manager_pointer, game->manager_counter,
        interpolation_enabled, game->game_pointer, game->game_frame);
    ++game->wrapper_ticks_since_present;
#if defined(ORACLE_GL_WORKER_CASES) && \
    defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    if ((game->manager_counter & 1u) == 0u && !s_ab_menu) {
#else
    if ((game->manager_counter & 1u) == 0u) {
#endif
        kage_vita_fullspeed_scheduler_note_game_update_begin(
            game->manager_counter, game->game_pointer, game->game_frame);
        if (result->game_update_time_count >= ORACLE_MAX_GAME_UPDATES)
            return 0;
        result->game_update_times[result->game_update_time_count++] = s_now;
        s_now += full_update_us;
        ++game->game_frame;
        kage_vita_fullspeed_scheduler_note_game_update_end(
            game->game_pointer, game->game_frame);
    } else {
        s_now += nonfull_update_us;
    }
    ++game->manager_counter;
    render = kage_vita_fullspeed_scheduler_plan_render(
        game->manager_pointer, game->manager_counter,
        interpolation_enabled, game->game_pointer, game->game_frame);
    if (render) {
#if defined(ORACLE_GL_WORKER_CASES) && \
    defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
        s_ab_ref_entry = s_now;
#endif
        kage_vita_fullspeed_scheduler_note_render_entry();
        kage_vita_fullspeed_scheduler_note_render_body(
            game->manager_pointer, game->manager_counter,
            interpolation_enabled);
#if defined(ORACLE_GL_WORKER_CASES)
        s_now += worker_jittered(render_us, s_wk_body_jit_us, 1u);
#else
        s_now += render_us;
#endif
#if defined(ORACLE_GL_WORKER_CASES) && \
    defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
        s_ab_ref_body = s_now - s_ab_ref_entry;
#endif
        kage_vita_fullspeed_scheduler_note_present();
        s_now += present_wait_us();
#if defined(ORACLE_GL_WORKER_CASES) && \
    defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
        s_ab_ref_cost = s_now - s_ab_ref_entry;
        s_ab_ref_have = 1u;
#endif
        kage_vita_fullspeed_scheduler_note_render_return();
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
        if (result->present_time_count >= ORACLE_MAX_GAME_UPDATES * 2u)
            return 0;
        result->present_times[result->present_time_count++] = s_now;
#endif
        if (game->wrapper_ticks_since_present >
                result->maximum_wrapper_ticks_between_presents)
            result->maximum_wrapper_ticks_between_presents =
                game->wrapper_ticks_since_present;
        game->wrapper_ticks_since_present = 0u;
    }
    if (kage_vita_fullspeed_scheduler_bypass_limiter())
        ++result->limiter_bypasses;
    else
        ++result->limiter_fallbacks;
    return render;
}

static uint32_t maximum_game_updates_per_second(
    const uint64_t *times, uint32_t count)
{
    uint32_t begin = 0u;
    uint32_t end = 0u;
    uint32_t maximum = 0u;

    while (begin < count) {
        uint64_t limit = times[begin] + 1000000u;
        if (limit < times[begin])
            limit = UINT64_MAX;
        if (end < begin)
            end = begin;
        while (end < count && times[end] < limit)
            ++end;
        if (end - begin > maximum)
            maximum = end - begin;
        ++begin;
    }
    return maximum;
}

/* For every pair of starts i < j: s_j - s_i >= (j - i) * T - cap, in 1/3-us
 * units, allowing the 2 units a wake rounds up to the next microsecond.
 * Every floor is at least one period after the previous one and a start is
 * never before its floor nor more than the cap after the anchor of the next
 * floor, which gives the bound.  It needs an exact sleep and a monotonic
 * clock, so the full-head advance oracle (wake overshoot) skips it. */
static int oracle_start_spacing_holds(const uint64_t *times, uint32_t count)
{
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    (void)times;
    (void)count;
#else
    uint32_t i, j;

    for (i = 0u; i < count; ++i)
        for (j = i + 1u; j < count; ++j) {
            if (times[j] < times[i])
                return 0;
            if (3u * (times[j] - times[i]) +
                    3u * (uint64_t)ORACLE_FLOOR_DEBT_CAP_US + 2u <
                    (uint64_t)(j - i) * KAGE_VITA_FULLSPEED_GAME_TICK_UNITS)
                return 0;
        }
#endif
    return 1;
}

static ScenarioResult run_scenario(
    uint32_t duration_us, uint32_t render_us,
    uint32_t interpolation_enabled, uint32_t stall_at_tick,
    uint32_t stall_us)
{
    ScenarioResult result;
    OracleGame game;
    uint32_t tick = 0u;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    s_now = 0u;
    s_log_calls = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    while (s_now < duration_us) {
        (void)run_one_tick(
            &game, 2500u, 500u, render_us,
            interpolation_enabled, &result);
        ++tick;
        if (tick == stall_at_tick)
            s_now += stall_us;
    }
    result.elapsed_us = s_now;
    result.maximum_game_updates_per_second =
        maximum_game_updates_per_second(
            result.game_update_times, result.game_update_time_count);
    kage_vita_fullspeed_scheduler_snapshot(&result.scheduler);
    return result;
}

#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
/* Stand-in for kage_vita_backend_render_admit (GL render thread one frame
 * behind).  Called only where an interpolated frame would otherwise render.
 * Behind = s_render_admit cleared, or the worker model's last closure not
 * done.  Under ISAAC_VITA_GL_THREAD_ADMIT_WAIT the scheduler calls behind /
 * refused / wait_until (ON) / fit_take instead (kage_vita_backend.c). */
static int s_render_admit = 1;
static uint32_t s_render_admit_calls, s_render_admit_denials;
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
/* GL admit behind stand-ins (kage_vita_backend.c). */
static uint32_t worker_outstanding(void)
{
    uint64_t left;

    if (s_ab_out_model == ORACLE_AB_OUT_FIXED)
        return s_ab_out_fixed;
    if (s_ab_out_model == ORACLE_AB_OUT_OVER)
        return UINT32_MAX;
    left = s_wk_done_last > s_now ? s_wk_done_last - s_now : 0u;
    return left > 1000000u ? UINT32_MAX : (uint32_t)(left * 8u);
}
int kage_vita_backend_admit_behind_gate(uint32_t *outstanding)
{
    ++s_render_admit_calls;
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    s_ab_arm = 0u; /* an admitted Render that never reached Present */
# endif
    if (s_render_admit && !worker_behind_now())
        return 0;
    *outstanding = worker_outstanding();
    return 1;
}
void kage_vita_backend_admit_behind_refused(
    uint64_t present_us, uint64_t return_us, uint64_t head_due_us,
    uint32_t would_admit)
{
    {
        /* Present predicted at gate + the last Render's body, Render return
         * at gate + its cost (both 0 before the first Render). */
        uint64_t body = s_ab_ref_have ? s_ab_ref_body : 0u;
        uint64_t cost = s_ab_ref_have ? s_ab_ref_cost : 0u;

        ++s_ab_ref_checked;
        if (body != cost)
            ++s_ab_ref_split;
        if (!s_ab_ref_bad &&
                (present_us != s_now + body || return_us != s_now + cost))
            s_ab_ref_bad = s_ab_ref_checked;
    }
    ++s_render_admit_denials;
    ++s_wk_rs;
    s_ab_check_armed = 1u;
    s_ab_check_pos = s_wk_posts;
    s_ab_check_present = present_us;
    s_ab_check_return = return_us;
    s_ab_check_head = head_due_us;
    s_ab_check_would = would_admit;
}
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
void kage_vita_backend_admit_behind_admitted(void)
{
    s_ab_arm = 1u;
}
uint32_t kage_vita_backend_admit_behind_hold_take(void)
{
    uint32_t hold_us = s_ab_hold_take_us;
    s_ab_hold_take_us = 0u;
    return hold_us;
}
# endif
void kage_vita_backend_gl_admit_behind_take(
    uint32_t out[KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS])
{
    if (out)
        memcpy(out, s_ab_be, sizeof s_ab_be);
    memset(s_ab_be, 0, sizeof s_ab_be);
}
#elif !(defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT)
int kage_vita_backend_render_admit(void)
{
    ++s_render_admit_calls;
# if defined(ORACLE_GL_ADMIT_BEHIND_TWIN)
    if (s_wk_gate_never_behind)
        return 1;
# endif
    if (s_render_admit && !worker_behind_now())
        return 1;
    ++s_render_admit_denials;
    ++s_wk_rs;
    return 0;
}
#else
int kage_vita_backend_render_behind(void)
{
    ++s_render_admit_calls;
    return !s_render_admit || worker_behind_now();
}
void kage_vita_backend_render_refused(uint64_t latest_us)
{
    ++s_render_admit_denials;
    ++s_wk_rs;
    if (latest_us) {
        s_wk_fit_armed = 1u;
        s_wk_fit_pos = s_wk_posts;
        s_wk_fit_latest = latest_us;
    }
}
# if ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
int kage_vita_backend_render_wait_until(uint64_t deadline_us)
{
    int done;
    if (s_render_admit && !worker_behind_now())
        return 1;
    if (s_render_admit && s_wk_done_last <= deadline_us) {
        s_now = s_wk_done_last + s_wk_wake_us;
        done = 1;
    } else {
        if (deadline_us > s_now)
            s_now = deadline_us;
        done = 0;
    }
    return done;
}
void kage_vita_backend_render_admitted_after_wait(void)
{
    s_wk_waited_arm = 1u;
}
uint32_t kage_vita_backend_admit_hold_take(void)
{
    uint32_t hold_us = s_wk_hold_take_us;
    s_wk_hold_take_us = 0u;
    return hold_us;
}
# endif
void kage_vita_backend_gl_admit_fit_take(uint32_t out[6])
{
    if (out)
        memcpy(out, s_wk_fit, sizeof s_wk_fit);
    memset(s_wk_fit, 0, sizeof s_wk_fit);
}
#endif

static ScenarioResult run_scenario(
    uint32_t duration_us, uint32_t render_us,
    uint32_t interpolation_enabled, uint32_t stall_at_tick,
    uint32_t stall_us);

static int check_render_admission(void)
{
    ScenarioResult admitted, behind;
    const KageVitaFullspeedSnapshot *a, *b;
    uint32_t admitted_calls;

    s_render_admit = 1;
    s_render_admit_calls = s_render_admit_denials = 0u;
    admitted = run_scenario(10000000u, 5000u, 1u, 0u, 0u);
    admitted_calls = s_render_admit_calls;
    s_render_admit = 0;
    s_render_admit_calls = s_render_admit_denials = 0u;
    behind = run_scenario(10000000u, 5000u, 1u, 0u, 0u);
    s_render_admit = 1;
    a = &admitted.scheduler;
    b = &behind.scheduler;
    /* Admitted: asked once per interpolated render, all rendered. */
    CHECK(admitted_calls > 0u);
    CHECK(a->nonfull_phases - a->nonfull_skips == admitted_calls);
    /* Behind: no interpolated frame renders, every refusal counted. */
    CHECK(b->nonfull_phases - b->nonfull_skips == 0u);
    CHECK(s_render_admit_denials == s_render_admit_calls &&
          s_render_admit_calls > 0u);
    CHECK(b->render_skips == b->nonfull_skips + b->late_full_skips);
    /* The update cadence is not hurt and full phases are not skipped more. */
    CHECK(b->game_updates >= a->game_updates);
    CHECK(b->late_full_skips <= a->late_full_skips);
    CHECK(behind.maximum_game_updates_per_second <=
          ORACLE_MAX_UPDATES_PER_SECOND); /* as check_scenario */
    printf("Vita GL render admission: PASS (admitted %u interpolated renders; "
           "behind: %u refused, %u vs %u Game updates)\n",
           (unsigned)admitted_calls, (unsigned)s_render_admit_denials,
           (unsigned)b->game_updates, (unsigned)a->game_updates);
    return 1;
}

/* GL admit wait worker cases (R60-DRYADMIT-SPEC-20261028.md section 6).
 * 600 warm-up loops, then 10 windows of 120 loops; non-full work 600 us.
 * Every printed field is pinned per mode: OFF and OBSERVE share the stock
 * numbers (OBSERVE never changes behaviour), ON has its own, and the
 * ph120.aw counters exist only under the knob.  ORACLE_GL_WORKER_CASES (at
 * the worker model) selects the proven combination. */
#if defined(ORACLE_GL_WORKER_CASES)
# if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
#  define ORACLE_GL_ADMIT_WAIT_MODE "on"
# elif defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
#  define ORACLE_GL_ADMIT_WAIT_MODE "observe"
# else
#  define ORACLE_GL_ADMIT_WAIT_MODE "off"
# endif
typedef struct WorkerCase {
    const char *name;
    uint32_t kind, period, tail_us, base_us, gpu_us, edge_us;
    uint32_t full_us, body_us, stall_at, stall_us, wake_us;
    uint32_t svc_jit_us, body_jit_us, tail_one_in;
} WorkerCase;

/* One mode's printed numbers: presents (window min..max), non-full skips,
 * GL refusals (ph120.rw rs), Present holds n/us, update gaps, the 1-s
 * update maximum, wall, update span and the per-window/update digest.
 * Every case also has g = 600 updates, lskip = 0, posts = presents,
 * dis = seq = 0. */
typedef struct WorkerExpect {
    uint32_t presents, pmin, pmax, nonfull_skips, refused, holds, mingap, ups1s;
    uint64_t hold_us, wall, span, maxgap, digest;
} WorkerExpect;

/* stock = OFF and OBSERVE.  observe = OBSERVE nw, fit(e,a), miss(us,max)
 * (its aw is n = stock.refused, those five, everything else 0).  on_aw = n,
 * ok, to, nw, bo, tt, us(sum,max), lt(c,us), hl(c,us), ov, fit(e,a),
 * miss(us,max), hd(n,us).  Pinned from print-only runs of this oracle
 * (ORACLE_GL_WORKER_PRINT_ONLY, clang 2026-10-28, review fix pass); the
 * first 26 rows equal the spec's table in every field the spec has, except
 * where R60-DRYADMIT-20261028.md "Review" lists a move. */
typedef struct WorkerCaseExpect {
    const char *name;
    WorkerExpect stock;
    uint32_t observe[5];
    WorkerExpect on;
    uint32_t on_aw[19];
} WorkerCaseExpect;

static const WorkerCaseExpect k_worker_expect[] = {
    { "water-idle",
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t8",
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t9",
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t10",
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t11",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 71200u, 594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t12",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 191200u, 1594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t13",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 311200u, 2594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t14",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 431200u, 3594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t15",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 551200u, 4594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t16",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 671200u, 5594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "lda-t11-wake",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 95200u, 794u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t11",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 413200u, 3444u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t13",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 653200u, 5444u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t14",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 773200u, 6444u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t15",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 893200u, 7444u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t16",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 1013200u, 8444u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t17",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 0u, 93600u, 468u },
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 200u, 0u, 200u, 0u, 0u, 0u, 1795000u, 8975u, 0u, 0u, 0u, 0u, 0u, 200u, 0u, 93600u, 468u, 0u, 0u } },
    { "stress-all",
      { 600u, 60u, 60u, 600u, 600u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xa576f0e4d5b37493ull },
      { 0u, 600u, 600u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 600u, 600u, 0u, 0u, 0u, 0u, 356000u, 594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "every3-11",
      { 900u, 90u, 90u, 300u, 300u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x6bef4383ac7c8023ull },
      { 0u, 300u, 300u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 200u, 200u, 0u, 0u, 0u, 0u, 118800u, 594u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "eid-fight",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 0u, 439600u, 2198u },
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 200u, 0u, 200u, 0u, 0u, 0u, 1185000u, 5925u, 0u, 0u, 0u, 0u, 0u, 200u, 0u, 439600u, 2198u, 0u, 0u } },
    { "stall-266ms",
      { 999u, 99u, 100u, 201u, 200u, 1u, 33333u, 30u, 232666ull, 20212293ull, 20174660ull, 241126ull, 0x3a19c87ebe13ee45ull },
      { 1u, 200u, 199u, 244327u, 244327u },
      { 1197u, 117u, 120u, 3u, 2u, 1u, 33333u, 30u, 232666ull, 20207993ull, 20174660ull, 241126ull, 0x7c410d40709915c7ull },
      { 121u, 119u, 1u, 1u, 0u, 0u, 81872u, 11266u, 0u, 0u, 0u, 0u, 0u, 1u, 0u, 244327u, 244327u, 0u, 0u } },
    { "gpuq-19-fight",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1026u, 101u, 103u, 174u, 174u, 11u, 33333u, 30u, 34668ull, 20000200ull, 19966867ull, 33334ull, 0x4b9c7b3e38fbd6cfull },
      { 383u, 209u, 55u, 22u, 97u, 0u, 585456u, 2833u, 0u, 0u, 11u, 41752u, 0u, 152u, 97u, 873950u, 15984u, 11u, 34668u } },
    { "gpuq-19-light",
      { 1053u, 105u, 106u, 147u, 147u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x369ca26395bc1524ull },
      { 0u, 147u, 147u, 0u, 0u },
      { 1052u, 104u, 106u, 148u, 148u, 11u, 33333u, 30u, 144221ull, 19997400ull, 19966867ull, 33334ull, 0xc0d521cc2c7dc809ull },
      { 369u, 221u, 60u, 22u, 66u, 0u, 1272750u, 11267u, 0u, 0u, 11u, 41855u, 0u, 126u, 66u, 486050u, 8190u, 11u, 144221u } },
    { "gpuq-11-water",
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "gpuq-5-dry",
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t14-svcj",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20057772ull, 20024565ull, 33902ull, 0x3cdd05a72d623092ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20057772ull, 20024565ull, 33902ull, 0x72bdfe653bb01beaull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 769521u, 7028u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t16-svcj",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20057772ull, 20024565ull, 33902ull, 0x3cdd05a72d623092ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20057772ull, 20024565ull, 33902ull, 0x72bdfe653bb01beaull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 1009521u, 9028u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t11-rjit",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 187u, 2390u, 430u },
      { 1191u, 118u, 120u, 9u, 9u, 0u, 33333u, 30u, 0ull, 19996353ull, 19966867ull, 33334ull, 0x7e933abdc3b0273eull },
      { 123u, 114u, 9u, 0u, 0u, 0u, 420069u, 6180u, 0u, 0u, 0u, 0u, 0u, 9u, 0u, 1704u, 419u, 0u, 0u } },
    { "ldc-t14-rjit",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 119u, 133987u, 3430u },
      { 1125u, 107u, 117u, 75u, 75u, 0u, 33333u, 30u, 0ull, 20002210ull, 19968465ull, 34214ull, 0xba54f7fa92637a36ull },
      { 150u, 75u, 46u, 0u, 29u, 0u, 712873u, 7703u, 3u, 1600u, 0u, 0u, 0u, 75u, 17u, 96786u, 3430u, 0u, 0u } },
    { "ldc-t16-rjit",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 57u, 354673u, 5430u },
      { 1057u, 102u, 110u, 143u, 143u, 4u, 33333u, 30u, 4826ull, 20003062ull, 19969120ull, 34329ull, 0x12a93a336c40bee0ull },
      { 177u, 34u, 89u, 0u, 54u, 0u, 839449u, 8579u, 5u, 2255u, 0u, 0u, 0u, 142u, 17u, 302145u, 5429u, 0u, 0u } },
    { "ldc-rtail14",
      { 1087u, 105u, 114u, 113u, 113u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x24607d87924260d8ull },
      { 0u, 113u, 113u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 24u, 33333u, 30u, 30148ull, 20006643ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 113u, 113u, 0u, 0u, 0u, 0u, 735630u, 7700u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 24u, 30148u } },
    { "ldc-rtail16",
      { 1087u, 105u, 114u, 113u, 113u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x24607d87924260d8ull },
      { 0u, 113u, 113u, 0u, 0u },
      { 1189u, 116u, 120u, 11u, 11u, 23u, 33333u, 30u, 120895ull, 20000200ull, 19966867ull, 33334ull, 0x1cfc47559fd93586ull },
      { 117u, 106u, 11u, 0u, 0u, 0u, 935913u, 8444u, 0u, 0u, 0u, 0u, 0u, 11u, 0u, 78237u, 9980u, 23u, 120895u } },
    { "ldc-rtail14-w1",
      { 1087u, 105u, 114u, 113u, 113u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x24607d87924260d8ull },
      { 0u, 113u, 113u, 0u, 0u },
      { 1189u, 116u, 120u, 11u, 5u, 23u, 33333u, 30u, 51895ull, 20000200ull, 19966867ull, 33334ull, 0x1cfc47559fd93586ull },
      { 117u, 106u, 5u, 0u, 0u, 6u, 867253u, 7444u, 0u, 0u, 0u, 0u, 0u, 5u, 0u, 9897u, 1980u, 23u, 51895u } },
    { "ldc-mix14",
      { 1087u, 105u, 114u, 113u, 113u, 0u, 33333u, 30u, 0ull, 20053334ull, 20024565ull, 33902ull, 0xb5015a847d897a35ull },
      { 0u, 113u, 69u, 85917u, 3427u },
      { 1146u, 111u, 118u, 54u, 54u, 5u, 33333u, 30u, 9393ull, 20057683ull, 20025478ull, 34596ull, 0x86d4fb3a1f122862ull },
      { 105u, 51u, 44u, 0u, 10u, 0u, 567622u, 7883u, 2u, 1407u, 0u, 0u, 0u, 54u, 7u, 83243u, 3588u, 5u, 9393u } },
    { "ldc-t14-wake2",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1200u, 120u, 120u, 0u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0x92b6db943818f7ebull },
      { 120u, 120u, 0u, 0u, 0u, 0u, 1013200u, 8444u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "ldc-t16-wake1",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1000u, 100u, 100u, 200u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 200u, 0u, 0u, 0u, 0u, 200u, 1888600u, 9443u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "gpuq-19-heavy",
      { 600u, 60u, 60u, 600u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xa576f0e4d5b37493ull },
      { 0u, 0u, 0u, 0u, 0u },
      { 600u, 60u, 60u, 600u, 0u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xa576f0e4d5b37493ull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
    { "gpuq-19-mid",
      { 1000u, 100u, 100u, 200u, 200u, 0u, 33333u, 30u, 0ull, 20000200ull, 19966867ull, 33334ull, 0xdfe62d1a9133b8a3ull },
      { 0u, 200u, 200u, 0u, 0u },
      { 1021u, 101u, 103u, 179u, 179u, 13u, 33333u, 30u, 51656ull, 20002800ull, 19966867ull, 33334ull, 0x2a4b01db4295af04ull },
      { 338u, 159u, 35u, 13u, 131u, 0u, 693471u, 7567u, 0u, 0u, 13u, 49498u, 0u, 166u, 131u, 412720u, 11899u, 13u, 51656u } },
    { "gpuq-19-ljit",
      { 1056u, 105u, 106u, 144u, 144u, 45u, 33333u, 30u, 241964ull, 20063301ull, 20024565ull, 33902ull, 0x61260591d46548eaull },
      { 0u, 145u, 87u, 345206u, 14741u },
      { 1056u, 105u, 106u, 144u, 144u, 81u, 33333u, 30u, 362514ull, 20058067ull, 20024618ull, 33902ull, 0xaf190c912a1b8a89ull },
      { 263u, 119u, 89u, 12u, 43u, 0u, 1215110u, 12401u, 1u, 53u, 6u, 29390u, 0u, 132u, 41u, 711160u, 16004u, 37u, 120689u } },
    { "render-bound",
      { 600u, 60u, 60u, 600u, 0u, 0u, 39160u, 26u, 0ull, 23496000ull, 23456840ull, 39160ull, 0x9e8dabc6e4755943ull },
      { 0u, 0u, 0u, 0u, 0u },
      { 600u, 60u, 60u, 600u, 0u, 0u, 39160u, 26u, 0ull, 23496000ull, 23456840ull, 39160ull, 0x9e8dabc6e4755943ull },
      { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } },
};

static void worker_reset(void)
{
    s_wk_done_last = 0u;
    s_wk_posts = s_wk_holds = s_wk_rs = 0u;
    s_wk_hold_us = 0u;
    s_wk_fit_armed = s_wk_fit_pos = 0u;
    s_wk_fit_latest = 0u;
    memset(s_wk_fit, 0, sizeof s_wk_fit);
    s_wk_waited_arm = s_wk_waited_pos = s_wk_hold_take_us = 0u;
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    s_ab_check_armed = s_ab_check_pos = s_ab_check_would = 0u;
    s_ab_check_present = s_ab_check_return = s_ab_check_head = 0u;
    memset(s_ab_be, 0, sizeof s_ab_be);
    /* Every caller resets the scheduler next, which forgets its last
     * Render too. */
    s_ab_ref_entry = s_ab_ref_body = s_ab_ref_cost = 0u;
    s_ab_ref_have = s_ab_ref_bad = 0u;
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    s_ab_arm = s_ab_chain = s_ab_hold_take_us = 0u;
# endif
#endif
}

/* The printed-line label of a worker pass ("worker": the pinned stock pass). */
static const char *s_wk_label = "worker";

#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
/* Per-tick ph120.ab accounting: take the scheduler's counters after every
 * loop, add them up (s_ab_total) and hold each tick against a shadow of the
 * back-off written from the knob's contract, not from the scheduler's code:
 * a head that reports hl arms 30; every non-full phase from that tick on is
 * inside the back-off while the count is non-zero, then spends one. */
static KageVitaFullspeedGlAdmitBehindSnapshot s_ab_total;
static uint32_t s_ab_shadow;

static int ab_account(uint32_t nonfull_phases)
{
    KageVitaFullspeedGlAdmitBehindSnapshot d;
    uint32_t live = 0u;

    kage_vita_fullspeed_scheduler_gl_admit_behind_take(&d);
    CHECK(s_ab_ref_bad == 0u);
    s_ab_total.gates += d.gates;
    s_ab_total.test_pass += d.test_pass;
    s_ab_total.test_fail += d.test_fail;
    s_ab_total.test_off += d.test_off;
    s_ab_total.over += d.over;
    s_ab_total.would += d.would;
    s_ab_total.admitted += d.admitted;
    s_ab_total.backoff += d.backoff;
    s_ab_total.time_test += d.time_test;
    s_ab_total.hold_late += d.hold_late;
    s_ab_total.hold_late_us += d.hold_late_us;
    if (d.out_max > s_ab_total.out_max)
        s_ab_total.out_max = d.out_max;
    CHECK(nonfull_phases <= 1u && d.gates <= nonfull_phases);
    CHECK(d.out_count == d.gates);
    CHECK(d.test_pass + d.test_fail + d.test_off == d.gates);
    CHECK(d.over + d.would <= d.gates);
    if (d.gates) {
        uint32_t within = d.out[0] <=
            kage_vita_fullspeed_scheduler_gl_admit_behind_bytes();
        CHECK(d.out_max == d.out[0]);
        CHECK(d.over == !within);
        CHECK(d.would == (within && !d.test_fail));
    }
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    CHECK(d.hold_late <= 1u);
    if (d.hold_late)
        s_ab_shadow = KAGE_VITA_GL_ADMIT_BEHIND_BACKOFF;
    if (nonfull_phases) {
        live = s_ab_shadow != 0u;
        if (s_ab_shadow)
            --s_ab_shadow;
    }
    if (d.gates && d.over) {
        CHECK(d.admitted == 0u && d.backoff == 0u && d.time_test == 0u);
    } else if (d.gates && live) {
        CHECK(d.backoff == 1u && d.admitted == 0u && d.time_test == 0u);
    } else if (d.gates) {
        /* Within the bound, outside a back-off: the stock time test alone
         * decides, and the gate's own copy of it agrees with it. */
        CHECK(d.backoff == 0u && d.admitted + d.time_test == 1u);
        CHECK(d.time_test == d.test_fail);
    }
# else
    (void)live;
    CHECK(d.admitted == 0u && d.backoff == 0u && d.time_test == 0u &&
          d.hold_late == 0u && d.hold_late_us == 0u);
# endif
    return 1;
}
#endif

static int run_worker_case(const WorkerCase *c, const WorkerCaseExpect *x,
                           uint32_t *presents_out, uint32_t *refused_out)
{
    static ScenarioResult result;
    OracleGame game;
    KageVitaFullspeedSnapshot before, after;
    uint32_t tick, window, first, i, pmin = UINT32_MAX, pmax = 0u;
    uint32_t rs0, holds0, posts0, ups1s;
    uint64_t hold_us0, start, span, digest = 1469598103934665603ull, mingap = UINT64_MAX, maxgap = 0u;
    const WorkerExpect *e;
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    KageVitaFullspeedGlAdmitWaitSnapshot aw;
    uint32_t fit[6], got[19], want[19];
#endif

#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
    e = x ? &x->on : NULL;
#else
    e = x ? &x->stock : NULL;
#endif
    if (x) {
        CHECK(strcmp(c->name, x->name) == 0);
    }
    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    game.game_frame = 100u;
    s_now = 1000u;
    s_delay_fail = 0u;
    s_render_admit = 1;
    s_present_wait_kind = c->kind;
    s_present_wait_edge_us = c->edge_us;
    s_present_wait_count = 0u;
    memset(s_present_wait_gpu_done, 0, sizeof s_present_wait_gpu_done);
    memset(s_present_wait_flip, 0, sizeof s_present_wait_flip);
    s_wk_period = c->period;
    s_wk_tail_us = c->tail_us;
    s_wk_base_us = c->base_us;
    s_wk_gpu_us = c->gpu_us;
    s_wk_stall_at = c->stall_us ? c->stall_at : UINT32_MAX;
    s_wk_stall_us = c->stall_us;
    s_wk_wake_us = c->wake_us;
    s_wk_svc_jit_us = c->svc_jit_us;
    s_wk_body_jit_us = c->body_jit_us;
    s_wk_tail_one_in = c->tail_one_in;
    s_wk_rng[0] = 0x9e3779b9u;
    s_wk_rng[1] = 0x85ebca6bu;
    s_wk_rng[2] = 0xc2b2ae35u;
    worker_reset();
    kage_vita_fullspeed_scheduler_reset();
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    s_ab_shadow = 0u;
    kage_vita_fullspeed_scheduler_gl_admit_behind_take(NULL);
#endif
    for (tick = 0u; tick < 600u; ++tick) {
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
        KageVitaFullspeedSnapshot tb, ta;
        kage_vita_fullspeed_scheduler_snapshot(&tb);
#endif
        (void)run_one_tick(&game, c->full_us, 600u, c->body_us, 1u, &result);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
        kage_vita_fullspeed_scheduler_snapshot(&ta);
        if (!ab_account(ta.nonfull_phases - tb.nonfull_phases))
            return 0;
#endif
    }
    first = result.game_update_time_count;
    rs0 = s_wk_rs; holds0 = s_wk_holds; hold_us0 = s_wk_hold_us; posts0 = s_wk_posts;
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    memset(&s_ab_total, 0, sizeof s_ab_total);
    kage_vita_backend_gl_admit_behind_take(NULL);
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    kage_vita_fullspeed_scheduler_gl_admit_wait_take(NULL);
    kage_vita_backend_gl_admit_fit_take(fit);
#endif
    kage_vita_fullspeed_scheduler_snapshot(&before);
    start = s_now;
    for (window = 0u; window < 10u; ++window) {
        KageVitaFullspeedSnapshot wb, wa;
        uint32_t p;
        kage_vita_fullspeed_scheduler_snapshot(&wb);
        for (tick = 0u; tick < 120u; ++tick) {
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
            KageVitaFullspeedSnapshot tb, ta;
            kage_vita_fullspeed_scheduler_snapshot(&tb);
#endif
            (void)run_one_tick(&game, c->full_us, 600u, c->body_us, 1u, &result);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
            kage_vita_fullspeed_scheduler_snapshot(&ta);
            if (!ab_account(ta.nonfull_phases - tb.nonfull_phases))
                return 0;
#endif
        }
        kage_vita_fullspeed_scheduler_snapshot(&wa);
        p = wa.presents - wb.presents;
        if (p < pmin) pmin = p;
        if (p > pmax) pmax = p;
        digest = (digest ^ p) * 1099511628211ull;
    }
    kage_vita_fullspeed_scheduler_snapshot(&after);
    CHECK(result.game_update_time_count > first);
    for (i = first; i < result.game_update_time_count; ++i) {
        digest = (digest ^ result.game_update_times[i]) * 1099511628211ull;
        if (i > first && result.game_update_times[i] -
                result.game_update_times[i - 1u] < mingap)
            mingap = result.game_update_times[i] - result.game_update_times[i - 1u];
        if (i > first && result.game_update_times[i] -
                result.game_update_times[i - 1u] > maxgap)
            maxgap = result.game_update_times[i] - result.game_update_times[i - 1u];
    }
    span = result.game_update_times[result.game_update_time_count - 1u] -
        result.game_update_times[first];
    ups1s = maximum_game_updates_per_second(
        result.game_update_times + first, result.game_update_time_count - first);
    printf("span=%llu maxgap=%llu ", (unsigned long long)span,
           (unsigned long long)maxgap);
    printf("%s %-14s p=%u (%u..%u) g=%u wall=%llu ups=%.3f nskip=%u lskip=%u rs=%u "
           "holds=%u/%llu posts=%u mingap=%llu dis=%u seq=%u ups1s=%u digest=%016llx",
           s_wk_label, c->name, (unsigned)(after.presents - before.presents),
           (unsigned)pmin, (unsigned)pmax,
           (unsigned)(after.game_updates - before.game_updates),
           (unsigned long long)(s_now - start),
           (after.game_updates - before.game_updates) * 1e6 / (double)(s_now - start),
           (unsigned)(after.nonfull_skips - before.nonfull_skips),
           (unsigned)(after.late_full_skips - before.late_full_skips),
           (unsigned)(s_wk_rs - rs0), (unsigned)(s_wk_holds - holds0),
           (unsigned long long)(s_wk_hold_us - hold_us0),
           (unsigned)(s_wk_posts - posts0),
           (unsigned long long)mingap,
           (unsigned)after.runtime_disables, (unsigned)after.sequence_violations,
           (unsigned)ups1s, (unsigned long long)digest);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    kage_vita_fullspeed_scheduler_gl_admit_wait_take(&aw);
    kage_vita_backend_gl_admit_fit_take(fit);
    printf(" | n=%u ok=%u to=%u nw=%u bo=%u tt=%u us=%u,%u lt=%u,%u hl=%u,%u ov=%u "
           "fit=%u,%u miss=%u,%u hd=%u,%u",
           aw.gates, aw.admitted, aw.timeouts, aw.no_window, aw.backoff,
           aw.time_test, aw.wait_us, aw.wait_max_us, aw.late, aw.late_us,
           aw.hold_late, aw.hold_late_us, aw.timeout_over_max_us,
           fit[0], fit[1], fit[2], fit[3], fit[4], fit[5]);
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
    {
        uint32_t be[KAGE_VITA_BACKEND_ADMIT_BEHIND_WORDS];
        const KageVitaFullspeedGlAdmitBehindSnapshot *t = &s_ab_total;

        kage_vita_backend_gl_admit_behind_take(be);
        printf(" | n=%u st=%u,%u,%u ov=%u wa=%u ok=%u bo=%u tt=%u obmax=%u hl=%u,%u "
               "pm=%u,%u,%u,%u wp=%u,%u,%u,%u phl=%u hd=%u,%u\n",
               t->gates, t->test_pass, t->test_fail, t->test_off, t->over,
               t->would, t->admitted, t->backoff, t->time_test, t->out_max,
               t->hold_late, t->hold_late_us, be[0], be[1], be[2], be[3],
               be[4], be[5], be[6], be[7], be[8], be[9], be[10]);
        /* rs (ph120.rw) = n - ok in both modes; the per-tick identities are
         * ab_account's.  pm: at most one check per refusal, plus one armed
         * before the window; wp and ok are subsets. */
        CHECK(s_wk_rs - rs0 == t->gates - t->admitted);
        CHECK(be[0] <= s_wk_rs - rs0 + 1u && be[1] <= be[0] &&
              be[4] <= be[0] && be[5] <= be[4] && be[8] <= be[0] - be[1]);
        if (s_ab_out_model == ORACLE_AB_OUT_OVER)
            CHECK(t->over == t->gates && t->would == 0u && be[4] == 0u);
        if (s_ab_out_model == ORACLE_AB_OUT_FIXED && s_ab_out_fixed == 0u)
            CHECK(t->over == 0u && t->out_max == 0u);
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
        CHECK(t->gates == t->over + t->backoff + t->admitted + t->time_test);
        CHECK(t->hold_late <= be[9] + 1u);
# else
        CHECK(be[9] == 0u && be[10] == 0u);
# endif
    }
#else
    printf("\n");
#endif
    s_present_wait_kind = PRESENT_WAIT_NONE;
    s_wk_stall_at = UINT32_MAX;
    s_wk_stall_us = 0u;
    s_wk_svc_jit_us = s_wk_body_jit_us = s_wk_tail_one_in = 0u;
    if (!x) {
        /* Unpinned pass (print-only re-pinning, or a GL admit behind pass
         * whose numbers a cross-build comparison holds instead). */
        *presents_out += after.presents - before.presents;
        *refused_out += s_wk_rs - rs0;
        worker_reset();
        return 1;
    }

    CHECK(after.presents - before.presents == e->presents);
    CHECK(pmin == e->pmin && pmax == e->pmax);
    CHECK(after.game_updates - before.game_updates == 600u);
    CHECK(s_now - start == e->wall);
    CHECK(after.nonfull_skips - before.nonfull_skips == e->nonfull_skips);
    CHECK(after.late_full_skips == before.late_full_skips);
    CHECK(s_wk_rs - rs0 == e->refused);
    CHECK(s_wk_holds - holds0 == e->holds && s_wk_hold_us - hold_us0 == e->hold_us);
    CHECK(s_wk_posts - posts0 == e->presents);
    CHECK(mingap == e->mingap && maxgap == e->maxgap && span == e->span);
    CHECK(after.runtime_disables == 0u && after.sequence_violations == 0u);
    CHECK(ups1s == e->ups1s);
    CHECK(digest == e->digest);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
    got[0] = aw.gates; got[1] = aw.admitted; got[2] = aw.timeouts;
    got[3] = aw.no_window; got[4] = aw.backoff; got[5] = aw.time_test;
    got[6] = aw.wait_us; got[7] = aw.wait_max_us; got[8] = aw.late;
    got[9] = aw.late_us; got[10] = aw.hold_late; got[11] = aw.hold_late_us;
    got[12] = aw.timeout_over_max_us;
    for (i = 0u; i < 6u; ++i)
        got[13u + i] = fit[i];
# if ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
    memcpy(want, x->on_aw, sizeof want);
# else
    memset(want, 0, sizeof want);
    want[0] = x->stock.refused;
    want[3] = x->observe[0];
    memcpy(want + 13, x->observe + 1, 4u * sizeof x->observe[0]);
# endif
    for (i = 0u; i < 19u; ++i) {
        if (got[i] != want[i])
            fprintf(stderr, "worker %s aw field %u: %u != %u\n",
                    c->name, (unsigned)i, (unsigned)got[i], (unsigned)want[i]);
        CHECK(got[i] == want[i]);
    }
    /* I1: rw rs == n - ok - tt.  I2: n >= ok + to + nw + bo + tt (ON:
     * equality).  I3: fit e <= n - ok - tt - nw + 1 (one arm carried over
     * the take edge).  I4 (OBSERVE): it never waits. */
    CHECK(s_wk_rs - rs0 == aw.gates - aw.admitted - aw.time_test);
    CHECK(aw.gates >= aw.admitted + aw.timeouts + aw.no_window +
          aw.backoff + aw.time_test);
    CHECK(fit[0] <= aw.gates - aw.admitted - aw.time_test - aw.no_window + 1u);
# if ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
    CHECK(aw.gates == aw.admitted + aw.timeouts + aw.no_window +
          aw.backoff + aw.time_test);
# else
    CHECK(aw.admitted == 0u && aw.timeouts == 0u && aw.backoff == 0u &&
          aw.time_test == 0u && aw.late == 0u && aw.wait_us == 0u &&
          aw.hold_late == 0u && aw.timeout_over_max_us == 0u &&
          fit[4] == 0u && fit[5] == 0u);
# endif
#endif
    *presents_out += after.presents - before.presents;
    *refused_out += s_wk_rs - rs0;
    worker_reset();
    return 1;
}

static const WorkerCase k_worker_cases[] = {
    /* name, kind, period, tail, base, gpu, edge, full, body, stall_at, stall_us, wake,
     * svc_jit, body_jit, tail_one_in */
    { "water-idle",    PRESENT_WAIT_WORKER, 0u, 0u, 300u, 0u, 0u, 3130u, 6110u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t8",        PRESENT_WAIT_WORKER, 5u, 8000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t9",        PRESENT_WAIT_WORKER, 5u, 9000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t10",       PRESENT_WAIT_WORKER, 5u, 10000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t11",       PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t12",       PRESENT_WAIT_WORKER, 5u, 12000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t13",       PRESENT_WAIT_WORKER, 5u, 13000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t14",       PRESENT_WAIT_WORKER, 5u, 14000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t15",       PRESENT_WAIT_WORKER, 5u, 15000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t16",       PRESENT_WAIT_WORKER, 5u, 16000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "lda-t11-wake",  PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 200u, 0u, 0u, 0u },
    { "ldc-t11",       PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "ldc-t13",       PRESENT_WAIT_WORKER, 5u, 13000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "ldc-t14",       PRESENT_WAIT_WORKER, 5u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "ldc-t15",       PRESENT_WAIT_WORKER, 5u, 15000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "ldc-t16",       PRESENT_WAIT_WORKER, 5u, 16000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "ldc-t17",       PRESENT_WAIT_WORKER, 5u, 17000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "stress-all",    PRESENT_WAIT_WORKER, 1u, 11000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "every3-11",     PRESENT_WAIT_WORKER, 3u, 11000u, 300u, 0u, 0u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "eid-fight",     PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 4750u, 9640u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "stall-266ms",   PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 2560u, 4300u, 1200u, 266000u, 0u, 0u, 0u, 0u },
    { "gpuq-19-fight", PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 19000u, 5000u, 4720u, 12090u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "gpuq-19-light", PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 19000u, 5000u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "gpuq-11-water", PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 11300u, 5000u, 3130u, 6110u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "gpuq-5-dry",    PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 5400u, 5000u, 2560u, 4300u, 0u, 0u, 0u, 0u, 0u, 0u },
    /* Review fix pass (R60-DRYADMIT-20261028.md "Review"): seeded service
     * jitter (svcj, round 1 finding 2), render-body jitter (rjit, round 2
     * finding 2), random tails (rtail, round 2 finding 3), cross-core wake
     * (wake), and GPU-queue rooms with heavier CPU (round 2 finding 1:
     * heavy = LDC P4 U 15.12 + R 11.15 ms, analyze.txt). */
    { "ldc-t14-svcj",    PRESENT_WAIT_WORKER, 5u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 300u, 0u, 0u },
    { "ldc-t16-svcj",    PRESENT_WAIT_WORKER, 5u, 16000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 300u, 0u, 0u },
    { "ldc-t11-rjit",    PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 3000u, 0u },
    { "ldc-t14-rjit",    PRESENT_WAIT_WORKER, 5u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 3000u, 0u },
    { "ldc-t16-rjit",    PRESENT_WAIT_WORKER, 5u, 16000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 3000u, 0u },
    { "ldc-rtail14",     PRESENT_WAIT_WORKER, 0u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 5u },
    { "ldc-rtail16",     PRESENT_WAIT_WORKER, 0u, 16000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 0u, 0u, 5u },
    { "ldc-rtail14-w1",  PRESENT_WAIT_WORKER, 0u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 1000u, 0u, 0u, 5u },
    { "ldc-mix14",       PRESENT_WAIT_WORKER, 0u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 0u, 300u, 3000u, 5u },
    { "ldc-t14-wake2",   PRESENT_WAIT_WORKER, 5u, 14000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 2000u, 0u, 0u, 0u },
    { "ldc-t16-wake1",   PRESENT_WAIT_WORKER, 5u, 16000u, 300u, 0u, 0u, 3120u, 6590u, 0u, 0u, 1000u, 0u, 0u, 0u },
    { "gpuq-19-heavy",   PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 19000u, 5000u, 15120u, 11150u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "gpuq-19-mid",     PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 19000u, 5000u, 8000u, 8000u, 0u, 0u, 0u, 0u, 0u, 0u },
    { "gpuq-19-ljit",    PRESENT_WAIT_WORKER_QUEUE, 0u, 0u, 300u, 19000u, 5000u, 2560u, 4300u, 0u, 0u, 0u, 300u, 3000u, 0u },
    { "render-bound",  PRESENT_WAIT_WORKER, 5u, 11000u, 300u, 0u, 0u, 2560u, 35000u, 0u, 0u, 0u, 0u, 0u, 0u },
};


static int check_worker_cases(void)
{
    uint32_t i, presents = 0u, refused = 0u;
    const uint32_t count = (uint32_t)(sizeof k_worker_cases / sizeof k_worker_cases[0]);

#if defined(ORACLE_GL_WORKER_PRINT_ONLY)
    /* Re-pinning aid: print every case with nothing checked, then stop. */
    (void)k_worker_expect;
    for (i = 0u; i < count; ++i)
        (void)run_worker_case(&k_worker_cases[i], NULL, &presents, &refused);
    printf("Vita GL admit wait (" ORACLE_GL_ADMIT_WAIT_MODE "): PRINT ONLY (%u "
           "worker cases)\n", (unsigned)count);
    return 0;
#endif
    CHECK(count == 40u &&
          sizeof k_worker_expect / sizeof k_worker_expect[0] == count);
    for (i = 0u; i < count; ++i) {
        if (!run_worker_case(&k_worker_cases[i], &k_worker_expect[i],
                             &presents, &refused))
            return 0;
    }
    printf("Vita GL admit wait (" ORACLE_GL_ADMIT_WAIT_MODE "): PASS (%u worker cases, "
           "every printed field pinned; presents=%u refused=%u over 10x120 loops "
           "per case, 600 updates each)\n",
           (unsigned)count, (unsigned)presents, (unsigned)refused);
    return 1;
}

#if (defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) || \
    defined(ORACLE_GL_ADMIT_BEHIND_TWIN)
/* One more pass over the 40 worker cases under another label.  pinned: every
 * printed field against the stock numbers (k_worker_expect[].stock). */
static int run_worker_pass(const char *label, int pinned,
                           uint32_t *presents, uint32_t *refused)
{
    const uint32_t count = (uint32_t)(sizeof k_worker_cases / sizeof k_worker_cases[0]);
    uint32_t i;

    s_wk_label = label;
    for (i = 0u; i < count; ++i) {
        if (!run_worker_case(&k_worker_cases[i],
                             pinned ? &k_worker_expect[i] : NULL,
                             presents, refused))
            return 0;
    }
    s_wk_label = "worker";
    return 1;
}
#endif

#if defined(ORACLE_GL_ADMIT_BEHIND_TWIN) && \
    !(defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND)
/* Knob OFF, the gate never told "behind": what stock does with a frame it
 * admits.  The ON "within" pass must print the same numbers wherever its
 * back-off never armed (test_kage_vita_fullspeed_scheduler.py compares). */
static int check_admit_behind_cases(void)
{
    uint32_t presents = 0u, refused = 0u;

    s_wk_gate_never_behind = 1u;
    if (!run_worker_pass("within", 0, &presents, &refused))
        return 0;
    s_wk_gate_never_behind = 0u;
    printf("Vita GL admit behind (twin): 40 cases with the stock gate never "
           "behind; presents=%u refused=%u\n",
           (unsigned)presents, (unsigned)refused);
    return 1;
}
#endif

#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
typedef struct AbDirected {
    OracleGame game;
    uint32_t nonfull, gates, over, ok, bo, tt, hl, hl_us;
} AbDirected;

/* One loop, light work (update 3000, non-full 600, Render 4000 us, no
 * Present wait), the gate forced behind; adds the tick's counters to *a and
 * returns them in *d. */
static int ab_directed_tick(AbDirected *a, ScenarioResult *result,
                            KageVitaFullspeedGlAdmitBehindSnapshot *d,
                            uint32_t *nonfull)
{
    KageVitaFullspeedSnapshot before, after;

    kage_vita_fullspeed_scheduler_snapshot(&before);
    (void)run_one_tick(&a->game, 3000u, 600u, 4000u, 1u, result);
    kage_vita_fullspeed_scheduler_snapshot(&after);
    kage_vita_fullspeed_scheduler_gl_admit_behind_take(d);
    *nonfull = after.nonfull_phases - before.nonfull_phases;
    CHECK(*nonfull <= 1u && d->gates <= *nonfull);
    CHECK(after.runtime_disables == 0u && after.sequence_violations == 0u);
    CHECK(s_ab_ref_bad == 0u);
    a->nonfull += *nonfull;
    a->gates += d->gates;
    a->over += d->over;
    a->ok += d->admitted;
    a->bo += d->backoff;
    a->tt += d->time_test;
    a->hl += d->hold_late;
    a->hl_us += d->hold_late_us;
    return 1;
}

static int ab_directed_run(AbDirected *a, ScenarioResult *result, uint32_t ticks)
{
    KageVitaFullspeedGlAdmitBehindSnapshot d;
    uint32_t nonfull;

    a->nonfull = a->gates = a->over = a->ok = a->bo = a->tt = a->hl = a->hl_us = 0u;
    while (ticks-- != 0u) {
        if (!ab_directed_tick(a, result, &d, &nonfull))
            return 0;
    }
    return 1;
}

/* The bound and the back-off, with every input under the test's hand: the
 * gate is behind at every non-full phase (s_render_admit = 0), outstanding
 * bytes are what the test says, a Present hold is what the test leaves for
 * the next loop head, and a late head is a clock step before it. */
static int check_admit_behind_directed(uint32_t *refusals_out)
{
    static ScenarioResult result;
    AbDirected a;
    KageVitaFullspeedGlAdmitBehindSnapshot d;
    const uint32_t bound = kage_vita_fullspeed_scheduler_gl_admit_behind_bytes();
    uint32_t nonfull, phase, refusals = 0u, gate30 = 0u, gate31 = 0u;

    CHECK(bound >= 1u && bound < UINT32_MAX);
    memset(&result, 0, sizeof result);
    memset(&a, 0, sizeof a);
    a.game.manager_pointer = 0x10000000u;
    a.game.game_pointer = 0x20000000u;
    a.game.game_frame = 100u;
    s_now = 1000u;
    s_delay_fail = 0u;
    s_present_wait_kind = PRESENT_WAIT_NONE;
    worker_reset();
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_gl_admit_behind_take(NULL);
    s_render_admit = 0;
    s_ab_out_model = ORACLE_AB_OUT_FIXED;

    /* 1. Exactly at the bound: every behind gate renders. */
    s_ab_out_fixed = bound;
    if (!ab_directed_run(&a, &result, 40u) || !ab_directed_run(&a, &result, 100u))
        return 0;
    CHECK(a.nonfull == 50u && a.gates == 50u && a.ok == 50u);
    CHECK(a.over == 0u && a.bo == 0u && a.tt == 0u && a.hl == 0u);
    /* 2. One byte over: every behind gate takes the stock refusal. */
    s_ab_out_fixed = bound + 1u;
    if (!ab_directed_run(&a, &result, 40u))
        return 0;
    CHECK(a.gates == 20u && a.over == 20u && a.ok == 0u && a.bo == 0u && a.hl == 0u);
    s_ab_out_fixed = bound;
    if (!ab_directed_run(&a, &result, 40u))
        return 0;
    CHECK(a.gates == 20u && a.ok == 20u && a.hl == 0u);
    /* 3. A hold the loop absorbed (the next head still sleeps): nothing. */
    CHECK((a.game.manager_counter & 1u) == 0u);
    s_ab_hold_take_us = 700u;
    if (!ab_directed_run(&a, &result, 40u))
        return 0;
    CHECK(s_ab_hold_take_us == 0u);
    CHECK(a.gates == 20u && a.ok == 20u && a.bo == 0u && a.hl == 0u);
    /* 4. A late head with no hold before it: nothing. */
    s_now += 17000u;
    if (!ab_directed_run(&a, &result, 40u))
        return 0;
    CHECK(a.bo == 0u && a.hl == 0u && a.over == 0u);
    if (!ab_directed_run(&a, &result, 80u))
        return 0;
    CHECK(a.gates == 40u && a.ok == 40u && a.bo == 0u && a.hl == 0u);
    /* 5. Hold and late head: hl, then exactly 30 non-full phases of stock
     * refusals, the 31st renders again. */
    CHECK((a.game.manager_counter & 1u) == 0u);
    s_ab_hold_take_us = 700u;
    s_now += 17000u;
    if (!ab_directed_tick(&a, &result, &d, &nonfull))
        return 0;
    CHECK(d.hold_late == 1u && d.hold_late_us != 0u);
    phase = nonfull;
    for (;;) {
        if (nonfull) {
            if (phase <= 30u) {
                CHECK(d.admitted == 0u && d.backoff == d.gates);
                refusals += d.backoff;
                if (phase == 30u)
                    gate30 = d.gates;
            } else {
                CHECK(d.backoff == 0u && d.admitted == d.gates);
                if (phase == 31u)
                    gate31 = d.gates;
            }
        }
        if (phase >= 45u)
            break;
        if (!ab_directed_tick(&a, &result, &d, &nonfull))
            return 0;
        CHECK(d.hold_late == 0u && d.over == 0u && d.time_test == 0u);
        phase += nonfull;
    }
    /* Both sides of the edge were really asked. */
    CHECK(gate30 == 1u && gate31 == 1u);
    CHECK(refusals == KAGE_VITA_GL_ADMIT_BEHIND_BACKOFF);
    s_render_admit = 1;
    s_ab_out_model = ORACLE_AB_OUT_OVER;
    s_ab_out_fixed = 0u;
    worker_reset();
    *refusals_out = refusals;
    return 1;
}
# endif

typedef struct AbTimeTest {
    uint32_t nonfull, gates, pass, fail, off, over, would, ok, bo, tt, rs;
} AbTimeTest;

/* `ticks` loops (update 3000, non-full 600 us, no Present wait) at one
 * Render cost with the gate forced behind; the counters of the last
 * `measured` of them. */
static int ab_time_test_run(OracleGame *game, ScenarioResult *result,
                            uint32_t render_us, uint32_t ticks,
                            uint32_t measured, AbTimeTest *a)
{
    uint32_t rs0 = s_render_admit_denials;

    memset(a, 0, sizeof *a);
    while (ticks != 0u) {
        KageVitaFullspeedSnapshot before, after;
        KageVitaFullspeedGlAdmitBehindSnapshot d;
        uint32_t nonfull;

        if (ticks == measured) {
            memset(a, 0, sizeof *a);
            rs0 = s_render_admit_denials;
        }
        --ticks;
        kage_vita_fullspeed_scheduler_snapshot(&before);
        (void)run_one_tick(game, 3000u, 600u, render_us, 1u, result);
        kage_vita_fullspeed_scheduler_snapshot(&after);
        kage_vita_fullspeed_scheduler_gl_admit_behind_take(&d);
        nonfull = after.nonfull_phases - before.nonfull_phases;
        CHECK(nonfull <= 1u && d.gates <= nonfull);
        CHECK(after.runtime_disables == 0u && after.sequence_violations == 0u);
        CHECK(s_ab_ref_bad == 0u);
        CHECK(d.test_pass + d.test_fail + d.test_off == d.gates);
        CHECK(d.hold_late == 0u);
        a->nonfull += nonfull;
        a->gates += d.gates;
        a->pass += d.test_pass;
        a->fail += d.test_fail;
        a->off += d.test_off;
        a->over += d.over;
        a->would += d.would;
        a->ok += d.admitted;
        a->bo += d.backoff;
        a->tt += d.time_test;
    }
    a->rs = s_render_admit_denials - rs0;
    return 1;
}

/* The stock time test at a behind gate, with the gate behind at every
 * non-full phase and the outstanding bytes under the test's hand.  Light
 * work (update 3000 at the floor, Render 11000 us: gate near floor + 15100,
 * predicted end near floor + 26100, the next floor at + 33333) passes it;
 * Render 16000 us (gate near + 20100, end near + 36100) fails it; a menu
 * (no Game::Update: the last floor is in the past) leaves it not live.
 * OBSERVE classifies (st p / f / x, wa) and refuses all of them; ON renders
 * p and x and refuses f by the stock time test (tt), unless the bytes are
 * over the bound (ov), which comes first. */
static int check_admit_behind_time_test(AbTimeTest out[4])
{
    static ScenarioResult result;
    OracleGame game;
    AbTimeTest a;
    const uint32_t bound = kage_vita_fullspeed_scheduler_gl_admit_behind_bytes();

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    game.game_frame = 100u;
    s_now = 1000u;
    s_delay_fail = 0u;
    s_present_wait_kind = PRESENT_WAIT_NONE;
    worker_reset();
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_gl_admit_behind_take(NULL);
    s_render_admit = 0;
    s_ab_out_model = ORACLE_AB_OUT_FIXED;
    s_ab_out_fixed = 0u;

    /* 1. The test passes: st p. */
    if (!ab_time_test_run(&game, &result, 11000u, 140u, 100u, &a))
        return 0;
    CHECK(a.nonfull == 50u && a.gates == 50u);
    CHECK(a.pass == 50u && a.fail == 0u && a.off == 0u);
    CHECK(a.over == 0u && a.would == 50u && a.bo == 0u && a.tt == 0u);
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    CHECK(a.ok == 50u && a.rs == 0u);
# else
    CHECK(a.ok == 0u && a.rs == 50u);
# endif
    out[0] = a;
    /* 2. The test fails: st f; ON refuses by the stock time test (tt), and
     * rs counts every one of them in both modes. */
    if (!ab_time_test_run(&game, &result, 16000u, 140u, 100u, &a))
        return 0;
    CHECK(a.nonfull == 50u && a.gates == 50u);
    CHECK(a.pass == 0u && a.fail == 50u && a.off == 0u);
    CHECK(a.over == 0u && a.would == 0u && a.bo == 0u && a.ok == 0u);
    CHECK(a.rs == 50u);
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    CHECK(a.tt == 50u);
# else
    CHECK(a.tt == 0u);
# endif
    out[1] = a;
    /* 3. Failing and one byte over the bound: ov, not tt. */
    s_ab_out_fixed = bound + 1u;
    if (!ab_time_test_run(&game, &result, 16000u, 100u, 100u, &a))
        return 0;
    CHECK(a.nonfull == 50u && a.gates == 50u);
    CHECK(a.fail == 50u && a.over == 50u && a.would == 0u);
    CHECK(a.tt == 0u && a.ok == 0u && a.bo == 0u && a.rs == 50u);
    out[2] = a;
    s_ab_out_fixed = 0u;
    /* 4. A menu: the test is not live (st x) and stock admits. */
    s_ab_menu = 1u;
    if (!ab_time_test_run(&game, &result, 4000u, 140u, 100u, &a))
        return 0;
    s_ab_menu = 0u;
    CHECK(a.gates != 0u && a.gates <= a.nonfull);
    CHECK(a.pass == 0u && a.fail == 0u && a.off == a.gates);
    CHECK(a.over == 0u && a.would == a.gates && a.bo == 0u && a.tt == 0u);
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    CHECK(a.ok == a.gates && a.rs == 0u);
# else
    CHECK(a.ok == 0u && a.rs == a.gates);
# endif
    out[3] = a;
    s_render_admit = 1;
    s_ab_out_model = ORACLE_AB_OUT_OVER;
    worker_reset();
    return 1;
}

static int check_admit_behind_cases(void)
{
    uint32_t presents[2] = { 0u, 0u }, refused[2] = { 0u, 0u };
    AbTimeTest tt[4];
    uint32_t i;
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    uint32_t refusals = 0u;
    const int pinned = 0;
# else
    const int pinned = 1;
# endif

    /* The 40 pinned "worker" lines above ran with every behind gate over
     * the bound.  Now within it at every gate, and mixed. */
    s_ab_out_model = ORACLE_AB_OUT_FIXED;
    s_ab_out_fixed = 0u;
    if (!run_worker_pass("within", pinned, &presents[0], &refused[0]))
        return 0;
    s_ab_out_model = ORACLE_AB_OUT_RATE;
    if (!run_worker_pass("mixed", pinned, &presents[1], &refused[1]))
        return 0;
    s_ab_out_model = ORACLE_AB_OUT_OVER;
# if ISAAC_VITA_GL_THREAD_ADMIT_BEHIND == 2
    if (!check_admit_behind_directed(&refusals))
        return 0;
    printf("Vita GL admit behind (on): PASS (over the bound: 40 worker cases "
           "pinned to the stock numbers; within: presents=%u refused=%u; "
           "mixed: presents=%u refused=%u; bound %u inclusive, bound+1 "
           "refused; hold without late head and late head without hold arm "
           "nothing; hold + late head: %u stock refusals over exactly %u "
           "non-full phases)\n",
           (unsigned)presents[0], (unsigned)refused[0], (unsigned)presents[1],
           (unsigned)refused[1],
           (unsigned)kage_vita_fullspeed_scheduler_gl_admit_behind_bytes(),
           (unsigned)refusals, (unsigned)KAGE_VITA_GL_ADMIT_BEHIND_BACKOFF);
# else
    printf("Vita GL admit behind (observe): PASS (3 x 40 worker cases pinned "
           "to the stock numbers: over the bound, within it, mixed; "
           "presents=%u,%u refused=%u,%u)\n",
           (unsigned)presents[0], (unsigned)presents[1], (unsigned)refused[0],
           (unsigned)refused[1]);
# endif
    if (!check_admit_behind_time_test(tt))
        return 0;
    /* The instants check must have had refusals to hold, some of them with
     * a Present wait between the two instants. */
    CHECK(s_ab_ref_checked != 0u && s_ab_ref_split != 0u);
    printf("Vita GL admit behind (time test): PASS (gate forced behind, "
           "n st(p,f,x) ov wa ok bo tt rs:");
    for (i = 0u; i < 4u; ++i)
        printf(" %s %u %u,%u,%u %u %u %u %u %u %u;",
               i == 0u ? "passing" : i == 1u ? "failing" :
               i == 2u ? "failing over the bound" : "menu",
               (unsigned)tt[i].gates, (unsigned)tt[i].pass,
               (unsigned)tt[i].fail, (unsigned)tt[i].off,
               (unsigned)tt[i].over, (unsigned)tt[i].would,
               (unsigned)tt[i].ok, (unsigned)tt[i].bo, (unsigned)tt[i].tt,
               (unsigned)tt[i].rs);
    printf(" %u refusals handed gate + the measured last Render body / "
           "cost, %u of them with a Present wait between the two)\n",
           (unsigned)s_ab_ref_checked, (unsigned)s_ab_ref_split);
    return 1;
}
#endif
#endif
#endif
static int check_scenario(uint32_t render_us, uint32_t interpolation_enabled)
{
    ScenarioResult result = run_scenario(
        10000000u, render_us, interpolation_enabled, 0u, 0u);
    const KageVitaFullspeedSnapshot *s = &result.scheduler;

    uint32_t minimum_wrappers = render_us <= 27000u ? 596u : 400u;

    if (s->wrapper_ticks < minimum_wrappers || s->wrapper_ticks > 602u) {
        fprintf(stderr,
                "render=%u interpolation=%u elapsed=%llu wrapper=%u "
                "game=%u render_calls=%u skipped=%u dropped=%u\n",
                render_us, interpolation_enabled,
                (unsigned long long)result.elapsed_us,
                s->wrapper_ticks, s->game_updates, s->render_calls,
                s->render_skips, s->dropped_ticks);
    }
    CHECK(s->wrapper_ticks >= minimum_wrappers && s->wrapper_ticks <= 602u);
    if (render_us <= 27000u)
        CHECK((uint64_t)s->wrapper_ticks * 1000000u >=
              result.elapsed_us * 59u);
    CHECK((uint64_t)s->wrapper_ticks * 1000000u <=
          result.elapsed_us * 61u + 1000000u);
    CHECK(s->service_calls == s->wrapper_ticks);
    CHECK(s->manager_dispatch_calls == s->wrapper_ticks);
    CHECK(s->manager_entries == s->wrapper_ticks);
    CHECK(s->game_updates == (s->wrapper_ticks + 1u) / 2u);
    CHECK(result.game_update_time_count == s->game_updates);
    CHECK(result.maximum_game_updates_per_second <=
          ORACLE_MAX_UPDATES_PER_SECOND);
    if (result.game_update_time_count > 1u) {
        uint32_t i;
        for (i = 1u; i < result.game_update_time_count; ++i)
            CHECK(result.game_update_times[i] -
                  result.game_update_times[i - 1u] >= ORACLE_MIN_UPDATE_GAP_US);
    }
    CHECK(oracle_start_spacing_holds(
        result.game_update_times, result.game_update_time_count));
    CHECK(s->full_phases == s->game_updates);
    CHECK(s->nonfull_phases == s->wrapper_ticks - s->full_phases);
    CHECK(s->interpolation_phases ==
          (interpolation_enabled ? s->nonfull_phases : 0u));
    CHECK(s->render_calls == s->render_bodies);
    CHECK(s->render_calls == s->presents);
    CHECK(s->render_calls > 0u && s->render_calls <= s->wrapper_ticks);
    if (interpolation_enabled && render_us <= 10000u) {
        CHECK(s->render_calls == s->wrapper_ticks);
        CHECK((uint64_t)s->render_calls * 1000000u >=
              result.elapsed_us * 59u);
    } else if (interpolation_enabled && render_us <= 20000u) {
        /* At this cost the single owner thread cannot fit 60 renders plus
         * 30 updates.  Keep simulation exact and discard interpolation. */
        CHECK((uint64_t)s->render_calls * 1000000u >=
              result.elapsed_us * 29u);
        CHECK((uint64_t)s->render_calls * 1000000u <=
              result.elapsed_us * 31u + 1000000u);
    } else {
        CHECK(s->render_calls <= s->game_updates + 1u);
        CHECK((uint64_t)s->render_calls * 1000000u <=
              result.elapsed_us * 31u + 1000000u);
    }
    CHECK(s->render_skips + s->render_calls == s->wrapper_ticks);
    CHECK(s->nonfull_skips <= s->nonfull_phases);
    if (!interpolation_enabled)
        CHECK(s->nonfull_skips == s->nonfull_phases);
    CHECK(s->render_skips ==
          s->nonfull_skips + s->late_full_skips);
    CHECK(s->forced_renders <= s->render_calls);
    CHECK(s->duplicate_tick_violations == 0u);
    CHECK(s->parity_violations == 0u);
    CHECK(s->game_frame_violations == 0u);
    CHECK(s->game_update_last_return_site ==
          KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA);
    CHECK(s->game_update_last_frame_after ==
          s->game_update_last_frame_before + 1u);
    CHECK(s->game_frame_violation_return_site == 0u);
    CHECK(s->game_frame_violation_before == 0u);
    CHECK(s->game_frame_violation_after == 0u);
    CHECK(s->manager_counter_rebase_events == 0u);
    CHECK(s->manager_counter_rebases == 0u);
    CHECK(s->manager_counter_rebase_violations == 0u);
    CHECK(s->game_pointer_publish_events == 0u);
    CHECK(s->game_pointer_rebinds == 0u);
    CHECK(s->game_pointer_violations == 0u);
    CHECK(s->sequence_violations == 0u);
    CHECK(s->runtime_disables == 0u);
    CHECK(s->clock_resets == 0u);
    if (render_us <= 27000u) {
        CHECK(s->dropped_ticks == 0u);
        CHECK(s->dropped_us == 0u);
    }
    CHECK(s->maximum_debt_us <= 83334u);
    CHECK(result.limiter_bypasses == s->wrapper_ticks);
    CHECK(result.limiter_fallbacks == 0u);
    CHECK(result.maximum_wrapper_ticks_between_presents <= 10u);
    CHECK(s_log_calls == 1u);
    return 1;
}

static int check_large_stall(void)
{
    ScenarioResult result = run_scenario(
        7000000u, 27000u, 0u, 60u, 5000000u);
    const KageVitaFullspeedSnapshot *s = &result.scheduler;

    CHECK(s->dropped_ticks >= 295u);
    CHECK(s->dropped_us >= 4916666u);
    CHECK(s->maximum_debt_us <= 83334u);
    CHECK(s->service_calls == s->wrapper_ticks);
    CHECK(s->manager_entries == s->wrapper_ticks);
    CHECK(s->game_updates == (s->wrapper_ticks + 1u) / 2u);
    CHECK(result.game_update_time_count == s->game_updates);
    CHECK(result.maximum_game_updates_per_second <=
          ORACLE_MAX_UPDATES_PER_SECOND);
    if (result.game_update_time_count > 1u) {
        uint32_t i;
        for (i = 1u; i < result.game_update_time_count; ++i)
            CHECK(result.game_update_times[i] -
                  result.game_update_times[i - 1u] >= ORACLE_MIN_UPDATE_GAP_US);
    }
    CHECK(oracle_start_spacing_holds(
        result.game_update_times, result.game_update_time_count));
    CHECK(s->render_calls == s->presents);
    CHECK(s->runtime_disables == 0u);
    CHECK(s->duplicate_tick_violations == 0u);
    CHECK(s->parity_violations == 0u);
    CHECK(s->game_frame_violations == 0u);
    CHECK(s->game_update_last_return_site ==
          KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA);
    CHECK(s->game_update_last_frame_after ==
          s->game_update_last_frame_before + 1u);
    CHECK(s->game_frame_violation_return_site == 0u);
    CHECK(s->manager_counter_rebase_events == 0u);
    CHECK(s->manager_counter_rebases == 0u);
    CHECK(s->manager_counter_rebase_violations == 0u);
    CHECK(s->game_pointer_publish_events == 0u);
    CHECK(s->game_pointer_rebinds == 0u);
    CHECK(s->game_pointer_violations == 0u);
    CHECK(s->sequence_violations == 0u);
    return 1;
}

static int check_backward_clock(void)
{
    ScenarioResult result;
    OracleGame game;
    KageVitaFullspeedSnapshot snapshot;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    s_now = 100000u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    (void)run_one_tick(&game, 1u, 1u, 1u, 0u, &result);
    s_now = 10u;
    (void)run_one_tick(&game, 1u, 1u, 1u, 0u, &result);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.clock_resets == 1u);
    CHECK(snapshot.wrapper_ticks == 2u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(snapshot.sequence_violations == 0u);
    return 1;
}

static int check_deadline_overflow_fail_open(void)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = UINT64_MAX / 3u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.wrapper_ticks == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_duplicate_fail_open(void)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.duplicate_tick_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_missing_tick_stage_fail_open(void)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    /* Starting the next distinct tick closes and validates the first one. */
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.wrapper_ticks == 2u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_parity_fail_open(void)
{
    KageVitaFullspeedSnapshot snapshot;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 2u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(render == 1);
    CHECK(snapshot.parity_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_game_frame_fail_open(void)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    s_disable_log_calls = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_end(
        0x20000000u, 9u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 1u);
    CHECK(snapshot.game_update_last_return_site ==
          KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA);
    CHECK(snapshot.game_update_last_frame_before == 7u);
    CHECK(snapshot.game_update_last_frame_after == 9u);
    CHECK(snapshot.game_frame_violation_return_site ==
          KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA);
    CHECK(snapshot.game_frame_violation_before == 7u);
    CHECK(snapshot.game_frame_violation_after == 9u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(s_disable_log_calls == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_nonfull_game_frame_state_rebase(void)
{
    KageVitaFullspeedSnapshot snapshot;
    ScenarioResult result;
    OracleGame game;
    uint64_t first_update_time;
    int render;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.manager_counter = 2u;
    game.game_pointer = 0x20000000u;
    game.game_frame = 420u;
    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();

    /* Continue/load can restore a saved frame on the odd-entry/even-exit
     * Manager phase without executing Game::Update.  It must not turn off
     * the scheduler or discard the native interpolation Render. */
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        game.manager_pointer, 1u, 1u, game.game_pointer, 7u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        game.manager_pointer, game.manager_counter, 1u,
        game.game_pointer, game.game_frame);
    CHECK(render == 1);
    kage_vita_fullspeed_scheduler_note_render_entry();
    kage_vita_fullspeed_scheduler_note_render_body(
        game.manager_pointer, game.manager_counter, 1u);
    kage_vita_fullspeed_scheduler_note_present();
    kage_vita_fullspeed_scheduler_note_render_return();

    /* Two following full phases still pass through the independent Game
     * floor; the saved-frame rebase cannot create a >30-UPS burst. */
    CHECK(run_one_tick(&game, 0u, 0u, 0u, 1u, &result) == 1);
    CHECK(result.game_update_time_count == 1u);
    first_update_time = result.game_update_times[0];
    CHECK(run_one_tick(&game, 0u, 0u, 0u, 1u, &result) == 1);
    CHECK(run_one_tick(&game, 0u, 0u, 0u, 1u, &result) == 1);
    CHECK(result.game_update_time_count == 2u);
    CHECK(result.game_update_times[1] - first_update_time >= 33334u);

    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_updates == 2u);
    CHECK(snapshot.game_frame_violations == 0u);
    CHECK(snapshot.sequence_violations == 0u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_game_frame_early_return_attribution(void)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_early_return(0x002ce1c7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 0u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());

    /* The token explains the unchanged frame; it does not accept it. */
    kage_vita_fullspeed_scheduler_note_game_update_end(
        0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 1u);
    CHECK(snapshot.game_update_last_return_site == 0x002ce1c7u);
    CHECK(snapshot.game_update_last_frame_before == 7u);
    CHECK(snapshot.game_update_last_frame_after == 7u);
    CHECK(snapshot.game_frame_violation_return_site == 0x002ce1c7u);
    CHECK(snapshot.game_frame_violation_before == 7u);
    CHECK(snapshot.game_frame_violation_after == 7u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

/* Any pinned early RET outside the accepted set keeps the fail-open. */
static int check_game_frame_unknown_site_fail_open(uint32_t return_site)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_early_return(
        return_site);
    kage_vita_fullspeed_scheduler_note_game_update_end(
        0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 1u);
    CHECK(snapshot.game_frame_violation_return_site == return_site);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_game_frame_zero_frame_early_return(uint32_t return_site)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_early_return(
        return_site);
    kage_vita_fullspeed_scheduler_note_game_update_end(
        0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 0u);
    CHECK(snapshot.game_update_last_return_site == return_site);
    CHECK(snapshot.game_update_last_frame_before == 7u);
    CHECK(snapshot.game_update_last_frame_after == 7u);
    CHECK(snapshot.game_frame_violation_return_site == 0u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_game_frame_zero_frame_wrong_delta_fail_open(
    uint32_t return_site)
{
    KageVitaFullspeedSnapshot snapshot;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_early_return(
        return_site);
    kage_vita_fullspeed_scheduler_note_game_update_end(
        0x20000000u, 9u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 1u);
    CHECK(snapshot.game_update_last_return_site == return_site);
    CHECK(snapshot.game_update_last_frame_before == 7u);
    CHECK(snapshot.game_update_last_frame_after == 9u);
    CHECK(snapshot.game_frame_violation_return_site == return_site);
    CHECK(snapshot.game_frame_violation_before == 7u);
    CHECK(snapshot.game_frame_violation_after == 9u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_game_return_missing_fail_open(void)
{
    KageVitaFullspeedSnapshot snapshot;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 1u, 0u, 0x20000000u, 8u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(render == 1);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_interpolation_transition(void)
{
    KageVitaFullspeedSnapshot snapshot;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();

    /* The real menu-to-game boundary enters Update with interpolation on and
     * reaches Render with it off.  This is game-owned state, not corruption. */
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 1u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_end(
        0x20000000u, 8u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 1u, 0u, 0x20000000u, 8u);
    CHECK(render == 1);
    kage_vita_fullspeed_scheduler_note_render_entry();
    kage_vita_fullspeed_scheduler_note_render_body(
        0x10000000u, 1u, 0u);
    kage_vita_fullspeed_scheduler_note_present();
    kage_vita_fullspeed_scheduler_note_render_return();

    /* A state change without the paired full Game::Update is still hostile
     * and must fail open. */
    s_now = 16667u;
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 1u, 0u, 0x20000000u, 8u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 2u, 1u, 0x20000000u, 8u);
    CHECK(render == 1);

    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.wrapper_ticks == 2u);
    CHECK(snapshot.game_updates == 1u);
    CHECK(snapshot.full_phases == 1u);
    CHECK(snapshot.nonfull_phases == 0u);
    CHECK(snapshot.interpolation_phases == 0u);
    CHECK(snapshot.render_calls == 1u);
    CHECK(snapshot.presents == 1u);
    CHECK(snapshot.nonfull_skips == 0u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.parity_violations == 0u);
    CHECK(snapshot.game_frame_violations == 0u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_game_pointer_publish_transition(void)
{
    KageVitaFullspeedSnapshot snapshot;
    const uint32_t manager_pointer = 0x10000000u;
    const uint32_t new_game_pointer = 0x30000000u;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        manager_pointer, 0u, 0u, 0u, 0u);

    /* Exact J835 order: Continue's constructor returns a new Game while the
     * global is null; the original publication follows this observation. */
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        manager_pointer, 0u, 0u, new_game_pointer);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, new_game_pointer, 0u);
    ++s_now;
    kage_vita_fullspeed_scheduler_note_game_update_end(
        new_game_pointer, 1u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        manager_pointer, 1u, 0u, new_game_pointer, 1u);
    CHECK(render == 1);
    kage_vita_fullspeed_scheduler_note_render_entry();
    kage_vita_fullspeed_scheduler_note_render_body(
        manager_pointer, 1u, 0u);
    kage_vita_fullspeed_scheduler_note_present();
    kage_vita_fullspeed_scheduler_note_render_return();

    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_publish_events == 1u);
    CHECK(snapshot.game_pointer_rebinds == 1u);
    CHECK(snapshot.game_pointer_violations == 0u);
    CHECK(snapshot.game_updates == 1u);
    CHECK(snapshot.game_frame_violations == 0u);
    CHECK(snapshot.sequence_violations == 0u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static void start_manager_counter_rebase_tick(uint32_t game_pointer)
{
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 1u, 1u, game_pointer,
        game_pointer != 0u ? 7u : 0u);
}

static int check_manager_counter_rebase_transition(void)
{
    KageVitaFullspeedSnapshot snapshot;
    const uint32_t manager_pointer = 0x10000000u;
    const uint32_t game_pointer = 0x20000000u;
    const uint32_t new_game_pointer = 0x30000000u;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;
    start_manager_counter_rebase_tick(game_pointer);

    /* Physical warm Continue enters on the odd menu phase.  J835 increments
     * Manager+0x4a264 before the existing Game's full update, then performs
     * the ordinary final increment before Render. */
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        manager_pointer, 1u, 2u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        2u, game_pointer, 7u);
    ++s_now;
    kage_vita_fullspeed_scheduler_note_game_update_end(game_pointer, 8u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        manager_pointer, 3u, 0u, game_pointer, 8u);
    CHECK(render == 1);
    kage_vita_fullspeed_scheduler_note_render_entry();
    kage_vita_fullspeed_scheduler_note_render_body(
        manager_pointer, 3u, 0u);
    kage_vita_fullspeed_scheduler_note_present();
    kage_vita_fullspeed_scheduler_note_render_return();
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebase_events == 1u);
    CHECK(snapshot.manager_counter_rebases == 1u);
    CHECK(snapshot.manager_counter_rebase_violations == 0u);
    CHECK(snapshot.game_pointer_publish_events == 0u);
    CHECK(snapshot.game_pointer_violations == 0u);
    CHECK(snapshot.game_updates == 1u);
    CHECK(snapshot.full_phases == 1u);
    CHECK(snapshot.sequence_violations == 0u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());

    /* The same exact counter normalization composes with the separately
     * authenticated null-to-new Game publication when allocation is needed. */
    start_manager_counter_rebase_tick(0u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        manager_pointer, 1u, 2u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        manager_pointer, 2u, 0u, new_game_pointer);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        2u, new_game_pointer, 0u);
    kage_vita_fullspeed_scheduler_note_game_update_end(
        new_game_pointer, 1u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        manager_pointer, 3u, 0u, new_game_pointer, 1u);
    CHECK(render == 1);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebase_events == 1u);
    CHECK(snapshot.manager_counter_rebases == 1u);
    CHECK(snapshot.manager_counter_rebase_violations == 0u);
    CHECK(snapshot.game_pointer_publish_events == 1u);
    CHECK(snapshot.game_pointer_rebinds == 1u);
    CHECK(snapshot.game_pointer_violations == 0u);
    CHECK(snapshot.sequence_violations == 0u);
    CHECK(snapshot.runtime_disables == 0u);
    return 1;
}

static int check_manager_counter_rebase_hostile_cases(void)
{
    KageVitaFullspeedSnapshot snapshot;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;

    /* A foreign Manager, stale old value or non-successor are not tokens. */
    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        0x10000004u, 1u, 2u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebase_events == 1u);
    CHECK(snapshot.manager_counter_rebases == 0u);
    CHECK(snapshot.manager_counter_rebase_violations == 1u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        0x10000000u, 3u, 4u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebases == 0u);
    CHECK(snapshot.manager_counter_rebase_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        0x10000000u, 1u, 4u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebases == 0u);
    CHECK(snapshot.manager_counter_rebase_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* The one exact store may be observed only once per Manager tick. */
    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        0x10000000u, 1u, 2u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        0x10000000u, 1u, 2u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebase_events == 2u);
    CHECK(snapshot.manager_counter_rebases == 1u);
    CHECK(snapshot.manager_counter_rebase_violations == 1u);
    CHECK(snapshot.duplicate_tick_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* Even a valid Continue rebase cannot authorize an interpolation change
     * without a paired full Game update. Retain this existing rejection. */
    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        0x10000000u, 1u, 2u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 3u, 0u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(render == 1);
    CHECK(snapshot.manager_counter_rebases == 1u);
    CHECK(snapshot.manager_counter_rebase_violations == 0u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* An unobserved counter change still fails closed at Game begin. */
    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        2u, 0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.manager_counter_rebase_events == 0u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    return 1;
}

static void start_game_pointer_tick(
    uint32_t game_pointer, uint32_t game_frame)
{
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        0x10000000u, 0u, 0u, game_pointer, game_frame);
}

static int check_game_pointer_hostile_cases(void)
{
    KageVitaFullspeedSnapshot snapshot;
    int render;

    s_now = 0u;
    s_delay_fail = 0u;

    /* A swap which did not pass the exact publication seam has no token. */
    start_game_pointer_tick(0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x30000000u, 0u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_publish_events == 0u);
    CHECK(snapshot.game_pointer_rebinds == 0u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* Continue may only construct into a Manager-entry null Game slot. */
    start_game_pointer_tick(0x20000000u, 7u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0u, 0x30000000u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_publish_events == 1u);
    CHECK(snapshot.game_pointer_rebinds == 0u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* The generated store must still see the original global as null. */
    start_game_pointer_tick(0u, 0u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0x20000000u, 0x30000000u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_publish_events == 1u);
    CHECK(snapshot.game_pointer_rebinds == 0u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* Null constructor results and a foreign Manager are rejected. */
    start_game_pointer_tick(0u, 0u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0u, 0u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_rebinds == 0u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    start_game_pointer_tick(0u, 0u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000004u, 0u, 0u, 0x30000000u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_rebinds == 0u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* The one-shot token cannot be duplicated in one Manager tick. */
    start_game_pointer_tick(0u, 0u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0u, 0x30000000u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0u, 0x40000000u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_publish_events == 2u);
    CHECK(snapshot.game_pointer_rebinds == 1u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.duplicate_tick_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* A publication must be consumed by exactly one paired Game update. */
    start_game_pointer_tick(0u, 0u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0u, 0x30000000u);
    render = kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 1u, 0u, 0x30000000u, 0u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(render == 1);
    CHECK(snapshot.game_pointer_rebinds == 1u);
    CHECK(snapshot.game_pointer_violations == 0u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);

    /* A second unobserved swap after a valid publication is still hostile. */
    start_game_pointer_tick(0u, 0u);
    kage_vita_fullspeed_scheduler_note_game_pointer_publish(
        0x10000000u, 0u, 0u, 0x30000000u);
    kage_vita_fullspeed_scheduler_note_game_update_begin(
        0u, 0x40000000u, 0u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_pointer_rebinds == 1u);
    CHECK(snapshot.game_pointer_violations == 1u);
    CHECK(snapshot.sequence_violations == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    return 1;
}

/* Exercise the production state machine, including the NEXT head's audit.
 * Constants copied from the existing rebase fixtures above; 15815 is the
 * restored frame printed by owner-video-game.log's Continue callback. */
static void complete_cutscene_render(uint32_t counter)
{
    kage_vita_fullspeed_scheduler_note_render_entry();
    kage_vita_fullspeed_scheduler_note_render_body(0x10000000u, counter, 1u);
    s_now += 5000u;
    kage_vita_fullspeed_scheduler_note_present();
    kage_vita_fullspeed_scheduler_note_render_return();
}

static int check_continue_cutscene_gameplay(uint32_t publish, uint32_t restore,
                                           uint32_t duty)
{
    OracleGame game = {0x10000000u, 2u, 0x20000000u, 7u, 0u};
    ScenarioResult result;
    KageVitaFullspeedSnapshot before, after;
    uint32_t tick, expected_presents;
    uint64_t gameplay_start, elapsed;

    memset(&result, 0, sizeof result);
    s_now = 0u;
    s_delay_fail = 0u;
    s_gate_log_calls = s_gate_log_truncated = 0u;
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
    kage_vita_fullspeed_scheduler_set_duty((uint8_t)duty);
#endif
    start_manager_counter_rebase_tick(publish ? 0u : game.game_pointer);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
        game.manager_pointer, 1u, game.manager_counter);
    if (publish) {
        game.game_pointer = 0x30000000u;
        game.game_frame = 0u;
        kage_vita_fullspeed_scheduler_note_game_pointer_publish(
            game.manager_pointer, game.manager_counter, 0u, game.game_pointer);
    }
    if (restore) game.game_frame = 15815u;
    ++game.manager_counter;
    CHECK(kage_vita_fullspeed_scheduler_plan_render(
        game.manager_pointer, game.manager_counter, 1u,
        game.game_pointer, game.game_frame));
    complete_cutscene_render(game.manager_counter);

    /* 120 ordinary cutscene ticks, no Game calls. This crosses the receipt's
     * lifetime and exercises both phases and all configured presentation locks. */
    for (tick = 0u; tick < 120u; ++tick) {
        kage_vita_fullspeed_scheduler_note_loop_head();
        kage_vita_fullspeed_scheduler_note_service();
        kage_vita_fullspeed_scheduler_note_manager_dispatch();
        kage_vita_fullspeed_scheduler_note_manager_entry(
            game.manager_pointer, game.manager_counter, 1u,
            game.game_pointer, game.game_frame);
        ++game.manager_counter;
        if (kage_vita_fullspeed_scheduler_plan_render(
                game.manager_pointer, game.manager_counter, 1u,
                game.game_pointer, game.game_frame))
            complete_cutscene_render(game.manager_counter);
        CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
    }
    kage_vita_fullspeed_scheduler_snapshot(&before);
    CHECK(before.game_updates == 0u);
    CHECK(before.full_phases == 61u && before.nonfull_phases == 60u);
    CHECK(before.manager_counter_rebases == 1u);
    CHECK(before.game_pointer_rebinds == publish);
    CHECK(before.runtime_disables == 0u);

    gameplay_start = s_now;
    for (tick = 0u; tick < 240u; ++tick)
        (void)run_one_tick(&game, 2500u, 500u, 5000u, 1u, &result);
    elapsed = s_now - gameplay_start;
    kage_vita_fullspeed_scheduler_snapshot(&after);
    CHECK(after.game_updates == 120u);
    CHECK(after.full_phases - before.full_phases == 120u);
    CHECK(after.nonfull_phases - before.nonfull_phases == 120u);
    expected_presents = 120u + (duty ? 120u / duty : 0u);
    CHECK(after.presents - before.presents == expected_presents);
    CHECK(after.nonfull_skips - before.nonfull_skips == 240u - expected_presents);
    CHECK(after.late_full_skips == 0u);
    CHECK(result.game_update_time_count == 120u);
    CHECK(maximum_game_updates_per_second(result.game_update_times, 120u) <=
          ORACLE_MAX_UPDATES_PER_SECOND);
    for (tick = 1u; tick < 120u; ++tick)
        CHECK(result.game_update_times[tick] - result.game_update_times[tick-1u] >=
              ORACLE_MIN_UPDATE_GAP_US);
    CHECK(elapsed >= 3950000u && elapsed <= 4050000u);
    CHECK(game.game_frame == (restore ? 15815u : (publish ? 0u : 7u)) + 120u);
    CHECK(after.sequence_violations == 0u && after.parity_violations == 0u);
    CHECK(after.game_frame_violations == 0u && after.game_pointer_violations == 0u);
    CHECK(after.clock_resets == 0u && after.runtime_disables == 0u);
    CHECK(result.limiter_fallbacks == 0u && result.limiter_bypasses == 240u);
    CHECK(s_gate_log_calls == 0u);
    /* Finish the last gameplay tick as well. */
    kage_vita_fullspeed_scheduler_note_loop_head();
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_continue_cutscene_faults(void)
{
    KageVitaFullspeedSnapshot snapshot;
    uint32_t mode;
    for (mode = 0u; mode < 12u; ++mode) {
        uint32_t manager = 0x10000000u, counter = 3u;
        uint32_t game = 0x20000000u, interpolation = 1u;
        s_now = 100u;
        s_delay_fail = 0u;
        s_gate_log_calls = s_gate_log_truncated = s_disable_log_calls = 0u;
        start_manager_counter_rebase_tick(mode == 7u ? 0u : game);
        if (mode != 6u)
            kage_vita_fullspeed_scheduler_note_manager_counter_rebase(manager, 1u, 2u);
        if (mode == 0u) manager += 4u;
        if (mode == 1u) counter += 1u;
        if (mode == 2u) game += 4u;
        if (mode == 3u) interpolation = 0u;
        if (mode == 4u) kage_vita_fullspeed_scheduler_note_game_update_begin(2u, game, 7u);
        if (mode == 5u) kage_vita_fullspeed_scheduler_note_game_update_end(game, 7u);
        if (mode == 7u) game = 0u;
        if (mode == 8u) s_now = 99u;
        if (mode >= 10u) {
            /* The accepted Continue receipt must not hide an incomplete
             * Render, nor a Game begin added after admission without its end. */
            CHECK(kage_vita_fullspeed_scheduler_plan_render(
                manager, counter, interpolation, game, 15815u));
            if (mode == 10u)
                kage_vita_fullspeed_scheduler_note_render_entry();
            else {
                complete_cutscene_render(counter);
                kage_vita_fullspeed_scheduler_note_game_update_begin(2u, game, 15815u);
            }
            kage_vita_fullspeed_scheduler_note_loop_head();
        } else if (mode == 9u) {
            /* No Render gate: an input token alone cannot exempt the next
             * head's required-stage / Continue audit. */
            kage_vita_fullspeed_scheduler_note_loop_head();
        } else if (mode == 6u) {
            /* Start on an even phase to test a full-frame restore without
             * either Continue token, rather than a wrong final counter. */
            start_game_pointer_tick(game, 7u);
            CHECK(kage_vita_fullspeed_scheduler_plan_render(manager, 1u, 0u, game, 15815u));
        } else {
            CHECK(kage_vita_fullspeed_scheduler_plan_render(
                manager, counter, interpolation, game, 15815u));
        }
        kage_vita_fullspeed_scheduler_snapshot(&snapshot);
        CHECK(snapshot.runtime_disables == 1u);
        CHECK(s_disable_log_calls == 1u);
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
        CHECK(s_gate_log_truncated == 0u);
        if (mode != 5u && mode < 9u) {
            static const char *reasons[] = {"manager-identity", "counter", "game-identity",
                "interpolation", "game-pair", "", "game-frame", "continue-no-game", "clock"};
            CHECK(s_gate_log_calls == 1u);
            CHECK(strstr(s_gate_record, reasons[mode]) != NULL);
            CHECK(strstr(s_gate_record, "saved/current mgr=") != NULL);
        }
        /* Fail-open repeats must not flood either first-failure record. */
        (void)kage_vita_fullspeed_scheduler_plan_render(manager, counter, interpolation, game, 15815u);
        CHECK(s_gate_log_calls <= 1u && s_disable_log_calls == 1u);
    }

    /* A successfully consumed token cannot excuse a full-phase frame change
     * on a subsequent cutscene tick. */
    s_now = 0u;
    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(0x10000000u, 1u, 2u);
    CHECK(kage_vita_fullspeed_scheduler_plan_render(0x10000000u, 3u, 1u, 0x20000000u, 15815u));
    complete_cutscene_render(3u);
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(0x10000000u, 3u, 1u, 0x20000000u, 15815u);
    CHECK(kage_vita_fullspeed_scheduler_plan_render(0x10000000u, 4u, 1u, 0x20000000u, 15815u));
    complete_cutscene_render(4u);
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(0x10000000u, 4u, 1u, 0x20000000u, 15815u);
    CHECK(kage_vita_fullspeed_scheduler_plan_render(0x10000000u, 5u, 1u, 0x20000000u, 15816u));
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.game_frame_violations == 1u && snapshot.runtime_disables == 1u);

    /* Exercise the real diagnostic formatter at full-width argument values.
     * The platform's existing body buffer has 384 bytes. */
    s_now = 0u;
    s_gate_log_calls = s_gate_log_truncated = 0u;
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(UINT32_MAX, UINT32_MAX, 1u, UINT32_MAX, UINT32_MAX);
    CHECK(kage_vita_fullspeed_scheduler_plan_render(UINT32_MAX - 1u, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX));
    CHECK(s_gate_log_calls == 1u && s_gate_log_truncated == 0u);
    CHECK(strstr(s_gate_record, "mgr=ffffffff/fffffffe") != NULL);
    CHECK(strstr(s_gate_record, "frame=4294967295/4294967295") != NULL);
    /* Allow the other nine decimal digits of a saturated tick counter. */
    CHECK(strlen(s_gate_record) + 9u < sizeof s_gate_record);
    return 1;
}

static int check_continue_cutscene_late_render(void)
{
    KageVitaFullspeedSnapshot snapshot;
    s_now = 0u;
    s_delay_fail = 0u;
    start_manager_counter_rebase_tick(0x20000000u);
    kage_vita_fullspeed_scheduler_note_manager_counter_rebase(0x10000000u, 1u, 2u);
    s_now = 20000u; /* Past the first 16.667-ms Render deadline. */
    CHECK(!kage_vita_fullspeed_scheduler_plan_render(
        0x10000000u, 3u, 1u, 0x20000000u, 15815u));
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.full_phases == 1u && snapshot.late_full_skips == 1u);
    CHECK(snapshot.presents == 0u && snapshot.game_updates == 0u);
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.sequence_violations == 0u && snapshot.runtime_disables == 0u);
    CHECK(kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

static int check_wait_failure_fail_open(void)
{
    ScenarioResult result;
    OracleGame game;
    KageVitaFullspeedSnapshot snapshot;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    s_now = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_reset();
    (void)run_one_tick(&game, 1u, 1u, 1u, 0u, &result);
    s_delay_fail = 1u;
    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.wait_calls == 1u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
    return 1;
}

#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
static int prepare_advanced_head(OracleGame *game, ScenarioResult *result)
{
    memset(game, 0, sizeof *game);
    memset(result, 0, sizeof *result);
    game->manager_pointer = 0x10000000u;
    game->game_pointer = 0x20000000u;
    s_now = 0u;
    s_delay_fail = s_test_backward_wait = 0u;
    s_test_overshoot_us = s_test_variable_wake = s_test_waits = 0u;
    s_test_service_us = 500u;
    kage_vita_fullspeed_scheduler_reset();
    CHECK(run_one_tick(game, 2500u, 500u, 5000u, 1u, result));
    CHECK(run_one_tick(game, 2500u, 500u, 5000u, 1u, result));
    return 1;
}

static int check_advanced_head_transitions(void)
{
    /* 0 active; 1 paused; 2..5 current identity misses; 6 rebase;
     * 7 publication; 8 menu; 9 clock retreat before a successful floor wait.
     * None of these baseline-permitted transitions becomes a new fault. */
    for (uint32_t mode = 0u; mode < 10u; ++mode) {
        OracleGame game;
        ScenarioResult result;
        KageVitaFullspeedSnapshot snap;
        uint64_t before_floor, after_floor, next_head;
        CHECK(prepare_advanced_head(&game, &result));
        kage_vita_fullspeed_scheduler_note_loop_head();
        CHECK(s_now >= 28833u && s_now <= 28834u);
        kage_vita_fullspeed_scheduler_note_service();
        s_now += 500u;
        kage_vita_fullspeed_scheduler_note_manager_dispatch();
        if (mode == 2u) game.manager_pointer += 16u;
        if (mode == 3u) game.game_pointer += 16u;
        if (mode == 4u) game.manager_counter += 2u;
        if (mode == 5u) game.game_frame += 10u;
        if (mode == 6u) ++game.manager_counter;
        if (mode == 7u || mode == 8u) game.game_pointer = 0u;
        kage_vita_fullspeed_scheduler_note_manager_entry(
            game.manager_pointer, game.manager_counter, 1u,
            game.game_pointer, game.game_frame);
        if (mode == 6u) {
            kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
                game.manager_pointer, game.manager_counter, game.manager_counter + 1u);
            ++game.manager_counter;
        }
        if (mode == 7u) {
            kage_vita_fullspeed_scheduler_note_game_pointer_publish(
                game.manager_pointer, game.manager_counter, 0u, 0x30000000u);
            game.game_pointer = 0x30000000u;
        }
        if (mode == 9u) s_now -= 1500u;
        before_floor = after_floor = s_now;
        if (game.game_pointer) {
            kage_vita_fullspeed_scheduler_note_game_update_begin(
                game.manager_counter, game.game_pointer, game.game_frame);
            after_floor = s_now;
            CHECK(after_floor >= result.game_update_times[0] + 33333u);
            s_now += 2500u;
            if (mode == 1u)
                kage_vita_fullspeed_scheduler_note_game_update_early_return(
                    KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_RETURN_RVA);
            else ++game.game_frame;
            kage_vita_fullspeed_scheduler_note_game_update_end(
                game.game_pointer, game.game_frame);
        }
        ++game.manager_counter;
        CHECK(kage_vita_fullspeed_scheduler_plan_render(
            game.manager_pointer, game.manager_counter, 1u,
            game.game_pointer, game.game_frame));
        kage_vita_fullspeed_scheduler_note_render_entry();
        kage_vita_fullspeed_scheduler_note_render_body(
            game.manager_pointer, game.manager_counter, 1u);
        s_now += 5000u;
        kage_vita_fullspeed_scheduler_note_present();
        kage_vita_fullspeed_scheduler_note_render_return();
        kage_vita_fullspeed_scheduler_note_loop_head();
        next_head = s_now;
        if (mode == 0u || mode == 8u)
            CHECK(next_head >= 50000u && next_head <= 50002u);
        else
            CHECK(next_head >= 50000u + after_floor - before_floor &&
                  next_head <= 50002u + after_floor - before_floor);
        kage_vita_fullspeed_scheduler_snapshot(&snap);
        CHECK(snap.runtime_disables == 0u && snap.sequence_violations == 0u);
        CHECK(snap.game_frame_violations == 0u && snap.game_pointer_violations == 0u);
    }
    return 1;
}

static int check_advanced_head_faults(void)
{
    OracleGame game;
    ScenarioResult result;
    KageVitaFullspeedSnapshot snap;
    for (uint32_t mode = 0u; mode < 10u; ++mode) {
        CHECK(prepare_advanced_head(&game, &result));
        if (mode == 0u) s_delay_fail = 1u;
        if (mode == 1u) s_test_backward_wait = 1u;
        if (mode == 2u) s_now = 10u;
        if (mode == 9u) s_now = UINT64_MAX / 3u + 1u;
        kage_vita_fullspeed_scheduler_note_loop_head();
        if (mode >= 3u && mode != 9u) {
            kage_vita_fullspeed_scheduler_note_service();
            if (mode == 8u) kage_vita_fullspeed_scheduler_note_service();
            s_now += 500u;
            kage_vita_fullspeed_scheduler_note_manager_dispatch();
            kage_vita_fullspeed_scheduler_note_manager_entry(
                game.manager_pointer, game.manager_counter, 1u,
                game.game_pointer, game.game_frame);
            if (mode == 3u) s_delay_fail = 1u;
            if (mode == 4u) s_test_backward_wait = 1u;
            kage_vita_fullspeed_scheduler_note_game_update_begin(
                game.manager_counter, game.game_pointer, game.game_frame);
            if (mode == 5u) s_now -= 1u;
            if (mode == 6u) s_now = UINT64_MAX / 3u + 1u;
            ++game.game_frame;
            if (mode == 7u)
                (void)kage_vita_fullspeed_scheduler_plan_render(
                    game.manager_pointer, game.manager_counter + 1u, 1u,
                    game.game_pointer, game.game_frame); /* lost return token */
            else
                kage_vita_fullspeed_scheduler_note_game_update_end(
                    game.game_pointer, game.game_frame);
        }
        kage_vita_fullspeed_scheduler_snapshot(&snap);
        if (mode == 0u || mode == 3u || mode >= 5u) {
            CHECK(snap.runtime_disables == 1u);
            CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
        } else {
            CHECK(snap.clock_resets == 1u && snap.runtime_disables == 0u);
        }
        kage_vita_fullspeed_scheduler_deactivate();
        CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());
        s_now = 100u;
        s_delay_fail = s_test_backward_wait = 0u;
        kage_vita_fullspeed_scheduler_reset();
        kage_vita_fullspeed_scheduler_note_loop_head();
        CHECK(s_now == 100u);
        kage_vita_fullspeed_scheduler_snapshot(&snap);
        CHECK(snap.clock_resets == 0u && snap.runtime_disables == 0u);
    }
    return 1;
}

static int check_advanced_head_present_intervals(void)
{
    for (uint32_t pattern = 0u; pattern < 4u; ++pattern) {
        for (uint32_t wake = 0u; wake < 3u; ++wake) {
            OracleGame game;
            ScenarioResult result;
            KageVitaFullspeedSnapshot snap;
            CHECK(prepare_advanced_head(&game, &result));
            /* Start a fresh epoch with the same public callback sequence. */
            game.manager_counter = game.game_frame = 0u;
            memset(&result, 0, sizeof result);
            s_now = 0u;
            s_test_waits = 0u;
            s_test_overshoot_us = wake == 0u ? 0u : wake == 1u ? 180u : 900u;
            s_test_variable_wake = 1u;
            kage_vita_fullspeed_scheduler_reset();
            for (uint32_t tick = 0u; tick < 960u; ++tick) {
                uint32_t full = (game.manager_counter & 1u) == 0u;
                uint32_t index = game.manager_counter / 2u;
                s_test_service_us = 300u;
                if (full && pattern == 1u && (index & 1u)) s_test_service_us += 1000u;
                if (full && pattern >= 2u && index % 16u == 15u)
                    s_test_service_us += pattern == 2u ? 4000u : 30000u;
                (void)run_one_tick(&game, 2200u, 300u, 9500u, 1u, &result);
            }
            kage_vita_fullspeed_scheduler_snapshot(&snap);
            CHECK(snap.runtime_disables == 0u && snap.dropped_ticks == 0u);
            CHECK(result.game_update_time_count == 480u);
            for (uint32_t i = 1u; i < result.game_update_time_count; ++i) {
                CHECK(result.game_update_times[i] - result.game_update_times[i - 1u] >= 30000u);
                CHECK((uint64_t)i * KAGE_VITA_FULLSPEED_GAME_TICK_UNITS <=
                      3u * (result.game_update_times[i] - result.game_update_times[0]));
            }
            if (pattern < 2u) {
                CHECK(snap.presents == 960u && snap.render_skips == 0u);
                for (uint32_t i = 241u; i < result.present_time_count; ++i) {
                    uint64_t delta = result.present_times[i] - result.present_times[i - 1u];
                    /* Reject the former 10.57/22.77-ms "60 FPS" grid policy. */
                    CHECK(delta >= 14000u && delta <= 20000u);
                }
            }
        }
    }
    s_test_service_us = 500u;
    s_test_overshoot_us = s_test_variable_wake = s_test_waits = 0u;
    return 1;
}
#endif

#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
/* port.ini fps_lock duty (port.ini design section 2):
 * kage_vita_fullspeed_scheduler_set_duty(k) caps the non-full (interpolated)
 * phase admission to every k-th one; k = 0 admits none.  The scenarios below
 * run the cheap 5-ms room that presents 120 per 120 ticks today. */
#define DUTY_TICKS 120u

/* 120 wrapper ticks (2.5-ms full update, 0.5-ms non-full, 5-ms render) from
 * a fresh reset with the duty last set; transcript[i] is 'F' (full phase
 * presented), 'N' (non-full presented) or '_' (skipped). */
static int run_duty_transcript(uint32_t interpolation_enabled,
                               char transcript[DUTY_TICKS + 1u],
                               KageVitaFullspeedSnapshot *snapshot)
{
    ScenarioResult result;
    OracleGame game;
    uint32_t tick;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    s_now = 0u;
    s_log_calls = 0u;
    s_delay_fail = 0u;
    s_banner[0] = '\0';
    kage_vita_fullspeed_scheduler_reset();
    for (tick = 0u; tick < DUTY_TICKS; ++tick) {
        int render = run_one_tick(&game, 2500u, 500u, 5000u,
                                  interpolation_enabled, &result);
        int full = (game.manager_counter & 1u) != 0u;

        transcript[tick] = render ? (full ? 'F' : 'N') : '_';
    }
    transcript[DUTY_TICKS] = '\0';
    kage_vita_fullspeed_scheduler_snapshot(snapshot);
    return result.limiter_fallbacks == 0u;
}

/* Section 2 pattern: every full phase; the n-th non-full phase (n from 1)
 * when n % k == 0; none when k == 0.  F N / F _ F N / F _ F _ F N / F _. */
static void expected_duty_transcript(uint32_t k, char out[DUTY_TICKS + 1u])
{
    uint32_t tick;
    uint32_t nonfull = 0u;

    for (tick = 0u; tick < DUTY_TICKS; ++tick) {
        if ((tick & 1u) == 0u) {
            out[tick] = 'F';
        } else {
            ++nonfull;
            out[tick] = (k != 0u && nonfull % k == 0u) ? 'N' : '_';
        }
    }
    out[DUTY_TICKS] = '\0';
}

/* " fps_lock=%u" sits once, after the pinned "parity=4a264 frame=1a30dc". */
static int check_banner_fps_lock(unsigned fps_lock)
{
    const char *parity = strstr(s_banner, "parity=4a264 frame=1a30dc");
    const char *suffix = strstr(s_banner, " fps_lock=");
    char want[24];

    (void)snprintf(want, sizeof want, " fps_lock=%u", fps_lock);
    CHECK(parity != NULL);
    CHECK(suffix != NULL && suffix > parity);
    CHECK(strstr(s_banner, want) != NULL);
    CHECK(strstr(suffix + 1, " fps_lock=") == NULL);
    return 1;
}

static int check_duty_lock(void)
{
    static const uint32_t k_values[4] = { 1u, 2u, 3u, 0u };
    static const unsigned k_fps[4] = { 60u, 45u, 40u, 30u };
    static const uint32_t k_presents[4] = { 120u, 90u, 80u, 60u };
    static const uint32_t k_skips[4] = { 0u, 30u, 40u, 60u };
    char baseline[DUTY_TICKS + 1u];
    char transcript[DUTY_TICKS + 1u];
    char expected[DUTY_TICKS + 1u];
    KageVitaFullspeedSnapshot baseline_snapshot;
    KageVitaFullspeedSnapshot snapshot;
    KageVitaFullspeedSnapshot other;
    unsigned i;

    /* Default duty (never set): today's transcript; banner reports 60. */
    CHECK(run_duty_transcript(1u, baseline, &baseline_snapshot));
    expected_duty_transcript(1u, expected);
    CHECK(strcmp(baseline, expected) == 0);
    CHECK(baseline_snapshot.presents == 120u);
    CHECK(baseline_snapshot.game_updates == 60u);
    CHECK(baseline_snapshot.nonfull_skips == 0u);
    CHECK(s_log_calls == 1u);
    if (!check_banner_fps_lock(60u))
        return 0;

    for (i = 0u; i < 4u; ++i) {
        kage_vita_fullspeed_scheduler_set_duty((uint8_t)k_values[i]);
        CHECK(run_duty_transcript(1u, transcript, &snapshot));
        expected_duty_transcript(k_values[i], expected);
        if (strcmp(transcript, expected) != 0)
            fprintf(stderr, "duty k=%u\n got  %s\n want %s\n",
                    k_values[i], transcript, expected);
        CHECK(strcmp(transcript, expected) == 0);
        CHECK(snapshot.wrapper_ticks == DUTY_TICKS);
        CHECK(snapshot.game_updates == 60u);
        CHECK(snapshot.full_phases == 60u);
        CHECK(snapshot.nonfull_phases == 60u);
        CHECK(snapshot.interpolation_phases == 60u);
        CHECK(snapshot.presents == k_presents[i]);
        CHECK(snapshot.render_calls == k_presents[i]);
        CHECK(snapshot.render_bodies == k_presents[i]);
        CHECK(snapshot.nonfull_skips == k_skips[i]);
        CHECK(snapshot.render_skips == k_skips[i]);
        CHECK(snapshot.late_full_skips == 0u);
        CHECK(snapshot.runtime_disables == 0u);
        CHECK(snapshot.sequence_violations == 0u);
        CHECK(snapshot.parity_violations == 0u);
        CHECK(snapshot.duplicate_tick_violations == 0u);
        CHECK(s_log_calls == 1u);
        if (!check_banner_fps_lock(k_fps[i]))
            return 0;
        if (k_values[i] == 1u)
            CHECK(strcmp(transcript, baseline) == 0);
    }

    /* k = 0 is the existing interpolation-off skip: same transcript and
     * counters as the default duty with the guest byte at 0. */
    kage_vita_fullspeed_scheduler_set_duty(1u);
    CHECK(run_duty_transcript(0u, expected, &other));
    kage_vita_fullspeed_scheduler_set_duty(0u);
    CHECK(run_duty_transcript(1u, transcript, &snapshot));
    CHECK(strcmp(transcript, expected) == 0);
    CHECK(snapshot.wrapper_ticks == other.wrapper_ticks);
    CHECK(snapshot.presents == other.presents);
    CHECK(snapshot.game_updates == other.game_updates);
    CHECK(snapshot.nonfull_skips == other.nonfull_skips);
    CHECK(snapshot.render_skips == other.render_skips);

    /* Out-of-range values clamp to 1 (today's behaviour). */
    kage_vita_fullspeed_scheduler_set_duty(7u);
    CHECK(run_duty_transcript(1u, transcript, &snapshot));
    CHECK(strcmp(transcript, baseline) == 0);
    if (!check_banner_fps_lock(60u))
        return 0;
    kage_vita_fullspeed_scheduler_set_duty(255u);
    CHECK(run_duty_transcript(1u, transcript, &snapshot));
    CHECK(strcmp(transcript, baseline) == 0);

    /* reset/deactivate (the per-epoch clear) keep the boot-time duty. */
    kage_vita_fullspeed_scheduler_set_duty(2u);
    kage_vita_fullspeed_scheduler_deactivate();
    kage_vita_fullspeed_scheduler_reset();
    kage_vita_fullspeed_scheduler_deactivate();
    CHECK(run_duty_transcript(1u, transcript, &snapshot));
    expected_duty_transcript(2u, expected);
    CHECK(strcmp(transcript, expected) == 0);
    CHECK(snapshot.presents == 90u);
    if (!check_banner_fps_lock(45u))
        return 0;

    kage_vita_fullspeed_scheduler_set_duty(1u);
    return 1;
}

/* With the lock at 30 (k = 0) a wait failure disables the scheduler; from
 * then on plan_render returns 1 on every phase and the lock is gone (the
 * existing CADENCE30 DISABLE line is the receipt; no re-arm). */
static int check_duty_fail_open(void)
{
    ScenarioResult result;
    OracleGame game;
    KageVitaFullspeedSnapshot snapshot;
    uint32_t tick;
    uint32_t presents = 0u;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    s_now = 0u;
    s_log_calls = 0u;
    s_disable_log_calls = 0u;
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_set_duty(0u);
    kage_vita_fullspeed_scheduler_reset();
    for (tick = 0u; tick < 20u; ++tick)
        if (run_one_tick(&game, 2500u, 500u, 5000u, 1u, &result))
            ++presents;
    CHECK(presents == 10u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.nonfull_skips == 10u);
    CHECK(snapshot.runtime_disables == 0u);
    CHECK(result.limiter_fallbacks == 0u);

    s_delay_fail = 1u;
    kage_vita_fullspeed_scheduler_note_loop_head();
    s_delay_fail = 0u;
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(s_disable_log_calls == 1u);
    CHECK(!kage_vita_fullspeed_scheduler_bypass_limiter());

    presents = 0u;
    for (tick = 0u; tick < 20u; ++tick)
        if (run_one_tick(&game, 2500u, 500u, 5000u, 1u, &result))
            ++presents;
    CHECK(presents == 20u);
    kage_vita_fullspeed_scheduler_snapshot(&snapshot);
    CHECK(snapshot.nonfull_skips == 10u);
    CHECK(snapshot.runtime_disables == 1u);
    CHECK(result.limiter_fallbacks == 20u);

    kage_vita_fullspeed_scheduler_set_duty(1u);
    return 1;
}
#endif

#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
static int check_video_priority(void)
{
    /* Costs are synthetic stress inputs, not predictions of device FPS.
     * Both 35-ms service and 51-ms Render are seen in the owner capture. */
    const uint32_t render_costs[] = {1000u, 51000u};
    const uint32_t gameplay_costs[] = {5000u, 19000u, 34000u, 55000u};
    uint32_t duty, cost, tick;

    for (duty = 0u; duty < 4u; ++duty) {
        kage_vita_fullspeed_scheduler_set_duty((uint8_t)duty);
        for (cost = 0u; cost < 2u; ++cost) {
            OracleGame game = {0};
            KageVitaFullspeedSnapshot snapshot;
            ScenarioResult resumed = {0};
            uint32_t expected = 0u, nonfull = 0u;
            game.manager_pointer = 0x10000000u;
            game.game_pointer = 0x20000000u;
            game.game_frame = 15815u; /* captured File 2 save frame */
            s_now = 0u;
            s_delay_fail = 0u;
            kage_vita_fullspeed_scheduler_reset();
            for (tick = 0u; tick < 240u; ++tick) {
                int render, wanted;
                kage_vita_fullspeed_scheduler_note_loop_head();
                kage_vita_fullspeed_scheduler_note_service();
                s_now += tick % 17u ? 35000u : 200000u;
                kage_vita_fullspeed_scheduler_note_video_service();
                kage_vita_fullspeed_scheduler_note_manager_dispatch();
                kage_vita_fullspeed_scheduler_note_manager_entry(
                    game.manager_pointer, game.manager_counter, 1u,
                    game.game_pointer, game.game_frame);
                ++game.manager_counter;
                wanted = (game.manager_counter & 1u) != 0u;
                if (!wanted) {
                    ++nonfull;
                    wanted = duty != 0u && nonfull % duty == 0u;
                }
                render = kage_vita_fullspeed_scheduler_plan_render(
                    game.manager_pointer, game.manager_counter, 1u,
                    game.game_pointer, game.game_frame);
                CHECK(render == wanted);
                if (render) {
                    ++expected;
                    kage_vita_fullspeed_scheduler_note_render_entry();
                    kage_vita_fullspeed_scheduler_note_render_body(
                        game.manager_pointer, game.manager_counter, 1u);
                    s_now += render_costs[cost];
                    kage_vita_fullspeed_scheduler_note_present();
                    kage_vita_fullspeed_scheduler_note_render_return();
                }
            }
            kage_vita_fullspeed_scheduler_snapshot(&snapshot);
            CHECK(snapshot.presents == expected);
            CHECK(snapshot.full_phases == 120u && snapshot.nonfull_phases == 120u);
            CHECK(snapshot.late_full_skips == 0u && snapshot.game_updates == 0u);
            CHECK(snapshot.runtime_disables == 0u && snapshot.sequence_violations == 0u);
            /* Real gameplay resumes through its unchanged paced call seam. */
            for (tick = 0u; tick < 240u; ++tick)
                (void)run_one_tick(&game, 2500u, 500u, 5000u, 1u, &resumed);
            CHECK(resumed.game_update_time_count == 120u);
            CHECK(maximum_game_updates_per_second(resumed.game_update_times,
                                                  resumed.game_update_time_count) <=
                  ORACLE_MAX_UPDATES_PER_SECOND);
            kage_vita_fullspeed_scheduler_snapshot(&snapshot);
            CHECK(snapshot.runtime_disables == 0u);
        }
    }
    kage_vita_fullspeed_scheduler_set_duty(1u);

    /* A clip left alive during real gameplay must be completely inert on
     * BOTH phases, including overdue interpolation. Compare actual schedules. */
    for (cost = 0u; cost < 4u; ++cost) {
        ScenarioResult without_clip, with_clip;
        s_test_single_clip = 0u;
        without_clip = run_scenario(3000000u, gameplay_costs[cost], 1u, 33u, 200000u);
        s_test_single_clip = 1u;
        with_clip = run_scenario(3000000u, gameplay_costs[cost], 1u, 33u, 200000u);
        CHECK(memcmp(&without_clip, &with_clip, sizeof with_clip) == 0);
    }
    s_test_single_clip = 0u;

    /* Receipt expires at the next head, including empty-service and a late
     * internal-service attempt. No receipt bypasses the original late skip. */
    for (uint32_t late_note = 0u; late_note < 2u; ++late_note) {
        OracleGame game = {0};
        game.manager_pointer = 0x10000000u;
        s_now = 0u;
        kage_vita_fullspeed_scheduler_reset();
        for (tick = 0u; tick < 2u; ++tick) {
            int render;
            kage_vita_fullspeed_scheduler_note_loop_head();
            kage_vita_fullspeed_scheduler_note_service();
            s_now += 100000u;
            if (!tick) kage_vita_fullspeed_scheduler_note_video_service();
            kage_vita_fullspeed_scheduler_note_manager_dispatch();
            kage_vita_fullspeed_scheduler_note_manager_entry(
                game.manager_pointer, game.manager_counter, 1u, 0u, 0u);
            if (tick && late_note) kage_vita_fullspeed_scheduler_note_video_service();
            ++game.manager_counter;
            render = kage_vita_fullspeed_scheduler_plan_render(
                game.manager_pointer, game.manager_counter, 1u, 0u, 0u);
            CHECK(render == (tick == 0u));
            if (render) {
                kage_vita_fullspeed_scheduler_note_render_entry();
                kage_vita_fullspeed_scheduler_note_render_body(
                    game.manager_pointer, game.manager_counter, 1u);
                kage_vita_fullspeed_scheduler_note_present();
                kage_vita_fullspeed_scheduler_note_render_return();
            }
        }
    }
    return 1;
}
#endif

/* C2 transcripts: the display wait inside Present against the non-full
 * admission test (ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT).  Each
 * run warms up for 600 loops, then measures ten 120-loop windows, i.e. ten
 * ph120 windows: 0.5-ms service, full_us Game::Update, 0.6-ms non-full
 * phase, body_us Render body, then the modelled Present wait. */
#define PRESENT_WAIT_WINDOWS 10u
#define PRESENT_WAIT_TICKS 120u

typedef struct PresentWaitRun {
    uint32_t presents_min, presents_max, presents_total;
    uint32_t updates_min, updates_max;
    uint64_t wall_min_us, wall_max_us, wall_total_us;
    uint32_t nonfull_skips, late_full_skips, forced_renders;
    uint32_t runtime_disables, sequence_violations;
    uint32_t max_updates_per_second;
    uint64_t min_update_gap_us;
    uint64_t max_wait_us;
    uint32_t start_spacing_ok;
} PresentWaitRun;

static int run_present_wait(uint32_t kind, uint32_t edge_us, uint32_t wait_us,
                            uint32_t full_us, uint32_t body_us,
                            PresentWaitRun *out)
{
    static ScenarioResult result;
    OracleGame game;
    KageVitaFullspeedSnapshot before, after;
    uint32_t window, tick, first, i;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    memset(out, 0, sizeof *out);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    game.game_frame = 100u;
    s_now = 1000u;
    s_log_calls = 0u;
    s_disable_log_calls = 0u;
    s_delay_fail = 0u;
    s_present_wait_kind = kind;
    s_present_wait_edge_us = edge_us;
    s_present_wait_us = wait_us;
    s_present_wait_count = 0u;
    memset(s_present_wait_gpu_done, 0, sizeof s_present_wait_gpu_done);
    memset(s_present_wait_flip, 0, sizeof s_present_wait_flip);
    kage_vita_fullspeed_scheduler_reset();
    for (tick = 0u; tick < 600u; ++tick)
        (void)run_one_tick(&game, full_us, 600u, body_us, 1u, &result);
    first = result.game_update_time_count;
    s_present_wait_max_us = 0u;
    out->presents_min = out->updates_min = UINT32_MAX;
    out->wall_min_us = UINT64_MAX;
    for (window = 0u; window < PRESENT_WAIT_WINDOWS; ++window) {
        uint64_t start = s_now;
        uint32_t presents, updates;

        kage_vita_fullspeed_scheduler_snapshot(&before);
        for (tick = 0u; tick < PRESENT_WAIT_TICKS; ++tick)
            (void)run_one_tick(&game, full_us, 600u, body_us, 1u, &result);
        kage_vita_fullspeed_scheduler_snapshot(&after);
        presents = after.presents - before.presents;
        updates = after.game_updates - before.game_updates;
        out->presents_total += presents;
        if (presents < out->presents_min) out->presents_min = presents;
        if (presents > out->presents_max) out->presents_max = presents;
        if (updates < out->updates_min) out->updates_min = updates;
        if (updates > out->updates_max) out->updates_max = updates;
        out->wall_total_us += s_now - start;
        if (s_now - start < out->wall_min_us) out->wall_min_us = s_now - start;
        if (s_now - start > out->wall_max_us) out->wall_max_us = s_now - start;
        out->nonfull_skips += after.nonfull_skips - before.nonfull_skips;
        out->late_full_skips += after.late_full_skips - before.late_full_skips;
        out->forced_renders += after.forced_renders - before.forced_renders;
    }
    out->runtime_disables = after.runtime_disables;
    out->sequence_violations = after.sequence_violations;
    out->max_updates_per_second = maximum_game_updates_per_second(
        result.game_update_times + first, result.game_update_time_count - first);
    out->min_update_gap_us = UINT64_MAX;
    for (i = first > 0u ? first : 1u; i < result.game_update_time_count; ++i)
        if (result.game_update_times[i] - result.game_update_times[i - 1u] <
                out->min_update_gap_us)
            out->min_update_gap_us =
                result.game_update_times[i] - result.game_update_times[i - 1u];
    out->max_wait_us = s_present_wait_max_us;
    out->start_spacing_ok = (uint32_t)oracle_start_spacing_holds(
        result.game_update_times + first, result.game_update_time_count - first);
    s_present_wait_kind = PRESENT_WAIT_NONE;
    return result.limiter_fallbacks == 0u &&
        result.game_update_time_count < ORACLE_MAX_GAME_UPDATES;
}

typedef struct PresentWaitCase {
    uint32_t kind, edge_us, wait_us, full_us, body_us;
    uint32_t presents, presents_min, presents_max;
    uint32_t nonfull_skips, late_full_skips;
    uint64_t wall_us;
} PresentWaitCase;

/* Ten-window totals, measured with this oracle in the service-reserve
 * configuration (the int2j recipe, head advance OFF); other configurations
 * check the invariants, the tour transcript and the heavy row only.
 *  - EDGE, 17 phases: every fifth Present waits for the next 60-Hz edge.
 *    Phase 4500 (the 18th row) is the task transcript: a 10.566-ms wait,
 *    100 presents per 120 loops with skip(a,l,n,f)=20,0,20,0 as in int2j
 *    tour 3 (2026-09-17 issue re-check) and 120 with the knob, both at
 *    exactly 2,000,000 us per window.
 *  - FIXED: every fifth Present waits a fixed time.  The tour's windows fit
 *    this better (rnd95 - rnd50 stays ~10.7 ms while rnd50 moves 5.5..7.3 ms;
 *    an edge wait would keep rnd95 itself constant).  With the knob the frame
 *    after a wait is admitted, a later wait lands on an interpolated frame
 *    and the Game::Update after it starts late: 1200 presents but
 *    20,184,200 us at 10.6 ms (29.73 UPS) against 1000 in 20,000,200.
 *  - QUEUE: two frames in flight, no vblank.  Every Present waits at 12 ms
 *    (unchanged); GPU-bound at 19 ms the knob costs 38 ms per 20 s.
 *  - QUEUE_VBLANK 19 ms: the knob admits frames the GPU cannot finish,
 *    1114 presents in 21.43 s (51.98 FPS, 27.99 UPS) against 1050 in
 *    20.00 s (52.50 FPS, 30.00 UPS): presents per 120 loops rise while real
 *    FPS and UPS fall.
 *  - NONE, 19-ms body: interpolation stays discarded either way. */
static const PresentWaitCase k_present_wait_cases[] = {
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    { PRESENT_WAIT_EDGE, 0u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 1000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 2000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 3000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 5000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 6000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 7000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 8000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 9000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 10000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 11000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 12000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 13000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 14000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 15000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 16000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4500u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 8000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 9000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 10000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20112200u },
    { PRESENT_WAIT_FIXED, 0u, 11000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20232200u },
    { PRESENT_WAIT_FIXED, 0u, 12000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20352200u },
    { PRESENT_WAIT_FIXED, 0u, 13000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20472200u },
    { PRESENT_WAIT_FIXED, 0u, 14000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20592200u },
    { PRESENT_WAIT_FIXED, 0u, 15000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20712200u },
    { PRESENT_WAIT_FIXED, 0u, 16000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20832200u },
    { PRESENT_WAIT_FIXED, 0u, 17000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 18000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 19000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 20000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 10600u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20184200u },
    { PRESENT_WAIT_QUEUE, 0u, 12000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_QUEUE, 0u, 17000u, 2500u, 6500u, 1188u, 118u, 119u, 0u, 12u, 20192730u },
    { PRESENT_WAIT_QUEUE, 0u, 19000u, 2500u, 6500u, 1059u, 105u, 106u, 141u, 0u, 20121000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 16000u, 3000u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 18000u, 3000u, 6500u, 1145u, 114u, 115u, 55u, 0u, 20916667u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 19000u, 3000u, 6500u, 1114u, 111u, 112u, 86u, 0u, 21433334u },
    { PRESENT_WAIT_NONE, 0u, 0u, 2500u, 19000u, 600u, 60u, 60u, 600u, 0u, 20000200u },
#else
    { PRESENT_WAIT_EDGE, 0u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 1000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 2000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 3000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 5000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 6000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 7000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 8000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 9000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 10000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 11000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 12000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 13000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 14000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 15000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 16000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4500u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 8000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 9000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 10000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 11000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 12000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 13000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 14000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 15000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 16000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 17000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 18000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 19000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 20000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_FIXED, 0u, 10600u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000200u },
    { PRESENT_WAIT_QUEUE, 0u, 12000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_QUEUE, 0u, 17000u, 2500u, 6500u, 1188u, 118u, 119u, 0u, 12u, 20192730u },
    { PRESENT_WAIT_QUEUE, 0u, 19000u, 2500u, 6500u, 1057u, 105u, 106u, 143u, 0u, 20083000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 16000u, 3000u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000200u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 18000u, 3000u, 6500u, 1146u, 114u, 115u, 54u, 0u, 20915567u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 19000u, 3000u, 6500u, 1050u, 105u, 105u, 150u, 0u, 20000000u },
    { PRESENT_WAIT_NONE, 0u, 0u, 2500u, 19000u, 600u, 60u, 60u, 600u, 0u, 20000200u },
#endif
};

#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2 && \
    ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US == 8000 && \
    defined(ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US) && \
    ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US == 2000
/* The same 39 cases with FLOOR_DEBT=ON, cap 8000 us, admit slack 2000 us,
 * measured with this oracle in the service-reserve configuration (the only
 * one pinned exactly).  Against k_present_wait_cases, presents and Game
 * updates per second over the ten windows:
 *  - inclusive cost: FIXED 9 ms 1000 -> 1200 presents at 30.00 UPS; QUEUE
 *    17 ms 1188 -> 1177 presents, 58.83 FPS both, 29.71 -> 29.99 UPS;
 *    QUEUE_VBLANK 18 ms 54.79 -> 55.00 FPS, 28.69 -> 30.00 UPS;
 *  - C2 (render-cost=excludes-present): FIXED 10..16 ms keep 1200 presents
 *    and go from 29.83..28.80 to 30.00 UPS; FIXED 17 ms 1000 -> 1200 at
 *    30.00 UPS; FIXED 18 ms 1000 -> 1200 presents but 30.00 -> 29.83 UPS
 *    (an admitted frame's Present wait overruns the cap: the one case here
 *    where ON lowers UPS); QUEUE_VBLANK 19 ms 51.98 -> 52.60 FPS, 27.99 ->
 *    30.00 UPS.
 * Every other case presents the same; the walls lose the stock 200 us of
 * clock quantisation (20,000,200 -> 20,000,000). */
# define ORACLE_PRESENT_WAIT_DEBT_TABLE 1
static const PresentWaitCase k_present_wait_debt_cases[] = {
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    { PRESENT_WAIT_EDGE, 0u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 1000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 2000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 3000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 5000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 6000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 7000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 8000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 9000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 10000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 11000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 12000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 13000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 14000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 15000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 16000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4500u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 8000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 9000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 10000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 11000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 12000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 13000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 14000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 15000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 16000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 17000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 18000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20112080u },
    { PRESENT_WAIT_FIXED, 0u, 19000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 20000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 10600u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE, 0u, 12000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE, 0u, 17000u, 2500u, 6500u, 1177u, 117u, 118u, 23u, 0u, 20006566u },
    { PRESENT_WAIT_QUEUE, 0u, 19000u, 2500u, 6500u, 1052u, 105u, 106u, 148u, 0u, 19988000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 16000u, 3000u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 18000u, 3000u, 6500u, 1111u, 111u, 112u, 89u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 19000u, 3000u, 6500u, 1052u, 105u, 106u, 148u, 0u, 20001100u },
    { PRESENT_WAIT_NONE, 0u, 0u, 2500u, 19000u, 600u, 60u, 60u, 600u, 0u, 20000000u },
#else
    { PRESENT_WAIT_EDGE, 0u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 1000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 2000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 3000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 5000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 6000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 7000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 8000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 9000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 10000u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 11000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 12000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 13000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 14000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 15000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 16000u, 0u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_EDGE, 4500u, 0u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 8000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 9000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 10000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 11000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 12000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 13000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 14000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 15000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 16000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 17000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 18000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 19000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 20000u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_FIXED, 0u, 10600u, 2500u, 6500u, 1000u, 100u, 100u, 200u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE, 0u, 12000u, 2500u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE, 0u, 17000u, 2500u, 6500u, 1177u, 117u, 118u, 23u, 0u, 20006566u },
    { PRESENT_WAIT_QUEUE, 0u, 19000u, 2500u, 6500u, 1052u, 105u, 106u, 148u, 0u, 19988000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 16000u, 3000u, 6500u, 1200u, 120u, 120u, 0u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 18000u, 3000u, 6500u, 1100u, 110u, 110u, 100u, 0u, 20000000u },
    { PRESENT_WAIT_QUEUE_VBLANK, 5000u, 19000u, 3000u, 6500u, 1050u, 105u, 105u, 150u, 0u, 20000000u },
    { PRESENT_WAIT_NONE, 0u, 0u, 2500u, 19000u, 600u, 60u, 60u, 600u, 0u, 20000000u },
#endif
};
#endif

static int check_present_wait(void)
{
#if defined(ORACLE_PRESENT_WAIT_DEBT_TABLE)
    const PresentWaitCase *cases = k_present_wait_debt_cases;
    const uint32_t count = (uint32_t)(sizeof k_present_wait_debt_cases /
                                      sizeof k_present_wait_debt_cases[0]);
#else
    const PresentWaitCase *cases = k_present_wait_cases;
    const uint32_t count =
        (uint32_t)(sizeof k_present_wait_cases / sizeof k_present_wait_cases[0]);
#endif
    uint32_t i;

    CHECK(count == 39u);
    for (i = 0u; i < count; ++i) {
        const PresentWaitCase *c = &cases[i];
        PresentWaitRun run;
        int exact;

        s_render_cost_banner_calls = 0u;
        CHECK(run_present_wait(c->kind, c->edge_us, c->wait_us, c->full_us,
                               c->body_us, &run));
        /* Everywhere: 60 Game updates per window, no replay above 30 UPS
         * (31 starts per sliding second under FLOOR_DEBT=ON), no update
         * closer than one floor (minus the cap under ON), the cumulative
         * start contract, no fail-open, one banner. */
        CHECK(run.updates_min == 60u && run.updates_max == 60u);
        CHECK(run.max_updates_per_second <= ORACLE_MAX_UPDATES_PER_SECOND);
        CHECK(run.min_update_gap_us >= ORACLE_MIN_UPDATE_GAP_US);
        CHECK(run.start_spacing_ok);
        CHECK(run.runtime_disables == 0u && run.sequence_violations == 0u);
        CHECK(s_log_calls == 1u);
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
        CHECK(s_render_cost_banner_calls == 1u);
#else
        CHECK(s_render_cost_banner_calls == 0u);
#endif
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2 && \
    !defined(ORACLE_PRESENT_WAIT_DEBT_TABLE)
        /* FLOOR_DEBT=ON with another cap or slack: invariants only. */
        exact = 0;
        (void)c;
#elif defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE) && \
    !(defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE)
        exact = 1;
#else
        /* The tour transcript and the heavy row do not depend on the
         * reserve or head-advance policy (measured in all seven harness
         * configurations); the heavy row's wall does. */
        exact = i == 17u;
        if (i == count - 1u) {
            CHECK(run.presents_total == c->presents);
            CHECK(run.nonfull_skips == c->nonfull_skips);
        }
#endif
        if (!exact)
            continue;
        if (run.presents_total != c->presents ||
                run.presents_min != c->presents_min ||
                run.presents_max != c->presents_max ||
                run.nonfull_skips != c->nonfull_skips ||
                run.late_full_skips != c->late_full_skips ||
                run.wall_total_us != c->wall_us)
            fprintf(stderr,
                    "present-wait case %u kind=%u edge=%u wait=%u: presents "
                    "%u [%u..%u] skip(n,f)=%u,%u wall %llu us\n",
                    i, c->kind, c->edge_us, c->wait_us, run.presents_total,
                    run.presents_min, run.presents_max, run.nonfull_skips,
                    run.late_full_skips, (unsigned long long)run.wall_total_us);
        CHECK(run.presents_total == c->presents);
        CHECK(run.presents_min == c->presents_min);
        CHECK(run.presents_max == c->presents_max);
        CHECK(run.nonfull_skips == c->nonfull_skips);
        CHECK(run.late_full_skips == c->late_full_skips);
        CHECK(run.wall_total_us == c->wall_us);
#if defined(ORACLE_PRESENT_WAIT_DEBT_TABLE)
        {
            /* Against the stock row: presents per second never lower (to
             * 0.01/s), Game updates per second never lower (to 0.001/s)
             * except the one C2 case named above. */
            const PresentWaitCase *stock = &k_present_wait_cases[i];
            double fps = run.presents_total * 1e6 / (double)run.wall_total_us;
            double ups = 600.0 * 1e6 / (double)run.wall_total_us;
            double stock_fps = stock->presents * 1e6 / (double)stock->wall_us;
            double stock_ups = 600.0 * 1e6 / (double)stock->wall_us;

            CHECK(stock->kind == c->kind && stock->edge_us == c->edge_us &&
                  stock->wait_us == c->wait_us);
            CHECK(fps >= stock_fps - 0.01);
# if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
            if (i == 28u)
                CHECK(ups < stock_ups - 0.1 && ups > 29.8);
            else
# endif
                CHECK(ups >= stock_ups - 0.001);
        }
#endif
    }
    CHECK(cases[17].kind == PRESENT_WAIT_EDGE && cases[17].edge_us == 4500u);
    return 1;
}

#if !(defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE)
/* S5 floor-debt scenarios.  They run in every build
 * without full-head advance, OFF included, and print one line per scenario
 * with a digest of every Game::Update start and every Render decision of the
 * measured window, so test_kage_vita_fullspeed_scheduler.py can prove
 * OBSERVE identical to OFF and compare ON with OFF in one define set.
 * Costs are synthetic stress inputs, not predictions of device FPS. */
typedef struct DebtPhase {
    uint32_t duration_us;
    uint32_t service_us, service_jitter_us; /* full ticks, before the pace */
    uint32_t full_us, nonfull_us;
    uint32_t render_us, render_jitter_us;
    uint32_t interpolation_enabled;
    uint32_t menu; /* no Game::Update at all (menu, pause screen) */
    /* Extra service before the pace of the n-th full tick of the phase
     * (1-based, 0 = none), extra time inside the n-th Game::Update, and
     * extra Render time on the n-th wrapper tick. */
    uint32_t stall_full, stall_us;
    uint32_t stall_update, stall_update_us;
    uint32_t stall_render_tick, stall_render_us;
    /* Wake overshoot of every loop-head and every pace sleep in the phase
     * (sceKernelDelayThread above); a full tick whose pace comes right
     * after its loop head; from the n-th full tick on (1-based,
     * 0 = never), one extra overshoot on the next pace sleep; on the n-th
     * full tick, a head sleep that returns with the clock 1 us before it
     * started; before the n-th full tick's loop head, the clock set 1 us
     * before the previous Render gate's reading (a loop-head clock reset). */
    uint32_t head_os_us, pace_os_us;
    uint32_t short_full; /* the n-th full tick has no service before its pace */
    uint32_t pace_extra_full, pace_extra_us;
    uint32_t head_back_full;
    uint32_t clock_back_full;
    /* The pace of the exact_full-th full tick starts exact_after_us after
     * the Game::Update start of the exact_ref_full-th (its service is
     * stretched to that instant; 0 = off). */
    uint32_t exact_ref_full, exact_full, exact_after_us;
} DebtPhase;

typedef struct DebtRun {
    uint32_t updates, presents, ticks;
    /* From the last Game::Update start before the window to the last one in
     * it: `updates` floor periods plus whatever the floor dropped. */
    uint64_t wall_us;
    uint32_t max_per_second;
    uint32_t spacing_ok;
    uint64_t digest;
    uint32_t disables, violations, dropped;
    /* whole run: the extra pace overshoot, backward head sleep and loop-head
     * clock reset that really happened (a scenario must not pass because
     * its trigger never fired) */
    uint32_t pace_extra_hits, head_back_hits, clock_back_hits;
    /* the measured phase's pace-wake overshoot: a boundary start that woke
     * from a floor wait is that much after its floor */
    uint32_t pace_os_us;
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    KageVitaFullspeedFloorDebtSnapshot debt; /* the window's paces */
    /* the floor stamp of the take that opened the window (debt.floor_us is
     * the closing one) */
    uint64_t floor_start_us;
#endif
    /* the window's first Game::Update start, whether its pace slept, and
     * the wake overshoot (+ extra) that sleep added */
    uint64_t first_start_us;
    uint32_t first_slept, first_wake_us;
} DebtRun;

static uint32_t s_debt_lcg;
/* Clock reading of the latest Render gate (plan_render), the scheduler's
 * last clock before the next loop head. */
static uint64_t s_debt_last_gate_us;
/* Armed by run_debt for one full tick: its pace starts at this instant. */
static uint64_t s_debt_pace_at_us;

static uint32_t debt_rand(uint32_t range)
{
    if (range == 0u)
        return 0u;
    s_debt_lcg = s_debt_lcg * 1664525u + 1013904223u;
    return (s_debt_lcg >> 8) % (range + 1u);
}

static uint64_t debt_fnv(uint64_t hash, uint64_t value)
{
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        hash ^= (value >> (8u * i)) & 0xffu;
        hash *= 1099511628211ull;
    }
    return hash;
}

/* run_one_tick with a per-tick service before Game::Update and a per-tick
 * Render cost, and no Present wait; otherwise the same seam sequence. */
static int run_debt_tick(OracleGame *game, uint32_t service_us,
                         uint32_t full_us, uint32_t nonfull_us,
                         uint32_t render_us, uint32_t interpolation_enabled,
                         uint32_t menu, ScenarioResult *result,
                         uint64_t *start_us)
{
    int render;

    s_debt_wait_site = DEBT_WAIT_HEAD;
    kage_vita_fullspeed_scheduler_note_loop_head();
    s_debt_wait_site = DEBT_WAIT_NONE;
    kage_vita_fullspeed_scheduler_note_service();
    s_now += service_us;
    if (s_debt_pace_at_us) {
        if (s_now < s_debt_pace_at_us)
            s_now = s_debt_pace_at_us;
        s_debt_pace_at_us = 0u;
    }
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        game->manager_pointer, game->manager_counter,
        interpolation_enabled, game->game_pointer, game->game_frame);
    *start_us = 0u;
    if ((game->manager_counter & 1u) == 0u && !menu) {
        s_debt_wait_site = DEBT_WAIT_PACE;
        kage_vita_fullspeed_scheduler_note_game_update_begin(
            game->manager_counter, game->game_pointer, game->game_frame);
        s_debt_wait_site = DEBT_WAIT_NONE;
        if (result->game_update_time_count < ORACLE_MAX_GAME_UPDATES)
            result->game_update_times[result->game_update_time_count++] =
                s_now;
        *start_us = s_now;
        s_now += full_us;
        ++game->game_frame;
        kage_vita_fullspeed_scheduler_note_game_update_end(
            game->game_pointer, game->game_frame);
    } else {
        s_now += nonfull_us;
    }
    ++game->manager_counter;
    s_debt_last_gate_us = s_now;
    render = kage_vita_fullspeed_scheduler_plan_render(
        game->manager_pointer, game->manager_counter,
        interpolation_enabled, game->game_pointer, game->game_frame);
    if (render) {
        kage_vita_fullspeed_scheduler_note_render_entry();
        kage_vita_fullspeed_scheduler_note_render_body(
            game->manager_pointer, game->manager_counter,
            interpolation_enabled);
        s_now += render_us;
        kage_vita_fullspeed_scheduler_note_present();
        kage_vita_fullspeed_scheduler_note_render_return();
    }
    if (kage_vita_fullspeed_scheduler_bypass_limiter())
        ++result->limiter_bypasses;
    else
        ++result->limiter_fallbacks;
    return render;
}

static void debt_clear_wake_hooks(void)
{
    s_debt_wait_site = DEBT_WAIT_NONE;
    s_debt_head_os_us = s_debt_pace_os_us = 0u;
    s_debt_pace_extra_us = s_debt_pace_extra_hits = 0u;
    s_debt_head_backward = s_debt_head_backward_hits = 0u;
    s_debt_pace_sleeps = s_debt_pace_wake_us = 0u;
    s_debt_pace_at_us = 0u;
}

/* The phases run in order from a fresh reset; the last one is measured. */
static int run_debt(const DebtPhase *phases, uint32_t count, uint32_t seed,
                    DebtRun *out)
{
    static ScenarioResult result;
    OracleGame game;
    KageVitaFullspeedSnapshot before, after;
    uint64_t last_start = 0u, window_prev_start = 0u;
    uint32_t phase;

    memset(&result, 0, sizeof result);
    memset(&game, 0, sizeof game);
    memset(&before, 0, sizeof before);
    memset(out, 0, sizeof *out);
    game.manager_pointer = 0x10000000u;
    game.game_pointer = 0x20000000u;
    game.game_frame = 100u;
    s_now = 1000u;
    s_log_calls = 0u;
    s_disable_log_calls = 0u;
    s_delay_fail = 0u;
    s_present_wait_kind = PRESENT_WAIT_NONE;
    s_debt_lcg = seed;
    debt_clear_wake_hooks();
    s_debt_last_gate_us = 0u;
    out->digest = 1469598103934665603ull;
    kage_vita_fullspeed_scheduler_reset();
    for (phase = 0u; phase < count; ++phase) {
        const DebtPhase *p = &phases[phase];
        uint64_t end = s_now + p->duration_us;
        uint64_t exact_ref_start = 0u;
        uint32_t full_index = 0u, tick_index = 0u;
        int window = phase + 1u == count;

        s_debt_head_os_us = p->head_os_us;
        s_debt_pace_os_us = p->pace_os_us;
        if (window) {
            out->pace_os_us = p->pace_os_us;
            kage_vita_fullspeed_scheduler_snapshot(&before);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            kage_vita_fullspeed_scheduler_floor_debt_take(&out->debt);
            out->floor_start_us = out->debt.floor_us;
            memset(&out->debt, 0, sizeof out->debt);
#endif
            window_prev_start = last_start;
            result.game_update_time_count = 0u;
        }
        while (s_now < end) {
            uint32_t full = (game.manager_counter & 1u) == 0u;
            uint32_t service = 500u;
            uint32_t render_us = p->render_us + debt_rand(p->render_jitter_us);
            uint64_t start_us;
            uint32_t pace_sleeps = s_debt_pace_sleeps;
            int render;

            ++tick_index;
            if (full) {
                ++full_index;
                service = p->service_us + debt_rand(p->service_jitter_us);
                if (full_index == p->short_full)
                    service = 0u;
                if (full_index == p->stall_full)
                    service += p->stall_us;
                if (full_index == p->pace_extra_full)
                    s_debt_pace_extra_us = p->pace_extra_us;
                if (full_index == p->head_back_full)
                    s_debt_head_backward = 1u;
                if (full_index == p->clock_back_full &&
                        s_debt_last_gate_us > 1u) {
                    s_now = s_debt_last_gate_us - 1u;
                    ++out->clock_back_hits;
                }
                if (full_index == p->exact_full && exact_ref_start)
                    s_debt_pace_at_us = exact_ref_start + p->exact_after_us;
            }
            if (tick_index == p->stall_render_tick)
                render_us += p->stall_render_us;
            render = run_debt_tick(&game, service,
                                   p->full_us + (full && full_index ==
                                       p->stall_update ? p->stall_update_us : 0u),
                                   p->nonfull_us,
                                   render_us, p->interpolation_enabled,
                                   p->menu, &result, &start_us);
            if (start_us)
                last_start = start_us;
            if (full && full_index == p->exact_ref_full)
                exact_ref_start = start_us;
            if (window) {
                ++out->ticks;
                if (start_us && out->updates == 0u) {
                    out->first_start_us = start_us;
                    out->first_slept = s_debt_pace_sleeps != pace_sleeps;
                    out->first_wake_us = out->first_slept ?
                        s_debt_pace_wake_us : 0u;
                }
                if (start_us) {
                    ++out->updates;
                    out->digest = debt_fnv(out->digest, start_us);
                }
                if (render)
                    ++out->presents;
                out->digest = debt_fnv(out->digest,
                                       ((uint64_t)tick_index << 1) |
                                       (uint64_t)(render != 0));
            }
        }
    }
    kage_vita_fullspeed_scheduler_snapshot(&after);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    kage_vita_fullspeed_scheduler_floor_debt_take(&out->debt);
#endif
    out->pace_extra_hits = s_debt_pace_extra_hits;
    out->head_back_hits = s_debt_head_backward_hits;
    debt_clear_wake_hooks();
    out->wall_us = last_start - window_prev_start;
    out->max_per_second = maximum_game_updates_per_second(
        result.game_update_times, result.game_update_time_count);
    out->spacing_ok = (uint32_t)oracle_start_spacing_holds(
        result.game_update_times, result.game_update_time_count);
    out->disables = after.runtime_disables;
    out->violations = after.sequence_violations + after.parity_violations +
        after.duplicate_tick_violations + after.game_frame_violations;
    out->dropped = after.dropped_ticks - before.dropped_ticks;
    return result.limiter_fallbacks == 0u && window_prev_start != 0u &&
        out->updates > 1u &&
        result.game_update_time_count < ORACLE_MAX_GAME_UPDATES;
}

/* One line per scenario: behaviour fields (equal OFF/OBSERVE) and, with the
 * knob, the receipt. */
static void print_debt_run(const char *name, const DebtRun *run)
{
    /* updates per second of the window, in thousandths */
    uint32_t ups_milli = run->wall_us ?
        (uint32_t)(((uint64_t)run->updates * 1000000000ull +
                    run->wall_us / 2u) / run->wall_us) : 0u;

    printf("Vita floor-debt scenario %s: updates=%u presents=%u ticks=%u "
           "wall_us=%llu ups=%u.%03u drop=%u digest=%016llx",
           name, run->updates, run->presents, run->ticks,
           (unsigned long long)run->wall_us, ups_milli / 1000u,
           ups_milli % 1000u, run->dropped,
           (unsigned long long)run->digest);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    printf(" p=%u late=%u,%u,%u kept=%u over=%u,%u clamp=%u,%u stale=%u,%u,%u "
           "gf=%u,%u,%d,%d slack=%u,%u",
           run->debt.paces, run->debt.late, run->debt.late_us,
           run->debt.late_max_us, run->debt.kept_us, run->debt.over,
           run->debt.over_us, run->debt.clamp, run->debt.clamp_us,
           run->debt.stale, run->debt.stale_grid,
           run->debt.stale_us, run->debt.gf_n, run->debt.gf_pos,
           (int)run->debt.gf_max_us, (int)run->debt.gf_min_us,
           run->debt.slack_band, run->debt.slack_late);
#endif
    printf("\n");
}

/* Prints the scenario line and checks what holds in this build's mode. */
static int check_debt_run(const char *name, const DebtRun *run)
{
    print_debt_run(name, run);
    CHECK(run->disables == 0u && run->violations == 0u);
    CHECK(run->spacing_ok);
    CHECK(run->max_per_second <= ORACLE_MAX_UPDATES_PER_SECOND);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    {
        const KageVitaFullspeedFloorDebtSnapshot *d = &run->debt;
        uint32_t hist = 0u, i;
        /* thirds of a microsecond; each pace rounds its lateness up by at
         * most 2 units and a floor wake by at most 2 more; a boundary start
         * that woke from a floor wait is the pace-wake overshoot after its
         * floor (filed under neither late nor stale) */
        uint64_t wall = 3u * run->wall_us;
        uint64_t periods = (uint64_t)run->updates *
            KAGE_VITA_FULLSPEED_GAME_TICK_UNITS;
        uint64_t slop = 4u * (uint64_t)run->updates + 3u +
            3u * (uint64_t)run->pace_os_us;

        for (i = 0u; i < 7u; ++i)
            hist += d->late_hist[i];
        CHECK(hist == d->late);
        CHECK(d->paces == run->updates);
        CHECK(d->late + d->stale <= d->paces);
        CHECK(d->stale_grid <= d->stale);
        /* The cap is whole microseconds, so the rounded sums add up. */
        CHECK(d->over <= d->late);
        CHECK(d->late_us == d->kept_us + d->over_us);
        CHECK(d->late_max_us <= 33334u);
        CHECK(d->gf_n == run->updates);
        /* The clamp's share of over.  With no pace late by
         * more than the cap, min(late, cap) = late, so everything over is
         * the clamp's (kept_us + clamp_us = sum min(late, cap)). */
        CHECK(d->clamp <= d->over && d->clamp_us <= d->over_us);
        if (d->late_max_us <= ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US)
            CHECK(d->clamp == d->over && d->clamp_us == d->over_us);
        /* The floor stamps of the two takes around the window
         * give the identity exactly -- no wake, boundary or cap slop: each
         * pace moves the floor one period plus the lateness it drops, and
         * each rounded us sum and each stamp lose at most 2 units.  The
         * design-draft control (NO_STALE_CUT) keeps debt from stale paces
         * that its receipt files as stale, so it is pinned to its
         * behavioural symptom instead. */
#  if !defined(ISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_STALE_CUT)
        {
            uint64_t got, want = (uint64_t)d->paces *
                KAGE_VITA_FULLSPEED_GAME_TICK_UNITS;

            CHECK(run->floor_start_us != 0u && d->floor_us > run->floor_start_us);
            /* The stamp is the floor the window's first pace meets, in
             * whole us rounded down: a pace that slept wakes at the floor
             * rounded up plus its overshoot, one that did not starts at or
             * after it. */
            if (run->first_slept) {
                uint64_t woke = run->first_start_us - run->first_wake_us;
                if (woke != run->floor_start_us &&
                        woke != run->floor_start_us + 1u)
                    fprintf(stderr, "floor stamp %llu, first pace woke at "
                            "%llu\n", (unsigned long long)run->floor_start_us,
                            (unsigned long long)woke);
                CHECK(woke == run->floor_start_us ||
                      woke == run->floor_start_us + 1u);
            } else {
                CHECK(run->first_start_us >= run->floor_start_us);
            }
            got = 3u * (d->floor_us - run->floor_start_us);
# if ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 1
            want += 3u * ((uint64_t)d->late_us + d->stale_us);
# else
            want += 3u * ((uint64_t)d->over_us + d->stale_us);
# endif
            if (got + 2u * (uint64_t)d->paces + 2u < want || got > want + 2u)
                fprintf(stderr, "floor stamp identity: got %llu want %llu units "
                        "(paces %u)\n", (unsigned long long)got,
                        (unsigned long long)want, d->paces);
            CHECK(got + 2u * (uint64_t)d->paces + 2u >= want && got <= want + 2u);
        }
#  endif
# if ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 1
        /* OBSERVE: the floor drops all lateness, as in stock builds. */
        {
            uint64_t want = periods + 3u * ((uint64_t)d->late_us + d->stale_us);
            CHECK(wall + slop >= want && wall <= want + slop);
        }
# else
        /* ON: the floor drops only what is over the cap and stale lateness;
         * the boundary paces move one kept amount across the window. */
        {
            uint64_t want = periods + 3u * ((uint64_t)d->over_us + d->stale_us);
            uint64_t cap = 3u * (uint64_t)ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US;
            CHECK(wall + slop + cap >= want && wall <= want + slop + cap);
        }
# endif
    }
#endif
    return 1;
}

/* Rooms: duration, service (+jitter), full update, non-full phase, Render
 * (+jitter), interpolation, menu, stalls. */
#define DEBT_ROOM(d, svc, svcj, full, rend, rendj, interp) \
    { d, svc, svcj, full, 600u, rend, rendj, interp, 0u, 0u, 0u, 0u, 0u,       0u, 0u, \
      0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u }
/* Light room whose service and Render vary per tick: the device's ordinary
 * late paces. */
#define DEBT_JITTER(d) DEBT_ROOM(d, 500u, 2500u, 6000u, 5000u, 3000u, 1u)
/* Interpolated frames fit with about 1 ms to spare while the floor sits
 * one service time behind the full head's grid deadline (gf = -500 us); a
 * floor more than ~0.5 ms ahead of the grid (gf > 0) refuses every one.
 * The 0.5-ms update equals the 0.5-ms service, so the stock reserve (last
 * update body) and the service reserve (pre-Game service) are the same. */
#define DEBT_EDGE(d) DEBT_ROOM(d, 500u, 0u, 500u, 14500u, 0u, 1u)
/* Render-bound, interpolation off: the grid falls behind until the stale
 * wrapper drop fires, while every pace is late by less than one tick. */
#define DEBT_HEAVY(d) DEBT_ROOM(d, 500u, 0u, 3000u, 42000u, 0u, 0u)
/* A 26-ms loop: it catches up the grid without ever waiting on the floor, so
 * whatever gf the heavy room left is carried, unchanged, into the next room. */
#define DEBT_MEDIUM(d) DEBT_ROOM(d, 500u, 0u, 3000u, 22000u, 0u, 1u)

static int run_debt_named(const char *name, const DebtPhase *phases,
                          uint32_t count, uint32_t seed, DebtRun *run)
{
    if (!run_debt(phases, count, seed, run)) {
        fprintf(stderr, "floor-debt scenario %s: run failed\n", name);
        return 0;
    }
    if (!check_debt_run(name, run)) {
        fprintf(stderr, "floor-debt scenario %s: check failed\n", name);
        return 0;
    }
    return 1;
}

/* The window of an edge room after something else must equal the fresh
 * room's: the same presents and updates, no lateness, gf never above 0. */
static int check_debt_same_as_fresh(const char *name, const DebtRun *run,
                                    const DebtRun *fresh)
{
    if (run->presents != fresh->presents || run->updates != fresh->updates)
        fprintf(stderr, "floor-debt %s: presents %u updates %u, fresh %u %u\n",
                name, run->presents, run->updates, fresh->presents,
                fresh->updates);
    CHECK(run->presents == fresh->presents);
    CHECK(run->updates == fresh->updates);
    CHECK(run->dropped == 0u);
    /* every start one floor period after the previous one */
    CHECK(3u * run->wall_us <= (uint64_t)run->updates *
          KAGE_VITA_FULLSPEED_GAME_TICK_UNITS + 3u * run->updates);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    CHECK(run->debt.gf_pos == 0u && run->debt.gf_max_us <= 0);
    /* clock quantisation only: the floor is in thirds of a microsecond */
    CHECK(run->debt.late_max_us <= 2u && run->debt.stale == 0u);
#endif
    return 1;
}

/* The edge room with wake overshoot that differs between the loop-head
 * sleep (150 us) and the pace sleep (250 us): a waited pace leaves
 * gf = 250 - 150 - 500 = -400 us, below 0 (the wake-overshoot case of
 * the grid clamp). */
static DebtPhase debt_skew(uint32_t duration_us)
{
    DebtPhase p = DEBT_EDGE(duration_us);

    p.head_os_us = 150u;
    p.pace_os_us = 250u;
    return p;
}

/* As check_debt_same_as_fresh for the skewed edge room: the same presents
 * and updates as the fresh skewed room, no drop, gf never above 0, nothing
 * stale.  Lateness is not compared: under ON the floor may legitimately sit
 * at another phase behind the grid than in the fresh run. */
static int check_debt_same_as_fresh_wake(const char *name, const DebtRun *run,
                                         const DebtRun *fresh,
                                         uint32_t extra_hits)
{
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    if (run->presents != fresh->presents || run->updates != fresh->updates ||
            run->debt.gf_pos != 0u || run->debt.gf_max_us > 0)
        fprintf(stderr, "floor-debt %s: presents %u updates %u gf(n,pos,max) "
                "%u,%u,%d, fresh %u %u\n", name, run->presents, run->updates,
                run->debt.gf_n, run->debt.gf_pos, (int)run->debt.gf_max_us,
                fresh->presents, fresh->updates);
#else
    if (run->presents != fresh->presents || run->updates != fresh->updates)
        fprintf(stderr, "floor-debt %s: presents %u updates %u, fresh %u %u\n",
                name, run->presents, run->updates, fresh->presents,
                fresh->updates);
#endif
    CHECK(run->pace_extra_hits == extra_hits);
    CHECK(run->presents == fresh->presents);
    CHECK(run->updates == fresh->updates);
    CHECK(run->dropped == 0u);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    CHECK(run->debt.gf_pos == 0u && run->debt.gf_max_us <= 0);
    CHECK(run->debt.stale == 0u);
#endif
    return 1;
}

/* A scenario whose clock runs backwards: the window identity and the start
 * spacing do not apply; only fail-open, violations and the trigger. */
static int run_debt_clock_fault(const char *name, const DebtPhase *phases,
                                uint32_t count, DebtRun *run)
{
    if (!run_debt(phases, count, 14u, run)) {
        fprintf(stderr, "floor-debt scenario %s: run failed\n", name);
        return 0;
    }
    print_debt_run(name, run);
    CHECK(run->disables == 0u && run->violations == 0u);
    CHECK(run->clock_back_hits + run->head_back_hits == 1u);
    return 1;
}

static int check_floor_debt_scenarios(void)
{
    static const uint32_t stalls_us[4] = { 5000u, 20000u, 50000u, 150000u };
    DebtRun fresh, run;
    uint32_t i;
    char name[64];

    {
        const DebtPhase phases[] = { DEBT_JITTER(1000000u),
                                     DEBT_JITTER(10000000u) };
        if (!run_debt_named("jitter", phases, 2u, 1u, &run))
            return 0;
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2
        /* ON: ordinary lateness below the cap is all kept: 30.00 UPS. */
        CHECK(run.debt.late > 0u && run.debt.over == 0u &&
              run.debt.stale == 0u);
        CHECK(3u * run.wall_us <= (uint64_t)run.updates *
              KAGE_VITA_FULLSPEED_GAME_TICK_UNITS +
              3u * (uint64_t)ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US +
              4u * run.updates);
#endif
    }
    {
        /* Service jitter up to 12 ms: some lateness is above the cap. */
        const DebtPhase phases[] = {
            DEBT_ROOM(1000000u, 500u, 12000u, 6000u, 5000u, 3000u, 1u),
            DEBT_ROOM(10000000u, 500u, 12000u, 6000u, 5000u, 3000u, 1u) };
        if (!run_debt_named("jitter-wide", phases, 2u, 2u, &run))
            return 0;
    }
    {
        const DebtPhase phases[] = { DEBT_EDGE(2000000u), DEBT_EDGE(4000000u) };
        if (!run_debt_named("edge", phases, 2u, 3u, &fresh))
            return 0;
        /* the room really is at the edge: every interpolated frame fits */
        CHECK(fresh.presents == fresh.ticks);
    }
    /* Stalls before a pace (service), inside Game::Update and inside an
     * interpolated Render, each followed by the edge room. */
    for (i = 0u; i < 4u; ++i) {
        DebtPhase stall = DEBT_EDGE(1000000u);
        DebtPhase render_stall = DEBT_EDGE(1000000u);
        DebtPhase update_stall = DEBT_EDGE(1000000u);
        DebtPhase window = DEBT_EDGE(2000000u);

        stall.stall_full = 30u;
        stall.stall_us = stalls_us[i];
        render_stall.stall_render_tick = 60u;
        render_stall.stall_render_us = stalls_us[i];
        update_stall.stall_update = 30u;
        update_stall.stall_update_us = stalls_us[i];
        window.stall_full = 10u;
        window.stall_us = stalls_us[i];
        {
            const DebtPhase phases[] = { stall, DEBT_EDGE(2000000u),
                                         DEBT_EDGE(4000000u) };
            (void)snprintf(name, sizeof name, "stall-%ums-edge",
                           (unsigned)(stalls_us[i] / 1000u));
            if (!run_debt_named(name, phases, 3u, 6u, &run) ||
                    !check_debt_same_as_fresh(name, &run, &fresh))
                return 0;
        }
        {
            const DebtPhase phases[] = { render_stall, DEBT_EDGE(2000000u),
                                         DEBT_EDGE(4000000u) };
            (void)snprintf(name, sizeof name, "render-stall-%ums-edge",
                           (unsigned)(stalls_us[i] / 1000u));
            if (!run_debt_named(name, phases, 3u, 7u, &run) ||
                    !check_debt_same_as_fresh(name, &run, &fresh))
                return 0;
        }
        {
            /* Inside Game::Update: with no stale cut at all, the 150-ms one
             * leaves the floor 3.4 ms ahead of the grid for good and this
             * room never presents an interpolated frame again. */
            const DebtPhase phases[] = { update_stall, DEBT_EDGE(2000000u),
                                         DEBT_EDGE(4000000u) };
            (void)snprintf(name, sizeof name, "update-stall-%ums-edge",
                           (unsigned)(stalls_us[i] / 1000u));
            if (!run_debt_named(name, phases, 3u, 7u, &run) ||
                    !check_debt_same_as_fresh(name, &run, &fresh))
                return 0;
        }
        {
            /* The stall's own window: one pace late by exactly the stall. */
            const DebtPhase phases[] = { DEBT_EDGE(1000000u), window };

            (void)snprintf(name, sizeof name, "stall-%ums",
                           (unsigned)(stalls_us[i] / 1000u));
            if (!run_debt_named(name, phases, 2u, 8u, &run))
                return 0;
        }
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
        if (stalls_us[i] < 33334u) {
            uint32_t cap = ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US;
            uint32_t kept = stalls_us[i] < cap ? stalls_us[i] : cap;

            /* + clock quantisation (the floor is in thirds of a us) */
            CHECK(run.debt.late_max_us >= stalls_us[i] &&
                  run.debt.late_max_us <= stalls_us[i] + 2u);
            CHECK(run.debt.over == (stalls_us[i] > cap ? 1u : 0u));
            CHECK(run.debt.kept_us >= kept &&
                  run.debt.kept_us <= kept + run.debt.late);
            CHECK(run.debt.stale == 0u);
        } else {
            /* one tick or more: the stale cut keeps nothing */
            CHECK(run.debt.stale == 1u && run.debt.stale_grid == 0u);
            CHECK(run.debt.stale_us >= stalls_us[i] &&
                  run.debt.stale_us <= stalls_us[i] + 2u);
            CHECK(run.debt.late_max_us <= 1u);
        }
#endif
    }
    /* Render-bound windows with stale wrapper drops, then the edge room. */
    for (i = 0u; i < 16u; ++i) {
        const DebtPhase phases[] = { DEBT_HEAVY(2000000u + 2100u * i),
                                     DEBT_EDGE(2000000u), DEBT_EDGE(4000000u) };
        (void)snprintf(name, sizeof name, "heavy-edge-%u", i);
        if (!run_debt_named(name, phases, 3u, 4u, &run) ||
                !check_debt_same_as_fresh(name, &run, &fresh))
            return 0;
    }
    /* The same through a room that never waits on the floor: here a floor
     * left ahead of the grid would stick (stale cut). */
    for (i = 0u; i < 16u; ++i) {
        const DebtPhase phases[] = { DEBT_HEAVY(2000000u + 2100u * i),
                                     DEBT_MEDIUM(1000000u),
                                     DEBT_EDGE(2000000u), DEBT_EDGE(4000000u) };
        (void)snprintf(name, sizeof name, "heavy-medium-edge-%u", i);
        if (!run_debt_named(name, phases, 4u, 4u, &run) ||
                !check_debt_same_as_fresh(name, &run, &fresh))
            return 0;
    }
    {
        /* The heavy room itself: drops happen and every pace is late. */
        const DebtPhase phases[] = { DEBT_HEAVY(1000000u), DEBT_HEAVY(4000000u) };
        if (!run_debt_named("heavy", phases, 2u, 5u, &run))
            return 0;
        CHECK(run.dropped > 0u);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
        /* The grid rule: paces after a drop keep nothing and are filed as
         * stale_grid.  The floor never gets ahead of the grid (with neither
         * the grid rule nor the grid clamp: gf > 0 on 4 of 86). */
        CHECK(run.debt.gf_pos == 0u && run.debt.gf_max_us <= 0);
        CHECK(run.debt.stale_grid > 0u);
#endif
    }
    {
        /* A pause / menu: no Game::Update for two seconds, then the room. */
        DebtPhase menu = DEBT_JITTER(2000000u);

        menu.menu = 1u;
        {
            const DebtPhase phases[] = { DEBT_JITTER(1000000u), menu,
                                         DEBT_JITTER(3000000u) };

            if (!run_debt_named("pause-resume", phases, 3u, 9u, &run))
                return 0;
        }
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
        /* the first pace after the menu is stale (two seconds late) */
        CHECK(run.debt.stale == 1u && run.debt.stale_us >= 1900000u);
#endif
    }
    {
        /* An interpolated frame predicted to end just after the floor. */
        const DebtPhase phases[] = {
            DEBT_ROOM(2000000u, 500u, 0u, 500u, 16000u, 0u, 1u),
            DEBT_ROOM(4000000u, 500u, 0u, 500u, 16000u, 0u, 1u) };
        if (!run_debt_named("slack", phases, 2u, 10u, &run))
            return 0;
    }
    {
        /* ... and one predicted to end well after it: refused everywhere. */
        const DebtPhase phases[] = {
            DEBT_ROOM(2000000u, 500u, 0u, 500u, 17500u, 0u, 1u),
            DEBT_ROOM(4000000u, 500u, 0u, 500u, 17500u, 0u, 1u) };
        if (!run_debt_named("refuse", phases, 2u, 11u, &run))
            return 0;
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
        CHECK(run.debt.slack_band == 0u);
#endif
    }
    {
        /* Grid clamp: a wake overshoot on a pace sleep that is
         * larger than the head sleep's plus the service.  In the skewed edge
         * room every pace is late or on time; the 10th full tick has no
         * service, so its pace comes 500 us before its floor and waits, and
         * that sleep wakes 250 + extra us late.  The realign moves the 60-Hz
         * grid by the whole wait, so the floor sits gf = 250 + extra - 150 -
         * 0 us ahead of the next full head's grid deadline and the next
         * pace is late by gf + 650 us.  Stock re-anchors it at the clock.
         * ON must not keep the part of that lateness that lies before the
         * pace's own grid deadline (the grid clamp): otherwise gf stays above
         * 0 until the next stale event, no pace waits or realigns again, and
         * from gf > 417 us (no slack) or > 2417 us (slack 2000 us) the edge
         * room refuses every interpolated frame for good. */
        static const uint32_t extras_us[4] = { 1500u, 3000u, 6000u, 12000u };
        DebtRun skew_fresh;
        DebtPhase trigger = debt_skew(1000000u);

        {
            const DebtPhase phases[] = { debt_skew(2000000u),
                                         debt_skew(4000000u) };
            if (!run_debt_named("wake-skew-edge", phases, 2u, 13u,
                                &skew_fresh))
                return 0;
        }
        trigger.short_full = 10u;
        {
            /* no extra: gf = +100 us after the trigger (mild) */
            const DebtPhase phases[] = { trigger, debt_skew(2000000u),
                                         debt_skew(4000000u) };
            if (!run_debt_named("wake-skew-short-edge", phases, 3u, 13u,
                                &run) ||
                    !check_debt_same_as_fresh_wake("wake-skew-short-edge",
                                                   &run, &skew_fresh, 0u))
                return 0;
        }
        trigger.pace_extra_full = 10u;
        for (i = 0u; i < 4u; ++i) {
            trigger.pace_extra_us = extras_us[i];
            {
                const DebtPhase phases[] = { trigger, debt_skew(2000000u),
                                             debt_skew(4000000u) };
                (void)snprintf(name, sizeof name, "wake-extra-%uus-edge",
                               (unsigned)extras_us[i]);
                if (!run_debt_named(name, phases, 3u, 13u, &run) ||
                        !check_debt_same_as_fresh_wake(name, &run,
                                                       &skew_fresh, 1u))
                    return 0;
            }
        }
        /* Mutant MA (OBSERVE skips the clamp): the same
         * trigger inside the measured window, so the receipt sees the clamp
         * bind.  The 10th full tick has no service, so its pace waits for
         * its floor, wakes 250 + extra us late, and the realign moves the
         * grid by that whole wake.  The next pace (head overshoot 150 us,
         * service 500 us) is late by 750 + extra us (+1: the floor is in
         * thirds of a us) while its own grid deadline leaves room for
         * exactly 150 + 500 = 650 us, so it keeps 650 and the clamp takes
         * min(late, cap) - 650: 101, 1601 and 7350 us at cap 8000 (3350 at
         * cap 4000).  OBSERVE and ON both reach this pace through the same
         * waited pace, so the pin is the same in both modes; their other
         * late paces differ (OBSERVE 39, ON 59 of 60, MEASURED) and none of
         * them binds the clamp. */
        trigger = debt_skew(2000000u);
        trigger.short_full = 10u;
        for (i = 0u; i < 3u; ++i) {
            static const uint32_t window_extras_us[3] = { 0u, 1500u, 12000u };
            uint32_t extra = window_extras_us[i];

            trigger.pace_extra_full = extra ? 10u : 0u;
            trigger.pace_extra_us = extra;
            {
                const DebtPhase phases[] = { debt_skew(2000000u), trigger };
                (void)snprintf(name, sizeof name, "wake-extra-%uus-in-window",
                               (unsigned)extra);
                if (!run_debt_named(name, phases, 2u, 13u, &run))
                    return 0;
            }
            CHECK(run.pace_extra_hits == (extra ? 1u : 0u));
            CHECK(run.dropped == 0u);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            {
                uint32_t late_max = run.debt.late_max_us;
                uint32_t capped = late_max < ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US ?
                    late_max : ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US;

                if (run.debt.clamp != 1u || capped < 650u ||
                        run.debt.clamp_us != capped - 650u ||
                        late_max < 750u + extra || late_max > 751u + extra)
                    fprintf(stderr, "floor-debt %s: clamp(c,us) %u,%u late max "
                            "%u, want 1,min(late max, cap) - 650 and late max "
                            "%u..%u\n", name, run.debt.clamp,
                            run.debt.clamp_us, late_max, 750u + extra,
                            751u + extra);
                CHECK(late_max >= 750u + extra && late_max <= 751u + extra);
                CHECK(run.debt.clamp == 1u &&
                      run.debt.clamp_us == capped - 650u);
                CHECK(run.debt.stale == 0u);
            }
#endif
        }
    }
    {
        /* The grid rule's head sites, read from
         * the receipt.  The 10th full tick of the measured phase stalls
         * 25 ms before its pace, after (a) a loop-head clock reset
         * (kage_vita_fullspeed_scheduler.c, note_loop_head: now_us <
         * s_last_clock_us) or (b) a head sleep that returned with the clock
         * before it started (wait_until returns 2).  Either moved the grid
         * to the clock; the pace is late by less than one Game tick, and the
         * grid rule files it as stale_grid. */
        DebtPhase reset = DEBT_EDGE(1000000u), back = DEBT_EDGE(1000000u);

        reset.clock_back_full = 10u;
        reset.stall_full = 10u;
        reset.stall_us = 25000u;
        back.head_back_full = 10u;
        back.stall_full = 10u;
        back.stall_us = 25000u;
        {
            const DebtPhase phases[] = { DEBT_EDGE(1000000u), reset };
            if (!run_debt_clock_fault("head-clock-reset", phases, 2u, &run))
                return 0;
            CHECK(run.clock_back_hits == 1u);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            CHECK(run.debt.stale == 1u && run.debt.stale_grid == 1u);
#endif
        }
        {
            const DebtPhase phases[] = { DEBT_EDGE(1000000u), back };
            if (!run_debt_clock_fault("head-wait-backward", phases, 2u, &run))
                return 0;
            CHECK(run.head_back_hits == 1u);
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            CHECK(run.debt.stale == 1u && run.debt.stale_grid == 1u);
#endif
        }
    }
    {
        /* The rule-A boundary.  The 10th pace is
         * stale (50-ms stall, anchored at the clock: floor F11 = start10 +
         * 33,333 1/3 us), the 11th waits for F11 (anchor F11, so F12 =
         * start10 + 66,666 2/3 us), and the 12th starts exactly 100,000 us
         * after the 10th: late by exactly one Game tick (100,000 units),
         * which rule A files as stale.  One microsecond earlier it is late. */
        DebtPhase tick = DEBT_EDGE(1000000u);

        tick.stall_full = 10u;
        tick.stall_us = 50000u;
        tick.exact_ref_full = 10u;
        tick.exact_full = 12u;
        tick.exact_after_us = 100000u;
        {
            const DebtPhase phases[] = { DEBT_EDGE(1000000u), tick };
            if (!run_debt_named("rule-a-one-tick", phases, 2u, 15u, &run))
                return 0;
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            CHECK(run.debt.stale == 2u && run.debt.stale_grid == 0u);
            CHECK(run.debt.late_max_us < 33333u);
#endif
        }
        tick.exact_after_us = 99999u;
        {
            const DebtPhase phases[] = { DEBT_EDGE(1000000u), tick };
            if (!run_debt_named("rule-a-below-tick", phases, 2u, 15u, &run))
                return 0;
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
            /* 99,997 units late: ceil(99,997 / 3) = 33,333 us */
            CHECK(run.debt.stale == 1u && run.debt.late_max_us == 33333u);
#endif
        }
    }
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
    {
        /* port.ini fps_lock 30 / 40 / 45 (k = 0 / 3 / 2) in the jitter room. */
        static const uint8_t k_values[3] = { 0u, 3u, 2u };
        static const unsigned k_fps[3] = { 30u, 40u, 45u };

        for (i = 0u; i < 3u; ++i) {
            const DebtPhase phases[] = { DEBT_JITTER(1000000u),
                                         DEBT_JITTER(10000000u) };

            kage_vita_fullspeed_scheduler_set_duty(k_values[i]);
            s_banner[0] = '\0';
            (void)snprintf(name, sizeof name, "jitter-fps%u", k_fps[i]);
            if (!run_debt_named(name, phases, 2u, 12u, &run))
                return 0;
            if (!check_banner_fps_lock(k_fps[i]))
                return 0;
# if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2
            CHECK(run.debt.over == 0u && run.debt.stale == 0u);
            CHECK(3u * run.wall_us <= (uint64_t)run.updates *
                  KAGE_VITA_FULLSPEED_GAME_TICK_UNITS +
                  3u * (uint64_t)ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US +
                  4u * run.updates);
# endif
        }
        kage_vita_fullspeed_scheduler_set_duty(1u);
    }
#endif
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
    {
        /* The reader and the per-epoch clear both zero the receipt. */
        KageVitaFullspeedFloorDebtSnapshot zero, taken;
        static ScenarioResult scratch;
        OracleGame game;
        uint64_t start_us;
        uint32_t tick, round;

        memset(&zero, 0, sizeof zero);
        memset(&scratch, 0, sizeof scratch);
        memset(&game, 0, sizeof game);
        game.manager_pointer = 0x10000000u;
        game.game_pointer = 0x20000000u;
        s_now = 1000u;
        kage_vita_fullspeed_scheduler_reset();
        for (round = 0u; round < 2u; ++round) {
            for (tick = 0u; tick < 40u; ++tick)
                (void)run_debt_tick(&game, 500u + 700u * (tick % 3u), 6000u,
                                    600u, 5000u, 1u, 0u, &scratch, &start_us);
            if (round == 0u) {
                uint64_t stamp;

                kage_vita_fullspeed_scheduler_floor_debt_take(&taken);
                CHECK(taken.paces >= 19u && taken.gf_n >= 19u);
                stamp = taken.floor_us;
                /* the struct has padding before floor_us: zero the
                 * destination so memcmp sees zero padding however the
                 * compiler copies the struct */
                memset(&taken, 0, sizeof taken);
                kage_vita_fullspeed_scheduler_floor_debt_take(&taken);
                /* the floor stamp is not a sum: with no pace in between,
                 * the second take reads the same live floor */
                CHECK(stamp != 0u && taken.floor_us == stamp);
                taken.floor_us = 0u;
            } else {
                kage_vita_fullspeed_scheduler_reset();
                memset(&taken, 0, sizeof taken);
                kage_vita_fullspeed_scheduler_floor_debt_take(&taken);
                CHECK(taken.floor_us == 0u); /* no live floor after reset */
            }
            CHECK(memcmp(&taken, &zero, sizeof zero) == 0);
        }
    }
#endif
    return 1;
}
#endif

int main(void)
{
#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
    if (!check_render_admission())
        return 1;
# if defined(ORACLE_GL_WORKER_CASES)
    if (!check_worker_cases())
        return 1;
#  if (defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) || \
    defined(ORACLE_GL_ADMIT_BEHIND_TWIN)
    if (!check_admit_behind_cases())
        return 1;
#  endif
# endif
#endif
    if (!check_scenario(5000u, 1u) ||
            !check_scenario(10000u, 1u) ||
            !check_scenario(19000u, 1u) ||
            !check_scenario(27000u, 0u) ||
            !check_scenario(40000u, 0u) ||
            !check_scenario(55000u, 0u) ||
            !check_scenario(40000u, 1u) ||
            !check_large_stall() ||
            !check_backward_clock() ||
            !check_deadline_overflow_fail_open() ||
            !check_duplicate_fail_open() ||
            !check_missing_tick_stage_fail_open() ||
            !check_parity_fail_open() ||
            !check_game_frame_fail_open() ||
            !check_nonfull_game_frame_state_rebase() ||
            !check_game_frame_early_return_attribution() ||
            !check_game_frame_zero_frame_early_return(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_RETURN_RVA) ||
            !check_game_frame_zero_frame_early_return(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE121_RVA) ||
            !check_game_frame_zero_frame_early_return(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE362_RVA) ||
            !check_game_frame_zero_frame_wrong_delta_fail_open(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE362_RVA) ||
#if defined(ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES)
            !check_game_frame_zero_frame_early_return(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE099_RVA) ||
            !check_game_frame_zero_frame_early_return(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE0EA_RVA) ||
            !check_game_frame_zero_frame_wrong_delta_fail_open(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE099_RVA) ||
#else
            !check_game_frame_unknown_site_fail_open(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE099_RVA) ||
            !check_game_frame_unknown_site_fail_open(
                KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE0EA_RVA) ||
#endif
            !check_game_frame_unknown_site_fail_open(0x002CE074u) ||
            !check_game_return_missing_fail_open() ||
            !check_interpolation_transition() ||
            !check_manager_counter_rebase_transition() ||
            !check_manager_counter_rebase_hostile_cases() ||
            !check_game_pointer_publish_transition() ||
            !check_game_pointer_hostile_cases() ||
            !check_continue_cutscene_faults() ||
            !check_continue_cutscene_late_render() ||
            !check_wait_failure_fail_open())
        return 1;
    for (uint32_t publish = 0u; publish < 2u; ++publish)
        for (uint32_t restore = 0u; restore < 2u; ++restore) {
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
            for (uint32_t duty = 0u; duty < 4u; ++duty)
#else
            for (uint32_t duty = 1u; duty < 2u; ++duty)
#endif
                if (!check_continue_cutscene_gameplay(publish, restore, duty))
                    return 1;
        }
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
    kage_vita_fullspeed_scheduler_set_duty(1u);
#endif
    puts("Vita Continue->cutscene->gameplay: PASS (authenticated rebase, optional publication, "
         "restore/no-restore, next-head audit, expired-token/identity/pair/frame/clock rejection, "
         "bounded first-failure gate receipt, 120 updates per 240 gameplay ticks)");
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    if (!check_advanced_head_transitions() || !check_advanced_head_faults() ||
            !check_advanced_head_present_intervals())
        return 1;
    puts("Vita full-head advance: PASS (current identity/refusal, pause rollback, "
         "head/floor wait and clock faults, deactivate/reset, actual Game/Present intervals)");
#endif
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
    if (!check_duty_lock() || !check_duty_fail_open())
        return 1;
    puts("Vita cadence30 fps-lock duty: PASS (k=1/2/3/0 -> 120/90/80/60 "
         "presents per 120 ticks at 60 updates with exact F/N/_ transcripts; "
         "k=1 == default; k=0 == interpolation-off; clamp; reset/deactivate "
         "keep k; fail-open stops the lock; banner fps_lock after the parity "
         "pin)");
#endif
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    if (!check_video_priority())
        return 1;
    puts("Vita video Render priority: PASS (late cinematic service, 30/40/45/60 locks, "
         "Game resumes <=30 UPS, retained-clip gameplay schedule identical, expired/late receipt refused)");
#endif
    if (!check_present_wait())
        return 1;
#if !(defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE)
    if (!check_floor_debt_scenarios())
        return 1;
# if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2
    puts("Vita floor-debt scenarios: PASS (mode=on cap="
         KAGE_ORACLE_STR(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US) "us; "
         "start contract start[j]-start[i] >= (j-i)*T - cap, <=31/s; "
         "jitter kept, 30.00 UPS; floor drops only over+stale; "
         "stale cut: heavy/stall/render-stall/update-stall -> edge room == fresh, gf <= 0; "
         "receipt zeroed by reader and reset)");
# elif defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 1
    puts("Vita floor-debt scenarios: PASS (mode=observe cap="
         KAGE_ORACLE_STR(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US) "us; "
         "stock start contract; wall == n*T + late + stale; "
         "stale cut: heavy/stall/render-stall/update-stall -> edge room == fresh, gf <= 0; "
         "receipt zeroed by reader and reset)");
# else
    puts("Vita floor-debt scenarios: PASS (mode=off; stock start contract; "
         "heavy/stall/render-stall/update-stall -> edge room == fresh)");
# endif
#endif
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2
# define ORACLE_GAP_TEXT "start[j]-start[i] >= (j-i)*33.333 ms - cap, <=31 starts/s"
#else
# define ORACLE_GAP_TEXT "<=30 UPS, >=33.333-ms gaps"
#endif
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    puts("Vita cadence30 Present-wait admission: PASS (render-cost=excludes-present; "
         "every-5th-Present edge wait at int2j timings 120 presents/120 loops, "
         "skip n=0, 2,000,000 us/window; 60 updates/window, " ORACLE_GAP_TEXT
         ", no fail-open in 39 cases)");
#else
    puts("Vita cadence30 Present-wait admission: PASS (inclusive render cost; "
         "every-5th-Present edge wait at int2j timings 100 presents/120 loops, "
         "skip n=20, 2,000,000 us/window; 60 updates/window, " ORACLE_GAP_TEXT
         ", no fail-open in 39 cases)");
#endif
#if defined(ORACLE_PRESENT_WAIT_DEBT_TABLE) && defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    puts("Vita cadence30 Present-wait costs (service reserve, floor-debt on cap 8000us "
         "admit-slack 2000us): pinned 39 cases; fixed 10.6-ms wait 1200 presents in "
         "20,000,000 us (30.00 UPS); fixed 18-ms wait 1200 presents in 20,112,080 us "
         "(29.83 UPS, stock 1000 in 20,000,200); never fewer presents/s than stock");
#else
    puts("Vita cadence30 Present-wait costs (service reserve, floor-debt on cap 8000us "
         "admit-slack 2000us): pinned 39 cases; fixed 9-ms wait 1200 presents in "
         "20,000,000 us (stock 1000); vblank queue 18 ms 1100 presents in 20,000,000 us "
         "(30.00 UPS, stock 28.69); never fewer presents/s or UPS than stock");
#endif
#elif defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE) && \
    !(defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && \
    !(defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2)
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    puts("Vita cadence30 Present-wait costs (service reserve): pinned 39 cases; "
         "fixed 10.6-ms wait 1200 presents in 20,184,200 us (29.73 UPS); "
         "GPU-bound vblank queue 19 ms 1114 presents in 21,433,334 us (27.99 UPS)");
#else
    puts("Vita cadence30 Present-wait costs (service reserve): pinned 39 cases; "
         "fixed 10.6-ms wait 1000 presents in 20,000,200 us (30.00 UPS); "
         "GPU-bound vblank queue 19 ms 1050 presents in 20,000,000 us (30.00 UPS)");
#endif
#endif
    puts(
        "Vita cadence30 scheduler oracle: PASS "
        "(60-Hz monotonic wrapper; native 30-Hz Game parity; "
        "native interpolated Render at 5/10 ms; late nonfull discard and "
        "full-phase Render at 27/40/55 ms; one-tick stale wrapper debt; "
#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT == 2
        "no stale simulation replay and <=31 Game update starts per sliding "
        "second (floor debt on); "
#else
        "no stale simulation replay and <=30 Game updates/second; "
#endif
        "game-owned interpolation transitions; "
        "exact Continue counter normalization; "
        "exact null-to-new Game publication; "
        "exact zero-frame early-return acceptance/attribution; "
        "duplicate/missing/parity/frame/counter/pointer/wait fail-open)");
    return 0;
}
