#ifndef ISAAC_HOST_VITA_LUA_COST_H
#define ISAAC_HOST_VITA_LUA_COST_H

#include <stdint.h>

/* ISAAC_VITA_LUA_COST_PROFILE (profiling only, default OFF): where the time
 * of a mod Lua callback goes.  Per ~2 s window, two bounded log lines:
 *   [isaac-lua] cost     top-level lua_pcallk imports (pc), the translated
 *                        LuaBridge callbacks they reach (cb), automatic GC
 *                        steps split by context, allocator calls, pcall
 *                        sites (ps)
 *   [isaac-lua] cost-top the 8 callback targets with the most time
 * VM time = pc - cb - gcvm (the clock reads around cb are inside pc).
 * Single guest CPU, game thread only (host_vita_lua.c owner rules). */

typedef struct isaac_vita_lua_cost_mark {
    uint32_t cb_depth;
    uint32_t gc_depth;
    uint32_t top;
} isaac_vita_lua_cost_mark;

/* lua_pcallk import, around lua_pcallk; ret = guest return word. leave
 * unwinds callbacks/GC steps a Lua error longjmped past (counted as lost). */
void isaac_vita_lua_cost_pcall_enter(uint32_t ret, isaac_vita_lua_cost_mark *mark);
void isaac_vita_lua_cost_pcall_leave(const isaac_vita_lua_cost_mark *mark);

/* Lua -> translated C function trampoline (vita_lua_call_guest). */
void isaac_vita_lua_cost_cb_enter(uint32_t target);
void isaac_vita_lua_cost_cb_leave(void);

/* Emit the current window now (host oracles; not called on the device). */
void isaac_vita_lua_cost_flush(void);

#if defined(ISAAC_VITA_UPDATE_HITCH_PROBE)
/* Lifetime modular totals pcall n/us, callback n/us, GC n/us; unaffected by
 * the unit's own window roll-over (kage_vita_update_hitch.h). */
void isaac_vita_lua_cost_hitch_peek(uint32_t out[6]);
#endif

#if defined(ISAAC_VITA_LUA_COST_PROFILE_ORACLE)
/* Host oracles supply the clock (microseconds) and the line sink. */
uint64_t isaac_vita_lua_cost_oracle_clock(void);
void isaac_vita_lua_cost_oracle_sink(const char *line);
#endif

#endif
