/* Native Userdata::getClass / getExact seam.  Design and exactness argument in
 * host_vita_lua_getclass.h.
 *
 * Compiled into the eboot when ISAAC_VITA_LUA_NATIVE_GETCLASS is ON (with
 * ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1 and the link's --wrap=sub_003f8e30
 * --wrap=sub_003f8c80) and, from the same source, into the 32-bit host oracle
 * (recomp/test_vita_lua_getclass.py) that links the generated bodies. */
#include "host_vita_lua_getclass.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"

#include "host_vita_lua.h"

#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
/* Lua 5.3.3 internals for the direct path (the pinned pristine headers the
 * isaac_lua53 archive is compiled from). */
#include "lapi.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"
#endif

#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
#error The native getClass seam must only be compiled for its opt-in feature
#endif
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT) && \
    !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP)
#error ISAAC_VITA_LUA_GETCLASS_DIRECT serves the --wrap seam (NATIVE_GETCLASS_WRAP)
#endif
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY) && \
    !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP)
#error ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY needs the __real bodies (WRAP)
#endif

/* recomp/vita/platform.h; declared here so the host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

#ifndef ISAAC_VITA_LUA_NATIVE_GETCLASS_BUILD_ID
#define ISAAC_VITA_LUA_NATIVE_GETCLASS_BUILD_ID "native-getclass:unstamped"
#endif

#define LGC_VA(rva) ((uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))
#define LGC_IDENTITY_KEY ((const void *)(uintptr_t)LGC_VA(ISAAC_LGC_IDENTITY_KEY_RVA))
#define LGC_STR_CONST    ((const char *)(uintptr_t)LGC_VA(ISAAC_LGC_STR_CONST_RVA))
#define LGC_STR_PARENT   ((const char *)(uintptr_t)LGC_VA(ISAAC_LGC_STR_PARENT_RVA))
#define LGC_STATS_PERIOD 0x10000U
#define LGC_VERIFY_FIRST 64U
#define LGC_VERIFY_EVERY 4096U

/* Import crossings of the translated bodies per path (guest_0120.c):
 * getClass exact match: rawgetp isuserdata getmetatable rawgetp type settop
 * absindex pushstring rawget type settop rawequal settop touserdata = 14;
 * a const object adds absindex pushstring rawget copy settop = 5; each
 * __parent hop adds absindex pushstring rawget type rotate settop rawequal
 * = 7.  getExact exact: absindex rawgetp isuserdata getmetatable rawgetp
 * type settop rawequal settop touserdata = 10; the const variant adds
 * absindex pushstring rawget rawequal = 4. */
#define LGC_IMPORTS_GETCLASS_BASE  14U
#define LGC_IMPORTS_GETCLASS_CONST 5U
#define LGC_IMPORTS_GETCLASS_HOP   7U
#define LGC_IMPORTS_GETEXACT_BASE  10U
#define LGC_IMPORTS_GETEXACT_CONST 4U

static isaac_vita_lua_getclass_stats s_stats;
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
static uint32_t s_memo[ISAAC_LGC_MEMO_COUNT];
#endif
static int s_banner_logged;
static int s_pins;                  /* 0 unchecked, 1 ok, -1 mismatch */
static uint32_t s_reason_logged;    /* bit per reason: first occurrence only */

static const char *const s_reason_names[ISAAC_LGC_REASON_COUNT] = {
    "handled", "stack", "pins", "owner", "class", "userdata", "metatable",
    "identity", "const", "const-class", "parent", "parent-type", "hops"
};
static const char *const s_function_names[ISAAC_LGC_FN_COUNT] = {
    "getClass", "getExact"
};

const isaac_vita_lua_getclass_stats *isaac_vita_lua_getclass_stats_get(void)
{
    return &s_stats;
}

void isaac_vita_lua_getclass_stats_reset(void)
{
    memset(&s_stats, 0, sizeof s_stats);
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    memset(s_memo, 0, sizeof s_memo);
#endif
    s_banner_logged = 0;
    s_pins = 0;
    s_reason_logged = 0U;
}

