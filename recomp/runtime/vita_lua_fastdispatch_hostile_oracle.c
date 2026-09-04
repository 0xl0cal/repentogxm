/* Hostile ILP32 differential oracle for ISAAC_VITA_LUA_IMPORT_FASTDISPATCH
 * (host_vita_lua.c), the companion of vita_lua_fastdispatch_oracle.c.
 *
 * Scenarios the shipped oracle does not cover: nested typed imports inside a
 * generic lua_pcallk root (the game's common shape: LuaBridge binding called
 * from Lua), a typed root whose Lua call re-enters guest code through a __gc
 * finalizer (typed lua_pushstring -> GC step -> guest __gc -> nested typed
 * imports), a foreign CPU entering while the owner word is held, an owner
 * word cleared underneath a typed root, a Lua error thrown through an
 * abandoned nested typed frame and recovered by the pcallk root, a frame
 * straddling the stack floor, a corrupt low-water diagnostic, a misaligned
 * ESP and a frame whose return word sits exactly at the floor.
 *
 * Every scenario runs twice on one Lua state: all imports through the
 * generic handlers, then all imports through the typed table where bound.
 * The complete observable outcome (EAX, ESP, low-water, fault, host census,
 * Lua stack dump, callback transcript) must be identical; after each run a
 * root import on a second CPU must succeed, proving the owner word was
 * released.
 *
 * Built and run by recomp/test_vita_lua_fastdispatch.py next to the main
 * oracle (same pristine Lua 5.3.3 + production host_vita_lua.c objects). */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "host_vita_lua.h"
#include "host_vita_startup.h"

extern unsigned g_isaac_vita_lua_fast_typed_runs;

#define RET_RVA          UINT32_C(0x0040eef0)   /* translated sites push RVAs */
#define CALLBACK_RETURN  UINT32_C(0xfff15333)
#define TARGET_BINDING   UINT32_C(0x003f8e30)
#define TARGET_GC        UINT32_C(0x003f95c0)
#define TARGET_CROSS     UINT32_C(0x00400400)
#define TARGET_ABORT     UINT32_C(0x00400300)
#define TARGET_BREAK     UINT32_C(0x00400500)

enum { STACK_WORDS = 4096, FLOOR_WORD = 64, CEIL_WORD = 4000, DUMP_MAX = 4096 };

_Alignas(16) static uint32_t s_stack_a[STACK_WORDS];
_Alignas(16) static uint32_t s_stack_b[STACK_WORDS];

typedef struct scope { jmp_buf env; } scope;

static lua_State *s_L;
static int s_use_typed;
static unsigned s_calls;
static char s_log[16384];
static size_t s_log_len;
static CPU s_cpu_b;
static int s_ref_table;
static int s_ref_userdata;
static void *s_userdata_pointer;
static unsigned s_failures;
static char s_binding_string[] = "binding-pushstring";
static char s_root_string[] = "root-pushstring";

static uint32_t pointer32(const void *pointer)
{
    if ((uintptr_t)pointer > UINT32_MAX) {
        fprintf(stderr, "pointer does not fit the 32-bit ABI\n");
        exit(2);
    }
    return (uint32_t)(uintptr_t)pointer;
}

static void note(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(s_log + s_log_len, sizeof s_log - s_log_len, fmt, ap);
    va_end(ap);
    if (n > 0)
        s_log_len += (size_t)n;
}

/* ------------------------------------------------ runtime stubs (host) --- */

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    scope *sc = (scope *)c->run_scope;
    c->fault = what;
    c->fault_addr = address;
    if (sc)
        longjmp(sc->env, 1);
    fprintf(stderr, "FAIL: unguarded fault at %08x: %s\n", (unsigned)address, what);
    exit(2);
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind; (void)size; (void)pc;
    guest_fault(c, address, "oracle guest stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "oracle guest stack owner violation");
    return 0;
}

void *isaac_vita_guest_malloc(size_t size) { return malloc(size); }
void *isaac_vita_guest_realloc(void *p, size_t size, int *valid_owner)
{
    if (valid_owner) *valid_owner = 1;
    return realloc(p, size);
}
int isaac_vita_guest_free(void *p) { free(p); return 1; }

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    (void)native_path; (void)capacity;
    guest_fault(c, guest_path, "oracle unexpectedly crossed the path mapper");
    return 0;
}

