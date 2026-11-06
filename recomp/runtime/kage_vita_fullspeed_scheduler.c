/* Parity-aware 60-Hz outer cadence for the physical Vita port.
 *
 * Repentance already owns the important 30-Hz rule.  Its Manager::Update
 * increments Manager+0x4a264 once per outer tick and calls Game::Update only
 * on one parity; Manager::Render also admits the interpolated other parity.
 * This module never calls guest code and never writes either that counter or
 * Game+0x1a30dc.  It schedules the unchanged outer wrapper at 60 Hz, observes
 * those two counters at frozen generated seams, and renders the game's native
 * interpolation phase while the next wrapper deadline still has time.  A late
 * interpolation phase remains the first work discarded.  Slow frames are
 * repaid by ordinary wrapper ticks with bounded debt and bounded render
 * starvation.  Game::Update itself has a separate monotonic 30-Hz floor: stale
 * wall-clock debt is dropped, never replayed as a burst of simulation ticks.
 * (ISAAC_VITA_FULLSPEED_FLOOR_DEBT=ON keeps up to a capped few milliseconds
 * of ordinary lateness on the floor instead of dropping it; stale debt is
 * still dropped.  See kage_vita_fullspeed_floor_debt_anchor.)
 *
 * Any sequencing or parity invariant required by the gates fails open: the
 * original Render and software limiter run from that point onward.
 */
#include "kage_vita_fullspeed_scheduler.h"
#include "kage_vita_guest_sampler.h"
#include "vita_host_services.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
# include "kage_vita_backend.h"
#endif

#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
# if !defined(ISAAC_VITA_FULLSPEED_SCHEDULER) || !defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
#  error "full-head advance requires fullspeed cadence and pre-Game service samples"
# endif
# define KAGE_VITA_HEAD_ADVANCE 1
/* Experimental model parameter, NOT a hardware-calibrated optimum. This
 * advances preparation of a full head only; published render deadlines and
 * the Game floor are not advanced. Keep smaller than one wrapper period. */
# define KAGE_VITA_HEAD_ADVANCE_UNITS (4500u * 3u)
#endif

#ifndef ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID
# define ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID "cadence30:unstamped"
#endif

void isaac_vita_log(const char *format, ...);
extern int sceKernelDelayThread(unsigned int delay_us);

#if defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT) && ISAAC_VITA_FULLSPEED_FLOOR_DEBT
/* S5 floor debt (ISAAC_VITA_FULLSPEED_FLOOR_DEBT, with the stale cut
 * described below).  1 = OBSERVE, 2 = ON. */
# if ISAAC_VITA_FULLSPEED_FLOOR_DEBT != 1 && ISAAC_VITA_FULLSPEED_FLOOR_DEBT != 2
#  error "ISAAC_VITA_FULLSPEED_FLOOR_DEBT must be 1 (OBSERVE) or 2 (ON)"
# endif
# if !defined(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US) || \
     ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US < 1 || \
     ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US > 16666
#  error "floor debt cap must be 1..16666 us (below one 60-Hz wrapper tick)"
# endif
# if defined(KAGE_VITA_HEAD_ADVANCE)
#  error "floor debt and full-head advance are not proven together"
# endif
# define KAGE_VITA_FLOOR_DEBT_MODE ISAAC_VITA_FULLSPEED_FLOOR_DEBT
# define KAGE_VITA_FLOOR_DEBT_CAP_UNITS \
    ((uint64_t)ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US * 3u)
static KageVitaFullspeedFloorDebtSnapshot s_floor_debt;
/* ON: the interpolated frame just admitted was inside the slack band; the
 * next pace's lateness is attributed to it (slack(l,us)). */
static uint32_t s_slack_admit_pending;
/* A stale wrapper drop or a loop-head clock reset happened since the last
 * pace: the grid moved without the floor, so the next pace keeps nothing. */
static uint32_t s_floor_debt_head_stale;
#endif
#if defined(ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US) && ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US
# if !defined(KAGE_VITA_FLOOR_DEBT_MODE)
#  error "admit slack without floor debt lowers UPS: never ship it alone"
# endif
# if ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US >= ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US
#  error "admit slack must be smaller than the floor debt cap"
# endif
# define KAGE_VITA_ADMIT_SLACK_UNITS \
    ((uint64_t)ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US * 3u)
#endif
/* The deadline the interpolated-frame time test compares against.  Only ON
 * with a slack moves it; OFF and OBSERVE keep the stock token. */
#if defined(KAGE_VITA_ADMIT_SLACK_UNITS) && KAGE_VITA_FLOOR_DEBT_MODE == 2
# define KAGE_VITA_ADMIT_DEADLINE_UNITS admit_deadline_units
#else
# define KAGE_VITA_ADMIT_DEADLINE_UNITS s_next_game_due_units
#endif

enum KageVitaFullspeedTickFlag {
    KAGE_VITA_TICK_SERVICE = 1u << 0,
    KAGE_VITA_TICK_MANAGER_DISPATCH = 1u << 1,
    KAGE_VITA_TICK_MANAGER_ENTRY = 1u << 2,
    KAGE_VITA_TICK_GAME_BEGIN = 1u << 3,
    KAGE_VITA_TICK_GAME_END = 1u << 4,
    KAGE_VITA_TICK_RENDER_GATE = 1u << 5,
    KAGE_VITA_TICK_RENDER_ENTRY = 1u << 6,
    KAGE_VITA_TICK_RENDER_BODY = 1u << 7,
    KAGE_VITA_TICK_PRESENT = 1u << 8,
    KAGE_VITA_TICK_RENDER_RETURN = 1u << 9,
    KAGE_VITA_TICK_GAME_POINTER_PUBLISH = 1u << 10,
    KAGE_VITA_TICK_MANAGER_COUNTER_REBASE = 1u << 11,
    /* Host-only receipt: the Render gate validated a Continue without Game.
     * Never emitted by a guest seam; expires at the next loop head. */
    KAGE_VITA_TICK_CONTINUE_WITHOUT_GAME = 1u << 12
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    , KAGE_VITA_TICK_VIDEO_SERVICE = 1u << 13
#endif
};

static uint64_t s_next_due_units;
static uint64_t s_current_due_units;
static uint64_t s_last_due_units;
static uint64_t s_next_game_due_units;
static uint64_t s_last_clock_us;
static uint32_t s_dropped_us_unit_remainder;
static KageVitaFullspeedSnapshot s_counters;
static uint32_t s_initialized;
static uint32_t s_runtime_enabled;
static uint32_t s_have_last_due;
static uint32_t s_have_game_due;
static uint64_t s_update_begin_us;
static uint64_t s_last_update_units;
static uint32_t s_have_last_update;
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
static uint64_t s_tick_head_us;
static uint64_t s_pre_game_units;
static uint32_t s_pre_game_manager;
static uint32_t s_pre_game_game;
static uint32_t s_pre_game_counter;
/* 1: sampled on this full tick; 2: its immediately following non-full tick. */
static uint32_t s_pre_game_age;
#endif
static uint64_t s_render_entry_us;
static uint64_t s_last_render_units;
static uint32_t s_have_last_render;
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
/* Render cost for the non-full admission test only.  int2j tour 3
 * (2026-09-17 issue re-check): every light-room window presented 100 per
 * 120 loops with skip(a,l,n,f)=20,0,20,0 at rnd50 6.4 ms, swp50 0.1 ms,
 * rnd95 17.1 ms.  One Render in five paid a ~10.6-ms wait inside Present,
 * the inclusive s_last_render_units then refused the next interpolated
 * frame, and the refusal put the next wait on a full-phase Render again.
 * The admission cost is therefore the last Render body (entry to the
 * Present seam) plus only the part of its Present wait that the Render
 * before it also paid: an isolated wait is not predicted, persistent
 * back-pressure (every Present waits) still is.  s_last_render_units stays
 * inclusive for the floor realignment and the render-bound full phase.
 * This is a bet that the wait will not land on the admitted frame: in the
 * host oracle it wins when the wait ends on the 60-Hz edge (120 presents at
 * exactly 30 UPS) and costs simulation time when the wait has a fixed length
 * (120 presents at 29.73 UPS for 10.6 ms) or the GPU is the bottleneck. */
static uint64_t s_present_entry_us;
static uint64_t s_last_present_wait_units;
static uint32_t s_have_last_present_wait;
static uint64_t s_render_admission_units;
#endif
static uint32_t s_tick_flags;
static uint32_t s_render_decision;
static uint32_t s_manager_pointer;
static uint32_t s_manager_counter_before;
static uint32_t s_interpolation_enabled;
static uint32_t s_game_pointer;
static uint32_t s_game_frame_before;
static uint32_t s_game_frame_after;
static uint32_t s_game_update_return_site;
static uint32_t s_consecutive_full_render_skips;
static uint32_t s_logged;
static uint32_t s_disable_logged;
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
static uint32_t s_previous_tick_had_game;
static uint32_t s_video_priority_logged;

static void kage_vita_fullspeed_video_priority_receipt(uint32_t full)
{
    if (s_video_priority_logged)
        return;
    s_video_priority_logged = 1u;
    isaac_vita_log("KAGE VITA VIDEO PRIORITY: tick=%u full=%u single-clip=1 "
                   "game-current/previous=0/0 late-render=admitted",
                   (unsigned)s_counters.wrapper_ticks, (unsigned)full);
}
#endif
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
/* port.ini fps_lock duty (design section 2): the non-full phase n (the
 * s_counters.nonfull_phases value after its increment, so the first one is
 * n = 1) is admitted when n % k == 0; k = 0 admits none.  With the counter
 * starting at 1 the present pattern per full/non-full pair is
 *   k=1  F N        (60)
 *   k=2  F _ F N    (45)
 *   k=3  F _ F _ F N (40)
 *   k=0  F _        (30)
 * The counter is zeroed by the per-epoch clear together with the rest of
 * s_counters; s_duty_k itself is boot state and is never cleared.  The
 * duty is a cap: another_tick_due and the affordability test still apply to
 * admitted phases exactly as without it. */
static uint8_t s_duty_k = 1u;

void kage_vita_fullspeed_scheduler_set_duty(uint8_t k)
{
    s_duty_k = k <= 3u ? k : 1u;
}

static inline int kage_vita_fullspeed_duty_admits(uint32_t nonfull_phases)
{
    return s_duty_k == 0u ? 0 : (nonfull_phases % s_duty_k) == 0u;
}

