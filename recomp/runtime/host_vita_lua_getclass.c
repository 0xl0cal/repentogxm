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

#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS)
#error The native getClass seam must only be compiled for its opt-in feature
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
        "parent,ptype,hops)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
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
        (unsigned)s_stats.reasons[11], (unsigned)s_stats.reasons[12]);
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
    if (isaac_vita_lua_getclass_try(c)) {
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
    if (isaac_vita_lua_getexact_try(c)) {
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        lgc_verify(c, ISAAC_LGC_FN_GETEXACT, entry_esp, entry_eax,
                   __real_sub_003f8c80);
#endif
        return;
    }
    __real_sub_003f8c80(c);
}
#endif