void isaac_vita_lua_getclass_stats_log(void)
{
    isaac_vita_log(
        "[isaac-lua] getclass stats: calls=%u native=%u fallbacks=%u "
        "getClass=%u/%u/%u getExact=%u/%u/%u hops=%u const=%u "
        "imports_elided=%u verify=%u/%u "
        "fb(stack,pins,owner,class,userdata,mt,identity,const,constclass,"
        "parent,ptype,hops)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u"
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
        " direct=1 memo(class hit/miss/stale,identity hit/miss/stale)="
        "%u/%u/%u,%u/%u/%u"
#endif
        ,
        (unsigned)(s_stats.calls[0] + s_stats.calls[1]),
        (unsigned)(s_stats.handled[0] + s_stats.handled[1]),
        (unsigned)(s_stats.fallbacks[0] + s_stats.fallbacks[1]),
        (unsigned)s_stats.calls[0], (unsigned)s_stats.handled[0],
        (unsigned)s_stats.fallbacks[0],
        (unsigned)s_stats.calls[1], (unsigned)s_stats.handled[1],
        (unsigned)s_stats.fallbacks[1],
        (unsigned)s_stats.hops, (unsigned)s_stats.const_objects,
        (unsigned)s_stats.imports_elided,
        (unsigned)s_stats.verify_mismatches, (unsigned)s_stats.verify_runs,
        (unsigned)s_stats.reasons[1], (unsigned)s_stats.reasons[2],
        (unsigned)s_stats.reasons[3], (unsigned)s_stats.reasons[4],
        (unsigned)s_stats.reasons[5], (unsigned)s_stats.reasons[6],
        (unsigned)s_stats.reasons[7], (unsigned)s_stats.reasons[8],
        (unsigned)s_stats.reasons[9], (unsigned)s_stats.reasons[10],
        (unsigned)s_stats.reasons[11], (unsigned)s_stats.reasons[12]
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
        , (unsigned)s_memo[ISAAC_LGC_MEMO_CLASS_HIT],
        (unsigned)s_memo[ISAAC_LGC_MEMO_CLASS_MISS],
        (unsigned)s_memo[ISAAC_LGC_MEMO_CLASS_STALE],
        (unsigned)s_memo[ISAAC_LGC_MEMO_IDENTITY_HIT],
        (unsigned)s_memo[ISAAC_LGC_MEMO_IDENTITY_MISS],
        (unsigned)s_memo[ISAAC_LGC_MEMO_IDENTITY_STALE]
#endif
        );
}

/* The two strings the native path hands to lua_pushstring are the game's own
 * .rdata bytes (identity-mapped image), so interning and the string cache see
 * the pointers the translated body would have pushed.  Pin them once. */
static int lgc_pins_ok(void)
{
    if (s_pins == 0) {
        s_pins = (strcmp(LGC_STR_CONST, "__const") == 0 &&
                  strcmp(LGC_STR_PARENT, "__parent") == 0) ? 1 : -1;
    }
    return s_pins > 0;
}

static void lgc_log_banner(void)
{
    if (s_banner_logged)
        return;
    s_banner_logged = 1;
    isaac_vita_log(
        "[isaac-lua] getclass seam: getClass@%08x getExact@%08x identity=%08x "
        "strings=%08x,%08x pins=%s max_hops=%u verify=%d build=%s",
        (unsigned)ISAAC_LGC_GETCLASS_RVA, (unsigned)ISAAC_LGC_GETEXACT_RVA,
        (unsigned)LGC_VA(ISAAC_LGC_IDENTITY_KEY_RVA),
        (unsigned)LGC_VA(ISAAC_LGC_STR_CONST_RVA),
        (unsigned)LGC_VA(ISAAC_LGC_STR_PARENT_RVA),
        lgc_pins_ok() ? "ok" : "MISMATCH",
        (unsigned)ISAAC_LGC_MAX_HOPS,
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        1,
#else
        0,
#endif
        ISAAC_VITA_LUA_NATIVE_GETCLASS_BUILD_ID);
}

static int lgc_fallback(unsigned fn, uint32_t reason)
{
    ++s_stats.fallbacks[fn];
    ++s_stats.reasons[reason];
    s_stats.last_reason = reason;
    if ((s_reason_logged & (1U << reason)) == 0U) {
        s_reason_logged |= 1U << reason;
        isaac_vita_log("[isaac-lua] getclass fallback: fn=%s reason=%s call=%u "
                       "(translated body used)",
                       s_function_names[fn], s_reason_names[reason],
                       (unsigned)s_stats.calls[fn]);
    }
    return 0;
}

static void lgc_handled(CPU *__restrict c, unsigned fn, uint32_t eax,
                        uint32_t imports)
{
    ++s_stats.handled[fn];
    s_stats.last_reason = ISAAC_LGC_HANDLED;
    s_stats.imports_elided += imports;
    guest_coverage_function(fn == ISAAC_LGC_FN_GETCLASS
                                ? ISAAC_LGC_GETCLASS_COVERAGE
                                : ISAAC_LGC_GETEXACT_COVERAGE);
    /* First native call, then every LGC_STATS_PERIOD handled calls: the
     * device gate reads calls/native/fallbacks from these lines. */
    if (s_stats.handled[0] + s_stats.handled[1] == 1U ||
        ((s_stats.handled[0] + s_stats.handled[1]) % LGC_STATS_PERIOD) == 0U)
        isaac_vita_lua_getclass_stats_log();
    /* Epilogue of the translated body: result in eax, pop the return word;
     * the caller's `add esp, N` removes the arguments. */
    c->eax = eax;
    (void)gpop_generated(c);
}