/* 1 -> 60, 2 -> 45, 3 -> 40, 0 -> 30 for the CADENCE30 banner. */
static unsigned kage_vita_fullspeed_duty_fps_lock(void)
{
    switch (s_duty_k) {
    case 0u:
        return 30u;
    case 2u:
        return 45u;
    case 3u:
        return 40u;
    default:
        return 60u;
    }
}
#endif

#if defined(KAGE_VITA_HEAD_ADVANCE)
static uint64_t s_head_advance_credit_units;
static uint64_t s_head_realign_credit_units;
static uint64_t s_head_realign_restore_units;
static uint32_t s_head_expected_manager;
static uint32_t s_head_expected_game;
static uint32_t s_head_expected_counter;
static uint32_t s_head_expected_frame;

static void kage_vita_fullspeed_clear_head_advance(void)
{
    s_head_advance_credit_units = 0u;
    s_head_realign_credit_units = 0u;
    s_head_realign_restore_units = 0u;
    s_head_expected_manager = 0u;
    s_head_expected_game = 0u;
    s_head_expected_counter = 0u;
    s_head_expected_frame = 0u;
}

static uint64_t kage_vita_fullspeed_head_target(uint64_t now_units)
{
    uint64_t target;

    /* Predict only from the completed, immediately preceding non-full tick
     * and its preceding advancing Game sample. Current identity is validated
     * at Manager entry; a prediction refusal must not invent a guest fault. */
    if (!s_runtime_enabled || !s_initialized || !s_have_last_due ||
            now_units >= s_next_due_units || !s_have_game_due ||
            s_next_game_due_units <= now_units || s_pre_game_age != 2u ||
            s_pre_game_units > KAGE_VITA_HEAD_ADVANCE_UNITS ||
            KAGE_VITA_HEAD_ADVANCE_UNITS >= KAGE_VITA_FULLSPEED_TICK_UNITS ||
            s_next_due_units <= KAGE_VITA_HEAD_ADVANCE_UNITS ||
            !(s_manager_counter_before & 1u) ||
            s_manager_counter_before == UINT32_MAX ||
            s_pre_game_counter == UINT32_MAX ||
            s_pre_game_counter + 1u != s_manager_counter_before ||
            !s_game_pointer || s_pre_game_manager != s_manager_pointer ||
            s_pre_game_game != s_game_pointer ||
            s_game_frame_before != s_game_frame_after ||
            !s_have_last_update || !s_have_last_render ||
            s_last_update_units > UINT64_MAX - s_last_render_units ||
            s_last_update_units + s_last_render_units >=
                KAGE_VITA_FULLSPEED_GAME_TICK_UNITS ||
            (s_tick_flags & (KAGE_VITA_TICK_MANAGER_COUNTER_REBASE |
                             KAGE_VITA_TICK_GAME_POINTER_PUBLISH)))
        return s_next_due_units;
    target = s_next_due_units - KAGE_VITA_HEAD_ADVANCE_UNITS;
    if (target <= s_last_due_units)
        return s_next_due_units;
    s_head_expected_manager = s_manager_pointer;
    s_head_expected_game = s_game_pointer;
    s_head_expected_counter = s_manager_counter_before + 1u;
    s_head_expected_frame = s_game_frame_after;
    return target;
}
#endif

static void kage_vita_fullspeed_add_u32(
    uint32_t *value, uint64_t increment)
{
    uint64_t sum = (uint64_t)*value + increment;
    *value = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

static uint32_t kage_vita_fullspeed_units_to_us(uint64_t units)
{
    uint64_t microseconds = (units + 2u) / 3u;
    return microseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)microseconds;
}

static int kage_vita_fullspeed_clock_units(
    uint64_t microseconds, uint64_t *units)
{
    if (!units || microseconds > UINT64_MAX / 3u)
        return 0;
    *units = microseconds * 3u;
    return 1;
}

#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
#define KAGE_VITA_STR2(x) #x
#define KAGE_VITA_STR(x) KAGE_VITA_STR2(x)

/* S5 floor debt, called by the Game::Update pace once its floor wait (if
 * any) is over, with the stock anchor (the old floor after a wait, the
 * clock otherwise).  Returns the anchor the next floor is built on.
 *
 * Late path (no wait, clock at or after a live floor): the update starts
 * late_units after its floor.  ON keeps min(late, CAP, lateness after this
 * full head's own grid deadline) of that as debt, so the next floor is
 * max(old floor, now - CAP, this head's grid deadline) + one Game tick:
 * never earlier than old floor + 33.333 ms, never more than CAP earlier than
 * stock, never before the next full head's grid deadline (gf <= 0; the grid
 * clamp below).
 *
 * Stale cut: nothing is kept when the
 * lateness is one Game tick or more (menus, Continue, loads, a stall that is
 * not ordinary jitter), or when a stale wrapper drop, a head clock reset or
 * a head sleep that returned with the clock before it started moved the
 * 60-Hz grid since the previous pace (the grid rule).  Rule A keeps a load,
 * pause or long stall from being repaid; the grid rule keeps nothing after
 * the grid itself jumped.  The grid clamp alone already keeps gf <= 0 after
 * a grid jump: without the grid rule the host oracle passes every behaviour
 * check, its heavy rooms start and render exactly as with it (same digest;
 * their post-drop paces are merely filed as late), and only the two
 * clock-fault scenarios keep debt across the fault.  The rule stays as the
 * conservative choice there.  OBSERVE counts and always returns the stock
 * anchor. */
static uint64_t kage_vita_fullspeed_floor_debt_anchor(
    uint32_t waited_for_floor, uint64_t now_units, uint64_t stock_anchor_units)
{
    uint64_t anchor_units = stock_anchor_units;
    uint32_t head_stale = s_floor_debt_head_stale;
    uint32_t slack_admit = s_slack_admit_pending;

# if defined(ISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_HEAD_STALE_CUT)
    /* Host-oracle negative control only: rule A (one Game tick) alone. */
    head_stale = 0u;
# endif
    s_floor_debt_head_stale = 0u;
    s_slack_admit_pending = 0u;
    if (waited_for_floor || (s_have_game_due &&
                             now_units == s_next_game_due_units)) {
        /* On time: slept to the floor, or reached it exactly. */
        kage_vita_fullspeed_add_u32(&s_floor_debt.paces, 1u);
    } else if (s_have_game_due && now_units > s_next_game_due_units) {
        uint64_t late_units = now_units - s_next_game_due_units;

        kage_vita_fullspeed_add_u32(&s_floor_debt.paces, 1u);
        if (head_stale ||
                late_units >= KAGE_VITA_FULLSPEED_GAME_TICK_UNITS) {
            kage_vita_fullspeed_add_u32(&s_floor_debt.stale, 1u);
            if (late_units < KAGE_VITA_FULLSPEED_GAME_TICK_UNITS)
                kage_vita_fullspeed_add_u32(&s_floor_debt.stale_grid, 1u);
            kage_vita_fullspeed_add_u32(&s_floor_debt.stale_us,
                kage_vita_fullspeed_units_to_us(late_units));
        } else {
            uint64_t kept_units = late_units < KAGE_VITA_FLOOR_DEBT_CAP_UNITS ?
                late_units : KAGE_VITA_FLOOR_DEBT_CAP_UNITS;
            /* Grid clamp (the wake-overshoot case):
             * keep at most this pace's lateness after its own full head's
             * grid deadline, s_next_due_units - TICK.  The rest is lateness
             * of the floor itself: a waited pace's realign (below) moves the
             * grid by the whole wait, wake overshoot included, so a pace-wait
             * overshoot above the head-wait overshoot plus the service leaves
             * the floor ahead of the next full head's grid deadline (gf > 0).
             * Keeping that part would put the next floor ahead of the grid
             * again, and gf would stay > 0 until the next stale event, since
             * every later pace then starts late and none waits or realigns.
             * With the clamp the next floor is never before the next full
             * head's grid deadline (s_next_due_units + TICK, the gf sample
             * point), so a late pace always leaves gf <= 0.  On this late
             * path no realign follows, so s_next_due_units is final here.
             * Whole microseconds, so late_us == kept_us + over_us holds.
             * OBSERVE applies it to its accounting too, but on stock's
             * trajectory: a waited pace there realigns the grid by its whole
             * wake overshoot, so the clamp binds at most late paces after
             * one, while ON leaves that state at its first
             * late pace and rarely binds again.  OBSERVE's kept is therefore
             * not what ON would keep; clamp_us (min(late, cap) - kept) is
             * counted apart so kept_us + clamp_us = sum min(late, cap), the
             * part of the lateness the cap lets ON keep. */
            uint64_t grid_units = s_next_due_units +
                KAGE_VITA_FULLSPEED_TICK_UNITS;
            uint64_t capped_units = kept_units;
            uint64_t room_units = 0u;
            uint32_t late_us = kage_vita_fullspeed_units_to_us(late_units);
            /* Bucket 0: the pace began inside its floor's own microsecond
             * (the clock is whole us, the floor is in 1/3 us). */
            uint32_t bucket = late_units < 3u ? 0u : late_us < 250u ? 1u :
                late_us < 1000u ? 2u : late_us < 2000u ? 3u :
                late_us < 5000u ? 4u : late_us < 16667u ? 5u : 6u;

            /* now + GAME_TICK cannot wrap: the pace disabled the scheduler
             * before calling here if it could. */
            if (grid_units >= s_next_due_units &&
                    now_units + KAGE_VITA_FULLSPEED_GAME_TICK_UNITS > grid_units)
                room_units = now_units + KAGE_VITA_FULLSPEED_GAME_TICK_UNITS -
                    grid_units;
# if !defined(ISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_GRID_CLAMP) && \
     !defined(ISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_STALE_CUT)
            if (kept_units > room_units)
                kept_units = room_units - room_units % 3u;
# else
            /* Host-oracle negative controls only: no grid clamp (round 0). */
            (void)room_units;
# endif
            kage_vita_fullspeed_add_u32(&s_floor_debt.late, 1u);
            kage_vita_fullspeed_add_u32(&s_floor_debt.late_us, late_us);
            kage_vita_fullspeed_add_u32(&s_floor_debt.late_hist[bucket], 1u);
            if (late_us > s_floor_debt.late_max_us)
                s_floor_debt.late_max_us = late_us;
            kage_vita_fullspeed_add_u32(&s_floor_debt.kept_us,
                kage_vita_fullspeed_units_to_us(kept_units));
            if (kept_units < late_units) {
                kage_vita_fullspeed_add_u32(&s_floor_debt.over, 1u);
                kage_vita_fullspeed_add_u32(&s_floor_debt.over_us,
                    kage_vita_fullspeed_units_to_us(late_units - kept_units));
            }
            /* The clamped kept is a multiple of 3 units, so the rounded
             * clamp_us adds up: kept_us + clamp_us = ceil(min(late, cap) / 3). */
            if (kept_units < capped_units) {
                kage_vita_fullspeed_add_u32(&s_floor_debt.clamp, 1u);
                kage_vita_fullspeed_add_u32(&s_floor_debt.clamp_us,
                    kage_vita_fullspeed_units_to_us(capped_units - kept_units));
            }
            if (slack_admit) {
                kage_vita_fullspeed_add_u32(&s_floor_debt.slack_late, 1u);
                kage_vita_fullspeed_add_u32(&s_floor_debt.slack_late_us,
                                            late_us);
            }
# if KAGE_VITA_FLOOR_DEBT_MODE == 2
            anchor_units = now_units - kept_units;
# endif
        }
# if KAGE_VITA_FLOOR_DEBT_MODE == 2 && \
     defined(ISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_STALE_CUT)
        /* Host-oracle negative control only (the design draft without the
         * stale cut): every late pace keeps min(late, CAP). */
        anchor_units = now_units - (late_units < KAGE_VITA_FLOOR_DEBT_CAP_UNITS ?
            late_units : KAGE_VITA_FLOOR_DEBT_CAP_UNITS);
# endif
    }
    return anchor_units;
}

