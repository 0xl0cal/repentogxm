#include "guest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                             \
    ++s_checks;                                                           \
    if (!(condition)) {                                                   \
        fprintf(stderr, "ReferenceCount direct oracle failed at %s:%d\n", \
                __FILE__, __LINE__);                                      \
        return 1;                                                         \
    }                                                                     \
} while (0)

enum {
    TARGET_COUNT = 5,
    WORD_COUNT = 8
};

typedef struct Target {
    uint32_t rva;
    guest_fn function;
} Target;

typedef struct Outcome {
    CPU cpu;
    uint32_t words[WORD_COUNT];
    GuestPhaseProfileCounters profile;
    unsigned cache_hits;
    unsigned exact_calls[TARGET_COUNT];
    unsigned fallback_calls;
} Outcome;

static uint32_t *s_active_words;
static unsigned s_exact_calls[TARGET_COUNT];
static unsigned s_fallback_calls;
static unsigned s_cache_hits;
static unsigned s_checks;
static int s_returning_fault;

GuestPhaseProfileCounters g_guest_phase_profile_counters;

void guest_phase_profile_note_lookup_cache_hit(void)
{
    ++s_cache_hits;
}

static void exact_effect(CPU *c, unsigned index, uint32_t rva)
{
    ++s_exact_calls[index];
    c->eax ^= rva;
    c->ecx += 0x101U + index;
    c->edx = (c->edx << 1) | (c->edx >> 31);
    c->esp += 4U + index * 4U;
    c->f_a = rva;
    c->f_r = c->eax;
    c->f_op = FLAG_LOGIC;
    s_active_words[index] ^= rva + c->ecx;
    s_active_words[WORD_COUNT - 1U] += index + 1U;
    if (s_returning_fault) {
        c->fault = "injected translated fault";
        c->fault_addr = rva;
    }
}

#define DEFINE_EXACT(index, suffix, address)                              \
    static void exact_##suffix(CPU *c)                                    \
    {                                                                     \
        exact_effect(c, index, address);                                  \
    }

DEFINE_EXACT(0U, decrement, 0x00007AF0U)
DEFINE_EXACT(1U, increment, 0x00007B50U)
DEFINE_EXACT(2U, increment_nonzero, 0x00007B70U)
DEFINE_EXACT(3U, mutex_enter, 0x00562E00U)
DEFINE_EXACT(4U, mutex_leave, 0x00562EC0U)

static const Target s_targets[TARGET_COUNT] = {
    {0x00007AF0U, exact_decrement},
    {0x00007B50U, exact_increment},
    {0x00007B70U, exact_increment_nonzero},
    {0x00562E00U, exact_mutex_enter},
    {0x00562EC0U, exact_mutex_leave},
};

static int reference_matches(uint32_t target, uint32_t rva)
{
    return target == rva ||
           target == (uint32_t)(GUEST_IMAGE_BASE + rva);
}

static void reference_guest_call(CPU *c, uint32_t target)
{
    unsigned index;

    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_LOOKUP();
    guest_phase_profile_note_lookup_cache_hit();
    for (index = 0U; index < TARGET_COUNT; ++index) {
        if (reference_matches(target, s_targets[index].rva)) {
            s_targets[index].function(c);
            return;
        }
    }
    ++s_fallback_calls;
    c->ebx ^= target;
    s_active_words[6] += target;
}

static void fast_call(CPU *c, uint32_t target, const Target *exact)
{
    if (guest_direct_translated_target(target, exact->rva)) {
        GUEST_PHASE_PROFILE_NOTE_CALL();
        GUEST_PHASE_PROFILE_NOTE_LOOKUP();
        guest_phase_profile_note_lookup_cache_hit();
        exact->function(c);
    } else {
        reference_guest_call(c, target);
    }
}

static void fixture(CPU *c, uint32_t words[WORD_COUNT])
{
    unsigned index;

    memset(c, 0, sizeof *c);
    c->eax = 0x13579BDFU;
    c->ecx = 0x2468ACE0U;
    c->edx = 0x89ABCDEFU;
    c->ebx = 0x10203040U;
    c->esp = 0x23003000U;
    for (index = 0U; index < WORD_COUNT; ++index)
        words[index] = 0xA5A50000U + index * 0x101U;
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    memset(s_exact_calls, 0, sizeof s_exact_calls);
    s_fallback_calls = 0U;
    s_cache_hits = 0U;
    s_active_words = words;
}

static void capture(Outcome *out, const CPU *c,
                    const uint32_t words[WORD_COUNT])
{
    out->cpu = *c;
    memcpy(out->words, words, sizeof out->words);
    out->profile = g_guest_phase_profile_counters;
    out->cache_hits = s_cache_hits;
    memcpy(out->exact_calls, s_exact_calls, sizeof out->exact_calls);
    out->fallback_calls = s_fallback_calls;
}

static void run_reference(Outcome *out, uint32_t target, int fault)
{
    CPU c;
    uint32_t words[WORD_COUNT];

    fixture(&c, words);
    s_returning_fault = fault;
    reference_guest_call(&c, target);
    capture(out, &c, words);
}

static void run_fast(Outcome *out, const Target *exact,
                     uint32_t target, int fault)
{
    CPU c;
    uint32_t words[WORD_COUNT];

    fixture(&c, words);
    s_returning_fault = fault;
    fast_call(&c, target, exact);
    capture(out, &c, words);
}

static int differential_case(const Target *exact,
                             uint32_t target, int fault)
{
    Outcome reference;
    Outcome fast;

    run_reference(&reference, target, fault);
    run_fast(&fast, exact, target, fault);
    CHECK(memcmp(&fast.cpu, &reference.cpu, sizeof fast.cpu) == 0);
    CHECK(memcmp(fast.words, reference.words, sizeof fast.words) == 0);
    CHECK(memcmp(&fast.profile, &reference.profile,
                 sizeof fast.profile) == 0);
    CHECK(fast.cache_hits == reference.cache_hits);
    CHECK(memcmp(fast.exact_calls, reference.exact_calls,
                 sizeof fast.exact_calls) == 0);
    CHECK(fast.fallback_calls == reference.fallback_calls);
    return 0;
}

int main(void)
{
    unsigned index;
    int result = 0;

    for (index = 0U; index < TARGET_COUNT; ++index) {
        const Target *exact = &s_targets[index];
        const Target *other = &s_targets[(index + 1U) % TARGET_COUNT];
        result |= differential_case(exact, exact->rva, 0);
        result |= differential_case(
            exact, (uint32_t)(GUEST_IMAGE_BASE + exact->rva), 0);
        result |= differential_case(exact, exact->rva, 1);
        result |= differential_case(exact, other->rva, 0);
        result |= differential_case(exact, 0x12345678U + index, 0);
    }
    if (result == 0)
        printf("Vita ReferenceCount direct-edge oracle: PASS; checks=%u; "
               "routes=25; base=%08x\n", s_checks,
               (unsigned)GUEST_IMAGE_BASE);
    return result;
}
