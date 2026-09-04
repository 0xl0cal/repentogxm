/* White-box host oracle for the opt-in guest dispatch accelerators.
 * Include the runtime in this translation unit so relocated-VA normalization
 * can be exercised without mapping the copyrighted PE merely to set its
 * private image bounds. */
#include "guest.c"

/* Referenced by guest_note_authenticated_import_call, which the linker no
 * longer discards from this white-box unit (MSVC 14.44 /OPT:REF); the real
 * definition lives in the platform host. */
unsigned g_host_import_calls;

#if defined(ISAAC_VITA_GUEST_LOOKUP_CACHE)

#define ORACLE_FUNCTION_COUNT 14868U
#define ORACLE_FIRST_RVA 0x00001000U
#define ORACLE_RVA_STRIDE 0x00000180U
#define ORACLE_IMAGE_SIZE 0x00840000U
#define ORACLE_IMPORT_COUNT 413U
#define ORACLE_IMPORT_SLOT_COUNT 438U
#define ORACLE_IMPORT_BASE 0x00606000U

static uint32_t s_addresses[ORACLE_FUNCTION_COUNT];
static guest_fn s_functions[ORACLE_FUNCTION_COUNT];
static guest_fn s_replacement_functions[ORACLE_FUNCTION_COUNT];
static guest_import s_imports[ORACLE_IMPORT_COUNT];
static guest_import s_replacement_imports[ORACLE_IMPORT_COUNT];
static uint32_t s_function_calls[4];
static uint32_t s_import_calls;
static uint32_t s_dynamic_calls;
static int s_handle_dynamic;

static void oracle_function_0(CPU *__restrict cpu) { (void)cpu; ++s_function_calls[0]; }
static void oracle_function_1(CPU *__restrict cpu) { (void)cpu; ++s_function_calls[1]; }
static void oracle_function_2(CPU *__restrict cpu) { (void)cpu; ++s_function_calls[2]; }
static void oracle_function_3(CPU *__restrict cpu) { (void)cpu; ++s_function_calls[3]; }

static const guest_fn s_function_cycle[] = {
    oracle_function_0, oracle_function_1,
    oracle_function_2, oracle_function_3
};

/* Logical census as the profiler prints it.  With the dispatch table a hit
 * is one call and one lookup (the successor of a confirmed dispatch-cache
 * hit) but guest_call pays a single dispatch_calls increment. */
static uint32_t oracle_calls(void)
{
    return g_guest_phase_profile_counters.guest_calls
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        + g_guest_phase_profile_counters.dispatch_calls
#endif
        ;
}

static uint32_t oracle_lookups(void)
{
    return g_guest_phase_profile_counters.guest_lookups
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        + (g_guest_phase_profile_counters.dispatch_calls -
           g_guest_phase_profile_counters.dispatch_slow)
#endif
        ;
}

static uint32_t oracle_cache_hits(void)
{
    return g_guest_lookup_cache_hits
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
        + (g_guest_phase_profile_counters.dispatch_calls -
           g_guest_phase_profile_counters.dispatch_slow)
#endif
        ;
}

static uint32_t function_call_count(guest_fn function)
{
    uint32_t i;

    for (i = 0U; i < 4U; ++i) {
        if (s_function_cycle[i] == function)
            return s_function_calls[i];
    }
    return UINT32_MAX;
}

int guest_host_import(CPU *__restrict cpu, const char *name)
{
    (void)cpu;
    ++s_import_calls;
    return strcmp(name, "dispatch-overlap") == 0;
}

int guest_host_dynamic(CPU *__restrict cpu, uint32_t token)
{
    (void)cpu;
    ++s_dynamic_calls;
    return s_handle_dynamic && token == UINT32_C(0x7d100001);
}

static int fail(const char *message, uint32_t index)
{
    fprintf(stderr, "guest lookup cache oracle: FAIL: %s (%u)\n",
            message, index);
    return 1;
}

static void prepare_functions(void)
{
    uint32_t i;

    for (i = 0U; i < ORACLE_FUNCTION_COUNT; ++i) {
        s_addresses[i] = ORACLE_FIRST_RVA + i * ORACLE_RVA_STRIDE;
        s_functions[i] = s_function_cycle[i & 3U];
        s_replacement_functions[i] = s_function_cycle[(i + 1U) & 3U];
    }
}

