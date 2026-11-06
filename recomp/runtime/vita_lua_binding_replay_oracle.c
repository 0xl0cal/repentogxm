/* Host oracle for ISAAC_VITA_LUA_BINDING_REPLAY
 * (recomp/runtime/host_vita_lua_binding_replay.c), built and run by
 * recomp/test_vita_lua_binding_replay.py as a 32-bit MSVC executable (the
 * frozen Lua ABI is ILP32: raw lua_State and userdata pointers cross the
 * bridge).
 *
 * It links
 *   - the GENERATED bodies, extracted verbatim from the corpus by the test:
 *     the 54 replayed bodies as lbr_t_sub_<rva> (the translated reference),
 *     the helpers sub_003f8820 / sub_003f8840 / sub_00280cc0 as they are, and
 *     getClass / getExact as __real_sub_003f8e30 / __real_sub_003f8c80,
 *   - the production replay TU (__wrap_sub_<rva>, the mirrors), the production
 *     bridge host_vita_lua.c, the native getClass seam host_vita_lua_getclass.c
 *     and pristine Lua 5.3.3,
 *   - this file: the runtime stubs, guest_call / guest_import_call (guest.c
 *     minus diagnostics), the five GUEST_STACK_REQUIRED=1 stack validators
 *     copied verbatim from guest.c (production sets REQUIRED=1 target-wide,
 *     so the oracle does too), the fake guest members and finalizers, the fixture,
 *     and lbr_oracle_tap_pushstring, which the replay TU's one lua_pushstring
 *     call reaches in this build only (/Dlua_pushstring=..., see the tap).
 * The frozen PE is mapped at GUEST_IMAGE_BASE exactly as the loader leaves
 * it (sections, .reloc applied, every IAT word tagged with its own VA), from
 * the image file the test writes, so the replay's pins run against the real
 * bytes.
 *
 * The comparison is a fork.  Every case loads a Lua chunk, then snapshots the
 * whole world (the deterministic fixed-VA arena that holds the Lua heap and
 * the allocator's own state, the guest stack including a guard zone, the CPU
 * struct, function coverage, the function-entry counter, the bridge owner
 * word), runs the chunk with every replayed body dispatched to its
 * TRANSLATED body, captures the world, restores the snapshot, runs it again
 * with every replayed body dispatched to its __wrap_, and requires the two
 * worlds to be byte-identical, plus the same ordered event log (body entries
 * and exits with eax/esp, every fake member and finalizer call with its
 * arguments), the same chunk status/result text and the same fault calls.
 * The host import census differs by exactly the crossings the replay elided.
 * VERIFY builds compare results, not the arena (the verifier runs a member
 * twice by design).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "lstate.h"      /* G(L) / luaE_setdebt: GC pressure (pristine Lua) */
#include "lfunc.h"       /* UpVal: closed upvalues in canon_lua_heap */
#include <stddef.h>

#include "host_vita_import_id.h"
#include "host_vita_lua.h"
#include "host_vita_lua_getclass.h"
#include "host_vita_lua_binding_replay.h"
#include "host_vita_lua_binding_table.h"
#include "host_vita_startup.h"

#if !defined(ISAAC_VITA_LUA_BINDING_REPLAY_ORACLE) || \
    !defined(ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP)
#error build through recomp/test_vita_lua_binding_replay.py
#endif
#if !GUEST_STACK_REQUIRED
#error This oracle mirrors production: compile with GUEST_STACK_REQUIRED=1
#endif
#ifndef ISAAC_LBR_WINDOW_BELOW
#define ISAAC_LBR_WINDOW_BELOW 0x100U
#endif
#if defined(ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY)
#define ORACLE_VERIFY 1
#else
#define ORACLE_VERIFY 0
#endif
/* run-time spellings of the build constants (no constant conditions) */
static const int s_verify = ORACLE_VERIFY;
static const int s_window_full = ISAAC_LBR_WINDOW_BELOW >= 0x100U;
static const uint32_t s_family_mask = ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK;

#define VA(rva) ((uint32_t)GUEST_IMAGE_BASE + (uint32_t)(rva))
#define IDENTITY ((const void *)(uintptr_t)VA(ISAAC_LGC_IDENTITY_KEY_RVA))
#define ENTITY_KEY 0x988066c0U     /* guest_0131.c sub_00446d90: push 0x988066c0 */
#define IMPORT_RETURN 0xfff15000U
#define CALLBACK_FRAME_GUARD 0x4000U

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "binding replay oracle failed at %d: %s\n", __LINE__, \
                #condition); \
        exit(1); \
    } \
} while (0)

/* ---- runtime stubs ------------------------------------------------------- */

#define COVERAGE_BYTES 65536U
static unsigned char s_cov_functions[COVERAGE_BYTES];
static unsigned char s_cov_imports[1024];
unsigned char *g_guest_coverage_functions = s_cov_functions;
unsigned char *g_guest_coverage_imports = s_cov_imports;
unsigned char *g_guest_coverage_cases;
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
uint32_t g_guest_function_entries;
#define ENTRIES() g_guest_function_entries
#else
#define ENTRIES() 0U
#endif

static uint32_t s_fault_calls;
static const char *s_first_fault;

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
    if (!s_first_fault)
        s_first_fault = what;
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

/* Verbatim GUEST_STACK_REQUIRED=1 validators from guest.c (guest.c:1002-1092
 * at 1262842).  The production bridge, the getClass seam and the replay
 * TU's checked gpop reach these, as on the Vita; the generated bodies keep
 * the raw GUEST_GENERATED_STACK_GUARD=0 operations. */
GUEST_STACK_HOT_NOINLINE int guest_stack_set(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(value - c->stack_floor > capacity))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_SET, value, 0U);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

GUEST_STACK_HOT_NOINLINE int guest_stack_adjust(
    CPU *__restrict c, uint32_t amount, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            offset > capacity || amount > capacity - offset))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ADJUST, c->esp, amount);
    c->esp += amount;
    return 1;
}

GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address(
    CPU *__restrict c, uint32_t address, uint32_t size, uint32_t pc)
{
    uint32_t capacity, offset;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            size == 0U || size > capacity || offset > capacity - size)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ACCESS, address, size);
        return 0U;
    }
    guest_stack_note_low(c, address);
    return address;
}

GUEST_STACK_HOT_NOINLINE void gpush_at(
    CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity, offset, next;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset < 4U || offset > capacity)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_PUSH, c->esp - 4U, 4U);
        return;
    }
    next = c->esp - 4U;
    c->esp = next;
    guest_stack_note_low(c, next);
    st32(next, value);
}

GUEST_STACK_HOT_NOINLINE uint32_t gpop_at(
    CPU *__restrict c, uint32_t pc)
{
    uint32_t capacity, offset, value;
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(capacity < 4U || offset > capacity - 4U)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_POP, c->esp, 4U);
        return 0U;
    }
    value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    (void)native_path;
    (void)capacity;
    guest_fault(c, guest_path, "oracle has no path mapper");
    return 0;
}

/* Log capture: every line is kept for pattern checks; the first ones print. */
#define LOG_KEEP 4096U
static char s_log[LOG_KEEP][256];
static uint32_t s_log_lines;

void isaac_vita_log(const char *format, ...)
{
    va_list args;
    char line[768];
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (s_log_lines < LOG_KEEP)
        snprintf(s_log[s_log_lines], sizeof s_log[0], "%s", line);
    if (++s_log_lines <= 40U)
        printf("  log: %s\n", line);
}

static uint32_t log_count(const char *needle)
{
    uint32_t i, n = 0U;
    for (i = 0U; i < s_log_lines && i < LOG_KEEP; ++i)
        if (strstr(s_log[i], needle))
            ++n;
    return n;
}

/* ---- deterministic fixed-VA arena (the Lua heap) -------------------------- */
/* All allocator state lives at the arena base, so restoring the arena bytes
 * restores the allocator too.  Blocks: 8-byte header {capacity, tag}, payload
 * 8-aligned; capacities are 16-byte classes up to 4 KiB (one free list each)
 * and 4 KiB multiples above (one exact-fit free list); bump otherwise. */

#define ARENA_BASE 0xa0000000U
#define ARENA_SIZE (48U << 20)
#define ARENA_SMALL_MAX 4096U
#define TAG_LIVE 0xa110c8edU
#define TAG_FREE 0xf4eef4eeU

typedef struct arena_header {
    uint32_t bump;                                /* offset of the next fresh block */
    uint32_t classes[ARENA_SMALL_MAX / 16U + 1U];   /* free list heads (payload VAs) */
    uint32_t large;
    uint32_t live;
} arena_header;

#define ARENA ((arena_header *)(uintptr_t)ARENA_BASE)

static void arena_init(void)
{
    void *p = VirtualAlloc((void *)(uintptr_t)ARENA_BASE, ARENA_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(p == (void *)(uintptr_t)ARENA_BASE);
    memset(ARENA, 0, sizeof *ARENA);
    ARENA->bump = (uint32_t)((sizeof(arena_header) + 15U) & ~15U);
}

static uint32_t arena_capacity(size_t size)
{
    if (size <= ARENA_SMALL_MAX)
        return size < 16U ? 16U : (uint32_t)((size + 15U) & ~(size_t)15U);
    return (uint32_t)((size + 4095U) & ~(size_t)4095U);
}

/* Every byte Lua did not write is zero: blocks are zeroed when allocated,
 * when freed, and beyond the new size of an in-place realloc.  Lua 5.3 never
 * reads such bytes, but they reach the heap through partially written values
 * (the upper half of the 8-byte Value of a pointer, boolean or light userdata
 * TValue keeps the slot's previous bytes), so without this the T and R arena
 * images would differ in bytes neither arm can observe. */
static void *arena_malloc(size_t size)
{
    uint32_t cap = arena_capacity(size), block, *link;
    if (size > ARENA_SIZE)
        return NULL;
    if (cap <= ARENA_SMALL_MAX && ARENA->classes[cap / 16U]) {
        uint32_t payload = ARENA->classes[cap / 16U];
        ARENA->classes[cap / 16U] = ld32(payload);
        st32(payload - 4U, TAG_LIVE);
        ++ARENA->live;
        memset((void *)(uintptr_t)payload, 0, cap);
        return (void *)(uintptr_t)payload;
    }
    if (cap > ARENA_SMALL_MAX) {
        for (link = &ARENA->large; *link; link = (uint32_t *)(uintptr_t)*link) {
            if (ld32(*link - 8U) == cap) {
                uint32_t payload = *link;
                *link = ld32(payload);
                st32(payload - 4U, TAG_LIVE);
                ++ARENA->live;
                memset((void *)(uintptr_t)payload, 0, cap);
                return (void *)(uintptr_t)payload;
            }
        }
    }
    block = ARENA->bump;
    if (block + 8U + cap > ARENA_SIZE)
        return NULL;
    ARENA->bump = block + 8U + cap;
    st32(ARENA_BASE + block, cap);
    st32(ARENA_BASE + block + 4U, TAG_LIVE);
    ++ARENA->live;
    /* past the restored bump the other arm's blocks are still there */
    memset((void *)(uintptr_t)(ARENA_BASE + block + 8U), 0, cap);
    return (void *)(uintptr_t)(ARENA_BASE + block + 8U);
}

static int arena_free(void *pointer)
{
    uint32_t payload = (uint32_t)(uintptr_t)pointer, cap;
    if (!pointer)
        return 1;
    CHECK(payload - ARENA_BASE < ARENA_SIZE && ld32(payload - 4U) == TAG_LIVE);
    cap = ld32(payload - 8U);
    st32(payload - 4U, TAG_FREE);
    --ARENA->live;
    memset(pointer, 0, cap);
    if (cap <= ARENA_SMALL_MAX) {
        st32(payload, ARENA->classes[cap / 16U]);
        ARENA->classes[cap / 16U] = payload;
    } else {
        st32(payload, ARENA->large);
        ARENA->large = payload;
    }
    return 1;
}

void *isaac_vita_guest_malloc(size_t size) { return arena_malloc(size); }

void *isaac_vita_guest_realloc(void *pointer, size_t size, int *valid_owner)
{
    uint32_t cap;
    void *copy;
    if (valid_owner)
        *valid_owner = 1;
    if (!pointer)
        return arena_malloc(size);
    cap = ld32((uint32_t)(uintptr_t)pointer - 8U);
    if (size <= cap) {
        memset((char *)pointer + size, 0, cap - size);
        return pointer;
    }
    copy = arena_malloc(size);
    if (!copy)
        return NULL;
    memcpy(copy, pointer, cap);
    (void)arena_free(pointer);
    return copy;
}

int isaac_vita_guest_free(void *pointer) { return arena_free(pointer); }

/* ---- imports: guest_import_name, the direct binding, the two routes -------- */

typedef struct oracle_import {
    uint32_t slot;
    const char *name;
} oracle_import;

static const oracle_import s_imports[] = {
#include "lbr_oracle_imports.inc"
};
#define IMPORT_COUNT ((uint32_t)(sizeof s_imports / sizeof s_imports[0]))

static guest_import_direct s_direct[GUEST_IMPORT_DIRECT_CAPACITY];
static unsigned s_census;            /* g_host_import_calls of this oracle */
static uint32_t s_bad_name_slot;     /* pin leg: guest_import_name lies here */
static uint32_t s_bad_binding_id = 0xffffffffU;  /* pin leg: foreign endpoint */

static int import_index_of_slot(uint32_t slot)
{
    uint32_t lo = 0U, hi = IMPORT_COUNT;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2U;
        if (s_imports[mid].slot == slot)
            return (int)mid;
        if (s_imports[mid].slot < slot)
            lo = mid + 1U;
        else
            hi = mid;
    }
    return -1;
}

const char *guest_import_name(uint32_t slot_addr)
{
    int index = import_index_of_slot(slot_addr - (uint32_t)GUEST_IMAGE_BASE);
    if (index < 0)
        return NULL;
    if (s_bad_name_slot && slot_addr == VA(s_bad_name_slot))
        return "Lua5.3.3r.dll!lua_bogus";
    return s_imports[index].name;
}

static int bogus_endpoint(CPU *__restrict c, uint32_t index, unsigned *count)
{
    (void)index;
    (void)count;
    guest_fault(c, 0U, "oracle bogus endpoint reached");
    return 1;
}

/* host_vita_import_id.c:478 family_indexed_fn(ISAAC_VITA_IMPORT_LUA) and
 * :29 ISAAC_VITA_LUA_FAST_BINDING: a Lua row binds the generic indexed
 * endpoint, replaced by the typed one when the fast table bound it. */
