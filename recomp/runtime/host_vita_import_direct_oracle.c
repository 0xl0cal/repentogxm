/* White-box host oracle for the direct IAT import dispatch (guest.h
 * GUEST_IMPORT_CALL/GUEST_IMPORT_JMP -> guest.c guest_import_call).
 *
 * guest.c is included so the real guest_call, the real direct table rebuild
 * hook in guest_register_imports and the real guest_import_call run against
 * the frozen 413-row contract; host_vita_import_id.c is linked as its own
 * translation unit so the binding both routes share (family_indexed_fn) is
 * the production one.  The import families themselves are stubs that record
 * the (kind, local index) they were entered with and mark the CPU they saw.
 *
 * Every check is differential: for each input the outcome of
 * guest_import_call(c, target, id) -- fault or not, fault text and address,
 * EAX, host call census, handler census, import coverage, phase-profile
 * census (with ISAAC_VITA_PROFILE_IMPORT_KINDS also the per-import census
 * g_guest_phase_profile_import_calls, one note per served import), fread
 * cadence -- must equal the outcome of guest_call(c, target).
 * Inputs cover all 413 IDs with the exact slot token, every ID with another
 * ID's token, untagged and off-by-one targets, IDs at and past the table
 * capacity, families that reject, and a table whose registration was
 * refused (the direct table must then be empty). */
#include "guest.c"

#if !defined(ISAAC_VITA_IMPORT_DIRECT) || !defined(ISAAC_VITA_IMPORT_ID_DISPATCH)
#error This oracle requires ISAAC_VITA_IMPORT_ID_DISPATCH and ISAAC_VITA_IMPORT_DIRECT
#endif

#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif
#ifndef ISAAC_VITA_LUA
#define ISAAC_VITA_LUA 0
#endif

typedef struct oracle_import_contract {
    uint16_t id;
    uint32_t slot_rva;
    const char *name;
    uint8_t kind;
    uint8_t local_index;
} oracle_import_contract;

#define ISAAC_VITA_IMPORT_ID_ROW(id, slot, import_name, binding_kind, local) \
    { (uint16_t)(id), (slot), (import_name), \
      (uint8_t)(binding_kind), (uint8_t)(local) },
static const oracle_import_contract s_contract[] = {
#include "host_vita_import_id_map.inc"
};
#undef ISAAC_VITA_IMPORT_ID_ROW

_Static_assert(sizeof s_contract / sizeof s_contract[0] ==
                   ISAAC_VITA_IMPORT_ID_COUNT,
               "oracle import contract count drifted");
_Static_assert(ISAAC_VITA_IMPORT_ID_COUNT <= GUEST_IMPORT_DIRECT_CAPACITY,
               "direct table capacity below the frozen row count");

static guest_import s_imports[ISAAC_VITA_IMPORT_ID_COUNT];
static unsigned char s_coverage[ISAAC_VITA_IMPORT_ID_COUNT];

unsigned g_host_import_calls;

static uint32_t s_last_kind;
static uint32_t s_last_index;
static CPU *s_last_cpu;
static unsigned s_handler_calls;
static unsigned s_fread_notes;
static uint32_t s_reject_kind = UINT32_MAX;

#define ORACLE_MAX_FAMILY_IMPORTS 67U
static const char *s_family_names[ISAAC_VITA_IMPORT_SHARED_LOADER + 1U]
                                 [ORACLE_MAX_FAMILY_IMPORTS];

static void oracle_prepare_family_names(void)
{
    uint32_t i, j;

    for (i = 0U; i <= ISAAC_VITA_IMPORT_SHARED_LOADER; ++i)
        for (j = 0U; j < ORACLE_MAX_FAMILY_IMPORTS; ++j)
            s_family_names[i][j] = NULL;
    for (i = 0U; i < ISAAC_VITA_IMPORT_ID_COUNT; ++i) {
        uint32_t kind = s_contract[i].kind;
        uint32_t index = s_contract[i].local_index;

        if (kind == ISAAC_VITA_IMPORT_UNRESOLVED ||
            kind == ISAAC_VITA_IMPORT_SHARED_LOADER)
            continue;
        if (kind == ISAAC_VITA_IMPORT_FREAD)
            kind = ISAAC_VITA_IMPORT_CRT;
        if (kind < ISAAC_VITA_IMPORT_SHARED_LOADER + 1U &&
            index < ORACLE_MAX_FAMILY_IMPORTS)
            s_family_names[kind][index] = s_contract[i].name;
    }
    /* Shared-loader rows own their family slots in production. */
    s_family_names[ISAAC_VITA_IMPORT_SYNC][2U] =
        "KERNEL32.dll!GetProcAddress";
    s_family_names[ISAAC_VITA_IMPORT_STARTUP][0U] =
        "KERNEL32.dll!FreeLibrary";
    s_family_names[ISAAC_VITA_IMPORT_STARTUP][4U] =
        "KERNEL32.dll!LoadLibraryA";
}

