/* Deterministic host oracle for the stable 30-Hz presentation policy. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_stable30.h"

#define ORACLE_MAX_FRAMES 1024u

static uint64_t s_now;
static uint32_t s_delay_fail;
static uint32_t s_delay_no_progress;
static uint32_t s_receipt_count;
static char s_receipt[256];

uint64_t isaac_vita_get_process_time(void) { return s_now; }

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_receipt, sizeof s_receipt, format, arguments);
    va_end(arguments);
    ++s_receipt_count;
}

int sceKernelDelayThread(unsigned int delay_us)
{
    if (s_delay_fail)
        return -1;
    if (!s_delay_no_progress)
        s_now += delay_us;
    return 0;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "stable30 oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 0; \
    } \
} while (0)

typedef struct Scenario {
    uint64_t update_at[ORACLE_MAX_FRAMES];
    uint64_t present_at[ORACLE_MAX_FRAMES];
    uint32_t update_count;
    uint32_t present_count;
    uint32_t original_limiter_calls;
    uint32_t manager_counter;
} Scenario;

static void original_60hz_limiter(uint64_t loop_start)
{
    uint64_t due = loop_start + 16667u;
    if (s_now < due)
        s_now = due;
}

static int run_tick(
    Scenario *scenario, uint32_t update_us, uint32_t render_us,
    uint32_t nonfull_us)
{
    uint64_t loop_start = s_now;
    int guarded_nonfull;
    int skip_render;
    int bypass;

    kage_vita_stable30_note_loop_head();
    guarded_nonfull =
        kage_vita_stable30_manager_entry(scenario->manager_counter);
    if (guarded_nonfull) {
        s_now += nonfull_us;
    } else {
        CHECK(scenario->update_count < ORACLE_MAX_FRAMES);
        scenario->update_at[scenario->update_count++] = s_now;
        s_now += update_us;
    }
    ++scenario->manager_counter;

    skip_render = kage_vita_stable30_plan_render(
        scenario->manager_counter);
    if (!skip_render) {
        CHECK(scenario->present_count < ORACLE_MAX_FRAMES);
        s_now += render_us;
        kage_vita_stable30_before_present();
        scenario->present_at[scenario->present_count++] = s_now;
        kage_vita_stable30_after_present(1);
    }

    bypass = kage_vita_stable30_finish_tick();
    if (!bypass) {
        ++scenario->original_limiter_calls;
        original_60hz_limiter(loop_start);
    }
    return 1;
}

static int check_normal(uint32_t update_us, uint32_t render_us)
{
    Scenario scenario;
    KageVitaStable30Snapshot snapshot;
    uint32_t i;

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    while (s_now < 10000000u)
        CHECK(run_tick(&scenario, update_us, render_us, 500u));
    kage_vita_stable30_snapshot(&snapshot);

    CHECK(scenario.update_count == scenario.present_count);
    CHECK(scenario.update_count >= 299u && scenario.update_count <= 301u);
    CHECK(scenario.present_at[0] == (uint64_t)update_us + render_us);
    CHECK(snapshot.full_phases == scenario.update_count);
    CHECK(snapshot.nonfull_phases == scenario.update_count);
    CHECK(snapshot.interpolation_renders_skipped == scenario.update_count);
    CHECK(snapshot.limiter_bypasses == scenario.update_count);
    CHECK(snapshot.limiter_fallbacks == scenario.update_count);
    CHECK(scenario.original_limiter_calls == scenario.update_count);
    CHECK(snapshot.delay_failures == 0u);
    CHECK(snapshot.clock_resets == 0u);
    for (i = 1u; i < scenario.update_count; ++i) {
        uint64_t update_delta =
            scenario.update_at[i] - scenario.update_at[i - 1u];
        uint64_t present_delta =
            scenario.present_at[i] - scenario.present_at[i - 1u];
        CHECK(update_delta >= 33333u);
        CHECK(update_delta <= 33335u);
        CHECK(present_delta >= 33333u);
        CHECK(present_delta <= 33335u);
        CHECK(scenario.present_at[i] > scenario.update_at[i]);
    }
    return 1;
}

static int check_overload_drops_time(void)
{
    Scenario scenario;
    uint32_t i;

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    for (i = 0u; i < 40u; ++i) {
        CHECK(run_tick(&scenario, 2500u, 40000u, 500u));
        CHECK(run_tick(&scenario, 2500u, 40000u, 500u));
        if (i == 10u)
            s_now += 250000u;
    }
    CHECK(scenario.update_count == 40u);
    CHECK(scenario.present_count == 40u);
    for (i = 1u; i < scenario.update_count; ++i) {
        CHECK(scenario.update_at[i] - scenario.update_at[i - 1u] >= 33333u);
        CHECK(scenario.present_at[i] - scenario.present_at[i - 1u] >= 33333u);
    }
    return 1;
}

static int check_variable_render_has_no_short_repayment(void)
{
    Scenario scenario;
    uint32_t frame;

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    for (frame = 0u; frame < 8u; ++frame) {
        uint32_t render_us = frame == 1u ? 25000u : 10000u;
        CHECK(run_tick(&scenario, 2500u, render_us, 500u));
        CHECK(run_tick(&scenario, 2500u, render_us, 500u));
    }
    CHECK(scenario.update_count == 8u);
    CHECK(scenario.present_count == 8u);
    CHECK(scenario.present_at[0] == 12500u);
    for (frame = 1u; frame < scenario.present_count; ++frame) {
        uint64_t delta =
            scenario.present_at[frame] - scenario.present_at[frame - 1u];
        CHECK(delta >= 33333u);
        if (frame >= 2u)
            CHECK(delta <= 33335u);
    }
    return 1;
}

static int check_counter_reset_keeps_full_floor(void)
{
    Scenario scenario;
    uint64_t update_delta;
    uint64_t present_delta;

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    scenario.manager_counter = 0u;
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    CHECK(scenario.update_count == 2u);
    CHECK(scenario.present_count == 2u);
    update_delta = scenario.update_at[1] - scenario.update_at[0];
    present_delta = scenario.present_at[1] - scenario.present_at[0];
    CHECK(update_delta >= 33333u && update_delta <= 33335u);
    CHECK(present_delta >= 33333u && present_delta <= 33335u);
    return 1;
}

static int check_signed_manager_parity(void)
{
    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;

    kage_vita_stable30_reset();
    CHECK(kage_vita_stable30_manager_entry(1u));
    kage_vita_stable30_reset();
    CHECK(kage_vita_stable30_manager_entry(0x7fffffffu));
    kage_vita_stable30_reset();
    CHECK(!kage_vita_stable30_manager_entry(0xffffffffu));
    kage_vita_stable30_reset();
    CHECK(!kage_vita_stable30_manager_entry(0x80000001u));
    kage_vita_stable30_reset();
    CHECK(!kage_vita_stable30_manager_entry(2u));
    return 1;
}

static int check_render_present_one_shot_ownership(void)
{
    uint64_t before;

    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();

    /* Establish both floors through the first exact full Render.  Its first
     * Present is deliberately immediate. */
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(!kage_vita_stable30_plan_render(1u));
    s_now = 10000u;
    kage_vita_stable30_before_present();
    CHECK(s_now == 10000u);
    kage_vita_stable30_after_present(1);

    /* A transition/Update-time platform Present before Manager::Render's
     * parity hook is unarmed: it neither waits nor rebases the old target. */
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(s_now == 33334u);
    before = s_now;
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(s_now == before);

    /* The exact full/odd Render hook arms one Present.  The old target is
     * still 10000 + 1/30, proving the unarmed call above did not rebase it. */
    CHECK(!kage_vita_stable30_plan_render(1u));
    kage_vita_stable30_before_present();
    CHECK(s_now == 43334u);
    kage_vita_stable30_after_present(1);

    /* Consumption happens before rebase.  A second wrapper call in the same
     * Render is unpaced and cannot move the target from 43334 + 1/30. */
    s_now = 50000u;
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(s_now == 50000u);
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(s_now == 66668u);
    CHECK(!kage_vita_stable30_plan_render(1u));
    s_now = 70000u;
    kage_vita_stable30_before_present();
    CHECK(s_now == 76668u);
    kage_vita_stable30_after_present(1);

    /* If an armed Render never reaches Present, the next loop head discards
     * ownership.  A later unrelated wrapper call stays below the live target
     * without waiting or rebasing it. */
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(s_now == 100002u);
    CHECK(!kage_vita_stable30_plan_render(1u));
    kage_vita_stable30_note_loop_head();
    s_now = 105000u;
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(s_now == 105000u);

    /* Any later Render parity decision clears stale ownership before it
     * decides whether this phase is the one allowed to arm. */
    s_now = 0u;
    kage_vita_stable30_reset();
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(!kage_vita_stable30_plan_render(1u));
    s_now = 10000u;
    kage_vita_stable30_after_present(1);
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(!kage_vita_stable30_plan_render(1u));
    CHECK(!kage_vita_stable30_plan_render(2u));
    s_now = 40000u;
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(s_now == 40000u);

    s_now = 0u;
    kage_vita_stable30_reset();
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(!kage_vita_stable30_plan_render(1u));
    s_now = 10000u;
    kage_vita_stable30_after_present(1);
    kage_vita_stable30_note_loop_head();
    CHECK(kage_vita_stable30_manager_entry(1u));
    CHECK(kage_vita_stable30_plan_render(2u));
    s_now = 20000u;
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(s_now == 20000u);
    return 1;
}

