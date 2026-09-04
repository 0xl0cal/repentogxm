/* ILP32 host oracle for ISAAC_VITA_LUA_IMPORT_FASTDISPATCH (host_vita_lua.c).
 *
 * Runs every typed Lua endpoint against the generic handler of the same
 * import on the same CPU, x86 stack and Lua stack state and requires the
 * complete observable outcome to be identical: EAX, EDX, ESP, the x87 ring
 * and its top, the stack low-water diagnostic, the fault record, the host
 * call census and a dump of the whole Lua stack (type + value / identity of
 * every slot).  The same comparison is made for the two fallback shapes --
 * an unbound CPU and a frame that ends past the stack ceiling -- where the
 * typed endpoint must take the generic handler itself (proved through the
 * oracle-only typed-body counter), and the name-bound table is checked to
 * hold exactly the thirteen typed imports.
 *
 * Built by recomp/test_vita_lua_fastdispatch.py with pristine Lua 5.3.3 and
 * the production host_vita_lua.c (ISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1,
 * ISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE=1) as a 32-bit MSVC executable. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "host_vita_lua.h"
#include "host_vita_startup.h"

#if !defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH) || \
    !defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH_ORACLE)
#error This oracle requires ISAAC_VITA_LUA_IMPORT_FASTDISPATCH and its ORACLE seam
#endif

extern unsigned g_isaac_vita_lua_fast_typed_runs;

#define IMPORT_RETURN UINT32_C(0xfff16000)
enum {
    STACK_WORDS = 4096U,
    STACK_CEILING_WORD = 4000U,   /* words past it exist but are outside */
    FIXTURE_TOP = 7,
    DUMP_MAX = 4096
};

_Alignas(16) static uint32_t s_guest_stack[STACK_WORDS];
static int s_ref_table;
static int s_ref_userdata;
static int s_ref_metatable;
static char s_rawgetp_key;
static char s_short_string[] = "str";
static char s_push_string[] = "typed";
static unsigned s_failures;

static uint32_t pointer32(const void *pointer)
{
    if ((uintptr_t)pointer > UINT32_MAX) {
        fprintf(stderr, "oracle pointer does not fit frozen 32-bit ABI\n");
        exit(2);
    }
    return (uint32_t)(uintptr_t)pointer;
}

/* ------------------------------------------------ runtime stubs (host) --- */

/* Production guest_fault is non-returning (longjmp to the run scope): a
 * rejected stack read never dereferences the zero address it returned.  The
 * oracle keeps that shape so the generic handler's fault path is the real
 * one, and, like guest_run_until_stop, drops the bridge ownership after. */
typedef struct oracle_scope {
    jmp_buf env;
} oracle_scope;

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    oracle_scope *scope = (oracle_scope *)c->run_scope;

    c->fault = what;
    c->fault_addr = address;
    if (scope)
        longjmp(scope->env, 1);
    fprintf(stderr, "FAIL: unguarded fault at %08x: %s\n", (unsigned)address,
            what);
    exit(2);
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

void guest_call(CPU *__restrict c, uint32_t address)
{
    guest_fault(c, address, "oracle guest_call target is not registered");
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
    guest_fault(c, guest_path, "oracle unexpectedly crossed the path mapper");
    return 0;
}

/* ------------------------------------------------------------ fixture --- */

static void fixture_create(lua_State *L)
{
    void *ud;

    lua_newtable(L);                        /* T */
    lua_pushstring(L, "one");
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, 2.5);
    lua_rawseti(L, -2, 2);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "k");
    ud = lua_newuserdata(L, 16U);           /* U */
    memset(ud, 0x5a, 16U);
    lua_newtable(L);                        /* M */
    lua_pushstring(L, "U");
    lua_setfield(L, -2, "__name");
    lua_pushvalue(L, -1);
    s_ref_metatable = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);                /* U.mt = M */
    lua_pushvalue(L, -1);
    lua_rawsetp(L, -3, &s_rawgetp_key);     /* T[P] = U */
    s_ref_userdata = luaL_ref(L, LUA_REGISTRYINDEX);
    s_ref_table = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Same seven slots, same object identities, every time. */