static int verify_all_functions(const guest_fn *expected)
{
    uint32_t i;

    for (i = 0U; i < ORACLE_FUNCTION_COUNT; ++i) {
        guest_fn by_rva = guest_lookup(s_addresses[i]);
        guest_fn by_va = guest_lookup(GUEST_IMAGE_BASE + s_addresses[i]);

        if (by_rva != expected[i] || by_va != expected[i] ||
            by_rva != by_va)
            return fail("RVA/relocated-VA function mismatch", i);
    }
    return 0;
}

static int verify_unknown_functions(void)
{
    uint32_t unknown = ORACLE_FIRST_RVA - 4U;

    if (guest_lookup(unknown) != NULL ||
        guest_lookup(GUEST_IMAGE_BASE + unknown) != NULL)
        return fail("unknown function was cached or resolved", unknown);
    return 0;
}

static int verify_function_collision(void)
{
    uint32_t first_by_bucket[GUEST_LOOKUP_CACHE_COUNT];
    uint32_t first = UINT32_MAX, second = UINT32_MAX;
    uint32_t hits_before, misses_before, i;

    for (i = 0U; i < GUEST_LOOKUP_CACHE_COUNT; ++i)
        first_by_bucket[i] = UINT32_MAX;
    for (i = 0U; i < ORACLE_FUNCTION_COUNT; ++i) {
        uint32_t bucket = guest_lookup_cache_index(s_addresses[i]);

        if (first_by_bucket[bucket] != UINT32_MAX) {
            first = first_by_bucket[bucket];
            second = i;
            break;
        }
        first_by_bucket[bucket] = i;
    }
    if (first == UINT32_MAX || second == UINT32_MAX)
        return fail("no deliberate direct-map collision found", 0U);
    s_functions[first] = oracle_function_0;
    s_functions[second] = oracle_function_1;
    s_replacement_functions[first] = oracle_function_2;

    guest_register(s_addresses, s_functions, ORACLE_FUNCTION_COUNT);
    hits_before = g_guest_lookup_cache_hits;
    misses_before = g_guest_lookup_cache_misses;
    if (guest_lookup(s_addresses[first]) != s_functions[first] ||
        guest_lookup(s_addresses[first]) != s_functions[first] ||
        guest_lookup(s_addresses[second]) != s_functions[second] ||
        guest_lookup(s_addresses[second]) != s_functions[second] ||
        guest_lookup(s_addresses[first]) != s_functions[first] ||
        guest_lookup(s_addresses[first]) != s_functions[first])
        return fail("collision fallback returned the wrong function", second);
    if (g_guest_lookup_cache_hits - hits_before != 3U ||
        g_guest_lookup_cache_misses - misses_before != 3U)
        return fail("collision hit/miss census is wrong", second);

    /* Leave the old function hot, then replace every value behind the same
     * keys.  The first lookup must miss after guest_register's cache reset;
     * otherwise an exact key can still return a stale function pointer. */
    guest_register(s_addresses, s_replacement_functions,
                   ORACLE_FUNCTION_COUNT);
    hits_before = g_guest_lookup_cache_hits;
    misses_before = g_guest_lookup_cache_misses;
    if (guest_lookup(s_addresses[first]) != s_replacement_functions[first] ||
        guest_lookup(s_addresses[first]) != s_replacement_functions[first])
        return fail("re-registration returned a stale function", first);
    if (g_guest_lookup_cache_hits - hits_before != 1U ||
        g_guest_lookup_cache_misses - misses_before != 1U)
        return fail("re-registration did not cold-reset the cache", first);
    return 0;
}

