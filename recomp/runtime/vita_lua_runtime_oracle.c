/* 32-bit host executable for the production Lua bridge.  It deliberately
 * uses the same uint32_t pointer/stack ABI as Vita and routes native Lua C
 * callbacks through a registered guest function table plus guest_call(). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "host_vita_lua.h"
#include "host_vita_startup.h"

enum {
    GUEST_ALLOCATOR = 0x003fca80U,
    GUEST_ADD = 0x00421000U,
    GUEST_FAIL = 0x00421010U,
    GUEST_BADCHECK = 0x00421020U,
    GUEST_FORMAT_FAIL = 0x00421030U,
    GUEST_REQUIRE_FAIL = 0x00421040U,
    GUEST_PUSHFSTRING = 0x00421050U,
    GUEST_BADFORMAT = 0x00421060U,
    GUEST_BADPUSHFSTRING = 0x00421070U,
    GUEST_LEVEL_INIT = 0x00421080U,
    /* LuaBridge-shaped __index fixture (wf/opt-lua-index).  GUEST_INDEX_REF
     * is the same oracle body under an address the bridge never replays. */
    GUEST_INDEX_REF = 0x00421090U,
    GUEST_GETTER = 0x004210a0U,
    GUEST_GETTER_BASE = 0x004210b0U,
    GUEST_BAD_GETTER = 0x004210c0U,
    GUEST_NEWOBJECT = 0x004210d0U,
    GUEST_BOXED_GETTER = 0x004210e0U,
    GUEST_NESTED_GETTER = 0x004210f0U,
    IMPORT_RETURN = 0xfff16000U,

    LUA_IMPORT_CLOSE = 0U,
    LUA_IMPORT_ISCFUNCTION = 2U,
    LUA_IMPORT_GETFIELD = 3U,
    LUA_IMPORT_SETMETATABLE = 6U,
    LUA_IMPORT_SETTOP = 8U,
    LUA_IMPORT_OPENLIBS = 10U,
    LUA_IMPORT_REQUIREF = 12U,
    LUA_IMPORT_RAWGET = 17U,
    LUA_IMPORT_GETTOP = 18U,
    LUA_IMPORT_PUSHSTRING = 23U,
    LUA_IMPORT_TOUSERDATA = 24U,
    LUA_IMPORT_PUSHFSTRING = 25U,
    LUA_IMPORT_GETGLOBAL = 27U,
    LUA_IMPORT_CALLK = 30U,
    LUA_IMPORT_LOADBUFFERX = 32U,
    LUA_IMPORT_NEWSTATE = 36U,
    LUA_IMPORT_SETGLOBAL = 37U,
    LUA_IMPORT_NEWUSERDATA = 38U,
    LUA_IMPORT_CHECKINTEGER = 40U,
    LUA_IMPORT_ERROR = 41U,
    LUA_IMPORT_SETFIELD = 44U,
    LUA_IMPORT_TYPE = 47U,
    LUA_IMPORT_ROTATE = 49U,
    LUA_IMPORT_GETMETATABLE = 51U,
    LUA_IMPORT_ABSINDEX = 52U,
    LUA_IMPORT_LOADFILEX = 57U,
    LUA_IMPORT_GC = 58U,
    LUA_IMPORT_PCALLK = 59U,
    LUA_IMPORT_PUSHINTEGER = 60U,
    LUA_IMPORT_PUSHCLOSURE = 61U,
    LUA_IMPORT_PUSHVALUE = 66U
};

/* The frozen LuaBridge CFunc::gcMetaMethod<T> stubs start at RVA 0x45d450;
 * the guest registers absolute (BASE+rva) function pointers. */
#define GUEST_FINALIZER ((uint32_t)GUEST_IMAGE_BASE + 0x0045d450U)
/* A second stub of the same family whose body re-enters the lua_gc import. */
#define GUEST_FINALIZER_NESTED ((uint32_t)GUEST_IMAGE_BASE + 0x0045d470U)
/* Return words the lua_gc policy pins (host_vita_lua_gc.c): the Level-init
 * family's GCCOLLECT and LuaEngine::Update's GCSTEP. */
#define GC_SITE_LEVEL_INIT_COLLECT 0x003b5870U
#define GC_SITE_UPDATE_STEP 0x004023c6U
/* The frozen registration pushes the relocated VA of CFunc::indexMetaMethod
 * (corpus: GPUSH(0x983f95c0U) under GUEST_IMAGE_BASE 0x98000000); the oracle
 * table is keyed by the exact value the closure carries. */
#define GUEST_INDEX_META ((uint32_t)GUEST_IMAGE_BASE + 0x003f95c0U)

_Alignas(16) static uint32_t s_guest_stack[8192];
static const uint32_t *s_guest_addresses;
static const guest_fn *s_guest_functions;
static uint32_t s_guest_function_count;
static unsigned s_allocator_calls;
static unsigned s_allocator_frees;
static unsigned s_guest_add_calls;
static unsigned s_guest_fail_calls;
static unsigned s_guest_badcheck_calls;
static unsigned s_guest_format_fail_calls;
static unsigned s_guest_require_fail_calls;
static unsigned s_guest_pushfstring_calls;
static unsigned s_guest_badformat_calls;
static unsigned s_guest_badpushfstring_calls;
static unsigned s_guest_finalizer_calls;
static unsigned s_guest_finalizer_nested_calls;
static unsigned s_guest_level_init_calls;
static char s_isaac_log[65536];
static size_t s_isaac_log_length;
static unsigned s_isaac_log_lines;
static unsigned s_guest_index_meta_calls;
static unsigned s_guest_getter_calls;
static unsigned s_guest_getter_base_calls;
static unsigned s_guest_bad_getter_calls;
static unsigned s_guest_newobject_calls;
static unsigned s_guest_boxed_getter_calls;
static unsigned s_guest_nested_getter_calls;
static unsigned s_path_map_calls;
static uint32_t s_path_map_last_guest;
static uint32_t s_path_map_last_capacity;
static const char *s_native_script_path;
static char s_native_missing_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
static char s_add_name[] = "guest_add";
static char s_fail_name[] = "guest_fail";
static char s_badcheck_name[] = "guest_badcheck";
static char s_format_fail_name[] = "guest_format_fail";
static char s_require_fail_name[] = "guest_require_fail";
static char s_pushfstring_name[] = "guest_pushfstring";
static char s_badformat_name[] = "guest_badformat";
static char s_badpushfstring_name[] = "guest_badpushfstring";
static char s_finalizer_name[] = "guest_finalizer";
static char s_finalizer_nested_name[] = "guest_finalizer_nested";
static char s_level_init_name[] = "guest_level_init";
/* Hostile GC cases: ~3 MB of dead tables, 20 translated finalizers and two
 * finalizers that call lua_gc themselves; one Lua __gc that raises. */
static const char s_gc_nested_chunk[] =
    "for i = 1, 40000 do local t = {i} end\n"
    "for i = 1, 20 do setmetatable({}, {__gc = guest_finalizer}) end\n"
    "for i = 1, 2 do setmetatable({}, {__gc = guest_finalizer_nested}) end\n";
static const char s_gc_small_chunk[] =
    "for i = 1, 40000 do local t = {i} end\n";
static const char s_gc_error_chunk[] =
    "setmetatable({}, {__gc = function() error('boom') end})\n";
static char s_gc_nested_chunk_name[] = "=gc_nested";
static char s_gc_small_chunk_name[] = "=gc_small";
static char s_gc_error_chunk_name[] = "=gc_error";
/* 200 K dead tables (~10 MB) plus 50 objects with a translated __gc: a
 * bounded 256 KB step sweeps well under a fifth of them, luaC_fullgc all. */
static const char s_gc_garbage_chunk[] =
    "for i = 1, 200000 do local t = {i} end\n"
    "for i = 1, 50 do setmetatable({}, {__gc = guest_finalizer}) end\n";
static char s_gc_chunk_name[] = "=gc_garbage";
static char s_native_index_name[] = "native_index";
static char s_ref_index_name[] = "ref_index";
static char s_getter_name[] = "guest_getter";
static char s_getter_base_name[] = "guest_getter_base";
static char s_bad_getter_name[] = "guest_bad_getter";
static char s_newobject_name[] = "guest_newobject";
static char s_boxed_getter_name[] = "guest_boxed_getter";
static char s_nested_getter_name[] = "guest_nested_getter";
static char s_value_key[] = "value";
static char s_index_trip_name[] = "index_trip";
static char s_propget_name[] = "__propget";
static char s_parent_name[] = "__parent";
static char s_percent_s[] = "%s";
/* sub_003f95c0 throws std::logic_error with exactly these texts. */
static char s_index_not_cfunction[] = "logic_error: not a cfunction";
static char s_index_missing_propget[] = "logic_error: missing __propget table";
static char s_index_parent_not_table[] = "logic_error: __parent is not a table";
static char s_bad_getter_format[] = "bad getter %s";
static char s_bad_getter_argument[] = "Prop";
static char s_index_digest_name[] = "index_digest";
static char s_index_expected_hits_name[] = "index_expected_hits";
static char s_index_expected_fallbacks_name[] = "index_expected_fallbacks";
static char s_index_expected_nested_name[] = "index_expected_nested";
/* Every Lua 5.3.3 conversion once, through the x86 cdecl vararg layout. */
static char s_guest_format[] =
    "module '%s' not found:%s|%d|%I|%f|%c|%p|%U|%%|tail";
