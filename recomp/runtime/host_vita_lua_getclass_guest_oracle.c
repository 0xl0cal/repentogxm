/* Guest-side oracle for the native Userdata::getClass / getExact seam.
 *
 * 32-bit host executable (the frozen Lua ABI is ILP32: raw lua_State and
 * userdata pointers cross the bridge).  It links
 *   - the GENERATED bodies of sub_003f8e30 / sub_003f8c80, extracted verbatim
 *     from the corpus by recomp/test_vita_lua_getclass.py (their IAT slot
 *     reads, string pushes and identity-key push use the PE's absolute guest
 *     addresses, so the oracle maps those pages at the very same addresses),
 *   - the production bridge host_vita_lua.c (ISAAC_VITA_LUA_NATIVE_GETCLASS=1),
 *   - the seam host_vita_lua_getclass.c with its __wrap_ entries,
 *   - pristine Lua 5.3.3,
 * and drives the translated body and the wrap on one bound CPU and one Lua
 * state, comparing eax / esp / callee-saved registers / the guest stack words
 * above esp / the Lua stack (depth, types, identities) / error texts.
 * __real_sub_* is this file's: it either runs the generated body (fallback
 * equivalence) or, for shapes where the translated body itself has undefined
 * behaviour (lua_rawget on a non-table), records that the seam handed over an
 * untouched CPU, an unchanged Lua stack top and unchanged guest stack bytes.
 * A __gc stress proves that a guest finalizer reached from a GC step inside
 * the native path finds the bridge owner.  The VERIFY build checks the seam's
 * own translated-vs-native comparison.
 * Hostile cases (review): LUA_ERRGCMM thrown by a guest __gc out of the GC
 * step inside the seam (longjmp across the native frames, bridge recovery),
 * a guest upvalue pseudo-index argument (vita_lua_index vs lgc_index), the
 * canBeConst byte with garbage upper bytes, the relative-index quirk
 * (index -2 checks one slot and returns another), an index beyond the top,
 * and the seam as the outermost owner while its own GC step runs a guest
 * __gc that re-enters the wrap (getExact, as the game's __gc chain does). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "lstate.h"      /* G(L)/luaE_setdebt: the GC-inside-the-seam stress */

#include "host_vita_lua.h"
#include "host_vita_lua_getclass.h"
#include "host_vita_startup.h"

/* the generated bodies (lgc_oracle_generated.c) */
void sub_003f8e30(CPU *__restrict c);
void sub_003f8c80(CPU *__restrict c);

#define IMPORT_TOKEN_BASE 0xfff16000U   /* fake IAT slot word = token + Lua local index */
#define IMPORT_RETURN     0xfff15000U
#define LUA_IAT_FIRST_RVA 0x00606150U   /* Lua5.3.3r.dll!lua_close; 67 contiguous slots */
#define LUA_IAT_PAGE_RVA  0x00606000U
#define RDATA_PAGE_RVA    0x0074f000U
#define RET_GETCLASS      0x0040cc27U   /* guest_0122.c: call 0x3f8e30; add esp, 8 */
#define RET_GETEXACT      0x0045d480U   /* guest_0190.c: call 0x3f8c80; add esp, 4 */
#define GUEST_PROBE       0x00421000U
#define GUEST_GC          0x00421010U
#define GUEST_GC_RAISE    0x00421020U   /* __gc that raises through luaL_error */
#define GUEST_GC_EXACT    0x00421030U   /* __gc that calls getExact (nested wrap) */
/* class-table registry keys: light userdata the game takes from its .data */
#define KEY_A     0x9880685cU
#define KEY_B     0x98806860U
#define KEY_C     0x98806864U
#define KEY_X     0x98806868U
#define KEY_P0    0x9880686cU
#define KEY_UNREG 0x98806870U
#define KEY_G     0x98806874U
#define LUA_IMPORT_CLOSE        0U
#define LUA_IMPORT_OPENLIBS     10U
#define LUA_IMPORT_SETGLOBAL    37U
#define LUA_IMPORT_NEWSTATE_AUX 39U
#define LUA_IMPORT_LUAL_ERROR   41U
#define LUA_IMPORT_PCALLK       59U
#define LUA_IMPORT_PUSHCLOSURE  61U
#define STACK_WORDS  8192
#define ESP_HEADROOM 256U

#define VA(rva) ((uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))
#define IDENTITY ((const void *)(uintptr_t)VA(ISAAC_LGC_IDENTITY_KEY_RVA))

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "native getclass guest oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* --- runtime stubs ---------------------------------------------------------- */

static unsigned char s_coverage[16384];
unsigned char *g_guest_coverage_functions = s_coverage;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;

static uint32_t s_fault_calls;
static uint32_t s_log_lines;

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
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

void *isaac_vita_guest_malloc(size_t size) { return malloc(size); }
void *isaac_vita_guest_realloc(void *pointer, size_t size, int *valid_owner)
{
    if (valid_owner)
        *valid_owner = 1;
    return realloc(pointer, size);
}
int isaac_vita_guest_free(void *pointer) { free(pointer); return 1; }

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    (void)native_path;
    (void)capacity;
    guest_fault(c, guest_path, "oracle has no path mapper");
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list args;
    char line[768];
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (++s_log_lines <= 48U)
        printf("  log: %s\n", line);
}

/* --- guest dispatch ---------------------------------------------------------- */

static void guest_probe(CPU *__restrict c);
static void guest_gc(CPU *__restrict c);
static void guest_gc_raise(CPU *__restrict c);
static void guest_gc_exact(CPU *__restrict c);

void guest_call(CPU *__restrict c, uint32_t address)
{
    if (address - (uint32_t)IMPORT_TOKEN_BASE < ISAAC_VITA_LUA_IMPORT_COUNT) {
        if (!isaac_vita_lua_import_indexed(
                c, address - (uint32_t)IMPORT_TOKEN_BASE, NULL))
            guest_fault(c, address, "oracle Lua import index rejected");
        return;
    }
    if (address == GUEST_PROBE) { guest_probe(c); return; }
    if (address == GUEST_GC) { guest_gc(c); return; }
    if (address == GUEST_GC_RAISE) { guest_gc_raise(c); return; }
    if (address == GUEST_GC_EXACT) { guest_gc_exact(c); return; }
    guest_fault(c, address, "oracle guest_call target is not registered");
}

/* --- fixture ------------------------------------------------------------------ */

_Alignas(16) static uint32_t s_stack[STACK_WORDS];
static CPU s_cpu;
static lua_State *s_L;
static uint32_t s_state_word;

/* Reservations start on the 64 KiB allocation granularity; commit the page
 * the bodies address inside it. */
static int map_fixed(uint32_t va, uint32_t size)
{
    uint32_t base = va & ~0xffffU;
    void *reserved = VirtualAlloc((void *)(uintptr_t)base, (va - base) + size,
                                  MEM_RESERVE, PAGE_READWRITE);
    void *committed;
    if (reserved != (void *)(uintptr_t)base) {
        fprintf(stderr, "map_fixed(%08x): reserve at %08x got %p error=%lu\n",
                (unsigned)va, (unsigned)base, reserved, GetLastError());
        return 0;
    }
    committed = VirtualAlloc((void *)(uintptr_t)va, size, MEM_COMMIT, PAGE_READWRITE);
    if (committed != (void *)(uintptr_t)va) {
        fprintf(stderr, "map_fixed(%08x): commit got %p error=%lu\n", (unsigned)va,
                committed, GetLastError());
        return 0;
    }
    return 1;
}

static void bind_stack(CPU *c)
{
    c->stack_owner = c;
    c->stack_floor = (uint32_t)(uintptr_t)s_stack;
    c->stack_ceiling = c->stack_floor + sizeof s_stack;
    c->stack_low_water = c->stack_ceiling;
    c->esp = c->stack_ceiling - ESP_HEADROOM;
}

