/* 32-bit host oracle for the Lua import scope / callback trampoline fast path
 * (ISAAC_VITA_LUA_SCOPE_FASTPATH, host_vita_lua.c).  Built twice by
 * recomp/test_vita_lua_scope.py (knob 1 and 0); both must pass identically.
 *
 * What it pins (two rounds of script A then script B, so every callback
 * frame slot is reused without the old memset):
 *   - re-entrant scope_enter: imports issued by guest callbacks while the
 *     same CPU owns the outer lua_pcallk scope (depth 1 and 2);
 *   - foreign owner: an import on a second, production-bound CPU while the
 *     first CPU owns the scope is rejected with the CAS-path fault text and
 *     runs no Lua -- once from the owner's own thread (simulation) and once
 *     from a real second thread (the CAS observes the other thread's store),
 *     at callback depth 1 and again at depth 2;
 *   - the slot is released afterwards (the second CPU enters as root);
 *   - callback frame reuse without memset: a 200-byte argerror text, then a
 *     guest fault, then a normal return, then a luaL_error stack raise, then
 *     normal returns, all on the same frame slot -- every error text must be
 *     exactly the one raised, every normal return must return normally;
 *   - hostile: a 300-byte luaL_argerror text (truncated to 255) on slot 0
 *     immediately followed by the 30-byte "translated Lua callback fault"
 *     text on the same slot: the stale tail must not leak;
 *   - hostile: a callee that pops too much and one that pops too little:
 *     both must raise the exact cdecl text and leave the guest stack
 *     restored (the fast path drops only the redundant post-return range
 *     re-check, never the esp comparison);
 *   - hostile: a direct Lua raise (luaL_checkinteger on no argument) inside
 *     a callback at depth 2 is caught by the guest's own nested lua_pcallk
 *     import, which recovers the abandoned trampoline frame and lets the
 *     depth-1 guest activation continue on the x86 stack with the message;
 *   - hostile: 25 recursive callbacks through lua_callk exhaust all 24
 *     frames ("translated Lua callback depth exceeded"), a Lua-level pcall
 *     catches it, the outer pcallk import recovers every abandoned frame and
 *     the guest stack is balanced; the second round proves every slot was
 *     released (the same 24-frame exhaustion happens again);
 *   - hostile: the trampoline entered with no active import scope (native
 *     lua_pcall from the oracle) raises the exact "no active guest CPU"
 *     text -- the relaxed load's empty-slot fallback;
 *   - native getClass/getExact seam owner protocol: root, repeated nested
 *     re-entry, foreign owner (including a real second thread), nested leave
 *     retaining the owner, root release, owner change and null legacy input. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "host_vita_lua.h"
#include "host_vita_startup.h"

#if !defined(ISAAC_VITA_LUA_SCOPE_FASTPATH)
#define ISAAC_VITA_LUA_SCOPE_FASTPATH 1
#endif

enum {
    GUEST_NESTED = 0x00431000U,
    GUEST_ARGERR_LONG = 0x00431010U,
    GUEST_FAULT = 0x00431020U,
    GUEST_CLEARFAULT = 0x00431030U,
    GUEST_ERR_STACK = 0x00431040U,
    GUEST_DEPTH = 0x00431050U,
    GUEST_BLOCK = 0x00431060U,
    GUEST_RECURSE = 0x00431070U,
    GUEST_RAISE_DIRECT = 0x00431080U,
    GUEST_OVER = 0x00431090U,
    GUEST_UNDER = 0x004310a0U,
    GUEST_ARGERR_MAX = 0x004310b0U,
    IMPORT_RETURN = 0xfff17000U,

    LUA_IMPORT_CLOSE = 0U,
    LUA_IMPORT_OPENLIBS = 10U,
    LUA_IMPORT_ARGERROR = 14U,
    LUA_IMPORT_GETTOP = 18U,
    LUA_IMPORT_CALLK = 30U,
    LUA_IMPORT_SETGLOBAL = 37U,
    LUA_IMPORT_NEWSTATE_AUX = 39U,
    LUA_IMPORT_CHECKINTEGER = 40U,
    LUA_IMPORT_ERROR = 41U,
    LUA_IMPORT_PCALLK = 59U,
    LUA_IMPORT_PUSHINTEGER = 60U,
    LUA_IMPORT_PUSHCLOSURE = 61U,

    STACK_WORDS = 8192,
    ROUNDS = 2,
    /* VITA_LUA_MAX_CALLBACK_DEPTH in host_vita_lua.c. */
    CALLBACK_FRAMES = 24,
    /* VITA_LUA_ERROR_TEXT_MAX in host_vita_lua.c. */
    ERROR_TEXT_MAX = 255
};

_Static_assert(sizeof(void *) == 4U,
               "the scope oracle requires the 32-bit bridge ABI");

static const char s_foreign_fault[] =
    "concurrent Lua bridge entry by another CPU";
