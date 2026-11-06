/* ISAAC_VITA_LUA_COST_PROFILE inside the frozen _RunCallback dispatch oracle
 * (vita_lua_callback_dispatch_oracle.c, recomp/test_vita_lua_cost.py): a real
 * microsecond clock, lines to stdout, and one flush at process exit so the
 * unchanged oracle main() needs no edit. */
#include <stdio.h>
#include <stdlib.h>

#include <windows.h>

#include "host_vita_lua_cost.h"

static void shim_flush(void)
{
    isaac_vita_lua_cost_flush();
    fflush(stdout);
}

uint64_t isaac_vita_lua_cost_oracle_clock(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
        atexit(shim_flush);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / (frequency.QuadPart / 1000000));
}

void isaac_vita_lua_cost_oracle_sink(const char *line)
{
    printf("%s\n", line);
}