/* Mirror of host_vita_lua.c vita_lua_index: the bridge keeps its guest
 * function target in native upvalue #1, so guest upvalue pseudo-indices move
 * down by one; registry and ordinary stack indices are unchanged. */
static int lgc_index(int32_t index)
{
    return index < LUA_REGISTRYINDEX ? (int)index - 1 : (int)index;
}

/* Common prefix of both bodies.  On entry the Lua stack is at `top`; on
 * success it holds [class @ top+1, mt @ top+2] with the identity boolean
 * already popped, exactly the translated body's state before its own
 * comparison.  Every negative return is a fallback reason and leaves the
 * stack for the caller to restore. */
static uint32_t lgc_prefix(lua_State *L, int top, int idx, uint32_t class_key)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)class_key);
    if (lua_type(L, top + 1) != LUA_TTABLE)
        return ISAAC_LGC_REASON_CLASS;
    /* Relative indices are evaluated at the same depth as the translated
     * `push esi; push edi; call lua_isuserdata` (class already pushed). */
    if (!lua_isuserdata(L, idx))
        return ISAAC_LGC_REASON_USERDATA;
    if (!lua_getmetatable(L, idx))
        return ISAAC_LGC_REASON_METATABLE;
    lua_rawgetp(L, top + 2, LGC_IDENTITY_KEY);
    if (lua_type(L, top + 3) != LUA_TBOOLEAN)
        return ISAAC_LGC_REASON_IDENTITY;
    lua_settop(L, top + 2);
    return ISAAC_LGC_HANDLED;
}

int isaac_vita_lua_getclass_try(CPU *__restrict c)
{
    lua_State *L;
    int top, idx, root, is_const;
    uint32_t class_key, reason, hops, imports;
    int32_t index;
    uint8_t can_be_const;
    void *ud;

    ++s_stats.calls[ISAAC_LGC_FN_GETCLASS];
    lgc_log_banner();
    /* [ret, classKey, canBeConst] must be inside the bound guest stack. */
    if (!guest_stack_contains(c, c->esp, 12U))
        return lgc_fallback(ISAAC_LGC_FN_GETCLASS, ISAAC_LGC_REASON_STACK);
    if (!lgc_pins_ok())
        return lgc_fallback(ISAAC_LGC_FN_GETCLASS, ISAAC_LGC_REASON_PINS);
    L = (lua_State *)(uintptr_t)c->ecx;
    index = (int32_t)c->edx;
    class_key = ld32(c->esp + 4U);
    can_be_const = ld8(c->esp + 8U);   /* cmp byte ptr [ebp + 0xc], 0 */
    if (!isaac_vita_lua_seam_enter(c, &root))
        return lgc_fallback(ISAAC_LGC_FN_GETCLASS, ISAAC_LGC_REASON_OWNER);

    top = lua_gettop(L);
    idx = lgc_index(index);
    reason = lgc_prefix(L, top, idx, class_key);
    imports = LGC_IMPORTS_GETCLASS_BASE;
    hops = 0U;
    is_const = 0;
    if (reason == ISAAC_LGC_HANDLED) {
        /* mt.__const: nil marks a const object (its metatable is the const
         * variant); a table means a mutable object whose class has one. */
        lua_pushstring(L, LGC_STR_CONST);
        lua_rawget(L, top + 2);
        is_const = lua_type(L, top + 3) == LUA_TNIL;
        lua_settop(L, top + 2);
        if (is_const) {
            if (!can_be_const) {
                reason = ISAAC_LGC_REASON_CONST;
            } else {
                /* class := class.__const, the const variant of the class
                 * table, so the chain walk compares const metatables. */
                lua_pushstring(L, LGC_STR_CONST);
                lua_rawget(L, top + 1);
                if (lua_type(L, top + 3) != LUA_TTABLE) {
                    reason = ISAAC_LGC_REASON_CONST_CLASS;
                } else {
                    lua_copy(L, top + 3, top + 1);
                    lua_settop(L, top + 2);
                    imports += LGC_IMPORTS_GETCLASS_CONST;
                }
            }
        }
    }
    while (reason == ISAAC_LGC_HANDLED) {
        int parent_type;
        if (lua_rawequal(L, top + 1, top + 2)) {
            lua_settop(L, top);
            ud = lua_touserdata(L, idx);
            isaac_vita_lua_seam_leave(c, root);
            s_stats.hops += hops;
            if (is_const)
                ++s_stats.const_objects;
            lgc_handled(c, ISAAC_LGC_FN_GETCLASS, (uint32_t)(uintptr_t)ud,
                        imports);
            return 1;
        }
        if (hops >= ISAAC_LGC_MAX_HOPS) {
            reason = ISAAC_LGC_REASON_HOPS;
            break;
        }
        /* mt := mt.__parent (the translated rotate(-2,-1) + settop(-2) leaves
         * the same two live slots [class, parent]). */
        lua_pushstring(L, LGC_STR_PARENT);
        lua_rawget(L, top + 2);
        parent_type = lua_type(L, top + 3);
        if (parent_type == LUA_TNIL) {
            reason = ISAAC_LGC_REASON_PARENT;
        } else if (parent_type != LUA_TTABLE) {
            reason = ISAAC_LGC_REASON_PARENT_TYPE;
        } else {
            lua_copy(L, top + 3, top + 2);
            lua_settop(L, top + 2);
            ++hops;
            imports += LGC_IMPORTS_GETCLASS_HOP;
        }
    }
    /* Fallback: back to the entry top before the translated body runs. */
    lua_settop(L, top);
    isaac_vita_lua_seam_leave(c, root);
    return lgc_fallback(ISAAC_LGC_FN_GETCLASS, reason);
}