static int verify_dispatch_fastpath(void)
{
    static const uint32_t dynamic_address[] = { UINT32_C(0x7d100001) };
    static const guest_fn dynamic_function[] = { oracle_function_3 };
    CPU cpu;
    guest_import overlap;
    uint32_t target = s_addresses[101U];
    guest_fn replacement = s_replacement_functions[101U];
    uint32_t calls_before, replacement_calls_before;
    uint32_t hits_before, misses_before, guest_calls_before;
    uint32_t lookups_before, dynamic_before;

    memset(&cpu, 0, sizeof cpu);
    memset(s_function_calls, 0, sizeof s_function_calls);
    s_import_calls = 0U;
    s_dynamic_calls = 0U;
    s_handle_dynamic = 0;

    guest_register(s_addresses, s_replacement_functions,
                   ORACLE_FUNCTION_COUNT);
    guest_register_imports(NULL, 0U);
    hits_before = oracle_cache_hits();
    misses_before = g_guest_lookup_cache_misses;
    guest_calls_before = oracle_calls();
    lookups_before = oracle_lookups();
    guest_call(&cpu, target);
    guest_call(&cpu, GUEST_IMAGE_BASE + target);
    if (function_call_count(replacement) != 2U ||
        s_dynamic_calls != 1U ||
        oracle_cache_hits() - hits_before != 1U ||
        g_guest_lookup_cache_misses - misses_before != 1U ||
        oracle_calls() - guest_calls_before != 2U ||
        oracle_lookups() - lookups_before != 2U)
        return fail("confirmed dispatch hit did not bypass classification", target);

    /* An IAT registration after a translated call is allowed to claim the
     * same number.  It must invalidate dispatch authorization and retain the
     * original import-before-function precedence. */
    overlap.slot_rva = target;
    overlap.name = "dispatch-overlap";
    calls_before = function_call_count(replacement);
    guest_register_imports(&overlap, 1U);
    guest_call(&cpu, target);
    if (s_import_calls != 1U ||
        function_call_count(replacement) != calls_before)
        return fail("IAT registration did not invalidate dispatch hit", target);

    /* Removing that import leaves the ordinary lookup cache usable, but the
     * dispatch edge must be classified once again before it can bypass. */
    dynamic_before = s_dynamic_calls;
    guest_register_imports(NULL, 0U);
    guest_call(&cpu, target);
    guest_call(&cpu, target);
    if (s_dynamic_calls - dynamic_before != 1U ||
        function_call_count(replacement) != calls_before + 2U)
        return fail("dispatch edge did not re-authorize after IAT change", target);

    /* Dynamic families always keep priority, even if the first call rejected
     * the token and therefore reached a deliberately overlapping function. */
    guest_register(dynamic_address, dynamic_function, 1U);
    guest_register_imports(NULL, 0U);
    calls_before = s_function_calls[3];
    s_handle_dynamic = 0;
    guest_call(&cpu, dynamic_address[0]);
    s_handle_dynamic = 1;
    guest_call(&cpu, dynamic_address[0]);
    if (s_function_calls[3] != calls_before + 1U)
        return fail("dynamic token lost priority to dispatch hit", 0U);

    /* Function-table replacement must cold-reset both lookup and dispatch. */
    s_handle_dynamic = 0;
    guest_register(s_addresses, s_functions, ORACLE_FUNCTION_COUNT);
    guest_register_imports(NULL, 0U);
    guest_call(&cpu, target);
    calls_before = function_call_count(s_functions[101U]);
    replacement_calls_before = function_call_count(
        s_replacement_functions[101U]);
    guest_register(s_addresses, s_replacement_functions,
                   ORACLE_FUNCTION_COUNT);
    guest_call(&cpu, target);
    if (function_call_count(s_functions[101U]) != calls_before ||
        function_call_count(s_replacement_functions[101U]) !=
            replacement_calls_before + 1U)
        return fail("re-registration retained stale dispatch function", target);

    return 0;
}

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
static int verify_dispatch_table(void)
{
    guest_dispatch_table_status status;
    CPU cpu;
    guest_import overlap;
    uint32_t i, slow_before, calls_before, dynamic_before;
    uint32_t hits_before, lookups_before, misses_before;

    memset(&cpu, 0, sizeof cpu);
    guest_register(s_addresses, s_functions, ORACLE_FUNCTION_COUNT);
    guest_register_imports(NULL, 0U);
    guest_dispatch_table_get_status(&status);
    if (!status.ready || status.keys != ORACLE_FUNCTION_COUNT ||
        status.skipped != 0U || status.reason != NULL)
        return fail("dispatch table did not cover every function",
                    status.keys);

    /* Every relocated VA is a table hit: correct function, no slow entry,
     * no dynamic probe, and the derived census equals the historical one. */
    memset(s_function_calls, 0, sizeof s_function_calls);
    s_dynamic_calls = 0U;
    slow_before = g_guest_phase_profile_counters.dispatch_slow;
    calls_before = oracle_calls();
    lookups_before = oracle_lookups();
    hits_before = oracle_cache_hits();
    misses_before = g_guest_lookup_cache_misses;
    for (i = 0U; i < ORACLE_FUNCTION_COUNT; ++i)
        guest_call(&cpu, GUEST_IMAGE_BASE + s_addresses[i]);
    for (i = 0U; i < 4U; ++i) {
        /* verify_function_collision re-points two entries, so derive the
         * expected census from the registered table itself. */
        uint32_t expected = 0U, j;

        for (j = 0U; j < ORACLE_FUNCTION_COUNT; ++j)
            expected += s_functions[j] == s_function_cycle[i];
        if (s_function_calls[i] != expected)
            return fail("dispatch table reached the wrong function", i);
    }
    if (g_guest_phase_profile_counters.dispatch_slow != slow_before ||
        s_dynamic_calls != 0U ||
        oracle_calls() - calls_before != ORACLE_FUNCTION_COUNT ||
        oracle_lookups() - lookups_before != ORACLE_FUNCTION_COUNT ||
        oracle_cache_hits() - hits_before != ORACLE_FUNCTION_COUNT ||
        g_guest_lookup_cache_misses != misses_before)
        return fail("dispatch table hit census is wrong", 0U);

    /* RVA-form keys are not table keys: they take the complete path. */
    slow_before = g_guest_phase_profile_counters.dispatch_slow;
    dynamic_before = s_dynamic_calls;
    guest_call(&cpu, s_addresses[7U]);
    if (g_guest_phase_profile_counters.dispatch_slow != slow_before + 1U ||
        s_dynamic_calls != dynamic_before + 1U ||
        function_call_count(s_functions[7U]) !=
            ORACLE_FUNCTION_COUNT / 4U + 1U +
            (7U < ORACLE_FUNCTION_COUNT % 4U ? 1U : 0U))
        return fail("RVA-form key bypassed the complete path", 7U);

    /* An import that claims a function's RVA removes that key only. */
    overlap.slot_rva = s_addresses[9U];
    overlap.name = "dispatch-overlap";
    guest_register_imports(&overlap, 1U);
    guest_dispatch_table_get_status(&status);
    if (!status.ready || status.keys != ORACLE_FUNCTION_COUNT - 1U ||
        status.skipped != 1U)
        return fail("import precedence did not skip exactly one key",
                    status.keys);
    slow_before = g_guest_phase_profile_counters.dispatch_slow;
    s_import_calls = 0U;
    calls_before = function_call_count(s_functions[9U]);
    guest_call(&cpu, GUEST_IMAGE_BASE + s_addresses[9U]);
    if (g_guest_phase_profile_counters.dispatch_slow != slow_before + 1U ||
        s_import_calls != 1U ||
        function_call_count(s_functions[9U]) != calls_before)
        return fail("import lost precedence to the dispatch table", 9U);
    guest_call(&cpu, GUEST_IMAGE_BASE + s_addresses[10U]);
    if (g_guest_phase_profile_counters.dispatch_slow != slow_before + 1U)
        return fail("neighbour of the claimed key left the table", 10U);
    guest_register_imports(NULL, 0U);
    guest_dispatch_table_get_status(&status);
    if (!status.ready || status.keys != ORACLE_FUNCTION_COUNT)
        return fail("dispatch table did not restore the reclaimed key",
                    status.keys);

    /* Unknown keys, the empty key, and native token families all miss. */
    slow_before = g_guest_phase_profile_counters.dispatch_slow;
    s_handle_dynamic = 1;
    guest_call(&cpu, UINT32_C(0x7d100001));
    s_handle_dynamic = 0;
    if (g_guest_phase_profile_counters.dispatch_slow != slow_before + 1U)
        return fail("dynamic token bypassed the complete path", 0U);

    /* Replacing the function table replaces every slot. */
    guest_register(s_addresses, s_replacement_functions,
                   ORACLE_FUNCTION_COUNT);
    calls_before = function_call_count(s_replacement_functions[3U]);
    slow_before = g_guest_phase_profile_counters.dispatch_slow;
    guest_call(&cpu, GUEST_IMAGE_BASE + s_addresses[3U]);
    if (function_call_count(s_replacement_functions[3U]) != calls_before + 1U ||
        g_guest_phase_profile_counters.dispatch_slow != slow_before)
        return fail("re-registration left a stale table slot", 3U);

    /* Without a mapped image no VA can normalise: the table stays empty and
     * fails closed. */
    g_image_base = 0U;
    guest_register(s_addresses, s_replacement_functions,
                   ORACLE_FUNCTION_COUNT);
    guest_dispatch_table_get_status(&status);
    if (status.ready || status.keys != 0U || status.reason == NULL)
        return fail("dispatch table built without an image base", 0U);
    g_image_base = GUEST_IMAGE_BASE;
    guest_register(s_addresses, s_replacement_functions,
                   ORACLE_FUNCTION_COUNT);
    guest_dispatch_table_get_status(&status);
    if (!status.ready || status.keys != ORACLE_FUNCTION_COUNT)
        return fail("dispatch table did not rebuild after the image", 0U);
    return 0;
}
#endif

