#if !defined(_WIN32) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE 1
#endif

#include "host_vita_save_reader_direct.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
#else
# include <sys/mman.h>
#endif

#define CHECK(condition) do {                                           \
    ++s_checks;                                                         \
    if (!(condition)) {                                                 \
        fprintf(stderr, "Save Reader direct oracle failed at %s:%d\n", \
                __FILE__, __LINE__);                                    \
        return 1;                                                       \
    }                                                                   \
} while (0)

enum {
    ARENA_ADDRESS = 0x23000000U,
    ARENA_BYTES = 0x10000U,
    STACK_FLOOR = ARENA_ADDRESS + 0x2000U,
    STACK_ENTRY = ARENA_ADDRESS + 0x3000U,
    STACK_CEILING = ARENA_ADDRESS + 0x4000U,
    TARGET_RVA = 0x0025ba60U,
    READER_COVERAGE_ID = 2328U
};

static uint8_t *s_arena;
static uint8_t s_function_coverage[2400];
static uint8_t s_import_coverage[1];
static uint8_t s_case_coverage[1];
static unsigned s_checks;
static unsigned s_reader_calls;
static unsigned s_lookup_cache_hits;
static int s_returning_fault;

unsigned char *g_guest_coverage_functions = s_function_coverage;
unsigned char *g_guest_coverage_imports = s_import_coverage;
unsigned char *g_guest_coverage_cases = s_case_coverage;
GuestPhaseProfileCounters g_guest_phase_profile_counters;

void guest_phase_profile_note_lookup_cache_hit(void)
{
    ++s_lookup_cache_hits;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)pc;
    (void)kind;
    (void)size;
    guest_fault(c, address, "oracle stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(
        c, pc, GUEST_STACK_FAULT_OWNER, c ? c->esp : 0U, 0U);
}

void sub_0025ba60(CPU *__restrict c)
{
    ++s_reader_calls;
    guest_coverage_function(READER_COVERAGE_ID);
    if (s_returning_fault) {
        guest_fault(c, TARGET_RVA, "injected direct reader fault");
        return;
    }
    c->eax = 4U;
    (void)gpop_generated(c);
    (void)guest_stack_adjust_generated(c, 12U);
    GUEST_STACK_CALLSITE_BARRIER();
}

static int oracle_map(void)
{
#if defined(_WIN32)
    s_arena = (uint8_t *)VirtualAlloc(
        (void *)(uintptr_t)ARENA_ADDRESS, ARENA_BYTES,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *mapped;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
# if defined(MAP_FIXED_NOREPLACE)
    flags |= MAP_FIXED_NOREPLACE;
# else
    flags |= MAP_FIXED;
# endif
    mapped = mmap((void *)(uintptr_t)ARENA_ADDRESS, ARENA_BYTES,
                  PROT_READ | PROT_WRITE, flags, -1, 0);
    s_arena = mapped == MAP_FAILED ? NULL : (uint8_t *)mapped;
#endif
    return s_arena == (uint8_t *)(uintptr_t)ARENA_ADDRESS;
}

static void oracle_unmap(void)
{
    if (s_arena == NULL)
        return;
#if defined(_WIN32)
    (void)VirtualFree(s_arena, 0U, MEM_RELEASE);
#else
    (void)munmap(s_arena, ARENA_BYTES);
#endif
    s_arena = NULL;
}

static void fixture(CPU *c)
{
    memset(s_arena, 0xa5, ARENA_BYTES);
    memset(c, 0, sizeof *c);
    c->esp = STACK_ENTRY;
    c->stack_floor = STACK_FLOOR;
    c->stack_ceiling = STACK_CEILING;
    c->stack_low_water = STACK_ENTRY;
    c->stack_owner = c;
    st32(STACK_ENTRY, 0x00528326U);
    st32(STACK_ENTRY + 4U, ARENA_ADDRESS + 0x1000U);
    st32(STACK_ENTRY + 8U, 4U);
    st32(STACK_ENTRY + 12U, 1U);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    s_lookup_cache_hits = 0U;
    s_returning_fault = 0;
}

static int handled_case(uint32_t target)
{
    CPU c;
    unsigned calls = s_reader_calls;

    fixture(&c);
    CHECK(isaac_vita_save_reader_direct_try(&c, target) == 1);
    CHECK(s_reader_calls == calls + 1U);
    CHECK(c.esp == STACK_ENTRY + 16U);
    CHECK(c.eax == 4U);
    CHECK(c.fault == NULL);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_lookups == 1U);
    CHECK(g_guest_phase_profile_counters.lookup_iterations == 0U);
    CHECK(s_lookup_cache_hits == 1U);
    CHECK(s_function_coverage[READER_COVERAGE_ID] == 1U);
    return 0;
}

static int rejected_case(void)
{
    CPU c;
    CPU before;
    unsigned calls = s_reader_calls;

    fixture(&c);
    before = c;
    CHECK(isaac_vita_save_reader_direct_try(&c, 0x12345678U) == 0);
    CHECK(memcmp(&c, &before, sizeof c) == 0);
    CHECK(s_reader_calls == calls);
    CHECK(g_guest_phase_profile_counters.guest_calls == 0U);
    CHECK(g_guest_phase_profile_counters.guest_lookups == 0U);
    CHECK(s_lookup_cache_hits == 0U);
    return 0;
}

static int returning_fault_case(void)
{
    CPU c;
    unsigned calls = s_reader_calls;

    fixture(&c);
    s_returning_fault = 1;
    CHECK(isaac_vita_save_reader_direct_try(&c, TARGET_RVA) == 1);
    CHECK(s_reader_calls == calls + 1U);
    CHECK(c.fault != NULL && c.fault_addr == TARGET_RVA);
    CHECK(c.esp == STACK_ENTRY);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_lookups == 1U);
    CHECK(s_lookup_cache_hits == 1U);
    return 0;
}

int main(void)
{
    int result = 0;

    if (!oracle_map())
        return 2;
    result |= handled_case(TARGET_RVA);
    result |= handled_case((uint32_t)(GUEST_IMAGE_BASE + TARGET_RVA));
    result |= rejected_case();
    result |= returning_fault_case();
    if (result == 0)
        printf("Vita Save Reader direct-edge oracle: PASS; checks=%u; "
               "rva=1; va=1; fault=1; rejected=1\n", s_checks);
    oracle_unmap();
    return result;
}
