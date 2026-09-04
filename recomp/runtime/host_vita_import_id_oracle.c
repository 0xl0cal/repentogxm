/* Host-runnable exhaustive oracle and dispatch-only microbenchmark.
 * This deliberately stubs guest handlers: it tests the 413-way ID router,
 * registration fail-closed rules, counter ownership and local-name guard. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "host_vita_import_id.h"

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif
#ifndef ISAAC_VITA_XINPUT
#define ISAAC_VITA_XINPUT 0
#endif
#ifndef ISAAC_VITA_LUA
#define ISAAC_VITA_LUA 0
#endif

typedef struct oracle_import_entry {
    guest_import imported;
    uint8_t kind;
    uint8_t local_index;
} oracle_import_entry;

#define ISAAC_VITA_IMPORT_ID_ROW(id, slot, import_name, binding_kind, local) \
    { { (slot), (import_name) }, (uint8_t)(binding_kind), (uint8_t)(local) },
static const oracle_import_entry s_contract[] = {
#include "host_vita_import_id_map.inc"
};
#undef ISAAC_VITA_IMPORT_ID_ROW

#define ISAAC_VITA_IMPORT_ID_ROW(id, slot, import_name, binding_kind, local) \
    { (slot), (import_name) },
static guest_import s_inputs[] = {
#include "host_vita_import_id_map.inc"
};
#undef ISAAC_VITA_IMPORT_ID_ROW

unsigned g_host_import_calls;

static uint32_t s_last_kind;
static uint32_t s_last_index;
static unsigned s_handler_calls;
static unsigned s_fread_notes;
static int s_name_drift;
static int s_inventory_tail_drift;
static uint32_t s_drift_kind;
static uint32_t s_drift_index;
#define ORACLE_MAX_FAMILY_IMPORTS 67U
static const char *s_family_names[ISAAC_VITA_IMPORT_SHARED_LOADER + 1U]
                                 [ORACLE_MAX_FAMILY_IMPORTS];

static void oracle_prepare_family_names(void)
{
    uint32_t i;

    memset(s_family_names, 0, sizeof s_family_names);
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
            s_family_names[kind][index] = s_contract[i].imported.name;
    }
    s_family_names[ISAAC_VITA_IMPORT_SYNC][2U] =
        "KERNEL32.dll!GetProcAddress";
    s_family_names[ISAAC_VITA_IMPORT_STARTUP][0U] =
        "KERNEL32.dll!FreeLibrary";
    s_family_names[ISAAC_VITA_IMPORT_STARTUP][4U] =
        "KERNEL32.dll!LoadLibraryA";
}

static int oracle_fail(const char *what, uint32_t id)
{
    fprintf(stderr, "FAIL id=%u: %s\n", (unsigned)id, what);
    return 1;
}

static const char *oracle_family_name(uint32_t kind, uint32_t index)
{
    if (s_inventory_tail_drift && kind == ISAAC_VITA_IMPORT_BASELINE &&
        index == 5U)
        return s_contract[0].imported.name;
    if (s_name_drift && kind == s_drift_kind && index == s_drift_index)
        return "oracle!deliberate-local-name-drift";
    if (kind >= ISAAC_VITA_IMPORT_SHARED_LOADER + 1U ||
        index >= ORACLE_MAX_FAMILY_IMPORTS)
        return NULL;
    return s_family_names[kind][index];
}

static int oracle_handler(uint32_t kind, CPU *__restrict c, uint32_t index,
                          unsigned *call_count)
{
    (void)c;
    s_last_kind = kind;
    s_last_index = index;
    ++s_handler_calls;
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

void isaac_vita_note_archive_fread(void)
{
    s_last_kind = ISAAC_VITA_IMPORT_FREAD;
    ++s_fread_notes;
}

int isaac_vita_shared_loader_import_indexed(CPU *__restrict c,
                                            uint32_t shared_index)
{
    return oracle_handler(ISAAC_VITA_IMPORT_SHARED_LOADER, c, shared_index,
                          &g_host_import_calls);
}

static int oracle_expect_registration_failure(const char *what)
{
    CPU cpu = { 0 };

    if (guest_host_import_ids_register(s_inputs,
                                       ISAAC_VITA_IMPORT_ID_COUNT)) {
        fprintf(stderr, "FAIL: invalid registration accepted: %s\n", what);
        return 1;
    }
    if (guest_host_import_id(&cpu, 0U) >= 0) {
        fprintf(stderr, "FAIL: failed registration stayed dispatch-ready: %s\n",
                what);
        return 1;
    }
    return 0;
}

static int oracle_test_registration_failures(void)
{
    const char *saved_name;
    uint32_t saved_slot;

    if (guest_host_import_ids_register(NULL, ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail("null table accepted", 0U);
    if (guest_host_import_ids_register(s_inputs,
                                       ISAAC_VITA_IMPORT_ID_COUNT - 1U))
        return oracle_fail("short table accepted", 0U);
    if (guest_host_import_ids_register(s_inputs,
                                       ISAAC_VITA_IMPORT_ID_COUNT + 1U))
        return oracle_fail("long table accepted", 0U);

    saved_slot = s_inputs[1].slot_rva;
    s_inputs[1].slot_rva = s_inputs[0].slot_rva;
    if (oracle_expect_registration_failure("duplicate slot"))
        return 1;
    s_inputs[1].slot_rva = saved_slot;

    saved_slot = s_inputs[2].slot_rva;
    s_inputs[2].slot_rva = s_inputs[0].slot_rva - 4U;
    if (oracle_expect_registration_failure("out-of-order slot"))
        return 1;
    s_inputs[2].slot_rva = saved_slot;

    saved_name = s_inputs[1].name;
    s_inputs[1].name = s_inputs[0].name;
    if (oracle_expect_registration_failure("duplicate name"))
        return 1;
    s_inputs[1].name = saved_name;

    saved_name = s_inputs[2].name;
    s_inputs[2].name = "oracle!non-exact-name";
    if (oracle_expect_registration_failure("non-exact name"))
        return 1;
    s_inputs[2].name = saved_name;

    s_name_drift = 1;
    s_drift_kind = ISAAC_VITA_IMPORT_BASELINE;
    s_drift_index = 0U;
    if (oracle_expect_registration_failure("local index/name drift"))
        return 1;
    s_name_drift = 0;

    s_inventory_tail_drift = 1;
    if (oracle_expect_registration_failure("family inventory tail drift"))
        return 1;
    s_inventory_tail_drift = 0;

    if (!guest_host_import_ids_register(s_inputs,
                                        ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail("exact table did not recover after failures", 0U);
    if (!guest_host_import_ids_register(s_inputs,
                                        ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail("repeated exact registration failed", 0U);
    return 0;
}

static int oracle_test_all_ids(void)
{
    CPU cpu = { 0 };
    uint32_t id;

    if (guest_host_import_id(&cpu, 0U) >= 0)
        return oracle_fail("dispatch was ready before registration", 0U);
    if (!guest_host_import_ids_register(s_inputs,
                                        ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail(guest_host_import_id_error(), 0U);

    for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; ++id) {
        const oracle_import_entry *entry = &s_contract[id];
        int handled;

        g_host_import_calls = 0U;
        s_handler_calls = 0U;
        s_fread_notes = 0U;
        s_last_kind = UINT32_MAX;
        s_last_index = UINT32_MAX;
        handled = guest_host_import_id(&cpu, id);
        if (entry->kind == ISAAC_VITA_IMPORT_UNRESOLVED ||
            (!ISAAC_VITA_AUDIO &&
             entry->kind == ISAAC_VITA_IMPORT_AUDIO) ||
            (!ISAAC_VITA_LUA &&
             entry->kind == ISAAC_VITA_IMPORT_LUA)) {
            if (handled != 0 || g_host_import_calls != 0U ||
                s_handler_calls != 0U)
                return oracle_fail("unresolved ID was claimed or counted", id);
            continue;
        }
        if (handled != 1)
            return oracle_fail("handled ID was rejected", id);
        if (g_host_import_calls != 1U || s_handler_calls != 1U)
            return oracle_fail("handled ID did not increment exactly once", id);
        if (s_last_kind != entry->kind ||
            s_last_index != entry->local_index)
            return oracle_fail("ID routed to the wrong binding/index", id);
        if ((entry->kind == ISAAC_VITA_IMPORT_FREAD) !=
            (s_fread_notes == 1U))
            return oracle_fail("fread cadence side effect drifted", id);
    }

    g_host_import_calls = 0U;
    if (guest_host_import_id(&cpu, ISAAC_VITA_IMPORT_ID_COUNT) >= 0 ||
        g_host_import_calls != 0U)
        return oracle_fail("ID 413 was not rejected", 413U);
    if (!guest_host_import_ids_register(s_inputs,
                                        ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail("registration did not recover after ID 413", 413U);
    if (guest_host_import_id(&cpu, UINT32_MAX) >= 0 ||
        g_host_import_calls != 0U)
        return oracle_fail("UINT32_MAX ID was not rejected", UINT32_MAX);
    return 0;
}

static int oracle_family_has(uint32_t kind, uint32_t count, const char *name)
{
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        const char *candidate = oracle_family_name(kind, index);
        if (candidate && strcmp(name, candidate) == 0)
            return 1;
    }
    return 0;
}

static int oracle_legacy_name_dispatch(const char *name)
{
    static const struct oracle_family_inventory {
        uint8_t kind;
        uint8_t count;
    } before_loader[] = {
        { ISAAC_VITA_IMPORT_BASELINE, 5U },
        { ISAAC_VITA_IMPORT_CRT, 60U },
        { ISAAC_VITA_IMPORT_MATH, 21U },
#if ISAAC_VITA_LUA
        { ISAAC_VITA_IMPORT_LUA, 67U },
#endif
        { ISAAC_VITA_IMPORT_RTTI, 1U },
        { ISAAC_VITA_IMPORT_EXCEPTION, 2U },
        { ISAAC_VITA_IMPORT_FILE_LOCK, 2U },
        { ISAAC_VITA_IMPORT_FILESYSTEM, 2U },
        { ISAAC_VITA_IMPORT_FIND, 3U },
        { ISAAC_VITA_IMPORT_COM, 3U },
        { ISAAC_VITA_IMPORT_POST_COM, 6U },
        { ISAAC_VITA_IMPORT_HEAP, 6U },
        { ISAAC_VITA_IMPORT_STEAM, 7U },
#if ISAAC_VITA_AUDIO
        { ISAAC_VITA_IMPORT_AUDIO, 24U },
#endif
    };
    static const struct oracle_family_inventory after_loader[] = {
        { ISAAC_VITA_IMPORT_SYNC, 14U },
        { ISAAC_VITA_IMPORT_MEMORY, 6U },
        { ISAAC_VITA_IMPORT_CONSOLE, 2U },
        { ISAAC_VITA_IMPORT_USER32, 6U },
        { ISAAC_VITA_IMPORT_STARTUP, 11U },
        { ISAAC_VITA_IMPORT_FLS, 3U },
    };
    static const char *const loader_names[] = {
        "KERNEL32.dll!LoadLibraryA",
        "KERNEL32.dll!GetProcAddress",
        "KERNEL32.dll!FreeLibrary",
        "OPENGL32.dll!wglGetProcAddress"
    };
    size_t family;
    size_t index;

    /* The real name wrapper pins fread before the baseline/family chain. */
    if (strcmp(name, "api-ms-win-crt-stdio-l1-1-0.dll!fread") == 0)
        return 1;
    for (family = 0U;
         family < sizeof before_loader / sizeof before_loader[0]; ++family) {
        if (oracle_family_has(before_loader[family].kind,
                              before_loader[family].count, name))
            return 1;
    }