static void fixture_reset(lua_State *L)
{
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ref_table);      /* 1: T */
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ref_userdata);   /* 2: U */
    lua_pushinteger(L, 42);                              /* 3 */
    lua_pushnumber(L, 3.25);                             /* 4 */
    lua_pushstring(L, s_short_string);                   /* 5 */
    lua_pushnil(L);                                      /* 6 */
    lua_pushboolean(L, 1);                               /* 7 */
}

static void dump_stack(lua_State *L, char *out, size_t capacity)
{
    int top = lua_gettop(L);
    int i;
    size_t used = 0U;

    out[0] = '\0';
    for (i = 1; i <= top && used + 96U < capacity; ++i) {
        int type = lua_type(L, i);
        int written;

        switch (type) {
        case LUA_TNIL:
            written = snprintf(out + used, capacity - used, "%d:nil;", i);
            break;
        case LUA_TBOOLEAN:
            written = snprintf(out + used, capacity - used, "%d:b%d;", i,
                               lua_toboolean(L, i));
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, i))
                written = snprintf(out + used, capacity - used, "%d:i%lld;",
                                   i, (long long)lua_tointeger(L, i));
            else
                written = snprintf(out + used, capacity - used, "%d:n%.17g;",
                                   i, lua_tonumber(L, i));
            break;
        case LUA_TSTRING:
            written = snprintf(out + used, capacity - used, "%d:s%s@%p;", i,
                               lua_tostring(L, i), lua_topointer(L, i));
            break;
        default:
            written = snprintf(out + used, capacity - used, "%d:%s@%p;", i,
                               lua_typename(L, type), lua_topointer(L, i));
            break;
        }
        if (written < 0)
            break;
        used += (size_t)written;
    }
}

/* --------------------------------------------------------------- runs --- */

typedef struct outcome {
    uint32_t eax, edx, esp, ebx, ecx;
    double st[8];
    int st_top;
    uint32_t low_water;
    const char *fault;
    uint32_t fault_addr;
    unsigned calls;
    unsigned typed_runs;
    int handled;
    int faulted;
    int top;
    char dump[DUMP_MAX];
} outcome;

typedef struct import_case {
    const char *name;
    uint32_t nargs;               /* argument slots after the state */
    uint32_t args[4];
    int expect_typed;             /* bound to a typed endpoint? */
} import_case;

static void cpu_prepare(CPU *c, int bound, int32_t esp_offset_from_ceiling)
{
    uint32_t floor = pointer32(s_guest_stack);
    uint32_t ceiling = pointer32(s_guest_stack + STACK_CEILING_WORD);
    int i;

    memset(c, 0, sizeof *c);
    c->eax = UINT32_C(0xdeadbeef);
    c->ecx = UINT32_C(0x11111111);
    c->edx = UINT32_C(0xcafef00d);
    c->ebx = UINT32_C(0x22222222);
    for (i = 0; i < 8; ++i)
        c->st[i] = 1.5 + i;
    c->st_top = 3;
    if (bound) {
        c->stack_owner = c;
        c->stack_floor = floor;
        c->stack_ceiling = ceiling;
        c->stack_low_water = ceiling;
    }
    c->esp = (uint32_t)((int32_t)ceiling - esp_offset_from_ceiling);
    c->ebp = c->esp;
}

static void frame_store(CPU *c, uint32_t state, const import_case *k,
                        uint32_t *esp_out)
{
    /* x86 cdecl as the generated caller lays it out: arguments right to
     * left, then the return word.  Stores are direct so the frame can be
     * placed across the ceiling for the fallback shapes. */
    uint32_t esp = c->esp;
    uint32_t i;

    for (i = k->nargs; i != 0U; --i) {
        esp -= 4U;
        *(uint32_t *)(uintptr_t)esp = k->args[i - 1U];
    }
    esp -= 4U;
    *(uint32_t *)(uintptr_t)esp = state;
    esp -= 4U;
    *(uint32_t *)(uintptr_t)esp = IMPORT_RETURN;
    c->esp = esp;
    *esp_out = esp;
}

