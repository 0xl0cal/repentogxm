/* Execute the generated frozen _RunCallback wrapper against the production
 * 32-bit Lua bridge.  The companion Python test emits sub_0040eef0 directly
 * from the authenticated PE; this file supplies only the native Lua state,
 * exact IAT endpoints, project sentinel setup and bounded error oracles. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "host_vita_lua.h"
#include "host_vita_startup.h"

#define IMPORT_RETURN UINT32_C(0xfff16100)
#define IMPORT_TOKEN_BASE UINT32_C(0xffe20000)
#define ENGINE_RETURN UINT32_C(0xfff040ee)

enum {
    GUEST_FAIL = 0x00421100U,
    GUEST_BADCHECK = 0x00421110U,

    LUA_IMPORT_CLOSE = 0U,
    LUA_IMPORT_OPENLIBS = 10U,
    LUA_IMPORT_GETTOP = 18U,
    LUA_IMPORT_GETGLOBAL = 27U,
    LUA_IMPORT_SETGLOBAL = 37U,
    LUA_IMPORT_NEWSTATE_AUX = 39U,
    LUA_IMPORT_CHECKINTEGER = 40U,
    LUA_IMPORT_ERROR = 41U,
    LUA_IMPORT_PCALLK = 59U,
    LUA_IMPORT_PUSHINTEGER = 60U,
    LUA_IMPORT_PUSHCLOSURE = 61U,
    LUA_IMPORT_RAWGETI = 63U,
    LUA_IMPORT_REF = 65U,
    LUA_IMPORT_PUSHVALUE = 66U,

    FROZEN_IAT_PAGE = 0x30600000U,
    FROZEN_IAT_PAGE_SIZE = 0x00010000U,
    FROZEN_IAT_SETTOP = 0x30606170U,
    FROZEN_IAT_PCALLK = 0x3060623cU,
    FROZEN_IAT_PUSHINTEGER = 0x30606240U,
    FROZEN_IAT_RAWGETI = 0x3060624cU,
    FROZEN_IAT_REF = 0x30606254U,
    FROZEN_IAT_PUSHVALUE = 0x30606258U,

    ENGINE_ERROR_HELPER_RETURN = 0x0040ef88U,
    ENGINE_THROW_RETURN = 0x0040ef8dU,
    ENGINE_THROW_INT3 = 0x0040ef8dU,
    ERROR_REPETITIONS = 25U,
    SENTINEL_POST_UPDATE = 1,
    SENTINEL_POST_RENDER = 2,
    ORACLE_LUAL_ERROR_CALLBACK = 9001,
    ORACLE_CHECK_ERROR_CALLBACK = 9002
};

typedef struct frozen_run_callback_registry {
    uint32_t state;
    int32_t key;
} frozen_run_callback_registry;

typedef struct frozen_run_callback_call {
    uint32_t registry;
    int32_t callback_id;
    int32_t parameter;
} frozen_run_callback_call;

typedef struct frozen_lua_bridge_ref {
    uint32_t state;
    int32_t key;
} frozen_lua_bridge_ref;

_Static_assert(sizeof(void *) == 4U,
               "the callback dispatcher oracle requires the 32-bit ABI");
_Static_assert(sizeof(frozen_run_callback_registry) == 8U,
               "frozen RunCallbackRegistry layout changed");
_Static_assert(sizeof(frozen_run_callback_call) == 12U,
               "frozen _RunCallback call object layout changed");
_Static_assert(sizeof(frozen_lua_bridge_ref) == 8U,
               "frozen LuaBridgeRef layout changed");

void sub_0040eef0(CPU *__restrict c);

static _Alignas(16) uint32_t s_guest_stacks[2][8192];
static unsigned s_guest_fail_calls;
static unsigned s_guest_badcheck_calls;
static unsigned s_error_helper_calls;
static unsigned s_throw_calls;
static unsigned s_int3_calls;
static unsigned s_top_level_markers;
static unsigned s_update_markers;
static unsigned s_render_markers;
static unsigned s_expected_error_kind;
static const char *s_oracle_failure;
static uint32_t s_expected_state;

unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;

static char s_run_callback_name[] = "_RunCallback";
static char s_fail_name[] = "__repentogxm_oracle_fail";
static char s_badcheck_name[] = "__repentogxm_oracle_badcheck";
static char s_guest_error[] = "dispatch boom";

static uint32_t pointer32(const void *pointer)
{
    if ((uintptr_t)pointer > UINT32_MAX) {
        fprintf(stderr, "oracle pointer does not fit frozen 32-bit ABI\n");
        exit(2);
    }
    return (uint32_t)(uintptr_t)pointer;
}

static void remember_failure(const char *message)
{
    if (!s_oracle_failure)
        s_oracle_failure = message;
}

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
    guest_fault(c, address, "callback oracle guest stack violation");
    c->fault_addr = pc ? pc : address;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "callback oracle guest stack owner violation");
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
    guest_fault(c, guest_path,
                "callback oracle unexpectedly crossed the path mapper");
    return 0;
}

/* The production import-ID binder (host_vita_import_id.c) hands a Lua ID to
 * its typed endpoint when ISAAC_VITA_LUA_IMPORT_FASTDISPATCH bound one and to
 * the generic family entry otherwise; the oracle dispatches the same way so
 * the knob build runs the frozen _RunCallback through the typed endpoints
 * (rawgeti/pushvalue/settop) and the generic ones (pcallk/luaL_ref). */
