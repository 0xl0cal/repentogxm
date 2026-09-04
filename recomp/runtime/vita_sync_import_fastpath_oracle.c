/* White-box host oracle for validated Enter/Leave import-ID routing.
 * guest.c is included so registration readiness and relocated-VA handling can
 * be exercised without a copyrighted PE or a Vita build. */
#include "guest.c"

#if !defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH)
#error This oracle requires ISAAC_VITA_SYNC_IMPORT_FASTPATH
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

static guest_import s_imports[ISAAC_VITA_IMPORT_ID_COUNT];
static unsigned char s_coverage[ISAAC_VITA_IMPORT_ID_COUNT];
static uint32_t s_registration_calls;
static uint32_t s_generic_calls;
static uint32_t s_last_generic_id;
static uint32_t s_dynamic_calls;
static uint32_t s_translated_calls;
static uint32_t s_hot_calls;
static uint32_t s_hot_indices[16];
static int s_hot_accept = 1;
static uint32_t s_run_address;
static uint32_t s_run_exact_slot;

unsigned g_host_import_calls;
#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
uint32_t g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
#endif

_Static_assert(sizeof s_contract / sizeof s_contract[0] ==
                   ISAAC_VITA_IMPORT_ID_COUNT,
               "oracle import contract count drifted");

int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    uint32_t i;

    ++s_registration_calls;
    if (!imports || count != ISAAC_VITA_IMPORT_ID_COUNT)
        return 0;
    for (i = 0U; i < count; ++i) {
        if (s_contract[i].id != i ||
            imports[i].slot_rva != s_contract[i].slot_rva ||
            !imports[i].name ||
            strcmp(imports[i].name, s_contract[i].name) != 0)
            return 0;
    }
    return 1;
}

int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    ++s_generic_calls;
    ++g_host_import_calls;
    s_last_generic_id = import_id;
    c->eax = UINT32_C(0x60000000) | import_id;
    return 1;
}

const char *guest_host_import_id_error(void)
{
    return "oracle generic import-ID error";
}

int isaac_vita_sync_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    (void)c;
    if (s_hot_calls < sizeof s_hot_indices / sizeof s_hot_indices[0])
        s_hot_indices[s_hot_calls] = index;
    ++s_hot_calls;
    if (!s_hot_accept)
        return 0;
    if (call_count)
        ++*call_count;
    return 1;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    ++s_dynamic_calls;
    return 0;
}

static void oracle_translated(CPU *__restrict c)
{
    (void)c;
    ++s_translated_calls;
}

static void oracle_run_call(CPU *__restrict c)
{
    if (!guest_try_direct_sync_import_call(
            c, s_run_address, s_run_exact_slot))
        guest_call(c, s_run_address);
}

static int fail(const char *message, uint32_t value)
{
    fprintf(stderr, "Vita sync import fast path oracle: FAIL: %s (%u)\n",
            message, value);
    return 1;
}

static int verify_hot_call(CPU *cpu, uint32_t address,
                           uint32_t expected_index)
{
    uint32_t calls_before = s_hot_calls;
    uint32_t host_before = g_host_import_calls;
    uint32_t guest_before = g_guest_phase_profile_counters.guest_calls;
    uint32_t lookups_before = g_guest_phase_profile_counters.guest_lookups;
    uint32_t eax = UINT32_C(0xa51ca11e);

    cpu->eax = eax;
    guest_call(cpu, address);
    if (s_hot_calls != calls_before + 1U ||
        s_hot_indices[calls_before] != expected_index ||
        g_host_import_calls != host_before + 1U ||
        s_generic_calls != 0U || s_dynamic_calls != 0U ||
        s_translated_calls != 0U || cpu->eax != eax ||
        g_guest_phase_profile_counters.guest_calls != guest_before + 1U ||
        g_guest_phase_profile_counters.guest_lookups != lookups_before)
        return fail("validated import did not take the exact sync endpoint",
                    address);
    return 0;
}

