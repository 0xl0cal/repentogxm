/* Stable 30-Hz presentation policy for the physical Vita port.
 *
 * Repentance advances Game::Update on only one Manager-counter parity.  The
 * other parity optionally performs a second, interpolated Render/Present.
 * Rendering both different-cost parities makes one 30-Hz simulation cadence
 * appear as the observed paired 20<->40 and 30<->60 presentation modes when
 * render cost crosses the original per-wrapper limiter's thresholds.
 *
 * This policy bypasses only the guest interpolation flag without changing
 * Manager memory.  On the ordinary cadence it renders and presents only the
 * completed Game-update phase, then paces the cheap no-op phase to an exact
 * 1/30 start-to-start floor.  Native 4a26c/4a26d transition guards remain in
 * control and their Presents stay unpaced.  No guest counter, Game frame or
 * framebuffer is synthesized, and no second render is run.
 * The shared Vita Present wrapper also floors each later full-phase Present
 * from the actual preceding completed Present, so a late frame is never
 * followed by a short compensating interval.
 */
#include "kage_vita_stable30.h"
#include "vita_host_services.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#ifndef ISAAC_VITA_STABLE30_BUILD_ID
# define ISAAC_VITA_STABLE30_BUILD_ID "stable30:unstamped"
#endif

void isaac_vita_log(const char *format, ...);
extern int sceKernelDelayThread(unsigned int delay_us);

enum KageVitaStable30Phase {
    KAGE_VITA_STABLE30_PHASE_NONE = 0,
    KAGE_VITA_STABLE30_PHASE_FULL = 1,
    KAGE_VITA_STABLE30_PHASE_NONFULL = 2
};

static uint64_t s_full_deadline_units;
static uint64_t s_present_deadline_units;
static KageVitaStable30Snapshot s_counters;
static uint32_t s_enabled;
static uint32_t s_have_full_deadline;
static uint32_t s_have_present_deadline;
static uint32_t s_logged;
static uint32_t s_pacing_enabled;
static uint32_t s_phase;
static uint32_t s_render_present_armed;

static void kage_vita_stable30_disable_pacing(void)
{
    s_have_full_deadline = 0u;
    s_have_present_deadline = 0u;
    s_pacing_enabled = 0u;
}

