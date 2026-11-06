/* Execute the production aggregator, including real maximum-width formatting.
 * No model of its arithmetic: every result below comes from the included code. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define ISAAC_VITA_THEORA_CADENCE_PROFILE 1
#include "host_vita_theora_cadence_profile.h"

static char lines[3][384];
static unsigned calls, max_body, max_line;
void isaac_vita_log(const char *format, ...)
{
    va_list args;
    char complete[512];
    char *body = lines[calls % 3];
    int n, whole;
    va_start(args, format);
    n = vsnprintf(body, 384, format, args);
    va_end(args);
    assert(n >= 0 && n < 384);
    /* platform.c's exact format, with the widest process-low timestamp/thread. */
    whole = snprintf(complete, sizeof complete, "[%u.%03u thr 0x%x] %s\n",
        UINT32_MAX / 1000000U, UINT32_MAX % 1000000U / 1000U, UINT32_MAX, body);
    assert(whole > 0 && whole < 512);
    if ((unsigned)n > max_body) max_body = (unsigned)n;
    if ((unsigned)whole > max_line) max_line = (unsigned)whole;
    ++calls;
}

int main(void)
{
    VitaTheoraCadenceProfile p = {0};
    VitaTheoraCadenceSample a = {0}, b = {0}, n = {0};
    uint32_t flags = 0;
    unsigned i;
    vita_tc_begin(&p, &a, 0x48bdf5U);
    vita_tc_end(&p, &a, 0, 0, 0);
    vita_tc_close(&p, &a, 0x48bdf5U);
    assert(calls == 0); /* idle is not an active clip */

    a.clips = 1; a.now_us = 100; a.scheduler.wrapper_ticks = 10;
    a.scheduler_enabled = 1;
    a.native.packets = 20; a.native.rgb_decodes = 8;
    b = a; b.now_us = 21100;
    b.native.packets += 3; b.native.rgb_decodes += 2;
    b.native.vorbis_syntheses = 4;
    b.native.packetin_us_sum = 11000; b.native.rgb_us_sum = 7000;
    b.native.vorbis_us_sum = 2000;
    vita_tc_begin(&p, &a, 0x48bdf5U);
    vita_tc_end(&p, &b, 100, 20100, 0);
    n = b; n.now_us = 40100; ++n.scheduler.wrapper_ticks;
    n.scheduler.full_phases = n.scheduler.late_full_skips = 1;
    n.scheduler.game_updates = 1;
    vita_tc_close(&p, &n, 0x476b38U);
    assert(calls == 3 && p.rows == 1);
    assert(strstr(lines[0], "site=0048bdf5 next=00476b38 tick=10..11"));
    assert(strstr(lines[0], "enabled=1/1/1"));
    assert(strstr(lines[0], "interval_us=0000000000009c40 pump_us=0000000000004e20 service_us=0000000000005208"));
    assert(strstr(lines[1], "packets=3 rgb=2 vorbis=4 dup=0 errors=0 service_p/r/v=3/2/4"));
    assert(strstr(lines[1], "packet_us=0000000000002af8 rgb_us=0000000000001b58 vorbis_us=00000000000007d0"));
    assert(strstr(lines[2], "full=1 nonfull=0 late_full_skip=1 nonfull_skip=0 presents=0 game=1"));
    assert(strstr(lines[2], "fetch=0 upload_pop=0 discard=0 no_upload=0"));

    /* Next interval: zero RGB, but two previously paid frames discarded and
     * one upload/pop. A second fetch reuses the texture, not a new video FPS. */
    a = n; vita_tc_begin(&p, &a, 0x476b38U);
    vita_tc_end(&p, &a, a.now_us, a.now_us, 0);
    vita_tc_fetch(&p, 7, 9, 4, 5, 0);
    vita_tc_fetch_time(&p, 100, 350);
    vita_tc_subupload_time(&p, 125, 325);
    vita_tc_fetch(&p, 9, 9, 5, 5, 0);
    vita_tc_fetch_time(&p, 350, 356);
    n = a; n.now_us += 1000; n.scheduler.presents += 1;
    n.scheduler_enabled = 0;
    vita_tc_close(&p, &n, 0x48bdf5U);
    assert(calls == 6);
    assert(strstr(lines[1], "packets=0 rgb=0 vorbis=0"));
    assert(strstr(lines[2], "fetch=2 upload_pop=1 discard=2 no_upload=1"));
    assert(strstr(lines[2], "fetch_us=0000000000000100"));
    assert(strstr(lines[2], "subuploads=1 subupload_us=00000000000000c8"));
    assert(strstr(lines[0], "enabled=1/1/0"));

    /* All-zero active interval is retained, then teardown closes the tail. */
    vita_tc_begin(&p, &n, 0x48bdf5U);
    vita_tc_end(&p, &n, n.now_us, n.now_us, 0);
    vita_tc_close(&p, &n, 0x48bdf5U);
    assert(calls == 9 && strstr(lines[2], "fetch=0 upload_pop=0 discard=0 no_upload=0"));
    assert(strstr(lines[2], "subuploads=0 subupload_us=0000000000000000"));
    vita_tc_begin(&p, &n, 0x48bdf5U);
    n.clips = 0;
    vita_tc_end(&p, &n, n.now_us, n.now_us, 0);
    vita_tc_close(&p, &n, 0x48bdf5U);
    assert(calls == 12 && strstr(lines[0], "clips=1/0/0"));

    /* Invalid counters must be visible, never silently unsigned underflow. */
    assert(vita_tc_delta32(8, 2, &flags) == 0 && flags == 1);
    assert(vita_tc_delta64(8, 2, &flags) == 0 && flags == 5);
    assert(vita_tc_delta32(UINT32_MAX, UINT32_MAX, &flags) == 0 && flags == 7);
    memset(&p, 0, sizeof p); a.clips = UINT32_MAX;
    vita_tc_begin(&p, &a, 0); vita_tc_end(&p, &a, 1, 0, 1);
    vita_tc_fetch(&p, 0, 0, 0, 0, 1);
    vita_tc_close(&p, &a, 0);
    assert(strstr(lines[0], "flags=2c"));
    assert(strstr(lines[2], "fetch_fault=1"));
    vita_tc_fetch_time(&p, 9, 1);
    assert(p.flags & 4U);
    p.fetch_us = UINT64_MAX - 1U;
    vita_tc_fetch_time(&p, 0, 2);
    assert(p.fetch_us == UINT64_MAX && (p.flags & 2U));
    p.flags = 0u;
    vita_tc_subupload_time(&p, 9, 1);
    assert(p.flags == 4U);
    p.subupload_us = UINT64_MAX - 1U;
    vita_tc_subupload_time(&p, 0, 2);
    assert(p.subupload_us == UINT64_MAX && (p.flags & 2U));

    /* Fill every emitted numeric field to its maximum width. The 32-bit
     * counters deliberately signal saturation; time uses both full words. */
    memset(&p, 0, sizeof p); memset(&a, 0, sizeof a);
    a.clips = UINT32_MAX;
    vita_tc_begin(&p, &a, UINT32_MAX);
    memset(&n, 0xff, sizeof n);
    vita_tc_end(&p, &n, 0, UINT64_MAX, 0);
    p.rows = VITA_TC_MAX_ROWS - 1U; p.nested = UINT32_MAX;
    p.fetches = p.upload_pops = p.discards = p.no_upload = p.fetch_faults = UINT32_MAX;
    p.fetch_us = UINT64_MAX;
    p.subuploads = UINT32_MAX;
    p.subupload_us = UINT64_MAX;
    vita_tc_close(&p, &n, UINT32_MAX);
    assert(p.rows == VITA_TC_MAX_ROWS);
    assert(strstr(lines[1], "packet_us=ffffffffffffffff"));
    i = calls;
    vita_tc_close(&p, &n, UINT32_MAX);
    assert(calls == i + 1 && p.overflow && strstr(lines[i % 3], "tc.overflow"));
    vita_tc_close(&p, &n, UINT32_MAX);
    vita_tc_fetch(&p, 0, 1, 0, 1, 0);
    assert(calls == i + 1);
    /* Widest at/tick fields, at the production row-id bound. */
    memset(&p, 0, sizeof p);
    vita_tc_begin(&p, &n, UINT32_MAX);
    p.rows = VITA_TC_MAX_ROWS - 1U;
    vita_tc_end(&p, &n, 0, UINT64_MAX, 0);
    vita_tc_close(&p, &n, UINT32_MAX);
    assert(strstr(lines[(calls - 3) % 3], "id=2048"));
    printf("theora cadence aggregator: PASS (ordered deltas, paid RGB/skip, later discard/upload, zero rows, tail, faults, saturation, overflow; max body=%u/383 complete=%u/511)\n", max_body, max_line);
    return 0;
}