static const char s_deliberate_fault[] = "oracle deliberate callback fault";
static const char s_fault_text[] = "translated Lua callback fault";
static const char s_cdecl_text[] = "translated Lua callback broke cdecl stack";
/* luaL_where(L, 1) names the Lua caller of the raising C function: line 4
 * of chunk "=scopeB" (rec) for the depth error, line 33 of "=scopeA" for the
 * direct raise; the pcall-invoked raises have a C caller and no prefix. */
static const char s_depth_text[] =
    "scopeB:4: translated Lua callback depth exceeded";
static const char s_noscope_text[] = "Lua callback has no active guest CPU";
static const char s_raise_text[] =
    "scopeA:33: bad argument #1 to 'raise_direct' "
    "(number expected, got no value)";

_Alignas(16) static uint32_t s_stack_a[STACK_WORDS];
_Alignas(16) static uint32_t s_stack_b[STACK_WORDS];
static CPU s_cpu_a;
static CPU s_cpu_b;
static uint32_t s_state;
static HANDLE s_entered;
static HANDLE s_released;
static volatile LONG s_thread_ok;

static unsigned s_nested_calls;
static unsigned s_argerr_calls;
static unsigned s_argerr_max_calls;
static unsigned s_fault_calls;
static unsigned s_clear_calls;
static unsigned s_errstack_calls;
static unsigned s_depth_calls;
static unsigned s_depth_errors;
static unsigned s_block_calls;
static unsigned s_recurse_calls;
static unsigned s_raise_calls;
static unsigned s_over_calls;
static unsigned s_under_calls;
static unsigned s_foreign_rejected;
static unsigned s_thread_rejected;
static unsigned s_released_roots;
static unsigned s_seam_checks;

static char s_nested_name[] = "nested";
static char s_argerr_name[] = "argerr_long";
static char s_argerr_max_name[] = "argerr_max";
static char s_fault_name[] = "faultfn";
static char s_clear_name[] = "clearfault";
static char s_errstack_name[] = "err_stack";
static char s_depth_name[] = "depth";
static char s_block_name[] = "block";
static char s_recurse_name[] = "recurse";
static char s_raise_name[] = "raise_direct";
static char s_over_name[] = "over";
static char s_under_name[] = "under";
static char s_format_s[] = "%s";
static char s_stack_boom[] = "stack boom";
static char s_long_text[201];
static char s_max_text[301];

static const char s_script[] =
    "local rep = string.rep('x', 200)\n"
    "local r = {}\n"
    "r.n1 = nested()\n"
    "local ok1, e1 = pcall(argerr_long)\n"
    "r.ok1 = (ok1 == false)\n"
    "r.e1 = e1\n"
    "local ok2, e2 = pcall(faultfn)\n"
    "r.ok2 = (ok2 == false)\n"
    "r.e2 = e2\n"
    "r.cleared = clearfault()\n"
    "r.n2 = nested(1, 2)\n"
    "local ok3, e3 = pcall(err_stack)\n"
    "r.ok3 = (ok3 == false)\n"
    "r.e3 = e3\n"
    "r.n3 = nested(1, 2, 3)\n"
    "r.d1 = depth(function() return nested(7) end)\n"
    "block()\n"
    "r.n4 = nested(1, 2, 3, 4)\n"
    "local ok5, e5 = pcall(argerr_max)\n"
    "r.ok5 = (ok5 == false)\n"
    "r.e5 = e5\n"
    "local ok6, e6 = pcall(faultfn)\n"
    "r.ok6 = (ok6 == false)\n"
    "r.e6 = e6\n"
    "r.cleared2 = clearfault()\n"
    "local ok7, e7 = pcall(over)\n"
    "r.ok7 = (ok7 == false)\n"
    "r.e7 = e7\n"
    "local ok8, e8 = pcall(under)\n"
    "r.ok8 = (ok8 == false)\n"
    "r.e8 = e8\n"
    "r.n5 = nested(1, 2, 3, 4, 5)\n"
    "r.d2 = depth(function() local v = raise_direct() return v end)\n"
    "r.d3 = depth(function() block() return 3 end)\n"
    "r.n6 = nested(1, 2, 3, 4, 5, 6)\n"
    "r.suffix = '(' .. rep .. ')'\n"
    "result = r\n";

/* Every level enters a new trampoline frame through lua_callk; the 25th
 * entry finds all 24 frames used and raises before setjmp.  Nothing after
 * the pcall may call a guest function: the abandoned frames stay allocated
 * until the outer lua_pcallk import recovers them (by design). */
static const char s_depth_script[] =
    "local hits = 0\n"
    "local function rec(n)\n"
    "  hits = n\n"
    "  recurse(function() rec(n + 1) end)\n"
    "end\n"
    "local ok, e = pcall(rec, 1)\n"
    "result_depth = { ok = (ok == false), e = e, hits = hits }\n";