int isaac_vita_lua_getexact_try(CPU *__restrict c)
{
    lua_State *L;
    int top, root;
    uint32_t class_key, reason, imports;
    void *ud;

    ++s_stats.calls[ISAAC_LGC_FN_GETEXACT];
    lgc_log_banner();
    /* [ret, classKey] must be inside the bound guest stack. */
    if (!guest_stack_contains(c, c->esp, 8U))
        return lgc_fallback(ISAAC_LGC_FN_GETEXACT, ISAAC_LGC_REASON_STACK);
    if (!lgc_pins_ok())
        return lgc_fallback(ISAAC_LGC_FN_GETEXACT, ISAAC_LGC_REASON_PINS);
    L = (lua_State *)(uintptr_t)c->ecx;
    class_key = ld32(c->esp + 4U);
    if (!isaac_vita_lua_seam_enter(c, &root))
        return lgc_fallback(ISAAC_LGC_FN_GETEXACT, ISAAC_LGC_REASON_OWNER);

    top = lua_gettop(L);
    /* The translated body checks stack index 1 (lua_absindex(L, 1) == 1). */
    reason = lgc_prefix(L, top, 1, class_key);
    imports = LGC_IMPORTS_GETEXACT_BASE;
    if (reason == ISAAC_LGC_HANDLED) {
        int match = lua_rawequal(L, top + 1, top + 2);
        if (!match) {
            /* the const variant of the class is the other exact match */
            lua_pushstring(L, LGC_STR_CONST);
            lua_rawget(L, top + 1);
            match = lua_rawequal(L, top + 3, top + 2);
            imports += LGC_IMPORTS_GETEXACT_CONST;
            if (!match)
                reason = ISAAC_LGC_REASON_PARENT;
        }
        if (match) {
            lua_settop(L, top);
            ud = lua_touserdata(L, 1);
            isaac_vita_lua_seam_leave(c, root);
            lgc_handled(c, ISAAC_LGC_FN_GETEXACT, (uint32_t)(uintptr_t)ud,
                        imports);
            return 1;
        }
    }
    lua_settop(L, top);
    isaac_vita_lua_seam_leave(c, root);
    return lgc_fallback(ISAAC_LGC_FN_GETEXACT, reason);
}

#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
/* ISAAC_VITA_LUA_GETCLASS_DIRECT: the same replay as the two _try functions
 * above, with each Lua C API call replaced by the body of that API function
 * in pristine Lua 5.3.3 lapi.c (lua_lock/lua_unlock and api_check expand to
 * nothing in this build), so the compiler inlines them, and with the two
 * light-userdata lookups (lua_rawgetp) served from a validated node memo.
 * What stays exactly as the API path does it (host_vita_lua_getclass.h,
 * "direct path"):
 *   - every Lua stack slot written, in the same order, and L->top after each
 *     step, so a GC step sees the same live stack;
 *   - every lua_pushstring: luaS_new(L, <guest .rdata pointer>) -- the
 *     string cache (G(L)->strcache, keyed by the pointer) and the string
 *     table evolve as before -- then the push and luaC_checkGC(L), the only
 *     GC points, at the same L->top with the same debt;
 *   - after a GC point every stack address is derived again from
 *     L->ci->func (a GC step can shrink the stack, a finalizer can grow it)
 *     and tables are read again from their stack slots (a finalizer can
 *     rehash them); the node memo is only consulted before the first GC
 *     point of a call and validates its node on every use. */

/* lapi.c:40-50 */
#define LGC_NONVALIDVALUE       cast(TValue *, luaO_nilobject)
#define lgc_isvalid(o)          ((o) != luaO_nilobject)
#define lgc_ispseudo(i)         ((i) <= LUA_REGISTRYINDEX)