int guest_host_import_id_direct_binding(uint32_t import_id,
                                        guest_import_family_fn *fn,
                                        uint32_t *local_index)
{
    const char *name;
    uint32_t j;
    if (import_id >= IMPORT_COUNT)
        return 0;
    name = s_imports[import_id].name;
    if (strncmp(name, "Lua5.3.3r.dll!", 14U) != 0)
        return 0;
    for (j = 0U; j < ISAAC_VITA_LUA_IMPORT_COUNT; ++j) {
        const char *local = isaac_vita_lua_import_name(j);
        if (!local || strcmp(local, name) != 0)
            continue;
        *fn = isaac_vita_lua_import_indexed;
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
        if (isaac_vita_lua_import_fast_binding(j))
            *fn = isaac_vita_lua_import_fast_binding(j);
#endif
        if (import_id == s_bad_binding_id)
            *fn = bogus_endpoint;
        *local_index = j;
        return 1;
    }
    return 0;
}

static void build_direct_table(void)
{
    uint32_t i;
    memset(s_direct, 0, sizeof s_direct);
    for (i = 0U; i < IMPORT_COUNT; ++i) {
        guest_import_family_fn fn = NULL;
        uint32_t local = 0U;
        CHECK(i == 0U || s_imports[i].slot > s_imports[i - 1U].slot);
        if (!guest_host_import_id_direct_binding(i, &fn, &local) || !fn)
            continue;
        s_direct[i].fn = fn;
        s_direct[i].local_index = local;
        s_direct[i].slot_va = VA(s_imports[i].slot);
    }
}

static void fake_dispatch(CPU *__restrict c, uint32_t address);
static void body_dispatch(CPU *__restrict c, uint32_t address);
static int is_body(uint32_t address);

/* guest.c:2757 guest_import_call without its diagnostics (phase-profile
 * notes, import coverage, sampler word). */
void guest_import_call(CPU *__restrict c, uint32_t target, uint32_t import_id)
{
    const guest_import_direct *entry;
    if (import_id >= GUEST_IMPORT_DIRECT_CAPACITY) {
        guest_call(c, target);
        return;
    }
    entry = &s_direct[import_id];
    if (target != entry->slot_va || !entry->fn) {
        guest_call(c, target);
        return;
    }
    if (!entry->fn(c, entry->local_index, &s_census))
        guest_fault(c, target, "oracle direct import rejected its local index");
}

#define FAKE_BASE 0x97f00000U
#define FAKE_END  0x97f02000U

/* guest_call: an IAT tag word reaches its bound endpoint (guest_call's import
 * branch), a replayed body's VA its translated body or its wrap (the arm),
 * a fake member / finalizer its C model. */
void guest_call(CPU *__restrict c, uint32_t address)
{
    int index;
    if (address - FAKE_BASE < FAKE_END - FAKE_BASE) {
        fake_dispatch(c, address);
        return;
    }
    if (is_body(address)) {
        body_dispatch(c, address);
        return;
    }
    index = import_index_of_slot(address - (uint32_t)GUEST_IMAGE_BASE);
    if (index >= 0 && s_direct[index].fn) {
        if (!s_direct[index].fn(c, s_direct[index].local_index, &s_census))
            guest_fault(c, address, "oracle import rejected its local index");
        return;
    }
    guest_fault(c, address, "oracle guest_call target is not registered");
}

/* ---- the bodies ------------------------------------------------------------ */

#define ROW_DECL(sym, rva, cb, key, helper, cov) \
    void lbr_t_##sym(CPU *__restrict c); \
    void __real_##sym(CPU *__restrict c); \
    void __wrap_##sym(CPU *__restrict c);
ISAAC_LBR_SCALAR_ROWS(ROW_DECL)
#undef ROW_DECL
void lbr_t_sub_00446d90(CPU *__restrict c);
void lbr_t_sub_00433870(CPU *__restrict c);
void lbr_t_sub_003f90b0(CPU *__restrict c);
void __real_sub_00446d90(CPU *__restrict c);
void __real_sub_00433870(CPU *__restrict c);
void __real_sub_003f90b0(CPU *__restrict c);
void __wrap_sub_00446d90(CPU *__restrict c);
void __wrap_sub_00433870(CPU *__restrict c);
void __wrap_sub_003f90b0(CPU *__restrict c);
/* translated getClass / getExact (lbr_oracle_bodies.c) */
void __real_sub_003f8e30(CPU *__restrict c);
void __real_sub_003f8c80(CPU *__restrict c);

#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_SCALAR
#define W_SCALAR(sym) __wrap_##sym
#else
#define W_SCALAR(sym) NULL
#endif
#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_GETDATA
#define W_GETDATA __wrap_sub_00446d90
#else
#define W_GETDATA NULL
#endif
#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_BOOLII
#define W_BOOLII __wrap_sub_00433870
#else
#define W_BOOLII NULL
#endif
#if ISAAC_VITA_LUA_BINDING_REPLAY_FAMILY_MASK & ISAAC_LBR_FAMILY_BIT_INDEX2
#define W_INDEX2 __wrap_sub_003f90b0
#else
#define W_INDEX2 NULL
#endif

typedef void (*body_fn)(CPU *__restrict);
typedef struct body {
    uint32_t rva;
    unsigned family;
    uint32_t cb, key, helper;
    body_fn translated;
    body_fn wrap;
} body;

#define ROW_ENTRY(sym, rva, cb, key, helper, cov) \
    { rva, ISAAC_LBR_FAMILY_SCALAR, cb, key, helper, lbr_t_##sym, W_SCALAR(sym) },
static const body s_bodies[] = {
    ISAAC_LBR_SCALAR_ROWS(ROW_ENTRY)
    { ISAAC_LBR_GETDATA_RVA, ISAAC_LBR_FAMILY_GETDATA, 0U, ENTITY_KEY, 0U,
      lbr_t_sub_00446d90, W_GETDATA },
    { ISAAC_LBR_BOOLII_RVA, ISAAC_LBR_FAMILY_BOOLII, 0U, 0U, 0U,
      lbr_t_sub_00433870, W_BOOLII },
    { ISAAC_LBR_INDEX2_RVA, ISAAC_LBR_FAMILY_INDEX2, 0U, 0U, 0U,
      lbr_t_sub_003f90b0, W_INDEX2 },
};
#undef ROW_ENTRY
#define BODY_COUNT ((uint32_t)(sizeof s_bodies / sizeof s_bodies[0]))
#define BODY_GETDATA (ISAAC_LBR_SCALAR_COUNT)
#define BODY_BOOLII  (ISAAC_LBR_SCALAR_COUNT + 1U)
#define BODY_INDEX2  (ISAAC_LBR_SCALAR_COUNT + 2U)

static const body *body_of(uint32_t address)
{
    uint32_t i;
    for (i = 0U; i < BODY_COUNT; ++i)
        if (VA(s_bodies[i].rva) == address)
            return &s_bodies[i];
    return NULL;
}

static int is_body(uint32_t address)
{
    return body_of(address) != NULL;
}

/* getClass for every caller: the native seam's wrap (production: the link's
 * --wrap=sub_003f8e30) or the translated body (the knob off). */
static int s_native_getclass = 1;
void sub_003f8e30(CPU *__restrict c)
{
    if (s_native_getclass)
        __wrap_sub_003f8e30(c);
    else
        __real_sub_003f8e30(c);
}

/* ---- event log (compared between the arms) -------------------------------- */

/* Only arm-independent events: a body entered from guest_call, a fake
 * member / finalizer / getter called with its arguments.  __real_ entries
 * exist in the replay arm only and are counted, not logged. */
enum {
    EV_BODY_ENTER = 1, EV_BODY_EXIT, EV_MEMBER, EV_GETDATA, EV_BOOLII,
    EV_GC, EV_GETTER
};
typedef struct event {
    uint32_t kind, a, b, d;
} event;
#define EVENT_CAP (1U << 18)
static event s_events[EVENT_CAP];
static event s_events_t[EVENT_CAP];
static uint32_t s_event_count, s_event_count_t, s_event_lost;

static void ev(uint32_t kind, uint32_t a, uint32_t b, uint32_t d)
{
    if (s_event_count < EVENT_CAP) {
        s_events[s_event_count].kind = kind;
        s_events[s_event_count].a = a;
        s_events[s_event_count].b = b;
        s_events[s_event_count].d = d;
        ++s_event_count;
    } else {
        ++s_event_lost;
    }
}

/* ---- arms, wrap entry contract, __real_ shims ----------------------------- */

enum { ARM_TRANSLATED = 0, ARM_REPLAY = 1 };
static int s_arm = ARM_TRANSLATED;
static lua_State *s_L;

/* luai_makeseed() of the oracle's Lua build (test_vita_lua_binding_replay.py
 * SEED_HEADER); argv[3]. */
static unsigned int s_lua_seed = 0x15aac5eeU;
unsigned int lbr_oracle_lua_seed(void);
unsigned int lbr_oracle_lua_seed(void) { return s_lua_seed; }
static uint32_t s_state_word;

typedef struct wrap_entry {
    uint32_t rva;
    uint32_t reg[8];
    uint32_t low_water;
    int top;
    uint8_t window[ISAAC_LBR_WINDOW_BELOW + 8U];
    /* fixture probes (not part of the entry contract): L->nCcalls at the
     * wrap entry (live_wrap), whether __real_ ran for it, and the finalizer
     * runs counted while its mirror ran (before any __real_) */
    unsigned short ccalls;
    int real_entered;
    uint32_t gc_mirror;
} wrap_entry;
static wrap_entry s_wraps[128];
static int s_wrap_depth;
static int s_real_stub;
static uint32_t s_real_calls, s_contract_failures;
/* calls whose __real_ ran after a mirror run that ran >= 1 finalizer (under
 * VERIFY: the verified calls with GC inside the mirror), and those runs */
static uint32_t s_mirror_gc_calls, s_mirror_gc_runs;
static int tap_live(lua_State *L);
static uint32_t s_wraps_in_tap;     /* replayed bodies entered under a live tap */

static void regs_get(const CPU *c, uint32_t reg[8])
{
    reg[0] = c->eax; reg[1] = c->ecx; reg[2] = c->edx; reg[3] = c->ebx;
    reg[4] = c->esp; reg[5] = c->ebp; reg[6] = c->esi; reg[7] = c->edi;
}

static void wrap_push(const CPU *c, uint32_t rva)
{
    wrap_entry *w;
    /* A Lua raise or a guest escape leaves a wrap without returning through
     * call_wrap.  A live enclosing activation entered at a strictly higher
     * ESP than anything nested in it, so entries at or below this ESP are
     * abandoned ones. */
    while (s_wrap_depth > 0 && s_wraps[s_wrap_depth - 1].reg[4] <= c->esp)
        --s_wrap_depth;
    CHECK(s_wrap_depth < (int)(sizeof s_wraps / sizeof s_wraps[0]));
    w = &s_wraps[s_wrap_depth++];
    w->rva = rva;
    regs_get(c, w->reg);
    w->low_water = c->stack_low_water;
    w->top = lua_gettop((lua_State *)(uintptr_t)ld32(c->esp + 4U));
    memcpy(w->window, (const void *)(uintptr_t)(c->esp - ISAAC_LBR_WINDOW_BELOW),
           sizeof w->window);
    w->ccalls = ((lua_State *)(uintptr_t)ld32(c->esp + 4U))->nCcalls;
    w->real_entered = 0;
    w->gc_mirror = 0U;
    if (tap_live((lua_State *)(uintptr_t)ld32(c->esp + 4U)))
        ++s_wraps_in_tap;
}

/* The innermost wrap whose body is still running: L->nCcalls counts nested
 * C calls (Lua 5.3.3 ldo.c:493 luaD_call ++), so everything that runs inside
 * a live wrap runs at a strictly higher count (a finalizer enters through
 * luaD_pcall -> dothecall -> luaD_call, lgc.c:821); a wrap a Lua raise left
 * is skipped once the raise restored the count (ldo.c:146) at or below its
 * entry count.  Read-only: wrap_push / call_wrap keep their own pruning. */
static wrap_entry *live_wrap(lua_State *L)
{
    int i;
    for (i = s_wrap_depth - 1; i >= 0; --i)
        if (s_wraps[i].ccalls < L->nCcalls)
            return &s_wraps[i];
    return NULL;
}

/* The wrap hands __real_ the entry CPU (all eight registers, the low-water
 * mark), the entry Lua top and the entry window bytes: directly on a
 * fallback, after the VERIFY rewind otherwise. */
static int lbr_oracle_real_enter(uint32_t rva, CPU *__restrict c)
{
    ++s_real_calls;
    if (s_wrap_depth > 0 && s_wraps[s_wrap_depth - 1].rva == rva) {
        wrap_entry *w = &s_wraps[s_wrap_depth - 1];
        uint32_t reg[8];
        /* a mirror ran before this __real_ only on the VERIFY rerun (a
         * fallback calls __real_ before any mirror statement) */
        if (!w->real_entered && w->gc_mirror) {
            ++s_mirror_gc_calls;
            s_mirror_gc_runs += w->gc_mirror;
        }
        w->real_entered = 1;
        regs_get(c, reg);
        if (memcmp(reg, w->reg, sizeof reg) != 0 ||
            c->stack_low_water != w->low_water ||
            lua_gettop((lua_State *)(uintptr_t)ld32(c->esp + 4U)) != w->top ||
            memcmp(w->window, (const void *)(uintptr_t)(c->esp - ISAAC_LBR_WINDOW_BELOW),
                   sizeof w->window) != 0) {
            ++s_contract_failures;
            fprintf(stderr, "__real_ entry contract broken at %08x: eax %08x/%08x "
                    "esp %08x/%08x low %08x/%08x\n", (unsigned)rva,
                    (unsigned)reg[0], (unsigned)w->reg[0], (unsigned)reg[4],
                    (unsigned)w->reg[4], (unsigned)c->stack_low_water,
                    (unsigned)w->low_water);
        }
    }
    if (s_real_stub) {
        c->eax = 0xdeadbeefU;
        c->esp += 4U;        /* every replayed body ends in a plain ret */
        return 1;
    }
    return 0;
}

#define ROW_REAL(sym, rva, cb, key, helper, cov) \
    void __real_##sym(CPU *__restrict c) \
    { \
        if (!lbr_oracle_real_enter(rva, c)) \
            lbr_t_##sym(c); \
    }
ISAAC_LBR_SCALAR_ROWS(ROW_REAL)
#undef ROW_REAL
void __real_sub_00446d90(CPU *__restrict c)
{
    if (!lbr_oracle_real_enter(ISAAC_LBR_GETDATA_RVA, c))
        lbr_t_sub_00446d90(c);
}
void __real_sub_00433870(CPU *__restrict c)
{
    if (!lbr_oracle_real_enter(ISAAC_LBR_BOOLII_RVA, c))
        lbr_t_sub_00433870(c);
}
void __real_sub_003f90b0(CPU *__restrict c)
{
    if (!lbr_oracle_real_enter(ISAAC_LBR_INDEX2_RVA, c))
        lbr_t_sub_003f90b0(c);
}

static void call_wrap(CPU *__restrict c, const body *b)
{
    int depth;
    wrap_push(c, b->rva);
    depth = s_wrap_depth;
    b->wrap(c);
    s_wrap_depth = depth - 1;
}

static void body_dispatch(CPU *__restrict c, uint32_t address)
{
    const body *b = body_of(address);
    lua_State *L = (lua_State *)(uintptr_t)ld32(c->esp + 4U);
    ev(EV_BODY_ENTER, b->rva, c->esp, (uint32_t)lua_gettop(L));
    if (s_arm == ARM_REPLAY && b->wrap)
        call_wrap(c, b);
    else
        b->translated(c);
    ev(EV_BODY_EXIT, b->rva, c->eax, c->esp);
}

/* ---- replay oracle hooks ------------------------------------------------- */

static uint32_t s_cond[7][2];
static uint32_t s_force_permille;
static uint32_t s_force_state = 0x2545f491U;
static uint32_t s_force_hits;

int isaac_lbr_oracle_force_real(uint32_t import_k)
{
    (void)import_k;
    if (!s_force_permille)
        return 0;
    s_force_state = s_force_state * 1664525U + 1013904223U;
    if ((s_force_state >> 8) % 1000U < s_force_permille) {
        ++s_force_hits;
        return 1;
    }
    return 0;
}

void isaac_lbr_oracle_cond(uint32_t id, int taken)
{
    CHECK(id < 7U);
    ++s_cond[id][taken ? 1 : 0];
}

/* The emulated lua_pushstring tap.  The test compiles the
 * replay TU -- and only that TU -- with /Dlua_pushstring=
 * lbr_oracle_tap_pushstring (test_vita_lua_binding_replay.py REPLAY_TAP_DEFS),
 * so lua.h declares this function under that name and the TU's one
 * lua_pushstring call, the emulated crossing LBR_EMU_PUSHSTRING
 * (host_vita_lua_binding_replay.c:659), lands here; the production source
 * and every Vita object stay untouched.  The tap calls the real
 * lua_pushstring and records which taps are live, so that the fixture
 * run_gc_in_emulated_pushstring can count the finalizers -- and the replayed
 * bodies -- that a GC step inside an emulated pushstring runs: the path of
 * the lbr_observed() call at :664, which no other fixture reaches.
 *
 * Liveness by L->nCcalls (Lua 5.3.3 ldo.c:493 luaD_call ++, ldo.c:146
 * luaD_rawrunprotected restores it): whatever a live tap runs is a finalizer,
 * entered through luaD_pcall -> dothecall -> luaD_call (lgc.c:821), at a
 * strictly higher count; a raise out of the pushstring lands in a protected
 * call entered before it, at a count <= the tap's.  tap_live prunes entries
 * whose count is >= the current one (abandoned), then answers whether any
 * tap is still live.  The pushstring itself never raises on its own here
 * (no ERRMEM in the arena), so an abandoned tap always means a raising
 * finalizer. */
#define TAP_CAP 64
static unsigned short s_tap_ccalls[TAP_CAP];
static int s_tap_depth;
static uint32_t s_tap_calls;

static int tap_live(lua_State *L)
{
    while (s_tap_depth > 0 && s_tap_ccalls[s_tap_depth - 1] >= L->nCcalls)
        --s_tap_depth;
    return s_tap_depth > 0;
}

const char *lbr_oracle_tap_pushstring(lua_State *L, const char *s);
const char *lbr_oracle_tap_pushstring(lua_State *L, const char *s)
{
    const char *r;
    int depth;
    /* only the replayed arm runs the replay TU's mirrors */
    CHECK(s_arm == ARM_REPLAY);
    (void)tap_live(L);
    CHECK(s_tap_depth < TAP_CAP);
    s_tap_ccalls[s_tap_depth++] = L->nCcalls;
    depth = s_tap_depth;
    ++s_tap_calls;
    r = lua_pushstring(L, s);
    /* a normal return: every tap nested in a finalizer returned as well (a
     * raise out of a finalizer also leaves this pushstring, lgc.c:832) */
    CHECK(s_tap_depth == depth && s_tap_ccalls[depth - 1] == L->nCcalls);
    --s_tap_depth;
    return r;
}

/* ---- guest stack, CPU ---------------------------------------------------- */

#define STACK_WORDS 16384U
#define GUARD_WORDS (CALLBACK_FRAME_GUARD / 4U)
_Alignas(16) static uint32_t s_stack_area[GUARD_WORDS + STACK_WORDS];
static CPU s_cpu;
static CPU s_other;      /* a foreign CPU for the hostile owner shapes */

static void reset_cpu(CPU *c, uint32_t headroom)
{
    memset(c, 0, sizeof *c);
    c->stack_owner = c;
    c->stack_floor = (uint32_t)(uintptr_t)(s_stack_area + GUARD_WORDS);
    c->stack_ceiling = c->stack_floor + STACK_WORDS * 4U;
    c->stack_low_water = c->stack_ceiling;
    c->esp = c->stack_ceiling - headroom;
    memset(s_stack_area, 0xcd, sizeof s_stack_area);
    c->eax = 0xa5a5a5a5U;
    c->ebx = 0x0b0b0b0bU;
    c->ebp = 0x0e0e0e0eU;
    c->esi = 0x51515151U;
    c->edi = 0xd1d1d1d1U;
}

/* Lua local import indices, resolved by name. */
static uint32_t s_local_pcallk, s_local_pushclosure, s_local_newstate_aux,
    s_local_openlibs, s_local_close;

static const char *s_phase = "startup";   /* named in a panic */

static int oracle_panic(lua_State *L)
{
    const char *message = lua_tostring(L, -1);
    void *frames[48];
    USHORT n, i;
    fprintf(stderr, "oracle Lua panic during %s: %s\n", s_phase, message ? message : "(none)");
    /* raw return addresses; the oracle links at a fixed base (/DYNAMICBASE:NO),
     * so `link /MAP` resolves them */
    n = CaptureStackBackTrace(0, 48, frames, NULL);
    for (i = 0; i < n; ++i)
        fprintf(stderr, "  panic frame %2u: %08x\n", (unsigned)i, (unsigned)(uintptr_t)frames[i]);
    {
        /* the Lua call chain: which C functions are active (link /MAP names them) */
        CallInfo *ci;
        fprintf(stderr, "  gcstate %u nny %u nCcalls %u\n", (unsigned)G(L)->gcstate,
                (unsigned)L->nny, (unsigned)L->nCcalls);
        for (ci = L->ci; ci; ci = ci->previous) {
            const TValue *f = ci->func;
            if (ttislcf(f))
                fprintf(stderr, "  ci light C %08x\n", (unsigned)(uintptr_t)fvalue(f));
            else if (ttisCclosure(f))
                fprintf(stderr, "  ci C closure %08x upvalue1 %08x\n",
                        (unsigned)(uintptr_t)clCvalue(f)->f,
                        clCvalue(f)->nupvalues && ttislightuserdata(&clCvalue(f)->upvalue[0])
                            ? (unsigned)(uintptr_t)pvalue(&clCvalue(f)->upvalue[0]) : 0U);
            else if (ttisLclosure(f))
                fprintf(stderr, "  ci Lua line %d\n", clLvalue(f)->p->linedefined);
            else
                fprintf(stderr, "  ci base\n");
        }
    }
    return 0;
}

static uint32_t local_of(const char *name)
{
    uint32_t j;
    for (j = 0U; j < ISAAC_VITA_LUA_IMPORT_COUNT; ++j)
        if (strcmp(isaac_vita_lua_import_name(j), name) == 0)
            return j;
    fprintf(stderr, "no Lua import named %s\n", name);
    exit(1);
}

static int invoke_import(CPU *c, uint32_t index, const uint32_t *arguments,
                         uint32_t count)
{
    uint32_t saved = c->esp, i;
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, IMPORT_RETURN + index);
    if (!isaac_vita_lua_import_indexed(c, index, NULL))
        return 0;
    if (c->fault || c->esp != saved - count * 4U)
        return 0;
    return guest_stack_adjust(c, count * 4U, index);
}