static uint32_t pointer32(const void *pointer)
{
    if ((uintptr_t)pointer > UINT32_MAX) {
        fprintf(stderr, "oracle pointer does not fit frozen 32-bit ABI\n");
        exit(2);
    }
    return (uint32_t)(uintptr_t)pointer;
}

/* ---- guest runtime surface the bridge links against ------------------- */

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    if (!c->fault) {
        c->fault_addr = address;
        c->fault = what;
    }
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind;
    (void)size;
    guest_fault(c, address, "oracle guest stack violation");
    c->fault_addr = pc ? pc : address;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "oracle guest stack owner violation");
    return 0;
}

void *isaac_vita_guest_malloc(size_t size)
{
    return malloc(size);
}

void *isaac_vita_guest_realloc(void *pointer, size_t size, int *valid_owner)
{
    if (valid_owner)
        *valid_owner = 1;
    return realloc(pointer, size);
}

int isaac_vita_guest_free(void *pointer)
{
    free(pointer);
    return 1;
}

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    (void)native_path;
    (void)capacity;
    guest_fault(c, guest_path, "oracle does not map paths");
    return 0;
}

static void guest_nested(CPU *__restrict c);
static void guest_argerr_long(CPU *__restrict c);
static void guest_argerr_max(CPU *__restrict c);
static void guest_faultfn(CPU *__restrict c);
static void guest_clearfault(CPU *__restrict c);
static void guest_err_stack(CPU *__restrict c);
static void guest_depth(CPU *__restrict c);
static void guest_block(CPU *__restrict c);
static void guest_recurse(CPU *__restrict c);
static void guest_raise_direct(CPU *__restrict c);
static void guest_over(CPU *__restrict c);
static void guest_under(CPU *__restrict c);

void guest_call(CPU *__restrict c, uint32_t address)
{
    switch (address) {
    case GUEST_NESTED: guest_nested(c); return;
    case GUEST_ARGERR_LONG: guest_argerr_long(c); return;
    case GUEST_ARGERR_MAX: guest_argerr_max(c); return;
    case GUEST_FAULT: guest_faultfn(c); return;
    case GUEST_CLEARFAULT: guest_clearfault(c); return;
    case GUEST_ERR_STACK: guest_err_stack(c); return;
    case GUEST_DEPTH: guest_depth(c); return;
    case GUEST_BLOCK: guest_block(c); return;
    case GUEST_RECURSE: guest_recurse(c); return;
    case GUEST_RAISE_DIRECT: guest_raise_direct(c); return;
    case GUEST_OVER: guest_over(c); return;
    case GUEST_UNDER: guest_under(c); return;
    default:
        guest_fault(c, address, "oracle guest_call target is not registered");
        return;
    }
}

/* ---- import driver ---------------------------------------------------- */

static void bind_cpu(CPU *c, uint32_t *stack)
{
    memset(c, 0, sizeof *c);
    /* Bound like production guest_stack_bind so the guarded stack checks
     * (owner + range) run on every push/pop/argument read. */
    c->stack_owner = c;
    c->stack_floor = pointer32(stack);
    c->stack_ceiling = pointer32(stack + STACK_WORDS);
    c->stack_low_water = c->stack_ceiling;
    c->esp = c->stack_ceiling;
    c->ebp = c->esp;
}

static int invoke_import(CPU *c, uint32_t index,
                         const uint32_t *arguments, uint32_t count)
{
    uint32_t saved = c->esp;
    uint32_t i;
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, IMPORT_RETURN + index);
    if (!isaac_vita_lua_import_indexed(c, index, NULL)) {
        guest_fault(c, index, "oracle Lua import index was rejected");
        return 0;
    }
    if (c->fault)
        return 0;
    if (c->esp != saved - count * 4U) {
        guest_fault(c, c->esp, "Lua import broke x86 cdecl stack");
        return 0;
    }
    return guest_stack_adjust(c, count * 4U, index);
}

static int import_pushinteger(CPU *c, uint32_t state, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    uint32_t arguments[] = {
        state, (uint32_t)bits, (uint32_t)(bits >> 32U)
    };
    return invoke_import(c, LUA_IMPORT_PUSHINTEGER, arguments, 3U);
}

static int register_closure(CPU *c, uint32_t state, uint32_t target,
                            const char *name)
{
    uint32_t closure[] = { state, target, 0U };
    uint32_t global[] = { state, pointer32(name) };
    return invoke_import(c, LUA_IMPORT_PUSHCLOSURE, closure, 3U) &&
           invoke_import(c, LUA_IMPORT_SETGLOBAL, global, 2U);
}

/* An import on a CPU that does not own the active scope: the handler must
 * fault with the CAS-path text before touching Lua, leaving its return word
 * (and the arguments) on that CPU's stack. */