static char s_format_name[] = "descriptions.rep.nl_nl";
static char s_format_long_text[321];
/* The frozen require (sub_003fc500) raises exactly this shape; EID parses
 * the second "no file '" entry for its mod path. */
static char s_require_format[] = "module '%s' not found:%s";
static char s_require_name[] = "";
static char s_require_message[] =
    "\n\tno file 'resources/scripts/.lua'"
    "\n\tno file 'ux0:data/isaacr001/mods/836319872/.lua'";
static char s_bad_format[] = "%q";
static char s_expected_name[] = "expected_format";
static char s_expected_format[512];
static char s_result_name[] = "result";
static char s_package_name[] = "package";
static char s_path_name[] = "path";
static char s_guest_error[] = "guest boom";
static char s_table_module[] = "table";
static char s_guest_mod_path[] =
    "ux0:data/isaacr001/mods/836319872/main.lua";
static char s_guest_package_path[] =
    "ux0:data/isaacr001/mods/836319872/?.lua;"
    ".\\resources\\scripts\\?.lua";
static const char s_path_map_fault[] = "oracle rejected guest mod path";

enum oracle_path_mode {
    ORACLE_PATH_SCRIPT = 0,
    ORACLE_PATH_MISSING,
    ORACLE_PATH_REJECT
};

static enum oracle_path_mode s_path_mode;

static uint32_t pointer32(const void *pointer)
{
    if ((uintptr_t)pointer > UINT32_MAX) {
        fprintf(stderr, "oracle pointer does not fit frozen 32-bit ABI\n");
        exit(2);
    }
    return (uint32_t)(uintptr_t)pointer;
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
    guest_fault(c, address, "oracle guest stack violation");
    c->fault_addr = pc ? pc : address;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "oracle guest stack owner violation");
    return 0;
}

/* recomp/vita/platform.h logger: host_vita_lua_gc.c's clamp and window
 * lines land here (the GC cases strstr the buffer) and every bridge
 * diagnostic ([isaac-lua] ...) is echoed to stdout as "[log] ..." for the
 * test driver (native-index stats/mismatch lines). */
void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    size_t available = sizeof s_isaac_log - s_isaac_log_length;
    int written;

    va_start(arguments, format);
    written = vsnprintf(s_isaac_log + s_isaac_log_length, available,
                        format, arguments);
    va_end(arguments);
    if (written > 0 && available != 0U) {
        size_t length = (size_t)written;
        if (length >= available)
            length = available - 1U;
        printf("[log] %s\n", s_isaac_log + s_isaac_log_length);
        s_isaac_log_length += length;
        if (s_isaac_log_length + 1U < sizeof s_isaac_log)
            s_isaac_log[s_isaac_log_length++] = '\n';
    }
    ++s_isaac_log_lines;
}

void guest_register(const uint32_t *addresses, const guest_fn *functions,
                    uint32_t count)
{
    s_guest_addresses = addresses;
    s_guest_functions = functions;
    s_guest_function_count = count;
}

guest_fn guest_lookup(uint32_t address)
{
    uint32_t low = 0U;
    uint32_t high = s_guest_function_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;
        if (s_guest_addresses[middle] == address)
            return s_guest_functions[middle];
        if (s_guest_addresses[middle] < address)
            low = middle + 1U;
        else
            high = middle;
    }
    return NULL;
}

void guest_call(CPU *__restrict c, uint32_t address)
{
    guest_fn function = guest_lookup(address);
    if (!function) {
        guest_fault(c, address, "oracle guest_call target is not registered");
        return;
    }
    function(c);
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
    const char *source;
    size_t length;

    ++s_path_map_calls;
    s_path_map_last_guest = guest_path;
    s_path_map_last_capacity = capacity;
    if (s_path_mode == ORACLE_PATH_REJECT) {
        guest_fault(c, guest_path, s_path_map_fault);
        return 0;
    }
    source = s_path_mode == ORACLE_PATH_MISSING
                 ? s_native_missing_path : s_native_script_path;
    if (!source || !native_path || !capacity) {
        guest_fault(c, guest_path, "oracle path mapper storage mismatch");
        return 0;
    }
    length = strlen(source);
    if (length >= capacity) {
        guest_fault(c, guest_path, "oracle mapped path exceeds output bound");
        return 0;
    }
    memcpy(native_path, source, length + 1U);
    return 1;
}

/* The production import-ID binder (host_vita_import_id.c) hands a Lua ID to
 * its typed endpoint when ISAAC_VITA_LUA_IMPORT_FASTDISPATCH bound one and to
 * the generic family entry otherwise; the oracle dispatches the same way, so
 * the knob build runs this smoke through the typed endpoints on the bound
 * main CPU and through their generic fallback on the unbound probe CPUs. */
static int bridge_import(CPU *c, uint32_t index)
{
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
    guest_import_family_fn fast = isaac_vita_lua_import_fast_binding(index);
    if (fast)
        return fast(c, index, NULL);
#endif
    return isaac_vita_lua_import_indexed(c, index, NULL);
}

/* return_word: what the translated call site pushes, i.e. its RVA.  The
 * lua_gc policy keys on it, so the GC cases fake pinned sites here. */
static int invoke_import_at(CPU *c, uint32_t index,
                            const uint32_t *arguments, uint32_t count,
                            uint32_t return_word)
{
    uint32_t saved = c->esp;
    uint32_t i;
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, return_word);
    if (!bridge_import(c, index)) {
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

static int invoke_import(CPU *c, uint32_t index,
                         const uint32_t *arguments, uint32_t count)
{
    return invoke_import_at(c, index, arguments, count,
                            IMPORT_RETURN + index);
}

static int64_t import_checkinteger(CPU *c, uint32_t state, int index)
{
    uint32_t arguments[] = { state, (uint32_t)index };
    if (!invoke_import(c, LUA_IMPORT_CHECKINTEGER, arguments, 2U))
        return 0;
    return (int64_t)((uint64_t)c->eax | ((uint64_t)c->edx << 32U));
}

static int import_pushinteger(CPU *c, uint32_t state, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    uint32_t arguments[] = {
        state, (uint32_t)bits, (uint32_t)(bits >> 32U)
    };
    return invoke_import(c, LUA_IMPORT_PUSHINTEGER, arguments, 3U);
}

static void guest_allocator(CPU *__restrict c)
{
    uint32_t entry = c->esp;
    void *pointer = (void *)(uintptr_t)ld32(entry + 8U);
    size_t new_size = (size_t)ld32(entry + 16U);
    void *result;

    ++s_allocator_calls;
    if (!new_size) {
        free(pointer);
        ++s_allocator_frees;
        result = NULL;
    } else {
        result = realloc(pointer, new_size);
        if (!result)
            fprintf(stderr, "oracle realloc(%u) failed (esp=%08x)\n",
                    (unsigned)new_size, c->esp);
    }
    c->eax = pointer32(result);
    (void)gpop(c); /* guest RET; bridge remains cdecl caller of four args */
}

static void guest_add(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    int64_t argument;
    int64_t upvalue;

    ++s_guest_add_calls;
    argument = import_checkinteger(c, state, 1);
    if (c->fault)
        return;
    upvalue = import_checkinteger(c, state, LUA_REGISTRYINDEX - 1);
    if (c->fault)
        return;
    if (!import_pushinteger(c, state, argument + upvalue))
        return;
    c->eax = 1U;
    (void)gpop(c);
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
    guest_fault(c, GUEST_BADCHECK, "luaL_checkinteger unexpectedly returned");
}

#define FORMAT_ARGUMENT_COUNT 12U

/* (L, fmt, char*, char*, int, lua_Integer, lua_Number, int, void*, long):
 * 64-bit values occupy two adjacent 4-byte slots, low word first. */
static void format_arguments(uint32_t *arguments, uint32_t state)
{
    uint64_t integer = UINT64_C(0x1122334455667788);
    double number = 2.5;
    uint64_t number_bits;

    memcpy(&number_bits, &number, sizeof number_bits);
    arguments[0] = state;
    arguments[1] = pointer32(s_guest_format);
    arguments[2] = pointer32(s_format_name);
    arguments[3] = pointer32(s_format_long_text);
    arguments[4] = (uint32_t)-7;
    arguments[5] = (uint32_t)integer;
    arguments[6] = (uint32_t)(integer >> 32U);
    arguments[7] = (uint32_t)number_bits;
    arguments[8] = (uint32_t)(number_bits >> 32U);
    arguments[9] = (uint32_t)'Q';
    arguments[10] = pointer32(s_guest_error);
    arguments[11] = 0x20ACU;
}

static void guest_format_fail(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[FORMAT_ARGUMENT_COUNT];
    ++s_guest_format_fail_calls;
    format_arguments(arguments, state);
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments,
                        FORMAT_ARGUMENT_COUNT);
    guest_fault(c, GUEST_FORMAT_FAIL,
                "formatted luaL_error unexpectedly returned");
}