static const char *oracle_family_name(uint32_t kind, uint32_t index)
{
    if (kind >= ISAAC_VITA_IMPORT_SHARED_LOADER + 1U ||
        index >= ORACLE_MAX_FAMILY_IMPORTS)
        return NULL;
    return s_family_names[kind][index];
}

static int oracle_handler(uint32_t kind, CPU *__restrict c, uint32_t index,
                          unsigned *call_count)
{
    s_last_kind = kind;
    s_last_index = index;
    s_last_cpu = c;
    ++s_handler_calls;
    if (kind == s_reject_kind)
        return 0;
    /* Prove the family saw the caller's CPU: the production shims read and
     * write this struct (GPR locals are flushed around the token). */
    c->eax = UINT32_C(0x60000000) | (kind << 8) | index;
    if (call_count)
        ++*call_count;
    return 1;
}

#define ORACLE_DEFINE_FAMILY(prefix, binding_kind)                         \
    const char *isaac_vita_##prefix##_import_name(uint32_t index)          \
    {                                                                      \
        return oracle_family_name((binding_kind), index);                  \
    }                                                                      \
    int isaac_vita_##prefix##_import_indexed(                              \
        CPU *__restrict c, uint32_t index, unsigned *call_count)           \
    {                                                                      \
        return oracle_handler((binding_kind), c, index, call_count);       \
    }

ORACLE_DEFINE_FAMILY(baseline, ISAAC_VITA_IMPORT_BASELINE)
ORACLE_DEFINE_FAMILY(crt, ISAAC_VITA_IMPORT_CRT)
ORACLE_DEFINE_FAMILY(math, ISAAC_VITA_IMPORT_MATH)
ORACLE_DEFINE_FAMILY(lua, ISAAC_VITA_IMPORT_LUA)
ORACLE_DEFINE_FAMILY(rtti, ISAAC_VITA_IMPORT_RTTI)
ORACLE_DEFINE_FAMILY(exception, ISAAC_VITA_IMPORT_EXCEPTION)
ORACLE_DEFINE_FAMILY(file_lock, ISAAC_VITA_IMPORT_FILE_LOCK)
ORACLE_DEFINE_FAMILY(filesystem, ISAAC_VITA_IMPORT_FILESYSTEM)
ORACLE_DEFINE_FAMILY(find, ISAAC_VITA_IMPORT_FIND)
ORACLE_DEFINE_FAMILY(com, ISAAC_VITA_IMPORT_COM)
ORACLE_DEFINE_FAMILY(post_com, ISAAC_VITA_IMPORT_POST_COM)
ORACLE_DEFINE_FAMILY(heap, ISAAC_VITA_IMPORT_HEAP)
ORACLE_DEFINE_FAMILY(steam, ISAAC_VITA_IMPORT_STEAM)
ORACLE_DEFINE_FAMILY(audio, ISAAC_VITA_IMPORT_AUDIO)
ORACLE_DEFINE_FAMILY(gl_wgl, ISAAC_VITA_IMPORT_GL_WGL)
ORACLE_DEFINE_FAMILY(sync, ISAAC_VITA_IMPORT_SYNC)
ORACLE_DEFINE_FAMILY(memory, ISAAC_VITA_IMPORT_MEMORY)
ORACLE_DEFINE_FAMILY(console, ISAAC_VITA_IMPORT_CONSOLE)
ORACLE_DEFINE_FAMILY(user32, ISAAC_VITA_IMPORT_USER32)
ORACLE_DEFINE_FAMILY(startup, ISAAC_VITA_IMPORT_STARTUP)
ORACLE_DEFINE_FAMILY(fls, ISAAC_VITA_IMPORT_FLS)