static int probe_foreign(CPU *foreign, uint32_t state)
{
    uint32_t saved = foreign->esp;
    uint32_t return_word = (uint32_t)IMPORT_RETURN + (uint32_t)LUA_IMPORT_GETTOP;
    int ok;
    gpush(foreign, state);
    gpush(foreign, return_word);
    if (!isaac_vita_lua_import_indexed(foreign, LUA_IMPORT_GETTOP, NULL))
        return 0;
    ok = foreign->fault != NULL &&
         strcmp(foreign->fault, s_foreign_fault) == 0 &&
         foreign->fault_addr == 0U &&
         foreign->esp == saved - 8U &&
         ld32(foreign->esp) == return_word;
    foreign->fault = NULL;
    foreign->fault_addr = 0U;
    (void)guest_stack_set(foreign, saved, 0U);
    return ok;
}

static DWORD WINAPI foreign_thread(LPVOID parameter)
{
    (void)parameter;
    if (WaitForSingleObject(s_entered, 30000) != WAIT_OBJECT_0)
        return 1U;
    if (probe_foreign(&s_cpu_b, s_state)) {
        ++s_thread_rejected;
        InterlockedExchange(&s_thread_ok, 1);
    }
    SetEvent(s_released);
    return 0U;
}

/* ---- guest CFunctions (cdecl: [esp] return word, [esp+4] lua_State) --- */

/* nested(...) -> lua_gettop through the nested import (re-entrant scope). */
static void guest_nested(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state };
    int32_t top;
    ++s_nested_calls;
    if (!invoke_import(c, LUA_IMPORT_GETTOP, arguments, 1U))
        return;
    top = (int32_t)c->eax;
    if (!import_pushinteger(c, state, top))
        return;
    c->eax = 1U;
    (void)gpop(c);
}

/* luaL_argerror(L, 1, <200 bytes>) -> ARGERROR escape, error_text filled. */
static void guest_argerr_long(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, 1U, pointer32(s_long_text) };
    ++s_argerr_calls;
    (void)invoke_import(c, LUA_IMPORT_ARGERROR, arguments, 3U);
    guest_fault(c, GUEST_ARGERR_LONG, "luaL_argerror unexpectedly returned");
}

/* luaL_argerror(L, 2, <300 bytes>) -> the bridge copies at most 255 bytes;
 * the frame's error_text is now full to the last byte for the next user of
 * the slot. */
static void guest_argerr_max(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, 2U, pointer32(s_max_text) };
    ++s_argerr_max_calls;
    (void)invoke_import(c, LUA_IMPORT_ARGERROR, arguments, 3U);
    guest_fault(c, GUEST_ARGERR_MAX, "luaL_argerror unexpectedly returned");
}

/* A guest fault inside the callback -> "translated Lua callback fault". */
static void guest_faultfn(CPU *__restrict c)
{
    ++s_fault_calls;
    guest_fault(c, GUEST_FAULT, s_deliberate_fault);
    (void)gpop(c);
}

/* Production guest_fault never returns; the recording oracle must clear the
 * deliberate fault once the trampoline has turned it into a Lua error. */
static void guest_clearfault(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    int cleared = c->fault != NULL && strcmp(c->fault, s_deliberate_fault) == 0;
    ++s_clear_calls;
    c->fault = NULL;
    c->fault_addr = 0U;
    lua_pushboolean((lua_State *)(uintptr_t)state, cleared);
    c->eax = 1U;
    (void)gpop(c);
}

/* luaL_error(L, "%s", "stack boom") -> LUAL_ERROR_STACK escape (error_text
 * unused). */
static void guest_err_stack(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = {
        state, pointer32(s_format_s), pointer32(s_stack_boom)
    };
    ++s_errstack_calls;
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments, 3U);
    guest_fault(c, GUEST_ERR_STACK, "luaL_error unexpectedly returned");
}

/* depth(f) -> lua_pcallk(L, 0, 1, 0, 0, NULL) on f: a second callback frame
 * (slot 1) and a third scope level while slot 0 is live.  Returns the one
 * result; on an error the message occupies the same slot (luaD_seterrorobj)
 * and is returned instead -- the guest activation continues on the x86 stack
 * after the nested import recovered the abandoned frames above it. */
static void guest_depth(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, 0U, 1U, 0U, 0U, 0U };
    ++s_depth_calls;
    if (!invoke_import(c, LUA_IMPORT_PCALLK, arguments, 6U))
        return;
    if (c->eax != (uint32_t)LUA_OK)
        ++s_depth_errors;
    c->eax = 1U;
    (void)gpop(c);
}

/* block(): while this CPU owns the scope, a foreign CPU is rejected from
 * this thread and from a second thread; afterwards the owner is still
 * re-entrant. */