static void guest_require_fail(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = {
        state, pointer32(s_require_format), pointer32(s_require_name),
        pointer32(s_require_message)
    };
    ++s_guest_require_fail_calls;
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments, 4U);
    guest_fault(c, GUEST_REQUIRE_FAIL,
                "require-shaped luaL_error unexpectedly returned");
}

static void guest_pushfstring(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    lua_State *native_state = (lua_State *)(uintptr_t)state;
    uint32_t arguments[FORMAT_ARGUMENT_COUNT];
    ++s_guest_pushfstring_calls;
    format_arguments(arguments, state);
    if (!invoke_import(c, LUA_IMPORT_PUSHFSTRING, arguments,
                       FORMAT_ARGUMENT_COUNT))
        return;
    /* cdecl EAX is the interned copy, i.e. lua_tostring of the new top. */
    if (c->eax != pointer32(lua_tostring(native_state, -1))) {
        guest_fault(c, GUEST_PUSHFSTRING,
                    "lua_pushfstring returned a foreign pointer");
        return;
    }
    c->eax = 1U;
    (void)gpop(c);
}

static void guest_badformat(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, pointer32(s_bad_format) };
    ++s_guest_badformat_calls;
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments, 2U);
    guest_fault(c, GUEST_BADFORMAT,
                "malformed luaL_error unexpectedly returned");
}

/* A LuaBridge-style __gc: consumes the object, returns nothing. */
static void guest_finalizer(CPU *__restrict c)
{
    ++s_guest_finalizer_calls;
    c->eax = 0U;
    (void)gpop(c);
}

/* Forward: the nested finalizer and the pcall'd level init re-enter lua_gc
 * through the same import the oracle drives. */
static int import_gc(CPU *c, uint32_t state, int what, int data,
                     uint32_t return_word, int *result);

/* A __gc that re-enters the lua_gc import while the collector is inside
 * lua_gc: a GCCOUNT query, LuaEngine::Update's paced step and the Level-init
 * family's GCCOLLECT (nested clamp).  Lua permits collectgarbage inside a
 * finalizer; the bridge must survive the nesting. */
static void guest_finalizer_nested(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    int result;
    ++s_guest_finalizer_nested_calls;
    if (!import_gc(c, state, LUA_GCCOUNT, 0,
                   (uint32_t)IMPORT_RETURN + LUA_IMPORT_GC, &result) ||
        !import_gc(c, state, LUA_GCSTEP, 1, GC_SITE_UPDATE_STEP, &result) ||
        !import_gc(c, state, LUA_GCCOLLECT, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &result) ||
        result != 0)
        return;
    c->eax = 0U;
    (void)gpop(c);
}

/* Level init reached from a mod callback (Isaac.ExecuteCommand "stage" runs
 * it synchronously): the pinned GCCOLLECT under the game's lua_pcallk. */
static void guest_level_init(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    int result;
    ++s_guest_level_init_calls;
    if (!import_gc(c, state, LUA_GCCOLLECT, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &result) ||
        result != 0)
        return;
    c->eax = 0U;
    (void)gpop(c);
}

static void guest_badpushfstring(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, pointer32(s_bad_format) };
    ++s_guest_badpushfstring_calls;
    (void)invoke_import(c, LUA_IMPORT_PUSHFSTRING, arguments, 2U);
    guest_fault(c, GUEST_BADPUSHFSTRING,
                "malformed lua_pushfstring unexpectedly returned");
}

static int import2(CPU *c, uint32_t index, uint32_t state, uint32_t a)
{
    uint32_t arguments[] = { state, a };
    return invoke_import(c, index, arguments, 2U);
}

static int import3(CPU *c, uint32_t index, uint32_t state, uint32_t a,
                   uint32_t b)
{
    uint32_t arguments[] = { state, a, b };
    return invoke_import(c, index, arguments, 3U);
}

#define INDEX_NEG(value) ((uint32_t)(int32_t)(value))

static void index_return_one(CPU *__restrict c)
{
    c->eax = 1U;
    (void)gpop(c);
}

/* lua_remove(L, index) as the frozen body issues it: lua_rotate(L, index, -1)
 * then lua_settop(L, -2). */
static int index_remove(CPU *c, uint32_t state, int index)
{
    return import3(c, LUA_IMPORT_ROTATE, state, INDEX_NEG(index),
                   INDEX_NEG(-1)) &&
           import2(c, LUA_IMPORT_SETTOP, state, INDEX_NEG(-2));
}

/* LuaBridge rawgetfield(L, -1, key): absindex, pushstring, rawget(absolute),
 * followed by the body's lua_type(L, -1); EAX holds that type. */
static int index_rawgetfield_type(CPU *c, uint32_t state, const char *key)
{
    uint32_t absolute;
    if (!import2(c, LUA_IMPORT_ABSINDEX, state, INDEX_NEG(-1)))
        return 0;
    absolute = c->eax;
    return import2(c, LUA_IMPORT_PUSHSTRING, state, pointer32(key)) &&
           import2(c, LUA_IMPORT_RAWGET, state, absolute) &&
           import2(c, LUA_IMPORT_TYPE, state, INDEX_NEG(-1));
}

/* The frozen body pops two and throws std::logic_error(text) through the CRT.
 * The oracle models that throw as luaL_error("%s", text) so a Lua pcall can
 * observe the text the bridge's fallback path must reproduce byte for byte. */
static void index_throw(CPU *c, uint32_t state, const char *text)
{
    uint32_t arguments[] = { state, pointer32(s_percent_s), pointer32(text) };
    if (!import2(c, LUA_IMPORT_SETTOP, state, INDEX_NEG(-3)))
        return;
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments, 3U);
    guest_fault(c, GUEST_INDEX_META,
                "index logic_error model unexpectedly returned");
}

/* sub_003f95c0, LuaBridge CFunc::indexMetaMethod, with the corpus's exact
 * import sequence (guest_0120.c; IAT slots resolved against the PE):
 * getmetatable(1) unchecked, then per class table: rawget key -> cfunction
 * returns it; nil -> __propget[key] cfunction -> called with the object;
 * nil -> __parent table -> repeat; nil -> return the nil.  Anything else
 * throws.  Both the native replay and its fallback must match this. */
static void guest_index_meta(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    ++s_guest_index_meta_calls;
    if (!import2(c, LUA_IMPORT_GETMETATABLE, state, 1U))
        return;
    for (;;) {
        if (!import2(c, LUA_IMPORT_PUSHVALUE, state, 2U) ||
            !import2(c, LUA_IMPORT_RAWGET, state, INDEX_NEG(-2)) ||
            !import2(c, LUA_IMPORT_ISCFUNCTION, state, INDEX_NEG(-1)))
            return;
        if (c->eax) {
            if (index_remove(c, state, -2))
                index_return_one(c);
            return;
        }
        if (!import2(c, LUA_IMPORT_TYPE, state, INDEX_NEG(-1)))
            return;
        if (c->eax != (uint32_t)LUA_TNIL) {
            index_throw(c, state, s_index_not_cfunction);
            return;
        }
        if (!import2(c, LUA_IMPORT_SETTOP, state, INDEX_NEG(-2)) ||
            !index_rawgetfield_type(c, state, s_propget_name))
            return;
        if (c->eax != (uint32_t)LUA_TTABLE) {
            index_throw(c, state, s_index_missing_propget);
            return;
        }
        if (!import2(c, LUA_IMPORT_PUSHVALUE, state, 2U) ||
            !import2(c, LUA_IMPORT_RAWGET, state, INDEX_NEG(-2)) ||
            !index_remove(c, state, -2) ||
            !import2(c, LUA_IMPORT_ISCFUNCTION, state, INDEX_NEG(-1)))
            return;
        if (c->eax) {
            uint32_t call_arguments[] = { state, 1U, 1U, 0U, 0U };
            if (index_remove(c, state, -2) &&
                import2(c, LUA_IMPORT_PUSHVALUE, state, 1U) &&
                invoke_import(c, LUA_IMPORT_CALLK, call_arguments, 5U))
                index_return_one(c);
            return;
        }
        if (!import2(c, LUA_IMPORT_TYPE, state, INDEX_NEG(-1)))
            return;
        if (c->eax != (uint32_t)LUA_TNIL) {
            index_throw(c, state, s_index_not_cfunction);
            return;
        }
        if (!import2(c, LUA_IMPORT_SETTOP, state, INDEX_NEG(-2)) ||
            !index_rawgetfield_type(c, state, s_parent_name))
            return;
        if (c->eax == (uint32_t)LUA_TTABLE) {
            if (!index_remove(c, state, -2))
                return;
            continue;
        }
        if (c->eax == (uint32_t)LUA_TNIL) {
            index_return_one(c);
            return;
        }
        index_throw(c, state, s_index_parent_not_table);
        return;
    }
}

