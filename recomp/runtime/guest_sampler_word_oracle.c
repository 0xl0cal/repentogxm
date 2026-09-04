/* White-box host oracle for guest.c's sampler word (ISAAC_VITA_GUEST_SAMPLER).
 *
 * guest.c is included so the production publish/restore code runs: the
 * guest_call wrapper around guest_call_dispatch, the entry store inside the
 * dispatcher, the sync fast path's publish/restore, guest_call's validated
 * sync branch and the direct IAT route guest_import_call.  Nothing here
 * writes g_kage_guest_last_indirect_target except the top-level "enclosing
 * value" set-ups; every other value the checks below read was stored by
 * guest.c.
 *
 * Contract under test (kage_vita_guest_sampler.h): the word names the callee
 * of the innermost *live* indirect dispatch, and returns to the enclosing
 * value -- 0 or an arbitrary stale target -- once that callee returns.
 *   A. outer -> inner -> leaf through guest_call (cache-miss then cache-hit
 *      dispatch): each body sees its own VA on entry, the caller's VA again
 *      after the nested call returns, and the top level sees its value back.
 *   B. same with a non-zero enclosing value (a stale vtable target).
 *   C. guest_try_direct_sync_import_call from inside a translated body and
 *      from the top level, RVA and VA target forms: the host import observes
 *      the IAT target, the body/top level sees its own value restored.
 *   D. guest_call's validated sync branch and generic import-ID dispatch:
 *      the host import observes the slot VA; restored after.
 *   E. a rejected fast-path probe (wrong slot) publishes nothing.
 *   F. guest_import_call (ISAAC_VITA_IMPORT_DIRECT: the generated
 *      GUEST_IMPORT_CALL/JMP route, which never enters guest_call) from inside
 *      a translated body and from the top level: the family endpoint observes
 *      the slot VA, the body/top level sees its own value restored, and the
 *      per-import census notes the ID once.
 *   G. guest_import_call's fail-closed fallback (a target that is not this
 *      ID's slot token) reaches guest_call's generic import route: the served
 *      import observes its own slot VA, the wrapper restores, and the census
 *      counts the served ID once.
 * The census (ISAAC_VITA_PROFILE_IMPORT_KINDS; guest.h's
 * g_guest_phase_profile_import_calls, owned by guest.c) is compiled in as well
 * so the import routes' one-increment-per-import discipline is checked on the
 * way.  With ISAAC_VITA_GUEST_DISPATCH_TABLE the wrapper sits around the
 * inline probe and the sync branch lives in guest_call_slow; the scenarios
 * and their expected output are identical in both builds. */
#include "guest.c"

#if !defined(ISAAC_VITA_GUEST_SAMPLER)
#error This oracle requires ISAAC_VITA_GUEST_SAMPLER
#endif
#if !defined(ISAAC_VITA_SYNC_IMPORT_FASTPATH) || \
    !defined(ISAAC_VITA_IMPORT_ID_DISPATCH)
#error This oracle requires the import-ID dispatcher and the sync fast path
#endif
#if !defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
#error This oracle requires ISAAC_VITA_PROFILE_IMPORT_KINDS
#endif
#if !defined(ISAAC_VITA_IMPORT_DIRECT)
#error This oracle requires ISAAC_VITA_IMPORT_DIRECT (guest_import_call)
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

static guest_import s_imports[ISAAC_VITA_IMPORT_ID_COUNT];
static unsigned char s_coverage[ISAAC_VITA_IMPORT_ID_COUNT];

/* Owned by host_vita_first_fault.c in the real build. */
unsigned g_host_import_calls;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
unsigned g_host_dynamic_calls;
int guest_host_import_id_kind(uint32_t import_id)
{
    return import_id < ISAAC_VITA_IMPORT_ID_COUNT ? 2 : -1;
}
#endif

/* Translated bodies (RVAs sorted for guest_register). */
#define RVA_LEAF   UINT32_C(0x000023f0)
#define RVA_OUTER  UINT32_C(0x00562e00)
#define RVA_INNER  UINT32_C(0x005a0de0)
#define VA(rva)    (GUEST_IMAGE_BASE + (rva))
#define STALE_WORD UINT32_C(0x98123456)   /* a finished vtable callee */