static int original_import_hole(uint32_t slot)
{
    return slot != 0U && slot != ORACLE_IMPORT_SLOT_COUNT - 1U &&
           slot % 17U == 0U;
}

static int replacement_import_hole(uint32_t slot)
{
    return slot >= 1U && slot <= 25U;
}

static int prepare_imports(guest_import *imports,
                           int (*is_hole)(uint32_t),
                           const char *name)
{
    uint32_t slot, count = 0U;

    for (slot = 0U; slot < ORACLE_IMPORT_SLOT_COUNT; ++slot) {
        if (is_hole(slot))
            continue;
        if (count >= ORACLE_IMPORT_COUNT)
            return fail("dense IAT produced too many entries", count);
        imports[count].slot_rva = ORACLE_IMPORT_BASE + slot * 4U;
        imports[count].name = name;
        ++count;
    }
    return count == ORACLE_IMPORT_COUNT
        ? 0 : fail("dense IAT entry count is wrong", count);
}

static int verify_all_imports(const guest_import *imports,
                              int (*is_hole)(uint32_t),
                              const char *name)
{
    uint32_t slot, expected = 0U;

    for (slot = 0U; slot < ORACLE_IMPORT_SLOT_COUNT; ++slot) {
        uint32_t rva = ORACLE_IMPORT_BASE + slot * 4U;
        uint32_t id = UINT32_MAX;
        const guest_import *by_rva = guest_import_lookup(rva, &id);
        uint32_t relocated_id = UINT32_MAX;
        const guest_import *by_va = guest_import_lookup(
            GUEST_IMAGE_BASE + rva, &relocated_id);

        if (is_hole(slot)) {
            if (by_rva != NULL || by_va != NULL ||
                id != UINT32_MAX || relocated_id != UINT32_MAX)
                return fail("dense-IAT hole resolved", slot);
            continue;
        }
        if (expected >= ORACLE_IMPORT_COUNT ||
            by_rva != &imports[expected] || by_va != &imports[expected] ||
            id != expected || relocated_id != expected ||
            guest_import_name(rva) != name ||
            guest_import_name(GUEST_IMAGE_BASE + rva) != name)
            return fail("dense-IAT RVA/VA lookup mismatch", slot);
        ++expected;
    }
    return expected == ORACLE_IMPORT_COUNT
        ? 0 : fail("dense-IAT exhaustive count is wrong", expected);
}