#undef ORACLE_DEFINE_FAMILY

#if ISAAC_VITA_LUA
/* guest.c's run boundary drops the Lua bridge ownership; no bridge here. */
void isaac_vita_lua_abort_cpu(CPU *c)
{
    (void)c;
}
#endif

/* ISAAC_VITA_LUA_IMPORT_FASTDISPATCH: stand-ins for the typed Lua endpoints.
 * The production binder (host_vita_import_id.c) must hand exactly the
 * thirteen typed names to the typed table on both routes and every other
 * Lua ID to the generic family entry; a refused registration must leave the
 * typed table empty. */
static unsigned s_lua_fast_handler_calls;
#if ISAAC_VITA_LUA && defined(ISAAC_VITA_LUA_IMPORT_FASTDISPATCH)
#define ORACLE_LUA_FAST 1
static const char *const s_lua_fast_names[13] = {
    "Lua5.3.3r.dll!lua_rawgetp", "Lua5.3.3r.dll!lua_getmetatable",
    "Lua5.3.3r.dll!lua_type", "Lua5.3.3r.dll!lua_touserdata",
    "Lua5.3.3r.dll!lua_pushvalue", "Lua5.3.3r.dll!lua_rawget",
    "Lua5.3.3r.dll!lua_rawgeti", "Lua5.3.3r.dll!lua_settop",
    "Lua5.3.3r.dll!lua_gettop", "Lua5.3.3r.dll!lua_pushnil",
    "Lua5.3.3r.dll!lua_pushinteger", "Lua5.3.3r.dll!lua_pushnumber",
    "Lua5.3.3r.dll!lua_pushstring"
};
guest_import_family_fn
    g_isaac_vita_lua_import_fast_by_index[ISAAC_VITA_LUA_IMPORT_COUNT];
static unsigned s_lua_fast_prepare_calls;

static int oracle_lua_fast_handler(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    ++s_lua_fast_handler_calls;
    return oracle_handler(ISAAC_VITA_IMPORT_LUA, c, index, call_count);
}

uint32_t isaac_vita_lua_import_fast_prepare(int enable)
{
    uint32_t i, j, bound = 0U;

    ++s_lua_fast_prepare_calls;
    memset(g_isaac_vita_lua_import_fast_by_index, 0,
           sizeof g_isaac_vita_lua_import_fast_by_index);
    if (!enable)
        return 0U;
    for (i = 0U; i < 13U; ++i) {
        for (j = 0U; j < ISAAC_VITA_LUA_IMPORT_COUNT; ++j) {
            const char *name = oracle_family_name(ISAAC_VITA_IMPORT_LUA, j);
            if (name && strcmp(name, s_lua_fast_names[i]) == 0) {
                g_isaac_vita_lua_import_fast_by_index[j] =
                    oracle_lua_fast_handler;
                ++bound;
                break;
            }
        }
    }
    return bound;
}

static int oracle_lua_fast_expected(const char *name)
{
    uint32_t i;

    for (i = 0U; i < 13U; ++i)
        if (strcmp(name, s_lua_fast_names[i]) == 0)
            return 1;
    return 0;
}

static int oracle_lua_fast_table_empty(void)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_LUA_IMPORT_COUNT; ++i)
        if (g_isaac_vita_lua_import_fast_by_index[i])
            return 0;
    return 1;
}
#else
#define ORACLE_LUA_FAST 0
static int oracle_lua_fast_expected(const char *name)
{
    (void)name;
    return 0;
}

static int oracle_lua_fast_table_empty(void)
{
    return 1;
}
#endif

void isaac_vita_note_archive_fread(void)
{
    ++s_fread_notes;
}

/* No GL/WGL token owner in this oracle: every non-import address falls to
 * guest_lookup and its loud fault on both routes. */
int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

int isaac_vita_shared_loader_import_indexed(CPU *__restrict c,
                                            uint32_t shared_index)
{
    return oracle_handler(ISAAC_VITA_IMPORT_SHARED_LOADER, c, shared_index,
                          &g_host_import_calls);
}

