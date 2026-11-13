/* Scheduler model for ISAAC_VITA_LUA_GC_IDLE_STEP: the real
 * kage_vita_fullspeed_scheduler.c driven through the seam sequence of the
 * main loop, with LuaEngine::Update's paced GC step as a cost at the loop
 * tail (sub_0048bc50: Manager::Update, Render, LuaEngine::Update, back to the
 * loop head).  recomp/test_vita_lua_gc_state_idle.py builds and reads it.
 *
 * The seam order is kage_vita_fullspeed_scheduler_oracle.c's run_debt_tick.
 * The scheduler is not modelled; the clock, the sleep, the costs and the
 * collector cycle are.  The carry decision is NOT modelled either: it is
 * host_vita_lua_gc_idle_rule.h, the function the unit calls.
 *
 * Policies per scene, same costs, same pseudo-random jitter:
 *   off     every tail runs its step (the build without the knob);
 *   on      the tail asks kage_vita_fullspeed_scheduler_gc_idle_query and
 *           applies the rule; a carried step runs at the start of the next
 *           tail's GC sequence (the unit runs it in the LUA_GCCOUNT import);
 *   sweep   as `on`, but no propagate step is ever proven short of the
 *           atomic (the gray-list proof never succeeds): the floor of what
 *           the rule carries;
 *   legacy  the rule of 3db58c5, before the fix: every BUSY step is
 *           carried, whatever it is.  Printed to show what the fix removes;
 *   nogc    the steps cost nothing -- the ceiling of ANY relocation of this
 *           work, since moving a step cannot gain more than deleting it.
 *
 * The collector cycle (INFERRED shape; the device figures of 2026-11-12 give one
 * 13 ms step per ~390 Update steps, 0.45-0.64 ms mean, <= 2.2 ms finalizer
 * steps for ~17 steps): CYCLE steps = a pause stretch whose calls run no
 * luaC_step, a propagate stretch whose last PROP_TAIL steps cannot be
 * proven and whose very last step reaches the atomic (gc_big_us), a sweep
 * stretch, and a callfin stretch of callfin_us steps.  CYCLE is odd so the
 * atomic falls on full and non-full ticks in turn.
 *
 * Asserted for any costs: the 30-Hz floor (no two update starts closer than
 * the floor period), no lost step, `on` and `sweep` never carry the atomic
 * step (`legacy` does: the positive control), and `off` on a scheduler
 * compiled WITH the knob is the transcript of the scheduler compiled
 * WITHOUT it (the test compares the DIGEST lines of the two builds). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kage_vita_fullspeed_scheduler.h"
#include "host_vita_lua_gc_idle_rule.h"

static uint64_t s_now;

uint64_t isaac_vita_get_process_time(void) { return s_now; }

int sceKernelDelayThread(unsigned int delay_us)
{
    s_now += delay_us;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

#define CHECK(condition, ...) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            exit(1); \
        } \
    } while (0)

enum { POLICY_OFF, POLICY_ON, POLICY_SWEEP, POLICY_LEGACY, POLICY_NOGC,
       POLICIES };
static const char *const s_policy_name[POLICIES] = {
    "off", "on", "sweep", "legacy", "nogc"
};

#define CYCLE 391U
#define CYCLE_PAUSE 61U
#define CYCLE_PROP 200U
#define CYCLE_PROP_TAIL 10U
#define CYCLE_SWEEP 110U
#define CYCLE_CALLFIN 20U
#define NOOP_US 20U

typedef struct Scene {
    const char *name;
    uint32_t service_us;
    uint32_t update_us;       /* Game::Update, full phase */
    uint32_t nonfull_us;      /* Manager::Update, non-full phase */
    uint32_t render_us;
    uint32_t jitter_us;       /* uniform 0..jitter added to update and render */
    uint32_t gc_us;
    uint32_t callfin_us;
    uint32_t gc_big_us;
    uint8_t duty;             /* port.ini fps_lock duty: 1 = 60, 0 = 30 */
} Scene;