static void run_one(lua_State *L, uint32_t index, const import_case *k,
                    int typed, int bound, int32_t esp_offset, outcome *out)
{
    CPU cpu;
    oracle_scope scope;
    uint32_t esp;
    unsigned calls = 7U;
    unsigned typed_before = g_isaac_vita_lua_fast_typed_runs;
    guest_import_family_fn fast = isaac_vita_lua_import_fast_binding(index);

    fixture_reset(L);
    cpu_prepare(&cpu, bound, esp_offset);
    frame_store(&cpu, pointer32(L), k, &esp);
    memset(out, 0, sizeof *out);
    cpu.run_scope = &scope;
    if (setjmp(scope.env) == 0) {
        if (typed) {
            if (!fast) {
                out->handled = -1;
                return;
            }
            out->handled = fast(&cpu, index, &calls);
        } else {
            out->handled = isaac_vita_lua_import_indexed(&cpu, index, &calls);
        }
    } else {
        out->faulted = 1;
        out->handled = -2;
        isaac_vita_lua_abort_cpu(&cpu);
    }
    cpu.run_scope = NULL;
    out->eax = cpu.eax;
    out->edx = cpu.edx;
    out->esp = cpu.esp;
    out->ebx = cpu.ebx;
    out->ecx = cpu.ecx;
    memcpy(out->st, cpu.st, sizeof out->st);
    out->st_top = cpu.st_top;
    out->low_water = cpu.stack_low_water;
    out->fault = cpu.fault;
    out->fault_addr = cpu.fault_addr;
    out->calls = calls;
    out->typed_runs = g_isaac_vita_lua_fast_typed_runs - typed_before;
    out->top = lua_gettop(L);
    dump_stack(L, out->dump, sizeof out->dump);
    (void)esp;
}

static int same_text(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

static int outcomes_equal(const outcome *a, const outcome *b)
{
    return a->eax == b->eax && a->edx == b->edx && a->esp == b->esp &&
           a->ebx == b->ebx && a->ecx == b->ecx &&
           memcmp(a->st, b->st, sizeof a->st) == 0 &&
           a->st_top == b->st_top && a->low_water == b->low_water &&
           same_text(a->fault, b->fault) && a->fault_addr == b->fault_addr &&
           a->calls == b->calls && a->handled == b->handled &&
           a->faulted == b->faulted &&
           a->top == b->top && strcmp(a->dump, b->dump) == 0;
}

static void report(const char *what, const import_case *k, const outcome *g,
                   const outcome *t)
{
    ++s_failures;
    fprintf(stderr, "FAIL %s: %s\n", k->name, what);
    fprintf(stderr,
            "  generic: eax=%08x edx=%08x esp=%08x low=%08x st_top=%d "
            "fault=%s@%08x calls=%u handled=%d top=%d typed_runs=%u\n  %s\n",
            (unsigned)g->eax, (unsigned)g->edx, (unsigned)g->esp,
            (unsigned)g->low_water, g->st_top, g->fault ? g->fault : "-",
            (unsigned)g->fault_addr, g->calls, g->handled, g->top,
            g->typed_runs, g->dump);
    fprintf(stderr,
            "  typed  : eax=%08x edx=%08x esp=%08x low=%08x st_top=%d "
            "fault=%s@%08x calls=%u handled=%d top=%d typed_runs=%u\n  %s\n",
            (unsigned)t->eax, (unsigned)t->edx, (unsigned)t->esp,
            (unsigned)t->low_water, t->st_top, t->fault ? t->fault : "-",
            (unsigned)t->fault_addr, t->calls, t->handled, t->top,
            t->typed_runs, t->dump);
}

static uint32_t find_index(const char *name)
{
    uint32_t i;
    char full[96];

    snprintf(full, sizeof full, "Lua5.3.3r.dll!%s", name);
    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i) {
        const char *candidate = isaac_vita_lua_import_name(i);
        if (candidate && strcmp(candidate, full) == 0)
            return i;
    }
    fprintf(stderr, "FAIL: import %s is not in the frozen inventory\n", name);
    exit(2);
}

static int32_t u32(int32_t v)
{
    return v;
}

