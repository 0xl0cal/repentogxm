/* Deterministic host oracle for parity-aware cadence, debt and fail-open. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_fullspeed_scheduler.h"

static uint64_t s_now;
static uint32_t s_log_calls;
static uint32_t s_disable_log_calls;
static uint32_t s_delay_fail;
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
static uint32_t s_test_service_us = 500u;
static uint32_t s_test_overshoot_us, s_test_variable_wake, s_test_waits;
static uint32_t s_test_backward_wait;
#endif

#define ORACLE_MAX_GAME_UPDATES 1024u

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
#endif
    s_now += delay_us;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char record[256];

    va_start(arguments, format);
    (void)vsnprintf(record, sizeof record, format, arguments);
    va_end(arguments);
    if (strstr(record, "KAGE VITA CADENCE30:") == record &&
            strstr(record,
                   "tick_units=50000 game_tick_units=100000 "
                   "max_catchup=4 stale_catchup=1 max_full_skip=4 "
                   "parity=4a264 frame=1a30dc") != NULL)
        ++s_log_calls;
    if (strstr(record, "KAGE VITA CADENCE30 DISABLE:") == record)
        ++s_disable_log_calls;
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

static int run_one_tick(
    OracleGame *game, uint32_t full_update_us, uint32_t nonfull_update_us,
    uint32_t render_us, uint32_t interpolation_enabled,
    ScenarioResult *result)
{
    int render;

    kage_vita_fullspeed_scheduler_note_loop_head();
    kage_vita_fullspeed_scheduler_note_service();
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    s_now += s_test_service_us;
#else
    s_now += 500u;
#endif
    kage_vita_fullspeed_scheduler_note_manager_dispatch();
    kage_vita_fullspeed_scheduler_note_manager_entry(
        game->manager_pointer, game->manager_counter,
        interpolation_enabled, game->game_pointer, game->game_frame);
    ++game->wrapper_ticks_since_present;
    if ((game->manager_counter & 1u) == 0u) {
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
        kage_vita_fullspeed_scheduler_note_render_entry();
        kage_vita_fullspeed_scheduler_note_render_body(
            game->manager_pointer, game->manager_counter,
            interpolation_enabled);
        s_now += render_us;
        kage_vita_fullspeed_scheduler_note_present();
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
    CHECK(result.maximum_game_updates_per_second <= 30u);
    if (result.game_update_time_count > 1u) {
        uint32_t i;
        for (i = 1u; i < result.game_update_time_count; ++i)
            CHECK(result.game_update_times[i] -
                  result.game_update_times[i - 1u] >= 33333u);
    }
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
    CHECK(result.maximum_game_updates_per_second <= 30u);
    if (result.game_update_time_count > 1u) {
        uint32_t i;
        for (i = 1u; i < result.game_update_time_count; ++i)
            CHECK(result.game_update_times[i] -
                  result.game_update_times[i - 1u] >= 33333u);
    }
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

    /* A rebase must be consumed by the same tick's paired full Game update. */
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

int main(void)
{
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
            !check_wait_failure_fail_open())
        return 1;
#if defined(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE) && ISAAC_VITA_FULLSPEED_HEAD_ADVANCE
    if (!check_advanced_head_transitions() || !check_advanced_head_faults() ||
            !check_advanced_head_present_intervals())
        return 1;
    puts("Vita full-head advance: PASS (current identity/refusal, pause rollback, "
         "head/floor wait and clock faults, deactivate/reset, actual Game/Present intervals)");
#endif
    puts(
        "Vita cadence30 scheduler oracle: PASS "
        "(60-Hz monotonic wrapper; native 30-Hz Game parity; "
        "native interpolated Render at 5/10 ms; late nonfull discard and "
        "full-phase Render at 27/40/55 ms; one-tick stale wrapper debt; "
        "no stale simulation replay and <=30 Game updates/second; "
        "game-owned interpolation transitions; "
        "exact Continue counter normalization; "
        "exact null-to-new Game publication; "
        "exact zero-frame early-return acceptance/attribution; "
        "duplicate/missing/parity/frame/counter/pointer/wait fail-open)");
    return 0;
}