typedef struct direct_route_outcome {
    CPU cpu;
    GuestPhaseProfileCounters profile;
    unsigned char coverage[ISAAC_VITA_IMPORT_ID_COUNT];
    uint32_t generic_calls;
    uint32_t last_generic_id;
    uint32_t dynamic_calls;
    uint32_t translated_calls;
    uint32_t hot_calls;
    uint32_t first_hot_index;
    unsigned host_import_calls;
} direct_route_outcome;

static void reset_route_observation(void)
{
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    memset(s_coverage, 0, sizeof s_coverage);
    memset(s_hot_indices, 0, sizeof s_hot_indices);
    s_generic_calls = 0U;
    s_last_generic_id = 0U;
    s_dynamic_calls = 0U;
    s_translated_calls = 0U;
    s_hot_calls = 0U;
    g_host_import_calls = 0U;
}

static void capture_route_outcome(direct_route_outcome *out, const CPU *cpu)
{
    out->cpu = *cpu;
    out->profile = g_guest_phase_profile_counters;
    memcpy(out->coverage, s_coverage, sizeof out->coverage);
    out->generic_calls = s_generic_calls;
    out->last_generic_id = s_last_generic_id;
    out->dynamic_calls = s_dynamic_calls;
    out->translated_calls = s_translated_calls;
    out->hot_calls = s_hot_calls;
    out->first_hot_index = s_hot_indices[0];
    out->host_import_calls = g_host_import_calls;
}

#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
/* The inline single-owner path completes the validated import without
 * the endpoint: same census/coverage marks, the endpoint stub is not
 * called, and the 24-byte object plus ESP show the Win32 effects.  Any
 * other object state still reaches the endpoint stub untouched. */
static uint32_t s_inline_stack[64];
static uint32_t s_inline_cs[8];