static int bridge_import(CPU *c, uint32_t index)
{
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
    guest_import_family_fn fast = isaac_vita_lua_import_fast_binding(index);
    if (fast)
        return fast(c, index, NULL);
#endif
    return isaac_vita_lua_import_indexed(c, index, NULL);
}

static int invoke_import(CPU *c, uint32_t index,
                         const uint32_t *arguments, uint32_t count)
{
    uint32_t saved = c->esp;
    uint32_t i;
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, IMPORT_RETURN + index);
    if (!bridge_import(c, index)) {
        guest_fault(c, index, "callback oracle Lua import was rejected");
        return 0;
    }
    if (c->fault)
        return 0;
    if (c->esp != saved - count * 4U) {
        guest_fault(c, c->esp, "callback oracle import broke cdecl stack");
        return 0;
    }
    return guest_stack_adjust(c, count * 4U, index);
}

static int64_t import_checkinteger(CPU *c, uint32_t state, int index)
{
    uint32_t arguments[] = { state, (uint32_t)index };
    if (!invoke_import(c, LUA_IMPORT_CHECKINTEGER, arguments, 2U))
        return 0;
    return (int64_t)((uint64_t)c->eax | ((uint64_t)c->edx << 32U));
}

static void guest_fail(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, pointer32(s_guest_error) };
    ++s_guest_fail_calls;
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments, 2U);
    guest_fault(c, GUEST_FAIL, "luaL_error unexpectedly returned");
}

static void guest_badcheck(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    ++s_guest_badcheck_calls;
    (void)import_checkinteger(c, state, 99);
    guest_fault(c, GUEST_BADCHECK,
                "luaL_checkinteger unexpectedly returned");
}

void guest_call(CPU *__restrict c, uint32_t address)
{
    if (address >= IMPORT_TOKEN_BASE &&
        address < IMPORT_TOKEN_BASE + ISAAC_VITA_LUA_IMPORT_COUNT) {
        uint32_t index = address - IMPORT_TOKEN_BASE;
        if (!bridge_import(c, index))
            guest_fault(c, address, "frozen Lua IAT token was rejected");
        return;
    }
    if (address == GUEST_FAIL) {
        guest_fail(c);
        return;
    }
    if (address == GUEST_BADCHECK) {
        guest_badcheck(c);
        return;
    }
    guest_fault(c, address,
                "callback oracle guest_call target is not registered");
}