/* Property getter: the object's 8 userdata bytes as an integer (1234 for a
 * table object), reached through the bridge again (guest re-entry). */
static void guest_getter(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    int64_t value = 1234;
    ++s_guest_getter_calls;
    if (!import2(c, LUA_IMPORT_TYPE, state, 1U))
        return;
    if (c->eax == (uint32_t)LUA_TUSERDATA) {
        if (!import2(c, LUA_IMPORT_TOUSERDATA, state, 1U))
            return;
        memcpy(&value, (const void *)(uintptr_t)c->eax, sizeof value);
    }
    if (import_pushinteger(c, state, value))
        index_return_one(c);
}

static void guest_getter_base(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    ++s_guest_getter_base_calls;
    if (import_pushinteger(c, state, 77))
        index_return_one(c);
}

/* Getter that raises: luaL_error(L, "bad getter %s", "Prop") through the
 * frozen vararg import; the text reaches Lua with luaL_where(L, 1) in front. */
static void guest_bad_getter(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, pointer32(s_bad_getter_format),
                             pointer32(s_bad_getter_argument) };
    ++s_guest_bad_getter_calls;
    (void)invoke_import(c, LUA_IMPORT_ERROR, arguments, 3U);
    guest_fault(c, GUEST_BAD_GETTER, "getter luaL_error unexpectedly returned");
}

/* guest_newobject(mt, value): an 8-byte userdata carrying value under mt,
 * the shape of a LuaBridge UserdataValue. */
static void guest_newobject(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    int64_t value;
    ++s_guest_newobject_calls;
    value = import_checkinteger(c, state, 2);
    if (c->fault || !import2(c, LUA_IMPORT_NEWUSERDATA, state, 8U))
        return;
    memcpy((void *)(uintptr_t)c->eax, &value, sizeof value);
    if (import2(c, LUA_IMPORT_PUSHVALUE, state, 1U) &&
        import2(c, LUA_IMPORT_SETMETATABLE, state, INDEX_NEG(-2)))
        index_return_one(c);
}

/* By-value getter (Vector-like): boxes a fresh userdata copy under the
 * object's metatable on every call, so two calls never compare rawequal. */
static void guest_boxed_getter(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    const void *source;
    void *box;
    ++s_guest_boxed_getter_calls;
    if (!import2(c, LUA_IMPORT_TOUSERDATA, state, 1U))
        return;
    source = (const void *)(uintptr_t)c->eax;
    if (!source) {
        guest_fault(c, GUEST_BOXED_GETTER, "boxed getter needs userdata");
        return;
    }
    if (!import2(c, LUA_IMPORT_NEWUSERDATA, state, 8U))
        return;
    box = (void *)(uintptr_t)c->eax;
    memcpy(box, source, 8U);
    if (!import2(c, LUA_IMPORT_GETMETATABLE, state, 1U))
        return;
    if (!c->eax) {
        guest_fault(c, GUEST_BOXED_GETTER,
                    "boxed getter object has no metatable");
        return;
    }
    if (import2(c, LUA_IMPORT_SETMETATABLE, state, INDEX_NEG(-2)))
        index_return_one(c);
}

/* Getter that re-enters __index while the outer __index is still running:
 * lua_getfield(L, 1, "value") through the import drives the object's own
 * __index closure (natively replayed when enabled) for the "value" getter,
 * and its single result is the property value. */
static void guest_nested_getter(CPU *__restrict c)
{
    uint32_t state = ld32(c->esp + 4U);
    uint32_t arguments[] = { state, 1U, pointer32(s_value_key) };
    ++s_guest_nested_getter_calls;
    if (invoke_import(c, LUA_IMPORT_GETFIELD, arguments, 3U))
        index_return_one(c);
}

/* Globals are fetched through the import (the state's guest allocator only
 * serves calls under an active import scope; interning a new global name
 * natively would panic with "not enough memory"). */
static int print_index_digest(CPU *c, uint32_t state,
                              lua_State *native_state)
{
    uint32_t arguments[] = { state, pointer32(s_index_digest_name) };
    const char *digest;
    const char *line;
    if (!invoke_import(c, LUA_IMPORT_GETGLOBAL, arguments, 2U))
        return 0;
    digest = lua_type(native_state, -1) == LUA_TSTRING
                 ? lua_tostring(native_state, -1) : NULL;
    if (!digest || !*digest) {
        lua_pop(native_state, 1);
        return 0;
    }
    for (line = digest; *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        printf("[digest] %.*s\n", (int)length, line);
        line += length + (end ? 1U : 0U);
    }
    lua_pop(native_state, 1);
    arguments[1] = pointer32(s_index_expected_hits_name);
    if (!invoke_import(c, LUA_IMPORT_GETGLOBAL, arguments, 2U))
        return 0;
    arguments[1] = pointer32(s_index_expected_fallbacks_name);
    if (!invoke_import(c, LUA_IMPORT_GETGLOBAL, arguments, 2U))
        return 0;
    arguments[1] = pointer32(s_index_expected_nested_name);
    if (!invoke_import(c, LUA_IMPORT_GETGLOBAL, arguments, 2U))
        return 0;
    if (!lua_isinteger(native_state, -3) ||
        !lua_isinteger(native_state, -2) ||
        !lua_isinteger(native_state, -1)) {
        lua_pop(native_state, 3);
        return 0;
    }
    printf("[expect] hits=%lld fallbacks=%lld nested=%lld\n",
           (long long)lua_tointeger(native_state, -3),
           (long long)lua_tointeger(native_state, -2),
           (long long)lua_tointeger(native_state, -1));
    lua_pop(native_state, 3);
    return 1;
}

static int register_closure(CPU *c, uint32_t state, uint32_t target,
                            const char *name, int with_upvalue)
{
    uint32_t closure_arguments[] = { state, target,
                                     with_upvalue ? 1U : 0U };
    uint32_t global_arguments[] = { state, pointer32(name) };
    if (with_upvalue && !import_pushinteger(c, state, 41))
        return 0;
    if (!invoke_import(c, LUA_IMPORT_PUSHCLOSURE,
                       closure_arguments, 3U))
        return 0;
    return invoke_import(c, LUA_IMPORT_SETGLOBAL, global_arguments, 2U);
}

/* lua_newstate installs no panic handler, so an unprotected Lua error would
 * abort() silently; name it instead. */
static int oracle_panic(lua_State *state)
{
    const char *message = lua_tostring(state, -1);
    fprintf(stderr, "oracle Lua panic: %s (top=%d allocs=%u)\n",
            message ? message : "(no message)", lua_gettop(state),
            s_allocator_calls);
    return 0;
}

static int fail(CPU *c, lua_State *state, const char *message)
{
    if (state) {
        /* Only a string top is rendered: lua_tostring on a number would
         * allocate outside an import scope and panic instead of reporting. */
        fprintf(stderr, "%s: %s\n", message,
                lua_gettop(state) && lua_type(state, -1) == LUA_TSTRING
                    ? lua_tostring(state, -1)
                    : lua_gettop(state) ? "(non-string top)"
                                        : "no Lua error string on the stack");
    } else if (c->fault) {
        fprintf(stderr, "%s: guest fault at %08x: %s\n", message,
                c->fault_addr, c->fault);
    } else {
        fprintf(stderr, "%s\n", message);
    }
    return 1;
}