static int check_native_transition_guard_stays_unpaced(void)
{
    uint64_t before;

    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    kage_vita_stable30_note_loop_head();
    CHECK(kage_vita_stable30_manager_entry(1u));
    /* An odd post-counter models the native 4a26c/4a26d guard retaining the
     * phase.  It is neither skipped nor armed and uses the original limiter. */
    CHECK(!kage_vita_stable30_plan_render(1u));
    s_now = 5000u;
    before = s_now;
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(s_now == before);
    CHECK(!kage_vita_stable30_finish_tick());
    return 1;
}

static int check_wait_failure_keeps_one_render(void)
{
    Scenario scenario;
    KageVitaStable30Snapshot snapshot;

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_fail = 1u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    kage_vita_stable30_snapshot(&snapshot);
    CHECK(scenario.update_count == 1u);
    CHECK(scenario.present_count == 1u);
    CHECK(snapshot.interpolation_renders_skipped == 1u);
    CHECK(snapshot.delay_failures == 1u);
    CHECK(snapshot.limiter_bypasses == 0u);
    CHECK(snapshot.limiter_fallbacks == 2u);
    CHECK(scenario.original_limiter_calls == 2u);

    /* A runtime pacing failure is sticky until reset: the original limiter
     * resumes, but the odd-only Manager/Render parity gate remains armed. */
    s_delay_fail = 0u;
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    kage_vita_stable30_snapshot(&snapshot);
    CHECK(scenario.update_count == 2u);
    CHECK(scenario.present_count == 2u);
    CHECK(snapshot.interpolation_renders_skipped == 2u);
    CHECK(snapshot.delay_failures == 1u);
    CHECK(snapshot.limiter_bypasses == 0u);
    CHECK(scenario.original_limiter_calls == 4u);

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_no_progress = 1u;
    kage_vita_stable30_reset();
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    s_delay_no_progress = 0u;
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    kage_vita_stable30_snapshot(&snapshot);
    CHECK(scenario.update_count == 2u);
    CHECK(scenario.present_count == 2u);
    CHECK(snapshot.interpolation_renders_skipped == 2u);
    CHECK(snapshot.delay_failures == 1u);
    CHECK(snapshot.limiter_bypasses == 0u);
    CHECK(scenario.original_limiter_calls == 4u);
    return 1;
}

