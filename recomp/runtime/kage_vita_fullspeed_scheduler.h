#ifndef KAGE_VITA_FULLSPEED_SCHEDULER_H
#define KAGE_VITA_FULLSPEED_SCHEDULER_H

#include <stdint.h>

/* The clock is integer microseconds.  Keeping deadlines in thirds of a
 * microsecond represents 1/60 second exactly:
 * 16,666 2/3 us == 50,000 units. */
#define KAGE_VITA_FULLSPEED_TICK_UNITS 50000u
/* 33,333 1/3 us: Game::Update floor period (start spacing: end of file). */
#define KAGE_VITA_FULLSPEED_GAME_TICK_UNITS 100000u
#define KAGE_VITA_FULLSPEED_MAX_CATCHUP_TICKS 4u
#define KAGE_VITA_FULLSPEED_MAX_DUE_TICKS \
    (KAGE_VITA_FULLSPEED_MAX_CATCHUP_TICKS + 1u)
/* Once a backlog is already large enough that time must be discarded, retain
 * only one catch-up wrapper after the current one.  Ordinary 27--55 ms render
 * recovery remains governed by MAX_CATCHUP_TICKS above. */
#define KAGE_VITA_FULLSPEED_STALE_CATCHUP_TICKS 1u
#define KAGE_VITA_FULLSPEED_STALE_DUE_TICKS \
    (KAGE_VITA_FULLSPEED_STALE_CATCHUP_TICKS + 1u)
#define KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS 4u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA 0x002CE74Fu
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_RETURN_RVA 0x002CE167u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE121_RVA 0x002CE121u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE362_RVA 0x002CE362u
/* Two more of the eight machine-pinned early RETs.  Device evidence (bid
 * perf:bundle35-raster720-v1 at 157.659 s, perf:wf-int720-v2 at 394.796 s):
 * both returned with Game+0x1a30dc unchanged (21346>21346, 5042>5042) while
 * the player left a run for the menu, and the fail-open then disabled the
 * 60-Hz cadence for the rest of the session.  Accepted only when the build
 * defines ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES. */
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE099_RVA 0x002CE099u
#define KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE0EA_RVA 0x002CE0EAu

typedef struct KageVitaFullspeedSnapshot {
    uint32_t wrapper_ticks;
    uint32_t service_calls;
    uint32_t manager_dispatch_calls;
    uint32_t manager_entries;
    uint32_t game_updates;
    uint32_t render_calls;
    uint32_t render_bodies;
    uint32_t presents;
    uint32_t full_phases;
    uint32_t nonfull_phases;
    uint32_t interpolation_phases;
    uint32_t render_skips;
    uint32_t late_full_skips;
    uint32_t nonfull_skips;
    uint32_t forced_renders;
    uint32_t duplicate_tick_violations;
    uint32_t parity_violations;
    uint32_t game_frame_violations;
    uint32_t game_update_last_return_site;
    uint32_t game_update_last_frame_before;
    uint32_t game_update_last_frame_after;
    uint32_t game_frame_violation_return_site;
    uint32_t game_frame_violation_before;
    uint32_t game_frame_violation_after;
    uint32_t manager_counter_rebase_events;
    uint32_t manager_counter_rebases;
    uint32_t manager_counter_rebase_violations;
    uint32_t game_pointer_publish_events;
    uint32_t game_pointer_rebinds;
    uint32_t game_pointer_violations;
    uint32_t sequence_violations;
    uint32_t runtime_disables;
    uint32_t clock_resets;
    uint32_t dropped_ticks;
    uint32_t dropped_us;
    uint32_t wait_calls;
    uint32_t waited_us;
    uint32_t debt_us;
    uint32_t maximum_debt_us;
    /* Subset of wait_calls/waited_us spent in the Game::Update pacing sleep
     * (kage_vita_fullspeed_pace_game_update), which executes inside the
     * profiler's upd phase.  ph120.ik pace(c,us) prints the window delta so
     * upd wall time can be split into CPU and sleep.  Accumulated only when
     * the scheduler is compiled with ISAAC_VITA_PROFILE_IMPORT_KINDS (the
     * census build that prints ph120.ik); zero in every other build.  The
     * fields stay unconditional so every includer shares one layout. */
    uint32_t pace_wait_calls;
    uint32_t pace_waited_us;
} KageVitaFullspeedSnapshot;