static int check_loadfile_boundary(CPU *c, uint32_t state,
                                   lua_State *native_state)
{
    CPU probe;
    uint32_t load_arguments[] = {
        state, pointer32(s_guest_mod_path), 0U
    };
    uint32_t gettop_arguments[] = { state };
    uint32_t saved_esp;
    uint32_t sentinel;
    unsigned map_calls;
    int top;
    const char *error;

    /* A mapped native open failure is still vanilla luaL_loadfilex: cdecl
     * returns LUA_ERRFILE and leaves exactly one error object on the stack. */
    s_path_mode = ORACLE_PATH_MISSING;
    map_calls = s_path_map_calls;
    top = lua_gettop(native_state);
    if (!invoke_import(c, LUA_IMPORT_LOADFILEX, load_arguments, 3U) ||
        c->eax != LUA_ERRFILE || s_path_map_calls != map_calls + 1U ||
        s_path_map_last_guest != load_arguments[1] ||
        s_path_map_last_capacity != ISAAC_VITA_STARTUP_PATH_MAX + 1U ||
        lua_gettop(native_state) != top + 1)
        return 0;
    error = lua_tostring(native_state, -1);
    if (!error || !strstr(error, "cannot open ") ||
        !strstr(error, s_native_missing_path))
        return 0;
    lua_pop(native_state, 1);

    /* A path-policy rejection is a project guest fault, not LUA_ERRFILE.
     * Production guest_fault is non-returning; this recording oracle proves
     * the handler does not forge a cdecl return, mutate EAX/Lua, or retain the
     * process-wide Lua owner before that transfer. */
    s_path_mode = ORACLE_PATH_REJECT;
    map_calls = s_path_map_calls;
    top = lua_gettop(native_state);
    saved_esp = c->esp;
    sentinel = UINT32_C(0xa55aa55a);
    c->eax = sentinel;
    if (invoke_import(c, LUA_IMPORT_LOADFILEX, load_arguments, 3U) ||
        !c->fault || c->fault_addr != load_arguments[1] ||
        strcmp(c->fault, s_path_map_fault) != 0 ||
        c->eax != sentinel || c->esp != saved_esp - 16U ||
        s_path_map_calls != map_calls + 1U ||
        lua_gettop(native_state) != top)
        return 0;

    memset(&probe, 0, sizeof probe);
    probe.esp = pointer32(s_guest_stack +
                          sizeof s_guest_stack / sizeof s_guest_stack[0] / 2U);
    if (!invoke_import(&probe, LUA_IMPORT_GETTOP, gettop_arguments, 1U) ||
        probe.eax != (uint32_t)top)
        return 0;
    c->esp = saved_esp;
    c->fault = NULL;
    c->fault_addr = 0U;

    /* NULL is the specified stdin branch and must bypass path policy.  The
     * test runner supplies an empty stdin, so vanilla Lua returns one empty
     * chunk without blocking. */
    load_arguments[1] = 0U;
    map_calls = s_path_map_calls;
    top = lua_gettop(native_state);
    if (!invoke_import(c, LUA_IMPORT_LOADFILEX, load_arguments, 3U) ||
        c->eax != LUA_OK || s_path_map_calls != map_calls ||
        lua_gettop(native_state) != top + 1)
        return 0;
    lua_pop(native_state, 1);
    s_path_mode = ORACLE_PATH_SCRIPT;
    return 1;
}

static int import_gc(CPU *c, uint32_t state, int what, int data,
                     uint32_t return_word, int *result)
{
    uint32_t arguments[] = { state, (uint32_t)what, (uint32_t)data };
    if (!invoke_import_at(c, LUA_IMPORT_GC, arguments, 3U, return_word))
        return 0;
    *result = (int32_t)c->eax;
    return 1;
}

static int gc_make_garbage(CPU *c, uint32_t state)
{
    uint32_t load_arguments[] = {
        state, pointer32(s_gc_garbage_chunk),
        (uint32_t)(sizeof s_gc_garbage_chunk - 1U),
        pointer32(s_gc_chunk_name), 0U
    };
    uint32_t pcall_arguments[] = { state, 0U, 0U, 0U, 0U, 0U };
    if (!invoke_import(c, LUA_IMPORT_LOADBUFFERX, load_arguments, 5U) ||
        c->eax != LUA_OK)
        return 0;
    return invoke_import(c, LUA_IMPORT_PCALLK, pcall_arguments, 6U) &&
           c->eax == LUA_OK;
}

/* lua_gc policy (host_vita_lua_gc.c).  Knobs ON: a GCCOLLECT whose return
 * word is the Level-init family's 0x3b5870 becomes one bounded step and
 * logs the clamp line; any other site collects fully and logs nothing; 120
 * LuaEngine::Update steps print one gc window line.  Knobs OFF: the pinned
 * site collects fully like every other and nothing is logged. */
static int check_gc_policy(CPU *c, uint32_t state)
{
    int base_kb;
    int garbage_kb;
    int full_kb;
    int pinned_kb;
    int result;
    unsigned finalizers = s_guest_finalizer_calls;
    unsigned lines;
    uint32_t plain = (uint32_t)IMPORT_RETURN + LUA_IMPORT_GC;

    /* Freeze the collector so the garbage stays until an explicit lua_gc
     * (GCCOLLECT and GCSTEP run regardless of gcrunning). */
    if (!import_gc(c, state, LUA_GCSTOP, 0, plain, &result) ||
        !import_gc(c, state, LUA_GCCOLLECT, 0, plain, &result) ||
        result != 0 ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &base_kb))
        return 0;
    if (!gc_make_garbage(c, state) ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &garbage_kb) ||
        garbage_kb < base_kb + 4096)
        return 0;
    /* Unpinned site: vanilla luaC_fullgc, every finalizer, no clamp line. */
    lines = s_isaac_log_lines;
    if (!import_gc(c, state, LUA_GCCOLLECT, 0, plain, &result) ||
        result != 0 ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &full_kb) ||
        full_kb > base_kb + 256 ||
        s_guest_finalizer_calls != finalizers + 50U ||
        s_isaac_log_lines != lines)
        return 0;
    /* Pinned site: the Level-init family's tail. */
    if (!gc_make_garbage(c, state) ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &garbage_kb) ||
        garbage_kb < base_kb + 4096)
        return 0;
    lines = s_isaac_log_lines;
    if (!import_gc(c, state, LUA_GCCOLLECT, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &result) ||
        result != 0 ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &pinned_kb))
        return 0;
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
    if (pinned_kb < base_kb + 2048 || s_isaac_log_lines != lines + 1U ||
        !strstr(s_isaac_log, "[isaac-lua] gccollect clamp: site=003b5870 "
                             "-> step 256 KB, before=") ||
        !strstr(s_isaac_log, " KB after=") ||
        !strstr(s_isaac_log, " KB, us="))
        return 0;
#else
    if (pinned_kb > base_kb + 256 || s_isaac_log_lines != lines)
        return 0;
#endif
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
    {
        const char *window;
        unsigned n = 0U;
        unsigned max_step = 0U;
        unsigned callbacks = 0U;
        unsigned i;

        lines = s_isaac_log_lines;
        for (i = 0U; i < 120U; ++i)
            if (!import_gc(c, state, LUA_GCSTEP, 1, GC_SITE_UPDATE_STEP,
                           &result))
                return 0;
        window = strstr(s_isaac_log, "[isaac-lua] gc window: win=1 "
                                     "gcstep(n,sum_us,p95_us,max_us)=120,");
        if (s_isaac_log_lines != lines + 1U || !window ||
            !strstr(window, " collect(clamp,full,us)=1,2,") ||
            !strstr(window, " kb(req,app)=120,120 "))
            return 0;
        window = strstr(window, " fin(n,max_step,cb)=");
        if (!window)
            return 0;
        {
            char *end = NULL;
            n = (unsigned)strtoul(window + 20, &end, 10);
            if (!end || *end != ',')
                return 0;
            max_step = (unsigned)strtoul(end + 1, &end, 10);
            if (!end || *end != ',')
                return 0;
            callbacks = (unsigned)strtoul(end + 1, &end, 10);
            if (!end || *end != ' ')
                return 0;
        }
        if (n < 50U || max_step < 50U || callbacks != n)
            return 0;
    }
#endif
    return import_gc(c, state, LUA_GCRESTART, 0, plain, &result);
}

static unsigned count_log(size_t from, const char *needle)
{
    unsigned n = 0U;
    const char *at = s_isaac_log + (from < s_isaac_log_length
                                        ? from : s_isaac_log_length);
    while ((at = strstr(at, needle)) != NULL) {
        ++n;
        at += strlen(needle);
    }
    return n;
}

static int gc_load_chunk(CPU *c, uint32_t state, const char *chunk,
                         size_t length, char *name)
{
    uint32_t load_arguments[] = {
        state, pointer32(chunk), (uint32_t)length, pointer32(name), 0U
    };
    uint32_t pcall_arguments[] = { state, 0U, 0U, 0U, 0U, 0U };
    if (!invoke_import(c, LUA_IMPORT_LOADBUFFERX, load_arguments, 5U) ||
        c->eax != LUA_OK)
        return 0;
    return invoke_import(c, LUA_IMPORT_PCALLK, pcall_arguments, 6U) &&
           c->eax == LUA_OK;
}

#if defined(ISAAC_VITA_LUA_GC_PROFILE)
/* 120 LuaEngine::Update steps -> exactly one new window line, returned. */
static const char *gc_run_window(CPU *c, uint32_t state)
{
    size_t from = s_isaac_log_length;
    unsigned lines = s_isaac_log_lines;
    unsigned i;
    int result;
    for (i = 0U; i < 120U; ++i)
        if (!import_gc(c, state, LUA_GCSTEP, 1, GC_SITE_UPDATE_STEP, &result))
            return NULL;
    if (s_isaac_log_lines != lines + 1U ||
        count_log(from, "[isaac-lua] gc window: ") != 1U)
        return NULL;
    return strstr(s_isaac_log + from, "[isaac-lua] gc window: ");
}
#endif