/* gf after a pace: the next full head's grid deadline (the next head is the
 * non-full one, s_next_due_units) minus the floor just set, in signed us. */
static void kage_vita_fullspeed_floor_debt_note_grid(void)
{
    uint64_t grid_units = s_next_due_units + KAGE_VITA_FULLSPEED_TICK_UNITS;
    int32_t gf_us;

    if (grid_units < s_next_due_units)
        return;
    if (grid_units >= s_next_game_due_units) {
        uint32_t us = kage_vita_fullspeed_units_to_us(
            grid_units - s_next_game_due_units);
        gf_us = us > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)us;
        if (grid_units > s_next_game_due_units)
            kage_vita_fullspeed_add_u32(&s_floor_debt.gf_pos, 1u);
    } else {
        uint32_t us = kage_vita_fullspeed_units_to_us(
            s_next_game_due_units - grid_units);
        gf_us = us > (uint32_t)INT32_MAX ? -INT32_MAX : -(int32_t)us;
    }
    if (s_floor_debt.gf_n == 0u || gf_us > s_floor_debt.gf_max_us)
        s_floor_debt.gf_max_us = gf_us;
    if (s_floor_debt.gf_n == 0u || gf_us < s_floor_debt.gf_min_us)
        s_floor_debt.gf_min_us = gf_us;
    s_floor_debt.gf_last_us = gf_us;
    kage_vita_fullspeed_add_u32(&s_floor_debt.gf_n, 1u);
}
#endif

/* Early RET tokens whose unchanged-frame return is accepted rather than
 * failed open.  The generated hooks only report the eight machine-pinned
 * sites, so membership here is the sole policy decision. */
static int kage_vita_fullspeed_zero_frame_site(uint32_t site)
{
    if (site == KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_RETURN_RVA ||
            site == KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE121_RVA ||
            site == KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE362_RVA)
        return 1;
#if defined(ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES)
    /* Exit-to-menu returns observed on hardware; see the header. */
    if (site == KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE099_RVA ||
            site == KAGE_VITA_FULLSPEED_GAME_UPDATE_ZERO_FRAME_2CE0EA_RVA)
        return 1;
#endif
    return 0;
}

static void kage_vita_fullspeed_disable(void)
{
#if defined(KAGE_VITA_HEAD_ADVANCE)
    kage_vita_fullspeed_clear_head_advance();
    s_pre_game_age = 0u;
#endif
    if (s_runtime_enabled) {
        s_runtime_enabled = 0u;
        s_render_decision = 1u;
        kage_vita_fullspeed_add_u32(&s_counters.runtime_disables, 1u);
        if (!s_disable_logged) {
            s_disable_logged = 1u;
            isaac_vita_log(
                "KAGE VITA CADENCE30 DISABLE: tick=%u flags=%03x "
                "mgr=%08x ctr=%u interp=%u game=%08x frame=%u>%u "
                "ret=%08x viol=%u/%u/%u/%u/%u/%u/%u debt=%u",
                (unsigned)s_counters.wrapper_ticks,
                (unsigned)s_tick_flags,
                (unsigned)s_manager_pointer,
                (unsigned)s_manager_counter_before,
                (unsigned)s_interpolation_enabled,
                (unsigned)s_game_pointer,
                (unsigned)s_game_frame_before,
                (unsigned)s_game_frame_after,
                (unsigned)s_game_update_return_site,
                (unsigned)s_counters.duplicate_tick_violations,
                (unsigned)s_counters.parity_violations,
                (unsigned)s_counters.game_frame_violations,
                (unsigned)s_counters.sequence_violations,
                (unsigned)s_counters.manager_counter_rebase_violations,
                (unsigned)s_counters.game_pointer_violations,
                (unsigned)s_counters.clock_resets,
                (unsigned)s_counters.debt_us);
        }
    }
}

static void kage_vita_fullspeed_sequence_failure(void)
{
    kage_vita_fullspeed_add_u32(&s_counters.sequence_violations, 1u);
    kage_vita_fullspeed_disable();
}

/* One bounded extra record before the existing first DISABLE record. The
 * caller immediately disables; subsequent gates return before reaching here.
 * Values on the right are the gate arguments, not stale entry snapshots. */
static void kage_vita_fullspeed_render_failure(
    const char *reason, uint32_t manager, uint32_t counter,
    uint32_t interpolation, uint32_t game, uint32_t frame)
{
    if (!s_disable_logged)
        isaac_vita_log(
            "KAGE VITA CADENCE30 GATE: reason=%s tick=%u flags=%03x "
            "saved/current mgr=%08x/%08x ctr=%u/%u interp=%u/%u "
            "game=%08x/%08x frame=%u/%u",
            reason, (unsigned)s_counters.wrapper_ticks, (unsigned)s_tick_flags,
            (unsigned)s_manager_pointer, (unsigned)manager,
            (unsigned)s_manager_counter_before, (unsigned)counter,
            (unsigned)s_interpolation_enabled, (unsigned)interpolation,
            (unsigned)s_game_pointer, (unsigned)game,
            (unsigned)s_game_frame_after, (unsigned)frame);
}

static void kage_vita_fullspeed_game_pointer_failure(void)
{
    kage_vita_fullspeed_add_u32(
        &s_counters.game_pointer_violations, 1u);
    kage_vita_fullspeed_sequence_failure();
}

static void kage_vita_fullspeed_manager_counter_rebase_failure(void)
{
    kage_vita_fullspeed_add_u32(
        &s_counters.manager_counter_rebase_violations, 1u);
    kage_vita_fullspeed_sequence_failure();
}

static int kage_vita_fullspeed_note_flag_once(uint32_t flag)
{
    if (!s_initialized || s_counters.wrapper_ticks == 0u) {
        kage_vita_fullspeed_sequence_failure();
        return 0;
    }
    if ((s_tick_flags & flag) != 0u) {
        kage_vita_fullspeed_add_u32(
            &s_counters.duplicate_tick_violations, 1u);
        kage_vita_fullspeed_disable();
        return 0;
    }
    s_tick_flags |= flag;
    return 1;
}

static int kage_vita_fullspeed_note_once(
    uint32_t flag, uint32_t *counter)
{
    kage_vita_fullspeed_add_u32(counter, 1u);
    return kage_vita_fullspeed_note_flag_once(flag);
}

static void kage_vita_fullspeed_drop_ticks(uint64_t ticks)
{
    uint64_t units;
    uint64_t whole_us;
    uint32_t remainder;

    if (ticks == 0u)
        return;
    kage_vita_fullspeed_add_u32(&s_counters.dropped_ticks, ticks);
    if (ticks > UINT64_MAX / KAGE_VITA_FULLSPEED_TICK_UNITS) {
        s_counters.dropped_us = UINT32_MAX;
        return;
    }
    units = ticks * KAGE_VITA_FULLSPEED_TICK_UNITS;
    whole_us = units / 3u;
    remainder = (uint32_t)(units % 3u) +
        s_dropped_us_unit_remainder;
    whole_us += remainder / 3u;
    s_dropped_us_unit_remainder = remainder % 3u;
    kage_vita_fullspeed_add_u32(&s_counters.dropped_us, whole_us);
}

static int kage_vita_fullspeed_wait_until(
    uint64_t due_units, uint64_t *now_us, uint64_t *now_units)
{
    uint32_t attempts = 0u;

    for (;;) {
        uint64_t before = isaac_vita_get_process_time();
        uint64_t before_units;
        uint64_t remaining_units;
        uint64_t requested_us;
        uint64_t after;
        int result;

        if (!kage_vita_fullspeed_clock_units(before, &before_units)) {
            kage_vita_fullspeed_disable();
            return 0;
        }
        if (before_units >= due_units) {
            *now_us = before;
            *now_units = before_units;
            return 1;
        }
        remaining_units = due_units - before_units;
        requested_us = (remaining_units + 2u) / 3u;
        if (requested_us > UINT_MAX)
            requested_us = UINT_MAX;
        if (requested_us == 0u)
            requested_us = 1u;

        kage_vita_fullspeed_add_u32(&s_counters.wait_calls, 1u);
        result = sceKernelDelayThread((unsigned int)requested_us);
        after = isaac_vita_get_process_time();
        if (after >= before)
            kage_vita_fullspeed_add_u32(
                &s_counters.waited_us, after - before);
        if (result < 0) {
            kage_vita_fullspeed_disable();
            *now_us = after;
            if (!kage_vita_fullspeed_clock_units(after, now_units))
                *now_units = 0u;
            return 0;
        }
        if (after < before) {
            kage_vita_fullspeed_add_u32(&s_counters.clock_resets, 1u);
            if (!kage_vita_fullspeed_clock_units(after, now_units)) {
                kage_vita_fullspeed_disable();
                return 0;
            }
            *now_us = after;
            return 2;
        }
        if (after == before && ++attempts >= 8u) {
            kage_vita_fullspeed_disable();
            *now_us = after;
            *now_units = before_units;
            return 0;
        }
    }
}