void kage_vita_fullspeed_scheduler_reset(void);
void kage_vita_fullspeed_scheduler_deactivate(void);
void kage_vita_fullspeed_scheduler_note_loop_head(void);
void kage_vita_fullspeed_scheduler_note_service(void);
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
/* The main Theora service observed exactly one live clip. This host-only
 * receipt expires at the next head and cannot authorize any guest update. */
void kage_vita_fullspeed_scheduler_note_video_service(void);
#endif
void kage_vita_fullspeed_scheduler_note_manager_dispatch(void);
void kage_vita_fullspeed_scheduler_note_manager_entry(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame);
void kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
    uint32_t manager_pointer, uint32_t old_manager_counter,
    uint32_t new_manager_counter);
void kage_vita_fullspeed_scheduler_note_game_pointer_publish(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t old_game_pointer, uint32_t new_game_pointer);
void kage_vita_fullspeed_scheduler_note_game_update_begin(
    uint32_t manager_counter, uint32_t game_pointer, uint32_t game_frame);
void kage_vita_fullspeed_scheduler_note_game_update_early_return(
    uint32_t return_site);
void kage_vita_fullspeed_scheduler_note_game_update_end(
    uint32_t game_pointer, uint32_t game_frame);
int kage_vita_fullspeed_scheduler_plan_render(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame);
void kage_vita_fullspeed_scheduler_note_render_entry(void);
void kage_vita_fullspeed_scheduler_note_render_body(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled);
void kage_vita_fullspeed_scheduler_note_render_return(void);
void kage_vita_fullspeed_scheduler_note_present(void);
int kage_vita_fullspeed_scheduler_bypass_limiter(void);
void kage_vita_fullspeed_scheduler_snapshot(
    KageVitaFullspeedSnapshot *snapshot);

#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
/* ISAAC_VITA_LUA_GC_IDLE_STEP (host_vita_lua_gc.c): what follows the loop
 * tail the caller stands on -- LuaEngine::Update's paced lua_gc step, the
 * last guest work of a main-loop iteration (sub_0048bc50: call at 0x48be08,
 * then the jump back to the loop head L_0048bd40).  A pure query: one clock
 * read, no scheduler state is written and nothing is disabled on failure.
 *   UNKNOWN  the scheduler does not own this tick (menu, video, disabled,
 *            first tick, clock retreat): the caller must act as without the
 *            knob;
 *   FREE     the next blocking wait is ahead and *slack_us is the time to
 *            its target: the loop head's grid wait, or -- a non-full tick
 *            ends late for the grid -- the 30-Hz floor wait of the full tick
 *            that follows;
 *   BUSY     this tick ran Game::Update and the head will not sleep: the
 *            next tick is the non-full one, whose render gate measures the
 *            time left to the floor.  Work done now is taken from it (the
 *            caller carries its step past that render only when the step
 *            cannot run atomic(): host_vita_lua_gc_idle_rule.h);
 *   LATE     no wait is ahead at all (the floor is already due). */
#define KAGE_VITA_GC_IDLE_UNKNOWN 0
#define KAGE_VITA_GC_IDLE_FREE 1
#define KAGE_VITA_GC_IDLE_BUSY 2
#define KAGE_VITA_GC_IDLE_LATE 3
int kage_vita_fullspeed_scheduler_gc_idle_query(uint32_t *slack_us);
#endif

#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
/* port.ini fps_lock (port.ini design section 2):
 * admission duty on the non-full (interpolated) phase.  k = 1 admits every
 * non-full phase (60 presents/s, today's behaviour), k = 2 every second
 * (45), k = 3 every third (40), k = 0 none (30); any other value is
 * clamped to 1.  Pushed once at boot by host_vita_runtime_settings.c before
 * the guest starts; survives reset/deactivate (the per-epoch clear does not
 * touch it).  Full phases, the 30-Hz Game floor, the 60-Hz grid, the
 * affordability test and fail-open are unchanged; after fail-open the lock
 * stops with the scheduler. */