static int verify_import_fallback(void)
{
    static const guest_import sparse[] = {
        { 0x00001000U, "sparse-0" },
        { 0x00002000U, "sparse-1" },
        { 0x00003000U, "sparse-2" },
    };
    uint32_t i;

    guest_register_imports(sparse, 3U);
    if (g_guest_import_dense_span != 0U)
        return fail("oversized dense-IAT span was accepted", 0U);
    for (i = 0U; i < 3U; ++i) {
        uint32_t id = UINT32_MAX;
        if (guest_import_lookup(sparse[i].slot_rva, &id) != &sparse[i] ||
            id != i)
            return fail("binary IAT fallback failed", i);
    }
    return 0;
}

static int verify_import_candidate_fallback(void)
{
    uint32_t target = 10U;
    uint32_t slot = (s_imports[target].slot_rva -
                     g_guest_import_dense_base) >> 2;
    uint16_t saved = g_guest_import_dense[slot];
    uint32_t id = UINT32_MAX;
    const guest_import *resolved;

    g_guest_import_dense[slot] = (uint16_t)(target + 1U);
    resolved = guest_import_lookup(s_imports[target].slot_rva, &id);
    g_guest_import_dense[slot] = saved;
    if (resolved != &s_imports[target] || id != target)
        return fail("checked dense-IAT candidate did not fall back", target);
    return 0;
}

