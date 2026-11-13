/* ISAAC_VITA_UPDATE_HITCH_PROBE: the scheduler's two call sites, executed.
 *
 * Linked with the production kage_vita_fullspeed_scheduler.c; the call
 * sequence is the one of kage_vita_fullspeed_overshoot_model.c (same file
 * directory).  test_kage_vita_update_hitch.py builds it twice:
 *   probe ON   (-DISAAC_VITA_UPDATE_HITCH_PROBE=1 on both units)
 *   probe OFF  (no define: the scheduler must not reference the probe)
 * and requires the two "sched:" lines to be equal: same number of clock
 * reads, same waits, same scheduler counters -- the probe observes, it does
 * not read a clock or move a decision. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kage_vita_fullspeed_scheduler.h"

static uint64_t s_now;
static uint64_t s_clock_reads, s_waits, s_waited;

uint64_t isaac_vita_get_process_time(void) { ++s_clock_reads; return s_now; }
int sceKernelDelayThread(unsigned int us)
{
    s_now += us + 180u;
    ++s_waits;
    s_waited += us + 180u;
    return 0;
}
void isaac_vita_log(const char *f, ...) { (void)f; }

static void fail(const char *what, uint32_t tick)
{
    fprintf(stderr, "FAIL: %s at tick %u\n", what, tick);
    exit(1);
}

#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
#include "kage_vita_update_hitch.h"
static uint32_t s_begins, s_ends;
static uint64_t s_begin_us, s_begin_seen_at;
static uint64_t s_end_begin_us, s_end_us, s_end_seen_at;
static uint32_t s_end_before, s_end_after;

void kage_vita_update_hitch_begin(uint64_t begin_us)
{
    ++s_begins;
    s_begin_us = begin_us;
    s_begin_seen_at = s_now;
}
void kage_vita_update_hitch_end(uint64_t begin_us, uint64_t end_us,
                                uint32_t frame_before, uint32_t frame_after)
{
    ++s_ends;
    s_end_begin_us = begin_us;
    s_end_us = end_us;
    s_end_seen_at = s_now;
    s_end_before = frame_before;
    s_end_after = frame_after;
}
#endif

int main(void)
{
    const uint64_t SERVICE = 300, NONFULL = 300, RENDER = 9000, PRESENT = 110;
    uint32_t mgr = 0x10000000u, game = 0x20000000u, counter = 0, frame = 0;
    uint32_t updates = 0, hitches = 0, tick;
    KageVitaFullspeedSnapshot s;
    uint32_t hash = 2166136261u;
    size_t i;

    kage_vita_fullspeed_scheduler_reset();
    for (tick = 0; tick < 2400; ++tick) {
        kage_vita_fullspeed_scheduler_note_loop_head();
        kage_vita_fullspeed_scheduler_note_service();
        s_now += SERVICE;
        kage_vita_fullspeed_scheduler_note_manager_dispatch();
        kage_vita_fullspeed_scheduler_note_manager_entry(mgr, counter, 1u, game, frame);
        if ((counter & 1u) == 0u) {
            /* Most updates 1.4 ms; every 97th is a 110 ms hitch, the size
             * measured in the bench's fight-start windows. */
            uint64_t update_us = updates % 97u == 96u ? 110906u : 1400u;
            uint32_t frame_before = frame;
#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
            uint32_t begins = s_begins, ends = s_ends;
#endif
            kage_vita_fullspeed_scheduler_note_game_update_begin(counter, game, frame);
#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
            /* Exactly one begin, carrying the clock value the scheduler
             * holds once its floor wait is over (the stub clock does not
             * advance on a read, so that is "now"). */
            if (s_begins != begins + 1u) fail("begin count", tick);
            if (s_begin_us != s_now || s_begin_seen_at != s_now)
                fail("begin clock value", tick);
#endif
            s_now += update_us;
            ++frame;
            kage_vita_fullspeed_scheduler_note_game_update_end(game, frame);
#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
            if (s_ends != ends + 1u) fail("end count", tick);
            if (s_end_begin_us != s_begin_us) fail("end carries begin", tick);
            if (s_end_us != s_now || s_end_seen_at != s_now)
                fail("end clock value", tick);
            if (s_end_us - s_end_begin_us != update_us)
                fail("update duration", tick);
            if (s_end_before != frame_before || s_end_after != frame)
                fail("frame numbers", tick);
#else
            (void)frame_before;
#endif
            ++updates;
            hitches += update_us > 30000u;
        } else {
            s_now += NONFULL;
        }
        ++counter;
        if (kage_vita_fullspeed_scheduler_plan_render(mgr, counter, 1u, game, frame)) {
            kage_vita_fullspeed_scheduler_note_render_entry();
            kage_vita_fullspeed_scheduler_note_render_body(mgr, counter, 1u);
            s_now += RENDER;
            kage_vita_fullspeed_scheduler_note_present();
            s_now += PRESENT;
            kage_vita_fullspeed_scheduler_note_render_return();
        }
        (void)kage_vita_fullspeed_scheduler_bypass_limiter();
    }
    kage_vita_fullspeed_scheduler_snapshot(&s);
    for (i = 0; i < sizeof s; ++i)
        hash = (hash ^ ((const unsigned char *)&s)[i]) * 16777619u;
    if (updates < 1000u || hitches < 10u || s.game_updates != updates)
        fail("model did not run updates", updates);
#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
    if (s_begins != updates || s_ends != updates)
        fail("one begin and one end per update", s_begins);
    printf("probe: begins=%u ends=%u hitches=%u\n", s_begins, s_ends, hitches);
#endif
    printf("sched: updates=%u presents=%u clock_reads=%llu waits=%llu "
           "waited_us=%llu end_us=%llu snapshot=%08x\n",
           s.game_updates, s.presents, (unsigned long long)s_clock_reads,
           (unsigned long long)s_waits, (unsigned long long)s_waited,
           (unsigned long long)s_now, hash);
    return 0;
}