static void reset_cpu(CPU *c)
{
    memset(c, 0, sizeof *c);
    bind_stack(c);
    memset(s_stack, 0xcd, sizeof s_stack);
    c->eax = 0xa5a5a5a5U;
    c->ebx = 0x0b0b0b0bU;
    c->ebp = 0x0e0e0e0eU;
    c->esi = 0x51515151U;
    c->edi = 0xd1d1d1d1U;
}

static int invoke_import(CPU *c, uint32_t index, const uint32_t *arguments,
                         uint32_t count)
{
    uint32_t saved = c->esp;
    uint32_t i;
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, IMPORT_RETURN + index);
    if (!isaac_vita_lua_import_indexed(c, index, NULL))
        return 0;
    if (c->fault)
        return 0;
    if (c->esp != saved - count * 4U)
        return 0;
    return guest_stack_adjust(c, count * 4U, index);
}

static int register_closure(CPU *c, uint32_t target, const char *name)
{
    uint32_t closure_arguments[] = { s_state_word, target, 0U };
    uint32_t global_arguments[] = { s_state_word, (uint32_t)(uintptr_t)name };
    if (!invoke_import(c, LUA_IMPORT_PUSHCLOSURE, closure_arguments, 3U))
        return 0;
    return invoke_import(c, LUA_IMPORT_SETGLOBAL, global_arguments, 2U);
}

static int oracle_panic(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    fprintf(stderr, "oracle Lua panic: %s\n", message ? message : "(none)");
    return 0;
}

/* Runs `code` (0 args) through the bridge's lua_pcallk import so guest
 * closures called from it find the active CPU, exactly like LuaEngine does. */
static int run_chunk(CPU *c, const char *code, int nresults)
{
    uint32_t arguments[] = { s_state_word, 0U, (uint32_t)nresults, 0U, 0U, 0U };
    int root = 0, status;
    /* the load allocates: a collector step here may reach a guest __gc
     * closure, which needs the active CPU (the game loads chunks through a
     * Lua import, i.e. under the bridge owner) */
    if (!isaac_vita_lua_seam_enter(c, &root))
        return 0;
    status = luaL_loadstring(s_L, code);
    isaac_vita_lua_seam_leave(c, root);
    if (status != LUA_OK) {
        fprintf(stderr, "chunk load: %s\n", lua_tostring(s_L, -1));
        return 0;
    }
    if (!invoke_import(c, LUA_IMPORT_PCALLK, arguments, 6U))
        return 0;
    if (c->eax != LUA_OK) {
        fprintf(stderr, "chunk pcall status %u: %s\n", (unsigned)c->eax,
                lua_tostring(s_L, -1));
        return 0;
    }
    return 1;
}

/* Finish every pending guest finalizer under a bridge owner (the game's GC
 * steps run under an import scope).  A raising finalizer ends one full
 * collection with LUA_ERRGCMM, so repeat until three consecutive
 * collections complete cleanly; native calls made outside any owner
 * afterwards cannot meet a guest __gc closure. */
static int native_collect(lua_State *L)
{
    lua_gc(L, LUA_GCCOLLECT, 0);
    return 0;
}

static int drain_finalizers(CPU *c)
{
    int root = 0, clean = 0, i;
    if (!isaac_vita_lua_seam_enter(c, &root))
        return 0;
    for (i = 0; i < 400 && clean < 3; ++i) {
        lua_pushcfunction(s_L, native_collect);
        if (lua_pcall(s_L, 0, 0, 0) == LUA_OK) {
            ++clean;
        } else {
            clean = 0;
            lua_pop(s_L, 1);
        }
    }
    isaac_vita_lua_seam_leave(c, root);
    return clean >= 3;
}

/* Class tables as the frozen Userdata binder builds them: identity boolean,
 * __type, optional __parent, optional __const variant (identity, "const X",
 * __parent = parent's const variant), registered under a light-userdata key. */
static int make_class(lua_State *L, const char *name, int parent_ref,
                      int const_parent_ref, uint32_t key, int want_const,
                      int *const_ref)
{
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_rawsetp(L, -2, IDENTITY);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__type");
    if (parent_ref) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, parent_ref);
        lua_setfield(L, -2, "__parent");
    }
    if (want_const) {
        lua_newtable(L);
        lua_pushboolean(L, 1);
        lua_rawsetp(L, -2, IDENTITY);
        lua_pushfstring(L, "const %s", name);
        lua_setfield(L, -2, "__type");
        if (const_parent_ref) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, const_parent_ref);
            lua_setfield(L, -2, "__parent");
        }
        lua_pushvalue(L, -1);
        *const_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_setfield(L, -2, "__const");
    }
    if (key) {
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)key);
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void make_object(lua_State *L, int mt_ref, const char *name)
{
    lua_getglobal(L, "objs");
    lua_newuserdata(L, 16U);
    if (mt_ref) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, mt_ref);
        lua_setmetatable(L, -2);
    }
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

static int native_newud(lua_State *L)
{
    lua_newuserdata(L, 200U);
    lua_pushvalue(L, 1);
    lua_setmetatable(L, -2);
    return 1;
}

/* Positive GC debt: the next luaC_checkGC anywhere performs a collector step
 * (pristine Lua internals, oracle only). */
static int native_gc_debt(lua_State *L)
{
    luaE_setdebt(G(L), 1);
    return 0;
}

static void push_object(lua_State *L, const char *name)
{
    if (strcmp(name, "table") == 0) {
        lua_newtable(L);
    } else if (strcmp(name, "string") == 0) {
        lua_pushstring(L, "not a userdata");
    } else if (strcmp(name, "light") == 0) {
        lua_pushlightuserdata(L, (void *)(uintptr_t)0x12340U);
    } else {
        lua_getglobal(L, "objs");
        lua_getfield(L, -1, name);
        lua_remove(L, -2);
    }
}

/* --- __real: the oracle's own ------------------------------------------------- */

enum { REAL_GENERATED = 0, REAL_STUB = 1 };
static int s_real_mode;
static uint32_t s_real_calls;
static uint32_t s_real_contract_failures;
static CPU s_entry_cpu;
static int s_entry_top;
static uint8_t s_entry_stack[sizeof s_stack];
static uint32_t s_entry_live;   /* bytes of the live guest stack [esp, ceiling) */
static int s_in_seam;
static int s_ref_G;

static void snapshot_entry(CPU *c)
{
    memcpy(&s_entry_cpu, c, sizeof *c);
    s_entry_top = lua_gettop((lua_State *)(uintptr_t)c->ecx);
    s_entry_live = c->stack_ceiling - c->esp;
    if (s_entry_live > sizeof s_entry_stack)
        s_entry_live = sizeof s_entry_stack;
    memcpy(s_entry_stack, (const void *)(uintptr_t)c->esp, s_entry_live);
}

/* A nested wrap activation (guest __gc -> getExact inside the outer seam)
 * takes its own entry snapshot; the outer one is parked here meanwhile. */
typedef struct entry_snapshot {
    CPU cpu;
    int top;
    uint32_t live;
    uint8_t stack[sizeof s_stack];
} entry_snapshot;
static entry_snapshot s_parked_entry;

static void snapshot_park(void)
{
    memcpy(&s_parked_entry.cpu, &s_entry_cpu, sizeof s_entry_cpu);
    s_parked_entry.top = s_entry_top;
    s_parked_entry.live = s_entry_live;
    memcpy(s_parked_entry.stack, s_entry_stack, s_entry_live);
}

static void snapshot_unpark(void)
{
    memcpy(&s_entry_cpu, &s_parked_entry.cpu, sizeof s_entry_cpu);
    s_entry_top = s_parked_entry.top;
    s_entry_live = s_parked_entry.live;
    memcpy(s_entry_stack, s_parked_entry.stack, s_entry_live);
}

