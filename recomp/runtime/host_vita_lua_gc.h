#ifndef ISAAC_HOST_VITA_LUA_GC_H
#define ISAAC_HOST_VITA_LUA_GC_H

#include <stdint.h>

#include "guest.h"
#include "lua.h"

/* GC policy behind the frozen lua_gc import (host_vita_lua_gc.c):
 *   ISAAC_VITA_LUA_GCCOLLECT_CLAMP  floor-transition GCCOLLECT -> bounded step
 *   ISAAC_VITA_LUA_GC_PROFILE       one bounded "[isaac-lua] gc window" line
 *                                   per 120 LuaEngine::Update GC steps
 * The bridge (host_vita_lua.c) calls isaac_vita_lua_gc_call in place of
 * lua_gc while either option is on.  `requested` is the guest's own data
 * argument; `data` is the value after the LuaEngine::Update GCSTEP clamp
 * (ISAAC_VITA_LUA_GCSTEP_CLAMP_KB), i.e. what lua_gc actually receives. */
int isaac_vita_lua_gc_call(CPU *__restrict c, lua_State *state, int what,
                           int requested, int data);

#if defined(ISAAC_VITA_LUA_GC_PROFILE)
/* Callback trampoline entry (every Lua -> translated C function call).  The
 * profile counts the ones that happen inside lua_gc, i.e. __gc finalizers. */
void isaac_vita_lua_gc_profile_note_callback(uint32_t target);

/* lua_pcallk import: the lua_gc nesting depth before the protected call and
 * its restoration afterwards (a Lua error inside a finalizer longjmps past
 * isaac_vita_lua_gc_call).  Counted in the window line as unwind=. */
uint32_t isaac_vita_lua_gc_profile_depth(void);
void isaac_vita_lua_gc_profile_unwind(uint32_t depth);

/* host_vita_lua.c: private-arena live/peak KiB and guest-heap fallbacks;
 * zeros when ISAAC_VITA_LUA_ARENA_MB is 0. */
void isaac_vita_lua_arena_stats(uint32_t *live_kb, uint32_t *peak_kb,
                                uint32_t *fallbacks);
#endif

#endif