typedef struct Result {
    uint32_t ticks;
    uint32_t updates;
    uint32_t presents;
    uint32_t min_gap_us;
    uint32_t max_gap_us;
    uint64_t late_us;         /* sum of (gap - floor period) over updates */
    uint32_t steps_requested;
    uint32_t steps_run;
    uint32_t carried;
    uint32_t carried_by[VITA_GCI_PHASES];
    uint32_t kinds[4];
    uint32_t big_at[4];       /* where the atomic steps ran, by answer kind */
    uint32_t big_carried;     /* atomic steps that were carried */
    uint64_t seconds_us;
    uint64_t digest;
} Result;

static uint32_t s_lcg;

static uint32_t jitter(uint32_t range)
{
    if (!range)
        return 0U;
    s_lcg = s_lcg * 1664525U + 1013904223U;   /* the oracle's debt_rand */
    return (s_lcg >> 8) % (range + 1U);
}

static uint64_t fnv(uint64_t hash, uint64_t value)
{
    uint32_t i;
    for (i = 0U; i < 8U; ++i) {
        hash ^= (value >> (8U * i)) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int step_is_big(uint32_t index)
{
    return index % CYCLE == CYCLE_PAUSE + CYCLE_PROP - 1U;
}

/* The phase host_vita_lua_gc.c would read for step `index`. */
static int step_phase(uint32_t index, int prop_proof)
{
    uint32_t at = index % CYCLE;

    CHECK(CYCLE_PAUSE + CYCLE_PROP + CYCLE_SWEEP + CYCLE_CALLFIN == CYCLE,
          "cycle shape");
    if (at < CYCLE_PAUSE)
        return VITA_GCI_PHASE_NOOP;
    at -= CYCLE_PAUSE;
    if (at < CYCLE_PROP)
        return prop_proof && at < CYCLE_PROP - CYCLE_PROP_TAIL ?
            VITA_GCI_PHASE_PROP_BOUND : VITA_GCI_PHASE_PROP;
    at -= CYCLE_PROP;
    return at < CYCLE_SWEEP ? VITA_GCI_PHASE_SWEEP : VITA_GCI_PHASE_CALLFIN;
}

static uint32_t step_cost(const Scene *scene, uint32_t index)
{
    int phase = step_phase(index, 1);

    if (step_is_big(index))
        return scene->gc_big_us;
    if (phase == VITA_GCI_PHASE_NOOP)
        return NOOP_US;
    if (phase == VITA_GCI_PHASE_CALLFIN)
        return scene->callfin_us;
    return scene->gc_us;
}

static Result run(const Scene *scene, int policy, uint32_t ticks)
{
    Result result;
    uint32_t manager_pointer = 0x10000000U;
    uint32_t game_pointer = 0x20000000U;
    uint32_t manager_counter = 0U;
    uint32_t game_frame = 0U;
    uint64_t last_update = 0U;
    uint32_t have_update = 0U;
    uint32_t owed = 0U;
    uint32_t owed_index = 0U;
    uint32_t tick;

    (void)owed_index;                  /* read by the carrying policies only */
    memset(&result, 0, sizeof result);
    result.min_gap_us = UINT32_MAX;
    result.digest = 1469598103934665603ULL;
    s_now = 1000U;
    s_lcg = 0x20261112U;
    kage_vita_fullspeed_scheduler_reset();
#if defined(ISAAC_VITA_RUNTIME_SETTINGS)
    kage_vita_fullspeed_scheduler_set_duty(scene->duty);
#endif
    for (tick = 0U; tick < ticks; ++tick) {
        uint32_t update_us = scene->update_us + jitter(scene->jitter_us);
        uint32_t render_us = scene->render_us + jitter(scene->jitter_us);
        uint32_t index = result.steps_requested;
        int render;

        kage_vita_fullspeed_scheduler_note_loop_head();
        kage_vita_fullspeed_scheduler_note_service();
        s_now += scene->service_us;
        kage_vita_fullspeed_scheduler_note_manager_dispatch();
        kage_vita_fullspeed_scheduler_note_manager_entry(
            manager_pointer, manager_counter, 1U, game_pointer, game_frame);
        if ((manager_counter & 1U) == 0U) {
            kage_vita_fullspeed_scheduler_note_game_update_begin(
                manager_counter, game_pointer, game_frame);
            if (have_update) {
                uint64_t gap = s_now - last_update;
                if (gap < result.min_gap_us)
                    result.min_gap_us = (uint32_t)gap;
                if (gap > result.max_gap_us)
                    result.max_gap_us = (uint32_t)gap;
                if (gap > 33334U)
                    result.late_us += gap - 33334U;
            }
            have_update = 1U;
            last_update = s_now;
            ++result.updates;
            result.digest = fnv(result.digest, s_now);
            s_now += update_us;
            ++game_frame;
            kage_vita_fullspeed_scheduler_note_game_update_end(
                game_pointer, game_frame);
        } else {
            s_now += scene->nonfull_us;
        }
        ++manager_counter;
        render = kage_vita_fullspeed_scheduler_plan_render(
            manager_pointer, manager_counter, 1U, game_pointer, game_frame);
        result.digest = fnv(result.digest, (uint64_t)render);
        if (render) {
            kage_vita_fullspeed_scheduler_note_render_entry();
            kage_vita_fullspeed_scheduler_note_render_body(
                manager_pointer, manager_counter, 1U);
            s_now += render_us;
            kage_vita_fullspeed_scheduler_note_present();
            kage_vita_fullspeed_scheduler_note_render_return();
            ++result.presents;
        }
        (void)kage_vita_fullspeed_scheduler_bypass_limiter();

        /* LuaEngine::Update: LUA_GCCOUNT, the controller, LUA_GCSTEP */
        ++result.steps_requested;
        if (policy == POLICY_NOGC) {
            ++result.steps_run;
        } else if (policy == POLICY_OFF) {
            s_now += step_cost(scene, index);
            ++result.steps_run;
        } else {
#if defined(ISAAC_VITA_LUA_GC_IDLE_STEP)
            uint32_t slack_us = 0U;
            int kind;
            int phase;
            int carry;

            /* the LUA_GCCOUNT import: the carried step, if any */
            if (owed) {
                s_now += step_cost(scene, owed_index);
                owed = 0U;
                ++result.steps_run;
            }
            kind = kage_vita_fullspeed_scheduler_gc_idle_query(&slack_us);
            CHECK(kind >= 0 && kind <= 3, "kind %d", kind);
            ++result.kinds[kind];
            phase = step_phase(index, policy == POLICY_ON);
            carry = policy == POLICY_LEGACY ?
                kind == KAGE_VITA_GC_IDLE_BUSY :
                vita_lua_gc_idle_carry(kind == KAGE_VITA_GC_IDLE_BUSY, owed,
                                       1U, phase);
            if (carry) {
                owed = 1U;
                owed_index = index;
                ++result.carried;
                ++result.carried_by[phase];
                if (step_is_big(index))
                    ++result.big_carried;
            } else {
                if (step_is_big(index))
                    ++result.big_at[kind];
                s_now += step_cost(scene, index);
                ++result.steps_run;
            }
#else
            CHECK(0, "this policy needs ISAAC_VITA_LUA_GC_IDLE_STEP");
#endif
        }
    }
    result.ticks = ticks;
    result.seconds_us = s_now - 1000U;
    CHECK(result.steps_run + owed == result.steps_requested, "lost a step");
    /* The 30-Hz floor: Game::Update never starts early (33,333 1/3 us). */
    CHECK(result.min_gap_us >= 33333U, "%s/%s: update gap %u us",
          scene->name, s_policy_name[policy], (unsigned)result.min_gap_us);
    /* The rule never carries the atomic step. */
    if (policy == POLICY_ON || policy == POLICY_SWEEP)
        CHECK(result.big_carried == 0U, "%s/%s: %u atomic steps carried",
              scene->name, s_policy_name[policy],
              (unsigned)result.big_carried);
    return result;
}

static void report(const Scene *scene, int policy, const Result *r)
{
    double seconds = (double)r->seconds_us / 1000000.0;

    printf("MODEL %s %s: pps=%.2f ups=%.3f max_gap_us=%u late_ms=%.1f "
           "seconds=%.2f", scene->name, s_policy_name[policy],
           (double)r->presents / seconds, (double)r->updates / seconds,
           (unsigned)r->max_gap_us, (double)r->late_us / 1000.0, seconds);
    if (policy == POLICY_ON || policy == POLICY_SWEEP ||
            policy == POLICY_LEGACY)
        printf(" kind(unk,free,busy,late)=%u,%u,%u,%u carried=%u "
               "carried(propb,sweep,callfin)=%u,%u,%u "
               "big_at(unk,free,busy,late)=%u,%u,%u,%u big_carried=%u",
               (unsigned)r->kinds[0], (unsigned)r->kinds[1],
               (unsigned)r->kinds[2], (unsigned)r->kinds[3],
               (unsigned)r->carried,
               (unsigned)r->carried_by[VITA_GCI_PHASE_PROP_BOUND],
               (unsigned)r->carried_by[VITA_GCI_PHASE_SWEEP],
               (unsigned)r->carried_by[VITA_GCI_PHASE_CALLFIN],
               (unsigned)r->big_at[0], (unsigned)r->big_at[1],
               (unsigned)r->big_at[2], (unsigned)r->big_at[3],
               (unsigned)r->big_carried);
    printf("\n");
}

int main(int argc, char **argv)
{
    /* service 500 us and non-full 500 us as in the scheduler oracle's
     * scenarios; gc 300 us, finalizer steps 2200 us and one 13 ms step per
     * cycle are device figures (gc window lines, 2026-11-12). */
    static const Scene fixed[] = {
        { "idle-slack15", 500U, 500U, 500U, 600U, 300U, 300U, 2200U, 13000U, 1U },
        { "light-60pps", 500U, 4000U, 500U, 7000U, 1500U, 300U, 2200U, 13000U, 1U },
        { "fight-slack10", 500U, 11000U, 500U, 11000U, 1500U, 300U, 2200U, 13000U, 1U },
        { "fight-slack7", 500U, 12500U, 500U, 12500U, 1500U, 300U, 2200U, 13000U, 1U },
        { "fight-slack4", 500U, 14000U, 500U, 14000U, 1500U, 300U, 2200U, 13000U, 1U },
        { "zero-slack", 500U, 19000U, 500U, 16000U, 1500U, 300U, 2200U, 13000U, 1U },
        { "lock30-fight", 500U, 12500U, 500U, 12500U, 1500U, 300U, 2200U, 13000U, 0U },
        { "lock30-light", 500U, 4000U, 500U, 7000U, 1500U, 300U, 2200U, 13000U, 0U },
        /* atomic vs render: Update 12 ms, render 12 ms +- 3: a render fits the 21 ms
         * left to the floor when nothing else is in the way, and the 13 ms
         * atomic is what decides it */
        { "atomic-vs-render", 500U, 12000U, 500U, 12000U, 6000U, 500U, 2200U, 13000U, 1U },
    };
    uint32_t ticks = 7200U;
    uint32_t i;
    int policy;
    int off_only = argc > 1 && !strcmp(argv[1], "off-only");

    setvbuf(stdout, NULL, _IONBF, 0);
    for (i = 0U; i < sizeof fixed / sizeof fixed[0]; ++i) {
        Result results[POLICIES];

        for (policy = 0; policy < POLICIES; ++policy) {
            if (off_only && policy != POLICY_OFF && policy != POLICY_NOGC)
                continue;
            results[policy] = run(&fixed[i], policy, ticks);
            if (!off_only)
                report(&fixed[i], policy, &results[policy]);
        }
        printf("DIGEST %s off=%016llx nogc=%016llx\n", fixed[i].name,
               (unsigned long long)results[POLICY_OFF].digest,
               (unsigned long long)results[POLICY_NOGC].digest);
    }
    if (off_only)
        return 0;
    /* Sweep: where on the render-cost axis does moving the step matter?
     * Update 12 ms + jitter, render 6..16 ms in 0.25 ms steps.  Two jitter
     * widths: the narrow one of the fixed scenes, and a wide one (a fight's
     * frame costs vary by several ms) with the step at 500 us -- the device's
     * Update-site GC is 25-35 ms/s over about 55 steps/s (gc window lines). */
    {
        static const uint32_t widths[2][2] = {
            { 1500U, 300U }, { 6000U, 500U }
        };
        uint32_t w;

        for (w = 0U; w < 2U; ++w) {
            double best[POLICIES], worst[POLICIES], sum[POLICIES];
            double worst_ups[POLICIES], worst_late[POLICIES];
            uint32_t best_at[POLICIES], worst_at[POLICIES];
            uint32_t big_carried[POLICIES];
            uint32_t points = 0U;
            uint32_t render_us;

            for (policy = 0; policy < POLICIES; ++policy) {
                best[policy] = -1e9;
                worst[policy] = 1e9;
                sum[policy] = 0.0;
                worst_ups[policy] = 1e9;
                worst_late[policy] = -1e9;
                best_at[policy] = worst_at[policy] = 0U;
                big_carried[policy] = 0U;
            }
            for (render_us = 6000U; render_us <= 16000U; render_us += 250U) {
                Scene scene = { "sweep", 500U, 12000U, 500U, 0U, 0U, 0U,
                                2200U, 13000U, 1U };
                Result results[POLICIES];
                double pps[POLICIES];
                double ups[POLICIES];

                scene.render_us = render_us;
                scene.jitter_us = widths[w][0];
                scene.gc_us = widths[w][1];
                for (policy = 0; policy < POLICIES; ++policy) {
                    results[policy] = run(&scene, policy, ticks);
                    pps[policy] = (double)results[policy].presents *
                        1000000.0 / (double)results[policy].seconds_us;
                    ups[policy] = (double)results[policy].updates *
                        1000000.0 / (double)results[policy].seconds_us;
                }
                printf("SWEEP jitter_us=%u gc_us=%u render_us=%u pps",
                       (unsigned)widths[w][0], (unsigned)widths[w][1],
                       (unsigned)render_us);
                for (policy = 0; policy < POLICIES; ++policy)
                    printf(" %s=%.2f", s_policy_name[policy], pps[policy]);
                printf(" ups");
                for (policy = 0; policy < POLICIES; ++policy)
                    printf(" %s=%.3f", s_policy_name[policy], ups[policy]);
                printf("\n");
                for (policy = 1; policy < POLICIES; ++policy) {
                    double gain = pps[policy] - pps[POLICY_OFF];
                    double late = ((double)results[policy].late_us -
                        (double)results[POLICY_OFF].late_us) / 1000.0;

                    if (gain > best[policy]) {
                        best[policy] = gain;
                        best_at[policy] = render_us;
                    }
                    if (gain < worst[policy]) {
                        worst[policy] = gain;
                        worst_at[policy] = render_us;
                    }
                    if (ups[policy] - ups[POLICY_OFF] < worst_ups[policy])
                        worst_ups[policy] = ups[policy] - ups[POLICY_OFF];
                    if (late > worst_late[policy])
                        worst_late[policy] = late;
                    sum[policy] += gain;
                    big_carried[policy] += results[policy].big_carried;
                }
                ++points;
            }
            for (policy = 1; policy < POLICIES; ++policy)
                printf("SWEEP-SUMMARY jitter_us=%u gc_us=%u points=%u "
                       "%s-off pps: mean=%+.2f best=%+.2f@%u worst=%+.2f@%u "
                       "ups worst=%+.3f late_ms worst=%+.1f big_carried=%u\n",
                       (unsigned)widths[w][0], (unsigned)widths[w][1],
                       (unsigned)points, s_policy_name[policy],
                       sum[policy] / points, best[policy],
                       (unsigned)best_at[policy], worst[policy],
                       (unsigned)worst_at[policy], worst_ups[policy],
                       worst_late[policy], (unsigned)big_carried[policy]);
        }
    }
    printf("MODEL PASS\n");
    return 0;
}