/* The seam promises to hand the translated body an untouched CPU, the entry
 * Lua stack top and unchanged live guest stack words (below esp is dead: a
 * guest __gc closure reached from a GC step inside the seam builds its frames
 * there). */
static void check_real_contract(CPU *c)
{
    lua_State *L = (lua_State *)(uintptr_t)c->ecx;
    int live_same = c->esp == s_entry_cpu.esp &&
                    memcmp(s_entry_stack, (const void *)(uintptr_t)c->esp,
                           s_entry_live) == 0;
    if (c->eax != s_entry_cpu.eax || c->ecx != s_entry_cpu.ecx ||
        c->edx != s_entry_cpu.edx || c->ebx != s_entry_cpu.ebx ||
        c->esp != s_entry_cpu.esp || c->ebp != s_entry_cpu.ebp ||
        c->esi != s_entry_cpu.esi || c->edi != s_entry_cpu.edi ||
        c->fault != NULL || lua_gettop(L) != s_entry_top || !live_same) {
        ++s_real_contract_failures;
        fprintf(stderr, "__real entry contract broken: eax=%08x/%08x esp=%08x/%08x "
                "top=%d/%d live stack=%s\n", (unsigned)c->eax,
                (unsigned)s_entry_cpu.eax, (unsigned)c->esp,
                (unsigned)s_entry_cpu.esp, lua_gettop(L), s_entry_top,
                live_same ? "same" : "CHANGED");
    }
}

/* Stub mode: a raw `ret` (the unbound-stack case must not trip the checked
 * pop) with a sentinel result, nothing else. */
void __real_sub_003f8e30(CPU *__restrict c)
{
    ++s_real_calls;
    check_real_contract(c);
    if (s_real_mode == REAL_STUB) {
        c->eax = 0xdeadbeefU;
        c->esp += 4U;
        return;
    }
    sub_003f8e30(c);
}

void __real_sub_003f8c80(CPU *__restrict c)
{
    ++s_real_calls;
    check_real_contract(c);
    if (s_real_mode == REAL_STUB) {
        c->eax = 0xdeadbeefU;
        c->esp += 4U;
        return;
    }
    sub_003f8c80(c);
}

/* --- the four entry variants --------------------------------------------------- */

enum { FN_GETCLASS = 0, FN_GETEXACT = 1 };
enum { VARIANT_TRANSLATED = 0, VARIANT_WRAP = 1 };

static void call_variant(CPU *c, int fn, int variant)
{
    snapshot_entry(c);
    s_in_seam = 1;
    if (fn == FN_GETCLASS) {
        if (variant == VARIANT_WRAP) __wrap_sub_003f8e30(c); else sub_003f8e30(c);
    } else {
        if (variant == VARIANT_WRAP) __wrap_sub_003f8c80(c); else sub_003f8c80(c);
    }
    s_in_seam = 0;
}

/* Arguments as the translated callers push them: getClass(L=ecx, index=edx,
 * [classKey, canBeConst]) then the return word; getExact(L=ecx, [classKey]). */
static uint32_t push_call(CPU *c, int fn, uint32_t key, uint32_t can_be_const,
                          int32_t index)
{
    c->ecx = s_state_word;
    c->edx = (uint32_t)index;
    if (fn == FN_GETCLASS) {
        gpush(c, can_be_const);
        gpush(c, key);
        gpush(c, RET_GETCLASS);
        return 8U;
    }
    gpush(c, key);
    gpush(c, RET_GETEXACT);
    return 4U;
}

/* --- guest closures ------------------------------------------------------------- */

static int s_probe_fn, s_probe_variant;
static uint32_t s_probe_key, s_probe_cbc;
static int32_t s_probe_index;
static uint32_t s_gc_calls, s_gc_in_seam;

static void guest_probe(CPU *__restrict c)
{
    uint32_t L = ld32(c->esp + 4U);
    uint32_t adjust;
    c->ecx = L;
    adjust = push_call(c, s_probe_fn, s_probe_key, s_probe_cbc, s_probe_index);
    call_variant(c, s_probe_fn, s_probe_variant);
    if (c->fault)
        return;
    (void)guest_stack_adjust(c, adjust, 0U);
    lua_pushlightuserdata((lua_State *)(uintptr_t)L, (void *)(uintptr_t)c->eax);
    c->eax = 1U;
    (void)gpop(c);
}

static void guest_gc(CPU *__restrict c)
{
    ++s_gc_calls;
    if (s_in_seam)
        ++s_gc_in_seam;
    c->eax = 0U;
    (void)gpop(c);
}

/* Hostile: a guest __gc closure that raises through the bridge's luaL_error
 * import (escape to the nested trampoline, native luaL_error).  GCTM catches
 * it and re-throws LUA_ERRGCMM out of the lua_pushstring that stepped the
 * collector: through the seam's native frames (wrap) or the import
 * handler's (translated). */
static uint32_t s_gc_raise_calls, s_gc_raise_in_seam;
static const char s_gc_raise_format[] = "%s";
static const char s_gc_raise_text[] = "boom";

static void guest_gc_raise(CPU *__restrict c)
{
    uint32_t L = ld32(c->esp + 4U);
    ++s_gc_raise_calls;
    if (s_in_seam)
        ++s_gc_raise_in_seam;
    /* luaL_error(L, "%s", "boom") exactly as a translated body pushes it */
    gpush(c, (uint32_t)(uintptr_t)s_gc_raise_text);
    gpush(c, (uint32_t)(uintptr_t)s_gc_raise_format);
    gpush(c, L);
    gpush(c, IMPORT_RETURN + LUA_IMPORT_LUAL_ERROR);
    (void)isaac_vita_lua_import_indexed(c, LUA_IMPORT_LUAL_ERROR, NULL);
    guest_fault(c, 0U, "luaL_error import returned to the __gc closure");
}

/* Hostile: the game's __gc handler chain calls getExact(L, classKey) on the
 * object being finalized -- a nested wrap activation inside the GC step that
 * the outer getClass seam's own lua_pushstring started (same CPU, same Lua
 * state, owner slot already held by the outer activation). */
static uint32_t s_gc_exact_calls, s_gc_exact_native, s_gc_exact_bad;

static void guest_gc_exact(CPU *__restrict c)
{
    lua_State *L = (lua_State *)(uintptr_t)ld32(c->esp + 4U);
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[FN_GETEXACT];
    void *expect = lua_touserdata(L, 1);
    ++s_gc_exact_calls;
    snapshot_park();
    c->ecx = (uint32_t)(uintptr_t)L;
    gpush(c, KEY_G);
    gpush(c, RET_GETEXACT);
    snapshot_entry(c);
    __wrap_sub_003f8c80(c);
    snapshot_unpark();
    if (c->fault)
        return;
    (void)guest_stack_adjust(c, 4U, 0U);
    if (c->eax != (uint32_t)(uintptr_t)expect)
        ++s_gc_exact_bad;
    if (st->handled[FN_GETEXACT] == handled_before + 1U)
        ++s_gc_exact_native;
    c->eax = 0U;
    (void)gpop(c);
}

/* --- direct comparison ------------------------------------------------------------ */

typedef struct outcome {
    uint32_t eax, esp, ecx, edx, ebx, ebp, esi, edi;
    const char *fault;
    int top;
    int types[8];
    const void *pointers[8];
    uint8_t above[ESP_HEADROOM + 16U];
} outcome;