/* The native error branch constructs and throws a C++ exception.  The host
 * oracle stops at those exact two helper entries: it validates the Lua error
 * object before a real throw would unwind the generated activation, then lets
 * the frozen INT3 tail provide the expected diagnostic stop. */
void sub_003f88d0(CPU *__restrict c)
{
    uint32_t state;
    const char *message;

    ++s_error_helper_calls;
    if (ld32(c->esp) != ENGINE_ERROR_HELPER_RETURN)
        remember_failure("_RunCallback error-helper return pin changed");
    state = ld32(c->esp + 4U);
    if (state != s_expected_state)
        remember_failure("_RunCallback error helper received another state");
    message = lua_tostring((lua_State *)(uintptr_t)state, -1);
    if (!message) {
        remember_failure("_RunCallback error helper received no Lua error");
    } else if (s_expected_error_kind == 1U) {
        size_t actual = strlen(message);
        size_t expected = sizeof s_guest_error - 1U;
        /* luaL_error is required to preserve our literal, but may prepend the
         * current Lua source/line before the protected call returns it. */
        if (actual < expected ||
            memcmp(message + actual - expected, s_guest_error, expected) != 0)
            remember_failure("luaL_error text changed before engine throw");
    } else if (s_expected_error_kind == 2U) {
        if (!strstr(message, "bad argument #99"))
            remember_failure("direct Lua check longjmp text changed");
    } else {
        remember_failure("_RunCallback entered an unexpected error branch");
    }
    (void)gpop(c);
}

void sub_0040ed40(CPU *__restrict c)
{
    ++s_throw_calls;
    if (ld32(c->esp) != ENGINE_THROW_RETURN)
        remember_failure("_RunCallback throw-helper return pin changed");
    (void)gpop(c);
}

void guest_int3(CPU *__restrict c, uint32_t address)
{
    ++s_int3_calls;
    if (address != ENGINE_THROW_INT3)
        remember_failure("_RunCallback error tail moved");
    guest_fault(c, address, "expected frozen _RunCallback throw tail");
}

static int oracle_print(lua_State *state)
{
    const char *message = lua_tostring(state, 1);
    if (!message)
        return 0;
    if (strcmp(message, "REPENTOGXM LUA SENTINEL TOPLEVEL") == 0)
        ++s_top_level_markers;
    else if (strcmp(message, "REPENTOGXM LUA SENTINEL POST UPDATE") == 0)
        ++s_update_markers;
    else if (strcmp(message, "REPENTOGXM LUA SENTINEL POST RENDER") == 0)
        ++s_render_markers;
    return 0;
}

static void init_cpu(CPU *c, unsigned stack_index)
{
    uint32_t floor;
    uint32_t ceiling;

    memset(c, 0, sizeof *c);
    floor = pointer32(&s_guest_stacks[stack_index][0]);
    ceiling = pointer32(&s_guest_stacks[stack_index][8192]);
    c->stack_owner = c;
    c->stack_floor = floor;
    c->stack_ceiling = ceiling;
    c->stack_low_water = ceiling;
    c->esp = ceiling;
}