static void kage_vita_fullspeed_finish_previous_tick(void)
{
    uint32_t required;

    if (s_counters.wrapper_ticks == 0u || !s_runtime_enabled)
        return;
    required = KAGE_VITA_TICK_SERVICE |
        KAGE_VITA_TICK_MANAGER_DISPATCH |
        KAGE_VITA_TICK_MANAGER_ENTRY |
        KAGE_VITA_TICK_RENDER_GATE;
    if ((s_tick_flags & required) != required)
        kage_vita_fullspeed_sequence_failure();
    if ((((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u) !=
             ((s_tick_flags & KAGE_VITA_TICK_GAME_END) != 0u)) ||
            ((s_tick_flags & (KAGE_VITA_TICK_MANAGER_COUNTER_REBASE |
                              KAGE_VITA_TICK_GAME_POINTER_PUBLISH)) != 0u &&
             (s_tick_flags & (KAGE_VITA_TICK_GAME_BEGIN |
                              KAGE_VITA_TICK_CONTINUE_WITHOUT_GAME)) == 0u))
        kage_vita_fullspeed_sequence_failure();
    if ((s_tick_flags & KAGE_VITA_TICK_RENDER_ENTRY) != 0u) {
        required = KAGE_VITA_TICK_RENDER_BODY |
            KAGE_VITA_TICK_PRESENT |
            KAGE_VITA_TICK_RENDER_RETURN;
        if ((s_tick_flags & required) != required)
            kage_vita_fullspeed_sequence_failure();
    }
}

static void kage_vita_fullspeed_pace_game_update(void)
{
    uint64_t now_us;
    uint64_t now_units;
    int wait_result;

    if (!s_runtime_enabled)
        return;
    now_us = isaac_vita_get_process_time();
    if (!kage_vita_fullspeed_clock_units(now_us, &now_units)) {
        kage_vita_fullspeed_disable();
        return;
    }
#if defined(KAGE_VITA_HEAD_ADVANCE)
    /* The original floor may recover a clock retreat by waiting. Do not let
     * that successful wait make credit from the old head clock epoch live
     * again; use this existing pre-floor clock read, not another sample. */
    if (now_us < s_tick_head_us || now_us < s_last_clock_us)
        kage_vita_fullspeed_clear_head_advance();
#endif
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
    /* Reuse the two existing clock reads.  The head timestamp is AFTER its
     * wait; this one is BEFORE the Game floor wait.  Thus this sample includes
     * input/service/Manager prologue, but neither sleep nor Game::Update. */
    s_pre_game_age = 0u;
    if (now_us >= s_tick_head_us &&
            kage_vita_fullspeed_clock_units(
                now_us - s_tick_head_us, &s_pre_game_units) &&
            s_pre_game_units < KAGE_VITA_FULLSPEED_GAME_TICK_UNITS) {
        s_pre_game_manager = s_manager_pointer;
        s_pre_game_game = s_game_pointer;
        s_pre_game_counter = s_manager_counter_before;
        s_pre_game_age = 1u;
    }
#endif
    uint64_t floor_anchor_units;
    uint64_t floor_wait_units = 0u;
    uint32_t waited_for_floor = 0u;

    if (s_have_game_due && now_units < s_next_game_due_units) {
        uint64_t before_units = now_units;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        /* pace(c,us) in ph120.ik is the only reader; production and perf
         * builds without the census compile the scheduler exactly as before. */
        uint32_t wait_calls_before = s_counters.wait_calls;
        uint32_t waited_us_before = s_counters.waited_us;
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        /* The sleep runs inside the profiler's upd phase before the
         * Game::Update marker exists; give the guest sampler its own bucket
         * so it is not reported as Manager::Update CPU. */
        uint32_t sampler_phase = g_kage_vita_guest_sampler_phase;

        KAGE_VITA_GUEST_SAMPLER_PHASE(KAGE_VITA_GUEST_SAMPLER_PACE);
#endif
        wait_result = kage_vita_fullspeed_wait_until(
            s_next_game_due_units, &now_us, &now_units);
#if defined(ISAAC_VITA_GUEST_SAMPLER)
        KAGE_VITA_GUEST_SAMPLER_PHASE(sampler_phase);
#endif
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        kage_vita_fullspeed_add_u32(
            &s_counters.pace_wait_calls,
            s_counters.wait_calls - wait_calls_before);
        kage_vita_fullspeed_add_u32(
            &s_counters.pace_waited_us,
            s_counters.waited_us - waited_us_before);
#endif
        if (wait_result == 0)
            return;
        if (wait_result == 2) {
            s_have_game_due = 0u;
#if defined(KAGE_VITA_HEAD_ADVANCE)
            kage_vita_fullspeed_clear_head_advance();
#endif
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
            s_pre_game_age = 0u;
#endif
        } else if (wait_result == 1) {
            waited_for_floor = 1u;
            if (now_units > before_units)
                floor_wait_units = now_units - before_units;
        }
    }
    if (now_units > UINT64_MAX - KAGE_VITA_FULLSPEED_GAME_TICK_UNITS) {
        kage_vita_fullspeed_disable();
        return;
    }

    /* Base the next deadline on the update that is actually about to execute,
     * not on an old absolute deadline.  A load or GPU stall therefore drops
     * stale simulation time instead of issuing several Game::Update calls in
     * rapid succession.
     *
     * When this call slept to reach the floor, anchor the next floor to the
     * deadline it slept FOR, not to the clock after waking.  sceKernelDelayThread
     * returns late by a small, roughly constant amount; anchoring to the
     * post-wake clock added that overshoot to every period, so the floor ran at
     * 33.33 ms + overshoot while the wrapper grid stayed at exactly 16.67 ms
     * per tick.  Bundle30 measured the result on hardware: 29.84 UPS (33.51 ms
     * period), scheduler debt growing ~11 ms per 120-tick window, 60 mid-tick
     * floor waits of ~27.6 ms per window, every render gate reporting
     * another_tick_due, 12 presents per 120 ticks, and a reset every ~5 ticks
     * when the stale-catchup drop fired.  With an ideal clock the two anchors
     * coincide, so this is invisible to the host oracle; on hardware it keeps
     * the 30-Hz floor phase-locked to the 60-Hz wrapper grid.  The late path
     * (no wait) still re-anchors to the current clock so stalls drop stale
     * simulation time exactly as before. */
    floor_anchor_units = waited_for_floor ? s_next_game_due_units : now_units;
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
    /* S5 floor debt: see kage_vita_fullspeed_floor_debt_anchor.  The
     * overflow guard below applies to its anchor too. */
    floor_anchor_units = kage_vita_fullspeed_floor_debt_anchor(
        waited_for_floor, now_units, floor_anchor_units);
#endif
    if (floor_anchor_units > UINT64_MAX - KAGE_VITA_FULLSPEED_GAME_TICK_UNITS)
        floor_anchor_units = now_units;
    s_next_game_due_units =
        floor_anchor_units + KAGE_VITA_FULLSPEED_GAME_TICK_UNITS;
    s_have_game_due = 1u;
    s_update_begin_us = now_us;

    /* The floor wait above happens after the loop head has already committed
     * this tick's wrapper deadline.  Whatever was slept is therefore counted
     * against the tick: with a 27-30 ms floor wait the render gate of this
     * full phase is always 12+ ms past its own deadline, the following
     * non-full head inherits the lateness, and the wrapper can never recover
     * because "catching up" means running ticks faster than 60 Hz while the
     * floor forbids running Game::Update faster than 30 Hz.  Bundle31 froze in
     * exactly that state: every in-game window 60 updates / 12 presents,
     * 60 floor waits of ~27 ms, wrapper debt flat, drops zero.
     *
     * When the floor made us sleep, the loop had idle time, so realigning the
     * presentation grid is free.  The grid is behind the floor by exactly the
     * time slept, so shift every pending wrapper deadline by that amount: the
     * non-full tick then falls one tick after this update and the next full
     * head lands on the next floor, while the head-to-update offset (service
     * work) is preserved.  Snapping to `now + tick` instead would re-add that
     * offset on every microsecond-sized floor wait and slip the grid by the
     * service time per cycle (the host oracle catches this as a 59.5-Hz
     * wrapper in its 27-ms no-interpolation scenario).  On the late path (no sleep) nothing changes, so throughput-bound
     * behaviour -- skip renders, hold 30 Hz -- is exactly as before, and with an
     * ideal clock the aligned steady state already satisfies the assignment.
     * No simulation time is discarded here; the floor is the simulation clock
     * and it is untouched.  Only the deadline the render gate compares against
     * moves, and only forwards, never past the next floor.
     *
     * Realign only when the loop is not floor-bound: the most recent
     * Game::Update plus the most recent Render must fit inside one 33.33-ms
     * floor period.  That is the real discriminator.  Bundle32 showed why a
     * wait-length threshold is not: the misaligned state settled at a floor
     * wait of 16.6 ms -- inside the 1.7-ms gap between "full gate is late"
     * (>= 14.97 ms) and the old "wait >= one tick" test (>= 16.67 ms) -- and
     * the wait threshold also refused rooms whose 20-ms render fits a floor
     * period but not a tick, which can and should present at 30 FPS.  With a
     * 40-55 ms frame update+render exceeds the period, the realign stays off,
     * and the fixed grid with its render skips holds game speed exactly as
     * before, which is the project's stated priority. */
    if (waited_for_floor && s_runtime_enabled &&
            (!s_have_last_render || !s_have_last_update ||
             (s_last_update_units <= UINT64_MAX - s_last_render_units &&
              s_last_update_units + s_last_render_units <
                  KAGE_VITA_FULLSPEED_GAME_TICK_UNITS)) &&
            floor_wait_units > 0u &&
            s_next_due_units <= UINT64_MAX - floor_wait_units)
#if defined(KAGE_VITA_HEAD_ADVANCE)
    {
        uint64_t credit = s_head_advance_credit_units;
        if (s_tick_flags & (KAGE_VITA_TICK_MANAGER_COUNTER_REBASE |
                            KAGE_VITA_TICK_GAME_POINTER_PUBLISH))
            credit = 0u;
        if (credit > floor_wait_units)
            credit = floor_wait_units;
        /* The original overflow guard covers restore as well as the smaller
         * adjusted deadline. Retain all recovery not caused by early service.
         * A zero-frame return below restores the original forward shift. */
        s_head_realign_restore_units = s_next_due_units + floor_wait_units;
        s_head_realign_credit_units = credit;
        s_next_due_units = s_head_realign_restore_units - credit;
    }
#else
        s_next_due_units += floor_wait_units;
#endif
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
    if (s_have_last_due)
        kage_vita_fullspeed_floor_debt_note_grid();
#endif
    s_last_clock_us = now_us;
}

static void kage_vita_fullspeed_scheduler_clear(uint32_t runtime_enabled)
{
#if defined(KAGE_VITA_HEAD_ADVANCE)
    kage_vita_fullspeed_clear_head_advance();
#endif
    s_next_due_units = 0u;
    s_current_due_units = 0u;
    s_last_due_units = 0u;
    s_next_game_due_units = 0u;
    s_last_clock_us = 0u;
    s_dropped_us_unit_remainder = 0u;
    memset(&s_counters, 0, sizeof s_counters);
    s_initialized = 0u;
    s_runtime_enabled = runtime_enabled;
    s_have_last_due = 0u;
    s_have_game_due = 0u;
    s_update_begin_us = 0u;
    s_last_update_units = 0u;
    s_have_last_update = 0u;
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
    s_tick_head_us = 0u;
    s_pre_game_units = 0u;
    s_pre_game_manager = 0u;
    s_pre_game_game = 0u;
    s_pre_game_counter = 0u;
    s_pre_game_age = 0u;
#endif
    s_render_entry_us = 0u;
    s_last_render_units = 0u;
    s_have_last_render = 0u;
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    s_present_entry_us = 0u;
    s_last_present_wait_units = 0u;
    s_have_last_present_wait = 0u;
    s_render_admission_units = 0u;
#endif
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
    memset(&s_floor_debt, 0, sizeof s_floor_debt);
    s_slack_admit_pending = 0u;
    s_floor_debt_head_stale = 0u;
#endif
    s_tick_flags = 0u;
    s_render_decision = 1u;
    s_manager_pointer = 0u;
    s_manager_counter_before = 0u;
    s_interpolation_enabled = 0u;
    s_game_pointer = 0u;
    s_game_frame_before = 0u;
    s_game_frame_after = 0u;
    s_game_update_return_site =
        KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA;
    s_consecutive_full_render_skips = 0u;
    s_logged = 0u;
    s_disable_logged = 0u;
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    s_previous_tick_had_game = 0u;
    s_video_priority_logged = 0u;
#endif
}

void kage_vita_fullspeed_scheduler_reset(void)
{
    kage_vita_fullspeed_scheduler_clear(1u);
}

void kage_vita_fullspeed_scheduler_deactivate(void)
{
    /* The next backend activation starts a fresh epoch.  Until then every
     * generated gate must retain the original Render and software limiter. */
    kage_vita_fullspeed_scheduler_clear(0u);
}

void kage_vita_fullspeed_scheduler_note_loop_head(void)
{
#if defined(KAGE_VITA_HEAD_ADVANCE)
    kage_vita_fullspeed_clear_head_advance();
#endif
    uint64_t now_us = isaac_vita_get_process_time();
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
    uint32_t pre_game_clock_ok = now_us >= s_last_clock_us;
#endif
    uint64_t now_units;
    uint64_t due_ticks = 1u;
    uint64_t dropped = 0u;
    uint64_t debt_units = 0u;
    int wait_result;
#if defined(KAGE_VITA_HEAD_ADVANCE)
    uint64_t head_target;
#endif

    kage_vita_fullspeed_finish_previous_tick();
    if (!kage_vita_fullspeed_clock_units(now_us, &now_units)) {
        kage_vita_fullspeed_disable();
        now_units = 0u;
    }

    if (!s_initialized) {
        s_initialized = 1u;
        s_next_due_units = now_units;
    } else if (now_us < s_last_clock_us) {
        kage_vita_fullspeed_add_u32(&s_counters.clock_resets, 1u);
        s_next_due_units = now_units;
        s_have_last_due = 0u;
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
        s_floor_debt_head_stale = 1u;
#endif
    }

#if defined(KAGE_VITA_HEAD_ADVANCE)
    head_target = pre_game_clock_ok ?
        kage_vita_fullspeed_head_target(now_units) : s_next_due_units;
    if (s_runtime_enabled && now_units < head_target) {
        wait_result = kage_vita_fullspeed_wait_until(
            head_target, &now_us, &now_units);
#else
    if (s_runtime_enabled && now_units < s_next_due_units) {
        wait_result = kage_vita_fullspeed_wait_until(
            s_next_due_units, &now_us, &now_units);
#endif
        if (wait_result == 2) {
#if defined(KAGE_VITA_HEAD_ADVANCE)
            kage_vita_fullspeed_clear_head_advance();
#endif
            s_next_due_units = now_units;
            s_have_last_due = 0u;
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
            pre_game_clock_ok = 0u;
#endif
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
            s_floor_debt_head_stale = 1u;
#endif
        }
    }
#if defined(KAGE_VITA_HEAD_ADVANCE)
    if (s_runtime_enabled && s_head_expected_manager &&
            now_units < s_next_due_units) {
        s_head_advance_credit_units = s_next_due_units - now_units;
        /* wait_until never accepts a clock before its requested target. */
        if (s_head_advance_credit_units > KAGE_VITA_HEAD_ADVANCE_UNITS)
            kage_vita_fullspeed_disable();
    }
#endif

    if (s_runtime_enabled) {
#if defined(KAGE_VITA_HEAD_ADVANCE)
        if (now_units < s_next_due_units && !s_head_advance_credit_units) {
#else
        if (now_units < s_next_due_units) {
#endif
            kage_vita_fullspeed_disable();
        } else {
#if defined(KAGE_VITA_HEAD_ADVANCE)
            due_ticks = now_units < s_next_due_units ? 1u :
#else
            due_ticks =
#endif
                (now_units - s_next_due_units) /
                KAGE_VITA_FULLSPEED_TICK_UNITS + 1u;
            if (due_ticks > KAGE_VITA_FULLSPEED_MAX_DUE_TICKS) {
                /* This backlog already crosses the policy's unavoidable
                 * drop boundary.  Keeping five stale wrapper calls made up
                 * to three 30-Hz Game updates arrive almost together after
                 * a load/stall.  Retain current+one catch-up tick instead;
                 * ordinary sub-boundary render recovery is unchanged. */
                dropped = due_ticks -
                    KAGE_VITA_FULLSPEED_STALE_DUE_TICKS;
                s_next_due_units +=
                    dropped * KAGE_VITA_FULLSPEED_TICK_UNITS;
                due_ticks = KAGE_VITA_FULLSPEED_STALE_DUE_TICKS;
                kage_vita_fullspeed_drop_ticks(dropped);
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
                /* The grid just jumped forward without the floor. */
                s_floor_debt_head_stale = 1u;
#endif
            }
            s_current_due_units = s_next_due_units;
            if (s_next_due_units >
                    UINT64_MAX - KAGE_VITA_FULLSPEED_TICK_UNITS) {
                kage_vita_fullspeed_disable();
            } else {
                s_next_due_units += KAGE_VITA_FULLSPEED_TICK_UNITS;
#if defined(KAGE_VITA_HEAD_ADVANCE)
                debt_units = now_units < s_current_due_units ? 0u :
                    now_units - s_current_due_units;
#else
                debt_units = now_units - s_current_due_units;
#endif
                if (s_have_last_due &&
                        s_current_due_units <= s_last_due_units)
                    kage_vita_fullspeed_sequence_failure();
                s_last_due_units = s_current_due_units;
                s_have_last_due = 1u;
            }
        }
    }
    if (!s_runtime_enabled) {
        s_current_due_units = now_units;
        s_next_due_units = now_units <=
                UINT64_MAX - KAGE_VITA_FULLSPEED_TICK_UNITS ?
            now_units + KAGE_VITA_FULLSPEED_TICK_UNITS : now_units;
        debt_units = 0u;
    }

    s_last_clock_us = now_us;
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
    if (s_runtime_enabled && pre_game_clock_ok &&
            now_us >= s_tick_head_us &&
            s_pre_game_age == 1u)
        s_pre_game_age = 2u;
    else
        s_pre_game_age = 0u;
    s_tick_head_us = now_us;
#endif
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    s_previous_tick_had_game = (s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u;
#endif
    s_tick_flags = 0u;
    s_render_decision = 1u;
    s_manager_pointer = 0u;
    s_game_pointer = 0u;
    s_game_frame_before = 0u;
    s_game_frame_after = 0u;
    kage_vita_fullspeed_add_u32(&s_counters.wrapper_ticks, 1u);
    s_counters.debt_us = kage_vita_fullspeed_units_to_us(debt_units);
    if (s_counters.debt_us > s_counters.maximum_debt_us)
        s_counters.maximum_debt_us = s_counters.debt_us;

    if (!s_logged) {
        s_logged = 1u;
        isaac_vita_log(
            "KAGE VITA CADENCE30: bid=%.32s tick_units=%u "
            "game_tick_units=%u max_catchup=%u stale_catchup=%u "
            "max_full_skip=%u "
            "parity=4a264 frame=1a30dc"
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
            " reserve=pre-game"
#endif
#if defined(KAGE_VITA_HEAD_ADVANCE)
            " head-advance=4500us-experimental"
#endif
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
            " render-cost=excludes-present"
#endif
#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
# if KAGE_VITA_FLOOR_DEBT_MODE == 2
            " floor-debt=on"
# else
            " floor-debt=observe"
# endif
            " cap=" KAGE_VITA_STR(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US) "us"
# if defined(KAGE_VITA_ADMIT_SLACK_UNITS)
            " admit-slack=" KAGE_VITA_STR(ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US) "us"
# endif
#endif
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
            " fps_lock=%u"
#endif
            ,
            ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID,
            (unsigned)KAGE_VITA_FULLSPEED_TICK_UNITS,
            (unsigned)KAGE_VITA_FULLSPEED_GAME_TICK_UNITS,
            (unsigned)KAGE_VITA_FULLSPEED_MAX_CATCHUP_TICKS,
            (unsigned)KAGE_VITA_FULLSPEED_STALE_CATCHUP_TICKS,
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
            (unsigned)KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS,
            kage_vita_fullspeed_duty_fps_lock());
#else
            (unsigned)KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS);
#endif
    }
}

void kage_vita_fullspeed_scheduler_note_service(void)
{
    (void)kage_vita_fullspeed_note_once(
        KAGE_VITA_TICK_SERVICE, &s_counters.service_calls);
}

#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
void kage_vita_fullspeed_scheduler_note_video_service(void)
{
    /* Only a completed outer service before Manager dispatch can publish the
     * receipt. Internal CL=1 services must not carry it into another phase. */
    if (s_initialized && s_runtime_enabled &&
            (s_tick_flags & KAGE_VITA_TICK_SERVICE) != 0u &&
            (s_tick_flags & (KAGE_VITA_TICK_MANAGER_DISPATCH |
                             KAGE_VITA_TICK_MANAGER_ENTRY |
                             KAGE_VITA_TICK_RENDER_GATE)) == 0u)
        s_tick_flags |= KAGE_VITA_TICK_VIDEO_SERVICE;
}
#endif

void kage_vita_fullspeed_scheduler_note_manager_dispatch(void)
{
    if (kage_vita_fullspeed_note_once(
            KAGE_VITA_TICK_MANAGER_DISPATCH,
            &s_counters.manager_dispatch_calls) &&
            (s_tick_flags & KAGE_VITA_TICK_SERVICE) == 0u)
        kage_vita_fullspeed_sequence_failure();
}

void kage_vita_fullspeed_scheduler_note_manager_entry(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame)
{
    if (!kage_vita_fullspeed_note_once(
            KAGE_VITA_TICK_MANAGER_ENTRY, &s_counters.manager_entries))
        return;
    if ((s_tick_flags & KAGE_VITA_TICK_MANAGER_DISPATCH) == 0u ||
            manager_pointer == 0u) {
        kage_vita_fullspeed_sequence_failure();
        return;
    }
    s_manager_pointer = manager_pointer;
    s_manager_counter_before = manager_counter;
    s_interpolation_enabled = interpolation_enabled != 0u;
    s_game_pointer = game_pointer;
    s_game_frame_before = game_frame;
    s_game_frame_after = game_frame;
#if defined(KAGE_VITA_HEAD_ADVANCE)
    if (s_head_expected_manager &&
            (manager_pointer != s_head_expected_manager ||
             game_pointer != s_head_expected_game ||
             manager_counter != s_head_expected_counter ||
             game_frame != s_head_expected_frame)) {
        /* The service may already have run early once during a transition.
         * Never promote a prediction miss into a new cadence violation. The
         * existing rebase/publication/sequence checks remain authoritative. */
        kage_vita_fullspeed_clear_head_advance();
        s_pre_game_age = 0u;
    }
#endif
}

void kage_vita_fullspeed_scheduler_note_manager_counter_rebase(
    uint32_t manager_pointer, uint32_t old_manager_counter,
    uint32_t new_manager_counter)
{
    kage_vita_fullspeed_add_u32(
        &s_counters.manager_counter_rebase_events, 1u);
    if (!kage_vita_fullspeed_note_flag_once(
            KAGE_VITA_TICK_MANAGER_COUNTER_REBASE)) {
        kage_vita_fullspeed_add_u32(
            &s_counters.manager_counter_rebase_violations, 1u);
        return;
    }
    if (!s_runtime_enabled)
        return;

    /* Continue emits this token immediately before its one pinned
     * Manager+0x4a264 store.  The store only normalizes an odd menu phase to
     * the following even counter so the same Manager invocation can perform
     * a full Game update.  Rebase the observation origin, never guest state. */
    if ((s_tick_flags & KAGE_VITA_TICK_MANAGER_ENTRY) == 0u ||
            (s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u ||
            (s_tick_flags & KAGE_VITA_TICK_RENDER_GATE) != 0u ||
            manager_pointer == 0u ||
            manager_pointer != s_manager_pointer ||
            old_manager_counter != s_manager_counter_before ||
            (old_manager_counter & 1u) == 0u ||
            new_manager_counter != old_manager_counter + 1u ||
            (new_manager_counter & 1u) != 0u) {
        kage_vita_fullspeed_manager_counter_rebase_failure();
        return;
    }

    s_manager_counter_before = new_manager_counter;
#if defined(KAGE_VITA_HEAD_ADVANCE)
    kage_vita_fullspeed_clear_head_advance();
    s_pre_game_age = 0u;
#endif
    kage_vita_fullspeed_add_u32(
        &s_counters.manager_counter_rebases, 1u);
}

void kage_vita_fullspeed_scheduler_note_game_pointer_publish(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t old_game_pointer, uint32_t new_game_pointer)
{
    kage_vita_fullspeed_add_u32(
        &s_counters.game_pointer_publish_events, 1u);
    if (!kage_vita_fullspeed_note_flag_once(
            KAGE_VITA_TICK_GAME_POINTER_PUBLISH)) {
        kage_vita_fullspeed_add_u32(
            &s_counters.game_pointer_violations, 1u);
        return;
    }
    if (!s_runtime_enabled)
        return;

    /* This token is emitted immediately before J835's only Manager-owned
     * Game-global publication: the pinned null check, allocation and
     * Game constructor have completed, but the original store has not.  Both
     * the Manager-entry snapshot and the observed global must therefore still
     * be null.  No unobserved pointer change receives this token. */
    if ((s_tick_flags & KAGE_VITA_TICK_MANAGER_ENTRY) == 0u ||
            (s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u ||
            (s_tick_flags & KAGE_VITA_TICK_RENDER_GATE) != 0u ||
            manager_pointer == 0u ||
            manager_pointer != s_manager_pointer ||
            manager_counter != s_manager_counter_before ||
            (manager_counter & 1u) != 0u ||
            s_game_pointer != 0u || old_game_pointer != 0u ||
            new_game_pointer == 0u) {
        kage_vita_fullspeed_game_pointer_failure();
        return;
    }

    s_game_pointer = new_game_pointer;
#if defined(KAGE_VITA_HEAD_ADVANCE)
    kage_vita_fullspeed_clear_head_advance();
    s_pre_game_age = 0u;
#endif
    kage_vita_fullspeed_add_u32(&s_counters.game_pointer_rebinds, 1u);
}

void kage_vita_fullspeed_scheduler_note_game_update_begin(
    uint32_t manager_counter, uint32_t game_pointer, uint32_t game_frame)
{
    if (!kage_vita_fullspeed_note_once(
            KAGE_VITA_TICK_GAME_BEGIN, &s_counters.game_updates))
        return;
    /* The ordinary completed path reaches the sole final RET.  Only one of
     * eight machine-pinned early RET hooks can replace this token. */
    s_game_update_return_site =
        KAGE_VITA_FULLSPEED_GAME_UPDATE_NORMAL_RETURN_RVA;
    if (!s_runtime_enabled)
        return;
    if ((s_tick_flags & KAGE_VITA_TICK_MANAGER_ENTRY) == 0u ||
            manager_counter != s_manager_counter_before) {
        kage_vita_fullspeed_sequence_failure();
        return;
    }
    if (game_pointer == 0u || game_pointer != s_game_pointer) {
        kage_vita_fullspeed_game_pointer_failure();
        return;
    }
    if ((manager_counter & 1u) != 0u) {
        kage_vita_fullspeed_add_u32(&s_counters.parity_violations, 1u);
        kage_vita_fullspeed_disable();
    }
    kage_vita_fullspeed_pace_game_update();
    s_game_frame_before = game_frame;
}

void kage_vita_fullspeed_scheduler_note_game_update_early_return(
    uint32_t return_site)
{
    /* Generated code calls this immediately before one exact pre-increment
     * RET.  The end observation authenticates the three hardware-observed
     * zero-frame transitions below; every other return keeps the ordinary
     * invariant. */
    s_game_update_return_site = return_site;
}

void kage_vita_fullspeed_scheduler_note_game_update_end(
    uint32_t game_pointer, uint32_t game_frame)
{
#if defined(KAGE_VITA_HEAD_ADVANCE)
    uint32_t return_clock_ok = 0u;
#endif
    if (!kage_vita_fullspeed_note_flag_once(KAGE_VITA_TICK_GAME_END))
        return;
    {
        /* Wall duration of this Game::Update, for the realign and the
         * interpolated-frame admission tests only. */
        uint64_t end_us = isaac_vita_get_process_time();
        uint64_t elapsed_units;

        if ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u &&
                end_us >= s_update_begin_us &&
                kage_vita_fullspeed_clock_units(
                    end_us - s_update_begin_us, &elapsed_units)) {
            s_last_update_units = elapsed_units;
            s_have_last_update = 1u;
#if defined(KAGE_VITA_HEAD_ADVANCE)
            return_clock_ok = end_us <= UINT64_MAX / 3u &&
                end_us >= s_last_clock_us;
#endif
        }
    }
    s_game_frame_after = game_frame;
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
    /* Pause/Exit zero-frame returns are valid, but must not seed gameplay's
     * next-start prediction.  Pointer/counter identity is checked at use. */
    if (game_frame != s_game_frame_before + 1u)
        s_pre_game_age = 0u;
#endif
    s_counters.game_update_last_return_site = s_game_update_return_site;
    s_counters.game_update_last_frame_before = s_game_frame_before;
    s_counters.game_update_last_frame_after = game_frame;
    if (!s_runtime_enabled)
        return;
    if ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) == 0u) {
        kage_vita_fullspeed_sequence_failure();
        return;
    }
    if (game_pointer == 0u || game_pointer != s_game_pointer) {
        kage_vita_fullspeed_game_pointer_failure();
        return;
    }
    if (game_frame != s_game_frame_before + 1u &&
            !(kage_vita_fullspeed_zero_frame_site(
                  s_game_update_return_site) &&
              game_frame == s_game_frame_before)) {
        s_counters.game_frame_violation_return_site =
            s_game_update_return_site;
        s_counters.game_frame_violation_before = s_game_frame_before;
        s_counters.game_frame_violation_after = game_frame;
        kage_vita_fullspeed_add_u32(
            &s_counters.game_frame_violations, 1u);
        kage_vita_fullspeed_disable();
    }
#if defined(KAGE_VITA_HEAD_ADVANCE)
    if (s_head_realign_credit_units) {
        if (!return_clock_ok) {
            kage_vita_fullspeed_add_u32(&s_counters.clock_resets, 1u);
            kage_vita_fullspeed_disable();
        } else if (game_frame != s_game_frame_before + 1u) {
            /* This is a valid paused/Exit return (invalid returns already
             * disabled above). Restore exactly the old forward realignment;
             * the early head must not grant credit to future paused ticks. */
            if (s_head_realign_credit_units <= s_head_realign_restore_units &&
                    s_next_due_units == s_head_realign_restore_units -
                        s_head_realign_credit_units)
                s_next_due_units = s_head_realign_restore_units;
            else
                kage_vita_fullspeed_sequence_failure();
        }
    }
    kage_vita_fullspeed_clear_head_advance();
#endif
}

int kage_vita_fullspeed_scheduler_plan_render(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled, uint32_t game_pointer,
    uint32_t game_frame)
{
    uint64_t now_us;
    uint64_t now_units;
    uint32_t full_phase;
    uint32_t another_tick_due;
    uint32_t continue_without_game;
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    uint32_t video_priority;
#endif

#define RENDER_FAILURE(reason) kage_vita_fullspeed_render_failure( \
    reason, manager_pointer, manager_counter, interpolation_enabled, \
    game_pointer, game_frame)

    if (!kage_vita_fullspeed_note_flag_once(KAGE_VITA_TICK_RENDER_GATE))
        return 1;
    if (!s_runtime_enabled)
        return 1;
    if ((s_tick_flags & KAGE_VITA_TICK_MANAGER_ENTRY) == 0u ||
            manager_pointer == 0u || manager_pointer != s_manager_pointer) {
        RENDER_FAILURE("manager-identity");
        kage_vita_fullspeed_sequence_failure();
        return 1;
    }
    /* A full Game::Update legitimately changes _enableInterpolation while
     * the frontend hands control to a newly-created game.  The Render gate
     * runs after that Update, so its value is authoritative only when the
     * exact paired Game begin/end observations prove that full update ran.
     * A change on the non-full phase remains an invariant violation. */
    if (interpolation_enabled != s_interpolation_enabled) {
        if ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) == 0u ||
                (s_tick_flags & KAGE_VITA_TICK_GAME_END) == 0u) {
            RENDER_FAILURE("interpolation");
            kage_vita_fullspeed_sequence_failure();
            return 1;
        }
        s_interpolation_enabled = interpolation_enabled != 0u;
    }
    if (((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u) !=
            ((s_tick_flags & KAGE_VITA_TICK_GAME_END) != 0u)) {
        RENDER_FAILURE("game-pair");
        kage_vita_fullspeed_sequence_failure();
        return 1;
    }
    /* J835 Continue normalizes the odd counter at pinned 004b4031. A
     * pending cutscene can then select Manager's cutscene branch instead of
     * 004b0311 (Game::Update). A device capture (tick 5400) records
     * precisely this rebase with neither Game hook. A publication alone is
     * NOT this exception. All identity/counter/interpolation/clock checks
     * still apply; only this tick may adopt the frame restored by Continue.
     * Runtime enabled here means the rebase/publication validators passed. */
    continue_without_game =
        (s_tick_flags & KAGE_VITA_TICK_MANAGER_COUNTER_REBASE) != 0u &&
        (s_tick_flags & (KAGE_VITA_TICK_GAME_BEGIN |
                         KAGE_VITA_TICK_GAME_END)) == 0u &&
        s_game_pointer != 0u;
    if ((s_tick_flags & (KAGE_VITA_TICK_MANAGER_COUNTER_REBASE |
                         KAGE_VITA_TICK_GAME_POINTER_PUBLISH)) != 0u &&
            ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) == 0u ||
             (s_tick_flags & KAGE_VITA_TICK_GAME_END) == 0u) &&
            !continue_without_game) {
        RENDER_FAILURE("continue-no-game");
        kage_vita_fullspeed_sequence_failure();
        return 1;
    }
    if (manager_counter != s_manager_counter_before + 1u) {
        RENDER_FAILURE("counter");
        kage_vita_fullspeed_add_u32(&s_counters.parity_violations, 1u);
        kage_vita_fullspeed_disable();
        return 1;
    }
    full_phase = manager_counter & 1u;
    if (game_pointer != s_game_pointer) {
        RENDER_FAILURE("game-identity");
        kage_vita_fullspeed_game_pointer_failure();
        return 1;
    }
    if (game_frame != s_game_frame_after) {
        /* Manager-owned Continue/load paths can restore Game+0x1a30dc from
         * saved state after the entry snapshot without calling Game::Update.
         * Accept that state rebase on the native non-full phase, or this
         * authenticated Continue-without-Game tick. Every
         * actual Game::Update remains authenticated above and independently
         * paced by the 33.333-ms floor; other full-phase changes stay strict. */
        if ((s_tick_flags & (KAGE_VITA_TICK_GAME_BEGIN |
                             KAGE_VITA_TICK_GAME_END)) == 0u &&
                (full_phase == 0u || continue_without_game)) {
            s_game_frame_before = game_frame;
            s_game_frame_after = game_frame;
        } else {
            RENDER_FAILURE("game-frame");
            kage_vita_fullspeed_add_u32(
                &s_counters.game_frame_violations, 1u);
            kage_vita_fullspeed_disable();
            return 1;
        }
    }

    now_us = isaac_vita_get_process_time();
    if (now_us < s_last_clock_us ||
            !kage_vita_fullspeed_clock_units(now_us, &now_units)) {
        RENDER_FAILURE("clock");
        kage_vita_fullspeed_add_u32(&s_counters.clock_resets, 1u);
        kage_vita_fullspeed_disable();
        return 1;
    }
    s_last_clock_us = now_us;
    another_tick_due = now_units >= s_next_due_units;
    if (continue_without_game)
        s_tick_flags |= KAGE_VITA_TICK_CONTINUE_WITHOUT_GAME;