#if ISAAC_VITA_XINPUT
    for (index = 0U; index < 3U; ++index) {
        if (strcmp(name, loader_names[index]) == 0)
            return 1;
    }
#endif
    for (index = 0U; index < sizeof loader_names / sizeof loader_names[0];
         ++index) {
        if (strcmp(name, loader_names[index]) == 0)
            return 1;
    }
    for (family = 0U;
         family < sizeof after_loader / sizeof after_loader[0]; ++family) {
        if (oracle_family_has(after_loader[family].kind,
                              after_loader[family].count, name))
            return 1;
    }
    return 0;
}

static int oracle_benchmark(void)
{
    enum { iterations = 200000 };
    CPU cpu = { 0 };
    volatile unsigned id_sum = 0U;
    volatile unsigned name_sum = 0U;
    clock_t started;
    clock_t id_ticks;
    clock_t name_ticks;
    unsigned i;

    if (!guest_host_import_ids_register(s_inputs,
                                        ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail("benchmark registration failed", 0U);
    started = clock();
    for (i = 0U; i < iterations; ++i) {
        uint32_t id = (uint32_t)(((uint64_t)i * 193U) %
                                 ISAAC_VITA_IMPORT_ID_COUNT);
        id_sum += guest_host_import_id(&cpu, id) > 0;
    }
    id_ticks = clock() - started;

    started = clock();
    for (i = 0U; i < iterations; ++i) {
        uint32_t id = (uint32_t)(((uint64_t)i * 193U) %
                                 ISAAC_VITA_IMPORT_ID_COUNT);
        name_sum += oracle_legacy_name_dispatch(
            s_contract[id].imported.name);
    }
    name_ticks = clock() - started;
    if (id_sum != name_sum)
        return oracle_fail("benchmark paths disagreed", 0U);

    printf("dispatch-only host microbenchmark (%u calls): "
           "dense-ID %.1f ns/call; synthetic old family/name chain "
           "%.1f ns/call",
           iterations,
           (double)id_ticks * 1000000000.0 /
               ((double)CLOCKS_PER_SEC * iterations),
           (double)name_ticks * 1000000000.0 /
               ((double)CLOCKS_PER_SEC * iterations));
    if (id_ticks != 0)
        printf("; scan/ID %.2fx", (double)name_ticks / (double)id_ticks);
    putchar('\n');
    return 0;
}

#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
/* The import-kinds snapshot must sum a per-ID census array per binding kind
 * to the contract's kind population, pin the fourteen hot imports by name,
 * ignore IDs beyond the supplied count and tolerate a NULL array.  (The
 * census increments themselves live in guest.c -- GUEST_PHASE_PROFILE_NOTE_IMPORT
 * on guest.h's array -- and are driven by guest_sampler_word_oracle.c and
 * kage_vita_phase_profile_oracle.c.) */
static int oracle_test_import_kinds(void)
{
    IsaacVitaImportKindSnapshot snapshot;
    uint32_t calls[ISAAC_VITA_IMPORT_ID_COUNT];
    uint32_t expected_kind[ISAAC_VITA_IMPORT_KIND_COUNT] = { 0 };
    uint32_t expected_total = 0U;
    static const struct { uint32_t hot; uint32_t id; } pins[] = {
        { ISAAC_VITA_IMPORT_HOT_ENTER_CS, 61U },
        { ISAAC_VITA_IMPORT_HOT_LEAVE_CS, 60U },
        { ISAAC_VITA_IMPORT_HOT_TRY_ENTER_CS, 62U },
        { ISAAC_VITA_IMPORT_HOT_SLEEP, 76U },
        { ISAAC_VITA_IMPORT_HOT_MALLOC, 307U },
        { ISAAC_VITA_IMPORT_HOT_FREE, 305U },
        { ISAAC_VITA_IMPORT_HOT_CALLOC, 306U },
        { ISAAC_VITA_IMPORT_HOT_REALLOC, 309U },
        { ISAAC_VITA_IMPORT_HOT_MEMCPY, 281U },
        { ISAAC_VITA_IMPORT_HOT_MEMSET, 280U },
        { ISAAC_VITA_IMPORT_HOT_MEMMOVE, 284U },
        { ISAAC_VITA_IMPORT_HOT_TIME_GET_TIME, 294U },
        { ISAAC_VITA_IMPORT_HOT_QPC, 47U },
        { ISAAC_VITA_IMPORT_HOT_FLOOR, 313U },
    };
    static const char *const pin_names[] = {
        "KERNEL32.dll!EnterCriticalSection",
        "KERNEL32.dll!LeaveCriticalSection",
        "KERNEL32.dll!TryEnterCriticalSection",
        "KERNEL32.dll!Sleep",
        "api-ms-win-crt-heap-l1-1-0.dll!malloc",
        "api-ms-win-crt-heap-l1-1-0.dll!free",
        "api-ms-win-crt-heap-l1-1-0.dll!calloc",
        "api-ms-win-crt-heap-l1-1-0.dll!realloc",
        "VCRUNTIME140.dll!memcpy",
        "VCRUNTIME140.dll!memset",
        "VCRUNTIME140.dll!memmove",
        "WINMM.dll!timeGetTime",
        "KERNEL32.dll!QueryPerformanceCounter",
        "api-ms-win-crt-math-l1-1-0.dll!floor",
    };
    uint32_t id;
    uint32_t i;

    if (!guest_host_import_ids_register(s_inputs,
                                        ISAAC_VITA_IMPORT_ID_COUNT))
        return oracle_fail(guest_host_import_id_error(), 0U);
    /* Every ID `id % 5` times (id 0 never), including unresolved rows: the
     * census counts guest_call's import classification, so a rejected
     * unresolved row still counts -- exactly like g(c) counted its call. */
    for (id = 0U; id < ISAAC_VITA_IMPORT_ID_COUNT; ++id) {
        calls[id] = id % 5U;
        expected_total += id % 5U;
        expected_kind[s_contract[id].kind] += id % 5U;
    }
    isaac_vita_import_id_calls_snapshot(calls, ISAAC_VITA_IMPORT_ID_COUNT,
                                        &snapshot);
    if (snapshot.total != expected_total)
        return oracle_fail("import census total drifted", snapshot.total);
    for (i = 0U; i < ISAAC_VITA_IMPORT_KIND_COUNT; ++i)
        if (snapshot.kind[i] != expected_kind[i])
            return oracle_fail("import census kind sum drifted", i);
    if (!snapshot.hot_ok)
        return oracle_fail("hot import pins do not match the frozen table",
                           0U);
    for (i = 0U; i < sizeof pins / sizeof pins[0]; ++i) {
        if (strcmp(s_contract[pins[i].id].imported.name, pin_names[i]) != 0)
            return oracle_fail("hot pin name/ID pair drifted", pins[i].id);
        if (snapshot.hot[pins[i].hot] != pins[i].id % 5U)
            return oracle_fail("hot import count drifted", pins[i].id);
    }
    /* A shorter array stops at its count: IDs 61 (Enter) and 60 (Leave) are
     * still read, 313 (floor) is not; a count beyond the table is clamped;
     * a NULL array yields the zero snapshot with hot_ok=0. */
    isaac_vita_import_id_calls_snapshot(calls, 62U, &snapshot);
    if (snapshot.hot[ISAAC_VITA_IMPORT_HOT_ENTER_CS] != 61U % 5U ||
            snapshot.hot[ISAAC_VITA_IMPORT_HOT_LEAVE_CS] != 60U % 5U ||
            snapshot.hot[ISAAC_VITA_IMPORT_HOT_FLOOR] != 0U ||
            !snapshot.hot_ok)
        return oracle_fail("short census array was not bounded", snapshot.total);
    for (id = 62U; id < ISAAC_VITA_IMPORT_ID_COUNT; ++id)
        expected_total -= id % 5U;
    if (snapshot.total != expected_total)
        return oracle_fail("short census total drifted", snapshot.total);
    isaac_vita_import_id_calls_snapshot(calls, UINT32_MAX, &snapshot);
    if (snapshot.hot[ISAAC_VITA_IMPORT_HOT_FLOOR] != 313U % 5U)
        return oracle_fail("over-long census count was not clamped",
                           snapshot.total);
    isaac_vita_import_id_calls_snapshot(NULL, ISAAC_VITA_IMPORT_ID_COUNT,
                                        &snapshot);
    if (snapshot.total != 0U || snapshot.hot_ok != 0U)
        return oracle_fail("NULL census array was not rejected", snapshot.total);
    return 0;
}
#endif

int main(void)
{
    oracle_prepare_family_names();
    if (sizeof s_contract / sizeof s_contract[0] !=
        ISAAC_VITA_IMPORT_ID_COUNT ||
        sizeof s_inputs / sizeof s_inputs[0] !=
        ISAAC_VITA_IMPORT_ID_COUNT)
        return oracle_fail("compiled contract count is not 413", 0U);
    if (oracle_test_all_ids() || oracle_test_registration_failures() ||
        oracle_benchmark())
        return 1;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    if (oracle_test_import_kinds())
        return 1;
    puts("PASS: per-ID import census (kinds, 14 hot pins, bounds)");
#endif
    puts("PASS: all 413 dense IDs, registration failures, exact counting, "
         "fread cadence and local binding drift");
    return 0;
}
