/* White-box host oracle for the boot-built raw-VA dispatch table
 * (ISAAC_VITA_GUEST_DISPATCH_TABLE) against the REAL generated tables.
 * recomp/test_guest_dispatch_table.py extracts s_addrs[] and the IAT slot
 * RVAs from the generated guest_table.c into guest_dispatch_oracle_keys.h,
 * so the table built here is bit-for-bit the one the device builds at boot
 * (deterministic integer arithmetic, same image base). */
#include "guest.c"
#include "guest_dispatch_oracle_keys.h"

#include <stddef.h>

#if !defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
#error compile with ISAAC_VITA_GUEST_DISPATCH_TABLE=1
#endif
#if GUEST_IMAGE_BASE != 0x98000000U
#error the oracle must use the device image base
#endif

/* Referenced by guest_note_authenticated_import_call, which the linker keeps
 * in this white-box unit; the real definition lives in the platform host. */
unsigned g_host_import_calls;
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* ISAAC_VITA_GL_SHIM_TABLE_TOKENS leg: gl_bridge.c and host_vita_gl.c are
 * linked in (the trampolines count into this word; host_vita_first_fault.c
 * owns it in production). */
unsigned g_host_dynamic_calls;
#include "gl_bridge.h"
#endif

/* Host entry points reached only by the complete path; a table hit must
 * never get here, so the oracle counts them and expects zero. */
static uint32_t s_host_calls;

int guest_host_import(CPU *__restrict cpu, const char *name)
{
    (void)cpu;
    (void)name;
    ++s_host_calls;
    return 0;
}

int guest_host_dynamic(CPU *__restrict cpu, uint32_t token)
{
    (void)cpu;
    (void)token;
    ++s_host_calls;
    return 0;
}

#define ORACLE_IMAGE_SIZE     0x00840000U
#define ORACLE_STUBS          8U
#define ORACLE_RANDOM_PROBES  100000U
#define ORACLE_TOKEN_PROBES   1024U

static guest_fn     s_fns[ORACLE_RVA_COUNT];
static guest_import s_imports[ORACLE_IMPORT_COUNT];
static uint32_t     s_stub_calls[ORACLE_STUBS];

static void stub_0(CPU *__restrict c) { (void)c; ++s_stub_calls[0]; }
static void stub_1(CPU *__restrict c) { (void)c; ++s_stub_calls[1]; }
static void stub_2(CPU *__restrict c) { (void)c; ++s_stub_calls[2]; }
static void stub_3(CPU *__restrict c) { (void)c; ++s_stub_calls[3]; }
static void stub_4(CPU *__restrict c) { (void)c; ++s_stub_calls[4]; }
static void stub_5(CPU *__restrict c) { (void)c; ++s_stub_calls[5]; }
static void stub_6(CPU *__restrict c) { (void)c; ++s_stub_calls[6]; }
static void stub_7(CPU *__restrict c) { (void)c; ++s_stub_calls[7]; }

static const guest_fn s_stubs[ORACLE_STUBS] = {
    stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7
};

static int fail(const char *message, uint32_t value)
{
    printf("guest dispatch table oracle: FAIL: %s (0x%08x)\n",
           message, (unsigned)value);
    return 1;
}

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* Registry tokens are table members too (while g_gl_dynamic_first holds,
 * which the production-shaped tables below never disturb). */
static int is_gl_token(uint32_t key)
{
    const uint32_t *tokens;
    uint32_t count = guest_gl_table_tokens(&tokens, NULL), i;

    for (i = 0U; i < count; ++i)
        if (tokens[i] == key)
            return 1;
    return 0;
}
#endif

/* Membership by binary search of the ascending generated RVA table. */
static int is_member(uint32_t key)
{
    uint32_t lo = 0U, hi = ORACLE_RVA_COUNT, rva;

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    if (is_gl_token(key))
        return 1;
#endif
    if (key < GUEST_IMAGE_BASE)
        return 0;
    rva = key - GUEST_IMAGE_BASE;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2U;

        if (s_oracle_rvas[mid] == rva)
            return 1;
        if (s_oracle_rvas[mid] < rva) lo = mid + 1U; else hi = mid;
    }
    return 0;
}