#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    /* owner-continuefix-first-arm.log: 297 paid RGB services lost their Render
     * admission, then fetch discarded 193 ready frames. During a cutscene
     * there is no Game update to protect by skipping that presentation.
     * Require no Game call in EITHER phase of a gameplay pair: a retained
     * clip cannot grant the following interpolation tick extra GPU time.
     * All identity/pair/frame/clock checks above remain authoritative. */
    video_priority = (s_tick_flags & KAGE_VITA_TICK_VIDEO_SERVICE) != 0u &&
        (s_tick_flags & (KAGE_VITA_TICK_GAME_BEGIN | KAGE_VITA_TICK_GAME_END)) == 0u &&
        !s_previous_tick_had_game;
    if (video_priority)
        another_tick_due = 0u; /* admission only; deadlines/Game floor untouched */
#endif

    if (full_phase) {
        kage_vita_fullspeed_add_u32(&s_counters.full_phases, 1u);
    } else {
        kage_vita_fullspeed_add_u32(&s_counters.nonfull_phases, 1u);
        if (interpolation_enabled)
            kage_vita_fullspeed_add_u32(
                &s_counters.interpolation_phases, 1u);
        if ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u) {
            RENDER_FAILURE("nonfull-game");
            kage_vita_fullspeed_add_u32(
                &s_counters.parity_violations, 1u);
            kage_vita_fullspeed_disable();
            return 1;
        }