#define SLOT_ENTER ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS
#define SLOT_LEAVE ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS
#define ID_ENTER   ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS
#define ID_LEAVE   ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS
#define ID_TRY     (ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS + 1U)
/* The direct route's row: crt-heap malloc (ph120.ih "ma"), the only row the
 * binding stub below hands to the direct table; every other row keeps
 * guest_call's classification (fn = NULL, guest_import_call falls back). */
#define ID_DIRECT   307U
#define NAME_DIRECT "api-ms-win-crt-heap-l1-1-0.dll!malloc"

/* What the host side of each import route saw in the word while running. */
static uint32_t s_sync_calls;
static uint32_t s_sync_seen_word;
static uint32_t s_sync_seen_index;
static uint32_t s_generic_calls;
static uint32_t s_generic_seen_word;
static uint32_t s_generic_seen_id;
static uint32_t s_direct_calls;
static uint32_t s_direct_seen_word;
static uint32_t s_direct_seen_index;

/* What each translated body saw. */
static uint32_t s_leaf_entry;
static uint32_t s_inner_entry, s_inner_after_leaf;
static uint32_t s_outer_entry, s_outer_after_inner;
static uint32_t s_outer_sync_inside, s_outer_sync_after;
static uint32_t s_outer_branch_inside, s_outer_branch_after;
static uint32_t s_outer_generic_inside, s_outer_generic_after;
static uint32_t s_outer_rejected_after;
static uint32_t s_outer_rejected_sync_calls;
static uint32_t s_outer_direct_inside, s_outer_direct_after;
static uint32_t s_outer_fallback_inside, s_outer_fallback_after;
static uint32_t s_outer_fallback_generic_calls;
static int s_outer_rejected_handled;
static int s_outer_sync_handled;

