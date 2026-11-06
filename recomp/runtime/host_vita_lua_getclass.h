/* Native Userdata::getClass / Userdata::getExact seam (ISAAC_VITA_LUA_NATIVE_GETCLASS).
 *
 * Every Lua API method the frozen game exposes on an entity/object first calls
 * Userdata::getClass (sub_003f8e30; 917 direct call sites + the dispatch
 * table) to check that argument `index` is a userdata of the class named by
 * `classKey` (or a derived class), honouring const objects; the __gc handler
 * chain calls Userdata::getExact (sub_003f8c80; 65 sites).  Both bodies are
 * nothing but a chain of Lua import calls (38 GUEST_IMPORT_CALL sites in
 * getClass, ~14 on the happy path; 29 in getExact, 10 on the happy path).
 * On the A9 each import crossing costs the translated push/flush, the direct
 * import dispatch, the bridge owner handshake, the handler and the reload;
 * with EID loaded the perf:wf-prof-v6 sampler put 3f8e30 in the top-12 of
 * 82 render/update windows (4,311 samples) and 3f8c80 in 32 windows.
 *
 * Calling convention (PE disassembly == corpus guest_0120.c, MSVC fastcall-
 * ish): L = ecx, index = edx, classKey = [esp+4], canBeConst = byte [esp+8];
 * result in eax; plain `ret` (the caller does `add esp, 8`).  getExact:
 * L = ecx, classKey = [esp+4], index is always 1, caller does `add esp, 4`.
 * The generated caller brackets the call with GUEST_GPR_FLUSH/RELOAD, so the
 * registers are in the CPU struct at entry and read back from it at return.
 *
 * What the seam does (host_vita_lua_getclass.c): it acquires the same
 * process-wide bridge owner an import handler holds (a GC step reached through
 * lua_pushstring may enter a guest __gc closure, which needs the active CPU),
 * then replays the translated body's happy path with direct Lua 5.3.3 API
 * calls in the same order with the same arguments -- including the very same
 * guest .rdata pointers for "__const" / "__parent", so string interning and
 * the string cache evolve exactly as they would have:
 *
 *   rawgetp(REGISTRY, classKey) -> class table (else FALLBACK)
 *   isuserdata(index)           (else FALLBACK: "%s expected, got %s")
 *   getmetatable(index)         (else FALLBACK)
 *   rawgetp(mt, identityKey @ 0x98804140) must be a boolean (else FALLBACK)
 *   mt.__const == nil means a const object: !canBeConst -> FALLBACK
 *     ("cannot be const" luaL_argerror); else class := class.__const (table)
 *   loop: rawequal(mt, class) -> settop(entry) ; eax = touserdata(index)
 *         else mt := mt.__parent (table; nil -> FALLBACK: wrong class)
 *
 * FALLBACK restores the entry stack top and runs __real_sub_003f8e30 on the
 * untouched CPU, so every error text, luaL_argerror level and stack shape is
 * the translated body's own.  Nothing observable is written before the
 * decision except Lua stack slots above the entry top (dead after settop) and
 * the interning of two strings the game interns on every call anyway.
 *
 * Census: the seam notes the body's coverage entry like the inflate seam; the
 * elided Lua import crossings are counted in the stats (imports_elided), not
 * replayed into the import census -- avoiding that per-import bookkeeping is
 * the saving.  Profile builds therefore see fewer ph120 import events while
 * the knob is ON (documented in the CMake option text).
 *
 * Proof: recomp/test_vita_lua_getclass.py (PE pins, corpus pins, a 32-bit
 * host oracle that links the two generated bodies against the real bridge and
 * pristine Lua 5.3.3 and compares eax/esp/guest stack/Lua stack/error text
 * between the translated body and the wrap, a __gc-during-seam stress, the
 * VERIFY build).  Default OFF.
 *
 * Direct path (ISAAC_VITA_LUA_GETCLASS_DIRECT, needs NATIVE_GETCLASS; RENDER60
 * S3.2 "getclass-direct"): the wraps call isaac_vita_lua_get{class,exact}_
 * direct_try, the same replay with each Lua C API call replaced by that API
 * function's pristine lapi.c body (inlined; lua_lock/api_check are empty in
 * this build) and the two lua_rawgetp lookups -- registry[classKey] and
 * mt[identityKey] -- served from a validated node memo (slot picked by the
 * class key, collision-free over the corpus's 76 keys, resp. by the
 * metatable pointer; a slot remembers its Table* and key, and its node is
 * used only when it lies on a node boundary inside the table's current
 * node array and still holds the light-userdata key, which makes it
 * exactly luaH_get's answer).  Kept as
 * the API path does it: every stack slot written and L->top after each
 * step; every lua_pushstring as luaS_new(L, guest .rdata pointer) + push +
 * luaC_checkGC (same string cache and string table evolution, same GC
 * points, same debt); after a GC point stack addresses are derived again
 * from L->ci->func and tables re-read from their slots.  The proof is the
 * host oracle's direct variants (recomp/test_vita_lua_getclass.py): every
 * case run translated vs direct wrap vs API try, with GC debt, total bytes,
 * string-cache and dead-slot snapshots compared, and a stack-moving __gc
 * stress.  Default OFF; OFF compiles the API path only.
 */
#ifndef ISAAC_HOST_VITA_LUA_GETCLASS_H
#define ISAAC_HOST_VITA_LUA_GETCLASS_H

#include <stdint.h>

#include "guest.h"