/* A guest-style import crossing from a fake guest function: the arguments and
 * the return word on the guest stack, guest_call on the slot's tag word. */
static void call_guest_import(CPU *c, const char *name, const uint32_t *arguments,
                         uint32_t count)
{
    uint32_t i, slot = 0U;
    for (i = 0U; i < IMPORT_COUNT; ++i)
        if (strcmp(s_imports[i].name, name) == 0)
            slot = s_imports[i].slot;
    CHECK(slot != 0U);
    for (i = count; i != 0U; --i)
        gpush(c, arguments[i - 1U]);
    gpush(c, 0x00421100U);
    guest_call(c, VA(slot));
    (void)guest_stack_adjust(c, count * 4U, 0U);
}

/* ---- fake guest code ------------------------------------------------------- */
/* Object layout (full userdata, 64 bytes): +0 vtable word, +4 object pointer
 * (= ud + 16); object +0 flags, +4 class index, +8 value seed, +12 data ref
 * (GetData), +16 serial. */
#define OBJ_OWNER_CLEAR   0x1U
#define OBJ_OWNER_FOREIGN 0x2U
#define OBJ_STACK_REBIND  0x4U
#define OBJ_IMPORTS       0x8U
#define USERDATA_SIZE 64U

#define FAKE_MEMBER(k)    (FAKE_BASE + 0x10U * (uint32_t)(k))   /* k < 64 */
#define FAKE_GETDATA      (FAKE_BASE + 0x1000U)
#define FAKE_BOOLII       (FAKE_BASE + 0x1010U)
#define FAKE_GC_PLAIN     (FAKE_BASE + 0x1020U)
#define FAKE_GC_RAISE     (FAKE_BASE + 0x1030U)
#define FAKE_GC_IMPORTS   (FAKE_BASE + 0x1040U)
#define FAKE_GETTER_C     (FAKE_BASE + 0x1050U)
#define FAKE_GETTER_RAISE (FAKE_BASE + 0x1060U)
#define FAKE_GC_OWNER     (FAKE_BASE + 0x1070U)

static const char s_text_gc[] = "gc-import";
static const char s_text_fmt[] = "%s";
static const char s_text_boom[] = "boom";
static const char s_text_getter_boom[] = "getter boom";

/* What hostile guest code between two crossings can do to the replay's
 * exactness conditions. */
static void hostile(CPU *c, uint32_t flags)
{
    int root = 0;
    if (flags & OBJ_IMPORTS) {
        uint32_t push[2], settop[2];
        push[0] = s_state_word;
        push[1] = (uint32_t)(uintptr_t)s_text_gc;
        settop[0] = s_state_word;
        settop[1] = 0xfffffffeU;
        call_guest_import(c, "Lua5.3.3r.dll!lua_pushstring", push, 2U);
        call_guest_import(c, "Lua5.3.3r.dll!lua_settop", settop, 2U);
    }
    if (flags & OBJ_OWNER_CLEAR)
        isaac_vita_lua_seam_leave(c, 1);          /* word c -> 0 */
    if (flags & OBJ_OWNER_FOREIGN) {
        isaac_vita_lua_seam_leave(c, 1);
        CHECK(isaac_vita_lua_seam_enter(&s_other, &root) && root == 1);
    }
    if (flags & OBJ_STACK_REBIND)
        c->stack_floor = c->esp - 0x40U;
}

/* thiscall `int T::get() const`: ecx = object (0 when the getter had no
 * argument), plain ret; the caller-saved registers come back clobbered. */
static void fake_member(CPU *c, uint32_t k)
{
    uint32_t object = c->ecx, value;
    ev(EV_MEMBER, k, object, c->esp);
    if (object) {
        value = (ld32(object + 8U) * 0x9e3779b1U) ^ (k * 0x01000193U);
        hostile(c, ld32(object));
    } else {
        value = 0x80000000U | k;
    }
    c->eax = value;
    c->ecx = 0xc1c1c100U ^ k;
    c->edx = 0xd2d2d200U ^ value;
    (void)gpop(c);
}

/* thiscall `LuaRef Entity::GetData()`: sret at [esp+4], ret 4.  A fresh
 * registry reference to the entity's data table (or LUA_REFNIL), which the
 * translated caller copies and releases (guest_0131.c sub_00446d90). */
static void fake_getdata(CPU *c)
{
    uint32_t object = c->ecx, sret = ld32(c->esp + 4U), ref = (uint32_t)LUA_REFNIL;
    ev(EV_GETDATA, object, sret, c->esp);
    if (object && (int32_t)ld32(object + 12U) != LUA_REFNIL) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, (lua_Integer)(int32_t)ld32(object + 12U));
        ref = (uint32_t)luaL_ref(s_L, LUA_REGISTRYINDEX);
    }
    if (object)
        hostile(c, ld32(object));
    st32(sret, s_state_word);
    st32(sret + 4U, ref);
    c->eax = sret;
    c->ecx = 0xc3c3c3c3U;
    c->edx = 0xd4d4d4d4U;
    (void)gpop(c);
    c->esp += 4U;
}

/* cdecl `bool f(int, int)`; only al is the result. */
static void fake_boolii(CPU *c)
{
    uint32_t a = ld32(c->esp + 4U), b = ld32(c->esp + 8U);
    ev(EV_BOOLII, a, b, c->esp);
    c->eax = (a << 8) | ((int32_t)a > (int32_t)b ? 1U : 0U);
    c->ecx = 0xc5c5c5c5U ^ b;
    c->edx = 0xd6d6d6d6U;
    (void)gpop(c);
}

/* finalizer runs per kind (FAKE_GC_* index: 0 plain, 1 raise, 2 imports,
 * 5 owner), in all and inside a live emulated-pushstring tap (both arms
 * count s_gc_runs; only the replayed arm has taps) */
