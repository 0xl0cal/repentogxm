/* Focused host oracle for the floor thunk direct seam.  ILP32 only: the seam
 * reads the binary64 argument through the guest address space (guest address
 * == host address), so the fixture stack must be addressable in 32 bits. */
#include "host_vita_floor_thunk_direct.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(ISAAC_VITA_GUEST_SAMPLER)
#include "kage_vita_guest_sampler.h"
#endif

_Static_assert(sizeof(void *) == 4, "the floor thunk oracle needs ILP32");

#define CHECK(condition) do {                                           \
    ++s_checks;                                                         \
    if (!(condition)) {                                                 \
        fprintf(stderr, "floor-thunk oracle failed at %s:%d: %s\n",     \
                __FILE__, __LINE__, #condition);                        \
        return 1;                                                       \
    }                                                                   \
} while (0)

enum {
    FLOOR_IAT_RVA = 0x00606524U,
    FLOOR_IMPORT_ID = 313U,
    STACK_WORDS = 64U,
    RETURN_RVA = 0x0055FBEEU           /* pixel-snap return word, an RVA */
};
#define FLOOR_SLOT_VA ((uint32_t)(GUEST_IMAGE_BASE + FLOOR_IAT_RVA))

static unsigned char s_import_coverage[413];
static unsigned s_checks;
static unsigned s_handled;
static unsigned s_rejected;
static unsigned s_hostile;
static uint32_t s_stack[STACK_WORDS] __attribute__((aligned(8)));

unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_imports = s_import_coverage;
unsigned char *g_guest_coverage_cases;
unsigned g_host_import_calls;
GuestPhaseProfileCounters g_guest_phase_profile_counters;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
uint32_t g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
#endif
int g_isaac_vita_import_ids_ready;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
volatile uint32_t g_kage_guest_last_indirect_target;
#define SAMPLER_ENCLOSING 0x00412340U
#endif

static uint32_t stack_address(unsigned word)
{
    return (uint32_t)(uintptr_t)&s_stack[word];
}