/* Hostile lua_gc cases (review).  A: near-miss return words (RunScript's
 * 0x40b6ff, the neighbouring instructions, BASE+rva, 0) collect fully and the
 * pinned site passes GCCOUNT/GCCOUNTB/GCSTEP straight through.  B: the clamp
 * has no Lua stack effect and returns 0.  C: finalizers that re-enter lua_gc
 * (nested query, Update step and pinned GCCOLLECT) while the collector runs
 * inside lua_gc; every finalizer still runs once, the nesting depth returns
 * to zero (a clean window follows).  D: a Lua __gc that raises while the
 * pinned GCCOLLECT runs under the game's lua_pcallk: the error longjmps past
 * the gc import frame; the pcall returns LUA_ERRGCMM, the guest stack is
 * restored, and the profile's nesting depth is unwound (unwind=1). */
static int check_gc_hostile(CPU *c, uint32_t state, lua_State *native_state)
{
    static const uint32_t near_miss_sites[] = {
        0x0040b6ffU, 0x003b586eU, 0x003b5874U,
        (uint32_t)GUEST_IMAGE_BASE + 0x003b5870U, 0U
    };
    uint32_t plain = (uint32_t)IMPORT_RETURN + LUA_IMPORT_GC;
    int result;
    int base_kb;
    int count_kb;
    int pinned_kb;
    size_t from;
    size_t i;

    if (!import_gc(c, state, LUA_GCSTOP, 0, plain, &result) ||
        !import_gc(c, state, LUA_GCCOLLECT, 0, plain, &result) ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &base_kb))
        return 0;

    /* A: near misses collect fully, silently. */
    for (i = 0U; i < sizeof near_miss_sites / sizeof near_miss_sites[0]; ++i) {
        from = s_isaac_log_length;
        if (!gc_load_chunk(c, state, s_gc_small_chunk,
                           sizeof s_gc_small_chunk - 1U,
                           s_gc_small_chunk_name) ||
            !import_gc(c, state, LUA_GCCOUNT, 0, plain, &count_kb) ||
            count_kb < base_kb + 1024 ||
            !import_gc(c, state, LUA_GCCOLLECT, 0, near_miss_sites[i],
                       &result) ||
            result != 0 ||
            !import_gc(c, state, LUA_GCCOUNT, 0, plain, &count_kb) ||
            count_kb > base_kb + 256 ||
            count_log(from, "gccollect clamp:") != 0U)
            return 0;
    }
    /* A: the pinned return word with any other `what` is a pass-through. */
    from = s_isaac_log_length;
    if (!import_gc(c, state, LUA_GCCOUNT, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &pinned_kb) ||
        !import_gc(c, state, LUA_GCCOUNT, 0, plain, &count_kb) ||
        pinned_kb != count_kb ||
        !import_gc(c, state, LUA_GCCOUNTB, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &result) ||
        result < 0 || result >= 1024 ||
        !import_gc(c, state, LUA_GCSTEP, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &result) ||
        !import_gc(c, state, LUA_GCISRUNNING, 0, GC_SITE_LEVEL_INIT_COLLECT,
                   &result) ||
        result != 0 ||
        count_log(from, "gccollect clamp:") != 0U)
        return 0;

    /* B: no stack effect, cdecl result 0. */
    {
        uint32_t gettop_arguments[] = { state };
        uint32_t settop_arguments[] = { state, (uint32_t)-3 };
        int top;
        if (!import_pushinteger(c, state, 0x1234567) ||
            !import_pushinteger(c, state, 0x7654321) ||
            !invoke_import(c, LUA_IMPORT_GETTOP, gettop_arguments, 1U))
            return 0;
        top = (int32_t)c->eax;
        if (!gc_load_chunk(c, state, s_gc_small_chunk,
                           sizeof s_gc_small_chunk - 1U,
                           s_gc_small_chunk_name) ||
            !import_gc(c, state, LUA_GCCOLLECT, 0, GC_SITE_LEVEL_INIT_COLLECT,
                       &result) ||
            result != 0 ||
            !invoke_import(c, LUA_IMPORT_GETTOP, gettop_arguments, 1U) ||
            (int32_t)c->eax != top ||
            lua_tointeger(native_state, -1) != 0x7654321 ||
            lua_tointeger(native_state, -2) != 0x1234567 ||
            !invoke_import(c, LUA_IMPORT_SETTOP, settop_arguments, 2U))
            return 0;
    }

    /* C: re-entrant finalizers. */
    {
        unsigned plain_before = s_guest_finalizer_calls;
        unsigned nested_before = s_guest_finalizer_nested_calls;
        uint32_t esp = c->esp;
        if (!import_gc(c, state, LUA_GCCOLLECT, 0, plain, &result) ||
            !gc_load_chunk(c, state, s_gc_nested_chunk,
                           sizeof s_gc_nested_chunk - 1U,
                           s_gc_nested_chunk_name))
            return 0;
        from = s_isaac_log_length;
        if (!import_gc(c, state, LUA_GCCOLLECT, 0, GC_SITE_LEVEL_INIT_COLLECT,
                       &result) ||
            result != 0)
            return 0;
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
        {
            const char *window = gc_run_window(c, state);
            if (!window || !strstr(window, "gc window: win=2 "
                                           "gcstep(n,sum_us,p95_us,max_us)=120,"))
                return 0;
        }
#endif
        if (!import_gc(c, state, LUA_GCCOLLECT, 0, plain, &result) ||
            result != 0 || c->fault || c->esp != esp ||
            s_guest_finalizer_calls != plain_before + 20U ||
            s_guest_finalizer_nested_calls != nested_before + 2U)
            return 0;
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
        /* The outer pinned call plus one nested pinned call per nested
         * finalizer, each logged once. */
        if (count_log(from, "gccollect clamp:") != 3U)
            return 0;
#else
        if (count_log(from, "gccollect clamp:") != 0U)
            return 0;
#endif
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
        {
            const char *window = gc_run_window(c, state);
            if (!window ||
                !strstr(window, "gc window: win=3 "
                                "gcstep(n,sum_us,p95_us,max_us)=120,") ||
                !strstr(window, " fin(n,max_step,cb)=0,0,0 ") ||
                !strstr(window, " kb(req,app)=120,120 "
                                "collect(clamp,full,us)=0,1,") ||
                !strstr(window, " other=0 unwind=0"))
                return 0;
        }
#endif
    }

    /* D: a raising __gc under lua_pcallk. */
    {
        uint32_t getglobal_arguments[] = { state, pointer32(s_level_init_name) };
        uint32_t pcall_arguments[] = { state, 0U, 0U, 0U, 0U, 0U };
        uint32_t settop_arguments[] = { state, (uint32_t)-2 };
        unsigned attempts = 0U;
        unsigned level_inits = s_guest_level_init_calls;
        uint32_t esp = c->esp;
        int status = LUA_OK;
        const char *message;

        if (!import_gc(c, state, LUA_GCCOLLECT, 0, plain, &result) ||
            !gc_load_chunk(c, state, s_gc_error_chunk,
                           sizeof s_gc_error_chunk - 1U,
                           s_gc_error_chunk_name))
            return 0;
        from = s_isaac_log_length;
        while (attempts < 64U && status == LUA_OK) {
            ++attempts;
            if (!invoke_import(c, LUA_IMPORT_GETGLOBAL, getglobal_arguments,
                               2U) ||
                !invoke_import(c, LUA_IMPORT_PCALLK, pcall_arguments, 6U))
                return 0;
            status = (int32_t)c->eax;
        }
        message = lua_tostring(native_state, -1);
        if (status != LUA_ERRGCMM || c->fault || c->esp != esp ||
            s_guest_level_init_calls != level_inits + attempts ||
            !message || !strstr(message, "error in __gc metamethod") ||
            !strstr(message, "boom") ||
            !invoke_import(c, LUA_IMPORT_SETTOP, settop_arguments, 2U))
            return 0;
        /* The raising attempt longjmps before the clamp line is logged. */
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
        if (count_log(from, "gccollect clamp:") != attempts - 1U)
            return 0;
#else
        if (attempts != 1U || count_log(from, "gccollect clamp:") != 0U)
            return 0;
#endif
#if defined(ISAAC_VITA_LUA_GC_PROFILE)
        {
            const char *window = gc_run_window(c, state);
            char expected[64];
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
            snprintf(expected, sizeof expected,
                     " collect(clamp,full,us)=%u,1,", attempts - 1U);
#else
            snprintf(expected, sizeof expected,
                     " collect(clamp,full,us)=0,%u,", attempts);
#endif
            if (!window ||
                !strstr(window, "gc window: win=4 "
                                "gcstep(n,sum_us,p95_us,max_us)=120,") ||
                !strstr(window, " fin(n,max_step,cb)=0,0,0 ") ||
                !strstr(window, expected) ||
                !strstr(window, " unwind=1"))
                return 0;
        }
#endif
    }
    return import_gc(c, state, LUA_GCRESTART, 0, plain, &result);
}

