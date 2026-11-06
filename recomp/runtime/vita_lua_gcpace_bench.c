/* Host model of LuaEngine::Update's paced GC (lua_gc(L, LUA_GCSTEP, K) once
 * per frame, K = ISAAC_VITA_LUA_GCSTEP_CLAMP_KB) against pristine Lua 5.3.3.
 * Live heap and natural garbage rate are command-line parameters taken from
 * the device log (session P2b: ~11.4 MB live, <=~100 KB/s of garbage at
 * ~50 frames/s).  Prints, per K, the total and worst per-frame time spent in
 * Lua (allocation + step, so work moved into allocation-driven steps is
 * counted), cycles completed and peak GCCOUNT.  Absolute times are x86 ones;
 * only ratios between K values are meant to transfer. */
#include <stdio.h>
#include <stdlib.h>

#include <windows.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const char *s_setup =
    "local live_kb, fin = ...\n"
    "keep = {}\n"
    "local mt = {__gc = function(o) fincount = fincount + 1 end}\n"
    "fincount = 0\n"
    "local i = 0\n"
    "while collectgarbage('count') < live_kb do\n"
    "  i = i + 1\n"
    "  keep[i] = {x = i, y = i * 2, name = 'e' .. i, sub = {i, i + 1}}\n"
    "end\n"
    "function frame(n)\n"
    "  for j = 1, n do\n"
    "    local v = {x = j, y = j}\n"
    "    if j % 4 == 0 then setmetatable(v, mt) end\n"
    "  end\n"
    "end\n";

int main(int argc, char **argv)
{
    int live_kb = argc > 1 ? atoi(argv[1]) : 11400;
    int per_frame = argc > 2 ? atoi(argv[2]) : 16;   /* objects per frame */
    int frames = argc > 3 ? atoi(argv[3]) : 3000;
    static const int ks[] = {128, 64, 32, 16, 8, 4, 1};
    size_t k;

    for (k = 0; k < sizeof ks / sizeof ks[0]; ++k) {
        lua_State *L = luaL_newstate();
        double total = 0.0, worst = 0.0, t0, t1;
        int f, peak = 0, cycles = 0;
        luaL_openlibs(L);
        if (luaL_loadstring(L, s_setup) != LUA_OK) {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            return 1;
        }
        lua_pushinteger(L, live_kb);
        lua_call(L, 1, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        for (f = 0; f < frames; ++f) {
            int kb;
            t0 = now_ms();
            lua_getglobal(L, "frame");
            lua_pushinteger(L, per_frame);
            lua_call(L, 1, 0);
            cycles += lua_gc(L, LUA_GCSTEP, ks[k]);
            t1 = now_ms();
            total += t1 - t0;
            if (t1 - t0 > worst)
                worst = t1 - t0;
            kb = lua_gc(L, LUA_GCCOUNT, 0);
            if (kb > peak)
                peak = kb;
        }
        lua_getglobal(L, "fincount");
        printf("K=%4d KB: lua %8.1f ms total %6.3f ms/frame worst %6.3f ms "
               "cycles=%d peak=%d KB finalizers=%d\n",
               ks[k], total, total / frames, worst, cycles, peak,
               (int)lua_tointeger(L, -1));
        lua_close(L);
    }
    return 0;
}