#undef RENDER_FAILURE
        /* Preserve the game's original even-phase Render when interpolation
         * is enabled and the following wrapper is not due yet.  Once a whole
         * tick is waiting, discard the interpolated frame first so the fixed
         * 30-Hz Game update cadence remains authoritative. */
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
        /* port.ini fps_lock: the duty term reuses this exact skip statement
         * (same counters, same return); see kage_vita_fullspeed_duty_admits. */
        if (!interpolation_enabled || another_tick_due ||
                !kage_vita_fullspeed_duty_admits(s_counters.nonfull_phases)) {
#else
        if (!interpolation_enabled || another_tick_due) {
#endif
            s_render_decision = 0u;
            kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);
            kage_vita_fullspeed_add_u32(&s_counters.nonfull_skips, 1u);
            return 0;
        }
#if defined(ISAAC_VITA_GL_THREAD_RENDER_ADMISSION) && ISAAC_VITA_GL_THREAD_RENDER_ADMISSION
        /* GL render thread one frame behind: the same interpolated-frame skip
         * (same counters, same return), so Present never waits for it and the
         * next Game::Update keeps its cadence.  Full phases always render. */
        if (!kage_vita_backend_render_admit()) {
            s_render_decision = 0u;
            kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);
            kage_vita_fullspeed_add_u32(&s_counters.nonfull_skips, 1u);
            return 0;
        }