static int check_present_wait_failure_keeps_one_render(void)
{
    Scenario scenario;
    KageVitaStable30Snapshot snapshot;

    memset(&scenario, 0, sizeof scenario);
    s_now = 0u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 10000u, 500u));

    s_delay_fail = 1u;
    CHECK(run_tick(&scenario, 2500u, 1000u, 500u));
    s_delay_fail = 0u;
    CHECK(run_tick(&scenario, 2500u, 1000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 1000u, 500u));
    CHECK(run_tick(&scenario, 2500u, 1000u, 500u));
    kage_vita_stable30_snapshot(&snapshot);

    CHECK(scenario.update_count == 3u);
    CHECK(scenario.present_count == 3u);
    CHECK(snapshot.interpolation_renders_skipped == 3u);
    CHECK(snapshot.delay_failures == 1u);
    CHECK(snapshot.limiter_bypasses == 1u);
    CHECK(scenario.original_limiter_calls == 5u);
    return 1;
}

static int check_hostile_clock_keeps_one_render(void)
{
    KageVitaStable30Snapshot snapshot;

    s_now = UINT64_MAX / 3u + 1u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    kage_vita_stable30_reset();
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(!kage_vita_stable30_plan_render(1u));
    CHECK(!kage_vita_stable30_finish_tick());
    kage_vita_stable30_note_loop_head();
    CHECK(kage_vita_stable30_manager_entry(1u));
    CHECK(kage_vita_stable30_plan_render(2u));
    CHECK(!kage_vita_stable30_finish_tick());
    kage_vita_stable30_snapshot(&snapshot);
    CHECK(snapshot.full_phases == 1u);
    CHECK(snapshot.nonfull_phases == 1u);
    CHECK(snapshot.interpolation_renders_skipped == 1u);
    CHECK(snapshot.clock_resets == 1u);
    CHECK(snapshot.limiter_bypasses == 0u);
    return 1;
}