static void run_direct(int fn, int variant, const char *object,
                       const char *object2, uint32_t key,
                       uint32_t can_be_const, int32_t index, outcome *out)
{
    CPU *c = &s_cpu;
    int i;
    lua_settop(s_L, 0);
    push_object(s_L, object);
    if (object2)
        push_object(s_L, object2);
    reset_cpu(c);
    (void)push_call(c, fn, key, can_be_const, index);
    call_variant(c, fn, variant);
    memset(out, 0, sizeof *out);
    out->eax = c->eax; out->esp = c->esp; out->ecx = c->ecx; out->edx = c->edx;
    out->ebx = c->ebx; out->ebp = c->ebp; out->esi = c->esi; out->edi = c->edi;
    out->fault = c->fault;
    out->top = lua_gettop(s_L);
    for (i = 1; i <= out->top && i <= 8; ++i) {
        out->types[i - 1] = lua_type(s_L, i);
        out->pointers[i - 1] = lua_topointer(s_L, i);
    }
    if (c->stack_ceiling - c->esp <= sizeof out->above)
        memcpy(out->above, (const void *)(uintptr_t)c->esp,
               c->stack_ceiling - c->esp);
}

static int same_outcome(const outcome *a, const outcome *b)
{
    return a->eax == b->eax && a->esp == b->esp && a->ecx == b->ecx &&
           a->edx == b->edx && a->ebx == b->ebx && a->ebp == b->ebp &&
           a->esi == b->esi && a->edi == b->edi && a->fault == b->fault &&
           a->top == b->top &&
           memcmp(a->types, b->types, sizeof a->types) == 0 &&
           memcmp(a->pointers, b->pointers, sizeof a->pointers) == 0 &&
           memcmp(a->above, b->above, sizeof a->above) == 0;
}

typedef struct direct_case {
    const char *name;
    int fn;
    const char *object;
    uint32_t key;
    uint32_t can_be_const;
    int32_t index;
    int native;             /* 1 = the wrap must serve it natively */
    uint32_t reason;        /* fallback reason when native == 0 */
    uint32_t hops;
    uint32_t imports;
} direct_case;

static const direct_case s_direct_cases[] = {
    { "getClass exact A",              FN_GETCLASS, "A",   KEY_A,  0U, 1, 1, 0U, 0U, 14U },
    { "getClass derived B:A (1 hop)",  FN_GETCLASS, "B",   KEY_A,  0U, 1, 1, 0U, 1U, 21U },
    { "getClass derived C:B:A (2)",    FN_GETCLASS, "C",   KEY_A,  0U, 1, 1, 0U, 2U, 28U },
    { "getClass const A canBeConst=1", FN_GETCLASS, "Ac",  KEY_A,  1U, 1, 1, 0U, 0U, 19U },
    { "getClass const B:A cbc=1",      FN_GETCLASS, "Bc",  KEY_A,  1U, 1, 1, 0U, 1U, 26U },
    { "getClass mutable A cbc=1",      FN_GETCLASS, "A",   KEY_A,  1U, 1, 1, 0U, 0U, 14U },
    /* hostile: canBeConst is `cmp byte ptr [ebp+0xc], 0`; upper bytes are garbage */
    { "getClass const A cbc=0xffffff01", FN_GETCLASS, "Ac",  KEY_A,  0xffffff01U, 1, 1, 0U, 0U, 19U },
    { "getClass exact B",              FN_GETCLASS, "B",   KEY_B,  0U, 1, 1, 0U, 0U, 14U },
    { "getClass 20-deep chain (hops)", FN_GETCLASS, "P20", KEY_P0, 0U, 1, 0, ISAAC_LGC_REASON_HOPS, 0U, 0U },
    { "getExact exact A",              FN_GETEXACT, "A",   KEY_A,  0U, 1, 1, 0U, 0U, 10U },
    { "getExact const A",              FN_GETEXACT, "Ac",  KEY_A,  0U, 1, 1, 0U, 0U, 14U },
    { "getExact exact B",              FN_GETEXACT, "B",   KEY_B,  0U, 1, 1, 0U, 0U, 10U },
};

typedef struct pcall_case {
    const char *name;
    int fn;
    const char *object;     /* Lua expression */
    uint32_t key;
    uint32_t can_be_const;
    int32_t index;
    uint32_t reason;
    const char *expect;     /* substring of the error message */
} pcall_case;

static const pcall_case s_pcall_cases[] = {
    { "getClass table",             FN_GETCLASS, "{}",             KEY_A, 0U, 1,  ISAAC_LGC_REASON_USERDATA, "(A expected, got table)" },
    { "getClass string",            FN_GETCLASS, "'s'",            KEY_A, 0U, 1,  ISAAC_LGC_REASON_USERDATA, "(A expected, got string)" },
    { "getClass wrong class A as B", FN_GETCLASS, "objs.A",        KEY_B, 0U, 1,  ISAAC_LGC_REASON_PARENT,   "(B expected, got A)" },
    { "getClass const violation",   FN_GETCLASS, "objs.Ac",        KEY_A, 0U, 1,  ISAAC_LGC_REASON_CONST,    "(cannot be const)" },
    { "getClass const wrong class", FN_GETCLASS, "objs.Ac",        KEY_B, 1U, 1,  ISAAC_LGC_REASON_PARENT,   "expected, got const A)" },
    { "getClass foreign metatable", FN_GETCLASS, "objs.foreign",   KEY_A, 0U, 1,  ISAAC_LGC_REASON_IDENTITY, "(A expected, got userdata)" },
    { "getClass identity not bool", FN_GETCLASS, "objs.wrongid",   KEY_A, 0U, 1,  ISAAC_LGC_REASON_IDENTITY, "(A expected, got userdata)" },
    { "getClass negative index",    FN_GETCLASS, "objs.A",         KEY_A, 0U, -1, ISAAC_LGC_REASON_USERDATA, "expected, got" },
    /* hostile: the byte read of canBeConst (0x100 -> byte 0 -> const violation) */
    { "getClass const cbc=0x100",   FN_GETCLASS, "objs.Ac",        KEY_A, 0x100U, 1, ISAAC_LGC_REASON_CONST, "(cannot be const)" },
    { "getClass const B:A cbc=0x4300", FN_GETCLASS, "objs.Bc",       KEY_A, 0x4300U, 1, ISAAC_LGC_REASON_CONST, "(cannot be const)" },
    /* hostile: an index beyond the top is LUA_TNONE ("no value") */
    { "getClass index beyond top",  FN_GETCLASS, "objs.A",         KEY_A, 0U, 5,  ISAAC_LGC_REASON_USERDATA, "(A expected, got no value)" },
    { "getExact derived B as A",    FN_GETEXACT, "objs.B",         KEY_A, 0U, 1,  ISAAC_LGC_REASON_PARENT,   "(A expected, got B)" },
    { "getExact table",             FN_GETEXACT, "{}",             KEY_A, 0U, 1,  ISAAC_LGC_REASON_USERDATA, "(A expected, got table)" },
    { "getExact foreign",           FN_GETEXACT, "objs.foreign",   KEY_A, 0U, 1,  ISAAC_LGC_REASON_IDENTITY, "(A expected, got userdata)" },
};

typedef struct stub_case {
    const char *name;
    int fn;
    const char *object;
    uint32_t key;
    uint32_t can_be_const;
    uint32_t reason;
} stub_case;

/* Shapes where the translated body would run lua_rawget on a non-table: the
 * seam must hand over before any side effect; __real is the recording stub. */