static void guest_block(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state };
    HANDLE thread;
    ++s_block_calls;
    InterlockedExchange(&s_thread_ok, 0);
    if (!probe_foreign(&s_cpu_b, state)) {
        guest_fault(c, GUEST_BLOCK, "same-thread foreign CPU was not rejected");
        return;
    }
    ++s_foreign_rejected;
    thread = CreateThread(NULL, 0U, foreign_thread, NULL, 0U, NULL);
    if (!thread) {
        guest_fault(c, GUEST_BLOCK, "CreateThread failed");
        return;
    }
    SetEvent(s_entered);
    if (WaitForSingleObject(s_released, 30000) != WAIT_OBJECT_0) {
        guest_fault(c, GUEST_BLOCK, "foreign thread did not finish");
        return;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (!s_thread_ok) {
        guest_fault(c, GUEST_BLOCK, "second-thread foreign CPU was not rejected");
        return;
    }
    ++s_foreign_rejected;
    if (!invoke_import(c, LUA_IMPORT_GETTOP, arguments, 1U))
        return;
    c->eax = 0U;
    (void)gpop(c);
}

/* recurse(f) -> lua_callk(L, 0, 0, 0, NULL) on f (unprotected): the Lua
 * error raised deeper propagates through this import and this activation
 * (both abandoned by Lua's longjmp) to the nearest Lua-level pcall. */
static void guest_recurse(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, 0U, 0U, 0U, 0U };
    ++s_recurse_calls;
    if (!invoke_import(c, LUA_IMPORT_CALLK, arguments, 5U))
        return;
    c->eax = 0U;
    (void)gpop(c);
}

/* raise_direct() -> luaL_checkinteger(L, 1) with no argument: vanilla
 * raises through Lua's own longjmp (no bridge escape), skipping the nested
 * import's scope_leave and this trampoline frame. */
static void guest_raise_direct(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, 1U };
    ++s_raise_calls;
    (void)invoke_import(c, LUA_IMPORT_CHECKINTEGER, arguments, 2U);
    guest_fault(c, GUEST_RAISE_DIRECT,
                "luaL_checkinteger unexpectedly returned");
}

/* over(): pops the caller's lua_State word as well (esp == saved_esp). */
static void guest_over(CPU *__restrict c)
{
    ++s_over_calls;
    (void)gpop(c);
    (void)gpop(c);
    c->eax = 0U;
}

/* under(): returns without popping the return word (esp == saved_esp - 8). */
static void guest_under(CPU *__restrict c)
{
    ++s_under_calls;
    c->eax = 0U;
}

/* ---- checks ------------------------------------------------------------ */

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s", message);
    if (s_cpu_a.fault)
        fprintf(stderr, " (cpu A fault 0x%08x: %s)", s_cpu_a.fault_addr,
                s_cpu_a.fault);
    if (s_cpu_b.fault)
        fprintf(stderr, " (cpu B fault 0x%08x: %s)", s_cpu_b.fault_addr,
                s_cpu_b.fault);
    fputc('\n', stderr);
    return 1;
}

static int oracle_panic(lua_State *state)
{
    fprintf(stderr, "Lua panic: %s\n", lua_tostring(state, -1));
    exit(2);
}

static int field_integer(lua_State *L, const char *name, lua_Integer expected)
{
    int ok;
    lua_getfield(L, -1, name);
    ok = lua_isinteger(L, -1) && lua_tointeger(L, -1) == expected;
    lua_pop(L, 1);
    return ok;
}

static int field_true(lua_State *L, const char *name)
{
    int ok;
    lua_getfield(L, -1, name);
    ok = lua_isboolean(L, -1) && lua_toboolean(L, -1);
    lua_pop(L, 1);
    return ok;
}

static int field_string(lua_State *L, const char *name, const char *expected)
{
    int ok;
    lua_getfield(L, -1, name);
    ok = lua_type(L, -1) == LUA_TSTRING &&
         strcmp(lua_tostring(L, -1), expected) == 0;
    if (!ok)
        fprintf(stderr, "field %s: %s\n", name,
                lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                               : luaL_typename(L, -1));
    lua_pop(L, 1);
    return ok;
}

/* "bad argument #<argument> to '<name>' (<count> x 'x')" byte for byte. */
static int field_argerror(lua_State *L, const char *field, int argument,
                          const char *name, size_t count)
{
    char expected[400];
    int n = snprintf(expected, sizeof expected, "bad argument #%d to '%s' (",
                     argument, name);
    if (n < 0 || (size_t)n + count + 2U > sizeof expected)
        return 0;
    memset(expected + n, 'x', count);
    expected[(size_t)n + count] = ')';
    expected[(size_t)n + count + 1U] = '\0';
    return field_string(L, field, expected);
}