/* ------------------------------------------------------------------ runs */

typedef struct oracle_outcome {
    int faulted;
    const char *fault;
    uint32_t fault_addr;
    int stop_kind;
    uint32_t eax;
    uint32_t fault_count;
    unsigned host_calls;
    unsigned handler_calls;
    unsigned fread_notes;
    unsigned lua_fast_calls;
    uint32_t kind;
    uint32_t index;
    int saw_cpu;
    unsigned char coverage[ISAAC_VITA_IMPORT_ID_COUNT];
    GuestPhaseProfileCounters profile;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    uint32_t import_census[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
#endif
} oracle_outcome;

#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
static uint32_t oracle_census_total(const oracle_outcome *out)
{
    uint32_t i, total = 0U;

    for (i = 0U; i < GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS; ++i)
        total += out->import_census[i];
    return total;
}
#endif

static void oracle_run(int direct, uint32_t target, uint32_t import_id,
                       oracle_outcome *out)
{
    CPU cpu;
    guest_run_scope scope;

    memset(&cpu, 0, sizeof cpu);
    memset(out, 0, sizeof *out);
    memset(s_coverage, 0, sizeof s_coverage);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    memset(g_guest_phase_profile_import_calls, 0,
           sizeof g_guest_phase_profile_import_calls);
#endif
    g_host_import_calls = 0U;
    g_fault_count = 0U;
    s_handler_calls = 0U;
    s_fread_notes = 0U;
    s_lua_fast_handler_calls = 0U;
    s_last_kind = UINT32_MAX;
    s_last_index = UINT32_MAX;
    s_last_cpu = NULL;
    cpu.run_scope = &scope;
    if (setjmp(scope.env) == 0) {
        if (direct)
            guest_import_call(&cpu, target, import_id);
        else
            guest_call(&cpu, target);
        out->faulted = 0;
    } else {
        out->faulted = 1;
    }
    out->fault = cpu.fault;
    out->fault_addr = cpu.fault_addr;
    out->stop_kind = (int)cpu.stop_kind;
    out->eax = cpu.eax;
    out->fault_count = g_fault_count;
    out->host_calls = g_host_import_calls;
    out->handler_calls = s_handler_calls;
    out->fread_notes = s_fread_notes;
    out->lua_fast_calls = s_lua_fast_handler_calls;
    out->kind = s_last_kind;
    out->index = s_last_index;
    out->saw_cpu = s_last_cpu == &cpu;
    memcpy(out->coverage, s_coverage, sizeof out->coverage);
    out->profile = g_guest_phase_profile_counters;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    memcpy(out->import_census, g_guest_phase_profile_import_calls,
           sizeof out->import_census);
#endif
}

static int oracle_same_text(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

static int oracle_outcomes_equal(const oracle_outcome *a,
                                 const oracle_outcome *b)
{
    return a->faulted == b->faulted &&
           oracle_same_text(a->fault, b->fault) &&
           a->fault_addr == b->fault_addr &&
           a->stop_kind == b->stop_kind &&
           a->eax == b->eax &&
           a->fault_count == b->fault_count &&
           a->host_calls == b->host_calls &&
           a->handler_calls == b->handler_calls &&
           a->fread_notes == b->fread_notes &&
           a->lua_fast_calls == b->lua_fast_calls &&
           a->kind == b->kind &&
           a->index == b->index &&
           a->saw_cpu == b->saw_cpu &&
           memcmp(a->coverage, b->coverage, sizeof a->coverage) == 0 &&
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
           memcmp(a->import_census, b->import_census,
                  sizeof a->import_census) == 0 &&
#endif
           memcmp(&a->profile, &b->profile, sizeof a->profile) == 0;
}

static unsigned s_differential_runs;

static int oracle_fail(const char *what, uint32_t id, uint32_t target)
{
    fprintf(stderr, "FAIL id=%u target=%08x: %s\n", (unsigned)id,
            (unsigned)target, what);
    return 1;
}

static int oracle_differential(uint32_t target, uint32_t import_id,
                               const char *what, oracle_outcome *slow_out)
{
    oracle_outcome slow, direct;

    oracle_run(0, target, import_id, &slow);
    oracle_run(1, target, import_id, &direct);
    ++s_differential_runs;
    if (!oracle_outcomes_equal(&slow, &direct)) {
        fprintf(stderr,
                "  slow  : faulted=%d fault=%s addr=%08x eax=%08x host=%u "
                "handler=%u kind=%u index=%u cpu=%d\n",
                slow.faulted, slow.fault ? slow.fault : "(null)",
                (unsigned)slow.fault_addr, (unsigned)slow.eax,
                slow.host_calls, slow.handler_calls, (unsigned)slow.kind,
                (unsigned)slow.index, slow.saw_cpu);
        fprintf(stderr,
                "  direct: faulted=%d fault=%s addr=%08x eax=%08x host=%u "
                "handler=%u kind=%u index=%u cpu=%d\n",
                direct.faulted, direct.fault ? direct.fault : "(null)",
                (unsigned)direct.fault_addr, (unsigned)direct.eax,
                direct.host_calls, direct.handler_calls,
                (unsigned)direct.kind, (unsigned)direct.index,
                direct.saw_cpu);
        return oracle_fail(what, import_id, target);
    }
    if (slow_out)
        *slow_out = slow;
    return 0;
}

static int oracle_kind_direct(uint32_t kind)
{
    if (kind == ISAAC_VITA_IMPORT_UNRESOLVED)
        return 0;
#if !ISAAC_VITA_AUDIO
    if (kind == ISAAC_VITA_IMPORT_AUDIO)
        return 0;
#endif
#if !ISAAC_VITA_LUA
    if (kind == ISAAC_VITA_IMPORT_LUA)
        return 0;
#endif
    return 1;
}

/* The FREAD composite notes the archive cadence and enters the CRT family. */
static uint32_t oracle_expected_kind(uint32_t kind)
{
    return kind == ISAAC_VITA_IMPORT_FREAD ? ISAAC_VITA_IMPORT_CRT : kind;
}

static uint32_t oracle_slot_va(uint32_t id)
{
    return (uint32_t)(GUEST_IMAGE_BASE + s_contract[id].slot_rva);
}

static int oracle_check_table(int expect_bound, const char *what)
{
    uint32_t id;

    for (id = 0U; id < GUEST_IMPORT_DIRECT_CAPACITY; ++id) {
        const guest_import_direct *entry = &g_guest_import_direct[id];
        int bound = id < ISAAC_VITA_IMPORT_ID_COUNT && expect_bound &&
                    oracle_kind_direct(s_contract[id].kind);

        if (!bound) {
            if (entry->slot_va != 0U || entry->fn != NULL)
                return oracle_fail(what, id, entry->slot_va);
            continue;
        }
        if (entry->slot_va != oracle_slot_va(id) || entry->fn == NULL ||
            entry->local_index != s_contract[id].local_index)
            return oracle_fail(what, id, entry->slot_va);
    }
    return 0;
}

static int oracle_test_before_registration(void)
{
    if (oracle_check_table(0, "direct table populated before registration"))
        return 1;
    if (oracle_differential(oracle_slot_va(0U), 0U,
                            "unregistered table: routes differ", NULL))
        return 1;
    return 0;
}

static int oracle_test_all_ids(void)
{
    uint32_t id;
    unsigned direct_hits = 0U;

    for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; ++id) {
        const oracle_import_contract *row = &s_contract[id];
        oracle_outcome slow;
        int bound = oracle_kind_direct(row->kind);

        if (oracle_differential(oracle_slot_va(id), id,
                                "exact token: routes differ", &slow))
            return 1;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
        /* One per-import note under exactly this ID (bound or not: the note
         * precedes the endpoint and the unresolved fault alike), and the
         * differential above proved the direct route matches it. */
        if (slow.import_census[id] != 1U || oracle_census_total(&slow) != 1U)
            return oracle_fail("exact token: per-import census is not one "
                               "note under this ID", id, oracle_slot_va(id));
#endif
        if (bound) {
            if (slow.faulted || slow.handler_calls != 1U ||
                slow.host_calls != 1U || !slow.saw_cpu ||
                slow.kind != oracle_expected_kind(row->kind) ||
                slow.index != row->local_index)
                return oracle_fail("bound row did not reach its family once",
                                   id, oracle_slot_va(id));
            if (slow.eax != (UINT32_C(0x60000000) |
                             (oracle_expected_kind(row->kind) << 8) |
                             row->local_index))
                return oracle_fail("family did not see the caller's CPU",
                                   id, oracle_slot_va(id));
            if ((row->kind == ISAAC_VITA_IMPORT_FREAD) !=
                (slow.fread_notes == 1U))
                return oracle_fail("fread cadence drifted", id,
                                   oracle_slot_va(id));
            /* Typed Lua endpoints: exactly the thirteen names, both routes
             * (the differential above already proved route equality). */
            if (slow.lua_fast_calls !=
                (row->kind == ISAAC_VITA_IMPORT_LUA &&
                 oracle_lua_fast_expected(row->name) ? 1U : 0U))
                return oracle_fail("typed Lua endpoint binding drifted", id,
                                   oracle_slot_va(id));
            ++direct_hits;
        } else if (!slow.faulted || slow.handler_calls != 0U ||
                   slow.host_calls != 0U ||
                   !oracle_same_text(slow.fault, row->name)) {
            return oracle_fail("unbound row did not fault with its name", id,
                               oracle_slot_va(id));
        }
        /* Another row's token with this ID: the guard must fall back to the
         * complete path, which then serves the *other* import. */
        if (oracle_differential(oracle_slot_va((id + 1U) %
                                               ISAAC_VITA_IMPORT_ID_COUNT),
                                id, "foreign token: routes differ", &slow))
            return 1;
        if (oracle_kind_direct(s_contract[(id + 1U) %
                                          ISAAC_VITA_IMPORT_ID_COUNT].kind) &&
            slow.kind != oracle_expected_kind(
                s_contract[(id + 1U) % ISAAC_VITA_IMPORT_ID_COUNT].kind))
            return oracle_fail("foreign token did not serve the other import",
                               id, oracle_slot_va(id));
        /* Untagged slot (raw RVA, never relocated), off-by-one values and
         * a guest store that rewrote the slot word. */
        if (oracle_differential(row->slot_rva, id,
                                "untagged slot: routes differ", NULL) ||
            oracle_differential(oracle_slot_va(id) + 1U, id,
                                "slot+1: routes differ", NULL) ||
            oracle_differential(oracle_slot_va(id) - 4U, id,
                                "slot-4: routes differ", NULL) ||
            oracle_differential(UINT32_C(0x00401000), id,
                                "code pointer in slot: routes differ", NULL) ||
            oracle_differential(0U, id, "zero slot: routes differ", NULL))
            return 1;
    }
    printf("direct-bound rows: %u of %u\n", direct_hits,
           (unsigned)ISAAC_VITA_IMPORT_ID_COUNT);
    return 0;
}

static int oracle_test_ids_out_of_range(void)
{
    static const uint32_t ids[] = {
        ISAAC_VITA_IMPORT_ID_COUNT, GUEST_IMPORT_DIRECT_CAPACITY - 1U,
        GUEST_IMPORT_DIRECT_CAPACITY, GUEST_IMPORT_DIRECT_CAPACITY + 1U,
        UINT32_MAX / 2U, UINT32_MAX
    };
    uint32_t i, id;

    for (i = 0U; i < sizeof ids / sizeof ids[0]; ++i) {
        for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; id += 37U) {
            if (oracle_differential(oracle_slot_va(id), ids[i],
                                    "out-of-range ID: routes differ", NULL))
                return 1;
        }
    }
    return 0;
}