/* ------------------------------------------------------ bridge access --- */

static uint32_t idx(const char *name)
{
    uint32_t i;
    char full[96];
    snprintf(full, sizeof full, "Lua5.3.3r.dll!%s", name);
    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i) {
        const char *cand = isaac_vita_lua_import_name(i);
        if (cand && strcmp(cand, full) == 0)
            return i;
    }
    fprintf(stderr, "FAIL: %s is not a frozen import\n", name);
    exit(2);
}

/* The production binder's choice: typed table where bound, else generic. */
static int dispatch(CPU *c, uint32_t index)
{
    guest_import_family_fn fast =
        s_use_typed ? isaac_vita_lua_import_fast_binding(index) : NULL;
    if (fast)
        return fast(c, index, &s_calls);
    return isaac_vita_lua_import_indexed(c, index, &s_calls);
}

/* One translated cdecl import call: args right to left, state, return RVA. */
static uint32_t icall(CPU *c, const char *name, uint32_t nargs, ...)
{
    uint32_t args[8];
    uint32_t i, before;
    va_list ap;

    va_start(ap, nargs);
    for (i = 0U; i < nargs; ++i)
        args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    for (i = nargs; i != 0U; --i)
        gpush(c, args[i - 1U]);
    gpush(c, pointer32(s_L));
    gpush(c, RET_RVA);
    before = c->esp;
    if (!dispatch(c, idx(name)))
        guest_fault(c, before, "hostile: import rejected");
    if (c->esp != before + 4U)
        guest_fault(c, c->esp, "hostile: import did not pop exactly the return word");
    if (!guest_stack_adjust(c, 4U + 4U * nargs, 0U))
        guest_fault(c, c->esp, "hostile: caller cleanup failed");
    return c->eax;
}

/* ------------------------------------------------- translated bodies --- */

static void body_enter(CPU *c, const char *who)
{
    uint32_t ret = ld32(c->esp);
    uint32_t state = ld32(c->esp + 4U);
    if (ret != CALLBACK_RETURN || state != pointer32(s_L)) {
        char what[96];
        snprintf(what, sizeof what, "%s: bad callback frame", who);
        guest_fault(c, ret, what);
    }
}

static void body_leave(CPU *c, uint32_t nresults)
{
    c->eax = nresults;
    (void)gpop(c);              /* ret: leaves [L] for the caller (cdecl) */
}

/* LuaBridge-like binding: the game's shape, all thirteen typed names. */
static void body_binding(CPU *c)
{
    uint32_t top, ud, type, mt, n2;
    static const uint64_t integer = (uint64_t)INT64_C(-77000000000);
    body_enter(c, "binding");
    top = icall(c, "lua_gettop", 0U);
    ud = icall(c, "lua_touserdata", 1U, 1U);
    type = icall(c, "lua_type", 1U, 1U);
    mt = icall(c, "lua_getmetatable", 1U, 1U);
    if (mt)
        icall(c, "lua_settop", 1U, (uint32_t)-2);
    (void)icall(c, "lua_pushstring", 1U, pointer32(s_binding_string)); /* GC here */
    icall(c, "lua_pushinteger", 2U, (uint32_t)integer, (uint32_t)(integer >> 32));
    icall(c, "lua_rawgeti", 3U, (uint32_t)LUA_REGISTRYINDEX, (uint32_t)s_ref_table, 0U);
    icall(c, "lua_pushvalue", 1U, (uint32_t)-1);
    (void)icall(c, "lua_rawget", 1U, (uint32_t)-2);          /* T[T] -> nil */
    icall(c, "lua_pushnil", 0U);
    icall(c, "lua_pushnumber", 2U, 0U, UINT32_C(0x40590000));  /* 100.0 */
    (void)icall(c, "lua_rawgetp", 2U, (uint32_t)-4, pointer32(s_binding_string));
    n2 = icall(c, "lua_gettop", 0U);
    icall(c, "lua_settop", 1U, (uint32_t)-6);            /* keep string, integer */
    note("binding top=%u ud=%d type=%u mt=%u n2=%u;", top,
         ud == pointer32(s_userdata_pointer) ? 1 : (ud ? 2 : 0), type, mt, n2);
    body_leave(c, 2U);
}

