#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define ISAAC_VITA_IO_WINDOW_PROFILE 1
#define ISAAC_VITA_STATIC_SFX_PROFILE 1
#define ISAAC_VITA_NATIVE_RESOURCE_PROFILE 1
#define ISAAC_VITA_STOCK_FBO_RT_REUSE 1
#define ISAAC_VITA_PNG_WINDOW_PROFILE 1
#define ISAAC_VITA_CRT_FILE_LOOKUP_HINT 1
static int capture(const char *, ...);
#define KVPP_PRINTF capture
#include "../../runtime/kage_vita_room_profile.h"

static uint32_t mode, lines, longest, io_lines, sfx_lines, reuse_lines, reuse_takes;
static uint32_t png_outer_lines;
static int capture(const char *fmt, ...)
{
    char line[1024];
    int n;
    va_list args;
    va_start(args, fmt);
    n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    assert(n > 0 && n < 512 && line[n - 1] == '\n');
    if ((uint32_t)n > longest) longest = (uint32_t)n;
    ++lines;
    if (strstr(line, "ph120.ioo ")) {
        ++io_lines;
        if (mode) {
            assert(strstr(line, "req=4294967295:4294967295"));
            assert(strstr(line, "valid=1"));
        } else assert(strstr(line, "valid=0"));
    }
    if (strstr(line, "ph120.sfx ")) {
        ++sfx_lines;
        assert(strstr(line, mode ? "take=1" : "take=0"));
    }
    if (strstr(line, "ph120.rr ")) {
        ++reuse_lines;
        assert(strstr(line, "abi=2"));
        assert(strstr(line, mode ? "switch=4294967295" : "switch=0"));
        assert(strstr(line, mode ? "sat=4294967295" : "sat=0"));
        assert(strstr(line, mode ? "lease(grants,hits,drains)=4294967295,4294967295,4294967295"
                                 : "lease(grants,hits,drains)=0,0,0"));
    }
    if (strstr(line, "ph120.pngo ")) {
        ++png_outer_lines;
        assert(strstr(line, "abi=1"));
        assert(strstr(line, "maxever=4294967295:4294967295"));
        if (mode) assert(strstr(line, "us=4294967295:4294967295"));
        else assert(strstr(line, "us=0:0"));
    }
    return n;
}

void kage_vita_io_window_take(kage_vita_io_window_snapshot *s)
{
    memset(s, mode ? 0xff : 0, sizeof *s);
    s->abi_version = KAGE_VITA_IO_WINDOW_ABI;
    s->snapshot_valid = mode;
}
int isaac_vita_static_sfx_profile_take_window(isaac_vita_static_sfx_window *s)
{
    if (!mode) return 0;
    memset(s, 0xff, sizeof *s);
    return 1;
}
void vglIsaacNativeResourceProfileTake(IsaacNativeResourceStats *s)
{
    memset(s, 0xff, sizeof *s);
    s->abi = ISAAC_NATIVE_RESOURCE_ABI;
    s->sizes_used = ISAAC_NATIVE_RESOURCE_SIZE_SLOTS;
}
void vglIsaacFboRtReuseProfileTake(IsaacFboRtReuseStats *s)
{
    ++reuse_takes;
    memset(s, mode ? 0xff : 0, sizeof *s);
}
void isaac_vita_png_window_snapshot(IsaacVitaPngWindowSnapshot *s)
{
    memset(s, mode ? 0xff : 0, sizeof *s);
}
void isaac_vita_png_outer_snapshot(IsaacVitaPngOuterSnapshot *s)
{
    /* Cumulative counters stay at the maximum after the first window. */
    memset(s, (mode || png_outer_lines) ? 0xff : 0, sizeof *s);
}
int isaac_vita_crt_file_lookup_hint_get(uint32_t out[2])
{
    if (!mode) return 0;
    out[0] = out[1] = UINT32_MAX;
    return 1;
}
int main(void)
{
    kage_vita_room_profile_discard();
    assert(reuse_takes == 1u && reuse_lines == 0u);
    mode = 1u;
    kage_vita_room_profile_report("12345678901234567890123456789012", UINT32_MAX, UINT32_MAX);
    assert(io_lines == KAGE_IO_OP_COUNT && sfx_lines == 1u);
    mode = 0u;
    kage_vita_room_profile_report("12345678901234567890123456789012", UINT32_MAX, UINT32_MAX);
    assert(io_lines == KAGE_IO_OP_COUNT * 2u && sfx_lines == 2u);
    assert(reuse_takes == 3u && reuse_lines == 2u);
    assert(png_outer_lines == 2u);
    printf("room profile formatting: PASS lines=%u max=%u (<512)\n", lines, longest);
    return 0;
}