static void kage_vita_stable30_add_u32(
    uint32_t *value, uint64_t increment)
{
    uint64_t sum = (uint64_t)*value + increment;
    *value = sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

static int kage_vita_stable30_clock_units(
    uint64_t microseconds, uint64_t *units)
{
    if (!units || microseconds > UINT64_MAX / 3u)
        return 0;
    *units = microseconds * 3u;
    return 1;
}

static int kage_vita_stable30_wait_until(uint64_t deadline_units)
{
    uint32_t no_progress = 0u;

    for (;;) {
        uint64_t before = isaac_vita_get_process_time();
        uint64_t before_units;
        uint64_t remaining_units;
        uint64_t requested_us;
        uint64_t after;
        int result;

        if (!kage_vita_stable30_clock_units(before, &before_units)) {
            kage_vita_stable30_add_u32(&s_counters.clock_resets, 1u);
            kage_vita_stable30_disable_pacing();
            return 0;
        }
        if (before_units >= deadline_units)
            return 1;

        remaining_units = deadline_units - before_units;
        requested_us = (remaining_units + 2u) / 3u;
        if (requested_us > UINT_MAX)
            requested_us = UINT_MAX;
        if (requested_us == 0u)
            requested_us = 1u;

        kage_vita_stable30_add_u32(&s_counters.wait_calls, 1u);
        result = sceKernelDelayThread((unsigned int)requested_us);
        after = isaac_vita_get_process_time();
        if (after >= before)
            kage_vita_stable30_add_u32(
                &s_counters.waited_us, after - before);
        if (result < 0) {
            kage_vita_stable30_add_u32(&s_counters.delay_failures, 1u);
            kage_vita_stable30_disable_pacing();
            return 0;
        }
        if (after < before) {
            kage_vita_stable30_add_u32(&s_counters.clock_resets, 1u);
            kage_vita_stable30_disable_pacing();
            return 0;
        }
        if (after == before) {
            if (++no_progress >= 8u) {
                kage_vita_stable30_add_u32(
                    &s_counters.delay_failures, 1u);
                kage_vita_stable30_disable_pacing();
                return 0;
            }
        } else {
            no_progress = 0u;
        }
    }
}

static void kage_vita_stable30_clear(uint32_t enabled)
{
    s_full_deadline_units = 0u;
    s_present_deadline_units = 0u;
    memset(&s_counters, 0, sizeof s_counters);
    s_enabled = enabled;
    s_have_full_deadline = 0u;
    s_have_present_deadline = 0u;
    s_logged = 0u;
    s_pacing_enabled = enabled;
    s_phase = KAGE_VITA_STABLE30_PHASE_NONE;
    s_render_present_armed = 0u;
}

void kage_vita_stable30_reset(void)
{
    kage_vita_stable30_clear(1u);
}

void kage_vita_stable30_deactivate(void)
{
    kage_vita_stable30_clear(0u);
}

void kage_vita_stable30_note_loop_head(void)
{
    if (!s_enabled)
        return;
    s_phase = KAGE_VITA_STABLE30_PHASE_NONE;
    s_render_present_armed = 0u;
    kage_vita_stable30_add_u32(&s_counters.loop_heads, 1u);
}

void kage_vita_stable30_before_present(void)
{
    if (!s_enabled ||
            !s_render_present_armed ||
            !s_pacing_enabled ||
            !s_have_present_deadline)
        return;
    if (!kage_vita_stable30_wait_until(s_present_deadline_units))
        kage_vita_stable30_disable_pacing();
}

void kage_vita_stable30_after_present(int succeeded)
{
    uint64_t now;
    uint64_t now_units;

    if (!s_enabled || !s_render_present_armed)
        return;
    s_render_present_armed = 0u;
    if (s_phase != KAGE_VITA_STABLE30_PHASE_FULL || !s_pacing_enabled)
        return;
    if (!succeeded) {
        s_have_present_deadline = 0u;
        return;
    }
    now = isaac_vita_get_process_time();
    if (!kage_vita_stable30_clock_units(now, &now_units) ||
            now_units > UINT64_MAX - KAGE_VITA_STABLE30_PERIOD_UNITS) {
        kage_vita_stable30_add_u32(&s_counters.clock_resets, 1u);
        kage_vita_stable30_disable_pacing();
        return;
    }

    /* Rebase from the actual completed Present, never from an old target.
     * A late frame therefore cannot be repaid by a short next interval. */
    s_present_deadline_units =
        now_units + KAGE_VITA_STABLE30_PERIOD_UNITS;
    s_have_present_deadline = 1u;
}

int kage_vita_stable30_manager_entry(uint32_t manager_counter)
{
    int nonfull;

    if (!s_enabled)
        return 0;
    if (!s_logged) {
        s_logged = 1u;
        isaac_vita_log(
            "KAGE VITA STABLE30: bid=%.32s period=100000/3us "
            "manager=004b0027>004b0043 render=004b0614>004b1089 "
            "limiter=0048be3e present=wrapper-pre/post full-only=1",
            ISAAC_VITA_STABLE30_BUILD_ID);
    }

    /* This is the exact signed remainder tested by the frozen instructions
     * at 0x004b0027..0x004b0052.  The counter is non-negative in measured
     * play, but retaining signed semantics avoids inventing a wrap policy. */
    nonfull = (int32_t)manager_counter % 2 == 1;
    if (nonfull) {
        s_phase = KAGE_VITA_STABLE30_PHASE_NONFULL;
        kage_vita_stable30_add_u32(&s_counters.nonfull_phases, 1u);
        return 1;
    }

    s_phase = KAGE_VITA_STABLE30_PHASE_FULL;
    kage_vita_stable30_add_u32(&s_counters.full_phases, 1u);
    {
        uint64_t now_units;
        uint64_t now = isaac_vita_get_process_time();

        if (!s_pacing_enabled) {
            s_have_full_deadline = 0u;
        } else if (!kage_vita_stable30_clock_units(now, &now_units)) {
            kage_vita_stable30_add_u32(&s_counters.clock_resets, 1u);
            kage_vita_stable30_disable_pacing();
        } else if (s_have_full_deadline &&
                now_units < s_full_deadline_units &&
                !kage_vita_stable30_wait_until(s_full_deadline_units)) {
            /* The full render still runs.  Only custom pacing fails closed
             * to the original limiter; parity suppression stays armed. */
            kage_vita_stable30_disable_pacing();
        } else {
            now = isaac_vita_get_process_time();
            if (!kage_vita_stable30_clock_units(now, &now_units) ||
                    now_units >
                        UINT64_MAX - KAGE_VITA_STABLE30_PERIOD_UNITS) {
                kage_vita_stable30_add_u32(
                    &s_counters.clock_resets, 1u);
                kage_vita_stable30_disable_pacing();
            } else {
                /* Rebase on the update that is actually executing.  A long
                 * load drops stale wall time; it cannot cause catch-up
                 * updates.  Waiting on a still-future prior deadline also
                 * preserves the 30-Hz floor across a Manager-counter reset. */
                s_full_deadline_units =
                    now_units + KAGE_VITA_STABLE30_PERIOD_UNITS;
                s_have_full_deadline = 1u;
            }
        }
    }
    return 0;
}

int kage_vita_stable30_plan_render(uint32_t manager_counter)
{
    s_render_present_armed = 0u;
    if (!s_enabled)
        return 0;
    if (s_phase == KAGE_VITA_STABLE30_PHASE_FULL &&
            (manager_counter & 1u) != 0u) {
        s_render_present_armed = 1u;
        return 0;
    }
    if (s_phase == KAGE_VITA_STABLE30_PHASE_NONFULL &&
            (manager_counter & 1u) == 0u) {
        kage_vita_stable30_add_u32(
            &s_counters.interpolation_renders_skipped, 1u);
        return 1;
    }
    if (s_phase == KAGE_VITA_STABLE30_PHASE_NONFULL)
        s_phase = KAGE_VITA_STABLE30_PHASE_NONE;
    return 0;
}

int kage_vita_stable30_finish_tick(void)
{
    if (!s_enabled ||
            s_phase != KAGE_VITA_STABLE30_PHASE_NONFULL ||
            !s_pacing_enabled ||
            !s_have_full_deadline) {
        if (s_enabled)
            kage_vita_stable30_add_u32(
                &s_counters.limiter_fallbacks, 1u);
        return 0;
    }

    if (!kage_vita_stable30_wait_until(s_full_deadline_units)) {
        /* Keep the parity/no-op policy armed, but permanently resume the
         * original limiter until the next explicit session reset. */
        kage_vita_stable30_disable_pacing();
        kage_vita_stable30_add_u32(
            &s_counters.limiter_fallbacks, 1u);
        return 0;
    }
    kage_vita_stable30_add_u32(&s_counters.limiter_bypasses, 1u);
    return 1;
}

void kage_vita_stable30_snapshot(KageVitaStable30Snapshot *snapshot)
{
    if (snapshot)
        *snapshot = s_counters;
}