static const stub_case s_stub_cases[] = {
    { "getClass userdata without metatable", FN_GETCLASS, "nomt",      KEY_A,     0U, ISAAC_LGC_REASON_METATABLE },
    { "getClass light userdata",             FN_GETCLASS, "light",     KEY_A,     0U, ISAAC_LGC_REASON_METATABLE },
    { "getClass unregistered class key",     FN_GETCLASS, "A",         KEY_UNREG, 0U, ISAAC_LGC_REASON_CLASS },
    { "getClass const object, class has no __const", FN_GETCLASS, "X", KEY_X,     1U, ISAAC_LGC_REASON_CONST_CLASS },
    { "getClass __parent is a number",       FN_GETCLASS, "badparent", KEY_A,     0U, ISAAC_LGC_REASON_PARENT_TYPE },
    { "getExact without metatable",          FN_GETEXACT, "nomt",      KEY_A,     0U, ISAAC_LGC_REASON_METATABLE },
    { "getExact unregistered class key",     FN_GETEXACT, "A",         KEY_UNREG, 0U, ISAAC_LGC_REASON_CLASS },
};

static int run_stub_case(const stub_case *k)
{
    CPU *c = &s_cpu;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t real_before = s_real_calls;
    uint32_t fallbacks_before = st->fallbacks[k->fn];
    uint32_t handled_before = st->handled[k->fn];
    uint32_t entry_esp;
    lua_settop(s_L, 0);
    push_object(s_L, k->object);
    reset_cpu(c);
    (void)push_call(c, k->fn, k->key, k->can_be_const, 1);
    entry_esp = c->esp;
    s_real_mode = REAL_STUB;
    call_variant(c, k->fn, VARIANT_WRAP);
    s_real_mode = REAL_GENERATED;
    CHECK(s_real_calls == real_before + 1U);
    CHECK(s_real_contract_failures == 0U);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U && !c->fault);
    CHECK(st->fallbacks[k->fn] == fallbacks_before + 1U);
    CHECK(st->handled[k->fn] == handled_before);
    CHECK(st->last_reason == k->reason);
    CHECK(lua_gettop(s_L) == 1);
    printf("  stub  : %-44s reason=%u OK\n", k->name, (unsigned)k->reason);
    return 0;
}

static int run_direct_case(const direct_case *k)
{
    outcome translated, wrapped;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[k->fn];
    uint32_t fallbacks_before = st->fallbacks[k->fn];
    uint32_t hops_before = st->hops;
    uint32_t imports_before = st->imports_elided;
    uint32_t real_before = s_real_calls;
    uint32_t entry_esp;

    run_direct(k->fn, VARIANT_TRANSLATED, k->object, NULL, k->key,
               k->can_be_const, k->index, &translated);
    CHECK(translated.fault == NULL && s_fault_calls == 0U);
    run_direct(k->fn, VARIANT_WRAP, k->object, NULL, k->key, k->can_be_const,
               k->index, &wrapped);
    CHECK(wrapped.fault == NULL && s_fault_calls == 0U);
    CHECK(same_outcome(&translated, &wrapped));
    entry_esp = s_cpu.stack_ceiling - ESP_HEADROOM -
                (k->fn == FN_GETCLASS ? 12U : 8U);
    CHECK(wrapped.esp == entry_esp + 4U);              /* plain ret */
    CHECK(wrapped.eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    CHECK(wrapped.top == 1);                            /* stack-neutral */
    CHECK(wrapped.ebx == 0x0b0b0b0bU && wrapped.ebp == 0x0e0e0e0eU &&
          wrapped.esi == 0x51515151U && wrapped.edi == 0xd1d1d1d1U);
    CHECK(wrapped.ecx == s_state_word && wrapped.edx == (uint32_t)k->index);
    if (k->native) {
        CHECK(st->handled[k->fn] == handled_before + 1U);
        CHECK(st->fallbacks[k->fn] == fallbacks_before);
        CHECK(st->last_reason == ISAAC_LGC_HANDLED);
        CHECK(st->hops == hops_before + (k->fn == FN_GETCLASS ? k->hops : 0U));
        CHECK(st->imports_elided == imports_before + k->imports);
        CHECK(s_coverage[k->fn == FN_GETCLASS ? ISAAC_LGC_GETCLASS_COVERAGE
                                              : ISAAC_LGC_GETEXACT_COVERAGE] == 1U);
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
        CHECK(s_real_calls == real_before + 1U);        /* the verifier ran __real */
#else
        CHECK(s_real_calls == real_before);
#endif
    } else {
        CHECK(st->handled[k->fn] == handled_before);
        CHECK(st->fallbacks[k->fn] == fallbacks_before + 1U);
        CHECK(st->last_reason == k->reason);
        CHECK(s_real_calls == real_before + 1U);
        CHECK(s_real_contract_failures == 0U);
    }
    printf("  direct: %-36s eax=%08x esp=%08x top=%d %s\n", k->name,
           (unsigned)wrapped.eax, (unsigned)wrapped.esp, wrapped.top,
           k->native ? "NATIVE" : "fallback");
    return 0;
}

static int run_pcall_variant(const pcall_case *k, int variant, char *message,
                             size_t capacity, int *ok)
{
    char code[512];
    const char *text;
    lua_settop(s_L, 0);
    s_probe_fn = k->fn;
    s_probe_variant = variant;
    s_probe_key = k->key;
    s_probe_cbc = k->can_be_const;
    s_probe_index = k->index;
    s_in_seam = 0;
    snprintf(code, sizeof code,
             "local ok, m = pcall(probe, %s) return ok, tostring(m)", k->object);
    if (!run_chunk(&s_cpu, code, 2))
        return 1;
    *ok = lua_toboolean(s_L, -2);
    text = lua_tostring(s_L, -1);
    snprintf(message, capacity, "%s", text ? text : "(nil)");
    lua_settop(s_L, 0);
    return 0;
}

static int run_pcall_case(const pcall_case *k)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    char translated[512], wrapped[512];
    int ok_translated, ok_wrapped;
    uint32_t fallbacks_before, handled_before, real_before;

    CHECK(run_pcall_variant(k, VARIANT_TRANSLATED, translated, sizeof translated,
                            &ok_translated) == 0);
    CHECK(!ok_translated);
    fallbacks_before = st->fallbacks[k->fn];
    handled_before = st->handled[k->fn];
    real_before = s_real_calls;
    CHECK(run_pcall_variant(k, VARIANT_WRAP, wrapped, sizeof wrapped,
                            &ok_wrapped) == 0);
    CHECK(!ok_wrapped);
    printf("  pcall : %-30s reason=%-2u \"%s\"\n", k->name, (unsigned)k->reason, wrapped);
    if (strcmp(translated, wrapped) != 0)
        printf("          translated: \"%s\"\n", translated);
    CHECK(strcmp(translated, wrapped) == 0);
    CHECK(strstr(wrapped, k->expect) != NULL);
    CHECK(st->fallbacks[k->fn] == fallbacks_before + 1U);
    CHECK(st->handled[k->fn] == handled_before);
    CHECK(st->last_reason == k->reason);
    CHECK(s_real_calls == real_before + 1U);
    CHECK(s_real_contract_failures == 0U);
    CHECK(s_fault_calls == 0U);
    return 0;
}

/* A finalizer implemented as a guest closure runs from GC steps that the
 * seam's own lua_pushstring triggers; it needs the bridge owner. */