/* A non-member must never see slot->key == key.  Key 0 is special: it may
 * land on a free slot, whose key is 0 and whose function is the trampoline
 * to the complete path with that same key. */
static int expect_miss(uint32_t key, const char *what, uint32_t *counted)
{
    const guest_dispatch_slot *slot;

    if (is_member(key))
        return 0;
    slot = guest_dispatch_probe(key);
    if (slot->key == key &&
        !(key == GUEST_DISPATCH_EMPTY_KEY &&
          slot->function == guest_dispatch_empty_slot))
        return fail(what, key);
    ++*counted;
    return 0;
}

int main(void)
{
    guest_dispatch_table_status status;
    CPU cpu;
    uint32_t i, x, counted_rva = 0U, counted_iat = 0U, counted_token = 0U;
    uint32_t counted_adjacent = 0U, counted_random = 0U, skipped_random = 0U;
#if defined(ISAAC_VITA_PHASE_PROFILE)
    uint32_t slow_before, calls_before;
#endif

    /* Layout pin: the ARM probe (recomp/runtime/guest.c guest_dispatch_probe)
     * relies on the bucket multipliers at offset 0, the first-level
     * multiplier at 2048 and 8-byte {key, fn} slots from 2056 so every load
     * uses an immediate or scaled-register offset off one base. */
    _Static_assert(offsetof(guest_dispatch_table, bucket_multiplier) == 0U,
                   "bucket multipliers must lead the table");
    _Static_assert(offsetof(guest_dispatch_table, multiplier) == 2048U,
                   "first-level multiplier must sit at 2048");
    _Static_assert(offsetof(guest_dispatch_table, slots) == 2056U,
                   "slots must start at 2056");
    _Static_assert(sizeof g_guest_dispatch.bucket_multiplier[0] == 2U,
                   "bucket multipliers must be 16-bit");
    _Static_assert(sizeof(void *) != 4U || sizeof(guest_dispatch_slot) == 8U,
                   "32-bit slots must be {key, fn} = 8 bytes");

    memset(&cpu, 0, sizeof cpu);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
#endif
    g_image_base = GUEST_IMAGE_BASE;
    g_image_size = ORACLE_IMAGE_SIZE;
    for (i = 0U; i < ORACLE_RVA_COUNT; ++i) {
        if (i && s_oracle_rvas[i] <= s_oracle_rvas[i - 1U])
            return fail("generated RVAs are not ascending", i);
        if (s_oracle_rvas[i] >= ORACLE_IMAGE_SIZE)
            return fail("generated RVA outside the image", s_oracle_rvas[i]);
        s_fns[i] = s_stubs[i % ORACLE_STUBS];
    }
    for (i = 0U; i < ORACLE_IMPORT_COUNT; ++i) {
        s_imports[i].slot_rva = s_oracle_import_rvas[i];
        s_imports[i].name = "oracle.dll!import";
    }

    guest_register(s_oracle_rvas, s_fns, ORACLE_RVA_COUNT);
    guest_register_imports(s_imports, ORACLE_IMPORT_COUNT);
    for (i = 0U; i < ORACLE_IMPORT_COUNT; ++i)
        if (!guest_import_lookup(GUEST_IMAGE_BASE + s_oracle_import_rvas[i],
                                 NULL))
            return fail("IAT slot did not register", s_oracle_import_rvas[i]);

    guest_dispatch_table_get_status(&status);
    if (!status.ready || status.reason != NULL)
        return fail(status.reason ? status.reason : "table not ready",
                    status.keys);
    if (status.keys != ORACLE_RVA_COUNT || status.skipped != 0U)
        return fail("table does not cover every generated function",
                    status.keys);
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* Every registry token resolves to exactly its trampoline, and the
     * status counts them apart from the translated functions. */
    {
        const uint32_t *tokens;
        const guest_fn *functions;
        uint32_t count = guest_gl_table_tokens(&tokens, &functions);

        if (count != 73U || status.gl_tokens != count)
            return fail("GL token census", status.gl_tokens);
        for (i = 0U; i < count; ++i) {
            const guest_dispatch_slot *slot = guest_dispatch_probe(tokens[i]);

            if ((tokens[i] & UINT32_C(0xff000000)) != UINT32_C(0x7e000000))
                return fail("GL token outside the 0x7e family", tokens[i]);
            if (slot->key != tokens[i])
                return fail("GL token misses", tokens[i]);
            if (!functions[i] || slot->function != functions[i])
                return fail("GL token resolves to another function",
                            tokens[i]);
        }
    }
#endif

    /* Every generated function resolves to exactly its registered pointer. */
    for (i = 0U; i < ORACLE_RVA_COUNT; ++i) {
        uint32_t key = GUEST_IMAGE_BASE + s_oracle_rvas[i];
        const guest_dispatch_slot *slot = guest_dispatch_probe(key);

        if (slot->key != key)
            return fail("generated function misses", key);
        if (slot->function != s_fns[i])
            return fail("generated function resolves to another function",
                        key);
    }

    /* Non-members: RVA-form keys (direct drivers before normalisation),
     * every IAT slot as RVA and VA, dynamic token families, adjacent
     * addresses (misaligned neighbours of real starts) and random values. */
    for (i = 0U; i < ORACLE_RVA_COUNT; ++i)
        if (expect_miss(s_oracle_rvas[i], "RVA-form key hits", &counted_rva))
            return 1;
    for (i = 0U; i < ORACLE_IMPORT_COUNT; ++i) {
        if (expect_miss(s_oracle_import_rvas[i], "IAT slot RVA hits",
                        &counted_iat) ||
            expect_miss(GUEST_IMAGE_BASE + s_oracle_import_rvas[i],
                        "IAT slot VA hits", &counted_iat))
            return 1;
    }
    for (i = 0U; i < ORACLE_TOKEN_PROBES; ++i) {
        if (expect_miss(UINT32_C(0x7d000000) + i * 17U, "WGL/XInput token hits",
                        &counted_token) ||
            expect_miss(UINT32_C(0x7e000000) + i * 17U, "typed GL token hits",
                        &counted_token))
            return 1;
    }
    if (expect_miss(0U, "key 0 hits a live slot", &counted_token))
        return 1;
    for (i = 0U; i < ORACLE_RVA_COUNT; ++i) {
        uint32_t key = GUEST_IMAGE_BASE + s_oracle_rvas[i];

        if (expect_miss(key - 1U, "adjacent key hits", &counted_adjacent) ||
            expect_miss(key + 1U, "adjacent key hits", &counted_adjacent) ||
            expect_miss(key + 2U, "adjacent key hits", &counted_adjacent) ||
            expect_miss(key + 3U, "adjacent key hits", &counted_adjacent))
            return 1;
    }
    x = UINT32_C(0x9E3779B9);
    for (i = 0U; i < ORACLE_RANDOM_PROBES; ++i) {
        uint32_t key;

        x ^= x << 13; x ^= x >> 17; x ^= x << 5;      /* xorshift32 */
        /* Half of the draws stay inside the image so they exercise the
         * populated buckets rather than only the empty ones. */
        key = (i & 1U) ? GUEST_IMAGE_BASE + (x % ORACLE_IMAGE_SIZE) : x;
        if (is_member(key)) {
            ++skipped_random;
            continue;
        }
        if (expect_miss(key, "random non-member hits", &counted_random))
            return 1;
    }

    /* guest_call end to end: every VA reaches its stub through the inline
     * probe, one dispatch_calls increment each and no slow entry. */
    memset(s_stub_calls, 0, sizeof s_stub_calls);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    slow_before = g_guest_phase_profile_counters.dispatch_slow;
    calls_before = g_guest_phase_profile_counters.dispatch_calls;
#endif
    for (i = 0U; i < ORACLE_RVA_COUNT; ++i)
        guest_call(&cpu, GUEST_IMAGE_BASE + s_oracle_rvas[i]);
    for (i = 0U; i < ORACLE_STUBS; ++i) {
        uint32_t expected = ORACLE_RVA_COUNT / ORACLE_STUBS +
            (i < ORACLE_RVA_COUNT % ORACLE_STUBS ? 1U : 0U);

        if (s_stub_calls[i] != expected)
            return fail("guest_call reached the wrong stub", i);
    }
    if (s_host_calls != 0U)
        return fail("a table hit reached a host entry point", s_host_calls);
#if defined(ISAAC_VITA_PHASE_PROFILE)
    if (g_guest_phase_profile_counters.dispatch_slow != slow_before)
        return fail("a table hit entered the slow path",
                    g_guest_phase_profile_counters.dispatch_slow);
    if (g_guest_phase_profile_counters.dispatch_calls - calls_before !=
            ORACLE_RVA_COUNT)
        return fail("dispatch_calls census is wrong",
                    g_guest_phase_profile_counters.dispatch_calls);
#endif

    /* Unmapping the image empties the table (fail closed); mapping it again
     * rebuilds the identical table. */
    g_image_base = 0U;
    guest_register(s_oracle_rvas, s_fns, ORACLE_RVA_COUNT);
    guest_dispatch_table_get_status(&status);
    if (status.ready || status.keys != 0U)
        return fail("table stayed live without an image", status.keys);
    for (i = 0U; i < ORACLE_RVA_COUNT; ++i)
        if (guest_dispatch_probe(GUEST_IMAGE_BASE + s_oracle_rvas[i])->key !=
                GUEST_DISPATCH_EMPTY_KEY)
            return fail("emptied table still resolves", s_oracle_rvas[i]);
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    {
        const uint32_t *tokens;
        uint32_t count = guest_gl_table_tokens(&tokens, NULL);

        if (status.gl_tokens != 0U)
            return fail("emptied table keeps GL tokens", status.gl_tokens);
        for (i = 0U; i < count; ++i)
            if (guest_dispatch_probe(tokens[i])->key != GUEST_DISPATCH_EMPTY_KEY)
                return fail("emptied table still resolves a GL token",
                            tokens[i]);
    }
#endif
    g_image_base = GUEST_IMAGE_BASE;
    guest_register(s_oracle_rvas, s_fns, ORACLE_RVA_COUNT);
    guest_dispatch_table_get_status(&status);
    if (!status.ready || status.keys != ORACLE_RVA_COUNT)
        return fail("table did not rebuild", status.keys);

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    if (status.gl_tokens != 73U)
        return fail("rebuilt table lost its GL tokens", status.gl_tokens);
    printf("guest dispatch table oracle: GL tokens=%u resolve to their "
           "trampolines; every 0x7e probe outside the registry misses; "
           "an emptied table drops them\n", (unsigned)status.gl_tokens);
#endif
    printf("guest dispatch table oracle: PASS (%s: %u VA keys resolve "
           "exactly; misses: %u RVA-form, %u IAT RVA+VA, %u tokens, "
           "%u adjacent, %u random (%u members skipped); max bucket %u; "
           "%u bucket tries; %u KB)\n",
           ORACLE_TABLE_NAME, (unsigned)ORACLE_RVA_COUNT,
           (unsigned)counted_rva, (unsigned)counted_iat,
           (unsigned)counted_token, (unsigned)counted_adjacent,
           (unsigned)counted_random, (unsigned)skipped_random,
           (unsigned)status.max_bucket, (unsigned)status.bucket_tries,
           (unsigned)(sizeof g_guest_dispatch / 1024U));
    return 0;
}