static uint64_t bits_of(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void fixture(CPU *c, double argument, unsigned esp_word)
{
    memset(c, 0xa5, sizeof *c);
    memset(s_import_coverage, 0, sizeof s_import_coverage);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    memset(g_guest_phase_profile_import_calls, 0,
           sizeof g_guest_phase_profile_import_calls);
#endif
    memset(s_stack, 0xcd, sizeof s_stack);
    c->eax = 0x11111111U;
    c->ecx = 0x22222222U;
    c->edx = 0x33333333U;
    c->ebx = 0x44444444U;
    c->ebp = 0x55555555U;
    c->esi = 0x66666666U;
    c->edi = 0x77777777U;
    c->esp = stack_address(esp_word);
    c->stack_owner = c;
    c->stack_floor = stack_address(0);
    c->stack_ceiling = stack_address(STACK_WORDS);
    c->stack_low_water = c->esp;         /* the caller's push noted it */
    c->fault = NULL;
    c->fault_addr = 0U;
    c->st_top = 0;
    for (unsigned i = 0U; i < 8U; ++i)
        c->st[i] = 1000.0 + i;
    if (esp_word + 3U <= STACK_WORDS) {
        s_stack[esp_word] = RETURN_RVA;
        memcpy(&s_stack[esp_word + 1U], &argument, sizeof argument);
    }
    g_isaac_vita_import_ids_ready = 1;
    g_host_import_calls = 0U;
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    g_kage_guest_last_indirect_target = SAMPLER_ENCLOSING;
#endif
}

static int no_receipts(void)
{
    unsigned char zero[sizeof s_import_coverage];
    memset(zero, 0, sizeof zero);
    if (g_host_import_calls != 0U ||
            g_guest_phase_profile_counters.guest_calls != 0U ||
            g_guest_phase_profile_counters.guest_lookups != 0U ||
            g_guest_phase_profile_counters.lookup_iterations != 0U ||
            memcmp(s_import_coverage, zero, sizeof zero) != 0)
        return 0;
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    for (unsigned i = 0U; i < GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS; ++i)
        if (g_guest_phase_profile_import_calls[i] != 0U)
            return 0;
#endif
    return 1;
}

static int expect_handled(double argument, double expected, int st_top,
                          uint32_t low_water)
{
    CPU c;
    uint32_t entry;
    uint32_t stack_before[STACK_WORDS];
    unsigned slot;

    fixture(&c, argument, 32U);
    c.st_top = st_top;
    c.stack_low_water = low_water;
    entry = c.esp;
    memcpy(stack_before, s_stack, sizeof stack_before);
    CHECK(isaac_vita_floor_thunk_direct_try(&c, FLOOR_SLOT_VA) == 1);
    ++s_handled;
    /* cdecl: only the return word is consumed; the guest stack is unchanged. */
    CHECK(c.esp == entry + 4U);
    CHECK(memcmp(stack_before, s_stack, sizeof stack_before) == 0);
    CHECK(c.eax == 0x11111111U && c.ecx == 0x22222222U &&
          c.edx == 0x33333333U && c.ebx == 0x44444444U &&
          c.ebp == 0x55555555U && c.esi == 0x66666666U &&
          c.edi == 0x77777777U);
    CHECK(c.fault == NULL && c.fault_addr == 0U);
    CHECK(c.stack_owner == &c && c.stack_floor == stack_address(0) &&
          c.stack_ceiling == stack_address(STACK_WORDS));
    /* guest_stack_address's low-water note of the argument address. */
    CHECK(c.stack_low_water == (entry + 4U < low_water ? entry + 4U : low_water));
    /* x87: one push onto the 8-entry ring, the other seven lanes untouched. */
    slot = (unsigned)((st_top - 1) & 7);
    CHECK((unsigned)c.st_top == slot);
    CHECK(bits_of(c.st[slot]) == bits_of(expected));
    CHECK(bits_of(c.st[slot]) == bits_of(floor(argument)));
    for (unsigned i = 0U; i < 8U; ++i)
        if (i != slot)
            CHECK(bits_of(c.st[i]) == bits_of(1000.0 + i));
    /* Receipts of guest_import_call -> isaac_vita_math_import_indexed. */
    CHECK(g_host_import_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_lookups == 0U &&
          g_guest_phase_profile_counters.lookup_iterations == 0U);
    CHECK(s_import_coverage[FLOOR_IMPORT_ID] == 1U);
    for (unsigned i = 0U; i < sizeof s_import_coverage; ++i)
        if (i != FLOOR_IMPORT_ID)
            CHECK(s_import_coverage[i] == 0U);
#if defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)
    CHECK(g_guest_phase_profile_import_calls[FLOOR_IMPORT_ID] == 1U);
    for (unsigned i = 0U; i < GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS; ++i)
        if (i != FLOOR_IMPORT_ID)
            CHECK(g_guest_phase_profile_import_calls[i] == 0U);
#endif
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    CHECK(g_kage_guest_last_indirect_target == SAMPLER_ENCLOSING);
#endif
    return 0;
}

static int expect_rejected(uint32_t target, int ready, unsigned esp_word,
                           int bound, int hostile)
{
    CPU c;
    CPU before;
    uint32_t stack_before[STACK_WORDS];

    fixture(&c, 2.5, esp_word);
    if (!bound)
        c.stack_owner = NULL;
    g_isaac_vita_import_ids_ready = ready;
    before = c;
    memcpy(stack_before, s_stack, sizeof stack_before);
    CHECK(isaac_vita_floor_thunk_direct_try(&c, target) == 0);
    if (hostile)
        ++s_hostile;
    else
        ++s_rejected;
    CHECK(memcmp(&c, &before, sizeof c) == 0);
    CHECK(memcmp(stack_before, s_stack, sizeof stack_before) == 0);
    CHECK(no_receipts());
#if defined(ISAAC_VITA_GUEST_SAMPLER)
    CHECK(g_kage_guest_last_indirect_target == SAMPLER_ENCLOSING);
#endif
    return 0;
}

int main(void)
{
    const double inf = INFINITY;
    const double nan_value = NAN;
    const double huge = 4503599627370497.0;      /* 2^52 + 1: already integral */
    CPU c;

    /* Handled: the values pixel snapping and the room code feed floor. */
    CHECK(expect_handled(2.5, 2.0, 0, stack_address(32)) == 0);
    CHECK(expect_handled(-2.5, -3.0, 0, stack_address(32)) == 0);
    CHECK(expect_handled(0.999999999, 0.0, 3, stack_address(32)) == 0);
    CHECK(expect_handled(-0.25, -1.0, 7, stack_address(32)) == 0);
    CHECK(expect_handled(0.0, 0.0, 0, stack_address(32)) == 0);
    CHECK(expect_handled(-0.0, -0.0, 0, stack_address(32)) == 0);
    CHECK(expect_handled(inf, inf, 0, stack_address(32)) == 0);
    CHECK(expect_handled(-inf, -inf, 0, stack_address(32)) == 0);
    CHECK(expect_handled(huge, huge, 0, stack_address(32)) == 0);
    CHECK(expect_handled(1e300, 1e300, 0, stack_address(32)) == 0);
    CHECK(expect_handled(-1e300, -1e300, 0, stack_address(32)) == 0);
    CHECK(expect_handled(2.2250738585072014e-308, 0.0, 0,
                         stack_address(32)) == 0);
    CHECK(expect_handled(nan_value, floor(nan_value), 0,
                         stack_address(32)) == 0);
    /* An untouched low-water mark (as after `mov esp`) moves to the argument
     * address exactly as guest_stack_address would move it. */
    CHECK(expect_handled(7.75, 7.0, 0, stack_address(STACK_WORDS)) == 0);
    /* The tightest frame the range check accepts: esp + 12 == ceiling. */
    fixture(&c, 3.5, STACK_WORDS - 3U);
    CHECK(isaac_vita_floor_thunk_direct_try(&c, FLOOR_SLOT_VA) == 1);
    ++s_handled;
    CHECK(c.esp == stack_address(STACK_WORDS - 2U) &&
          bits_of(c.st[7]) == bits_of(3.0) && g_host_import_calls == 1U);

    /* Rejected: everything guest_import_call would also refuse. */
    CHECK(expect_rejected(0x12345678U, 1, 32U, 1, 0) == 0);
    CHECK(expect_rejected(FLOOR_IAT_RVA, 1, 32U, 1, 0) == 0);  /* raw RVA */
    CHECK(expect_rejected(FLOOR_SLOT_VA, 0, 32U, 1, 0) == 0);  /* not ready */
    /* Short stack: esp + 12 > ceiling (one word short), esp at the ceiling. */
    CHECK(expect_rejected(FLOOR_SLOT_VA, 1, STACK_WORDS - 2U, 1, 0) == 0);
    CHECK(expect_rejected(FLOOR_SLOT_VA, 1, STACK_WORDS, 1, 0) == 0);
    /* Unbound context (no stack owner). */
    CHECK(expect_rejected(FLOOR_SLOT_VA, 1, 32U, 0, 0) == 0);

    /* Hostile: a clobbered IAT word (neighbour slot, zero, all ones, one bit
     * flipped, the memset slot) and a NULL context. */
    CHECK(expect_rejected(FLOOR_SLOT_VA + 4U, 1, 32U, 1, 1) == 0);
    CHECK(expect_rejected(0U, 1, 32U, 1, 1) == 0);
    CHECK(expect_rejected(0xFFFFFFFFU, 1, 32U, 1, 1) == 0);
    CHECK(expect_rejected(FLOOR_SLOT_VA ^ 1U, 1, 32U, 1, 1) == 0);
    CHECK(expect_rejected((uint32_t)(GUEST_IMAGE_BASE + 0x00606484U),
                          1, 32U, 1, 1) == 0);
    CHECK(isaac_vita_floor_thunk_direct_try(NULL, FLOOR_SLOT_VA) == 0);
    ++s_hostile;
    CHECK(no_receipts());

    printf("Vita floor thunk oracle: PASS; checks=%u; "
           "handled=%u; rejected=%u; hostile=%u\n",
           s_checks, s_handled, s_rejected, s_hostile);
    return 0;
}
