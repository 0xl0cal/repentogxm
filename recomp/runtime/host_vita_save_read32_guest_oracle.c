#if !defined(_WIN32) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE 1
#endif

#include "host_vita_save_read32_guest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif

#define CHECK(condition) do {                                           \
    ++s_checks;                                                         \
    if (!(condition)) {                                                 \
        fprintf(stderr, "Save read32 oracle failed at %s:%d: %s\n",   \
                __FILE__, __LINE__, #condition);                        \
        return 1;                                                       \
    }                                                                   \
} while (0)

enum {
    ARENA_ADDRESS = 0x22000000U,
    ARENA_BYTES = 0x00010000U,
    SOURCE_OFFSET = 0x1000U,
    DEST_OFFSET = 0x2000U,
    WRAPPER_OFFSET = 0x3000U,
    READER_OFFSET = 0x3100U,
    STACK_FLOOR_OFFSET = 0x4000U,
    STACK_ENTRY_OFFSET = 0x5000U,
    STACK_CEILING_OFFSET = 0x6000U,
    READER_VTABLE_RVA = 0x00746d3cU,
    READER_COVERAGE_ID = 2328U,
    CHECKSUM_COVERAGE_ID = 2319U
};

static uint8_t *s_arena;
static uint8_t s_initial[ARENA_BYTES];
static uint8_t s_expected[ARENA_BYTES];
static uint8_t s_function_coverage[8000];
static uint8_t s_import_coverage[1];
static uint8_t s_case_coverage[1];
static unsigned s_checks;
static unsigned s_reader_calls;
static unsigned s_checksum_calls;
static unsigned s_lookup_cache_hits;

enum oracle_fault_mode {
    ORACLE_FAULT_NONE,
    ORACLE_FAULT_READER,
    ORACLE_FAULT_CHECKSUM
};

static enum oracle_fault_mode s_fault_mode;

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
    c->fault = what;
    c->fault_addr = address;
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
# elif defined(MAP_FIXED)
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

static void store_u32(uint32_t address, uint32_t value)
{
    st32(address, value);
}

/* Focused stand-ins for the exact generated owners.  The fusion must call
 * these boundaries, preserve their coverage, and accept their stdcall ESP. */
void sub_0025ba60(CPU *__restrict c)
{
    uint32_t destination = ld32(c->esp + 4U);
    uint32_t requested = ld32(c->esp + 8U) * ld32(c->esp + 12U);
    uint32_t cursor = ld32(c->ecx + 0x18U);
    uint32_t end = ld32(c->ecx + 0x1cU);
    uint32_t copied = requested < end - cursor ? requested : end - cursor;
    uint32_t source = ld32(c->ecx + 0x10U) + cursor;

    ++s_reader_calls;
    guest_coverage_function(READER_COVERAGE_ID);
    if (s_fault_mode == ORACLE_FAULT_READER) {
        guest_fault(c, 0x25ba60U, "injected reader fault");
        return;
    }
    memcpy((void *)(uintptr_t)destination,
           (const void *)(uintptr_t)source, copied);
    cursor += copied;
    st32(c->ecx + 0x18U, cursor);
    st8(c->ecx + 0x21U, (uint8_t)(cursor >= end));
    c->eax = copied;
    c->ecx = (c->ecx & UINT32_C(0xffffff00)) |
             (cursor >= end ? 1U : 0U);
    (void)gpop_generated(c);
    (void)guest_stack_adjust_generated(c, 12U);
    GUEST_STACK_CALLSITE_BARRIER();
}

void sub_0025b340(CPU *__restrict c)
{
    uint32_t data = ld32(c->esp + 4U);
    uint32_t size = ld32(c->esp + 8U);
    uint32_t value = ld32(c->ecx + 8U);
    uint32_t index;

    ++s_checksum_calls;
    guest_coverage_function(CHECKSUM_COVERAGE_ID);
    if (s_fault_mode == ORACLE_FAULT_CHECKSUM) {
        guest_fault(c, 0x25b340U, "injected checksum fault");
        return;
    }
    for (index = 0U; index < size; ++index)
        value = (value << 5) ^ (value >> 27) ^ ld8(data + index);
    st32(c->ecx + 8U, value);
    c->eax = value;
    (void)gpop_generated(c);
    (void)guest_stack_adjust_generated(c, 8U);
    GUEST_STACK_CALLSITE_BARRIER();
}