/* __gc finalizer of a garbage userdata: nested imports at depth two. */
static void body_gc(CPU *c)
{
    uint32_t ud, type, top;
    body_enter(c, "gc");
    ud = icall(c, "lua_touserdata", 1U, 1U);
    type = icall(c, "lua_type", 1U, 1U);
    top = icall(c, "lua_gettop", 0U);
    icall(c, "lua_pushnil", 0U);
    icall(c, "lua_settop", 1U, (uint32_t)-2);
    note("gc ud=%s type=%u top=%u;", ud ? "ptr" : "null", type, top);
    body_leave(c, 0U);
}

/* __gc that lets a second CPU try to enter the bridge while c owns it. */
static void body_cross(CPU *c)
{
    scope sc;
    CPU *b = &s_cpu_b;
    uint32_t esp_b;
    unsigned calls_before = s_calls;
    body_enter(c, "cross");
    memset(b, 0, sizeof *b);
    b->stack_owner = b;
    b->stack_floor = pointer32(s_stack_b + FLOOR_WORD);
    b->stack_ceiling = pointer32(s_stack_b + CEIL_WORD);
    b->stack_low_water = b->stack_ceiling;
    b->esp = b->stack_ceiling - 64U;
    b->eax = UINT32_C(0xdeadbeef);
    b->run_scope = &sc;
    esp_b = b->esp;
    if (setjmp(sc.env) == 0) {
        (void)icall(b, "lua_gettop", 0U);
        note("cross: NO FAULT eax=%08x;", (unsigned)b->eax);
    } else {
        note("cross fault='%s' addr=%08x esp_delta=%d eax=%08x calls=%u;",
             b->fault, (unsigned)b->fault_addr, (int)(b->esp - esp_b),
             (unsigned)b->eax, s_calls - calls_before);
    }
    b->run_scope = NULL;
    body_leave(c, 0U);
}

/* __gc that clears the owner word underneath the root activation. */
static void body_abort(CPU *c)
{
    body_enter(c, "abort");
    isaac_vita_lua_abort_cpu(c);
    note("abort;");
    body_leave(c, 0U);
}

/* __gc that breaks cdecl: the trampoline raises luaL_error, which GCTM
 * rethrows as LUA_ERRGCMM through the nested typed pushstring frame. */
static void body_break(CPU *c)
{
    body_enter(c, "break");
    note("break;");
    c->eax = 0U;
    /* no pop: esp is saved-8, not saved-4 */
}

void guest_call(CPU *__restrict c, uint32_t address)
{
    switch (address) {
    case TARGET_BINDING: body_binding(c); return;
    case TARGET_GC:      body_gc(c); return;
    case TARGET_CROSS:   body_cross(c); return;
    case TARGET_ABORT:   body_abort(c); return;
    case TARGET_BREAK:   body_break(c); return;
    default:
        guest_fault(c, address, "oracle guest_call target is not registered");
    }
}

/* ------------------------------------------------------------ fixture --- */

static void fixture_create(lua_State *L)
{
    void *ud;
    lua_newtable(L);                        /* T */
    lua_pushstring(L, "one");
    lua_rawseti(L, -2, 1);
    ud = lua_newuserdata(L, 16U);           /* U */
    memset(ud, 0x5a, 16U);
    s_userdata_pointer = ud;
    lua_newtable(L);                        /* M */
    lua_pushstring(L, "U");
    lua_setfield(L, -2, "__name");
    lua_setmetatable(L, -2);
    s_ref_userdata = luaL_ref(L, LUA_REGISTRYINDEX);
    s_ref_table = luaL_ref(L, LUA_REGISTRYINDEX);
}