static int run_script(CPU *c, uint32_t state, const char *script,
                      const char *chunk_name)
{
    lua_State *L = (lua_State *)(uintptr_t)state;
    uint32_t pcall_arguments[6];
    if (luaL_loadbuffer(L, script, strlen(script), chunk_name) != LUA_OK)
        return fail(lua_tostring(L, -1));
    pcall_arguments[0] = state;
    pcall_arguments[1] = 0U;
    pcall_arguments[2] = 0U;
    pcall_arguments[3] = 0U;
    pcall_arguments[4] = 0U;
    pcall_arguments[5] = 0U;
    if (!invoke_import(c, LUA_IMPORT_PCALLK, pcall_arguments, 6U))
        return fail("lua_pcallk bridge failed");
    if (c->eax != (uint32_t)LUA_OK) {
        fprintf(stderr, "script error: %s\n", lua_tostring(L, -1));
        return fail("script did not run to completion");
    }
    if (s_cpu_a.fault || s_cpu_b.fault)
        return fail("a CPU fault survived the script");
    if (s_cpu_a.esp != s_cpu_a.stack_ceiling ||
        s_cpu_b.esp != s_cpu_b.stack_ceiling)
        return fail("guest stacks are not balanced");
    return 0;
}

/* No scope is active between imports: the foreign CPU enters as root (and
 * leaves again) -- the slot is free, not stuck -- and so does the owner. */
static int check_released(uint32_t state)
{
    uint32_t gettop_arguments[1];
    gettop_arguments[0] = state;
    if (!invoke_import(&s_cpu_b, LUA_IMPORT_GETTOP, gettop_arguments, 1U) ||
        s_cpu_b.eax != 0U)
        return fail("idle-slot import on the second CPU failed");
    if (!invoke_import(&s_cpu_a, LUA_IMPORT_GETTOP, gettop_arguments, 1U) ||
        s_cpu_a.eax != 0U)
        return fail("idle-slot import on the first CPU failed");
    ++s_released_roots;
    return 0;
}

static int check_round(lua_State *L)
{
    lua_getglobal(L, "result");
    if (!lua_istable(L, -1))
        return fail("result table missing");
    if (!field_integer(L, "n1", 0) || !field_integer(L, "n2", 2) ||
        !field_integer(L, "n3", 3) || !field_integer(L, "n4", 4) ||
        !field_integer(L, "n5", 5) || !field_integer(L, "n6", 6) ||
        !field_integer(L, "d1", 1) || !field_integer(L, "d3", 3))
        return fail("nested/depth callbacks did not return normally");
    if (!field_true(L, "ok1") || !field_true(L, "ok2") ||
        !field_true(L, "ok3") || !field_true(L, "ok5") ||
        !field_true(L, "ok6") || !field_true(L, "ok7") ||
        !field_true(L, "ok8") || !field_true(L, "cleared") ||
        !field_true(L, "cleared2"))
        return fail("error callbacks did not raise / fault was not cleared");
    if (!field_argerror(L, "e1", 1, s_argerr_name, 200U))
        return fail("200-byte luaL_argerror text was not reproduced");
    if (!field_argerror(L, "e5", 2, s_argerr_max_name, (size_t)ERROR_TEXT_MAX))
        return fail("300-byte luaL_argerror text was not truncated to 255");
    if (!field_string(L, "e2", s_fault_text))
        return fail("callback fault text changed");
    if (!field_string(L, "e6", s_fault_text))
        return fail("callback fault text after a full error_text leaked "
                    "the stale tail");
    if (!field_string(L, "e3", "stack boom"))
        return fail("luaL_error stack raise text changed");
    if (!field_string(L, "e7", s_cdecl_text))
        return fail("over-popping callee was not reported");
    if (!field_string(L, "e8", s_cdecl_text))
        return fail("under-popping callee was not reported");
    if (!field_string(L, "d2", s_raise_text))
        return fail("direct Lua raise at depth 2 was not recovered by the "
                    "nested lua_pcallk");
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setglobal(L, "result");
    return 0;
}

static int check_depth_round(lua_State *L)
{
    lua_getglobal(L, "result_depth");
    if (!lua_istable(L, -1))
        return fail("result_depth table missing");
    if (!field_true(L, "ok"))
        return fail("depth exhaustion did not raise");
    if (!field_string(L, "e", s_depth_text))
        return fail("depth exhaustion text changed");
    if (!field_integer(L, "hits", CALLBACK_FRAMES + 1))
        return fail("depth exhaustion happened at the wrong frame count "
                    "(a frame slot leaked from the previous round?)");
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setglobal(L, "result_depth");
    return 0;
}

/* The trampoline entered with no import scope active (native lua_pcall from
 * the oracle itself): the exact empty-slot diagnostic, no frame pushed. */