#endif
        uint64_t next_start_reserve_units = s_last_update_units;
#if defined(ISAAC_VITA_FULLSPEED_SERVICE_RESERVE)
        /* The floor is the next Game::Update START, not its completion.
         * Reserve the work before that start, not the update body after it.
         * Use only the preceding advancing full tick of this same Manager /
         * Game epoch.  Unknown, stale or oversized samples keep stock policy.
         * Like last Render, this is a prediction, not a bound on future work. */
        if (s_pre_game_age == 2u &&
                s_pre_game_manager == manager_pointer &&
                s_pre_game_game == game_pointer &&
                s_manager_counter_before == s_pre_game_counter + 1u)
            next_start_reserve_units = s_pre_game_units;
#endif
        /* Admit the interpolated frame only if, at the measured cost of the
         * last Render, it finishes before the next Game::Update floor with
         * the selected next-start reserve left over (stock uses the update
         * body; the opt-in above uses pre-Game service).  Otherwise the frame
         * is paid for by delaying
         * the simulation: Bundle32 rooms with a 20-ms render presented 46 per
         * 120 ticks at 27 UPS, alternating a full render, an interpolated
         * render that ran past the floor, a late update and a skipped full
         * frame.  Skipping the interpolated frame there yields 60 presents
         * at exactly 30 UPS.
         *
         * The test only makes sense while a floor is live, i.e. still in the
         * future.  In menus no Game::Update runs, the last floor from gameplay
         * is stale and in the past, and without this guard every non-full
         * menu frame was refused: Bundle33 measured the post-Exit menu at
         * 30 FPS with 60 non-full skips per window against 43 FPS before. */
#if defined(KAGE_VITA_ADMIT_SLACK_UNITS)
        /* S5 admit slack.  slack_band is
         * 1 exactly when the time test below is live and the frame's
         * predicted end lies in (floor, floor + slack]: the stock test
         * refuses it, ON admits it (its overrun then becomes lateness that
         * the floor debt keeps), OBSERVE refuses it and counts it.  The
         * operands are the time test's own, read after the duty skip, the
         * GL admission and the reserve above. */