static void fixture_reset(lua_State *L)
{
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ref_userdata);   /* 1: U */
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_ref_table);      /* 2: T */
    lua_pushinteger(L, 7);                               /* 3 */
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
            written = snprintf(out + used, capacity - used, "%d:nil;", i); break;
        case LUA_TBOOLEAN:
            written = snprintf(out + used, capacity - used, "%d:b%d;", i, lua_toboolean(L, i)); break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, i))
                written = snprintf(out + used, capacity - used, "%d:i%lld;", i, (long long)lua_tointeger(L, i));
            else
                written = snprintf(out + used, capacity - used, "%d:n%.17g;", i, lua_tonumber(L, i));
            break;
        case LUA_TSTRING:
            written = snprintf(out + used, capacity - used, "%d:s%s@%p;", i, lua_tostring(L, i), lua_topointer(L, i)); break;
        default:
            written = snprintf(out + used, capacity - used, "%d:%s@%p;", i, lua_typename(L, type), lua_topointer(L, i)); break;
        }
        if (written < 0) break;
        used += (size_t)written;
    }
}

static void cpu_bind(CPU *c, uint32_t *stack)
{
    memset(c, 0, sizeof *c);
    c->eax = UINT32_C(0xdeadbeef);
    c->ecx = UINT32_C(0x11111111);
    c->edx = UINT32_C(0xcafef00d);
    c->ebx = UINT32_C(0x22222222);
    c->st_top = 3;
    c->stack_owner = c;
    c->stack_floor = pointer32(stack + FLOOR_WORD);
    c->stack_ceiling = pointer32(stack + CEIL_WORD);
    c->stack_low_water = c->stack_ceiling;
    c->esp = c->stack_ceiling - 64U;
    c->ebp = c->esp;
}

/* Root import on the second CPU through the generic handler: succeeds only
 * when nobody holds the owner word. */
static int owner_released(void)
{
    scope sc;
    CPU *b = &s_cpu_b;
    int ok;
    cpu_bind(b, s_stack_b);
    b->run_scope = &sc;
    if (setjmp(sc.env) == 0) {
        int saved = s_use_typed;
        s_use_typed = 0;
        (void)icall(b, "lua_gettop", 0U);
        s_use_typed = saved;
        ok = 1;
    } else {
        note("owner-probe fault='%s';", b->fault);
        ok = 0;
    }
    b->run_scope = NULL;
    return ok;
}

/* Garbage userdata whose __gc is a guest closure of `target`, GC armed so
 * the next luaC_checkGC (the next lua_pushstring) runs a complete cycle
 * and the finalizer inside it. */
static void arm_gc(CPU *c, uint32_t target)
{
    lua_State *L = s_L;
    lua_gc(L, LUA_GCSETPAUSE, 0);
    lua_gc(L, LUA_GCSETSTEPMUL, 1000000);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_newuserdata(L, 8U);
    lua_newtable(L);
    (void)icall(c, "lua_pushcclosure", 2U, target, 0U);   /* guest closure */
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);                                      /* now garbage */
}

static void disarm_gc(void)
{
    lua_gc(s_L, LUA_GCSETPAUSE, 200);
    lua_gc(s_L, LUA_GCSETSTEPMUL, 200);
}

/* --------------------------------------------------------- scenarios --- */

typedef struct outcome {
    uint32_t eax, edx, esp, low_water;
    const char *fault;
    uint32_t fault_addr;
    unsigned calls;
    unsigned typed_runs;
    int faulted;
    int top;
    int released;
    char dump[DUMP_MAX];
    char log[sizeof s_log];
} outcome;

typedef void (*scenario_fn)(CPU *c);

static void capture(CPU *c, outcome *o, unsigned typed_before)
{
    o->eax = c->eax; o->edx = c->edx; o->esp = c->esp;
    o->low_water = c->stack_low_water;
    o->fault = c->fault; o->fault_addr = c->fault_addr;
    o->calls = s_calls;
    o->typed_runs = g_isaac_vita_lua_fast_typed_runs - typed_before;
    o->top = lua_gettop(s_L);
    dump_stack(s_L, o->dump, sizeof o->dump);
    o->released = owner_released();
    memcpy(o->log, s_log, sizeof o->log);
}