int main(int argc, char **argv)
{
    static const uint32_t addresses[] = {
        GUEST_ALLOCATOR, GUEST_ADD, GUEST_FAIL, GUEST_BADCHECK,
        GUEST_FORMAT_FAIL, GUEST_REQUIRE_FAIL, GUEST_PUSHFSTRING,
        GUEST_BADFORMAT, GUEST_BADPUSHFSTRING, GUEST_LEVEL_INIT,
        GUEST_INDEX_REF, GUEST_GETTER, GUEST_GETTER_BASE, GUEST_BAD_GETTER,
        GUEST_NEWOBJECT, GUEST_BOXED_GETTER, GUEST_NESTED_GETTER,
        GUEST_INDEX_META, GUEST_FINALIZER, GUEST_FINALIZER_NESTED
    };
    static const guest_fn functions[] = {
        guest_allocator, guest_add, guest_fail, guest_badcheck,
        guest_format_fail, guest_require_fail, guest_pushfstring,
        guest_badformat, guest_badpushfstring, guest_level_init,
        guest_index_meta, guest_getter, guest_getter_base, guest_bad_getter,
        guest_newobject, guest_boxed_getter, guest_nested_getter,
        guest_index_meta, guest_finalizer, guest_finalizer_nested
    };
    CPU cpu;
    uint32_t state;
    lua_State *native_state;
    uint32_t newstate_arguments[] = { GUEST_ALLOCATOR, 0x12345678U };
    uint32_t openlibs_arguments[1];
    uint32_t requiref_arguments[4];
    uint32_t load_arguments[3];
    uint32_t pcall_arguments[6];
    uint32_t getglobal_arguments[2];
    uint32_t getfield_arguments[3];
    uint32_t pushstring_arguments[2];
    uint32_t setfield_arguments[3];
    uint32_t settop_arguments[2];
    uint32_t close_arguments[1];
    int64_t result;
    int missing_length;
    char expected_package_path[ISAAC_VITA_STARTUP_PATH_MAX + 80U];
    const char *native_separator;
    const char *native_mod_marker;
    const char *native_core_suffix;
    const char *actual_package_path;
    size_t native_directory_length;
    size_t native_root_length;
    int package_length;

    if (argc != 2 && !(argc == 3 && strcmp(argv[2], "trip") == 0)) {
        fprintf(stderr, "usage: vita-lua-runtime-oracle <main.lua> [trip]\n");
        return 2;
    }
    s_native_script_path = argv[1];
    missing_length = snprintf(
        s_native_missing_path, sizeof s_native_missing_path,
        "%s.isaac-oracle-missing", argv[1]);
    if (missing_length < 0 ||
        (size_t)missing_length >= sizeof s_native_missing_path) {
        fprintf(stderr, "oracle script path exceeds Vita path bound\n");
        return 2;
    }
    /* The driver parses stdout; keep every line ahead of a possible abort. */
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&cpu, 0, sizeof cpu);
    /* Bound like the production CPU (guest_stack_bind): the guarded stack
     * checks run here; the probe CPUs below stay unbound (legacy guard). */
    cpu.stack_owner = &cpu;
    cpu.stack_floor = pointer32(s_guest_stack);
    cpu.stack_ceiling = pointer32(s_guest_stack +
                                  sizeof s_guest_stack / sizeof s_guest_stack[0]);
    cpu.stack_low_water = cpu.stack_ceiling;
    cpu.esp = cpu.stack_ceiling;
    cpu.ebp = cpu.esp;
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
    if (isaac_vita_lua_import_fast_prepare(1) != 13U) {
        fprintf(stderr, "typed Lua endpoint table did not bind 13 names\n");
        return 2;
    }