int main(void)
{
    uint32_t hits_before, misses_before;

    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = ORACLE_IMAGE_SIZE;
    prepare_functions();

    guest_register(s_addresses, s_functions, ORACLE_FUNCTION_COUNT);
    if (verify_all_functions(s_functions) || verify_unknown_functions())
        return 1;
    if (g_guest_lookup_cache_hits != ORACLE_FUNCTION_COUNT ||
        g_guest_lookup_cache_misses != ORACLE_FUNCTION_COUNT + 2U)
        return fail("cold RVA/hot VA census is wrong", 0U);
    if (verify_function_collision())
        return 1;

    /* Repeat the full table after replacement, then repeat it again without
     * registration.  Every answer must remain exact through eviction. */
    if (verify_all_functions(s_replacement_functions) ||
        verify_all_functions(s_replacement_functions))
        return 1;
    if (oracle_cache_hits() + g_guest_lookup_cache_misses !=
        oracle_lookups())
        return fail("cache census does not cover every function lookup", 0U);
    if (verify_dispatch_fastpath())
        return 1;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    if (verify_dispatch_table())
        return 1;
#endif

    if (prepare_imports(s_imports, original_import_hole, "original") ||
        prepare_imports(s_replacement_imports, replacement_import_hole,
                        "replacement"))
        return 1;
    guest_register_imports(s_imports, ORACLE_IMPORT_COUNT);
    if (g_guest_import_dense_span != ORACLE_IMPORT_SLOT_COUNT ||
        verify_all_imports(s_imports, original_import_hole, "original") ||
        verify_import_candidate_fallback())
        return 1;
    guest_register_imports(s_replacement_imports, ORACLE_IMPORT_COUNT);
    if (g_guest_import_dense_span != ORACLE_IMPORT_SLOT_COUNT ||
        verify_all_imports(s_replacement_imports,
                           replacement_import_hole, "replacement") ||
        verify_import_fallback())
        return 1;

    hits_before = g_guest_lookup_cache_hits;
    misses_before = g_guest_lookup_cache_misses;
    guest_register(NULL, NULL, 0U);
    if (guest_lookup(ORACLE_FIRST_RVA) != NULL ||
        g_guest_lookup_cache_hits != hits_before ||
        g_guest_lookup_cache_misses != misses_before + 1U)
        return fail("empty re-registration retained a cached function", 0U);

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    puts("guest lookup cache oracle: PASS "
         "(14868 RVA+VA; exact collisions; confirmed dispatch bypass; "
         "reset/repeat; "
         "413/438 dense IAT; binary fallback; "
         "dispatch table 14868 VA hits, RVA/import/dynamic/no-image misses)");
#else
    puts("guest lookup cache oracle: PASS "
         "(14868 RVA+VA; exact collisions; confirmed dispatch bypass; "
         "reset/repeat; "
         "413/438 dense IAT; binary fallback)");
#endif
    return 0;
}

#else

static void oracle_off_function_0(CPU *__restrict cpu) { (void)cpu; }
static void oracle_off_function_1(CPU *__restrict cpu) { (void)cpu; }

int guest_host_import(CPU *__restrict cpu, const char *name)
{
    (void)cpu;
    (void)name;
    return 0;
}

int guest_host_dynamic(CPU *__restrict cpu, uint32_t token)
{
    (void)cpu;
    (void)token;
    return 0;
}

int main(void)
{
    static const uint32_t addresses[] = { 0x00001000U, 0x00002000U };
    static const guest_fn functions[] = {
        oracle_off_function_0, oracle_off_function_1
    };
    static const guest_fn replacements[] = {
        oracle_off_function_1, oracle_off_function_0
    };

    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = 0x00840000U;
    guest_register(addresses, functions, 2U);
    if (guest_lookup(addresses[0]) != functions[0] ||
        guest_lookup(GUEST_IMAGE_BASE + addresses[0]) != functions[0] ||
        guest_lookup(0x00001004U) != NULL) {
        fputs("guest lookup cache OFF oracle: FAIL: binary lookup\n", stderr);
        return 1;
    }
    guest_register(addresses, replacements, 2U);
    if (guest_lookup(addresses[0]) != replacements[0]) {
        fputs("guest lookup cache OFF oracle: FAIL: re-registration\n", stderr);
        return 1;
    }
    puts("guest lookup cache OFF oracle: PASS "
         "(binary lookup; re-register; no cache path)");
    return 0;
}

#endif
