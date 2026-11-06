/* LuaBridge binding replays (ISAAC_VITA_LUA_BINDING_REPLAY).
 *
 * 54 frozen LuaBridge thunk bodies are Lua C functions the game registers on
 * its classes: 51 CallMember scalar getters (one 92-byte template, "template
 * A"), Entity:GetData (sub_00446d90), a bool f(int,int) free-function thunk
 * (sub_00433870) and the second CFunc::indexMetaMethod (sub_003f90b0).  Each
 * is a chain of Lua import crossings around one or two guest calls; on the A9
 * every crossing pays the translated push/flush, the direct import dispatch,
 * the bridge owner handshake, the endpoint's frame check and the reload.
 *
 * Mechanism.  Each body has exactly one translated definition, in a unit that
 * contains no other reference to it, and the dispatch table in guest_table.c
 * names it: GNU ld --wrap=sub_<rva> therefore redirects every edge to
 * __wrap_sub_<rva> (this file's .c), and __real_sub_<rva> is the translated
 * body.  The wrap runs a MIRROR of the translated body
 * (host_vita_lua_binding_mirror.h, generated from the corpus text by
 * recomp/vita/lua_binding_replay_table.py): the corpus statements verbatim --
 * the same GPUSH/GPOP/GESP_* stack traffic, the same register locals, the same
 * guest calls (getClass through its own --wrap, the members, the 280cc0 /
 * 3f8820 / 3f8840 helpers mirrored the same way) -- with each Lua import
 * crossing replaced by the endpoint's own semantics on the same guest frame:
 * the return word is pushed, the endpoint's low-water note is made, the Lua
 * 5.3.3 API is called with the arguments read from the guest stack exactly as
 * host_vita_lua.c reads them (vita_lua_index mapping included), EAX/EDX are set
 * where the endpoint sets them and the return word is popped.
 *
 * Exactness conditions, checked per crossing (a failed one makes that crossing
 * the real one, GUEST_IMPORT_CALL / guest_call with the full register file
 * published, i.e. exactly the translated statement):
 *   - the call target (IAT word or the cached register) equals the slot's
 *     tag word GUEST_IMAGE_BASE + slot, which the loader writes (guest.c
 *     tag_imports) and which routes to the Lua endpoint;
 *   - the endpoint frame [esp, esp + 4 + 4*args) lies inside the window
 *     [E - D, E + 8) validated against the bound guest stack at entry;
 *   - the bridge owner word named this CPU at entry and after every guest
 *     observation point since (getClass, a member, a GC-capable or
 *     guest-running import, a real crossing); otherwise every later crossing
 *     of the call is real ("drift").
 * An import that can run guest code (lua_pushstring through luaC_checkGC and
 * a __gc finalizer; lua_callk) or raise (luaL_ref / luaL_unref ERRMEM,
 * luaL_checkinteger) is entered with all eight registers and ESP published,
 * so a callback or a longjmp sees the CPU the translated crossing would have
 * left; GC-capable / guest-running ones reload all eight afterwards and pop
 * through gpop when ESP moved (VITA_LUA_FAST_END_STORE / VITA_LUA_END rules).
 * The replay holds no host resource across an observation point, so Lua
 * raises and guest escapes unwind through it harmlessly.
 *
 * Entry pre-checks (nothing is written before they pass; a failure calls
 * __real_ on the untouched CPU): the row's pins (template / body bytes
 * against the mapped image, once), the 17 IAT tag words and the two .rdata
 * strings (once), the window [E - D, E + 8) inside the bound guest stack,
 * and the bridge owner == this CPU (nested: inside a Lua call the owner word
 * already names it; root == 1 would mean no import scope is open).
 *
 * Not replayed (same policy as ISAAC_VITA_LUA_NATIVE_GETCLASS): the per-import
 * host call census (g_host_import_calls), the import beat receipt, the
 * phase-profile import notes (GUEST_PHASE_PROFILE_NOTE_CALL / _IMPORT), the
 * import coverage hook and the guest sampler's "ext" word -- the elided time
 * is attributed to the thunk.  Function coverage / PROFILE_FUNCTION_ENTRIES
 * ARE replayed: the mirrors keep the corpus guest_coverage_function calls.
 *
 * Proof: recomp/test_vita_lua_binding_replay.py (both headers generated to
 * their sha256 pins, a 32-bit MSVC host oracle that links the generated
 * bodies, the production bridge, both getClass modes and pristine Lua 5.3.3
 * and compares the translated and the replayed arm byte for byte: whole Lua
 * heap arena, guest stack, CPU struct, coverage, member/finalizer logs).
 * Default OFF.
 */
#ifndef ISAAC_HOST_VITA_LUA_BINDING_REPLAY_H
#define ISAAC_HOST_VITA_LUA_BINDING_REPLAY_H

#include <stdint.h>

#include "guest.h"

/* _FAMILIES bit mask (CMake ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES). */
#define ISAAC_LBR_FAMILY_BIT_SCALAR  0x1U
#define ISAAC_LBR_FAMILY_BIT_GETDATA 0x2U
#define ISAAC_LBR_FAMILY_BIT_BOOLII  0x4U
#define ISAAC_LBR_FAMILY_BIT_INDEX2  0x8U
#ifndef ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK
#define ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK 0xfU
#endif

enum isaac_lbr_family {
    ISAAC_LBR_FAMILY_SCALAR = 0,
    ISAAC_LBR_FAMILY_GETDATA,
    ISAAC_LBR_FAMILY_BOOLII,
    ISAAC_LBR_FAMILY_INDEX2,
    ISAAC_LBR_FAMILY_COUNT
};

/* Why a call went to the translated body (0 = served by the mirror). */
enum isaac_lbr_reason {
    ISAAC_LBR_HANDLED = 0,
    ISAAC_LBR_REASON_STACK,   /* [E - D, E + 8) not inside the bound guest stack */
    ISAAC_LBR_REASON_OWNER,   /* the bridge owner word does not name this CPU */
    ISAAC_LBR_REASON_PINS,    /* row bytes / IAT tag words / .rdata strings differ */
    ISAAC_LBR_REASON_COUNT
};

typedef struct isaac_vita_lua_binding_replay_stats {
    uint32_t calls[ISAAC_LBR_FAMILY_COUNT];      /* __wrap entries */
    uint32_t handled[ISAAC_LBR_FAMILY_COUNT];    /* served by the mirror */
    uint32_t fallbacks[ISAAC_LBR_FAMILY_COUNT];  /* sent to __real_ */
    uint32_t reasons[ISAAC_LBR_REASON_COUNT];
    uint32_t imports_elided;  /* crossings the mirror served natively */
    uint32_t imports_real;    /* crossings it made for real (a check failed) */
    uint32_t drift_calls;     /* handled calls that entered drift mode */
    uint32_t pins_disabled;   /* rows disabled by a pin mismatch */
    uint32_t last_reason;
    uint32_t verify_runs;     /* VERIFY: mirror results compared with __real_ */
    uint32_t verify_mismatches;
} isaac_vita_lua_binding_replay_stats;

const isaac_vita_lua_binding_replay_stats *isaac_vita_lua_binding_replay_stats_get(void);
void isaac_vita_lua_binding_replay_stats_reset(void);
void isaac_vita_lua_binding_replay_stats_log(void);

#endif /* ISAAC_HOST_VITA_LUA_BINDING_REPLAY_H */