static void reference_wrapper(CPU *__restrict c)
{
    gpush_generated(c, c->ebp); GUEST_STACK_CALLSITE_BARRIER();
    c->ebp = c->esp;
    gpush_generated(c, c->esi); GUEST_STACK_CALLSITE_BARRIER();
    c->esi = ld32(c->ebp + 8U);
    gpush_generated(c, c->edi); GUEST_STACK_CALLSITE_BARRIER();
    c->edi = c->ecx;
    gpush_generated(c, 1U); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, 4U); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, c->esi); GUEST_STACK_CALLSITE_BARRIER();
    c->ecx = ld32(c->edi);
    c->eax = ld32(c->ecx);
    gpush_generated(c, 0x0052eaa6U); GUEST_STACK_CALLSITE_BARRIER();
    GUEST_PHASE_PROFILE_NOTE_CALL();
    GUEST_PHASE_PROFILE_NOTE_LOOKUP();
    guest_phase_profile_note_lookup_cache_hit();
    sub_0025ba60(c);
    c->eax = ld32(c->esi);
    c->ecx = c->edi + 0x0cU;
    st32(c->ebp + 8U, c->eax);
    c->eax = c->ebp + 8U;
    gpush_generated(c, 4U); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, c->eax); GUEST_STACK_CALLSITE_BARRIER();
    gpush_generated(c, 0x0052eab9U); GUEST_STACK_CALLSITE_BARRIER();
    sub_0025b340(c);
    c->edi = gpop_generated(c);
    c->esi = gpop_generated(c);
    c->ebp = gpop_generated(c);
    (void)gpop_generated(c);
    (void)guest_stack_adjust_generated(c, 4U);
    GUEST_STACK_CALLSITE_BARRIER();
}

static void fixture(CPU *c, uint32_t cursor, uint32_t end)
{
    uint32_t source = ARENA_ADDRESS + SOURCE_OFFSET;
    uint32_t destination = ARENA_ADDRESS + DEST_OFFSET;
    uint32_t wrapper = ARENA_ADDRESS + WRAPPER_OFFSET;
    uint32_t reader = ARENA_ADDRESS + READER_OFFSET;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;
    unsigned index;

    memset(s_arena, 0xa5, ARENA_BYTES);
    memset(c, 0, sizeof *c);
    c->eax = 0x11111111U;
    c->ecx = wrapper;
    c->edx = 0x33333333U;
    c->ebx = 0x44444444U;
    c->esp = entry;
    c->ebp = 0x66666666U;
    c->esi = 0x77777777U;
    c->edi = 0x88888888U;
    c->stack_floor = ARENA_ADDRESS + STACK_FLOOR_OFFSET;
    c->stack_ceiling = ARENA_ADDRESS + STACK_CEILING_OFFSET;
    c->stack_low_water = c->stack_ceiling;
    c->stack_owner = c;
    store_u32(wrapper, reader);
    store_u32(wrapper + 0x14U, 0x13579bdfU);
    store_u32(reader,
              (uint32_t)(GUEST_IMAGE_BASE + READER_VTABLE_RVA));
    store_u32(reader + 0x10U, source);
    store_u32(reader + 0x18U, cursor);
    store_u32(reader + 0x1cU, end);
    store_u32(entry, 0xdeadbeefU);
    store_u32(entry + 4U, destination);
    for (index = 0U; index < 16U; ++index)
        st8(source + index, (uint8_t)(0x20U + index));
}

static int cpu_equal(const CPU *left, const CPU *right)
{
    CPU a = *left;
    CPU b = *right;
    a.stack_owner = NULL;
    b.stack_owner = NULL;
    return memcmp(&a, &b, sizeof a) == 0;
}