static int check_no_scope(lua_State *L)
{
    int status;
    lua_getglobal(L, "nested");
    status = lua_pcall(L, 0, 1, 0);
    if (status != LUA_ERRRUN || lua_type(L, -1) != LUA_TSTRING ||
        strcmp(lua_tostring(L, -1), s_noscope_text) != 0) {
        fprintf(stderr, "no-scope status %d: %s\n", status,
                lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?");
        return fail("trampoline without an active scope did not raise the "
                    "exact diagnostic");
    }
    lua_pop(L, 1);
    if (lua_gettop(L) != 0)
        return fail("no-scope probe left values on the Lua stack");
    return 0;
}

typedef struct seam_thread_probe {
    int entered;
    int root;
} seam_thread_probe;

static DWORD WINAPI seam_foreign_thread(LPVOID argument)
{
    seam_thread_probe *probe = (seam_thread_probe *)argument;
    probe->root = -1;
    probe->entered = isaac_vita_lua_seam_enter(&s_cpu_b, &probe->root);
    if (probe->entered)
        isaac_vita_lua_seam_leave(&s_cpu_b, probe->root);
    return 0;
}

/* Exercise the actual exported production seam, not a replay of its CAS or
 * relaxed-read algorithm. No Lua operation/finalizer is replaced by this test.
 * The same cases must pass with ISAAC_VITA_LUA_SCOPE_FASTPATH both ON and OFF. */
static int check_seam_scope(void)
{
    int root_a, root_b, nested, null_root;
    unsigned iteration;
    HANDLE thread;
    seam_thread_probe probe = { -1, -1 };

#define SEAM_CHECK(condition, message) do { \
    if (!(condition)) return fail(message); \
    ++s_seam_checks; \
} while (0)

    SEAM_CHECK(isaac_vita_lua_seam_enter(NULL, &null_root) && null_root == 1,
               "null seam root changed its legacy CAS(0, 0) result");
    isaac_vita_lua_seam_leave(NULL, null_root);
    SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_a, &root_a) && root_a == 1,
               "native seam root did not acquire empty owner");
    SEAM_CHECK(!isaac_vita_lua_seam_enter(NULL, &null_root) && null_root == 0,
               "null seam input did not reject a foreign owner");
    for (iteration = 0U; iteration < 4096U; ++iteration) {
        SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_a, &nested) && nested == 0,
                   "native seam nested entry claimed root ownership");
        isaac_vita_lua_seam_leave(&s_cpu_a, nested);
    }
    SEAM_CHECK(!isaac_vita_lua_seam_enter(&s_cpu_b, &root_b) &&
               root_b == 0 && !s_cpu_b.fault,
               "native seam foreign entry must fall back without faulting");
    thread = CreateThread(NULL, 0U, seam_foreign_thread, &probe, 0U, NULL);
    SEAM_CHECK(thread != NULL, "native seam foreign thread creation failed");
    SEAM_CHECK(WaitForSingleObject(thread, 5000U) == WAIT_OBJECT_0,
               "native seam foreign thread did not finish");
    CloseHandle(thread);
    SEAM_CHECK(probe.entered == 0 && probe.root == 0 && !s_cpu_b.fault,
               "native seam failed the actual second-thread foreign probe");
    isaac_vita_lua_seam_leave(&s_cpu_a, root_a);
    SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_b, &root_b) && root_b == 1,
               "native seam root release left the owner held");
    isaac_vita_lua_seam_leave(&s_cpu_b, root_b);

    SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_a, &root_a) && root_a == 1,
               "native seam owner-change probe could not acquire root");
    isaac_vita_lua_abort_cpu(&s_cpu_a);
    SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_b, &root_b) && root_b == 1,
               "native seam owner-change probe could not replace owner");
    isaac_vita_lua_seam_leave(&s_cpu_a, root_a);
    SEAM_CHECK(s_cpu_a.fault &&
               strcmp(s_cpu_a.fault, "Lua bridge owner changed during call") == 0,
               "native seam changed the owner-loss diagnostic");
    SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_b, &nested) && nested == 0,
               "failed native seam release cleared the replacement owner");
    isaac_vita_lua_seam_leave(&s_cpu_b, nested);
    isaac_vita_lua_seam_leave(&s_cpu_b, root_b);
    s_cpu_a.fault = NULL;
    s_cpu_a.fault_addr = 0U;
    SEAM_CHECK(isaac_vita_lua_seam_enter(&s_cpu_a, &root_a) && root_a == 1,
               "native seam could not recover after owner loss");
    isaac_vita_lua_seam_leave(&s_cpu_a, root_a);
    SEAM_CHECK(!s_cpu_a.fault && !s_cpu_b.fault,
               "native seam probe left an unexpected guest fault");
#undef SEAM_CHECK
    return 0;
}