# if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
#  define KAGE_VITA_ADMIT_COST_UNITS s_render_admission_units
# else
#  define KAGE_VITA_ADMIT_COST_UNITS s_last_render_units
# endif
        uint64_t admit_deadline_units =
            s_next_game_due_units <= UINT64_MAX - KAGE_VITA_ADMIT_SLACK_UNITS ?
                s_next_game_due_units + KAGE_VITA_ADMIT_SLACK_UNITS : UINT64_MAX;
        uint32_t slack_band =
            s_have_game_due && s_have_last_render && s_have_last_update &&
            s_next_game_due_units > now_units &&
            KAGE_VITA_ADMIT_COST_UNITS <= UINT64_MAX - next_start_reserve_units &&
            now_units <= UINT64_MAX -
                (KAGE_VITA_ADMIT_COST_UNITS + next_start_reserve_units) &&
            now_units + KAGE_VITA_ADMIT_COST_UNITS + next_start_reserve_units >
                s_next_game_due_units &&
            now_units + KAGE_VITA_ADMIT_COST_UNITS + next_start_reserve_units <=
                admit_deadline_units;
# undef KAGE_VITA_ADMIT_COST_UNITS
        if (slack_band)
            kage_vita_fullspeed_add_u32(&s_floor_debt.slack_band, 1u);
# if KAGE_VITA_FLOOR_DEBT_MODE == 2
        s_slack_admit_pending = slack_band;
# endif
#endif
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
        /* Same test on the body plus recurring Present wait (see the
         * declaration): an isolated wait no longer refuses the frame. */
        if (s_have_game_due && s_have_last_render && s_have_last_update &&
                s_next_game_due_units > now_units &&
                s_render_admission_units <=
                    UINT64_MAX - next_start_reserve_units &&
                now_units <= UINT64_MAX -
                    (s_render_admission_units + next_start_reserve_units) &&
                now_units + s_render_admission_units +
                    next_start_reserve_units > KAGE_VITA_ADMIT_DEADLINE_UNITS) {
#else
        if (s_have_game_due && s_have_last_render && s_have_last_update &&
                s_next_game_due_units > now_units &&
                s_last_render_units <= UINT64_MAX - next_start_reserve_units &&
                now_units <= UINT64_MAX -
                    (s_last_render_units + next_start_reserve_units) &&
                now_units + s_last_render_units + next_start_reserve_units >
                    KAGE_VITA_ADMIT_DEADLINE_UNITS) {
#endif
            s_render_decision = 0u;
            kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);
            kage_vita_fullspeed_add_u32(&s_counters.nonfull_skips, 1u);
            return 0;
        }
        s_consecutive_full_render_skips = 0u;
        s_render_decision = 1u;
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
        if (video_priority && now_units >= s_next_due_units)
            kage_vita_fullspeed_video_priority_receipt(0u);
#endif
        return 1;
    }

    /* Skipping a full-phase Render only pays when the loop can use the saved
     * time to get back on the grid and render more often later.  When one
     * Game::Update plus one Render no longer fit inside a floor period the
     * loop is render-bound: every skip merely discards a frame the player
     * would have seen, the next update is still only as early as the floor,
     * and four skips in five (Bundle32 model at a 34-ms render: 14 presents
     * per 120 ticks) turn a 28-FPS room into a 7-FPS one for a 6 % gain in
     * simulation rate.  Present every full phase there and let the
     * simulation run at the rate the renderer allows; the floor still bounds
     * it at 30 Hz from above.  This holds up to one and a half floor periods
     * (34 ms -> 28 FPS at 28 UPS, 45 ms -> 21 at 21).  Beyond that a frame
     * costs so much that presenting every one would drag the simulation under
     * 20 Hz (55 ms -> 17 Hz), which the cadence contract forbids, so the
     * original catch-up skip logic applies there unchanged.  Below one floor
     * period it is likewise unchanged. */
    if (another_tick_due &&
            s_consecutive_full_render_skips <
                KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS &&
            !(s_have_last_update && s_have_last_render &&
              s_last_update_units <= UINT64_MAX - s_last_render_units &&
              s_last_update_units + s_last_render_units >=
                  KAGE_VITA_FULLSPEED_GAME_TICK_UNITS &&
              s_last_update_units + s_last_render_units <
                  KAGE_VITA_FULLSPEED_GAME_TICK_UNITS +
                  KAGE_VITA_FULLSPEED_GAME_TICK_UNITS / 2u)) {
        ++s_consecutive_full_render_skips;
        s_render_decision = 0u;
        kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);
        kage_vita_fullspeed_add_u32(&s_counters.late_full_skips, 1u);
        return 0;
    }
    if (another_tick_due)
        kage_vita_fullspeed_add_u32(&s_counters.forced_renders, 1u);
    s_consecutive_full_render_skips = 0u;
    s_render_decision = 1u;
#if defined(ISAAC_VITA_THEORA_RENDER_PRIORITY) && ISAAC_VITA_THEORA_RENDER_PRIORITY
    if (video_priority && now_units >= s_next_due_units) {
        kage_vita_fullspeed_add_u32(&s_counters.forced_renders, 1u);
        kage_vita_fullspeed_video_priority_receipt(1u);
    }
#endif
    return 1;
}

void kage_vita_fullspeed_scheduler_note_render_entry(void)
{
    if (kage_vita_fullspeed_note_once(
            KAGE_VITA_TICK_RENDER_ENTRY, &s_counters.render_calls) &&
            (!s_render_decision ||
             (s_tick_flags & KAGE_VITA_TICK_RENDER_GATE) == 0u))
        kage_vita_fullspeed_sequence_failure();
    s_render_entry_us = isaac_vita_get_process_time();
}

void kage_vita_fullspeed_scheduler_note_render_body(
    uint32_t manager_pointer, uint32_t manager_counter,
    uint32_t interpolation_enabled)
{
    if (!kage_vita_fullspeed_note_once(
            KAGE_VITA_TICK_RENDER_BODY, &s_counters.render_bodies))
        return;
    if (!s_runtime_enabled)
        return;
    if ((s_tick_flags & KAGE_VITA_TICK_RENDER_ENTRY) == 0u ||
            manager_pointer != s_manager_pointer ||
            manager_counter != s_manager_counter_before + 1u ||
            ((manager_counter & 1u) == 0u && !interpolation_enabled) ||
            interpolation_enabled != s_interpolation_enabled) {
        kage_vita_fullspeed_add_u32(&s_counters.parity_violations, 1u);
        kage_vita_fullspeed_disable();
    }
}

void kage_vita_fullspeed_scheduler_note_render_return(void)
{
    uint64_t now_us;
    uint64_t elapsed_units;

    if (kage_vita_fullspeed_note_flag_once(
            KAGE_VITA_TICK_RENDER_RETURN) &&
            (s_tick_flags & KAGE_VITA_TICK_RENDER_ENTRY) == 0u)
        kage_vita_fullspeed_sequence_failure();
    /* Wall duration of the most recent Render body (inclusive of Present),
     * consulted only by the floor realignment below: a realignment is
     * justified only when the idle time it reclaims could have held a
     * frame. */
    now_us = isaac_vita_get_process_time();
    if ((s_tick_flags & KAGE_VITA_TICK_RENDER_ENTRY) != 0u &&
            now_us >= s_render_entry_us &&
            kage_vita_fullspeed_clock_units(
                now_us - s_render_entry_us, &elapsed_units)) {
        s_last_render_units = elapsed_units;
        s_have_last_render = 1u;
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
        /* Present is nested inside Render.  Its seam is the first statement
         * of kage_pc_backend_present, a few calls before the profiler's swp
         * bracket around vglSwapBuffers; "wait" is everything from that
         * seam to this return, so body + wait == elapsed exactly.  The
         * first sample, a tick without Present, or an inconsistent clock
         * admits on the inclusive duration, as without this knob. */
        {
            uint64_t body_units;
            uint64_t wait_units;

            if ((s_tick_flags & KAGE_VITA_TICK_PRESENT) != 0u &&
                    s_present_entry_us >= s_render_entry_us &&
                    s_present_entry_us <= now_us &&
                    kage_vita_fullspeed_clock_units(
                        s_present_entry_us - s_render_entry_us,
                        &body_units)) {
                wait_units = elapsed_units - body_units;
                s_render_admission_units = body_units +
                    (s_have_last_present_wait &&
                     s_last_present_wait_units < wait_units ?
                        s_last_present_wait_units : wait_units);
                s_last_present_wait_units = wait_units;
                s_have_last_present_wait = 1u;
            } else {
                s_render_admission_units = elapsed_units;
                s_have_last_present_wait = 0u;
            }
        }
#endif
    }
}

void kage_vita_fullspeed_scheduler_note_present(void)
{
#if defined(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT)
    /* The one added clock read per Present; consumed at Render return. */
    s_present_entry_us = isaac_vita_get_process_time();
#endif
    if (kage_vita_fullspeed_note_once(
            KAGE_VITA_TICK_PRESENT, &s_counters.presents) &&
            (s_tick_flags & KAGE_VITA_TICK_RENDER_BODY) == 0u)
        kage_vita_fullspeed_sequence_failure();
}

int kage_vita_fullspeed_scheduler_bypass_limiter(void)
{
    /* Absolute pacing happens at the next loop head.  Retaining the original
     * per-iteration 16.7-ms limiter here would turn a 27-ms full phase plus a
     * cheap parity phase into a 44-ms pair and recreate slow motion. */
    return s_initialized && s_runtime_enabled;
}

void kage_vita_fullspeed_scheduler_snapshot(
    KageVitaFullspeedSnapshot *snapshot)
{
    if (snapshot)
        *snapshot = s_counters;
}

#if defined(KAGE_VITA_FLOOR_DEBT_MODE)
void kage_vita_fullspeed_scheduler_floor_debt_take(
    KageVitaFullspeedFloorDebtSnapshot *snapshot)
{
    if (!snapshot)
        return;
    *snapshot = s_floor_debt;
    /* The window identity stamp (header): the floor the next pace waits
     * for, read on the game thread like every other receipt field. */
    snapshot->floor_us = s_runtime_enabled && s_have_game_due ?
        s_next_game_due_units / 3u : 0u;
    memset(&s_floor_debt, 0, sizeof s_floor_debt);
}
#endif

#if defined(ISAAC_VITA_DEBUG_OVERLAY)
void kage_vita_fullspeed_scheduler_debug_overlay_timings(
    uint32_t *update_us, uint32_t *render_us)
{
    if (update_us)
        *update_us = s_have_last_update
            ? kage_vita_fullspeed_units_to_us(s_last_update_units) : 0u;
    if (render_us)
        *render_us = s_have_last_render
            ? kage_vita_fullspeed_units_to_us(s_last_render_units) : 0u;
}
#endif