#define GC_KIND(address) ((((address) - FAKE_GC_PLAIN) / 0x10U) & 7U)
#define GC_PLAIN 0U
#define GC_RAISE 1U
#define GC_IMPORTS 2U
#define GC_OWNER 5U
static uint32_t s_gc_runs[8], s_gc_in_tap[8];
static uint32_t s_gc_in_mirror;     /* runs while a replayed body's mirror ran */

static void fake_gc(CPU *c, uint32_t address)
{
    uint32_t L = ld32(c->esp + 4U);
    wrap_entry *w;
    ++s_gc_runs[GC_KIND(address)];
    if (tap_live((lua_State *)(uintptr_t)L))
        ++s_gc_in_tap[GC_KIND(address)];
    w = live_wrap((lua_State *)(uintptr_t)L);
    if (w && !w->real_entered) {
        ++w->gc_mirror;
        ++s_gc_in_mirror;
    }
    ev(EV_GC, address, c->esp, (uint32_t)lua_gettop((lua_State *)(uintptr_t)L));
    if (address == FAKE_GC_RAISE) {
        uint32_t error[3];
        error[0] = L;
        error[1] = (uint32_t)(uintptr_t)s_text_fmt;
        error[2] = (uint32_t)(uintptr_t)s_text_boom;
        call_guest_import(c, "Lua5.3.3r.dll!luaL_error", error, 3U);
        guest_fault(c, 0U, "luaL_error import returned to the __gc closure");
        return;
    }
    if (address == FAKE_GC_IMPORTS)
        hostile(c, OBJ_IMPORTS);
    if (address == FAKE_GC_OWNER)
        hostile(c, OBJ_OWNER_CLEAR);
    c->eax = 0U;
    (void)gpop(c);
}

static void fake_getter(CPU *c, uint32_t address)
{
    uint32_t L = ld32(c->esp + 4U);
    ev(EV_GETTER, address, c->esp, (uint32_t)lua_gettop((lua_State *)(uintptr_t)L));
    if (address == FAKE_GETTER_RAISE) {
        uint32_t error[3];
        error[0] = L;
        error[1] = (uint32_t)(uintptr_t)s_text_fmt;
        error[2] = (uint32_t)(uintptr_t)s_text_getter_boom;
        call_guest_import(c, "Lua5.3.3r.dll!luaL_error", error, 3U);
        guest_fault(c, 0U, "luaL_error import returned to the getter");
        return;
    }
    lua_pushinteger((lua_State *)(uintptr_t)L, 4242 + lua_gettop((lua_State *)(uintptr_t)L));
    c->eax = 1U;
    (void)gpop(c);
}

static void fake_dispatch(CPU *__restrict c, uint32_t address)
{
    if (address < FAKE_GETDATA) {
        CHECK((address & 0xfU) == 0U);
        fake_member(c, (address - FAKE_BASE) / 0x10U);
    } else if (address == FAKE_GETDATA) {
        fake_getdata(c);
    } else if (address == FAKE_BOOLII) {
        fake_boolii(c);
    } else if (address == FAKE_GC_PLAIN || address == FAKE_GC_RAISE ||
               address == FAKE_GC_IMPORTS || address == FAKE_GC_OWNER) {
        fake_gc(c, address);
    } else if (address == FAKE_GETTER_C || address == FAKE_GETTER_RAISE) {
        fake_getter(c, address);
    } else {
        guest_fault(c, address, "oracle fake target is not registered");
    }
}

/* ---- the world: snapshot, restore, compare ---------------------------------- */

typedef struct world {
    uint8_t *arena;
    uint32_t arena_bytes;
    uint8_t stack[sizeof s_stack_area];
    CPU cpu;
    unsigned char cov[COVERAGE_BYTES];
    uint32_t entries;
    int owner;
    uint32_t fault_calls;
    const char *first_fault;
    unsigned census;
    uint32_t status;
    char result[4096];
} world;

static world s_w0, s_wt, s_wr;

/* 0: no owner, 1: this CPU, 2: another CPU.  Probing leaves the word as found. */
static int owner_state(void)
{
    int root = 0;
    if (isaac_vita_lua_seam_enter(&s_cpu, &root)) {
        if (root) {
            isaac_vita_lua_seam_leave(&s_cpu, root);
            return 0;
        }
        return 1;
    }
    return 2;
}

static void owner_reset(void)
{
    int state = owner_state();
    if (state == 1)
        isaac_vita_lua_seam_leave(&s_cpu, 1);
    else if (state == 2)
        isaac_vita_lua_seam_leave(&s_other, 1);
    CHECK(owner_state() == 0);
}

/* ---- the bytes no Lua code reads ----------------------------------------------
 * On this 32-bit build a TValue is { Value value_ (8 bytes); int tt_ } padded
 * to 16, and only numbers use all of value_.  The rest of value_ and the
 * padding keep whatever the slot held, and Lua 5.3.3 itself copies C-stack
 * TValues into the heap: GCTM (lgc.c:809-819) builds `TValue v` on the C
 * stack and setobj2s's it into the Lua stack as the finalizer argument.  A
 * finalizer run from a GC step inside a crossing therefore leaves C-stack
 * garbage that depends on which C code (translated endpoint path or replay)
 * sat below the step.  Before each snapshot those bytes are zeroed in place,
 * in every TValue of every live object; Lua never reads them, so the program
 * is unchanged and the T/R arena comparison stays byte-exact for every byte
 * that has a meaning (types, payloads, chains, string bytes, sizes). */
static int in_arena(const void *p, size_t bytes)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    return a >= ARENA_BASE && a - ARENA_BASE <= ARENA_SIZE - (uint32_t)bytes;
}

static void canon_value_hi(Value *v, int tt)
{
    tt &= 0x3f;                       /* variant tag without the collectable bit */
    if (!(tt == LUA_TNUMINT || tt == LUA_TNUMFLT))
        memset((unsigned char *)v + 4, 0, sizeof(Value) - 4U);
}

static void canon_tv(TValue *o)
{
    canon_value_hi(&o->value_, rttype(o));
    memset((unsigned char *)o + offsetof(TValue, tt_) + sizeof(int), 0,
           sizeof(TValue) - offsetof(TValue, tt_) - sizeof(int));
}

static void canon_tvs(TValue *o, size_t n)
{
    size_t i;
    if (!n || !in_arena(o, n * sizeof(TValue)))
        return;
    for (i = 0; i < n; ++i)
        canon_tv(&o[i]);
}

static void canon_object(GCObject *o)
{
    switch (o->tt) {
    case LUA_TTABLE: {
        Table *h = gco2t(o);
        canon_tvs(h->array, h->sizearray);
        if (h->lastfree != NULL && in_arena(h->node, (size_t)sizenode(h) * sizeof(Node))) {
            int i;
            for (i = 0; i < sizenode(h); ++i) {
                Node *n = &h->node[i];
                canon_tv(&n->i_val);
                canon_value_hi(&n->i_key.nk.value_, n->i_key.nk.tt_);   /* `next` kept */
            }
        }
        break;
    }
    case LUA_TTHREAD: {
        lua_State *th = gco2th(o);
        canon_tvs(th->stack, (size_t)th->stacksize);
        break;
    }
    case LUA_TUSERDATA:
        canon_value_hi(&gco2u(o)->user_, gco2u(o)->ttuv_);
        break;
    case LUA_TCCL: {
        CClosure *f = gco2ccl(o);
        canon_tvs(f->upvalue, f->nupvalues);
        break;
    }
    case LUA_TLCL: {
        LClosure *f = gco2lcl(o);
        int i;
        for (i = 0; i < f->nupvalues; ++i) {
            UpVal *uv = f->upvals[i];
            if (uv && in_arena(uv, sizeof *uv) && !upisopen(uv))
                canon_tv(&uv->u.value);
        }
        break;
    }
    case LUA_TPROTO: {
        Proto *p = gco2p(o);
        canon_tvs(p->k, (size_t)p->sizek);
        break;
    }
    default:
        break;
    }
}

static void canon_lua_heap(void)
{
    global_State *g;
    GCObject *lists[4];
    GCObject *o;
    int k;
    if (!s_L)
        return;
    g = G(s_L);
    canon_tv(&g->l_registry);
    canon_tvs(g->mainthread->stack, (size_t)g->mainthread->stacksize);
    lists[0] = g->allgc;
    lists[1] = g->finobj;
    lists[2] = g->tobefnz;
    lists[3] = g->fixedgc;
    for (k = 0; k < 4; ++k)
        for (o = lists[k]; o; o = o->next)
            canon_object(o);
}

static void world_take(world *w)
{
    canon_lua_heap();
    w->arena_bytes = ARENA->bump;
    memcpy(w->arena, (const void *)(uintptr_t)ARENA_BASE, w->arena_bytes);
    memcpy(w->stack, s_stack_area, sizeof s_stack_area);
    memcpy(&w->cpu, &s_cpu, sizeof s_cpu);
    memcpy(w->cov, s_cov_functions, sizeof s_cov_functions);
    w->entries = ENTRIES();
    w->owner = owner_state();
    w->fault_calls = s_fault_calls;
    w->first_fault = s_first_fault;
    w->census = s_census;
}

static void world_restore(const world *w0, uint32_t dirty_bytes)
{
    memcpy((void *)(uintptr_t)ARENA_BASE, w0->arena, w0->arena_bytes);
    if (dirty_bytes > w0->arena_bytes)
        memset((void *)(uintptr_t)(ARENA_BASE + w0->arena_bytes), 0,
               dirty_bytes - w0->arena_bytes);
    memcpy(s_stack_area, w0->stack, sizeof s_stack_area);
    memcpy(&s_cpu, &w0->cpu, sizeof s_cpu);
    memcpy(s_cov_functions, w0->cov, sizeof s_cov_functions);
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    g_guest_function_entries = w0->entries;
#endif
    owner_reset();
    CHECK(w0->owner == 0);
    s_fault_calls = w0->fault_calls;
    s_first_fault = w0->first_fault;
    s_census = w0->census;
}

static uint32_t first_difference(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0U; i < n; ++i)
        if (a[i] != b[i])
            return i;
    return n;
}

/* Name the arena block a difference falls in (walk the bump-ordered block
 * headers of the T copy) and dump both arms' words around it. */
static void explain_arena_difference(const world *t, const world *r, uint32_t d)
{
    uint32_t block = (uint32_t)((sizeof(arena_header) + 15U) & ~15U), i, lo;
    const uint32_t *tw = (const uint32_t *)(const void *)t->arena;
    const uint32_t *rw = (const uint32_t *)(const void *)r->arena;
    while (block + 8U <= t->arena_bytes) {
        uint32_t cap = tw[block / 4U];
        if (cap == 0U || block + 8U + cap > t->arena_bytes)
            break;
        if (d < block + 8U + cap) {
            fprintf(stderr, "  in block %08x cap %u tag T %08x R %08x, payload +%u%s%s\n",
                    (unsigned)(ARENA_BASE + block + 8U), (unsigned)cap,
                    (unsigned)tw[block / 4U + 1U], (unsigned)rw[block / 4U + 1U],
                    (unsigned)(d >= block + 8U ? d - block - 8U : 0U),
                    s_L && (uint32_t)(uintptr_t)s_L->stack == ARENA_BASE + block + 8U
                        ? " (the main thread's Lua stack)" : "",
                    s_L && (uint32_t)(uintptr_t)s_L >= ARENA_BASE + block + 8U &&
                    (uint32_t)(uintptr_t)s_L < ARENA_BASE + block + 8U + cap
                        ? " (lua_State + global_State)" : "");
            break;
        }
        block += 8U + cap;
    }
    if (s_L) {
        /* which live object (of the replayed world) owns the address */
        static const char *const lists[3] = { "allgc", "finobj", "tobefnz" };
        global_State *g = G(s_L);
        GCObject *heads[3];
        uint32_t va = ARENA_BASE + d, k;
        heads[0] = g->allgc;
        heads[1] = g->finobj;
        heads[2] = g->tobefnz;
        for (k = 0U; k < 3U; ++k) {
            GCObject *o;
            for (o = heads[k]; o; o = o->next) {
                uint32_t base = 0U, size = 0U;
                const char *what = NULL;
                if (o->tt == LUA_TTABLE) {
                    Table *h = gco2t(o);
                    if (va - (uint32_t)(uintptr_t)h->array < h->sizearray * (uint32_t)sizeof(TValue)) {
                        base = (uint32_t)(uintptr_t)h->array;
                        size = h->sizearray * (uint32_t)sizeof(TValue);
                        what = "table array part";
                    } else if (va - (uint32_t)(uintptr_t)h->node <
                               (uint32_t)sizenode(h) * (uint32_t)sizeof(Node)) {
                        base = (uint32_t)(uintptr_t)h->node;
                        size = (uint32_t)sizenode(h) * (uint32_t)sizeof(Node);
                        what = "table node part";
                    }
                } else if (o->tt == LUA_TTHREAD) {
                    lua_State *th = gco2th(o);
                    if (va - (uint32_t)(uintptr_t)th->stack <
                        (uint32_t)th->stacksize * (uint32_t)sizeof(TValue)) {
                        base = (uint32_t)(uintptr_t)th->stack;
                        size = (uint32_t)th->stacksize * (uint32_t)sizeof(TValue);
                        what = th == s_L ? "main thread stack" : "coroutine stack";
                    }
                }
                if (what) {
                    fprintf(stderr, "  owner: %s (%s) base %08x size %u offset %u "
                            "(element %u, byte %u)%s\n", what, lists[k],
                            (unsigned)base, (unsigned)size, (unsigned)(va - base),
                            (unsigned)((va - base) / (o->tt == LUA_TTABLE && size &&
                                       what[6] == 'n' ? sizeof(Node) : sizeof(TValue))),
                            (unsigned)((va - base) % (o->tt == LUA_TTABLE &&
                                       what[6] == 'n' ? sizeof(Node) : sizeof(TValue))),
                            o->tt == LUA_TTHREAD &&
                            va >= (uint32_t)(uintptr_t)gco2th(o)->top ? " above top" : "");
                    k = 3U;
                    break;
                }
            }
        }
        fprintf(stderr, "  main stack %08x top %08x\n",
                (unsigned)(uintptr_t)s_L->stack, (unsigned)(uintptr_t)s_L->top);
    }
    lo = (d & ~3U) >= 32U ? (d & ~3U) - 32U : 0U;
    for (i = lo; i < lo + 96U && i + 4U <= t->arena_bytes; i += 4U)
        fprintf(stderr, "  %08x: T %08x R %08x%s\n", (unsigned)(ARENA_BASE + i),
                (unsigned)tw[i / 4U], (unsigned)rw[i / 4U],
                tw[i / 4U] != rw[i / 4U] ? "  <-" : "");
}

