/* Compile the production seams, not a reimplementation of their wiring. */
#define ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE 1
#define ISAAC_VITA_PHASE_PROFILE 1
#include "../../runtime/kage_vita_phase_profile.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

GuestPhaseProfileCounters g_guest_phase_profile_counters;
IsaacVitaGlPhaseProfileCounters g_isaac_vita_gl_phase_profile_counters;
uint32_t g_isaac_vita_post_com_time_get_time_calls;
static uint64_t test_now = 1000u;
static uint32_t test_clocks, test_records;
static char test_spikes[6][512];

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++test_clocks;
    return test_now;
}
void gl_vita_backend_phase_profile_window_boundary(void) {}
int sceClibPrintf(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    int n;
    va_start(args, fmt);
    n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (strstr(line, "ph120.sp ")) {
        assert(test_records < 6u && n > 0 && n < 512);
        memcpy(test_spikes[test_records++], line, (size_t)n + 1u);
    }
    test_now += 777u; /* deliberately expensive logging, excluded from all */
    return n;
}

int main(void)
{
    uint32_t i;
    kage_vita_phase_profile_reset();
    kage_vita_phase_profile_note_loop_head(0u);
    for (i = 0u; i < 240u; ++i) {
        kage_vita_phase_profile_note_service_entry();
        test_now += 100u;
        kage_vita_phase_profile_note_update_entry();
        test_now += i == 5u ? 120000u : 1000u;
        kage_vita_phase_profile_note_render_entry();
        test_now += i == 6u ? 50000u : 1000u;
        ++g_isaac_vita_gl_phase_profile_counters.draw_elements;
        kage_vita_phase_profile_note_present_enter();
        test_now += 2000u;
        kage_vita_phase_profile_note_present_return();
        test_now += 1000u;
        kage_vita_phase_profile_note_render_return();
        kage_vita_phase_profile_note_limiter_entry();
        test_now += 1000u;
        kage_vita_phase_profile_note_limiter_exit();
        kage_vita_phase_profile_note_loop_head(i + 1u);
    }
#if defined(ISAAC_VITA_FRAME_SPIKE_PROFILE)
    assert(test_records == 6u);
    assert(strstr(test_spikes[0], "kind=all loop=5 "));
    assert(strstr(test_spikes[0], "all=125100 svc=100 upd=120000 rnd=4000 swp=2000 lim=1000"));
    assert(strstr(test_spikes[0], "mask=31 draw=1 clear=0 fbo=0 tex=0 bad=0 clamp=0"));
    assert(strstr(test_spikes[1], "kind=upd loop=5 "));
    assert(strstr(test_spikes[2], "kind=rnd loop=6 "));
    assert(strstr(test_spikes[2], "all=55100 svc=100 upd=1000 rnd=53000 swp=2000"));
    assert(strstr(test_spikes[3], "win=2 loops=239 kind=all loop=120 "));
    assert(strstr(test_spikes[3], "all=6100 svc=100 upd=1000 rnd=4000"));
#else
    assert(test_records == 0u);
#endif
    printf("frame spike seams: PASS clocks=%u records=%u\n", test_clocks, test_records);
    return 0;
}