int main(void)
{
    lua_State *L;
    uint32_t newstate_arguments[1];
    uint32_t openlibs_arguments[1];
    uint32_t close_arguments[1];
    unsigned round;

    memset(s_long_text, 'x', sizeof s_long_text - 1U);
    s_long_text[sizeof s_long_text - 1U] = '\0';
    memset(s_max_text, 'x', sizeof s_max_text - 1U);
    s_max_text[sizeof s_max_text - 1U] = '\0';
    bind_cpu(&s_cpu_a, s_stack_a);
    bind_cpu(&s_cpu_b, s_stack_b);
    if (check_seam_scope())
        return 1;
    s_entered = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_released = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_entered || !s_released)
        return fail("CreateEvent failed");

    newstate_arguments[0] = 0U;
    if (!invoke_import(&s_cpu_a, LUA_IMPORT_NEWSTATE_AUX,
                       newstate_arguments, 0U) || !s_cpu_a.eax)
        return fail("luaL_newstate bridge failed");
    s_state = s_cpu_a.eax;
    L = (lua_State *)(uintptr_t)s_state;
    (void)lua_atpanic(L, oracle_panic);
    openlibs_arguments[0] = s_state;
    if (!invoke_import(&s_cpu_a, LUA_IMPORT_OPENLIBS, openlibs_arguments, 1U))
        return fail("luaL_openlibs bridge failed");
    if (!register_closure(&s_cpu_a, s_state, GUEST_NESTED, s_nested_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_ARGERR_LONG,
                          s_argerr_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_ARGERR_MAX,
                          s_argerr_max_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_FAULT, s_fault_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_CLEARFAULT,
                          s_clear_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_ERR_STACK,
                          s_errstack_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_DEPTH, s_depth_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_BLOCK, s_block_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_RECURSE,
                          s_recurse_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_RAISE_DIRECT,
                          s_raise_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_OVER, s_over_name) ||
        !register_closure(&s_cpu_a, s_state, GUEST_UNDER, s_under_name))
        return fail("guest CFunction registration failed");

    if (check_released(s_state))
        return 1;
    if (check_no_scope(L))
        return 1;

    for (round = 0U; round < ROUNDS; ++round) {
        if (run_script(&s_cpu_a, s_state, s_script, "=scopeA") || check_round(L) ||
            check_released(s_state))
            return 1;
        if (run_script(&s_cpu_a, s_state, s_depth_script, "=scopeB") ||
            check_depth_round(L) || check_released(s_state))
            return 1;
        if (check_no_scope(L))
            return 1;
    }

    if (s_nested_calls != 7U * ROUNDS || s_argerr_calls != ROUNDS ||
        s_argerr_max_calls != ROUNDS || s_fault_calls != 2U * ROUNDS ||
        s_clear_calls != 2U * ROUNDS || s_errstack_calls != ROUNDS ||
        s_depth_calls != 3U * ROUNDS || s_depth_errors != ROUNDS ||
        s_block_calls != 2U * ROUNDS || s_foreign_rejected != 4U * ROUNDS ||
        s_thread_rejected != 2U * ROUNDS || s_over_calls != ROUNDS ||
        s_under_calls != ROUNDS || s_raise_calls != ROUNDS ||
        s_recurse_calls != CALLBACK_FRAMES * ROUNDS ||
        s_released_roots != 1U + 2U * ROUNDS) {
        fprintf(stderr,
                "census: nested=%u argerr=%u argerr_max=%u fault=%u clear=%u "
                "errstack=%u depth=%u(err=%u) block=%u foreign=%u thread=%u "
                "over=%u under=%u raise=%u recurse=%u released=%u\n",
                s_nested_calls, s_argerr_calls, s_argerr_max_calls,
                s_fault_calls, s_clear_calls, s_errstack_calls, s_depth_calls,
                s_depth_errors, s_block_calls, s_foreign_rejected,
                s_thread_rejected, s_over_calls, s_under_calls, s_raise_calls,
                s_recurse_calls, s_released_roots);
        return fail("callback census mismatch");
    }

    close_arguments[0] = s_state;
    if (!invoke_import(&s_cpu_a, LUA_IMPORT_CLOSE, close_arguments, 1U))
        return fail("lua_close bridge failed");
    CloseHandle(s_entered);
    CloseHandle(s_released);
    printf("Lua scope oracle fastpath=%d: rounds=%u nested=%u depth=%u "
           "(recovered_direct_raise=%u) argerr=%u+%u(255-cap) fault=%u "
           "errstack=%u cdecl_broken=%u+%u depth_exhausted=%ux%u frames "
           "noscope=%u foreign_rejected=%u (thread=%u) released=%u "
           "seam_checks=%u: PASS\n",
           (int)ISAAC_VITA_LUA_SCOPE_FASTPATH, (unsigned)ROUNDS,
           s_nested_calls, s_depth_calls, s_depth_errors, s_argerr_calls,
           s_argerr_max_calls, s_fault_calls, s_errstack_calls, s_over_calls,
           s_under_calls, (unsigned)ROUNDS, (unsigned)CALLBACK_FRAMES,
           (unsigned)(1U + ROUNDS), s_foreign_rejected, s_thread_rejected,
           s_released_roots, s_seam_checks);
    return 0;
}