/* lapi.c:59-84 index2addr, verbatim (static there). */
static TValue *lgc_index2addr (lua_State *L, int idx) {
  CallInfo *ci = L->ci;
  if (idx > 0) {
    TValue *o = ci->func + idx;
    api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
    if (o >= L->top) return LGC_NONVALIDVALUE;
    else return o;
  }
  else if (!lgc_ispseudo(idx)) {  /* negative index */
    api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
    return L->top + idx;
  }
  else if (idx == LUA_REGISTRYINDEX)
    return &G(L)->l_registry;
  else {  /* upvalues */
    idx = LUA_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttislcf(ci->func))  /* light C function? */
      return LGC_NONVALIDVALUE;  /* it has no upvalues */
    else {
      CClosure *func = clCvalue(ci->func);
      return (idx <= func->nupvalues) ? &func->upvalue[idx-1] : LGC_NONVALIDVALUE;
    }
  }
}

/* Validated node memo for luaH_get(t, light userdata p).  A light-userdata
 * key is never in the array part, never becomes a dead key (lgc.c clears
 * only collectable keys) and occupies at most one node of a table (ltable.c
 * luaH_newkey inserts a key only after luaH_get missed it; a moved colliding
 * node takes its key with it).  So when node n lies on a node boundary
 * inside t's current node array and holds key p, luaH_get(t, p) -- the walk
 * of getgeneric, ltable.c:541-553 -- returns exactly gval(n), whatever
 * happened to t since the memo was filled (rehash, a freed and reused array
 * or Table).  Anything else is a miss served by luaH_get itself, whose hit
 * gval(n) == &n->i_val == (TValue *)n refills the slot. */
#define LGC_MEMO_SLOTS 256U

typedef struct lgc_memo_slot {
    const Table *t;
    const void *key;
    const Node *n;
} lgc_memo_slot;

static lgc_memo_slot s_class_memo[LGC_MEMO_SLOTS];
static lgc_memo_slot s_identity_memo[LGC_MEMO_SLOTS];

static int lgc_node_holds(const Table *t, const Node *n, const void *p)
{
    uintptr_t offset = (uintptr_t)n - (uintptr_t)t->node;
    return offset < (uintptr_t)sizenode(t) * (uintptr_t)sizeof(Node) &&
           offset % sizeof(Node) == 0U &&
           ttislightuserdata(gkey(n)) && pvalue(gkey(n)) == p;
}

static const TValue *lgc_get_light(Table *t, const void *p,
                                   lgc_memo_slot *slot, unsigned memo)
{
    TValue k;
    const TValue *v;
    if (slot->t == t && slot->key == p) {
        if (lgc_node_holds(t, slot->n, p)) {
            ++s_memo[memo + 0U];                      /* hit */
            return gval(slot->n);
        }
        ++s_memo[memo + 2U];                          /* stale */
    } else {
        ++s_memo[memo + 1U];                          /* miss */
    }
    setpvalue(&k, cast(void *, p));
    v = luaH_get(t, &k);
    if (v != luaO_nilobject &&
        lgc_node_holds(t, (const Node *)(const void *)v, p)) {
        slot->t = t;
        slot->key = p;
        slot->n = (const Node *)(const void *)v;
    }
    return v;
}

/* Identity memo: keyed by the metatable, a Lua heap pointer (the shift drops
 * the allocation-alignment bits).  Its slot spread depends on the heap layout
 * and is not checked here; the device line's identity hit/miss/stale counters
 * show it. */
static unsigned lgc_memo_index(uintptr_t value)
{
    return (unsigned)((value >> 3) ^ (value >> 11)) & (LGC_MEMO_SLOTS - 1U);
}

/* Class memo: keyed by the class key, the address of a LuaBridge 1-byte
 * ClassInfo static in the guest image.  These are packed densely: the
 * generated corpus pushes 76 distinct keys, 0x988066a6..0x98806984, before
 * its 917 getClass (sub_003f8e30) and 65 getExact (sub_003f8c80) calls (every
 * getExact key is also a getClass key).  The heap-pointer hash above drops the
 * low three bits that tell neighbouring 1-byte statics apart: it puts the 76
 * keys into 28 slots (74 keys share a slot, up to 5 per slot), so alternating
 * classes evict each other; (v ^ (v >> 8)) & 255 gives each of the 76 its own
 * slot.  test_vita_lua_getclass.py re-derives the key set from the corpus and
 * fails if this expression stops being collision-free on it. */
static unsigned lgc_class_memo_index(uint32_t class_key)
{
    return (unsigned)(class_key ^ (class_key >> 8)) & (LGC_MEMO_SLOTS - 1U);
}

/* lapi.c:669-680 lua_rawgetp, the luaH_get through the memo */
static int lgc_rawgetp(lua_State *L, int idx, const void *p,
                       lgc_memo_slot *memo, unsigned which)
{
  StkId t;
  t = lgc_index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top, lgc_get_light(hvalue(t), p, memo, which));
  api_incr_top(L);
  return ttnov(L->top - 1);
}