static int worlds_equal(const char *name, const world *t, const world *r,
                        int full)
{
    int ok = 1;
    uint32_t d;
    if (t->status != r->status || strcmp(t->result, r->result) != 0) {
        fprintf(stderr, "[%s] result: T status %u \"%.300s\"\n           R status %u \"%.300s\"\n",
                name, (unsigned)t->status, t->result, (unsigned)r->status, r->result);
        ok = 0;
    }
    if (t->fault_calls != r->fault_calls || t->first_fault != r->first_fault ||
        t->owner != r->owner) {
        fprintf(stderr, "[%s] faults %u/%u (%s / %s) owner %d/%d\n", name,
                (unsigned)t->fault_calls, (unsigned)r->fault_calls,
                t->first_fault ? t->first_fault : "-",
                r->first_fault ? r->first_fault : "-", t->owner, r->owner);
        ok = 0;
    }
    if (memcmp(&t->cpu, &r->cpu, sizeof t->cpu) != 0) {
        fprintf(stderr, "[%s] CPU differs: eax %08x/%08x esp %08x/%08x low %08x/%08x fault %s/%s\n",
                name, (unsigned)t->cpu.eax, (unsigned)r->cpu.eax,
                (unsigned)t->cpu.esp, (unsigned)r->cpu.esp,
                (unsigned)t->cpu.stack_low_water, (unsigned)r->cpu.stack_low_water,
                t->cpu.fault ? t->cpu.fault : "-", r->cpu.fault ? r->cpu.fault : "-");
        ok = 0;
    }
    if (!full)
        return ok;
    if (t->arena_bytes != r->arena_bytes ||
        (d = first_difference(t->arena, r->arena, t->arena_bytes)) != t->arena_bytes) {
        d = first_difference(t->arena, r->arena,
                             t->arena_bytes < r->arena_bytes ? t->arena_bytes : r->arena_bytes);
        fprintf(stderr, "[%s] arena differs: bytes %u/%u, first difference at %08x\n",
                name, (unsigned)t->arena_bytes, (unsigned)r->arena_bytes,
                (unsigned)(ARENA_BASE + d));
        explain_arena_difference(t, r, d);
        ok = 0;
    }
    if ((d = first_difference(t->stack, r->stack, sizeof t->stack)) != sizeof t->stack) {
        fprintf(stderr, "[%s] guest stack differs at %08x\n", name,
                (unsigned)((uint32_t)(uintptr_t)s_stack_area + d));
        ok = 0;
    }
    if (memcmp(t->cov, r->cov, sizeof t->cov) != 0 || t->entries != r->entries) {
        fprintf(stderr, "[%s] coverage/entries differ (entries %u/%u)\n", name,
                (unsigned)t->entries, (unsigned)r->entries);
        ok = 0;
    }
    if (s_event_count_t != s_event_count ||
        memcmp(s_events_t, s_events, s_event_count * sizeof s_events[0]) != 0) {
        uint32_t i, n = s_event_count_t < s_event_count ? s_event_count_t : s_event_count;
        for (i = 0U; i < n; ++i)
            if (memcmp(&s_events_t[i], &s_events[i], sizeof s_events[0]) != 0)
                break;
        fprintf(stderr, "[%s] event logs differ: %u/%u events, first at %u", name,
                (unsigned)s_event_count_t, (unsigned)s_event_count, (unsigned)i);
        if (i < n)
            fprintf(stderr, ": T{%u %08x %08x %08x} R{%u %08x %08x %08x}",
                    (unsigned)s_events_t[i].kind, (unsigned)s_events_t[i].a,
                    (unsigned)s_events_t[i].b, (unsigned)s_events_t[i].d,
                    (unsigned)s_events[i].kind, (unsigned)s_events[i].a,
                    (unsigned)s_events[i].b, (unsigned)s_events[i].d);
        fprintf(stderr, "\n");
        ok = 0;
    }
    return ok;
}

/* ---- running a chunk ---------------------------------------------------------- */

static void run_loaded(uint32_t headroom, world *w)
{
    CPU *c = &s_cpu;
    uint32_t arguments[6];
    const char *text;
    reset_cpu(c, headroom);
    arguments[0] = s_state_word;
    arguments[1] = 0U;
    arguments[2] = 1U;
    arguments[3] = 0U;
    arguments[4] = 0U;
    arguments[5] = 0U;
    s_event_count = 0U;
    s_wrap_depth = 0;
    s_tap_depth = 0;
    (void)invoke_import(c, s_local_pcallk, arguments, 6U);
    w->status = c->fault ? 0xfa017U : c->eax;
    text = lua_type(s_L, -1) == LUA_TSTRING ? lua_tostring(s_L, -1) : "(not a string)";
    snprintf(w->result, sizeof w->result, "%s", text ? text : "(null)");
    lua_settop(s_L, 0);
}

typedef struct case_stats {
    isaac_vita_lua_binding_replay_stats before, after;
    uint32_t census_t, census_r;
} case_stats;

static uint32_t s_forks;

/* Fork one chunk: translated arm, restore, replay arm, compare. */
static void fork_chunk(const char *name, const char *code, uint32_t headroom,
                       case_stats *out)
{
    int root = 0, status;
    uint32_t dirty;
    case_stats local;
    case_stats *st = out ? out : &local;

    s_phase = name;
    lua_settop(s_L, 0);
    CHECK(isaac_vita_lua_seam_enter(&s_cpu, &root) && root == 1);
    status = luaL_loadstring(s_L, code);
    isaac_vita_lua_seam_leave(&s_cpu, root);
    if (status != LUA_OK) {
        fprintf(stderr, "[%s] load: %s\n", name, lua_tostring(s_L, -1));
        exit(1);
    }
    CHECK(owner_state() == 0);
    world_take(&s_w0);

    s_arm = ARM_TRANSLATED;
    run_loaded(headroom, &s_wt);
    world_take(&s_wt);
    memcpy(s_events_t, s_events, s_event_count * sizeof s_events[0]);
    s_event_count_t = s_event_count;
    dirty = ARENA->bump;

    world_restore(&s_w0, dirty);
    st->before = *isaac_vita_lua_binding_replay_stats_get();
    s_arm = ARM_REPLAY;
    run_loaded(headroom, &s_wr);
    world_take(&s_wr);
    st->after = *isaac_vita_lua_binding_replay_stats_get();
    s_arm = ARM_TRANSLATED;
    st->census_t = s_wt.census - s_w0.census;
    st->census_r = s_wr.census - s_w0.census;
    CHECK(s_event_lost == 0U);
    if (!worlds_equal(name, &s_wt, &s_wr, !ORACLE_VERIFY)) {
        fprintf(stderr, "[%s] translated and replayed worlds differ\n", name);
        exit(1);
    }
#if !ORACLE_VERIFY
    /* every crossing the replay did not make is one census event less */
    if (st->census_t - st->census_r !=
        st->after.imports_elided - st->before.imports_elided) {
        fprintf(stderr, "[%s] census %u/%u, elided %u\n", name,
                (unsigned)st->census_t, (unsigned)st->census_r,
                (unsigned)(st->after.imports_elided - st->before.imports_elided));
        exit(1);
    }
#endif
    CHECK(s_contract_failures == 0U);
    /* the state continues from the replayed arm (== the translated one) */
    s_cpu.fault = NULL;
    owner_reset();
    ++s_forks;
    s_phase = "between forks";
}

/* ---- fixture -------------------------------------------------------------------- */

static void init_object(void *ud, uint32_t klass, uint32_t flags, int32_t data_ref)
{
    uint32_t u = (uint32_t)(uintptr_t)ud, o = u + 16U;
    memset(ud, 0, USERDATA_SIZE);
    st32(u, 0x0b1e0000U | klass);
    st32(u + 4U, o);
    st32(o, flags);
    st32(o + 4U, klass);
    /* the seed derives from the address: the arena is part of the forked
     * world, a host counter would not be */
    st32(o + 8U, 0x1000U + klass * 0x101U + (u >> 4) * 7U);
    st32(o + 12U, (uint32_t)data_ref);
    st32(o + 16U, u >> 4);
}

/* newud(mt [, flags]): a game-shaped object of no class (finalizable garbage
 * and hostile objects are made from Lua). */
static int native_newud(lua_State *L)
{
    /* read the optional flags before the new userdata takes slot 2 */
    uint32_t flags = (uint32_t)luaL_optinteger(L, 2, 0);
    void *ud;
    lua_settop(L, 1);
    ud = lua_newuserdata(L, USERDATA_SIZE);
    init_object(ud, 0x7fU, flags, LUA_REFNIL);
    lua_pushvalue(L, 1);
    lua_setmetatable(L, -2);
    return 1;
}

static int native_gc_debt(lua_State *L)
{
    luaE_setdebt(G(L), 1);
    return 0;
}

static CPU *fixture_cpu(void)
{
    reset_cpu(&s_cpu, 512U);
    return &s_cpu;
}

/* a bridge closure: target VA, n guest upvalues taken from the Lua stack */
static void push_closure(uint32_t target, uint32_t upvalues)
{
    uint32_t arguments[3];
    arguments[0] = s_state_word;
    arguments[1] = target;
    arguments[2] = upvalues;
    CHECK(invoke_import(fixture_cpu(), s_local_pushclosure, arguments, 3U));
}

/* LuaBridge Userdata class tables: identity boolean, __type, __const (the
 * mutable table) or not (the const table), __parent, __index = the second
 * indexMetaMethod (0x3f90b0), __propget table; registered under the key. */
static int make_class_table(lua_State *L, const char *name, int parent_ref,
                            int is_const)
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
    push_closure(VA(ISAAC_LBR_INDEX2_RVA), 0U);
    lua_setfield(L, -2, "__index");
    lua_newtable(L);
    lua_setfield(L, -2, "__propget");
    (void)is_const;
    lua_pushvalue(L, -1);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

#define MAX_CLASSES 64
static uint32_t s_class_keys[MAX_CLASSES];
static int s_class_ref[MAX_CLASSES], s_class_const_ref[MAX_CLASSES];
static uint32_t s_class_count;
static uint32_t s_entity_class;

static uint32_t class_of_key(uint32_t key)
{
    uint32_t i;
    for (i = 0U; i < s_class_count; ++i)
        if (s_class_keys[i] == key)
            return i;
    CHECK(s_class_count < MAX_CLASSES);
    s_class_keys[s_class_count] = key;
    return s_class_count++;
}

static void set_global_table(lua_State *L, const char *name)
{
    lua_newtable(L);
    lua_setglobal(L, name);
}

static void table_set_ref(lua_State *L, const char *table, lua_Integer index, int ref)
{
    lua_getglobal(L, table);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_rawseti(L, -2, index);
    lua_pop(L, 1);
}

static void table_set_int(lua_State *L, const char *table, lua_Integer index, lua_Integer v)
{
    lua_getglobal(L, table);
    lua_pushinteger(L, v);
    lua_rawseti(L, -2, index);
    lua_pop(L, 1);
}