static int differential_case(uint32_t cursor, uint32_t end)
{
    CPU c;
    CPU initial_cpu;
    CPU expected_cpu;
    GuestPhaseProfileCounters expected_profile;
    unsigned expected_lookup_cache_hits;

    fixture(&c, cursor, end);
    s_fault_mode = ORACLE_FAULT_NONE;
    initial_cpu = c;
    memcpy(s_initial, s_arena, ARENA_BYTES);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    s_lookup_cache_hits = 0U;
    reference_wrapper(&c);
    expected_cpu = c;
    expected_profile = g_guest_phase_profile_counters;
    expected_lookup_cache_hits = s_lookup_cache_hits;
    CHECK(expected_profile.guest_calls == 1U &&
          expected_profile.guest_lookups == 1U &&
          expected_profile.lookup_iterations == 0U);
    CHECK(expected_lookup_cache_hits == 1U);
    memcpy(s_expected, s_arena, ARENA_BYTES);

    memcpy(s_arena, s_initial, ARENA_BYTES);
    c = initial_cpu;
    c.stack_owner = &c;
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    s_lookup_cache_hits = 0U;
    CHECK(isaac_vita_save_read32_guest_try(&c) == 1);
    CHECK(cpu_equal(&c, &expected_cpu));
    CHECK(memcmp(s_arena, s_expected, ARENA_BYTES) == 0);
    CHECK(memcmp(&g_guest_phase_profile_counters, &expected_profile,
                 sizeof expected_profile) == 0);
    CHECK(s_lookup_cache_hits == expected_lookup_cache_hits);
    CHECK(s_function_coverage[READER_COVERAGE_ID] == 1U);
    CHECK(s_function_coverage[CHECKSUM_COVERAGE_ID] == 1U);
    return 0;
}

static int returning_fault_case(enum oracle_fault_mode mode)
{
    CPU c;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;
    uint32_t destination = ARENA_ADDRESS + DEST_OFFSET;
    uint32_t wrapper = ARENA_ADDRESS + WRAPPER_OFFSET;
    unsigned reader_before = s_reader_calls;
    unsigned checksum_before = s_checksum_calls;
    uint32_t checksum_state;

    fixture(&c, 2U, 10U);
    checksum_state = ld32(wrapper + 0x14U);
    s_fault_mode = mode;
    CHECK(isaac_vita_save_read32_guest_try(&c) == 1);
    CHECK(c.fault != NULL);
    if (mode == ORACLE_FAULT_READER) {
        CHECK(c.fault_addr == 0x25ba60U);
        CHECK(c.esp == entry - 28U);
        CHECK(s_reader_calls == reader_before + 1U);
        CHECK(s_checksum_calls == checksum_before);
        CHECK(ld32(destination) == 0xa5a5a5a5U);
    } else {
        CHECK(c.fault_addr == 0x25b340U);
        CHECK(c.esp == entry - 24U);
        CHECK(s_reader_calls == reader_before + 1U);
        CHECK(s_checksum_calls == checksum_before + 1U);
        CHECK(ld32(destination) == 0x25242322U);
    }
    CHECK(ld32(wrapper + 0x14U) == checksum_state);
    s_fault_mode = ORACLE_FAULT_NONE;
    return 0;
}

static int rejected_case(unsigned kind)
{
    CPU c;
    CPU before;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;
    uint32_t wrapper = ARENA_ADDRESS + WRAPPER_OFFSET;
    uint32_t reader = ARENA_ADDRESS + READER_OFFSET;

    fixture(&c, 2U, 10U);
    if (kind == 0U) {
        store_u32(reader, 0x12345678U);
    } else if (kind == 1U) {
        c.ecx = entry - 20U;
        store_u32(c.ecx, reader);
    } else if (kind == 2U) {
        reader = entry - 32U;
        store_u32(wrapper, reader);
        store_u32(reader,
                  (uint32_t)(GUEST_IMAGE_BASE + READER_VTABLE_RVA));
    } else {
        c.stack_floor = entry - 40U;
    }
    before = c;
    memcpy(s_initial, s_arena, ARENA_BYTES);
    CHECK(isaac_vita_save_read32_guest_try(&c) == 0);
    CHECK(cpu_equal(&c, &before));
    CHECK(memcmp(s_arena, s_initial, ARENA_BYTES) == 0);
    return 0;
}

int main(void)
{
    unsigned kind;
    int result = 0;

    if (!oracle_map()) {
        fprintf(stderr, "Save read32 oracle could not map its low arena\n");
        return 2;
    }
    memset(s_function_coverage, 0, sizeof s_function_coverage);
    result |= differential_case(0U, 12U);
    result |= differential_case(8U, 10U);
    result |= differential_case(10U, 10U);
    result |= returning_fault_case(ORACLE_FAULT_READER);
    result |= returning_fault_case(ORACLE_FAULT_CHECKSUM);
    for (kind = 0U; kind < 4U; ++kind)
        result |= rejected_case(kind);
    if (result == 0) {
        CHECK(s_reader_calls == 8U);
        CHECK(s_checksum_calls == 7U);
        printf("Vita Save read32 fused oracle: PASS; checks=%u; "
               "handled=3; returning-fault=2; rejected=4\n", s_checks);
    }
    oracle_unmap();
    return result;
}