/* lapi.c:251-254 lua_type */
static int lgc_type (lua_State *L, int idx) {
  StkId o = lgc_index2addr(L, idx);
  return (lgc_isvalid(o) ? ttnov(o) : LUA_TNONE);
}

/* lapi.c:172-186 lua_settop */
static void lgc_settop (lua_State *L, int idx) {
  StkId func = L->ci->func;
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
    while (L->top < (func + 1) + idx)
      setnilvalue(L->top++);
    L->top = (func + 1) + idx;
  }
  else {
    api_check(L, -(idx+1) <= (L->top - (func + 1)), "invalid new top");
    L->top += idx+1;  /* 'subtract' index (index is negative) */
  }
}

/* lapi.c:222-233 lua_copy */
static void lgc_copy (lua_State *L, int fromidx, int toidx) {
  TValue *fr, *to;
  fr = lgc_index2addr(L, fromidx);
  to = lgc_index2addr(L, toidx);
  api_check(L, lgc_isvalid(to), "invalid index");
  setobj(L, to, fr);
  if (toidx < LUA_REGISTRYINDEX)  /* function upvalue? (isupvalue) */
    luaC_barrier(L, clCvalue(L->ci->func), fr);
}

/* lapi.c:289-292 lua_isuserdata */
static int lgc_isuserdata (lua_State *L, int idx) {
  const TValue *o = lgc_index2addr(L, idx);
  return (ttisfulluserdata(o) || ttislightuserdata(o));
}

/* lapi.c:295-299 lua_rawequal; luaV_rawequalobj is luaV_equalobj(NULL, ..)
 * (lvm.h:48), whose table/table case is pointer equality (lvm.c:409-418,
 * 435-437: same variant, and with L == NULL no __eq). */
static int lgc_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = lgc_index2addr(L, index1);
  StkId o2 = lgc_index2addr(L, index2);
  if (!lgc_isvalid(o1) || !lgc_isvalid(o2))
    return 0;
  if (ttistable(o1) && ttistable(o2))
    return hvalue(o1) == hvalue(o2);
  return luaV_rawequalobj(o1, o2);
}