static int install_iat_page(void)
{
    void *page = VirtualAlloc((void *)(uintptr_t)FROZEN_IAT_PAGE,
                              FROZEN_IAT_PAGE_SIZE,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (page != (void *)(uintptr_t)FROZEN_IAT_PAGE)
        return 0;
    st32(FROZEN_IAT_SETTOP, IMPORT_TOKEN_BASE + 8U);
    st32(FROZEN_IAT_PCALLK, IMPORT_TOKEN_BASE + LUA_IMPORT_PCALLK);
    st32(FROZEN_IAT_PUSHINTEGER,
         IMPORT_TOKEN_BASE + LUA_IMPORT_PUSHINTEGER);
    st32(FROZEN_IAT_RAWGETI, IMPORT_TOKEN_BASE + LUA_IMPORT_RAWGETI);
    st32(FROZEN_IAT_REF, IMPORT_TOKEN_BASE + LUA_IMPORT_REF);
    st32(FROZEN_IAT_PUSHVALUE, IMPORT_TOKEN_BASE + LUA_IMPORT_PUSHVALUE);
    return 1;
}

static void set_harness_arguments(lua_State *state, int argc, char **argv)
{
    int index;
    lua_createtable(state, argc - 2, 1);
    for (index = 1; index < argc; ++index) {
        lua_pushstring(state, argv[index]);
        lua_rawseti(state, -2, index - 1);
    }
    lua_setglobal(state, "arg");
}

static int run_setup_harness(lua_State *state, int argc, char **argv)
{
    int status;

    lua_pushcfunction(state, oracle_print);
    lua_setglobal(state, "print");
    set_harness_arguments(state, argc, argv);
    status = luaL_loadfile(state, argv[1]);
    if (status == LUA_OK)
        status = lua_pcall(state, 0, 0, 0);
    if (status != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        fprintf(stderr, "sentinel setup failed: %s\n",
                message ? message : "Lua error");
        return 0;
    }
    return lua_gettop(state) == 0;
}

static int bridge_global_ref(CPU *c, uint32_t state, const char *name,
                             int32_t *key)
{
    uint32_t get_arguments[] = { state, pointer32(name) };
    uint32_t ref_arguments[] = { state, (uint32_t)LUA_REGISTRYINDEX };
    if (!invoke_import(c, LUA_IMPORT_GETGLOBAL, get_arguments, 2U) ||
        !invoke_import(c, LUA_IMPORT_REF, ref_arguments, 2U))
        return 0;
    *key = (int32_t)c->eax;
    return *key >= 0;
}

static int bridge_guest_global(CPU *c, uint32_t state, uint32_t target,
                               const char *name)
{
    uint32_t closure_arguments[] = { state, target, 0U };
    uint32_t global_arguments[] = { state, pointer32(name) };
    return invoke_import(c, LUA_IMPORT_PUSHCLOSURE,
                         closure_arguments, 3U) &&
           invoke_import(c, LUA_IMPORT_SETGLOBAL, global_arguments, 2U);
}

static int register_error_callbacks(lua_State *state)
{
    static const char script[] =
        "local a=RegisterMod('dispatch luaL_error oracle',1) "
        "a:AddCallback(9001,__repentogxm_oracle_fail) "
        "local b=RegisterMod('dispatch direct-check oracle',1) "
        "b:AddCallback(9002,__repentogxm_oracle_badcheck)";
    int status = luaL_loadbuffer(state, script, sizeof script - 1U,
                                 "@callback_dispatch_oracle.lua");
    if (status == LUA_OK)
        status = lua_pcall(state, 0, 0, 0);
    if (status != LUA_OK) {
        const char *message = lua_tostring(state, -1);
        fprintf(stderr, "error callback registration failed: %s\n",
                message ? message : "Lua error");
        return 0;
    }
    return 1;
}

static void prepare_engine_call(CPU *c,
                                const frozen_run_callback_call *call,
                                frozen_lua_bridge_ref *result)
{
    c->ebx = 0x1b1b1b1bU;
    c->ebp = 0x2b2b2b2bU;
    c->esi = 0x3b3b3b3bU;
    c->edi = 0x4b4b4b4bU;
    gpush(c, 0x5b5b5b5bU); /* frozen ret 8's second, unused argument */
    gpush(c, pointer32(result));
    gpush(c, ENGINE_RETURN);
    c->ecx = pointer32(call);
}

static int dispatch_success(CPU *c, frozen_run_callback_call *call,
                            frozen_lua_bridge_ref *result,
                            uint32_t state, lua_State *native_state)
{
    uint32_t saved_esp = c->esp;
    int saved_top = lua_gettop(native_state);

    result->state = 0xa5a5a5a5U;
    result->key = (int32_t)0x5a5a5a5aU;
    prepare_engine_call(c, call, result);
    sub_0040eef0(c);
    return !c->fault && c->esp == saved_esp &&
           c->eax == pointer32(result) &&
           c->ebx == 0x1b1b1b1bU && c->ebp == 0x2b2b2b2bU &&
           c->esi == 0x3b3b3b3bU && c->edi == 0x4b4b4b4bU &&
           result->state == state && result->key == LUA_REFNIL &&
           lua_gettop(native_state) == saved_top;
}

static int dispatch_error(CPU *c, frozen_run_callback_call *call,
                          frozen_lua_bridge_ref *result,
                          lua_State *native_state, unsigned kind)
{
    uint32_t saved_esp = c->esp;
    unsigned helpers = s_error_helper_calls;
    unsigned throws = s_throw_calls;
    unsigned traps = s_int3_calls;
    int saved_top = lua_gettop(native_state);

    result->state = 0xa5a5a5a5U;
    result->key = (int32_t)0x5a5a5a5aU;
    s_expected_error_kind = kind;
    prepare_engine_call(c, call, result);
    sub_0040eef0(c);
    if (!c->fault || c->fault_addr != ENGINE_THROW_INT3 ||
        s_error_helper_calls != helpers + 1U ||
        s_throw_calls != throws + 1U || s_int3_calls != traps + 1U ||
        result->state != 0xa5a5a5a5U ||
        result->key != (int32_t)0x5a5a5a5aU ||
        lua_gettop(native_state) != saved_top + 1 || s_oracle_failure)
        return 0;
    lua_pop(native_state, 1);

    /* A real C++ throw does not return to this activation.  Our helper stubs
     * intentionally stop at INT3, so restore only the synthetic caller frame
     * before the next independent invocation.  Do not call the bridge abort
     * hook: repeated calls and the second CPU below must expose any leaked
     * callback frame or Lua owner. */
    c->esp = saved_esp;
    c->fault = NULL;
    c->fault_addr = 0U;
    return 1;
}

static int fail(CPU *c, lua_State *state, const char *message)
{
    if (s_oracle_failure)
        fprintf(stderr, "%s: %s\n", message, s_oracle_failure);
    else if (c && c->fault)
        fprintf(stderr, "%s: guest fault at %08x: %s\n", message,
                c->fault_addr, c->fault);
    else if (state && lua_gettop(state))
        fprintf(stderr, "%s: %s\n", message, lua_tostring(state, -1));
    else
        fprintf(stderr, "%s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    CPU cpu;
    CPU second_cpu;
    uint32_t state;
    lua_State *native_state;
    uint32_t open_arguments[1];
    uint32_t close_arguments[1];
    uint32_t gettop_arguments[1];
    frozen_run_callback_registry registry;
    frozen_run_callback_call call;
    frozen_lua_bridge_ref result;
    unsigned i;

    if (argc != 5) {
        fprintf(stderr,
                "usage: %s HARNESS.LUA SENTINEL_ROOT CORE_SCRIPTS TRANSCRIPT\n",
                argv[0]);
        return 2;
    }
    if (!install_iat_page()) {
        fprintf(stderr, "could not reserve exact frozen Lua IAT page\n");
        return 2;
    }
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
    if (isaac_vita_lua_import_fast_prepare(1) != 13U) {
        fprintf(stderr, "typed Lua endpoint table did not bind 13 names\n");
        return 2;
    }
#endif
    init_cpu(&cpu, 0U);
    init_cpu(&second_cpu, 1U);

    if (!invoke_import(&cpu, LUA_IMPORT_NEWSTATE_AUX, NULL, 0U))
        return fail(&cpu, NULL, "project luaL_newstate bridge failed");
    state = cpu.eax;
    native_state = (lua_State *)(uintptr_t)state;
    s_expected_state = state;
    if (!state)
        return fail(&cpu, NULL, "project allocator could not create Lua state");
    open_arguments[0] = state;
    if (!invoke_import(&cpu, LUA_IMPORT_OPENLIBS, open_arguments, 1U))
        return fail(&cpu, native_state, "luaL_openlibs bridge failed");
    if (!run_setup_harness(native_state, argc, argv) ||
        s_top_level_markers != 1U)
        return fail(&cpu, native_state,
                    "exact core/project sentinel setup did not complete");

    registry.state = state;
    if (!bridge_global_ref(&cpu, state, s_run_callback_name, &registry.key))
        return fail(&cpu, native_state,
                    "frozen _RunCallback registry capture failed");
    call.registry = pointer32(&registry);
    call.parameter = -1;

    call.callback_id = SENTINEL_POST_UPDATE;
    if (!dispatch_success(&cpu, &call, &result, state, native_state) ||
        s_update_markers != 1U)
        return fail(&cpu, native_state,
                    "generated _RunCallback did not reach sentinel update");

    if (!bridge_guest_global(&cpu, state, GUEST_FAIL, s_fail_name) ||
        !bridge_guest_global(&cpu, state, GUEST_BADCHECK, s_badcheck_name) ||
        !register_error_callbacks(native_state))
        return fail(&cpu, native_state,
                    "translated callback registration failed");

    call.callback_id = ORACLE_LUAL_ERROR_CALLBACK;
    for (i = 0U; i < ERROR_REPETITIONS; ++i)
        if (!dispatch_error(&cpu, &call, &result, native_state, 1U))
            return fail(&cpu, native_state,
                        "engine luaL_error dispatch recovery failed");
    call.callback_id = ORACLE_CHECK_ERROR_CALLBACK;
    for (i = 0U; i < ERROR_REPETITIONS; ++i)
        if (!dispatch_error(&cpu, &call, &result, native_state, 2U))
            return fail(&cpu, native_state,
                        "engine direct-check longjmp recovery failed");

    if (s_guest_fail_calls != ERROR_REPETITIONS ||
        s_guest_badcheck_calls != ERROR_REPETITIONS ||
        s_error_helper_calls != ERROR_REPETITIONS * 2U ||
        s_throw_calls != ERROR_REPETITIONS * 2U ||
        s_int3_calls != ERROR_REPETITIONS * 2U)
        return fail(&cpu, native_state,
                    "engine error/longjmp census changed");

    /* A second CPU is decisive for process-owner cleanup: same-CPU re-entry
     * would be accepted even if the outer bridge owner were accidentally
     * retained. */
    call.callback_id = SENTINEL_POST_RENDER;
    if (!dispatch_success(&second_cpu, &call, &result,
                          state, native_state) ||
        s_render_markers != 1U)
        return fail(&second_cpu, native_state,
                    "post-longjmp second-CPU sentinel dispatch failed");
    gettop_arguments[0] = state;
    if (!invoke_import(&second_cpu, LUA_IMPORT_GETTOP,
                       gettop_arguments, 1U) || second_cpu.eax != 0U)
        return fail(&second_cpu, native_state,
                    "post-dispatch Lua stack was not empty");

    close_arguments[0] = state;
    if (!invoke_import(&second_cpu, LUA_IMPORT_CLOSE, close_arguments, 1U))
        return fail(&second_cpu, NULL, "lua_close bridge failed");
    if (!VirtualFree((void *)(uintptr_t)FROZEN_IAT_PAGE, 0U, MEM_RELEASE)) {
        fprintf(stderr, "frozen Lua IAT page release failed\n");
        return 1;
    }

    printf("generated _RunCallback -> project sentinel: PASS "
           "(success=2 luaL_error=%u direct-longjmp=%u helpers=%u)\n",
           s_guest_fail_calls, s_guest_badcheck_calls, s_error_helper_calls);
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE)
    {
        extern unsigned g_isaac_vita_lua_fast_typed_runs;
        /* Every frozen _RunCallback dispatch runs rawgeti, pushvalue and
         * settop through the typed endpoints on the bound CPU stacks. */
        if (g_isaac_vita_lua_fast_typed_runs == 0U) {
            fprintf(stderr, "typed Lua endpoints were never taken\n");
            return 1;
        }
        printf("fastdispatch: typed_runs=%u\n",
               g_isaac_vita_lua_fast_typed_runs);
    }
#endif
    return 0;
}