/* Frozen PE facts (isaac-ng.exe.unpacked.exe, ImageBase 0x400000); RVAs. */
#define ISAAC_LGC_GETCLASS_RVA          0x003f8e30U
#define ISAAC_LGC_GETEXACT_RVA          0x003f8c80U
#define ISAAC_LGC_GETCLASS_COVERAGE     4588U
#define ISAAC_LGC_GETEXACT_COVERAGE     4587U
#define ISAAC_LGC_IDENTITY_KEY_RVA      0x00804140U   /* .data light-userdata key */
#define ISAAC_LGC_STR_TYPE_RVA          0x0074f298U   /* "__type" (error paths) */
#define ISAAC_LGC_STR_FORMAT_RVA        0x0074f2a0U   /* "%s expected, got %s" */
#define ISAAC_LGC_STR_CONST_RVA         0x0074f2b4U   /* "__const" */
#define ISAAC_LGC_STR_CANNOT_CONST_RVA  0x0074f314U   /* "cannot be const" */
#define ISAAC_LGC_STR_PARENT_RVA        0x0074f324U   /* "__parent" */

/* Longest __parent chain the native loop walks before handing the call to the
 * translated body (which walks any length). */
#define ISAAC_LGC_MAX_HOPS 16U

/* Why the last call fell back (0 = handled natively). */
enum isaac_lgc_reason {
    ISAAC_LGC_HANDLED = 0,
    ISAAC_LGC_REASON_STACK,        /* [esp, esp+args] not inside the bound guest stack */
    ISAAC_LGC_REASON_PINS,         /* "__const"/"__parent" not at their .rdata addresses */
    ISAAC_LGC_REASON_OWNER,        /* another CPU owns the Lua bridge */
    ISAAC_LGC_REASON_CLASS,        /* registry[classKey] is not a table */
    ISAAC_LGC_REASON_USERDATA,     /* argument is not a userdata (argerror path) */
    ISAAC_LGC_REASON_METATABLE,    /* userdata without a metatable */
    ISAAC_LGC_REASON_IDENTITY,     /* mt[identityKey] is not a boolean (foreign mt) */
    ISAAC_LGC_REASON_CONST,        /* const object where canBeConst == 0 */
    ISAAC_LGC_REASON_CONST_CLASS,  /* class.__const is not a table */
    ISAAC_LGC_REASON_PARENT,       /* chain ended without a match (wrong class) */
    ISAAC_LGC_REASON_PARENT_TYPE,  /* mt.__parent is neither nil nor a table */
    ISAAC_LGC_REASON_HOPS,         /* more than ISAAC_LGC_MAX_HOPS parents */
    ISAAC_LGC_REASON_COUNT
};

enum isaac_lgc_function {
    ISAAC_LGC_FN_GETCLASS = 0,
    ISAAC_LGC_FN_GETEXACT = 1,
    ISAAC_LGC_FN_COUNT
};

typedef struct isaac_vita_lua_getclass_stats {
    uint32_t calls[ISAAC_LGC_FN_COUNT];      /* __wrap entries */
    uint32_t handled[ISAAC_LGC_FN_COUNT];    /* served natively */
    uint32_t fallbacks[ISAAC_LGC_FN_COUNT];  /* sent to the translated body */
    uint32_t hops;                           /* __parent hops on handled getClass calls */
    uint32_t const_objects;                  /* handled getClass calls on const objects */
    uint32_t imports_elided;                 /* Lua import crossings the native path replaced */
    uint32_t reasons[ISAAC_LGC_REASON_COUNT];
    uint32_t last_reason;
    uint32_t verify_runs;                    /* VERIFY: native results compared with __real */
    uint32_t verify_mismatches;
} isaac_vita_lua_getclass_stats;

/* Bodies of the two wraps: 1 = handled (eax set, return word popped), 0 = the
 * caller must run the __real body on the untouched CPU. */
int isaac_vita_lua_getclass_try(CPU *__restrict c);
int isaac_vita_lua_getexact_try(CPU *__restrict c);

const isaac_vita_lua_getclass_stats *isaac_vita_lua_getclass_stats_get(void);
void isaac_vita_lua_getclass_stats_reset(void);
void isaac_vita_lua_getclass_stats_log(void);

#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP)
void __wrap_sub_003f8e30(CPU *__restrict c);
void __wrap_sub_003f8c80(CPU *__restrict c);
#endif

#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
/* Node memo counters of the direct path, in [getclass stats] as
 * memo(class hit/miss/stale,identity hit/miss/stale). */
enum isaac_lgc_memo_counter {
    ISAAC_LGC_MEMO_CLASS_HIT = 0,
    ISAAC_LGC_MEMO_CLASS_MISS,
    ISAAC_LGC_MEMO_CLASS_STALE,      /* slot matched, node no longer valid */
    ISAAC_LGC_MEMO_IDENTITY_HIT,
    ISAAC_LGC_MEMO_IDENTITY_MISS,
    ISAAC_LGC_MEMO_IDENTITY_STALE,
    ISAAC_LGC_MEMO_COUNT
};

int isaac_vita_lua_getclass_direct_try(CPU *__restrict c);
int isaac_vita_lua_getexact_direct_try(CPU *__restrict c);
void isaac_vita_lua_getclass_direct_memo(uint32_t out[ISAAC_LGC_MEMO_COUNT]);
/* Oracle: forget every memo slot (the next lookups miss). */
void isaac_vita_lua_getclass_direct_memo_flush(void);
#endif

#endif /* ISAAC_HOST_VITA_LUA_GETCLASS_H */