static void run_scenario(const char *name, scenario_fn fn, int typed, outcome *o)
{
    CPU c;
    scope sc;
    unsigned typed_before = g_isaac_vita_lua_fast_typed_runs;

    s_use_typed = typed;
    s_calls = 0U;
    s_log_len = 0U;
    memset(s_log, 0, sizeof s_log);
    memset(o, 0, sizeof *o);
    fixture_reset(s_L);
    cpu_bind(&c, s_stack_a);
    c.run_scope = &sc;
    if (setjmp(sc.env) == 0) {
        fn(&c);
    } else {
        o->faulted = 1;
        note("ROOT FAULT '%s' at %08x;", c.fault, (unsigned)c.fault_addr);
        isaac_vita_lua_abort_cpu(&c);
    }
    c.run_scope = NULL;
    disarm_gc();
    capture(&c, o, typed_before);
    (void)name;
}

static int same_text(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static void compare(const char *name, const outcome *g, const outcome *t,
                    unsigned expect_typed_min)
{
    int equal = g->eax == t->eax && g->edx == t->edx && g->esp == t->esp &&
                g->low_water == t->low_water && same_text(g->fault, t->fault) &&
                g->fault_addr == t->fault_addr && g->calls == t->calls &&
                g->faulted == t->faulted && g->top == t->top &&
                strcmp(g->dump, t->dump) == 0 && strcmp(g->log, t->log) == 0 &&
                g->released == t->released;
    if (!equal || g->typed_runs != 0U || t->typed_runs < expect_typed_min ||
        !g->released) {
        ++s_failures;
        fprintf(stderr, "FAIL %s\n", name);
    }
    printf("%s %s: generic eax=%08x esp=%08x low=%08x fault=%s calls=%u top=%d released=%d\n"
           "  typed   eax=%08x esp=%08x low=%08x fault=%s calls=%u top=%d released=%d typed_runs=%u\n"
           "  log(g): %s\n  log(t): %s\n  dump(g): %s\n  dump(t): %s\n",
           equal ? "OK  " : "DIFF", name,
           (unsigned)g->eax, (unsigned)g->esp, (unsigned)g->low_water,
           g->fault ? g->fault : "-", g->calls, g->top, g->released,
           (unsigned)t->eax, (unsigned)t->esp, (unsigned)t->low_water,
           t->fault ? t->fault : "-", t->calls, t->top, t->released,
           t->typed_runs, g->log, t->log, g->dump, t->dump);
}

/* S1: generic lua_pcallk root -> Lua calls the guest binding -> nested typed
 *     imports; the binding's pushstring runs a GC cycle whose __gc finalizer
 *     nests typed imports at depth two. */
static void s1(CPU *c)
{
    uint32_t status;
    (void)icall(c, "lua_pushcclosure", 2U, TARGET_BINDING, 0U);
    arm_gc(c, TARGET_GC);           /* no allocation until the binding's pushstring */
    lua_pushvalue(s_L, 1);                              /* arg: U */
    status = icall(c, "lua_pcallk", 5U, 1U, (uint32_t)LUA_MULTRET, 0U, 0U, 0U);
    note("pcall status=%u;", status);
}

/* S2: typed lua_pushstring root -> GC cycle -> __gc nests typed imports. */
static void s2(CPU *c)
{
    uint32_t r;
    arm_gc(c, TARGET_GC);
    r = icall(c, "lua_pushstring", 1U, pointer32(s_root_string));
    note("pushstring eax=%s;", r == pointer32(lua_tostring(s_L, -1)) ? "top" : "??");
    r = icall(c, "lua_type", 1U, (uint32_t)-1);
    note("type=%u;", r);
}

/* S3: typed root -> __gc lets a second CPU try to enter. */
static void s3(CPU *c)
{
    arm_gc(c, TARGET_CROSS);
    (void)icall(c, "lua_pushstring", 1U, pointer32(s_root_string));
    note("after-cross top=%u;", icall(c, "lua_gettop", 0U));
}

/* S4: typed root -> __gc clears the owner word -> root leave must fault
 *     "Lua bridge owner changed during call" without popping. */
static void s4(CPU *c)
{
    arm_gc(c, TARGET_ABORT);
    (void)icall(c, "lua_pushstring", 1U, pointer32(s_root_string));
    note("UNEXPECTED: no fault;");
}

/* S5: pcallk root -> binding -> typed pushstring -> GC -> __gc breaks cdecl
 *     -> luaL_error -> LUA_ERRGCMM thrown through the nested typed frame,
 *     recovered by the pcallk root. */
static void s5(CPU *c)
{
    uint32_t status;
    (void)icall(c, "lua_pushcclosure", 2U, TARGET_BINDING, 0U);
    arm_gc(c, TARGET_BREAK);
    lua_pushvalue(s_L, 1);
    status = icall(c, "lua_pcallk", 5U, 1U, (uint32_t)LUA_MULTRET, 0U, 0U, 0U);
    note("pcall status=%u msg='%s';", status,
         lua_type(s_L, -1) == LUA_TSTRING ? lua_tostring(s_L, -1) : "(non-string)");
}

/* S6: return word below the floor (esp = floor - 4): the range check
 *     rejects, the generic handler reads the arguments then faults on the
 *     pop; both must fault identically. */
static void s6(CPU *c)
{
    c->esp = c->stack_floor - 4U + 8U;    /* icall pushes 2 words for gettop */
    (void)icall(c, "lua_gettop", 0U);
    note("UNEXPECTED: no fault;");
}

/* S7: corrupt low-water (above the ceiling): typed must fall back; the
 *     generic fast_bound check ignores the diagnostic and proceeds. */
static void s7(CPU *c)
{
    uint32_t r;
    c->stack_low_water = c->stack_ceiling + 4U;
    r = icall(c, "lua_touserdata", 1U, 1U);
    note("touserdata=%d;", r == pointer32(s_userdata_pointer));
}

/* S8: misaligned ESP (by two): the loads read the same bytes. */
static void s8(CPU *c)
{
    uint32_t r;
    c->esp -= 2U;
    r = icall(c, "lua_touserdata", 1U, 1U);
    note("touserdata=%d;", r == pointer32(s_userdata_pointer));
    icall(c, "lua_pushinteger", 2U, 5U, 0U);
    icall(c, "lua_pushnumber", 2U, 0U, UINT32_C(0x40590000));
    icall(c, "lua_rawgeti", 3U, 2U, 1U, 0U);
}

/* S9: return word exactly at the floor: inclusive lower edge, typed runs. */
static void s9(CPU *c)
{
    uint32_t r;
    c->esp = c->stack_floor + 12U;        /* touserdata frame = 12 bytes */
    r = icall(c, "lua_touserdata", 1U, 1U);
    note("touserdata=%d;", r == pointer32(s_userdata_pointer));
}

static int panic(lua_State *L)
{
    fprintf(stderr, "FAIL: Lua panic: %s\n", lua_tostring(L, -1));
    exit(3);
}

int main(void)
{
    static const struct { const char *name; scenario_fn fn; unsigned typed_min; } cases[] = {
        { "S1 nested typed inside generic pcallk root (+__gc depth 2)", s1, 12U },
        { "S2 typed pushstring root -> __gc nested typed", s2, 6U },
        { "S3 typed root -> __gc foreign CPU entry", s3, 2U },
        { "S4 typed root -> __gc clears owner word", s4, 1U },
        { "S5 pcallk root -> nested typed frame abandoned by LUA_ERRGCMM", s5, 5U },
        { "S6 return word below the stack floor", s6, 0U },
        { "S7 corrupt low-water diagnostic", s7, 0U },
        { "S8 misaligned ESP", s8, 4U },
        { "S9 return word exactly at the floor", s9, 1U },
    };
    size_t i;

    s_L = luaL_newstate();
    if (!s_L) return 2;
    lua_atpanic(s_L, panic);
    luaL_openlibs(s_L);
    fixture_create(s_L);
    if (isaac_vita_lua_import_fast_prepare(1) != 13U) {
        fprintf(stderr, "FAIL: prepare(1) != 13\n");
        return 1;
    }
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        outcome g, t;
        run_scenario(cases[i].name, cases[i].fn, 0, &g);
        run_scenario(cases[i].name, cases[i].fn, 1, &t);
        compare(cases[i].name, &g, &t, cases[i].typed_min);
    }
    lua_close(s_L);
    if (s_failures) {
        fprintf(stderr, "hostile oracle: %u failures\n", s_failures);
        return 1;
    }
    printf("Vita Lua fast dispatch HOSTILE oracle: PASS (%u scenarios x generic/typed)\n",
           (unsigned)(sizeof cases / sizeof cases[0]));
    return 0;
}
