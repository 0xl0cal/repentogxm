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

#ifndef ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID
# define ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID "cadence30:unstamped"
#endif

void isaac_vita_log(const char *format, ...);
extern int sceKernelDelayThread(unsigned int delay_us);

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
    KAGE_VITA_TICK_MANAGER_COUNTER_REBASE = 1u << 11
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
static uint64_t s_render_entry_us;
static uint64_t s_last_render_units;
static uint32_t s_have_last_render;
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
             (s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) == 0u))
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
        if (wait_result == 2)
            s_have_game_due = 0u;
        else if (wait_result == 1) {
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
        s_next_due_units += floor_wait_units;
    s_last_clock_us = now_us;
}

static void kage_vita_fullspeed_scheduler_clear(uint32_t runtime_enabled)
{
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
    s_render_entry_us = 0u;
    s_last_render_units = 0u;
    s_have_last_render = 0u;
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
    uint64_t now_us = isaac_vita_get_process_time();
    uint64_t now_units;
    uint64_t due_ticks = 1u;
    uint64_t dropped = 0u;
    uint64_t debt_units = 0u;
    int wait_result;

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
    }

    if (s_runtime_enabled && now_units < s_next_due_units) {
        wait_result = kage_vita_fullspeed_wait_until(
            s_next_due_units, &now_us, &now_units);
        if (wait_result == 2) {
            s_next_due_units = now_units;
            s_have_last_due = 0u;
        }
    }

    if (s_runtime_enabled) {
        if (now_units < s_next_due_units) {
            kage_vita_fullspeed_disable();
        } else {
            due_ticks =
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
            }
            s_current_due_units = s_next_due_units;
            if (s_next_due_units >
                    UINT64_MAX - KAGE_VITA_FULLSPEED_TICK_UNITS) {
                kage_vita_fullspeed_disable();
            } else {
                s_next_due_units += KAGE_VITA_FULLSPEED_TICK_UNITS;
                debt_units = now_units - s_current_due_units;
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
            "parity=4a264 frame=1a30dc",
            ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID,
            (unsigned)KAGE_VITA_FULLSPEED_TICK_UNITS,
            (unsigned)KAGE_VITA_FULLSPEED_GAME_TICK_UNITS,
            (unsigned)KAGE_VITA_FULLSPEED_MAX_CATCHUP_TICKS,
            (unsigned)KAGE_VITA_FULLSPEED_STALE_CATCHUP_TICKS,
            (unsigned)KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS);
    }
}

void kage_vita_fullspeed_scheduler_note_service(void)
{
    (void)kage_vita_fullspeed_note_once(
        KAGE_VITA_TICK_SERVICE, &s_counters.service_calls);
}

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
        }
    }
    s_game_frame_after = game_frame;
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

    if (!kage_vita_fullspeed_note_flag_once(KAGE_VITA_TICK_RENDER_GATE))
        return 1;
    if (!s_runtime_enabled)
        return 1;
    if ((s_tick_flags & KAGE_VITA_TICK_MANAGER_ENTRY) == 0u ||
            manager_pointer == 0u || manager_pointer != s_manager_pointer) {
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
            kage_vita_fullspeed_sequence_failure();
            return 1;
        }
        s_interpolation_enabled = interpolation_enabled != 0u;
    }
    if (((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u) !=
            ((s_tick_flags & KAGE_VITA_TICK_GAME_END) != 0u)) {
        kage_vita_fullspeed_sequence_failure();
        return 1;
    }
    if ((s_tick_flags & (KAGE_VITA_TICK_MANAGER_COUNTER_REBASE |
                         KAGE_VITA_TICK_GAME_POINTER_PUBLISH)) != 0u &&
            ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) == 0u ||
             (s_tick_flags & KAGE_VITA_TICK_GAME_END) == 0u)) {
        kage_vita_fullspeed_sequence_failure();
        return 1;
    }
    if (manager_counter != s_manager_counter_before + 1u) {
        kage_vita_fullspeed_add_u32(&s_counters.parity_violations, 1u);
        kage_vita_fullspeed_disable();
        return 1;
    }
    full_phase = manager_counter & 1u;
    if (game_pointer != s_game_pointer) {
        kage_vita_fullspeed_game_pointer_failure();
        return 1;
    }
    if (game_frame != s_game_frame_after) {
        /* Manager-owned Continue/load paths can restore Game+0x1a30dc from
         * saved state after the entry snapshot without calling Game::Update.
         * Accept that state rebase only on the native non-full phase.  Every
         * actual Game::Update remains authenticated above and independently
         * paced by the 33.333-ms floor; full-phase frame changes stay strict. */
        if ((s_tick_flags & (KAGE_VITA_TICK_GAME_BEGIN |
                             KAGE_VITA_TICK_GAME_END)) == 0u &&
                full_phase == 0u) {
            s_game_frame_before = game_frame;
            s_game_frame_after = game_frame;
        } else {
            kage_vita_fullspeed_add_u32(
                &s_counters.game_frame_violations, 1u);
            kage_vita_fullspeed_disable();
            return 1;
        }
    }

    now_us = isaac_vita_get_process_time();
    if (now_us < s_last_clock_us ||
            !kage_vita_fullspeed_clock_units(now_us, &now_units)) {
        kage_vita_fullspeed_add_u32(&s_counters.clock_resets, 1u);
        kage_vita_fullspeed_disable();
        return 1;
    }
    s_last_clock_us = now_us;
    another_tick_due = now_units >= s_next_due_units;

    if (full_phase) {
        kage_vita_fullspeed_add_u32(&s_counters.full_phases, 1u);
    } else {
        kage_vita_fullspeed_add_u32(&s_counters.nonfull_phases, 1u);
        if (interpolation_enabled)
            kage_vita_fullspeed_add_u32(
                &s_counters.interpolation_phases, 1u);
        if ((s_tick_flags & KAGE_VITA_TICK_GAME_BEGIN) != 0u) {
            kage_vita_fullspeed_add_u32(
                &s_counters.parity_violations, 1u);
            kage_vita_fullspeed_disable();
            return 1;
        }
        /* Preserve the game's original even-phase Render when interpolation
         * is enabled and the following wrapper is not due yet.  Once a whole
         * tick is waiting, discard the interpolated frame first so the fixed
         * 30-Hz Game update cadence remains authoritative. */
        if (!interpolation_enabled || another_tick_due) {
            s_render_decision = 0u;
            kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);
            kage_vita_fullspeed_add_u32(&s_counters.nonfull_skips, 1u);
            return 0;
        }
        /* Admit the interpolated frame only if, at the measured cost of the
         * last Render, it finishes before the next Game::Update floor with
         * room for that update.  Otherwise the frame is paid for by delaying
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
        if (s_have_game_due && s_have_last_render && s_have_last_update &&
                s_next_game_due_units > now_units &&
                s_last_render_units <= UINT64_MAX - s_last_update_units &&
                now_units <= UINT64_MAX -
                    (s_last_render_units + s_last_update_units) &&
                now_units + s_last_render_units + s_last_update_units >
                    s_next_game_due_units) {
            s_render_decision = 0u;
            kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);
            kage_vita_fullspeed_add_u32(&s_counters.nonfull_skips, 1u);
            return 0;
        }
        s_consecutive_full_render_skips = 0u;
        s_render_decision = 1u;
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
    }
}

void kage_vita_fullspeed_scheduler_note_present(void)
{
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