static int verify_inline_owner_path(void)
{
    CPU cpu;
    uint32_t floor = (uint32_t)(uintptr_t)&s_inline_stack[0];
    uint32_t ceiling = (uint32_t)(uintptr_t)&s_inline_stack[63];
    uint32_t object = (uint32_t)(uintptr_t)&s_inline_cs[0];
    uint32_t hot_before;
    uint32_t host_before;
    uint32_t guest_before;
    uint32_t target = GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS;

    memset(&cpu, 0, sizeof cpu);
    cpu.stack_owner = &cpu;
    cpu.stack_floor = floor;
    cpu.stack_ceiling = ceiling;
    cpu.stack_low_water = ceiling;
    cpu.eax = UINT32_C(0xa51ca11e);
    cpu.vita_sync_thread_id = UINT32_C(0x40010003);
    g_isaac_vita_sync_inline_owner = UINT32_C(0x40010003);
    s_inline_cs[0] = ISAAC_VITA_SYNC_CS_MAGIC;
    s_inline_cs[1] = (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE;
    s_inline_cs[3] = ISAAC_VITA_SYNC_CS_VERSION;
    s_inline_cs[4] = 0U;
    s_inline_cs[5] = 0U;

    /* Enter twice (acquire, recurse), Leave twice, all inline. */
    for (hot_before = s_hot_calls; ; ) {
        uint32_t k;
        for (k = 0U; k < 4U; ++k) {
            uint32_t enter = k < 2U;
            cpu.esp = ceiling - 32U;
            s_inline_stack[63 - 8] = UINT32_C(0x00562e2f);
            s_inline_stack[63 - 7] = object;
            host_before = g_host_import_calls;
            guest_before = g_guest_phase_profile_counters.guest_calls;
            target = GUEST_IMAGE_BASE +
                     (enter ? ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS
                            : ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS);
            if (!guest_try_direct_sync_import_call(
                    &cpu, target,
                    enter ? ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS
                          : ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS) ||
                s_hot_calls != hot_before ||
                g_host_import_calls != host_before + 1U ||
                g_guest_phase_profile_counters.guest_calls !=
                    guest_before + 1U ||
                cpu.esp != ceiling - 24U ||
                cpu.eax != UINT32_C(0xa51ca11e) || cpu.fault ||
                cpu.stack_low_water != ceiling - 28U ||
                s_inline_cs[4] != (k == 3U ? 0U : UINT32_C(0x40010003)) ||
                s_inline_cs[5] != (k < 2U ? k + 1U : 3U - k))
                return fail("inline owner path diverged", k);
        }
        break;
    }
    if (!s_coverage[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS] ||
        !s_coverage[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS])
        return fail("inline owner path skipped import coverage", 0U);

    /* Not initialized: the endpoint stub, and only it, is reached. */
    s_inline_cs[0] = 0U;
    cpu.esp = ceiling - 32U;
    host_before = g_host_import_calls;
    if (!guest_try_direct_sync_import_call(
            &cpu, GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS) ||
        s_hot_calls != hot_before + 1U ||
        g_host_import_calls != host_before + 1U ||
        cpu.esp != ceiling - 32U || s_inline_cs[4] || s_inline_cs[5])
        return fail("uninitialized object escaped the endpoint", 0U);

    /* Another identity (or an unbound CPU) never runs inline. */
    s_inline_cs[0] = ISAAC_VITA_SYNC_CS_MAGIC;
    cpu.vita_sync_thread_id = 0U;
    cpu.esp = ceiling - 32U;
    if (!guest_try_direct_sync_import_call(
            &cpu, GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS) ||
        s_hot_calls != hot_before + 2U || s_inline_cs[4] || s_inline_cs[5])
        return fail("unbound CPU ran the inline owner path", 0U);
    g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
    return 0;
}
#endif

static int verify_direct_differential(uint32_t address,
                                      uint32_t exact_slot,
                                      int expected_direct)
{
    direct_route_outcome reference;
    direct_route_outcome direct;
    CPU reference_cpu;
    CPU direct_cpu;
    int handled;

    memset(&reference_cpu, 0, sizeof reference_cpu);
    reference_cpu.eax = UINT32_C(0xa51ca11e);
    reference_cpu.ecx = UINT32_C(0x12345678);
    reference_cpu.edx = UINT32_C(0x89abcdef);
    reference_cpu.esp = UINT32_C(0x23004000);
    direct_cpu = reference_cpu;
    memset(&reference, 0, sizeof reference);
    memset(&direct, 0, sizeof direct);

    reset_route_observation();
    guest_call(&reference_cpu, address);
    capture_route_outcome(&reference, &reference_cpu);

    reset_route_observation();
    handled = guest_try_direct_sync_import_call(
        &direct_cpu, address, exact_slot);
    if (!handled)
        guest_call(&direct_cpu, address);
    capture_route_outcome(&direct, &direct_cpu);

    if (handled != expected_direct ||
        memcmp(&direct, &reference, sizeof direct) != 0)
        return fail("direct-IAT route disagreed with guest_call", address);
    return 0;
}

int main(void)
{
    uint32_t translated_address[1];
    const guest_fn translated_function[] = { oracle_translated };
    CPU cpu;
    uint32_t i;
    uint32_t hot_before;
    uint32_t generic_before;
    uint32_t host_before;
    int stop;

    memset(&cpu, 0, sizeof cpu);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    for (i = 0U; i < ISAAC_VITA_IMPORT_ID_COUNT; ++i) {
        s_imports[i].slot_rva = s_contract[i].slot_rva;
        s_imports[i].name = s_contract[i].name;
    }

    if (s_contract[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS].kind !=
            ISAAC_VITA_IMPORT_SYNC ||
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS].slot_rva !=
            ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS ||
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS].local_index !=
            ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS ||
        strcmp(s_contract[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS].name,
               "KERNEL32.dll!LeaveCriticalSection") != 0 ||
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].kind !=
            ISAAC_VITA_IMPORT_SYNC ||
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].slot_rva !=
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS ||
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].local_index !=
            ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS ||
        strcmp(s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].name,
               "KERNEL32.dll!EnterCriticalSection") != 0)
        return fail("named constants disagree with frozen map", 0U);

    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = UINT32_C(0x00840000);
    g_guest_coverage_imports = s_coverage;
    translated_address[0] =
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].slot_rva;
    guest_register(translated_address, translated_function, 1U);
    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    if (s_registration_calls != 1U ||
        !g_vita_sync_import_fastpath_ready)
        return fail("exact table did not arm fast routing",
                    s_registration_calls);

    /* Compare the generated route shape (try exact endpoint, otherwise the
     * untouched guest_call) against guest_call itself.  Both accepted RVA/VA
     * forms and two rejection classes must preserve the full CPU, coverage,
     * phase and host-import observations. */
    if (verify_direct_differential(
            ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS,
            ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS, 1) ||
        verify_direct_differential(
            GUEST_IMAGE_BASE + ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS, 1) ||
        verify_direct_differential(
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
            ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS, 0) ||
        verify_direct_differential(
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
            UINT32_C(0x006060f4), 0))
        return 1;
    reset_route_observation();

    if (verify_hot_call(
            &cpu,
            s_contract[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS].slot_rva,
            ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS) ||
        verify_hot_call(
            &cpu,
            GUEST_IMAGE_BASE +
                s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].slot_rva,
            ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS) ||
        verify_hot_call(
            &cpu,
            s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].slot_rva,
            ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS) ||
        verify_hot_call(
            &cpu,
            GUEST_IMAGE_BASE +
                s_contract[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS].slot_rva,
            ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS))
        return 1;
    if (!s_coverage[ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS] ||
        !s_coverage[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS])
        return fail("fast route skipped semantic import coverage", 0U);

    /* The adjacent try-lock remains on generic dispatch: its BOOL and
     * contention behavior are deliberately outside this optimization. */
    generic_before = s_generic_calls;
    guest_call(&cpu,
               s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS + 1U].slot_rva);
    if (s_generic_calls != generic_before + 1U ||
        s_last_generic_id != ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS + 1U ||
        s_hot_calls != 4U ||
        !s_coverage[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS + 1U])
        return fail("TryEnterCriticalSection escaped generic dispatch",
                    s_last_generic_id);

    /* A replaced row makes complete registration fail.  The exact Enter slot
     * must then take the established generic path, never the trusted shortcut. */
    s_imports[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].name = "replaced-enter";
    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    if (g_vita_sync_import_fastpath_ready)
        return fail("replaced table retained fast routing", 0U);
    hot_before = s_hot_calls;
    generic_before = s_generic_calls;
    if (guest_try_direct_sync_import_call(
            &cpu, ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS,
            ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS))
        return fail("failed registration entered direct-IAT route", 0U);
    guest_call(&cpu,
               s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].slot_rva);
    if (s_hot_calls != hot_before ||
        s_generic_calls != generic_before + 1U ||
        s_last_generic_id != ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS)
        return fail("failed registration did not fall back to generic ID",
                    s_last_generic_id);

    /* Re-arm, then force the impossible local-index rejection.  It must stop
     * as an attributed guest fault rather than falling through another API. */
    s_imports[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].name =
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].name;
    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    s_hot_accept = 0;
    s_run_address =
        s_contract[ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS].slot_rva;
    s_run_exact_slot = ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS;
    generic_before = s_generic_calls;
    host_before = g_host_import_calls;
    stop = guest_run_until_stop(&cpu, oracle_run_call);
    if (stop != GUEST_RUN_FAULT || !cpu.fault ||
        strcmp(cpu.fault,
               "validated Vita sync import rejected its local ID") != 0 ||
        cpu.fault_addr != s_run_address ||
        s_generic_calls != generic_before ||
        g_host_import_calls != host_before)
        return fail("rejected trusted local ID did not fail closed", stop);

#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
    reset_route_observation();
    s_hot_accept = 1;
    if (verify_inline_owner_path())
        return 1;
    puts("Vita sync import fast path oracle: PASS "
         "(exact 413 rows; Enter/Leave RVA+VA; import precedence; "
         "three-site direct-IAT differential; registration and local-ID fail closed; "
         "inline single-owner Enter/Leave without the endpoint)");
#else
    puts("Vita sync import fast path oracle: PASS "
         "(exact 413 rows; Enter/Leave RVA+VA; import precedence; "
         "three-site direct-IAT differential; registration and local-ID fail closed)");
#endif
    return 0;
}