/* class tables, objects and the 54 bridge closures */
static void setup_fixture(void)
{
    lua_State *L = s_L;
    uint32_t i, j;
    int root = 0;
    char name[64];

    CHECK(isaac_vita_lua_seam_enter(&s_cpu, &root) && root == 1);
    lua_pushcfunction(L, native_newud);
    lua_setglobal(L, "newud");
    lua_pushcfunction(L, native_gc_debt);
    lua_setglobal(L, "gc_debt");
    set_global_table(L, "G");          /* G[i]: the getter closure of row i */
    set_global_table(L, "rowcls");     /* class index (1-based) of row i */
    set_global_table(L, "rowcb");
    set_global_table(L, "C");          /* C[j]: class table j */
    set_global_table(L, "CC");         /* CC[j]: const class table j */
    set_global_table(L, "par");        /* par[j]: parent class index or 0 */
    set_global_table(L, "objs");       /* objs[j]: mutable object of class j */
    set_global_table(L, "cobjs");      /* cobjs[j]: const object of class j */
    set_global_table(L, "ents");       /* entity objects (GetData) */
    set_global_table(L, "gcmt");       /* finalizer metatables */

    for (i = 0U; i < ISAAC_LBR_SCALAR_COUNT; ++i)
        (void)class_of_key(s_bodies[i].key);
    s_entity_class = class_of_key(ENTITY_KEY);
    /* classes: every fourth one a root, the others derived from the previous
     * (getClass walks __parent; 0x3f90b0 walks it too) */
    for (j = 0U; j < s_class_count; ++j) {
        int parent = (j % 4U) ? s_class_ref[j - 1U] : 0;
        int cparent = (j % 4U) ? s_class_const_ref[j - 1U] : 0;
        snprintf(name, sizeof name, "K%u", (unsigned)j);
        s_class_ref[j] = make_class_table(L, name, parent, 0);
        snprintf(name, sizeof name, "const K%u", (unsigned)j);
        s_class_const_ref[j] = make_class_table(L, name, cparent, 1);
        lua_setfield(L, -2, "__const");          /* mutable.__const = const */
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, (const void *)(uintptr_t)s_class_keys[j]);
        /* a plain method (direct hit), a Lua getter (not a C function), a
         * guest getter, a raising guest getter */
        snprintf(name, sizeof name, "m%u", (unsigned)j);
        lua_pushinteger(L, 9000 + (lua_Integer)j);
        lua_setfield(L, -2, name);
        lua_getfield(L, -1, "__propget");
        snprintf(name, sizeof name, "lf%u", (unsigned)j);
        CHECK(luaL_loadstring(L, "return 'lua-getter'") == LUA_OK);
        lua_setfield(L, -2, name);
        snprintf(name, sizeof name, "gc%u", (unsigned)j);
        push_closure(FAKE_GETTER_C, 0U);
        lua_setfield(L, -2, name);
        snprintf(name, sizeof name, "gr%u", (unsigned)j);
        push_closure(FAKE_GETTER_RAISE, 0U);
        lua_setfield(L, -2, name);
        lua_pop(L, 2);
        table_set_ref(L, "C", (lua_Integer)j + 1, s_class_ref[j]);
        table_set_ref(L, "CC", (lua_Integer)j + 1, s_class_const_ref[j]);
        table_set_int(L, "par", (lua_Integer)j + 1, (j % 4U) ? (lua_Integer)j : 0);
    }
    /* row getters: upvalue 1 = the member function pointer cell */
    for (i = 0U; i < ISAAC_LBR_SCALAR_COUNT; ++i) {
        uint32_t klass = class_of_key(s_bodies[i].key);
        uint32_t *cell = (uint32_t *)lua_newuserdata(L, 4U);
        *cell = FAKE_MEMBER(i);
        push_closure(VA(s_bodies[i].rva), 1U);
        lua_getglobal(L, "G");
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
        lua_pop(L, 1);
        /* the getter in its class's __propget (const getters in the const
         * table's too) */
        snprintf(name, sizeof name, "f%u", (unsigned)i);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_class_ref[klass]);
        lua_getfield(L, -1, "__propget");
        lua_pushvalue(L, -3);
        lua_setfield(L, -2, name);
        lua_pop(L, 2);
        if (s_bodies[i].cb) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_class_const_ref[klass]);
            lua_getfield(L, -1, "__propget");
            lua_pushvalue(L, -3);
            lua_setfield(L, -2, name);
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        table_set_int(L, "rowcls", (lua_Integer)i + 1, (lua_Integer)klass + 1);
        table_set_int(L, "rowcb", (lua_Integer)i + 1, (lua_Integer)s_bodies[i].cb);
    }
    /* GetData and the bool(int,int) thunk */
    {
        uint32_t *cell = (uint32_t *)lua_newuserdata(L, 4U);
        *cell = FAKE_GETDATA;
        push_closure(VA(ISAAC_LBR_GETDATA_RVA), 1U);
        lua_setglobal(L, "GetData");
        cell = (uint32_t *)lua_newuserdata(L, 4U);
        *cell = FAKE_BOOLII;
        push_closure(VA(ISAAC_LBR_BOOLII_RVA), 1U);
        lua_setglobal(L, "B");
    }
    /* objects */
    for (j = 0U; j < s_class_count; ++j) {
        void *ud = lua_newuserdata(L, USERDATA_SIZE);
        init_object(ud, j, 0U, LUA_REFNIL);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_class_ref[j]);
        lua_setmetatable(L, -2);
        lua_getglobal(L, "objs");
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, (lua_Integer)j + 1);
        lua_pop(L, 2);
        ud = lua_newuserdata(L, USERDATA_SIZE);
        init_object(ud, j, 0U, LUA_REFNIL);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_class_const_ref[j]);
        lua_setmetatable(L, -2);
        lua_getglobal(L, "cobjs");
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, (lua_Integer)j + 1);
        lua_pop(L, 2);
    }
    /* entities: three with a data table, one with LUA_REFNIL */
    for (i = 0U; i < 4U; ++i) {
        int data_ref = LUA_REFNIL;
        void *ud;
        if (i < 3U) {
            lua_newtable(L);
            lua_pushinteger(L, 100 + (lua_Integer)i);
            lua_setfield(L, -2, "x");
            data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        ud = lua_newuserdata(L, USERDATA_SIZE);
        init_object(ud, s_entity_class, 0U, data_ref);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_class_ref[s_entity_class]);
        lua_setmetatable(L, -2);
        lua_getglobal(L, "ents");
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
        lua_pop(L, 2);
    }
    lua_pushinteger(L, (lua_Integer)s_entity_class + 1);
    lua_setglobal(L, "entcls");
    /* finalizer metatables: a plain guest __gc, a guest __gc that crosses
     * imports, and a class derived from row 1's class whose __gc is row 1's
     * getter (a replayed body run by the collector) */
    for (i = 0U; i < 3U; ++i) {
        lua_newtable(L);
        if (i < 2U) {
            push_closure(i == 0U ? FAKE_GC_PLAIN : FAKE_GC_IMPORTS, 0U);
        } else {
            uint32_t klass = class_of_key(s_bodies[0].key);
            lua_pop(L, 1);
            (void)make_class_table(L, "GCX", s_class_ref[klass], 0);
            lua_newtable(L);
            lua_pushboolean(L, 1);
            lua_rawsetp(L, -2, IDENTITY);
            lua_setfield(L, -2, "__const");
            lua_getglobal(L, "G");
            lua_rawgeti(L, -1, 1);
            lua_remove(L, -2);
        }
        lua_setfield(L, -2, "__gc");
        lua_getglobal(L, "gcmt");
        lua_pushvalue(L, -2);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
        lua_pop(L, 2);
    }
    /* the raising and the owner-clearing finalizers (their own cases) */
    lua_newtable(L);
    push_closure(FAKE_GC_RAISE, 0U);
    lua_setfield(L, -2, "__gc");
    lua_setglobal(L, "gcraise");
    lua_newtable(L);
    push_closure(FAKE_GC_OWNER, 0U);
    lua_setfield(L, -2, "__gc");
    lua_setglobal(L, "gcowner");
    isaac_vita_lua_seam_leave(&s_cpu, root);
    CHECK(lua_gettop(L) == 0);
    CHECK(owner_state() == 0);
}

/* ---- the Lua drivers ------------------------------------------------------------- */

/* One seeded driver for every family; `fam` picks the operation mix, `gc`
 * leaves finalizable garbage and positive GC debt between calls, `hostile`
 * adds hostile objects as the last operation. */
static const char s_driver[] =
    "local fam, seed, steps, gc, hostile = ...\n"
    "local G, rowcls, rowcb, C, CC, par = G, rowcls, rowcb, C, CC, par\n"
    "local objs, cobjs, ents, B, GetData = objs, cobjs, ents, B, GetData\n"
    "local nrows, ncls = #G, #C\n"
    "local out = {}\n"
    "local function rnd(n) seed = (seed * 1103515245 + 12345) % 2147483648; return seed % n + 1 end\n"
    "local function add(ok, v, w) out[#out + 1] = tostring(ok) .. ':' .. tostring(v) .. ':' .. tostring(w) end\n"
    "local function anyobj() local k = rnd(8)\n"
    "  if k == 1 then return cobjs[rnd(ncls)] elseif k == 2 then return 7\n"
    "  elseif k == 3 then return {} elseif k == 4 then return nil end\n"
    "  return objs[rnd(ncls)] end\n"
    "local function child(j) for k = 1, ncls do if par[k] == j and rnd(2) == 1 then return k end end return j end\n"
    "local ints = { 0, 1, -1, 2147483647, -2147483648, 4294967296 + 5, '12', 3.0, 2.5, 'x', nil, 77 }\n"
    "local function key(j)\n"
    "  local k = rnd(9)\n"
    "  if k <= 3 then local i = rnd(nrows) return 'f' .. (i - 1) end\n"
    "  if k == 4 then return 'm' .. (j - 1) elseif k == 5 then return 'lf' .. (j - 1)\n"
    "  elseif k == 6 then return 'gc' .. (j - 1) elseif k == 7 then return 'gr' .. (j - 1)\n"
    "  elseif k == 8 and par[j] ~= 0 then return 'm' .. (par[j] - 1) end\n"
    "  return 'nope' end\n"
    "for step = 1, steps do\n"
    "  if gc then if rnd(3) == 1 then local u = newud(gcmt[rnd(#gcmt)]) end\n"
    "    if rnd(4) == 1 then gc_debt() end end\n"
    "  if fam == 1 then\n"
    "    local i = rnd(nrows); local j = rowcls[i]; local k = rnd(10); local o\n"
    "    if k <= 4 then o = objs[j] elseif k == 5 then o = cobjs[j] elseif k == 6 then o = objs[child(j)]\n"
    "    elseif k == 7 then o = cobjs[child(j)] else o = anyobj() end\n"
    "    if k == 10 then add(pcall(G[i])) else add(pcall(G[i], o)) end\n"
    "  elseif fam == 2 then\n"
    "    local k = rnd(8); local o\n"
    "    if k <= 5 then o = ents[rnd(#ents)] elseif k == 6 then o = objs[rnd(ncls)] elseif k == 7 then o = cobjs[entcls] end\n"
    "    local ok, t = pcall(GetData, o)\n"
    "    add(ok, type(t), type(t) == 'table' and t.x or t)\n"
    "  elseif fam == 3 then\n"
    "    local a, b = ints[rnd(#ints)], ints[rnd(#ints)]\n"
    "    if rnd(10) == 1 then add(pcall(B, a)) else add(pcall(B, a, b)) end\n"
    "  else\n"
    "    local j = rnd(ncls); local o = rnd(4) == 1 and cobjs[j] or objs[j]\n"
    "    local name = key(j)\n"
    "    add(pcall(function() return o[name] end))\n"
    "  end\n"
    "end\n"
    "if hostile then\n"
    "  local i = 1; local j = rowcls[i]\n"
    "  local h = newud(C[j], hostile)\n"
    "  if fam == 1 then add(pcall(G[i], h))\n"
    "  elseif fam == 2 then local e = newud(C[entcls], hostile); add(pcall(GetData, e))\n"
    "  else add(pcall(function() return h['f0'] end)) end\n"
    "end\n"
    "return table.concat(out, ';')\n";

static void fork_driver(const char *name, int fam, uint32_t seed, uint32_t steps,
                        int gc, uint32_t hostile_flags, uint32_t headroom,
                        case_stats *st)
{
    static char code[sizeof s_driver + 256];
    char hostile_arg[16];
    if (hostile_flags)
        snprintf(hostile_arg, sizeof hostile_arg, "%u", (unsigned)hostile_flags);
    else
        snprintf(hostile_arg, sizeof hostile_arg, "nil");
    snprintf(code, sizeof code,
             "return (function(...)\n%s\nend)(%d, %u, %u, %s, %s)", s_driver, fam,
             (unsigned)seed, (unsigned)steps, gc ? "true" : "false", hostile_arg);
    fork_chunk(name, code, headroom, st);
}

/* ---- checks ------------------------------------------------------------------------- */

static uint32_t sum_handled(const isaac_vita_lua_binding_replay_stats *s)
{
    return s->handled[0] + s->handled[1] + s->handled[2] + s->handled[3];
}

static uint32_t sum_fallbacks(const isaac_vita_lua_binding_replay_stats *s)
{
    return s->fallbacks[0] + s->fallbacks[1] + s->fallbacks[2] + s->fallbacks[3];
}

static const char *s_family_names[4] = { "scalar", "getdata", "boolii", "index2" };

static int family_enabled(int family)
{
    return (s_family_mask & (1U << family)) != 0U;
}

/* exact accounting of one handled call: crossings elided per shape */
typedef struct accounting_case {
    const char *name;
    int family;
    const char *code;
    uint32_t handled;    /* replayed body activations */
    uint32_t elided;     /* crossings served by the replay */
} accounting_case;

static const accounting_case s_accounting[] = {
    /* template A: lua_type, lua_touserdata, lua_pushinteger (the helper) */
    { "row exact object", 0, "return tostring(G[1](objs[rowcls[1]]))", 1U, 3U },
    /* nil argument: lua_type 0, edi = 0, the member gets ecx = 0 (an absent
     * argument is LUA_TNONE = -1, which goes to getClass and raises) */
    { "row with nil argument", 0, "return tostring(G[1](nil))", 1U, 3U },
    /* 446d90: type, touserdata, rawgeti (280cc0), luaL_ref, luaL_unref,
     * rawgeti (280cc0), luaL_unref */
    { "GetData with data", 1, "return tostring(GetData(ents[1]).x)", 1U, 7U },
    /* LUA_REFNIL (cmp [esi+4], -1; je 0x446e14): the 280cc0 copy and its
     * luaL_ref are skipped: type, touserdata, unref, rawgeti, unref */
    { "GetData REFNIL", 1, "return tostring(GetData(ents[4]))", 1U, 5U },
    /* 433870: touserdata, checkinteger x2, pushboolean */
    { "bool(int,int)", 2, "return tostring(B(3, 2))", 1U, 4U },
    /* 3f90b0 direct hit: getmetatable, pushvalue, rawget, type, rotate, settop */
    { "index direct method", 3, "return tostring(objs[1].m0)", 1U, 6U },
    /* propget hit: getmetatable, pushvalue, rawget, type (nil), then
     * settop, absindex, pushstring, rawget, pushvalue, rawget, rotate,
     * settop, iscfunction (C), rotate, settop, pushvalue, callk = 17, plus
     * the getter row's 3 */
    { "index propget getter", 3, "return tostring(objs[rowcls[1]].f0)", 2U, 20U },
};

static void run_accounting(void)
{
    uint32_t i;
    for (i = 0U; i < sizeof s_accounting / sizeof s_accounting[0]; ++i) {
        const accounting_case *k = &s_accounting[i];
        case_stats st;
        uint32_t handled, elided;
        int expect = family_enabled(k->family) &&
                     (k->handled == 1U || family_enabled(0));
        fork_chunk(k->name, k->code, 512U, &st);
        handled = sum_handled(&st.after) - sum_handled(&st.before);
        elided = st.after.imports_elided - st.before.imports_elided;
        printf("  count : %-26s replayed=%u elided=%u census T/R=%u/%u result=%s\n",
               k->name, (unsigned)handled, (unsigned)elided, (unsigned)st.census_t,
               (unsigned)st.census_r, s_wr.result);
        if (expect && s_window_full && !s_force_permille && !s_verify) {
            CHECK(handled == k->handled);
            CHECK(elided == k->elided);
            CHECK(st.after.imports_real == st.before.imports_real);
        }
    }
}

/* Direct calls of a wrap with the translated body stubbed: the fallback
 * decisions, before any side effect. */
static void direct_fallback(const char *name, uint32_t body_index,
                            uint32_t reason, void (*prepare)(CPU *),
                            void (*undo)(CPU *))
{
    CPU *c = &s_cpu;
    const body *b = &s_bodies[body_index];
    const isaac_vita_lua_binding_replay_stats *st = isaac_vita_lua_binding_replay_stats_get();
    uint32_t real_before, fallbacks_before, entry_esp, low_before;
    uint8_t window_before[ISAAC_LBR_WINDOW_BELOW + 8U];
    int top_before;
    if (!b->wrap)
        return;
    reset_cpu(c, 512U);
    gpush(c, s_state_word);
    gpush(c, 0x00421200U);
    if (prepare)
        prepare(c);
    real_before = s_real_calls;
    fallbacks_before = sum_fallbacks(st);
    entry_esp = c->esp;
    low_before = c->stack_low_water;
    top_before = lua_gettop(s_L);
    memcpy(window_before, (const void *)(uintptr_t)(entry_esp - ISAAC_LBR_WINDOW_BELOW),
           sizeof window_before);
    s_real_stub = 1;
    s_arm = ARM_REPLAY;
    call_wrap(c, b);
    s_arm = ARM_TRANSLATED;
    s_real_stub = 0;
    CHECK(s_real_calls == real_before + 1U);
    CHECK(c->eax == 0xdeadbeefU && c->esp == entry_esp + 4U && !c->fault);
    CHECK(c->stack_low_water == low_before && lua_gettop(s_L) == top_before);
    CHECK(memcmp(window_before,
                 (const void *)(uintptr_t)(entry_esp - ISAAC_LBR_WINDOW_BELOW),
                 sizeof window_before) == 0);
    CHECK(sum_fallbacks(st) == fallbacks_before + 1U);
    CHECK(st->last_reason == reason);
    CHECK(s_contract_failures == 0U);
    if (undo)
        undo(c);
    printf("  stub  : %-44s reason=%u OK\n", name, (unsigned)reason);
}