/* lapi.c:413-420 lua_touserdata */
static void *lgc_touserdata (lua_State *L, int idx) {
  StkId o = lgc_index2addr(L, idx);
  switch (ttnov(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}

/* lapi.c:491-504 lua_pushstring for a non-NULL string: the GC point. */
static void lgc_pushstring (lua_State *L, const char *s) {
  TString *ts;
  ts = luaS_new(L, s);
  setsvalue2s(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
}

/* lapi.c:646-654 lua_rawget; the key on top is the short string luaS_new
 * returned ("__const"/"__parent", 7/8 bytes <= LUAI_MAXSHORTLEN), for which
 * luaH_get is luaH_getshortstr (ltable.c:570-572). */
static int lgc_rawget (lua_State *L, int idx) {
  StkId t;
  t = lgc_index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  api_check(L, ttisshrstring(L->top - 1), "short string key expected");
  setobj2s(L, L->top - 1, luaH_getshortstr(hvalue(t), tsvalue(L->top - 1)));
  return ttnov(L->top - 1);
}

/* lapi.c:696-718 lua_getmetatable */
static int lgc_getmetatable (lua_State *L, int objindex) {
  const TValue *obj;
  Table *mt;
  int res = 0;
  obj = lgc_index2addr(L, objindex);
  switch (ttnov(obj)) {
    case LUA_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttnov(obj)];
      break;
  }
  if (mt != NULL) {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  return res;
}

/* lgc_prefix with the inlined API: no GC point inside. */
static uint32_t lgc_direct_prefix(lua_State *L, int top, int idx,
                                  uint32_t class_key)
{
    Table *mt;
    lgc_rawgetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)class_key,
                &s_class_memo[lgc_class_memo_index(class_key)],
                ISAAC_LGC_MEMO_CLASS_HIT);
    if (lgc_type(L, top + 1) != LUA_TTABLE)
        return ISAAC_LGC_REASON_CLASS;
    if (!lgc_isuserdata(L, idx))
        return ISAAC_LGC_REASON_USERDATA;
    if (!lgc_getmetatable(L, idx))
        return ISAAC_LGC_REASON_METATABLE;
    mt = hvalue(L->top - 1);
    lgc_rawgetp(L, top + 2, LGC_IDENTITY_KEY,
                &s_identity_memo[lgc_memo_index((uintptr_t)mt)],
                ISAAC_LGC_MEMO_IDENTITY_HIT);
    if (lgc_type(L, top + 3) != LUA_TBOOLEAN)
        return ISAAC_LGC_REASON_IDENTITY;
    lgc_settop(L, top + 2);
    return ISAAC_LGC_HANDLED;
}

int isaac_vita_lua_getclass_direct_try(CPU *__restrict c)
{
    lua_State *L;
    int top, idx, root, is_const;
    uint32_t class_key, reason, hops, imports;
    int32_t index;
    uint8_t can_be_const;
    void *ud;

    ++s_stats.calls[ISAAC_LGC_FN_GETCLASS];
    lgc_log_banner();
    if (!guest_stack_contains(c, c->esp, 12U))
        return lgc_fallback(ISAAC_LGC_FN_GETCLASS, ISAAC_LGC_REASON_STACK);
    if (!lgc_pins_ok())
        return lgc_fallback(ISAAC_LGC_FN_GETCLASS, ISAAC_LGC_REASON_PINS);
    L = (lua_State *)(uintptr_t)c->ecx;
    index = (int32_t)c->edx;
    class_key = ld32(c->esp + 4U);
    can_be_const = ld8(c->esp + 8U);
    if (!isaac_vita_lua_seam_enter(c, &root))
        return lgc_fallback(ISAAC_LGC_FN_GETCLASS, ISAAC_LGC_REASON_OWNER);

    top = cast_int(L->top - (L->ci->func + 1));   /* lapi.c:167 lua_gettop */
    idx = lgc_index(index);
    reason = lgc_direct_prefix(L, top, idx, class_key);
    imports = LGC_IMPORTS_GETCLASS_BASE;
    hops = 0U;
    is_const = 0;
    if (reason == ISAAC_LGC_HANDLED) {
        lgc_pushstring(L, LGC_STR_CONST);              /* GC point */
        lgc_rawget(L, top + 2);
        is_const = lgc_type(L, top + 3) == LUA_TNIL;
        lgc_settop(L, top + 2);
        if (is_const) {
            if (!can_be_const) {
                reason = ISAAC_LGC_REASON_CONST;
            } else {
                lgc_pushstring(L, LGC_STR_CONST);      /* GC point */
                lgc_rawget(L, top + 1);
                if (lgc_type(L, top + 3) != LUA_TTABLE) {
                    reason = ISAAC_LGC_REASON_CONST_CLASS;
                } else {
                    lgc_copy(L, top + 3, top + 1);
                    lgc_settop(L, top + 2);
                    imports += LGC_IMPORTS_GETCLASS_CONST;
                }
            }
        }
    }
    while (reason == ISAAC_LGC_HANDLED) {
        int parent_type;
        if (lgc_rawequal(L, top + 1, top + 2)) {
            lgc_settop(L, top);
            ud = lgc_touserdata(L, idx);
            isaac_vita_lua_seam_leave(c, root);
            s_stats.hops += hops;
            if (is_const)
                ++s_stats.const_objects;
            lgc_handled(c, ISAAC_LGC_FN_GETCLASS, (uint32_t)(uintptr_t)ud,
                        imports);
            return 1;
        }
        if (hops >= ISAAC_LGC_MAX_HOPS) {
            reason = ISAAC_LGC_REASON_HOPS;
            break;
        }
        lgc_pushstring(L, LGC_STR_PARENT);             /* GC point */
        lgc_rawget(L, top + 2);
        parent_type = lgc_type(L, top + 3);
        if (parent_type == LUA_TNIL) {
            reason = ISAAC_LGC_REASON_PARENT;
        } else if (parent_type != LUA_TTABLE) {
            reason = ISAAC_LGC_REASON_PARENT_TYPE;
        } else {
            lgc_copy(L, top + 3, top + 2);
            lgc_settop(L, top + 2);
            ++hops;
            imports += LGC_IMPORTS_GETCLASS_HOP;
        }
    }
    lgc_settop(L, top);
    isaac_vita_lua_seam_leave(c, root);
    return lgc_fallback(ISAAC_LGC_FN_GETCLASS, reason);
}

int isaac_vita_lua_getexact_direct_try(CPU *__restrict c)
{
    lua_State *L;
    int top, root;
    uint32_t class_key, reason, imports;
    void *ud;

    ++s_stats.calls[ISAAC_LGC_FN_GETEXACT];
    lgc_log_banner();
    if (!guest_stack_contains(c, c->esp, 8U))
        return lgc_fallback(ISAAC_LGC_FN_GETEXACT, ISAAC_LGC_REASON_STACK);
    if (!lgc_pins_ok())
        return lgc_fallback(ISAAC_LGC_FN_GETEXACT, ISAAC_LGC_REASON_PINS);
    L = (lua_State *)(uintptr_t)c->ecx;
    class_key = ld32(c->esp + 4U);
    if (!isaac_vita_lua_seam_enter(c, &root))
        return lgc_fallback(ISAAC_LGC_FN_GETEXACT, ISAAC_LGC_REASON_OWNER);

    top = cast_int(L->top - (L->ci->func + 1));   /* lapi.c:167 lua_gettop */
    reason = lgc_direct_prefix(L, top, 1, class_key);
    imports = LGC_IMPORTS_GETEXACT_BASE;
    if (reason == ISAAC_LGC_HANDLED) {
        int match = lgc_rawequal(L, top + 1, top + 2);
        if (!match) {
            lgc_pushstring(L, LGC_STR_CONST);          /* GC point */
            lgc_rawget(L, top + 1);
            match = lgc_rawequal(L, top + 3, top + 2);
            imports += LGC_IMPORTS_GETEXACT_CONST;
            if (!match)
                reason = ISAAC_LGC_REASON_PARENT;
        }
        if (match) {
            lgc_settop(L, top);
            ud = lgc_touserdata(L, 1);
            isaac_vita_lua_seam_leave(c, root);
            lgc_handled(c, ISAAC_LGC_FN_GETEXACT, (uint32_t)(uintptr_t)ud,
                        imports);
            return 1;
        }
    }
    lgc_settop(L, top);
    isaac_vita_lua_seam_leave(c, root);
    return lgc_fallback(ISAAC_LGC_FN_GETEXACT, reason);
}

void isaac_vita_lua_getclass_direct_memo(uint32_t out[ISAAC_LGC_MEMO_COUNT])
{
    memcpy(out, s_memo, sizeof s_memo);
}

void isaac_vita_lua_getclass_direct_memo_flush(void)
{
    memset(s_class_memo, 0, sizeof s_class_memo);
    memset(s_identity_memo, 0, sizeof s_identity_memo);
}
#endif /* ISAAC_VITA_LUA_GETCLASS_DIRECT */

#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP)
void __real_sub_003f8e30(CPU *__restrict c);
void __real_sub_003f8c80(CPU *__restrict c);

#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
/* VERIFY: on the first LGC_VERIFY_FIRST native hits and every LGC_VERIFY_EVERY
 * afterwards, rewind the CPU to its entry state and run the translated body on
 * the same Lua state (a handled call is stack-neutral, so the second run sees
 * the identical arguments), compare eax/esp/Lua top and ship the translated
 * result.  ecx/edx/ebx/ebp/esi/edi are untouched by both paths. */
static uint32_t s_verify_hits;

static void lgc_verify(CPU *__restrict c, unsigned fn, uint32_t entry_esp,
                       uint32_t entry_eax, void (*real)(CPU *__restrict))
{
    lua_State *L = (lua_State *)(uintptr_t)c->ecx;
    uint32_t native_eax, native_esp, real_eax, real_esp;
    int native_top, real_top, match;

    ++s_verify_hits;
    if (s_verify_hits > LGC_VERIFY_FIRST &&
        (s_verify_hits % LGC_VERIFY_EVERY) != 0U)
        return;
    native_eax = c->eax;
    native_esp = c->esp;
    native_top = lua_gettop(L);
    c->esp = entry_esp;
    c->eax = entry_eax;
    real(c);
    real_eax = c->eax;
    real_esp = c->esp;
    real_top = lua_gettop(L);
    match = native_eax == real_eax && native_esp == real_esp &&
            native_top == real_top && !c->fault;
    ++s_stats.verify_runs;
    if (!match)
        ++s_stats.verify_mismatches;
    if (!match || s_verify_hits <= 8U ||
        (s_verify_hits % LGC_VERIFY_EVERY) == 0U) {
        isaac_vita_log("[isaac-lua] getclass VERIFY: fn=%s n=%u native=%08x "
                       "translated=%08x esp=%08x/%08x top=%d/%d result=%s "
                       "mismatches=%u",
                       s_function_names[fn], (unsigned)s_verify_hits,
                       (unsigned)native_eax, (unsigned)real_eax,
                       (unsigned)native_esp, (unsigned)real_esp,
                       native_top, real_top, match ? "MATCH" : "MISMATCH",
                       (unsigned)s_stats.verify_mismatches);
    }
}
#endif

void __wrap_sub_003f8e30(CPU *__restrict c)
{
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
    uint32_t entry_esp = c->esp;
    uint32_t entry_eax = c->eax;
#endif
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    if (isaac_vita_lua_getclass_direct_try(c)) {
#else
    if (isaac_vita_lua_getclass_try(c)) {
#endif
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        lgc_verify(c, ISAAC_LGC_FN_GETCLASS, entry_esp, entry_eax,
                   __real_sub_003f8e30);
#endif
        return;
    }
    __real_sub_003f8e30(c);
}

void __wrap_sub_003f8c80(CPU *__restrict c)
{
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
    uint32_t entry_esp = c->esp;
    uint32_t entry_eax = c->eax;
#endif
#if defined(ISAAC_VITA_LUA_GETCLASS_DIRECT)
    if (isaac_vita_lua_getexact_direct_try(c)) {
#else
    if (isaac_vita_lua_getexact_try(c)) {
#endif
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        lgc_verify(c, ISAAC_LGC_FN_GETEXACT, entry_esp, entry_eax,
                   __real_sub_003f8c80);
#endif
        return;
    }
    __real_sub_003f8c80(c);
}
#endif
