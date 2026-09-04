/* Host model of the Bundle30-32 loop with a sceKernelDelayThread wake-up
 * overshoot, which the deterministic cadence30 oracle cannot represent (its
 * DelayThread is `s_now += delay_us`).  Not part of the build.  From recomp/:
 *   clang -std=c11 -O2 -Iruntime -Ivita -DISAAC_VITA_FULLSPEED_SCHEDULER=1  *     runtime/kage_vita_fullspeed_scheduler.c  *     vita/host_tests/kage_vita_fullspeed_overshoot_model.c -o model
 *   ./model <stall_us> <overshoot_us> [render_us] [offset_us]
 * Same call sequence as the oracle's run_one_tick().  The stall lands inside a
 * Render (a room load); offset_us is one-time extra work before the first
 * post-stall Game::Update and steers the floor/grid phase offset -- scanning it
 * 0..33 ms exposes any offset band a policy leaves unaligned.
 *
 * Reproductions against real bundles (stall 200 ms, overshoot 180 us):
 *   155935b   render 7.4      60 upd / 12 pres, 60 waits ~29 ms, debt +10.8 ms/win   == Bundle30
 *   2a0eaa8   render 7.4      120, except offsets 15-16 ms -> 60                    == Bundle32 room2
 *   2a0eaa8   render 20       60 or 12 depending on offset                          == Bundle32 room1/3
 *   final     render <=12     120 / 60 at 30 UPS;  15-30 -> 60 / 60;  34-45 -> every
 *             full phase, FPS == UPS == 30*33.3/(update+render);  55 -> original 4:1.
 *             No offset band and no stall/overshoot combination breaks alignment. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kage_vita_fullspeed_scheduler.h"

static uint64_t s_now;
static uint64_t s_overshoot_us = 180;   /* Bundle31: 29.84 UPS -> 0.18 ms/period */
static uint64_t s_waits, s_waited;

uint64_t isaac_vita_get_process_time(void) { return s_now; }
int sceKernelDelayThread(unsigned int us) { s_now += us + s_overshoot_us; s_waits++; s_waited += us + s_overshoot_us; return 0; }
void isaac_vita_log(const char *f, ...) { (void)f; }

int main(int argc, char **argv)
{
    uint64_t SERVICE = 300, UPDATE = 1400, NONFULL = 300, RENDER = 9000, PRESENT = 110; if (argc > 3) RENDER = strtoull(argv[3], 0, 10);
    uint64_t stall_us = argc > 1 ? strtoull(argv[1], 0, 10) : 200000;
    if (argc > 2) s_overshoot_us = strtoull(argv[2], 0, 10);
    uint64_t perturb_us = argc > 4 ? strtoull(argv[4], 0, 10) : 0;
    uint32_t mgr = 0x10000000u, game = 0x20000000u, counter = 0, frame = 0;
    KageVitaFullspeedSnapshot prev; memset(&prev, 0, sizeof prev);
    uint64_t prev_waits = 0, prev_waited = 0;
    kage_vita_fullspeed_scheduler_reset();
    printf("stall=%llu us overshoot=%llu us\n", (unsigned long long)stall_us, (unsigned long long)s_overshoot_us);
    printf("%-4s %-8s %-8s %-6s %-8s %-8s %-9s\n","win","updates","presents","waits","avgwait","debt_us","wall_ms");
    for (uint32_t win = 0; win < 20; ++win) {
        uint64_t t0 = s_now;
        for (uint32_t tick = 0; tick < 120; ++tick) {
            kage_vita_fullspeed_scheduler_note_loop_head();
            kage_vita_fullspeed_scheduler_note_service();
            s_now += SERVICE;
            kage_vita_fullspeed_scheduler_note_manager_dispatch();
            kage_vita_fullspeed_scheduler_note_manager_entry(mgr, counter, 1u, game, frame);
            if ((counter & 1u) == 0u) {
                if (win == 0 && tick == 10 && perturb_us) { s_now += perturb_us; perturb_us = 0; }
                kage_vita_fullspeed_scheduler_note_game_update_begin(counter, game, frame);
                s_now += UPDATE; ++frame;
                kage_vita_fullspeed_scheduler_note_game_update_end(game, frame);
            } else s_now += NONFULL;
            ++counter;
            if (kage_vita_fullspeed_scheduler_plan_render(mgr, counter, 1u, game, frame)) {
                kage_vita_fullspeed_scheduler_note_render_entry();
                kage_vita_fullspeed_scheduler_note_render_body(mgr, counter, 1u);
                s_now += RENDER;
                if (win == 0 && tick >= 5 && tick <= 8 && stall_us) { s_now += stall_us; stall_us = 0; }
                kage_vita_fullspeed_scheduler_note_present();
                s_now += PRESENT;
                kage_vita_fullspeed_scheduler_note_render_return();
            }
            (void)kage_vita_fullspeed_scheduler_bypass_limiter();
        }
        KageVitaFullspeedSnapshot s; kage_vita_fullspeed_scheduler_snapshot(&s);
        uint64_t w = s_waits - prev_waits, wu = s_waited - prev_waited;
        printf("%-4u %-8u %-8u %-6llu %-8llu %-8u %-9.1f\n", win,
               s.game_updates - prev.game_updates, s.presents - prev.presents,
               (unsigned long long)w, (unsigned long long)(w ? wu / w : 0), s.debt_us,
               (s_now - t0) / 1000.0);
        prev = s; prev_waits = s_waits; prev_waited = s_waited;
    }
    return 0;
}