static int s_probe_root;
static void prep_foreign(CPU *c)
{
    (void)c;
    CHECK(isaac_vita_lua_seam_enter(&s_other, &s_probe_root) && s_probe_root == 1);
}
static void undo_foreign(CPU *c)
{
    (void)c;
    isaac_vita_lua_seam_leave(&s_other, s_probe_root);
}
static void prep_unbound(CPU *c) { c->stack_owner = NULL; }
static void undo_unbound(CPU *c) { c->stack_owner = c; }
static void prep_deep(CPU *c)
{
    /* E - WINDOW below the floor: the return word and L copied down (the
     * guard zone under the floor keeps the window snapshot readable) */
    uint32_t e = c->stack_floor + 0x20U;
    st32(e, ld32(c->esp));
    st32(e + 4U, ld32(c->esp + 4U));
    c->esp = e;
}

/* The replay's stats are reset between modes and around every pin leg; the
 * VERIFY verdict is over the whole run, so its counters accumulate here. */
static uint32_t s_verify_runs_total;
static uint32_t s_verify_mismatches_total;
static void oracle_stats_reset(void)
{
    const isaac_vita_lua_binding_replay_stats *st = isaac_vita_lua_binding_replay_stats_get();
    s_verify_runs_total += st->verify_runs;
    s_verify_mismatches_total += st->verify_mismatches;
    isaac_vita_lua_binding_replay_stats_reset();
}

static uint32_t s_saved_word;
static uint32_t s_corrupt_at;
static void prep_corrupt(CPU *c)
{
    (void)c;
    s_saved_word = ld32(s_corrupt_at);
    st32(s_corrupt_at, s_saved_word ^ 0x00000100U);
    oracle_stats_reset();
}
static void undo_corrupt(CPU *c)
{
    (void)c;
    st32(s_corrupt_at, s_saved_word);
    oracle_stats_reset();
}
static void prep_bad_name(CPU *c)
{
    (void)c;
    s_bad_name_slot = 0x0060620cU;         /* lua_type */
    oracle_stats_reset();
}
static void undo_bad_name(CPU *c)
{
    (void)c;
    s_bad_name_slot = 0U;
    oracle_stats_reset();
}
static void prep_bad_binding(CPU *c)
{
    (void)c;
    s_bad_binding_id = 89U;                /* lua_settop's dense id */
    oracle_stats_reset();
}
static void undo_bad_binding(CPU *c)
{
    (void)c;
    s_bad_binding_id = 0xffffffffU;
    oracle_stats_reset();
}

static void run_direct_fallbacks(void)
{
    uint32_t first = family_enabled(0) ? 0U : (family_enabled(1) ? BODY_GETDATA :
                     (family_enabled(2) ? BODY_BOOLII : BODY_INDEX2));
    uint32_t mismatches_before = log_count("binding replay pin MISMATCH");
    direct_fallback("no import scope open (owner word 0)", first,
                    ISAAC_LBR_REASON_OWNER, NULL, NULL);
    CHECK(owner_state() == 0);
    direct_fallback("bridge owned by another CPU", first,
                    ISAAC_LBR_REASON_OWNER, prep_foreign, undo_foreign);
    CHECK(owner_state() == 0);
    direct_fallback("unbound guest stack", first, ISAAC_LBR_REASON_STACK,
                    prep_unbound, undo_unbound);
    direct_fallback("window below the stack floor", first,
                    ISAAC_LBR_REASON_STACK, prep_deep, NULL);
    /* template A: the word at +28 holds the canBeConst byte (+29); a single
     * body: a prologue byte */
    s_corrupt_at = VA(s_bodies[first].rva) + (first == 0U ? 28U : 8U);
    direct_fallback("body bytes differ from the pinned image", first,
                    ISAAC_LBR_REASON_PINS, prep_corrupt, undo_corrupt);
    s_corrupt_at = VA(0x0060620cU);
    direct_fallback("IAT word is not its tag", first, ISAAC_LBR_REASON_PINS,
                    prep_corrupt, undo_corrupt);
    s_corrupt_at = VA(ISAAC_LBR_STR_PROPGET_RVA);
    direct_fallback(".rdata __propget differs", first, ISAAC_LBR_REASON_PINS,
                    prep_corrupt, undo_corrupt);
    direct_fallback("IAT slot names another import", first,
                    ISAAC_LBR_REASON_PINS, prep_bad_name, undo_bad_name);
    direct_fallback("slot bound to a foreign endpoint", first,
                    ISAAC_LBR_REASON_PINS, prep_bad_binding, undo_bad_binding);
    CHECK(log_count("binding replay pin MISMATCH") >= mismatches_before + 5U);
    /* every pin restored: the next call is served again.  Through Lua (a fork),
     * not call_wrap: the body reads lua_upvalueindex(1), which only exists
     * inside the C closure the game registered. */
    if (first == 0U) {
        case_stats again;
        fork_chunk("served again after the pin legs", "return G[1](nil)", 256U, &again);
        CHECK(again.after.handled[0] - again.before.handled[0] == 1U);
        CHECK(again.after.fallbacks[0] == again.before.fallbacks[0]);
        CHECK(again.after.last_reason == ISAAC_LBR_HANDLED);
        printf("  stub  : %-44s served again OK\n", "every pin restored");
    }
}

static void print_family_stats(const char *what)
{
    const isaac_vita_lua_binding_replay_stats *s = isaac_vita_lua_binding_replay_stats_get();
    int f;
    printf("  stats : %-16s", what);
    for (f = 0; f < 4; ++f)
        printf(" %s=%u/%u/%u", s_family_names[f], (unsigned)s->calls[f],
               (unsigned)s->handled[f], (unsigned)s->fallbacks[f]);
    printf(" elided=%u real=%u drift=%u\n", (unsigned)s->imports_elided,
           (unsigned)s->imports_real, (unsigned)s->drift_calls);
}

static void run_seeded(const char *tag, uint32_t steps, uint32_t rounds, int gc)
{
    int fam;
    uint32_t round;
    char name[96];
    for (fam = 1; fam <= 4; ++fam) {
        for (round = 0U; round < rounds; ++round) {
            snprintf(name, sizeof name, "%s %s round %u", tag, s_family_names[fam - 1],
                     (unsigned)round);
            /* E mod 8 varies with the headroom: the bodies' and esp, -8 */
            fork_driver(name, fam, 0x1234U + 7919U * round + 104729U * (uint32_t)fam,
                        steps, gc, 0U, 256U + 4U * (round % 4U), NULL);
        }
    }
}

/* Finish every pending finalizer under the bridge owner (a raising one ends
 * a collection with LUA_ERRGCMM, so repeat until three clean ones). */
/* Through the bridge's lua_pcallk import, as the game runs every chunk: a
 * finalizer that raises from inside a translated callback abandons that
 * callback's bridge frame, and only the bridge's native pcall seam recovers
 * abandoned frames (host_vita_lua.c vita_lua_recover_callbacks).  A direct
 * lua_pcall here left them standing, and the next guest luaL_error then
 * longjmp'd into a dead frame (an "unprotected" LUA_ERRGCMM panic). */
static void drain_finalizers(void)
{
    static world drained;      /* status / result only (no arena) */
    int root, clean = 0, i, status;
    s_phase = "drain_finalizers";
    for (i = 0; i < 400 && clean < 3; ++i) {
        lua_settop(s_L, 0);
        root = 0;
        CHECK(isaac_vita_lua_seam_enter(&s_cpu, &root) && root == 1);
        status = luaL_loadstring(s_L, "collectgarbage('collect')");
        isaac_vita_lua_seam_leave(&s_cpu, root);
        if (status == LUA_OK) {
            run_loaded(512U, &drained);
            clean = drained.status == 0U ? clean + 1 : 0;
        } else {
            clean = 0;                    /* a GC step of the parser raised */
        }
        lua_settop(s_L, 0);
        s_cpu.fault = NULL;
        owner_reset();
    }
    CHECK(clean >= 3);
    s_phase = "after drain_finalizers";
}

/* Where the finalizers of one fork ran (both arms; only the replayed arm has
 * taps and live wraps). */
typedef struct gc_census {
    uint32_t runs[8], in_tap[8], in_mirror, wraps_in_tap, taps;
} gc_census;

static void gc_census_take(gc_census *g)
{
    memcpy(g->runs, s_gc_runs, sizeof g->runs);
    memcpy(g->in_tap, s_gc_in_tap, sizeof g->in_tap);
    g->in_mirror = s_gc_in_mirror;
    g->wraps_in_tap = s_wraps_in_tap;
    g->taps = s_tap_calls;
}

static uint32_t gc_census_sum(const uint32_t *after, const uint32_t *before)
{
    uint32_t i, n = 0U;
    for (i = 0U; i < 8U; ++i)
        n += after[i] - before[i];
    return n;
}

static void print_gc_census(const char *tag, const gc_census *a, const gc_census *b)
{
    printf("  %s: finalizer runs %u, inside a replayed mirror %u, inside an emulated "
           "lua_pushstring %u (plain/imports/owner/raise %u/%u/%u/%u), replayed bodies "
           "entered there %u, emulated pushstrings %u\n", tag,
           (unsigned)gc_census_sum(b->runs, a->runs), (unsigned)(b->in_mirror - a->in_mirror),
           (unsigned)gc_census_sum(b->in_tap, a->in_tap),
           (unsigned)(b->in_tap[GC_PLAIN] - a->in_tap[GC_PLAIN]),
           (unsigned)(b->in_tap[GC_IMPORTS] - a->in_tap[GC_IMPORTS]),
           (unsigned)(b->in_tap[GC_OWNER] - a->in_tap[GC_OWNER]),
           (unsigned)(b->in_tap[GC_RAISE] - a->in_tap[GC_RAISE]),
           (unsigned)(b->wraps_in_tap - a->wraps_in_tap), (unsigned)(b->taps - a->taps));
}

/* A raising __gc between replayed calls.  The GC step is spent in
 * OP_CLOSURE's luaC_checkGC (the pcall'd closure is the first allocation
 * after gc_debt(), before 3f90b0 is entered), so the
 * LUA_ERRGCMM is raised at the Lua level, and the replayed calls that follow
 * run with abandoned bridge frames standing (recovered at the next bridge
 * pcall).  The gcerr line prints where the finalizers ran; a raise from
 * inside an emulated lua_pushstring is run_gc_in_emulated_pushstring's. */
static void run_raising_finalizer(void)
{
    static const char code[] =
        "local mt = gcraise\n"
        "local caught = {}\n"
        "for i = 1, 4000 do\n"
        "  local u = newud(mt)\n"
        "  gc_debt()\n"
        "  local ok, m = pcall(function() return objs[rowcls[1]].f0 end)\n"
        "  if not ok then caught[#caught + 1] = tostring(m) end\n"
        "  local ok2, m2 = pcall(B, i, 1)\n"
        "  if not ok2 then caught[#caught + 1] = tostring(m2) end\n"
        "  local ok3, m3 = pcall(GetData, ents[1])\n"
        "  if not ok3 then caught[#caught + 1] = tostring(m3) end\n"
        "  if #caught > 60 then break end\n"
        "end\n"
        "return table.concat(caught, '|')\n";
    gc_census a, b;
    gc_census_take(&a);
    fork_chunk("raising finalizer inside the replay", code, 256U, NULL);
    gc_census_take(&b);
    if (strstr(s_wr.result, "error in __gc metamethod") == NULL)
        fprintf(stderr, "raising finalizer: status=%u result=%.300s\n",
                (unsigned)s_wr.status, s_wr.result);
    CHECK(strstr(s_wr.result, "error in __gc metamethod") != NULL);
    printf("  gcerr : LUA_ERRGCMM between replayed calls: worlds equal (%.120s)\n",
           s_wr.result);
    print_gc_census("gcerr ", &a, &b);
    drain_finalizers();
}

/* A finalizer that clears the bridge owner word between replayed calls (the
 * GC step is spent in OP_CLOSURE again): the next bridge
 * crossing sees the cleared word.  The same finalizer run inside an emulated
 * lua_pushstring -- lbr_observed's typed / generic leave cases,
 * host_vita_lua_binding_replay.c:464-489 -- is the "owner" fork of
 * run_gc_in_emulated_pushstring. */
static void run_gc_owner_clear(void)
{
    static const char code[] =
        "local caught = {}\n"
        "for i = 1, 600 do\n"
        "  local u = newud(gcowner)\n"
        "  gc_debt()\n"
        "  local ok, m = pcall(function() return objs[rowcls[1]].f0 end)\n"
        "  caught[#caught + 1] = tostring(ok) .. ':' .. tostring(m)\n"
        "end\n"
        "return table.concat(caught, '|')\n";
    gc_census a, b;
    gc_census_take(&a);
    fork_chunk("finalizer clears the owner inside the replay", code, 256U, NULL);
    gc_census_take(&b);
    printf("  gcown : status=%u faults=%u (%s) worlds equal\n", (unsigned)s_wr.status,
           (unsigned)s_wr.fault_calls, s_wr.first_fault ? s_wr.first_fault : "-");
    print_gc_census("gcown ", &a, &b);
    drain_finalizers();
}