#endif
    guest_register(addresses, functions,
                   (uint32_t)(sizeof addresses / sizeof addresses[0]));

    if (!invoke_import(&cpu, LUA_IMPORT_NEWSTATE, newstate_arguments, 2U))
        return fail(&cpu, NULL, "lua_newstate bridge failed");
    state = cpu.eax;
    native_state = (lua_State *)(uintptr_t)state;
    if (!state)
        return fail(&cpu, NULL, "guest allocator could not create Lua state");
    (void)lua_atpanic(native_state, oracle_panic);

    openlibs_arguments[0] = state;
    if (!invoke_import(&cpu, LUA_IMPORT_OPENLIBS, openlibs_arguments, 1U))
        return fail(&cpu, native_state, "luaL_openlibs bridge failed");
    requiref_arguments[0] = state;
    requiref_arguments[1] = pointer32(s_table_module);
    requiref_arguments[2] = (uint32_t)GUEST_IMAGE_BASE + 0x005e159dU;
    requiref_arguments[3] = 1U;
    if (!invoke_import(&cpu, LUA_IMPORT_REQUIREF, requiref_arguments, 4U))
        return fail(&cpu, native_state, "frozen luaL_requiref thunk map failed");
    lua_pop(native_state, 1);
    if (!register_closure(&cpu, state, GUEST_ADD, s_add_name, 1) ||
        !register_closure(&cpu, state, GUEST_FAIL, s_fail_name, 0) ||
        !register_closure(&cpu, state, GUEST_BADCHECK,
                          s_badcheck_name, 0) ||
        !register_closure(&cpu, state, GUEST_FORMAT_FAIL,
                          s_format_fail_name, 0) ||
        !register_closure(&cpu, state, GUEST_REQUIRE_FAIL,
                          s_require_fail_name, 0) ||
        !register_closure(&cpu, state, GUEST_PUSHFSTRING,
                          s_pushfstring_name, 0) ||
        !register_closure(&cpu, state, GUEST_BADFORMAT,
                          s_badformat_name, 0) ||
        !register_closure(&cpu, state, GUEST_BADPUSHFSTRING,
                          s_badpushfstring_name, 0) ||
        !register_closure(&cpu, state, GUEST_FINALIZER,
                          s_finalizer_name, 0) ||
        !register_closure(&cpu, state, GUEST_FINALIZER_NESTED,
                          s_finalizer_nested_name, 0) ||
        !register_closure(&cpu, state, GUEST_LEVEL_INIT,
                          s_level_init_name, 0) ||
        !register_closure(&cpu, state, GUEST_INDEX_META,
                          s_native_index_name, 0) ||
        !register_closure(&cpu, state, GUEST_INDEX_REF, s_ref_index_name, 0) ||
        !register_closure(&cpu, state, GUEST_GETTER, s_getter_name, 0) ||
        !register_closure(&cpu, state, GUEST_GETTER_BASE,
                          s_getter_base_name, 0) ||
        !register_closure(&cpu, state, GUEST_BAD_GETTER,
                          s_bad_getter_name, 0) ||
        !register_closure(&cpu, state, GUEST_NEWOBJECT, s_newobject_name, 0) ||
        !register_closure(&cpu, state, GUEST_BOXED_GETTER,
                          s_boxed_getter_name, 0) ||
        !register_closure(&cpu, state, GUEST_NESTED_GETTER,
                          s_nested_getter_name, 0))
        return fail(&cpu, native_state, "guest CFunction registration failed");
    /* "trip" (argv[2]) arms main.lua's VERIFY-trip scenarios: a getter whose
     * result is a fresh table per call must disable the native replay. */
    if (argc == 3) {
        uint32_t trip_arguments[] = { state, pointer32(s_index_trip_name) };
        if (!import_pushinteger(&cpu, state, 1) ||
            !invoke_import(&cpu, LUA_IMPORT_SETGLOBAL, trip_arguments, 2U))
            return fail(&cpu, native_state, "index_trip global failed");
    }

    /* The reference rendering is the native lua_pushfstring with the same
     * values; the bridge must reproduce it byte-for-byte from the x86 stack,
     * and well past the old 255-byte error-text cap.  Render it in a private
     * state: the bridge state's guest allocator only serves calls made under
     * an active import scope. */
    {
        lua_State *reference = luaL_newstate();
        const char *text;
        size_t length;
        uint32_t expected_arguments[2];
        uint32_t expected_global_arguments[2];

        if (!reference)
            return fail(&cpu, NULL, "reference Lua state creation failed");
        memset(s_format_long_text, 'x', sizeof s_format_long_text - 1U);
        s_format_long_text[sizeof s_format_long_text - 1U] = '\0';
        text = lua_pushfstring(reference, s_guest_format, s_format_name,
                               s_format_long_text, -7,
                               (lua_Integer)INT64_C(0x1122334455667788), 2.5,
                               (int)'Q', (void *)s_guest_error, (long)0x20AC);
        length = lua_rawlen(reference, -1);
        if (!text || length <= 255U || length >= sizeof s_expected_format) {
            lua_close(reference);
            return fail(&cpu, NULL, "format reference has an unexpected size");
        }
        memcpy(s_expected_format, text, length + 1U);
        lua_close(reference);
        expected_arguments[0] = state;
        expected_arguments[1] = pointer32(s_expected_format);
        expected_global_arguments[0] = state;
        expected_global_arguments[1] = pointer32(s_expected_name);
        if (!invoke_import(&cpu, LUA_IMPORT_PUSHSTRING,
                           expected_arguments, 2U) ||
            !invoke_import(&cpu, LUA_IMPORT_SETGLOBAL,
                           expected_global_arguments, 2U))
            return fail(&cpu, native_state, "expected_format global failed");
    }

    /* Mirror the frozen RunScript contract before crossing luaL_loadfilex:
     * the engine exposes its guest-visible script dirname to package.path. */
    getglobal_arguments[0] = state;
    getglobal_arguments[1] = pointer32(s_package_name);
    pushstring_arguments[0] = state;
    pushstring_arguments[1] = pointer32(s_guest_package_path);
    setfield_arguments[0] = state;
    setfield_arguments[1] = (uint32_t)-2;
    setfield_arguments[2] = pointer32(s_path_name);
    settop_arguments[0] = state;
    settop_arguments[1] = 0U;
    if (!invoke_import(&cpu, LUA_IMPORT_GETGLOBAL,
                       getglobal_arguments, 2U) ||
        !invoke_import(&cpu, LUA_IMPORT_PUSHSTRING,
                       pushstring_arguments, 2U) ||
        !invoke_import(&cpu, LUA_IMPORT_SETFIELD,
                       setfield_arguments, 3U) ||
        !invoke_import(&cpu, LUA_IMPORT_SETTOP, settop_arguments, 2U))
        return fail(&cpu, native_state,
                    "frozen RunScript package.path setup failed");

    load_arguments[0] = state;
    load_arguments[1] = pointer32(s_guest_mod_path);
    load_arguments[2] = 0U;
    if (!invoke_import(&cpu, LUA_IMPORT_LOADFILEX, load_arguments, 3U) ||
        cpu.eax != LUA_OK || s_path_map_calls != 1U ||
        s_path_map_last_guest != load_arguments[1] ||
        s_path_map_last_capacity != ISAAC_VITA_STARTUP_PATH_MAX + 1U)
        return fail(&cpu, native_state, "main.lua did not load");

    native_separator = strrchr(s_native_script_path, '/');
    if (!native_separator)
        native_separator = strrchr(s_native_script_path, '\\');
    if (!native_separator)
        return fail(&cpu, native_state, "native main.lua has no directory");
    native_directory_length =
        (size_t)(native_separator - s_native_script_path);
    native_mod_marker = strstr(s_native_script_path, "/mods/");
    native_core_suffix = "/resources/scripts/?.lua";
    if (!native_mod_marker) {
        native_mod_marker = strstr(s_native_script_path, "\\mods\\");
        native_core_suffix = "\\resources\\scripts\\?.lua";
    }
    if (!native_mod_marker)
        return fail(&cpu, native_state,
                    "native main.lua is not below the mod root");
    native_root_length =
        (size_t)(native_mod_marker - s_native_script_path);
    package_length = snprintf(
        expected_package_path, sizeof expected_package_path,
        "%.*s/?.lua;%.*s%s",
        (int)native_directory_length, s_native_script_path,
        (int)native_root_length, s_native_script_path,
        native_core_suffix);
    if (package_length < 0 ||
        (size_t)package_length >= sizeof expected_package_path)
        return fail(&cpu, native_state, "expected package.path overflowed");
    getfield_arguments[0] = state;
    getfield_arguments[1] = (uint32_t)-1;
    getfield_arguments[2] = pointer32(s_path_name);
    if (!invoke_import(&cpu, LUA_IMPORT_GETGLOBAL,
                       getglobal_arguments, 2U) ||
        !invoke_import(&cpu, LUA_IMPORT_GETFIELD,
                       getfield_arguments, 3U))
        return fail(&cpu, native_state,
                    "rebased package.path lookup failed");
    actual_package_path = lua_tostring(native_state, -1);
    if (!actual_package_path ||
        strcmp(actual_package_path, expected_package_path) != 0)
        return fail(&cpu, native_state,
                    "mod/core package.path was not natively rebased");
    settop_arguments[1] = 1U; /* retain the loaded main chunk */
    if (!invoke_import(&cpu, LUA_IMPORT_SETTOP, settop_arguments, 2U))
        return fail(&cpu, native_state,
                    "rebased package.path stack cleanup failed");

    pcall_arguments[0] = state;
    pcall_arguments[1] = 0U;
    pcall_arguments[2] = 0U;
    pcall_arguments[3] = 0U;
    pcall_arguments[4] = 0U;
    pcall_arguments[5] = 0U;
    if (!invoke_import(&cpu, LUA_IMPORT_PCALLK, pcall_arguments, 6U) ||
        cpu.eax != LUA_OK)
        return fail(&cpu, native_state, "main.lua execution failed");

    getglobal_arguments[0] = state;
    getglobal_arguments[1] = pointer32(s_result_name);
    if (!invoke_import(&cpu, LUA_IMPORT_GETGLOBAL,
                       getglobal_arguments, 2U))
        return fail(&cpu, native_state, "result global lookup failed");
    result = import_checkinteger(&cpu, state, -1);
    /* guest_fail: once directly, then once per __index hierarchy through a
     * method hit (obj.fail()). */
    if (cpu.fault || result != 42 || s_guest_add_calls != 1U ||
        s_guest_fail_calls != 3U || s_guest_badcheck_calls != 1U)
        return fail(&cpu, native_state,
                    "callback result/upvalue/error boundary mismatch");
    if (s_guest_format_fail_calls != 2U || s_guest_require_fail_calls != 1U ||
        s_guest_pushfstring_calls != 1U || s_guest_badformat_calls != 1U ||
        s_guest_badpushfstring_calls != 1U)
        return fail(&cpu, native_state,
                    "guest vararg format callbacks were not all exercised");
    if (!check_loadfile_boundary(&cpu, state, native_state))
        return fail(&cpu, native_state,
                    "luaL_loadfilex path/fallback/error ABI mismatch");
    if (!check_gc_policy(&cpu, state))
        return fail(&cpu, native_state,
                    "lua_gc GCCOLLECT clamp / GC profile policy mismatch");
    if (!check_gc_hostile(&cpu, state, native_state))
        return fail(&cpu, native_state,
                    "lua_gc hostile cases (near-miss sites, stack effect, "
                    "re-entrant finalizers, raising __gc under pcall) failed");
    if (!s_guest_index_meta_calls || !s_guest_getter_calls ||
        !s_guest_getter_base_calls || !s_guest_bad_getter_calls ||
        !s_guest_newobject_calls || !s_guest_boxed_getter_calls ||
        !s_guest_nested_getter_calls)
        return fail(&cpu, native_state,
                    "LuaBridge __index fixture callbacks were not all exercised");
    if (!print_index_digest(&cpu, state, native_state))
        return fail(&cpu, native_state, "LuaBridge __index digest missing");
#if defined(ISAAC_VITA_LUA_NATIVE_INDEX)
    isaac_vita_lua_native_index_report();
#endif

    close_arguments[0] = state;
    if (!invoke_import(&cpu, LUA_IMPORT_CLOSE, close_arguments, 1U))
        return fail(&cpu, native_state, "lua_close bridge failed");
    if (!s_allocator_calls || !s_allocator_frees)
        return fail(&cpu, NULL, "guest allocator callback was not exercised");

    printf("Lua 5.3.3 mapped main.lua + per-mod/core require roots + "
           "loadfile fallback/error ABI + guest vararg luaL_error/pushfstring + "
           "guest CFunction/allocator boundary + lua_gc policy (%s): PASS "
           "(map=%u alloc=%u free=%u finalizers=%u nested=%u "
           "level_inits=%u log_lines=%u)\n",
#if defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP) && defined(ISAAC_VITA_LUA_GC_PROFILE)
           "GCCOLLECT clamp at 003b5870 + gc window",
#elif defined(ISAAC_VITA_LUA_GCCOLLECT_CLAMP)
           "GCCOLLECT clamp at 003b5870",
#elif defined(ISAAC_VITA_LUA_GC_PROFILE)
           "gc window",
#else
           "knobs OFF: pinned site collects fully, nothing logged",
#endif
           s_path_map_calls, s_allocator_calls, s_allocator_frees,
           s_guest_finalizer_calls, s_guest_finalizer_nested_calls,
           s_guest_level_init_calls, s_isaac_log_lines);
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE)
    {
        extern unsigned g_isaac_vita_lua_fast_typed_runs;
        /* settop/gettop/pushstring/pushinteger ran typed on the bound CPU;
         * the unbound probe CPUs took the generic fallback. */
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