int main(void)
{
    lua_State *L;
    void *userdata_pointer;
    const void *table_pointer;
    unsigned differential = 0U;
    unsigned typed_bodies = 0U;
    uint32_t i;
    static const char *const typed_names[13] = {
        "lua_rawgetp", "lua_getmetatable", "lua_type", "lua_touserdata",
        "lua_pushvalue", "lua_rawget", "lua_rawgeti", "lua_settop",
        "lua_gettop", "lua_pushnil", "lua_pushinteger", "lua_pushnumber",
        "lua_pushstring"
    };
    import_case cases[32];
    unsigned case_count = 0U;
    double number_arg = -12345.6789;
    uint64_t number_bits;
    uint64_t integer_arg = (uint64_t)INT64_C(-9000000000);

    L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "FAIL: luaL_newstate\n");
        return 2;
    }
    luaL_openlibs(L);
    fixture_create(L);
    fixture_reset(L);
    userdata_pointer = lua_touserdata(L, 2);
    table_pointer = lua_topointer(L, 1);

    /* 1. binding census: exactly the thirteen names, prepare() idempotent. */
    if (isaac_vita_lua_import_fast_prepare(1) != 13U) {
        fprintf(stderr, "FAIL: prepare(1) did not bind 13 typed endpoints\n");
        return 1;
    }
    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i) {
        const char *name = isaac_vita_lua_import_name(i);
        int expected = 0;
        unsigned j;

        for (j = 0U; j < 13U; ++j) {
            if (strcmp(name + strlen("Lua5.3.3r.dll!"), typed_names[j]) == 0)
                expected = 1;
        }
        if ((isaac_vita_lua_import_fast_binding(i) != NULL) != expected) {
            fprintf(stderr, "FAIL: binding census differs at %u (%s)\n",
                    (unsigned)i, name);
            return 1;
        }
        typed_bodies += (unsigned)expected;
    }
    if (typed_bodies != 13U ||
        isaac_vita_lua_import_fast_binding(ISAAC_VITA_LUA_IMPORT_COUNT) ||
        isaac_vita_lua_import_fast_binding(UINT32_MAX)) {
        fprintf(stderr, "FAIL: binding census count/out-of-range\n");
        return 1;
    }
    (void)isaac_vita_lua_import_fast_prepare(0);
    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i) {
        if (isaac_vita_lua_import_fast_binding(i)) {
            fprintf(stderr, "FAIL: prepare(0) left a binding at %u\n",
                    (unsigned)i);
            return 1;
        }
    }
    if (isaac_vita_lua_import_fast_prepare(1) != 13U) {
        fprintf(stderr, "FAIL: prepare(1) after prepare(0)\n");
        return 1;
    }

    /* 2. cases: every typed import with representative operands, plus two
     *    generic-only imports as controls (their typed spelling is the
     *    generic handler itself, expect_typed=0). */