static int run_gc_stress(void)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[FN_GETCLASS];
    uint32_t fallbacks_before = st->fallbacks[FN_GETCLASS];
    lua_settop(s_L, 0);
    s_probe_fn = FN_GETCLASS;
    s_probe_variant = VARIANT_WRAP;
    s_probe_key = KEY_A;
    s_probe_cbc = 0U;
    s_probe_index = 1;
    s_gc_calls = 0U;
    s_gc_in_seam = 0U;
    /* Every iteration leaves finalizable garbage and a positive GC debt with
     * no GC check between it and the getClass call (a growing OP_SETTABLE
     * does that in the game; gc_debt() makes it happen on every iteration),
     * so the seam's own lua_pushstring is the checkpoint that steps the
     * collector and runs the guest __gc closure. */
    CHECK(run_chunk(&s_cpu,
        "local mt = { __gc = gc_probe }\n"
        "local a = objs.A\n"
        "local expect = tostring(probe(a))\n"
        "local t = {}\n"
        "for i = 1, 40000 do\n"
        "  local u = newud(mt)\n"
        "  t[i] = i\n"
        "  gc_debt()\n"
        "  if tostring(probe(a)) ~= expect then error('pointer drift at ' .. i) end\n"
        "end\n"
        "collectgarbage()\n"
        "return true", 1));
    CHECK(lua_toboolean(s_L, -1));
    lua_settop(s_L, 0);
    CHECK(st->handled[FN_GETCLASS] == handled_before + 40001U);
    CHECK(st->fallbacks[FN_GETCLASS] == fallbacks_before);
    CHECK(s_gc_calls >= 40000U);
    CHECK(s_gc_in_seam > 0U);
    CHECK(s_fault_calls == 0U);
    printf("  gc    : 40001 native getClass under allocation pressure; guest __gc "
           "calls=%u, of which inside the seam=%u\n",
           (unsigned)s_gc_calls, (unsigned)s_gc_in_seam);
    return 0;
}

static int run_stack_and_owner_cases(void)
{
    CPU *c = &s_cpu;
    CPU other;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t real_before = s_real_calls;
    uint32_t entry_esp;
    int root = 0;

    /* esp outside the bound stack: no Lua call, straight to the body */
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
    c->stack_owner = NULL;
    entry_esp = c->esp;
    s_real_mode = REAL_STUB;
    call_variant(c, FN_GETCLASS, VARIANT_WRAP);
    s_real_mode = REAL_GENERATED;
    CHECK(s_real_calls == real_before + 1U && st->last_reason == ISAAC_LGC_REASON_STACK);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U);
    c->stack_owner = c;
    printf("  stub  : %-44s reason=%u OK\n", "unbound guest stack", (unsigned)ISAAC_LGC_REASON_STACK);

    /* another CPU owns the Lua bridge */
    memset(&other, 0, sizeof other);
    CHECK(isaac_vita_lua_seam_enter(&other, &root) && root == 1);
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
    entry_esp = c->esp;
    s_real_mode = REAL_STUB;
    call_variant(c, FN_GETCLASS, VARIANT_WRAP);
    s_real_mode = REAL_GENERATED;
    CHECK(s_real_calls == real_before + 2U && st->last_reason == ISAAC_LGC_REASON_OWNER);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U);
    isaac_vita_lua_seam_leave(&other, root);
    CHECK(s_real_contract_failures == 0U && s_fault_calls == 0U);
    printf("  stub  : %-44s reason=%u OK\n", "bridge owned by another CPU", (unsigned)ISAAC_LGC_REASON_OWNER);

    /* the owner is released: a native call succeeds again */
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(c);
    (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
    call_variant(c, FN_GETCLASS, VARIANT_WRAP);
    CHECK(st->last_reason == ISAAC_LGC_HANDLED && c->eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    return 0;
}

/* --- hostile cases (review) -------------------------------------------------------- */

/* The relative-index quirk: with index -2 the bodies test the userdata at
 * depth entry+1 (the class table is already pushed) but return
 * lua_touserdata(index) at the entry depth -- a different slot.  The seam
 * must replicate that, not correct it. */
static int run_negative_index_quirk(void)
{
    outcome translated, wrapped;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t handled_before = st->handled[FN_GETCLASS];
    run_direct(FN_GETCLASS, VARIANT_TRANSLATED, "A", "B", KEY_B, 0U, -2, &translated);
    CHECK(translated.fault == NULL);
    run_direct(FN_GETCLASS, VARIANT_WRAP, "A", "B", KEY_B, 0U, -2, &wrapped);
    CHECK(wrapped.fault == NULL && s_fault_calls == 0U);
    CHECK(same_outcome(&translated, &wrapped));
    CHECK(wrapped.top == 2);
    CHECK(wrapped.eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));   /* A's block */
    CHECK(wrapped.eax != (uint32_t)(uintptr_t)lua_touserdata(s_L, 2));   /* not B's */
    CHECK(st->handled[FN_GETCLASS] == handled_before + 1U);
    CHECK(st->last_reason == ISAAC_LGC_HANDLED);
    printf("  quirk : %-36s eax=%08x (slot 1; class B checked on slot 2) NATIVE\n",
           "getClass index -2 of [A, B] as B", (unsigned)wrapped.eax);
    return 0;
}

/* A guest upvalue pseudo-index argument: getClass(L, lua_upvalueindex(1), ..)
 * from a guest closure whose guest upvalue #1 is the userdata.  Every index
 * the translated bodies pass crosses vita_lua_index (native upvalue #1 is
 * the bridge's target word); the seam's lgc_index must agree. */
static int run_upvalue_case(void)
{
    CPU *c = &s_cpu;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t closure_arguments[] = { s_state_word, GUEST_PROBE, 1U };
    uint32_t global_arguments[] = { s_state_word, (uint32_t)(uintptr_t)"probe_uv" };
    char translated[512], wrapped[512];
    int variant;

    lua_settop(s_L, 0);
    push_object(s_L, "A");                       /* guest upvalue #1 */
    reset_cpu(c);
    CHECK(invoke_import(c, LUA_IMPORT_PUSHCLOSURE, closure_arguments, 3U));
    CHECK(invoke_import(c, LUA_IMPORT_SETGLOBAL, global_arguments, 2U));
    CHECK(lua_gettop(s_L) == 0);
    for (variant = VARIANT_TRANSLATED; variant <= VARIANT_WRAP; ++variant) {
        char *message = variant == VARIANT_WRAP ? wrapped : translated;
        uint32_t handled_before = st->handled[FN_GETCLASS];
        uint32_t real_before = s_real_calls;
        const char *text;
        s_probe_fn = FN_GETCLASS;
        s_probe_variant = variant;
        s_probe_key = KEY_A;
        s_probe_cbc = 0U;
        s_probe_index = LUA_REGISTRYINDEX - 1;     /* guest lua_upvalueindex(1) */
        CHECK(run_chunk(c, "return tostring(probe_uv())", 1));
        text = lua_tostring(s_L, -1);
        snprintf(message, 512, "%s", text ? text : "(nil)");
        lua_settop(s_L, 0);
        if (variant == VARIANT_WRAP) {
            CHECK(st->handled[FN_GETCLASS] == handled_before + 1U);
            CHECK(st->last_reason == ISAAC_LGC_HANDLED);
#if !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
            CHECK(s_real_calls == real_before);
#else
            (void)real_before;
#endif
        }
    }
    CHECK(strcmp(translated, wrapped) == 0);
    push_object(s_L, "A");
    lua_pushlightuserdata(s_L, lua_touserdata(s_L, -1));
    CHECK(strcmp(luaL_tolstring(s_L, -1, NULL), wrapped) == 0);   /* objs.A's block */
    lua_settop(s_L, 0);
    printf("  upval : %-36s %s NATIVE\n", "getClass via guest lua_upvalueindex(1)", wrapped);
    /* the wrong class through the same pseudo-index: identical argerror text */
    for (variant = VARIANT_TRANSLATED; variant <= VARIANT_WRAP; ++variant) {
        char *message = variant == VARIANT_WRAP ? wrapped : translated;
        const char *text;
        s_probe_variant = variant;
        s_probe_key = KEY_B;
        CHECK(run_chunk(c, "local ok, m = pcall(probe_uv) return ok, tostring(m)", 2));
        CHECK(!lua_toboolean(s_L, -2));
        text = lua_tostring(s_L, -1);
        snprintf(message, 512, "%s", text ? text : "(nil)");
        lua_settop(s_L, 0);
    }
    CHECK(strcmp(translated, wrapped) == 0);
    CHECK(strstr(wrapped, "(B expected, got A)") != NULL);
    CHECK(st->last_reason == ISAAC_LGC_REASON_PARENT);
    printf("  upval : %-36s \"%s\"\n", "wrong class via upvalue", wrapped);
    CHECK(s_real_contract_failures == 0U && s_fault_calls == 0U);
    return 0;
}