/* GC steps inside an emulated lua_pushstring (the fixture that catches a
 * missing lbr_observed()).  `get`'s __index call is the first allocation after
 * gc_debt(), so the step lands in 3f90b0's first lua_pushstring
 * ("__propget"), whose emulation (LBR_EMU_PUSHSTRING) then runs the
 * finalizers and ends in lbr_observed(), host_vita_lua_binding_replay.c:664:
 *   benign  plain, import-making (lua_pushstring + lua_settop crossings) and
 *           gcmt[3]'s __gc, row 1's getter: a replayed body run there;
 *   owner   clears the bridge owner word: the typed endpoint's leave stores
 *           the entered owner back, the generic one does not (lbr_observed's
 *           two cases, :464-489);
 *   raise   LUA_ERRGCMM out of the emulated crossing, through the wrap.
 * Each fork CHECKs that its finalizers really ran inside an emulated
 * pushstring (the tap above) when 3f90b0 is replayed with the full window.
 * Negative control: the test's mutant-observe leg deletes that
 * lbr_observed() call and must be caught here.  At most 60 raises per fork:
 * every pending raising object costs drain_finalizers one more pass. */
static void gcps_fork(const char *name, const char *mts, uint32_t kind, int nested)
{
    static char code[2048];
    char label[96];
    gc_census a, b;
    int full = family_enabled(3) && s_window_full;
    snprintf(code, sizeof code,
             "local o = objs[rowcls[1]]\n"
             "local mts = { %s }\n"
             "local function get(x) return x.f0 end\n"
             "local caught, n = {}, 0\n"
             "for i = 1, 3000 do\n"
             "  newud(mts[i %% #mts + 1])\n"
             "  gc_debt()\n"
             "  local ok, m = pcall(get, o)\n"
             "  if not ok then n = n + 1; if n <= 6 then caught[#caught + 1] = tostring(m) end end\n"
             "  if n >= 60 then break end\n"
             "end\n"
             "return tostring(n) .. ':' .. table.concat(caught, '|')\n", mts);
    snprintf(label, sizeof label, "gc in emulated pushstring: %s", name);
    gc_census_take(&a);
    fork_chunk(label, code, 256U, NULL);
    gc_census_take(&b);
    printf("  gcps  : %-7s worlds equal; status=%u faults=%u (%s) result=%.60s\n", name,
           (unsigned)s_wr.status, (unsigned)s_wr.fault_calls,
           s_wr.first_fault ? s_wr.first_fault : "-", s_wr.result);
    print_gc_census("gcps  ", &a, &b);
    if (full) {
        CHECK(b.in_tap[kind] - a.in_tap[kind] > 0U);
        if (kind == GC_PLAIN)
            CHECK(b.in_tap[GC_IMPORTS] - a.in_tap[GC_IMPORTS] > 0U);
        if (nested && family_enabled(0))
            CHECK(b.wraps_in_tap - a.wraps_in_tap > 0U);
    }
    drain_finalizers();
}

static void run_gc_in_emulated_pushstring(void)
{
    gcps_fork("benign", "gcmt[1], gcmt[2], gcmt[3]", GC_PLAIN, 1);
    gcps_fork("owner", "gcowner", GC_OWNER, 0);
    gcps_fork("raise", "gcraise", GC_RAISE, 0);
}

/* VERIFY: GC steps -- and finalizers --
 * inside the mirror run of a verified const getter, the case VERIFY's
 * live-state compare exists for (host_vita_lua_binding_replay.c:819-870).
 * Run first in VERIFY builds: the replay verifies the first 1000 calls of
 * const rows, then every 4096th (:779-807), and its hit counter cannot be
 * reset from here; 200 iterations leave most of the first 1000 to run_mode.
 * CHECKs that some verified call ran a finalizer inside its mirror run; the
 * fork compare and the final verify-mismatch CHECK do the rest. */
static void run_verify_gc(void)
{
    static const char code[] =
        "local i = 1\n"
        "while rowcb[i] ~= 1 do i = i + 1 end\n"
        "local f, o = G[i], objs[rowcls[i]]\n"
        "local mts = { gcmt[1], gcmt[2], gcmt[3] }\n"
        "local s = 0\n"
        "for k = 1, 200 do\n"
        "  newud(mts[k % 3 + 1])\n"
        "  gc_debt()\n"
        "  s = s + f(o)\n"
        "end\n"
        "return tostring(s)\n";
    const isaac_vita_lua_binding_replay_stats *st = isaac_vita_lua_binding_replay_stats_get();
    uint32_t calls0 = s_mirror_gc_calls, runs0 = s_mirror_gc_runs;
    uint32_t verify0 = st->verify_runs, mismatch0 = st->verify_mismatches;
    gc_census a, b;
    gc_census_take(&a);
    fork_chunk("verify: gc inside verified mirror runs", code, 256U, NULL);
    gc_census_take(&b);
    printf("  vgc   : verified calls with a finalizer inside the mirror run %u (finalizer runs "
           "%u) of %u verified, mismatches %u; result=%.40s\n",
           (unsigned)(s_mirror_gc_calls - calls0), (unsigned)(s_mirror_gc_runs - runs0),
           (unsigned)(st->verify_runs - verify0), (unsigned)(st->verify_mismatches - mismatch0),
           s_wr.result);
    print_gc_census("vgc   ", &a, &b);
    CHECK(s_mirror_gc_calls - calls0 > 0U);
    CHECK(st->verify_mismatches == mismatch0);
    drain_finalizers();
}

static void run_hostile(void)
{
    static const uint32_t flags[] = {
        OBJ_IMPORTS, OBJ_OWNER_CLEAR, OBJ_OWNER_FOREIGN, OBJ_STACK_REBIND,
        OBJ_IMPORTS | OBJ_OWNER_CLEAR
    };
    static const char *const names[] = {
        "member crosses imports", "member clears the owner word",
        "member leaves a foreign owner", "member rebinds the guest stack",
        "imports + owner clear"
    };
    uint32_t i;
    int fam;
    char name[128];
    for (i = 0U; i < sizeof flags / sizeof flags[0]; ++i) {
        for (fam = 1; fam <= 4; ++fam) {
            if (fam == 3)
                continue;           /* bool(int,int) calls a free function */
            snprintf(name, sizeof name, "hostile %s (%s)", names[i], s_family_names[fam - 1]);
            fork_driver(name, fam, 0x777U + i, 20U, 0, flags[i], 256U, NULL);
            printf("  hostile: %-46s status=%u faults=%u\n", name,
                   (unsigned)s_wr.status, (unsigned)s_wr.fault_calls);
        }
    }
}

static void map_image(const char *path)
{
    FILE *image;
    long size;
    void *mapped;
    /* the frozen image as the loader leaves it (written by the test):
     * sections at their RVAs, .reloc applied for GUEST_IMAGE_BASE, every
     * IAT word = its own VA (guest.c tag_imports) */
    image = fopen(path, "rb");
    CHECK(image != NULL);
    CHECK(fseek(image, 0, SEEK_END) == 0);
    size = ftell(image);
    CHECK(size > 0x700000 && fseek(image, 0, SEEK_SET) == 0);
    mapped = VirtualAlloc((void *)(uintptr_t)VA(0U), (SIZE_T)size,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(mapped == (void *)(uintptr_t)VA(0U));
    CHECK(fread(mapped, 1U, (size_t)size, image) == (size_t)size);
    fclose(image);
    CHECK(ld32(VA(0x0060620cU)) == VA(0x0060620cU));
}

static void run_mode(int rounds)
{
    const isaac_vita_lua_binding_replay_stats *st = isaac_vita_lua_binding_replay_stats_get();
    int f;
    const char *mode = s_native_getclass ? "native" : "translated";
    char tag[64];

    printf("getClass %s:\n", s_native_getclass ? "native (seam wrap)" : "translated body");
    oracle_stats_reset();
    run_accounting();
    snprintf(tag, sizeof tag, "seeded/%s", mode);
    run_seeded(tag, 2000U, (uint32_t)rounds, 0);
    print_family_stats("seeded");
    if (s_window_full && !s_verify) {
        /* the game's shapes: every crossing served, nothing fell back */
        CHECK(st->imports_real == 0U && st->drift_calls == 0U);
        CHECK(st->reasons[ISAAC_LBR_REASON_STACK] == 0U &&
              st->reasons[ISAAC_LBR_REASON_OWNER] == 0U &&
              st->reasons[ISAAC_LBR_REASON_PINS] == 0U);
    }
    for (f = 0; f < 4; ++f)
        CHECK(family_enabled(f) ? st->handled[f] > 0U : st->calls[f] == 0U);
    snprintf(tag, sizeof tag, "gc/%s", mode);
    run_seeded(tag, 1500U, (uint32_t)(rounds / 2 > 0 ? rounds / 2 : 1), 1);
    print_family_stats("gc pressure");
    s_force_permille = 300U;
    snprintf(tag, sizeof tag, "forced/%s", mode);
    run_seeded(tag, 1000U, 2U, 1);
    s_force_permille = 0U;
    print_family_stats("forced real");
    if (family_enabled(0) || family_enabled(1) || family_enabled(3))
        CHECK(st->imports_real > 0U);
    if (s_verify)
        return;   /* VERIFY runs a member twice by design: no hostile shapes */
    run_raising_finalizer();
    run_gc_owner_clear();
    run_gc_in_emulated_pushstring();
    run_hostile();
    print_family_stats("hostile");
    if (family_enabled(0) || family_enabled(1) || family_enabled(3))
        CHECK(st->drift_calls > 0U);
}

int main(int argc, char **argv)
{
    CPU *c = &s_cpu;
    uint32_t arguments[1], i, total_cond;
    const isaac_vita_lua_binding_replay_stats *st;
    int rounds = 10;

    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not swallow the transcript */
    CHECK(argc >= 2);
    if (argc >= 3)
        rounds = atoi(argv[2]);
    if (argc >= 4)
        s_lua_seed = (unsigned int)strtoul(argv[3], NULL, 0);
    map_image(argv[1]);
    arena_init();
    s_w0.arena = (uint8_t *)malloc(ARENA_SIZE);
    s_wt.arena = (uint8_t *)malloc(ARENA_SIZE);
    s_wr.arena = (uint8_t *)malloc(ARENA_SIZE);
    CHECK(s_w0.arena && s_wt.arena && s_wr.arena);

    s_local_pcallk = local_of("Lua5.3.3r.dll!lua_pcallk");
    s_local_pushclosure = local_of("Lua5.3.3r.dll!lua_pushcclosure");
    s_local_newstate_aux = local_of("Lua5.3.3r.dll!luaL_newstate");
    s_local_openlibs = local_of("Lua5.3.3r.dll!luaL_openlibs");
    s_local_close = local_of("Lua5.3.3r.dll!lua_close");
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
    CHECK(isaac_vita_lua_import_fast_prepare(1) == 13U);
#endif
    build_direct_table();

    reset_cpu(c, 512U);
    CHECK(invoke_import(c, s_local_newstate_aux, NULL, 0U));
    s_state_word = c->eax;
    s_L = (lua_State *)(uintptr_t)s_state_word;
    CHECK(s_L != NULL && s_state_word - ARENA_BASE < ARENA_SIZE);
    (void)lua_atpanic(s_L, oracle_panic);
    arguments[0] = s_state_word;
    CHECK(invoke_import(c, s_local_openlibs, arguments, 1U));
    setup_fixture();
    printf("fixture: %u classes, %u rows, entity class %u, arena %u KiB, "
           "families 0x%x window %u verify %d lua seed 0x%08x hash seed 0x%08x\n",
           (unsigned)s_class_count,
           (unsigned)ISAAC_LBR_SCALAR_COUNT, (unsigned)s_entity_class,
           (unsigned)(ARENA->bump >> 10), (unsigned)s_family_mask,
           (unsigned)ISAAC_LBR_WINDOW_BELOW, s_verify, (unsigned)s_lua_seed,
           (unsigned)G(s_L)->seed);

    s_native_getclass = 1;
    if (s_verify && family_enabled(0))
        run_verify_gc();             /* first: it needs the first verified calls */
    run_mode(rounds);
    s_native_getclass = 0;
    run_mode(rounds);
    s_native_getclass = 1;
    run_direct_fallbacks();
    /* every conditional branch of the replayed mirrors, both directions */
    total_cond = 0U;
    printf("  branches:");
    for (i = 0U; i < 7U; ++i) {
        printf(" %u:%u/%u", (unsigned)i, (unsigned)s_cond[i][0], (unsigned)s_cond[i][1]);
        total_cond += s_cond[i][0] + s_cond[i][1];
    }
    printf("\n");
    if (s_family_mask == 0xfU)
        for (i = 0U; i < 7U; ++i)
            CHECK(s_cond[i][0] > 0U && s_cond[i][1] > 0U);
    st = isaac_vita_lua_binding_replay_stats_get();
    s_verify_runs_total += st->verify_runs;
    s_verify_mismatches_total += st->verify_mismatches;
#if ORACLE_VERIFY
    CHECK(!family_enabled(0) || s_verify_runs_total > 0U);
    CHECK(s_verify_mismatches_total == 0U);
    CHECK(!family_enabled(0) || log_count("result=MATCH") > 0U);
    CHECK(log_count("result=MISMATCH") == 0U);
#else
    CHECK(s_verify_runs_total == 0U);
#endif
    CHECK(log_count("[isaac-lua] binding replay: rows=51+3") >= 1U);
    CHECK(s_contract_failures == 0U);
    isaac_vita_lua_binding_replay_stats_log();

    arguments[0] = s_state_word;
    reset_cpu(c, 512U);
    CHECK(invoke_import(c, s_local_close, arguments, 1U));
    printf("binding replay oracle: PASS; forks=%u families=0x%x window=%u verify=%d "
           "fastdispatch=%d import_direct=%d coverage=%d branches=%u forced=%u "
           "real_calls=%u verify=%u/%u gc_in_pushstring=%u gc_in_verified_mirror=%u/%u "
           "arena_live_after_close=%u\n",
           (unsigned)s_forks, (unsigned)s_family_mask,
           (unsigned)ISAAC_LBR_WINDOW_BELOW, s_verify,
#if defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
           1,
#else
           0,
#endif
           GUEST_IMPORT_DIRECT, GUEST_COVERAGE_HOOKS, (unsigned)total_cond,
           (unsigned)s_force_hits, (unsigned)s_real_calls,
           (unsigned)s_verify_mismatches_total, (unsigned)s_verify_runs_total,
           (unsigned)(s_gc_in_tap[0] + s_gc_in_tap[1] + s_gc_in_tap[2] + s_gc_in_tap[3] +
                      s_gc_in_tap[4] + s_gc_in_tap[5] + s_gc_in_tap[6] + s_gc_in_tap[7]),
           (unsigned)s_mirror_gc_calls, (unsigned)s_mirror_gc_runs,
           (unsigned)ARENA->live);
    return 0;
}