static int s_failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            ++s_failures; \
            fprintf(stderr, "guest sampler word oracle: FAIL %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition); \
        } \
    } while (0)

/* ------------------------------------------------------------- stubs ---- */

int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    uint32_t i;

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
    (void)c;
    ++s_generic_calls;
    ++g_host_import_calls;
    s_generic_seen_word = g_kage_guest_last_indirect_target;
    s_generic_seen_id = import_id;
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
    ++s_sync_calls;
    s_sync_seen_word = g_kage_guest_last_indirect_target;
    s_sync_seen_index = index;
    if (call_count)
        ++*call_count;
    return 1;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

/* The validated family endpoint guest_import_call enters directly (the
 * production binding is host_vita_import_id.c's family_indexed_fn). */
static int oracle_direct_family(CPU *__restrict c, uint32_t local_index,
                                unsigned *call_count)
{
    (void)c;
    ++s_direct_calls;
    s_direct_seen_word = g_kage_guest_last_indirect_target;
    s_direct_seen_index = local_index;
    if (call_count)
        ++*call_count;
    return 1;
}

int guest_host_import_id_direct_binding(uint32_t import_id,
                                        guest_import_family_fn *fn,
                                        uint32_t *local_index)
{
    if (import_id != ID_DIRECT)
        return 0;
    *fn = oracle_direct_family;
    *local_index = s_contract[ID_DIRECT].local_index;
    return 1;
}

/* ------------------------------------------------- translated bodies ---- */

static void body_leaf(CPU *__restrict c)
{
    (void)c;
    s_leaf_entry = g_kage_guest_last_indirect_target;
}

static void body_inner(CPU *__restrict c)
{
    s_inner_entry = g_kage_guest_last_indirect_target;
    guest_call(c, VA(RVA_LEAF));
    s_inner_after_leaf = g_kage_guest_last_indirect_target;
}

static void body_outer(CPU *__restrict c)
{
    uint32_t sync_before;
    uint32_t generic_before;

    s_outer_entry = g_kage_guest_last_indirect_target;
    guest_call(c, VA(RVA_INNER));
    s_outer_after_inner = g_kage_guest_last_indirect_target;

    /* C: the generated Enter/LeaveCriticalSection route (bypasses guest_call). */
    s_outer_sync_handled = guest_try_direct_sync_import_call(
        c, VA(SLOT_ENTER), SLOT_ENTER);
    s_outer_sync_inside = s_sync_seen_word;
    s_outer_sync_after = g_kage_guest_last_indirect_target;

    /* D: guest_call's validated sync branch, then generic import-ID dispatch. */
    guest_call(c, VA(SLOT_LEAVE));
    s_outer_branch_inside = s_sync_seen_word;
    s_outer_branch_after = g_kage_guest_last_indirect_target;
    guest_call(c, VA(s_contract[ID_TRY].slot_rva));
    s_outer_generic_inside = s_generic_seen_word;
    s_outer_generic_after = g_kage_guest_last_indirect_target;

    /* E: a probe the fast path rejects (Enter target, Leave slot pin). */
    sync_before = s_sync_calls;
    s_outer_rejected_handled = guest_try_direct_sync_import_call(
        c, VA(SLOT_ENTER), SLOT_LEAVE);
    s_outer_rejected_sync_calls = s_sync_calls - sync_before;
    s_outer_rejected_after = g_kage_guest_last_indirect_target;

    /* F: the generated GUEST_IMPORT_CALL/JMP route (guest_import_call), which
     * enters the family endpoint without guest_call's wrapper. */
    guest_import_call(c, VA(s_contract[ID_DIRECT].slot_rva), ID_DIRECT);
    s_outer_direct_inside = s_direct_seen_word;
    s_outer_direct_after = g_kage_guest_last_indirect_target;

    /* G: its fail-closed fallback (another row's token with this ID) takes
     * guest_call, which serves the *other* import on the generic route. */
    generic_before = s_generic_calls;
    guest_import_call(c, VA(s_contract[ID_TRY].slot_rva), ID_DIRECT);
    s_outer_fallback_generic_calls = s_generic_calls - generic_before;
    s_outer_fallback_inside = s_generic_seen_word;
    s_outer_fallback_after = g_kage_guest_last_indirect_target;
}

/* ---------------------------------------------------------- scenarios --- */

static void reset_observations(void)
{
    s_sync_calls = 0U;
    s_sync_seen_word = UINT32_C(0xffffffff);
    s_sync_seen_index = UINT32_C(0xffffffff);
    s_generic_calls = 0U;
    s_generic_seen_word = UINT32_C(0xffffffff);
    s_generic_seen_id = UINT32_C(0xffffffff);
    s_direct_calls = 0U;
    s_direct_seen_word = UINT32_C(0xffffffff);
    s_direct_seen_index = UINT32_C(0xffffffff);
    s_leaf_entry = s_inner_entry = s_inner_after_leaf = UINT32_C(0xffffffff);
    s_outer_entry = s_outer_after_inner = UINT32_C(0xffffffff);
    s_outer_sync_inside = s_outer_sync_after = UINT32_C(0xffffffff);
    s_outer_branch_inside = s_outer_branch_after = UINT32_C(0xffffffff);
    s_outer_generic_inside = s_outer_generic_after = UINT32_C(0xffffffff);
    s_outer_rejected_after = UINT32_C(0xffffffff);
    s_outer_rejected_sync_calls = 0U;
    s_outer_direct_inside = s_outer_direct_after = UINT32_C(0xffffffff);
    s_outer_fallback_inside = s_outer_fallback_after = UINT32_C(0xffffffff);
    s_outer_fallback_generic_calls = 0U;
    s_outer_rejected_handled = -1;
    s_outer_sync_handled = -1;
}

static void scenario_nested(CPU *cpu, uint32_t enclosing, const char *label)
{
    uint32_t host_before = g_host_import_calls;
    uint32_t enter_before = g_guest_phase_profile_import_calls[ID_ENTER];
    uint32_t leave_before = g_guest_phase_profile_import_calls[ID_LEAVE];
    uint32_t try_before = g_guest_phase_profile_import_calls[ID_TRY];
    uint32_t direct_before = g_guest_phase_profile_import_calls[ID_DIRECT];
    uint32_t eax = UINT32_C(0xa51ca11e);

    reset_observations();
    cpu->eax = eax;
    g_kage_guest_last_indirect_target = enclosing;
    guest_call(cpu, VA(RVA_OUTER));

    /* A/B: publish on entry, restore after every nested return. */
    CHECK(s_outer_entry == VA(RVA_OUTER));
    CHECK(s_inner_entry == VA(RVA_INNER));
    CHECK(s_leaf_entry == VA(RVA_LEAF));
    CHECK(s_inner_after_leaf == VA(RVA_INNER));
    CHECK(s_outer_after_inner == VA(RVA_OUTER));
    CHECK(g_kage_guest_last_indirect_target == enclosing);
    /* C: fast path publishes the IAT target for the import's duration. */
    CHECK(s_outer_sync_handled == 1);
    CHECK(s_outer_sync_inside == VA(SLOT_ENTER));
    CHECK(s_outer_sync_after == VA(RVA_OUTER));
    /* D: guest_call's sync branch and generic dispatch likewise. */
    CHECK(s_outer_branch_inside == VA(SLOT_LEAVE));
    CHECK(s_outer_branch_after == VA(RVA_OUTER));
    CHECK(s_outer_generic_inside == VA(s_contract[ID_TRY].slot_rva));
    CHECK(s_generic_seen_id == ID_TRY);
    CHECK(s_outer_generic_after == VA(RVA_OUTER));
    /* E: rejected probe: no import, no publish. */
    CHECK(s_outer_rejected_handled == 0);
    CHECK(s_outer_rejected_sync_calls == 0U);
    CHECK(s_outer_rejected_after == VA(RVA_OUTER));
    /* F: the direct IAT route publishes the slot for the endpoint's duration
     * and restores the body's own value; the endpoint saw its local index. */
    CHECK(s_direct_calls == 1U);
    CHECK(s_outer_direct_inside == VA(s_contract[ID_DIRECT].slot_rva));
    CHECK(s_direct_seen_index == s_contract[ID_DIRECT].local_index);
    CHECK(s_outer_direct_after == VA(RVA_OUTER));
    /* G: the fail-closed fallback served the other row through guest_call. */
    CHECK(s_outer_fallback_generic_calls == 1U);
    CHECK(s_outer_fallback_inside == VA(s_contract[ID_TRY].slot_rva));
    CHECK(s_generic_seen_id == ID_TRY);
    CHECK(s_outer_fallback_after == VA(RVA_OUTER));
    /* Route census: two sync imports + two generic + one direct, one note
     * per import route (the fast path, guest_call's import classification,
     * guest_import_call's validated branch). */
    CHECK(s_sync_calls == 2U);
    CHECK(s_generic_calls == 2U);
    CHECK(g_host_import_calls == host_before + 5U);
    CHECK(g_guest_phase_profile_import_calls[ID_ENTER] == enter_before + 1U);
    CHECK(g_guest_phase_profile_import_calls[ID_LEAVE] == leave_before + 1U);
    CHECK(g_guest_phase_profile_import_calls[ID_TRY] == try_before + 2U);
    CHECK(g_guest_phase_profile_import_calls[ID_DIRECT] == direct_before + 1U);
    CHECK(cpu->eax == eax);

    printf("scenario %s: enclosing=%08x outer=%08x inner=%08x leaf=%08x "
           "inner.after=%08x outer.after=%08x sync.inside=%08x sync.after=%08x "
           "branch.inside=%08x branch.after=%08x generic.inside=%08x "
           "generic.after=%08x rejected.after=%08x direct.inside=%08x "
           "direct.after=%08x fallback.inside=%08x fallback.after=%08x "
           "final=%08x\n",
           label, enclosing, s_outer_entry, s_inner_entry, s_leaf_entry,
           s_inner_after_leaf, s_outer_after_inner, s_outer_sync_inside,
           s_outer_sync_after, s_outer_branch_inside, s_outer_branch_after,
           s_outer_generic_inside, s_outer_generic_after,
           s_outer_rejected_after, s_outer_direct_inside, s_outer_direct_after,
           s_outer_fallback_inside, s_outer_fallback_after,
           g_kage_guest_last_indirect_target);
}

static void scenario_top_level_direct(CPU *cpu, uint32_t enclosing,
                                      const char *label)
{
    uint32_t target = VA(s_contract[ID_DIRECT].slot_rva);
    uint32_t host_before = g_host_import_calls;
    uint32_t direct_before = g_guest_phase_profile_import_calls[ID_DIRECT];

    reset_observations();
    g_kage_guest_last_indirect_target = enclosing;
    guest_import_call(cpu, target, ID_DIRECT);
    CHECK(s_direct_calls == 1U);
    CHECK(s_generic_calls == 0U);
    CHECK(s_direct_seen_word == target);
    CHECK(s_direct_seen_index == s_contract[ID_DIRECT].local_index);
    CHECK(g_kage_guest_last_indirect_target == enclosing);
    CHECK(g_host_import_calls == host_before + 1U);
    CHECK(g_guest_phase_profile_import_calls[ID_DIRECT] == direct_before + 1U);
    printf("scenario %s: target=%08x enclosing=%08x inside=%08x after=%08x\n",
           label, target, enclosing, s_direct_seen_word,
           g_kage_guest_last_indirect_target);
}

static void scenario_top_level_sync(CPU *cpu, uint32_t target,
                                    uint32_t exact_slot, uint32_t enclosing,
                                    const char *label)
{
    int handled;

    reset_observations();
    g_kage_guest_last_indirect_target = enclosing;
    handled = guest_try_direct_sync_import_call(cpu, target, exact_slot);
    CHECK(handled == 1);
    CHECK(s_sync_calls == 1U);
    CHECK(s_sync_seen_word == target);
    CHECK(g_kage_guest_last_indirect_target == enclosing);
    printf("scenario %s: target=%08x enclosing=%08x inside=%08x after=%08x\n",
           label, target, enclosing, s_sync_seen_word,
           g_kage_guest_last_indirect_target);
}

int main(void)
{
    static const uint32_t addresses[] = { RVA_LEAF, RVA_OUTER, RVA_INNER };
    static const guest_fn functions[] = { body_leaf, body_outer, body_inner };
    CPU cpu;
    uint32_t i;

    memset(&cpu, 0, sizeof cpu);
    cpu.esp = UINT32_C(0x23004000);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    for (i = 0U; i < ISAAC_VITA_IMPORT_ID_COUNT; ++i) {
        s_imports[i].slot_rva = s_contract[i].slot_rva;
        s_imports[i].name = s_contract[i].name;
    }
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = UINT32_C(0x00840000);
    g_guest_coverage_imports = s_coverage;
    guest_register(addresses, functions, 3U);
    guest_register_imports(s_imports, ISAAC_VITA_IMPORT_ID_COUNT);
    CHECK(g_vita_sync_import_fastpath_ready);
    CHECK(g_kage_guest_last_indirect_target == 0U);
    /* The direct table bound exactly the one stubbed row. */
    CHECK(strcmp(s_contract[ID_DIRECT].name, NAME_DIRECT) == 0);
    CHECK(g_guest_import_direct[ID_DIRECT].fn == oracle_direct_family);
    CHECK(g_guest_import_direct[ID_DIRECT].slot_va ==
          VA(s_contract[ID_DIRECT].slot_rva));
    CHECK(g_guest_import_direct[ID_TRY].fn == NULL);

    /* A: no live dispatch above (word 0 = "ind" for the sampler). */
    scenario_nested(&cpu, 0U, "A idle");
    /* B: a stale finished callee is the enclosing value and must survive. */
    scenario_nested(&cpu, STALE_WORD, "B stale");
    /* A again: the second pass takes the dispatch cache's hit path. */
    scenario_nested(&cpu, 0U, "A cached");
    /* C: top-level fast path, RVA and VA target forms. */
    scenario_top_level_sync(&cpu, SLOT_ENTER, SLOT_ENTER, 0U, "C rva");
    scenario_top_level_sync(&cpu, VA(SLOT_LEAVE), SLOT_LEAVE, STALE_WORD,
                            "C va");
    /* F: top-level direct IAT route, idle and stale enclosing values. */
    scenario_top_level_direct(&cpu, 0U, "F idle");
    scenario_top_level_direct(&cpu, STALE_WORD, "F stale");

    if (s_failures) {
        fprintf(stderr, "guest sampler word oracle: %d failure(s)\n",
                s_failures);
        return 1;
    }
    puts("Vita guest sampler word oracle: PASS");
    return 0;
}