/* The seam as the OUTERMOST bridge owner (a direct call with no import scope
 * active) while the collector step inside its own lua_pushstring finalizes
 * an object of class G whose guest __gc calls getExact: the nested wrap
 * must find the owner (same CPU, root=0), be served natively, and the outer
 * activation must still release the slot and return the right pointer. */
static int run_nested_gc_exact_case(void)
{
    CPU *c = &s_cpu;
    CPU other;
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    uint32_t i;
    int root = 0;
    s_gc_exact_calls = s_gc_exact_native = s_gc_exact_bad = 0U;
    for (i = 0U; i < 2000U && s_gc_exact_calls == 0U; ++i) {
        uint32_t handled_before;
        lua_settop(s_L, 0);
        push_object(s_L, "A");
        /* finalizable garbage of class G (__gc = gc_exact) */
        lua_newuserdata(s_L, 16U);
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_ref_G);
        lua_setmetatable(s_L, -2);
        lua_pop(s_L, 1);
        luaE_setdebt(G(s_L), 1);
        reset_cpu(c);
        (void)push_call(c, FN_GETCLASS, KEY_A, 0U, 1);
        handled_before = st->handled[FN_GETCLASS];
        call_variant(c, FN_GETCLASS, VARIANT_WRAP);
        CHECK(!c->fault);
        CHECK(st->handled[FN_GETCLASS] == handled_before + 1U);
        CHECK(c->eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
        CHECK(lua_gettop(s_L) == 1);
        /* the outermost activation released the owner slot */
        memset(&other, 0, sizeof other);
        CHECK(isaac_vita_lua_seam_enter(&other, &root) && root == 1);
        isaac_vita_lua_seam_leave(&other, root);
    }
    CHECK(s_gc_exact_calls > 0U);
    CHECK(drain_finalizers(c));            /* the loop's remaining G objects */
    CHECK(s_gc_exact_native == s_gc_exact_calls);
    CHECK(s_gc_exact_bad == 0U);
    CHECK(s_real_contract_failures == 0U && s_fault_calls == 0U);
    printf("  nested: %-36s __gc getExact calls=%u native=%u (outer root=1, %u iterations)\n",
           "guest __gc -> getExact inside the seam", (unsigned)s_gc_exact_calls,
           (unsigned)s_gc_exact_native, (unsigned)i);
    return 0;
}

/* LUA_ERRGCMM raised by a guest __gc from the collector step that the body's
 * own lua_pushstring performs: the longjmp crosses the seam's native frames
 * (wrap) or the import handler's (translated).  Lua's pcall must see the same
 * status and text, the bridge must recover the abandoned callback frame, the
 * owner slot must be free afterwards, and the next call must work. */
static int run_gc_error_variant(int variant, char *message, size_t capacity,
                                int *ok, int *raised_inside)
{
    const char *text;
    lua_settop(s_L, 0);
    s_probe_fn = FN_GETCLASS;
    s_probe_variant = variant;
    s_probe_key = KEY_A;
    s_probe_cbc = 0U;
    s_probe_index = 1;
    s_gc_raise_calls = 0U;
    s_gc_raise_in_seam = 0U;
    s_in_seam = 0;
    /* objs.B is derived: the body pushes "__const" and "__parent" (two GC
     * checkpoints); each iteration leaves one finalizable object behind and
     * a positive debt right before the call. */
    if (!run_chunk(&s_cpu,
        "local mt = { __gc = gc_raise }\n"
        "local b = objs.B\n"
        "for i = 1, 20000 do\n"
        "  local u = newud(mt)\n"
        "  gc_debt()\n"
        "  local ok, m = pcall(probe, b)\n"
        "  if not ok then return ok, tostring(m), i end\n"
        "end\n"
        "return true, 'no finalizer raised inside getClass in 20000 iterations', 0", 3))
        return 1;
    *ok = lua_toboolean(s_L, -3);
    text = lua_tostring(s_L, -2);
    snprintf(message, capacity, "%s", text ? text : "(nil)");
    *raised_inside = s_gc_raise_in_seam > 0U;
    s_in_seam = 0;                 /* the longjmp skipped call_variant's reset */
    lua_settop(s_L, 0);
    /* drain the remaining raising finalizers (each collection propagates at
     * most one LUA_ERRGCMM) so later native calls cannot meet one */
    if (!drain_finalizers(&s_cpu))
        return 1;
    return 0;
}

static int run_gc_error_case(void)
{
    const isaac_vita_lua_getclass_stats *st = isaac_vita_lua_getclass_stats_get();
    CPU other;
    char translated[512], wrapped[512];
    int ok_translated, ok_wrapped, inside_translated, inside_wrapped, root = 0;
    uint32_t calls_before, done_before, abandoned;

    CHECK(run_gc_error_variant(VARIANT_TRANSLATED, translated, sizeof translated,
                               &ok_translated, &inside_translated) == 0);
    CHECK(!ok_translated && inside_translated);
    calls_before = st->calls[FN_GETCLASS];
    done_before = st->handled[FN_GETCLASS] + st->fallbacks[FN_GETCLASS];
    CHECK(run_gc_error_variant(VARIANT_WRAP, wrapped, sizeof wrapped,
                               &ok_wrapped, &inside_wrapped) == 0);
    CHECK(!ok_wrapped && inside_wrapped);
    printf("  gcerr : %-36s \"%s\"\n", "guest __gc raises inside getClass", wrapped);
    if (strcmp(translated, wrapped) != 0)
        printf("          translated: \"%s\"\n", translated);
    CHECK(strcmp(translated, wrapped) == 0);
    CHECK(strstr(wrapped, "error in __gc metamethod (boom)") != NULL);
    /* the activation the longjmp abandoned is counted as a call but neither
     * handled nor fallen back (0 when VERIFY's re-run took the throw) */
    abandoned = (st->calls[FN_GETCLASS] - calls_before) -
                (st->handled[FN_GETCLASS] + st->fallbacks[FN_GETCLASS] - done_before);
    CHECK(abandoned <= 1U);
    CHECK(s_fault_calls == 0U && s_real_contract_failures == 0U);
    /* the owner slot is free and a plain native call works again */
    memset(&other, 0, sizeof other);
    CHECK(isaac_vita_lua_seam_enter(&other, &root) && root == 1);
    isaac_vita_lua_seam_leave(&other, root);
    lua_settop(s_L, 0);
    push_object(s_L, "A");
    reset_cpu(&s_cpu);
    (void)push_call(&s_cpu, FN_GETCLASS, KEY_A, 0U, 1);
    call_variant(&s_cpu, FN_GETCLASS, VARIANT_WRAP);
    CHECK(!s_cpu.fault && st->last_reason == ISAAC_LGC_HANDLED &&
          s_cpu.eax == (uint32_t)(uintptr_t)lua_touserdata(s_L, 1));
    lua_settop(s_L, 0);
    printf("  gcerr : abandoned wrap activations=%u; bridge recovered, owner free, next call native\n",
           (unsigned)abandoned);
    return 0;
}

