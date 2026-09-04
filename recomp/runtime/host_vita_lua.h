#ifndef ISAAC_HOST_VITA_LUA_H
#define ISAAC_HOST_VITA_LUA_H

#include <stdint.h>

#include "guest.h"

#define ISAAC_VITA_LUA_IMPORT_COUNT 67U

const char *isaac_vita_lua_import_name(uint32_t index);
int isaac_vita_lua_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count);

#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
/* ISAAC_VITA_LUA_IMPORT_FASTDISPATCH (wf/opt-lua-direct): typed direct
 * endpoints for the thirteen hottest Lua imports, each with the family
 * signature so guest_import_call and guest_host_import_id reach it with one
 * indirect call (host_vita_lua.c has the contract).  host_vita_import_id.c
 * calls prepare(0) when a registration starts and prepare(1) once the
 * complete 413-row contract passed; the table stays empty (every import on
 * its generic handler) unless all thirteen names bound.  Returns the number
 * bound (13 or 0). */
uint32_t isaac_vita_lua_import_fast_prepare(int enable);
extern guest_import_family_fn
    g_isaac_vita_lua_import_fast_by_index[ISAAC_VITA_LUA_IMPORT_COUNT];
static inline guest_import_family_fn
isaac_vita_lua_import_fast_binding(uint32_t index)
{
    return index < ISAAC_VITA_LUA_IMPORT_COUNT
        ? g_isaac_vita_lua_import_fast_by_index[index] : NULL;
}
#endif

/* guest_fault()/guest_exit() can leave a native Lua activation through the
 * outer guest run boundary.  This drops process-local bridge ownership and
 * invalidates callback records before another CPU is allowed to enter. */
void isaac_vita_lua_abort_cpu(CPU *c);

/* ISAAC_VITA_LUA_ARENA_MB > 0: reserve the private LuaEngine mspace.  Call it
 * before vitaGL init so the block is carved out of USER_RW memory that vitaGL
 * would otherwise absorb into its RAM pool.  Returns 0 (and logs) when the
 * arena is unavailable; the bridge then keeps the guest allocator. */
int isaac_vita_lua_arena_reserve(void);

#if defined(ISAAC_VITA_LUA_NATIVE_INDEX)
/* ISAAC_VITA_LUA_NATIVE_INDEX: emit the '[isaac-lua] native-index' counters
 * (hits by kind, fallbacks by reason, VERIFY runs/mismatches) now; the bridge
 * itself logs one line per 65536 native hits and after the VERIFY burst. */
void isaac_vita_lua_native_index_report(void);
#endif

#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
/* Bridge owner for the native getClass/getExact seam (host_vita_lua_getclass.c):
 * enter returns 0 when another CPU owns the Lua bridge (the seam then falls
 * back to the translated body); *root reports whether this activation took
 * the slot and must hand it to leave(). */
int isaac_vita_lua_seam_enter(CPU *__restrict c, int *root);
void isaac_vita_lua_seam_leave(CPU *__restrict c, int root);
#endif

#endif