#define CASE(import_name, n, a0, a1, a2, a3, typed_expected)               \
    do {                                                                   \
        import_case *k_ = &cases[case_count++];                            \
        k_->name = (import_name);                                          \
        k_->nargs = (n);                                                   \
        k_->args[0] = (uint32_t)(a0);                                      \
        k_->args[1] = (uint32_t)(a1);                                      \
        k_->args[2] = (uint32_t)(a2);                                      \
        k_->args[3] = (uint32_t)(a3);                                      \
        k_->expect_typed = (typed_expected);                               \
    } while (0)
    memcpy(&number_bits, &number_arg, sizeof number_bits);
    CASE("lua_rawgetp", 2U, 1, pointer32(&s_rawgetp_key), 0, 0, 1);
    CASE("lua_rawgetp", 2U, 1, pointer32(&s_short_string), 0, 0, 1); /* miss */
    CASE("lua_getmetatable", 1U, 2, 0, 0, 0, 1);
    CASE("lua_getmetatable", 1U, 3, 0, 0, 0, 1);                      /* none */
    CASE("lua_type", 1U, 5, 0, 0, 0, 1);
    CASE("lua_type", 1U, u32(-1), 0, 0, 0, 1);
    CASE("lua_type", 1U, 40, 0, 0, 0, 1);                            /* LUA_TNONE */
    CASE("lua_touserdata", 1U, 2, 0, 0, 0, 1);
    CASE("lua_touserdata", 1U, 3, 0, 0, 0, 1);                       /* NULL */
    CASE("lua_pushvalue", 1U, 1, 0, 0, 0, 1);
    CASE("lua_pushvalue", 1U, u32(-2), 0, 0, 0, 1);
    CASE("lua_pushvalue", 1U, LUA_REGISTRYINDEX, 0, 0, 0, 1);
    CASE("lua_rawget", 1U, 1, 0, 0, 0, 1);      /* key = slot 7 (true) */
    CASE("lua_rawgeti", 3U, 1, (uint32_t)2, 0U, 0, 1);
    CASE("lua_rawgeti", 3U, 1, (uint32_t)7, 0U, 0, 1);               /* nil */
    CASE("lua_rawgeti", 3U, LUA_REGISTRYINDEX, (uint32_t)s_ref_table,
         0U, 0, 1);
    CASE("lua_settop", 1U, 3, 0, 0, 0, 1);
    CASE("lua_settop", 1U, 9, 0, 0, 0, 1);
    CASE("lua_settop", 1U, u32(-3), 0, 0, 0, 1);
    CASE("lua_gettop", 0U, 0, 0, 0, 0, 1);
    CASE("lua_pushnil", 0U, 0, 0, 0, 0, 1);
    CASE("lua_pushinteger", 2U, (uint32_t)integer_arg,
         (uint32_t)(integer_arg >> 32U), 0, 0, 1);
    CASE("lua_pushinteger", 2U, 7U, 0U, 0, 0, 1);
    CASE("lua_pushnumber", 2U, (uint32_t)number_bits,
         (uint32_t)(number_bits >> 32U), 0, 0, 1);
    CASE("lua_pushstring", 1U, pointer32(s_push_string), 0, 0, 0, 1);
    CASE("lua_pushstring", 1U, 0U, 0, 0, 0, 1);                      /* NULL */
    CASE("lua_isstring", 1U, 5, 0, 0, 0, 0);                         /* control */
    CASE("lua_toboolean", 1U, 7, 0, 0, 0, 0);                        /* control */