static int oracle_test_rejecting_families(void)
{
    uint32_t kind, id;

    for (kind = 0U; kind <= ISAAC_VITA_IMPORT_SHARED_LOADER; ++kind) {
        s_reject_kind = kind;
        for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; ++id) {
            oracle_outcome slow;

            if (oracle_expected_kind(s_contract[id].kind) != kind)
                continue;
            if (oracle_differential(oracle_slot_va(id), id,
                                    "rejecting family: routes differ", &slow))
                return 1;
            if (oracle_kind_direct(kind) &&
                (!slow.faulted || slow.host_calls != 0U ||
                 !oracle_same_text(slow.fault, s_contract[id].name)))
                return oracle_fail("rejecting family did not fault loudly",
                                   id, oracle_slot_va(id));
        }
    }
    s_reject_kind = UINT32_MAX;
    return 0;
}

static int oracle_test_registration_refused(void)
{
    uint32_t saved = s_imports[1].slot_rva;
    uint32_t id;

    s_imports[1].slot_rva = s_imports[0].slot_rva;
    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    if (oracle_check_table(0, "refused registration left direct entries"))
        return 1;
    if (!oracle_lua_fast_table_empty())
        return oracle_fail("refused registration left typed Lua entries",
                           0U, 0U);
    for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; id += 13U) {
        oracle_outcome slow;

        if (oracle_differential(oracle_slot_va(id), id,
                                "refused registration: routes differ", &slow))
            return 1;
        if (slow.handler_calls != 0U)
            return oracle_fail("refused registration still reached a family",
                               id, oracle_slot_va(id));
    }
    s_imports[1].slot_rva = saved;

    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT - 1U);
    if (oracle_check_table(0, "short registration left direct entries"))
        return 1;
    guest_register_imports(NULL, 0U);
    if (oracle_check_table(0, "null registration left direct entries"))
        return 1;

    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    if (oracle_check_table(1, "exact re-registration did not rebuild"))
        return 1;