static int setup_lua(void)
{
    CPU *c = &s_cpu;
    uint32_t openlibs_arguments[1];
    int ref_A, ref_Ac = 0, ref_B, ref_Bc = 0, ref_C, ref_Cc = 0, ref_X, ref_dummy;
    int ref_foreign, ref_wrongid, ref_badparent, previous, i;

    reset_cpu(c);
    CHECK(invoke_import(c, LUA_IMPORT_NEWSTATE_AUX, NULL, 0U));
    s_state_word = c->eax;
    s_L = (lua_State *)(uintptr_t)s_state_word;
    CHECK(s_L != NULL);
    (void)lua_atpanic(s_L, oracle_panic);
    openlibs_arguments[0] = s_state_word;
    CHECK(invoke_import(c, LUA_IMPORT_OPENLIBS, openlibs_arguments, 1U));
    CHECK(register_closure(c, GUEST_PROBE, "probe"));
    CHECK(register_closure(c, GUEST_GC, "gc_probe"));
    CHECK(register_closure(c, GUEST_GC_RAISE, "gc_raise"));
    CHECK(register_closure(c, GUEST_GC_EXACT, "gc_exact"));
    lua_pushcfunction(s_L, native_newud);
    lua_setglobal(s_L, "newud");
    lua_pushcfunction(s_L, native_gc_debt);
    lua_setglobal(s_L, "gc_debt");
    lua_newtable(s_L);
    lua_setglobal(s_L, "objs");

    ref_A = make_class(s_L, "A", 0, 0, KEY_A, 1, &ref_Ac);
    ref_B = make_class(s_L, "B", ref_A, ref_Ac, KEY_B, 1, &ref_Bc);
    ref_C = make_class(s_L, "C", ref_B, ref_Bc, KEY_C, 1, &ref_Cc);
    ref_X = make_class(s_L, "X", 0, 0, KEY_X, 0, &ref_dummy);
    /* G: a class whose __gc is the guest closure calling getExact */
    s_ref_G = make_class(s_L, "G", 0, 0, KEY_G, 0, &ref_dummy);
    lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_ref_G);
    lua_getglobal(s_L, "gc_exact");
    lua_setfield(s_L, -2, "__gc");
    lua_pop(s_L, 1);
    /* foreign: a metatable without the identity key */
    lua_newtable(s_L);
    lua_pushstring(s_L, "F");
    lua_setfield(s_L, -2, "__type");
    ref_foreign = luaL_ref(s_L, LUA_REGISTRYINDEX);
    /* wrongid: identity present but not a boolean */
    lua_newtable(s_L);
    lua_pushinteger(s_L, 1);
    lua_rawsetp(s_L, -2, IDENTITY);
    lua_pushstring(s_L, "W");
    lua_setfield(s_L, -2, "__type");
    ref_wrongid = luaL_ref(s_L, LUA_REGISTRYINDEX);
    /* badparent: mutable (has __const) with a non-table __parent */
    lua_newtable(s_L);
    lua_pushboolean(s_L, 1);
    lua_rawsetp(s_L, -2, IDENTITY);
    lua_pushstring(s_L, "BP");
    lua_setfield(s_L, -2, "__type");
    lua_newtable(s_L);
    lua_setfield(s_L, -2, "__const");
    lua_pushinteger(s_L, 5);
    lua_setfield(s_L, -2, "__parent");
    ref_badparent = luaL_ref(s_L, LUA_REGISTRYINDEX);
    /* P0 <- P1 <- ... <- P20: deeper than the native hop bound */
    previous = 0;
    for (i = 0; i <= 20; ++i) {
        char name[8];
        int unused_const = 0;
        snprintf(name, sizeof name, "P%d", i);
        previous = make_class(s_L, name, previous, 0, i == 0 ? (uint32_t)KEY_P0 : 0U, 1,
                              &unused_const);
    }
    make_object(s_L, ref_A, "A");
    make_object(s_L, ref_Ac, "Ac");
    make_object(s_L, ref_B, "B");
    make_object(s_L, ref_Bc, "Bc");
    make_object(s_L, ref_C, "C");
    make_object(s_L, ref_X, "X");
    make_object(s_L, ref_foreign, "foreign");
    make_object(s_L, ref_wrongid, "wrongid");
    make_object(s_L, ref_badparent, "badparent");
    make_object(s_L, 0, "nomt");
    make_object(s_L, previous, "P20");
    (void)ref_C;
    (void)ref_Cc;
    CHECK(lua_gettop(s_L) == 0);
    return 0;
}

int main(void)
{
    const isaac_vita_lua_getclass_stats *st;
    uint32_t i, expected_verify_runs, handled;
    CPU *c = &s_cpu;
    uint32_t close_arguments[1];

    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not swallow the transcript */
    /* the PE pages the generated bodies address absolutely */
    CHECK(map_fixed(VA(LUA_IAT_PAGE_RVA), 0x1000U));
    CHECK(map_fixed(VA(RDATA_PAGE_RVA), 0x1000U));
    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i)
        st32(VA(LUA_IAT_FIRST_RVA) + 4U * i, (uint32_t)IMPORT_TOKEN_BASE + i);
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_TYPE_RVA), "__type");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_FORMAT_RVA), "%s expected, got %s");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_CONST_RVA), "__const");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_CANNOT_CONST_RVA), "cannot be const");
    strcpy((char *)(uintptr_t)VA(ISAAC_LGC_STR_PARENT_RVA), "__parent");

    CHECK(setup_lua() == 0);
    st = isaac_vita_lua_getclass_stats_get();

    for (i = 0U; i < sizeof s_direct_cases / sizeof s_direct_cases[0]; ++i)
        CHECK(run_direct_case(&s_direct_cases[i]) == 0);
    for (i = 0U; i < sizeof s_pcall_cases / sizeof s_pcall_cases[0]; ++i)
        CHECK(run_pcall_case(&s_pcall_cases[i]) == 0);
    for (i = 0U; i < sizeof s_stub_cases / sizeof s_stub_cases[0]; ++i)
        CHECK(run_stub_case(&s_stub_cases[i]) == 0);
    CHECK(run_stack_and_owner_cases() == 0);
    CHECK(run_gc_stress() == 0);
    CHECK(run_negative_index_quirk() == 0);
    CHECK(run_upvalue_case() == 0);
    CHECK(run_nested_gc_exact_case() == 0);
    CHECK(run_gc_error_case() == 0);

    handled = st->handled[0] + st->handled[1];
#if defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY)
    /* first 64 hits, then every 4096th (hit counter multiples in (64, handled]) */
    expected_verify_runs = handled < 64U ? handled : 64U + handled / 4096U;
    /* a verify re-run that took the __gc throw counted its hit but not a run */
    CHECK(st->verify_runs == expected_verify_runs ||
          st->verify_runs + 1U == expected_verify_runs);
    expected_verify_runs = st->verify_runs;
    CHECK(st->verify_mismatches == 0U);
#else
    expected_verify_runs = 0U;
    CHECK(st->verify_runs == 0U && st->verify_mismatches == 0U);
#endif
    CHECK(s_real_contract_failures == 0U);
    CHECK(s_fault_calls == 0U);
    isaac_vita_lua_getclass_stats_log();

    close_arguments[0] = s_state_word;
    reset_cpu(c);
    CHECK(invoke_import(c, LUA_IMPORT_CLOSE, close_arguments, 1U));
    printf("native getclass guest oracle: PASS; direct=%u pcall=%u stub=%u hostile=4 "
           "native=%u fallbacks=%u gc_in_seam=%u verify=%u/%u\n",
           (unsigned)(sizeof s_direct_cases / sizeof s_direct_cases[0]),
           (unsigned)(sizeof s_pcall_cases / sizeof s_pcall_cases[0]),
           (unsigned)(sizeof s_stub_cases / sizeof s_stub_cases[0] + 2U),
           (unsigned)handled, (unsigned)(st->fallbacks[0] + st->fallbacks[1]),
           (unsigned)s_gc_in_seam, (unsigned)st->verify_mismatches,
           (unsigned)expected_verify_runs);
    return 0;
}