void kage_vita_fullspeed_scheduler_set_duty(uint8_t k);
#endif
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
# define KAGE_VITA_FULLSPEED_RESET() \
    kage_vita_fullspeed_scheduler_reset()
# define KAGE_VITA_FULLSPEED_DEACTIVATE() \
    kage_vita_fullspeed_scheduler_deactivate()
# define KAGE_VITA_FULLSPEED_LOOP_HEAD() \
    kage_vita_fullspeed_scheduler_note_loop_head()
# define KAGE_VITA_FULLSPEED_NOTE_SERVICE() \
    kage_vita_fullspeed_scheduler_note_service()
# define KAGE_VITA_FULLSPEED_NOTE_MANAGER_DISPATCH() \
    kage_vita_fullspeed_scheduler_note_manager_dispatch()
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_ENTRY() \
    kage_vita_fullspeed_scheduler_note_render_entry()
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_RETURN() \
    kage_vita_fullspeed_scheduler_note_render_return()
# define KAGE_VITA_FULLSPEED_NOTE_PRESENT() \
    kage_vita_fullspeed_scheduler_note_present()
#else
# define KAGE_VITA_FULLSPEED_RESET() ((void)0)
# define KAGE_VITA_FULLSPEED_DEACTIVATE() ((void)0)
# define KAGE_VITA_FULLSPEED_LOOP_HEAD() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_SERVICE() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_MANAGER_DISPATCH() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_ENTRY() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_RENDER_RETURN() ((void)0)
# define KAGE_VITA_FULLSPEED_NOTE_PRESENT() ((void)0)
#endif

#if defined(ISAAC_VITA_DEBUG_OVERLAY)
/* On-screen debug overlay (host_vita_debug_overlay.c): the wall durations
 * this scheduler already measures for its admission test -- the last
 * Game::Update body and the last Render (entry to return, inclusive of
 * Present) -- in microseconds; 0 before the first sample.  Game thread. */
void kage_vita_fullspeed_scheduler_debug_overlay_timings(
    uint32_t *update_us, uint32_t *render_us);
#endif