#if ORACLE_LUA_FAST
    if (oracle_lua_fast_table_empty())
        return oracle_fail("exact re-registration did not rebind typed Lua "
                           "entries", 0U, 0U);
#endif
    return 0;
}

static void oracle_bench(void)
{
    enum { ITERATIONS = 20000000 };
    CPU cpu;
    guest_run_scope scope;
    clock_t started;
    double slow_ns, direct_ns;
    uint32_t id = 0U, i;
    uint32_t target;

    while (id < ISAAC_VITA_IMPORT_ID_COUNT &&
           s_contract[id].kind != ISAAC_VITA_IMPORT_BASELINE)
        ++id;
    if (id >= ISAAC_VITA_IMPORT_ID_COUNT)
        return;
    target = oracle_slot_va(id);
    memset(&cpu, 0, sizeof cpu);
    cpu.run_scope = &scope;
    if (setjmp(scope.env) != 0) {
        printf("bench: unexpected fault\n");
        return;
    }
    started = clock();
    for (i = 0U; i < ITERATIONS; ++i)
        guest_call(&cpu, target);
    slow_ns = (double)(clock() - started) * 1e9 / CLOCKS_PER_SEC / ITERATIONS;
    started = clock();
    for (i = 0U; i < ITERATIONS; ++i)
        guest_import_call(&cpu, target, id);
    direct_ns = (double)(clock() - started) * 1e9 / CLOCKS_PER_SEC /
                ITERATIONS;
    printf("host bench (this CPU, indicative only): guest_call %.1f ns, "
           "guest_import_call %.1f ns per import call (%.2fx)\n",
           slow_ns, direct_ns, direct_ns > 0.0 ? slow_ns / direct_ns : 0.0);
}

int main(void)
{
    uint32_t id;

    oracle_prepare_family_names();
    for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; ++id) {
        s_imports[id].slot_rva = s_contract[id].slot_rva;
        s_imports[id].name = s_contract[id].name;
    }
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = UINT32_C(0x840000);
    g_guest_coverage_imports = s_coverage;

    if (oracle_test_before_registration())
        return 1;
    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    if (oracle_check_table(1, "direct table after exact registration"))
        return 1;
    if (oracle_test_all_ids() || oracle_test_ids_out_of_range() ||
        oracle_test_rejecting_families() || oracle_test_registration_refused())
        return 1;
    oracle_bench();
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    printf("Vita IAT direct dispatch oracle: per-import census compared on "
           "every differential run\n");
#endif
    printf("Vita IAT direct dispatch oracle: PASS (%u differential runs; "
           "%u rows; audio=%d lua=%d lua_fast=%d)\n", s_differential_runs,
           (unsigned)ISAAC_VITA_IMPORT_ID_COUNT, (int)ISAAC_VITA_AUDIO,
           (int)ISAAC_VITA_LUA, (int)ORACLE_LUA_FAST);
    return 0;
}