static int check_lifecycle_and_exact_parity(void)
{
    KageVitaStable30Snapshot snapshot;

    s_now = 123u;
    s_delay_fail = 0u;
    s_delay_no_progress = 0u;
    s_receipt_count = 0u;
    kage_vita_stable30_reset();
    kage_vita_stable30_note_loop_head();
    CHECK(!kage_vita_stable30_manager_entry(0u));
    CHECK(s_receipt_count == 1u);
    CHECK(strcmp(
        s_receipt,
        "KAGE VITA STABLE30: bid=host-oracle period=100000/3us "
        "manager=004b0027>004b0043 render=004b0614>004b1089 "
        "limiter=0048be3e present=wrapper-pre/post full-only=1") == 0);
    CHECK(!kage_vita_stable30_plan_render(1u));
    kage_vita_stable30_before_present();
    kage_vita_stable30_after_present(1);
    CHECK(!kage_vita_stable30_finish_tick());
    kage_vita_stable30_note_loop_head();
    CHECK(kage_vita_stable30_manager_entry(1u));
    CHECK(s_receipt_count == 1u);
    CHECK(kage_vita_stable30_plan_render(2u));
    {
        uint64_t before = s_now;
        kage_vita_stable30_before_present();
        kage_vita_stable30_after_present(1);
        CHECK(s_now == before);
    }
    s_now = 40000u;
    CHECK(kage_vita_stable30_finish_tick());

    kage_vita_stable30_deactivate();
    CHECK(!kage_vita_stable30_manager_entry(1u));
    CHECK(!kage_vita_stable30_plan_render(2u));
    CHECK(!kage_vita_stable30_finish_tick());
    CHECK(s_receipt_count == 1u);
    kage_vita_stable30_snapshot(&snapshot);
    CHECK(snapshot.loop_heads == 0u);
    CHECK(snapshot.full_phases == 0u);
    CHECK(snapshot.nonfull_phases == 0u);
    return 1;
}

int main(void)
{
    CHECK(check_normal(2500u, 10000u));
    CHECK(check_normal(2500u, 25000u));
    CHECK(check_overload_drops_time());
    CHECK(check_variable_render_has_no_short_repayment());
    CHECK(check_counter_reset_keeps_full_floor());
    CHECK(check_signed_manager_parity());
    CHECK(check_render_present_one_shot_ownership());
    CHECK(check_native_transition_guard_stays_unpaced());
    CHECK(check_wait_failure_keeps_one_render());
    CHECK(check_present_wait_failure_keeps_one_render());
    CHECK(check_hostile_clock_keeps_one_render());
    CHECK(check_lifecycle_and_exact_parity());
    puts("Vita stable30 presentation oracle: PASS "
         "(30 updates/presents; no interpolation render; "
         "light/heavy/variable/overload/rebase/signed-parity/one-shot/"
         "native-transition/failure/clock/lifecycle)");
    return 0;
}