#undef CASE

    for (i = 0U; i < case_count; ++i) {
        const import_case *k = &cases[i];
        uint32_t index = find_index(k->name);
        outcome generic, typed;

        if ((isaac_vita_lua_import_fast_binding(index) != NULL) !=
            k->expect_typed) {
            fprintf(stderr, "FAIL %s: binding expectation\n", k->name);
            return 1;
        }
        if (!k->expect_typed)
            continue;

        /* (a) bound stack, frame well inside: the typed body runs. */
        run_one(L, index, k, 0, 1, 64, &generic);
        run_one(L, index, k, 1, 1, 64, &typed);
        ++differential;
        if (!outcomes_equal(&generic, &typed))
            report("bound stack: typed differs from generic", k, &generic,
                   &typed);
        if (generic.typed_runs != 0U || typed.typed_runs != 1U)
            report("bound stack: typed body census", k, &generic, &typed);
        if (generic.fault || generic.calls != 8U || generic.handled != 1 ||
            generic.esp != pointer32(s_guest_stack + STACK_CEILING_WORD) -
                           64U - 4U * k->nargs - 4U)
            report("bound stack: generic outcome shape", k, &generic, &typed);

        /* (b) unbound CPU (legacy host tests): the typed endpoint must take
         *     the generic handler itself. */
        run_one(L, index, k, 0, 0, 64, &generic);
        run_one(L, index, k, 1, 0, 64, &typed);
        ++differential;
        if (!outcomes_equal(&generic, &typed))
            report("unbound CPU: typed differs from generic", k, &generic,
                   &typed);
        if (typed.typed_runs != 0U)
            report("unbound CPU: typed body ran", k, &generic, &typed);

        /* (c) frame laid down from ceiling+4: its last argument slot (the
         *     state slot for the argument-free imports) is the word past the
         *     ceiling.  The generic handler faults on that slot; the typed
         *     endpoint's single range check rejects the frame and must yield
         *     the identical fault by running the generic handler itself. */
        run_one(L, index, k, 0, 1, -4, &generic);
        run_one(L, index, k, 1, 1, -4, &typed);
        ++differential;
        if (!outcomes_equal(&generic, &typed))
            report("short stack: typed differs from generic", k, &generic,
                   &typed);
        if (typed.typed_runs != 0U || !generic.fault ||
            !same_text(generic.fault, "oracle guest stack violation"))
            report("short stack: expected the generic stack fault", k,
                   &generic, &typed);
        /* (d) frame ending exactly at the ceiling: both succeed, typed body
         *     runs (the range check's inclusive upper edge). */
        run_one(L, index, k, 0, 1, 0, &generic);
        run_one(L, index, k, 1, 1, 0, &typed);
        ++differential;
        if (!outcomes_equal(&generic, &typed))
            report("exact frame: typed differs from generic", k, &generic,
                   &typed);
        if (generic.fault || typed.typed_runs != 1U)
            report("exact frame: expected success through the typed body", k,
                   &generic, &typed);
    }

    /* 3. spot semantics on the typed route (the differential already proves
     *    identity; these pin that the fixture exercised real Lua work). */
    {
        outcome t;
        import_case k;

        memset(&k, 0, sizeof k);
        k.name = "lua_touserdata"; k.nargs = 1U; k.args[0] = 2U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != pointer32(userdata_pointer))
            report("touserdata result", &k, &t, &t);
        k.name = "lua_gettop"; k.nargs = 0U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != (uint32_t)FIXTURE_TOP)
            report("gettop result", &k, &t, &t);
        k.name = "lua_type"; k.nargs = 1U; k.args[0] = 5U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != (uint32_t)LUA_TSTRING)
            report("type result", &k, &t, &t);
        k.name = "lua_getmetatable"; k.nargs = 1U; k.args[0] = 2U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != 1U || t.top != FIXTURE_TOP + 1)
            report("getmetatable result", &k, &t, &t);
        k.name = "lua_rawgetp"; k.nargs = 2U; k.args[0] = 1U;
        k.args[1] = pointer32(&s_rawgetp_key);
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != (uint32_t)LUA_TUSERDATA || t.top != FIXTURE_TOP + 1 ||
            lua_touserdata(L, -1) != userdata_pointer)
            report("rawgetp result", &k, &t, &t);
        k.name = "lua_rawgeti"; k.nargs = 3U; k.args[0] = 1U; k.args[1] = 2U;
        k.args[2] = 0U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != (uint32_t)LUA_TNUMBER || lua_tonumber(L, -1) != 2.5)
            report("rawgeti result", &k, &t, &t);
        k.name = "lua_pushstring"; k.nargs = 1U;
        k.args[0] = pointer32(s_push_string);
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != pointer32(lua_tostring(L, -1)) ||
            strcmp(lua_tostring(L, -1), s_push_string) != 0)
            report("pushstring result", &k, &t, &t);
        k.name = "lua_pushnumber"; k.nargs = 2U;
        k.args[0] = (uint32_t)number_bits;
        k.args[1] = (uint32_t)(number_bits >> 32U);
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (lua_tonumber(L, -1) != number_arg)
            report("pushnumber result", &k, &t, &t);
        k.name = "lua_pushinteger"; k.nargs = 2U;
        k.args[0] = (uint32_t)integer_arg;
        k.args[1] = (uint32_t)(integer_arg >> 32U);
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if ((uint64_t)lua_tointeger(L, -1) != integer_arg)
            report("pushinteger result", &k, &t, &t);
        k.name = "lua_settop"; k.nargs = 1U; k.args[0] = 3U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.top != 3)
            report("settop result", &k, &t, &t);
        k.name = "lua_pushvalue"; k.nargs = 1U; k.args[0] = 1U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (lua_topointer(L, -1) != table_pointer)
            report("pushvalue result", &k, &t, &t);
        k.name = "lua_rawget"; k.nargs = 1U; k.args[0] = 1U;
        run_one(L, find_index(k.name), &k, 1, 1, 64, &t);
        if (t.eax != (uint32_t)LUA_TNIL || t.top != FIXTURE_TOP)
            report("rawget result", &k, &t, &t);
    }

    lua_close(L);
    if (s_failures) {
        fprintf(stderr, "Vita Lua import fast dispatch oracle: %u failures\n",
                s_failures);
        return 1;
    }
    printf("Vita Lua import fast dispatch oracle: PASS (%u typed endpoints; "
           "%u differential runs x4 shapes: bound, unbound fallback, "
           "short-stack fallback, exact frame)\n",
           typed_bodies, differential);
    return 0;
}