#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
/* ISAAC_VITA_FULLSPEED_FLOOR_DEBT (render-60 lever S5):
 * 1 = OBSERVE (stock behaviour, counts only), 2 = ON.
 *
 * Restated contract with the knob ON.  Game floors stay
 * KAGE_VITA_FULLSPEED_GAME_TICK_UNITS apart, but a Game::Update START may
 * come up to FLOOR_DEBT_CAP_US earlier than one floor period after the
 * previous start: for any two starts i < j,
 *     start[j] - start[i] >= (j - i) * 33,333 1/3 us - cap,
 * so one gap may be as short as 25,333 us at the 8000-us cap, at most 31
 * starts fit in any sliding second, and the long-run rate stays <= 30 UPS.
 * Stock builds and OBSERVE keep no debt, but their floor is not a start
 * spacing either: a start that slept to its floor and woke o us late does
 * not move the next floor, so start[j] - start[i] >= (j - i) * 33,333 1/3 us
 * - o_i, and one gap can be short by that wake overshoot (modelled with
 * 180 +- 60 us oversleep: smallest gap 30,515-33,207 us, 11-13 % of gaps
 * below 33,333 us, at most 30 per sliding second).  ON's bound is the same
 * with max(o_i, cap) in place of o_i.  The per-tick wrapper sequence (one Manager::Update
 * per 60-Hz tick, Game::Update on one parity) is unchanged in every mode.
 * The per-second wrapper count is not: ON removes most floor waits, so the
 * 60-Hz grid is realigned less often and wrapper ticks and Manager::Update
 * calls rise from about 59/s toward 60/s (host oracle jitter room: 593
 * ticks in 10.026 s stock, 601 in 10.000 s ON).  Game::Update's authenticated
 * inputs (Manager counter, parity, frame) are unchanged; the wall-clock time
 * it reads is not: the guest calls timeGetTime from 366 sites, and mods read
 * Isaac.GetTime().
 *
 * Receipt (ph120.fd / ph120.fs), per profile window: taken and zeroed by
 * kage_vita_fullspeed_scheduler_floor_debt_take() (one reader, the game
 * thread's phase-profile emission) and by the per-epoch clear.  Sums
 * saturate at UINT32_MAX.  floor_us is a stamp, not a sum: the next Game
 * floor at the take, so two consecutive takes give the window identity
 *     floor_us[k] - floor_us[k-1] = paces * 33,333 1/3 us + late_us +
 *     stale_us (OFF/OBSERVE: every pace re-anchors at its floor plus the
 *     lateness it drops), over_us + stale_us in place of late_us under ON,
 * exact up to 1 us of rounding per pace (tools/bench/bench_run.py fres; the
 * report time ph120.s at= is read before the loop
 * head's wait, so wall-clock deltas carry the boundary tick's work).
 *
 * "Late" = a pace with a live floor that did not wait and started after the
 * floor, by less than one Game tick, with no stale wrapper drop, no
 * loop-head clock reset and no head sleep that returned with the clock
 * before its start since the previous pace.  Every other pace that did
 * not wait is "stale": the floor re-anchors at the current clock exactly as
 * in stock builds and nothing is kept (the stale cut).
 * gf = (next full head's 60-Hz grid deadline) - (next Game floor), sampled
 * after every pace: > 0 means the floor sits ahead of the grid, so the next
 * full head reaches its deadline before the floor.  After a pace that waited
 * for the floor and realigned the grid (the last Game::Update plus Render
 * fit in one Game tick), gf = o_pace - o_head - svc in every mode (the pace
 * wait's wake overshoot minus the head wait's overshoot minus the service
 * time between the head and the pace), so gf > 0 is legitimate wherever the
 * pace wake overshoots more than the head wake plus the service; stock builds
 * show it too, and it lasts until the next pace.  A waited pace that did not
 * realign leaves gf = this head's deadline - its floor < 0.  After a late
 * pace gf <= 0 in every mode: stock and OBSERVE re-anchor at the clock
 * (gf = this head's deadline - its start), ON keeps at most
 * the lateness after this full head's own grid deadline (the grid clamp in
 * kage_vita_fullspeed_floor_debt_anchor), and a stale pace keeps nothing.
 * A window whose every sample has gf > 0 and whose last is above 1 ms needs
 * every pace wait in it to overshoot by over 1 ms more than its head wait;
 * without the clamp it is what one such overshoot leaves behind under ON
 * (gf frozen across late paces), so tools/bench/bench_run.py counts those
 * windows (gfstuck). */
typedef struct KageVitaFullspeedFloorDebtSnapshot {
    uint32_t paces;            /* Game::Update paces with a live floor */
    uint32_t late;             /* late paces (definition above) */
    uint32_t late_us;          /* sum of their lateness */
    uint32_t late_max_us;      /* window maximum */
    uint32_t kept_us;          /* sum of min(late, cap, grid room): kept (ON); OBSERVE: what ON would keep on stock's trajectory */
    uint32_t over;             /* late paces kept below their lateness (cap or grid clamp) */
    uint32_t over_us;          /* sum of (late - kept); late_us == kept_us + over_us */
    /* Late paces the grid clamp kept below min(late, cap), and the sum of
     * min(late, cap) - kept over them: kept_us + clamp_us = sum of
     * min(late, cap), the cap decision's numerator (under
     * OBSERVE the clamp binds on stock's trajectory, which ON leaves after
     * its first late pace, so kept_us does not predict ON's keep). */
    uint32_t clamp;
    uint32_t clamp_us;
    uint32_t stale;            /* stale paces (stock re-anchor, nothing kept) */
    uint32_t stale_grid;       /* of which < one Game tick late: cut by the grid rule */
    uint32_t stale_us;         /* sum of the stale paces' lateness */
    uint32_t gf_n;             /* gf samples (paces with a live wrapper grid) */
    uint32_t gf_pos;           /* samples with gf > 0 */
    int32_t gf_max_us;         /* window maximum; 0 when gf_n == 0 */
    int32_t gf_min_us;         /* window minimum; 0 when gf_n == 0 */
    int32_t gf_last_us;        /* last sample; 0 when gf_n == 0 */
    /* late paces by lateness: < 1 us (inside the floor's own microsecond),
     * < 250, < 1000, < 2000, < 5000, < 16667, < 33334 us; sums to late */
    uint32_t late_hist[7];
    uint32_t slack_band;       /* interpolated frames predicted to end in (floor, floor + slack] */
    uint32_t slack_late;       /* ON: late paces right after a slack-band admit */
    uint32_t slack_late_us;    /* their summed lateness */
    /* Stamp, set by the take only: the next Game floor in whole us (floor
     * of the 1/3-us units), 0 without a live floor or with the scheduler
     * disabled. */
    uint64_t floor_us;
} KageVitaFullspeedFloorDebtSnapshot;
void kage_vita_fullspeed_scheduler_floor_debt_take(
    KageVitaFullspeedFloorDebtSnapshot *snapshot);
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT
typedef struct KageVitaFullspeedGlAdmitWaitSnapshot {
    uint32_t gates;       /* n: non-full gates that found the render thread behind */
    uint32_t admitted;    /* ok: of which rendered (ON) */
    uint32_t timeouts;    /* to: closure still running at latest (ON) */
    uint32_t no_window;   /* nw: no live floor, or latest <= now */
    uint32_t backoff;     /* bo: refused inside a back-off (ON) */
    uint32_t time_test;   /* tt: closure done, then refused by the tick-due or time test (ON) */
    uint32_t wait_us;     /* summed gate wait (ON) */
    uint32_t wait_max_us; /* window maximum */
    uint32_t late;        /* lt: late Game::Update starts right after a behind-admitted render
                           * whose loop head did not sleep (ON) */
    uint32_t late_us;     /* their summed lateness */
    uint32_t hold_late;   /* hl: loop heads that did not sleep after a Present held on a
                           * closure admitted after a wait (ON) */
    uint32_t hold_late_us; /* their summed lateness past the head's due time */
    uint32_t timeout_over_max_us; /* ov: largest wake past a timed-out wait's deadline (ON) */
} KageVitaFullspeedGlAdmitWaitSnapshot;
void kage_vita_fullspeed_scheduler_gl_admit_wait_take(
    KageVitaFullspeedGlAdmitWaitSnapshot *snapshot);
#endif
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND) && ISAAC_VITA_GL_THREAD_ADMIT_BEHIND
/* GL admit behind (R60-FABLE-PLAN.md lane B).  BACKOFF: non-full phases of
 * stock refusals after a loop head that did not sleep behind a Present hold
 * on a behind-admitted frame.  SAMPLES: outstanding-byte readings kept per
 * take for the median (a 120-loop window has at most 60 non-full gates;
 * gates beyond the cap still count in n and in the maximum). */
#define KAGE_VITA_GL_ADMIT_BEHIND_BACKOFF 30
#define KAGE_VITA_GL_ADMIT_BEHIND_SAMPLES 64u
typedef struct KageVitaFullspeedGlAdmitBehindSnapshot {
    uint32_t gates;        /* n: non-full gates that found the render thread behind */
    uint32_t test_pass;    /* st p: stock time test live and passing at the last Render cost */
    uint32_t test_fail;    /* st f: live and failing (stock would refuse this frame anyway) */
    uint32_t test_off;     /* st x: not live (no floor ahead, or no samples): stock admits */
    uint32_t over;         /* ov: outstanding bytes over the bound */
    uint32_t would;        /* wa: within the bound and st not f */
    uint32_t admitted;     /* ok: rendered while behind (ON) */
    uint32_t backoff;      /* bo: within the bound, refused inside a back-off (ON) */
    uint32_t time_test;    /* tt: within the bound, refused by the stock time test (ON) */
    uint32_t hold_late;    /* hl: loop heads that did not sleep after a Present hold on a
                            * behind-admitted frame (ON); each arms the back-off */
    uint32_t hold_late_us; /* by how far past their due time */
    uint32_t out_max;      /* ob max: largest outstanding-byte reading */
    uint32_t out_count;    /* readings stored in out[] */
    uint32_t out[KAGE_VITA_GL_ADMIT_BEHIND_SAMPLES];
} KageVitaFullspeedGlAdmitBehindSnapshot;
void kage_vita_fullspeed_scheduler_gl_admit_behind_take(
    KageVitaFullspeedGlAdmitBehindSnapshot *snapshot);
/* The bound the gate compares with, as compiled into the scheduler. */
uint32_t kage_vita_fullspeed_scheduler_gl_admit_behind_bytes(void);
#endif

#endif
