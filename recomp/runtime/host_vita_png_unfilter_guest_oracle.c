/* Hostile host oracle for the frozen png_read_filter_row guest entry shim. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE 1
#endif
#include "host_vita_png_unfilter_guest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_heap.h"
#include "host_vita_png_unfilter_native.h"
#include "kage_vita_png_decode_profile.h"

#if defined(_WIN32)
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "PNG guest shim oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

enum {
    ORACLE_ARENA_ADDRESS = 0x21000000U,
    ORACLE_ARENA_BYTES = 0x00010000U,
    ORACLE_PNG_OFFSET = 0x1000U,
    ORACLE_ROW_OFFSET = 0x2000U,
    ORACLE_PREVIOUS_OFFSET = 0x3000U,
    ORACLE_STACK_OFFSET = 0x4000U,
    ORACLE_ROW_BYTES = 64U,
    ORACLE_ALLOCATION_COUNT = 3U
};

typedef struct oracle_allocation {
    uint32_t base;
    uint32_t size;
    uint32_t token;
} oracle_allocation;

typedef struct oracle_fixture {
    CPU cpu;
    uint32_t png;
    uint32_t row_base;
    uint32_t row;
    uint32_t previous_base;
    uint32_t previous;
    uint32_t stack;
} oracle_fixture;

static uint8_t *s_arena;
static oracle_allocation s_allocations[ORACLE_ALLOCATION_COUNT];
static uint32_t s_next_token;
static uint32_t s_lease_calls;
static uint32_t s_release_calls;
static uint32_t s_fail_lease_call;
static uint32_t s_fail_release_call;
static uint32_t s_helper_calls;
static uint32_t s_helper_result;
static uint32_t s_helper_mutations;
static uint32_t s_note_calls;
static uint32_t s_last_outcome;
static uint32_t s_fault_calls;

static void store_u32le(uint32_t address, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)(uintptr_t)address;

    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static int oracle_map(void)
{
#if defined(_WIN32)
    s_arena = (uint8_t *)VirtualAlloc(
        (void *)(uintptr_t)ORACLE_ARENA_ADDRESS, ORACLE_ARENA_BYTES,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *mapped;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
# if defined(MAP_32BIT)
    flags |= MAP_32BIT;
# endif
    mapped = mmap((void *)(uintptr_t)ORACLE_ARENA_ADDRESS,
                  ORACLE_ARENA_BYTES, PROT_READ | PROT_WRITE,
                  flags, -1, 0);
    s_arena = mapped == MAP_FAILED ? NULL : (uint8_t *)mapped;
#endif
    return s_arena != NULL &&
           (uintptr_t)s_arena <= UINT32_MAX - ORACLE_ARENA_BYTES;
}

static void oracle_unmap(void)
{
    if (!s_arena)
        return;
#if defined(_WIN32)
    (void)VirtualFree(s_arena, 0U, MEM_RELEASE);
#else
    (void)munmap(s_arena, ORACLE_ARENA_BYTES);
#endif
    s_arena = NULL;
}

static void oracle_reset_runtime(void)
{
    memset(s_allocations, 0, sizeof s_allocations);
    s_next_token = 1U;
    s_lease_calls = 0U;
    s_release_calls = 0U;
    s_fail_lease_call = 0U;
    s_fail_release_call = 0U;
    s_helper_calls = 0U;
    s_helper_result = ISAAC_VITA_PNG_UNFILTER_HANDLED;
    s_helper_mutations = 0U;
    s_note_calls = 0U;
    s_last_outcome = UINT32_MAX;
    s_fault_calls = 0U;
}

static void oracle_register(uint32_t index, uint32_t base, uint32_t size)
{
    s_allocations[index].base = base;
    s_allocations[index].size = size;
}

static void oracle_setup(oracle_fixture *fixture)
{
    uint32_t index;

    memset(s_arena, 0x6d, ORACLE_ARENA_BYTES);
    oracle_reset_runtime();
    memset(fixture, 0xa5, sizeof *fixture);
    fixture->png = (uint32_t)(uintptr_t)s_arena + ORACLE_PNG_OFFSET;
    fixture->row_base = (uint32_t)(uintptr_t)s_arena + ORACLE_ROW_OFFSET;
    fixture->row = fixture->row_base + 1U;
    fixture->previous_base =
        (uint32_t)(uintptr_t)s_arena + ORACLE_PREVIOUS_OFFSET;
    fixture->previous = fixture->previous_base + 1U;
    fixture->stack =
        (uint32_t)(uintptr_t)s_arena + ORACLE_STACK_OFFSET + 0x40U;
    fixture->cpu.ecx = fixture->png;
    fixture->cpu.edx = fixture->png + 0xf8U;
    fixture->cpu.esp = fixture->stack;
    fixture->cpu.stack_owner = &fixture->cpu;
    fixture->cpu.stack_floor = fixture->stack - 0x40U;
    fixture->cpu.stack_ceiling = fixture->stack + 0x80U;
    fixture->cpu.stack_low_water = fixture->cpu.stack_floor;
    fixture->cpu.fault = NULL;
    fixture->cpu.fault_addr = 0U;
    store_u32le(fixture->stack, 0x005b17fcU);
    store_u32le(fixture->stack + 4U, fixture->row);
    store_u32le(fixture->stack + 8U, fixture->previous);
    store_u32le(fixture->stack + 12U, 2U);
    store_u32le(fixture->cpu.edx + 4U, ORACLE_ROW_BYTES);
    *(uint8_t *)(uintptr_t)(fixture->cpu.edx + 11U) = 32U;
    for (index = 0U; index < ORACLE_ROW_BYTES; ++index) {
        *(uint8_t *)(uintptr_t)(fixture->row + index) = (uint8_t)(index * 3U);
        *(uint8_t *)(uintptr_t)(fixture->previous + index) =
            (uint8_t)(index * 5U + 1U);
    }
    oracle_register(0U, fixture->png, 0x200U);
    oracle_register(1U, fixture->row_base, ORACLE_ROW_BYTES + 1U);
    oracle_register(2U, fixture->previous_base, ORACLE_ROW_BYTES + 1U);
}

uint32_t isaac_vita_guest_heap_lease_exact_range(
    const void *allocation_pointer, const void *range_pointer, size_t size)
{
    const uintptr_t base = (uintptr_t)allocation_pointer;
    const uintptr_t range = (uintptr_t)range_pointer;
    uint32_t index;

    ++s_lease_calls;
    if (s_fail_lease_call == s_lease_calls)
        return 0U;
    for (index = 0U; index < ORACLE_ALLOCATION_COUNT; ++index) {
        oracle_allocation *allocation = &s_allocations[index];
        uintptr_t offset;

        if (allocation->base != base || allocation->token != 0U ||
                range < base)
            continue;
        offset = range - base;
        if (size == 0U || offset > allocation->size ||
                size > allocation->size - offset)
            return 0U;
        allocation->token = s_next_token++;
        return allocation->token;
    }
    return 0U;
}

int isaac_vita_guest_heap_lease_release(uint32_t token)
{
    uint32_t index;

    ++s_release_calls;
    if (s_fail_release_call == s_release_calls)
        return 0;
    for (index = 0U; index < ORACLE_ALLOCATION_COUNT; ++index)
        if (s_allocations[index].token == token) {
            s_allocations[index].token = 0U;
            return 1;
        }
    return 0;
}

int isaac_vita_png_unfilter_try(
    const void *row_info, size_t row_info_capacity,
    uint8_t *row, size_t row_capacity,
    const uint8_t *previous_row, size_t previous_row_capacity,
    uint32_t filter_type)
{
    ++s_helper_calls;
    if (row_info_capacity != ISAAC_VITA_PNG_ROW_INFO_BYTES ||
            row_capacity != ORACLE_ROW_BYTES || filter_type > 4U ||
            (filter_type >= 2U &&
             (previous_row == NULL ||
              previous_row_capacity != ORACLE_ROW_BYTES)) ||
            row_info == NULL || row == NULL)
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;
    if (s_helper_result == ISAAC_VITA_PNG_UNFILTER_HANDLED) {
        row[0] ^= 0x5aU;
        ++s_helper_mutations;
    }
    return (int)s_helper_result;
}

void kage_vita_png_profile_native_result(uint32_t outcome)
{
    ++s_note_calls;
    s_last_outcome = outcome;
}

uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value;

    memcpy(&value, (const void *)(uintptr_t)c->esp, sizeof value);
    c->esp += 4U;
    return value;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
    c->fault_addr = address;
    c->fault = what;
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind;
    (void)address;
    (void)size;
    guest_fault(c, pc, "PNG oracle stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(
        c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
}

static int oracle_no_live_leases(void)
{
    uint32_t index;

    for (index = 0U; index < ORACLE_ALLOCATION_COUNT; ++index)
        if (s_allocations[index].token != 0U)
            return 0;
    return 1;
}

static int expect_fallback(oracle_fixture *fixture, uint32_t outcome)
{
    CPU before = fixture->cpu;
    uint8_t stack_before[16U];
    uint8_t row_info_before[ISAAC_VITA_PNG_ROW_INFO_BYTES];
    uint8_t row_before[ORACLE_ROW_BYTES];
    uint8_t previous_before[ORACLE_ROW_BYTES];

    memcpy(stack_before, (const void *)(uintptr_t)fixture->stack,
           sizeof stack_before);
    memcpy(row_info_before, (const void *)(uintptr_t)fixture->cpu.edx,
           sizeof row_info_before);
    memcpy(row_before, (const void *)(uintptr_t)fixture->row, sizeof row_before);
    memcpy(previous_before, (const void *)(uintptr_t)fixture->previous,
           sizeof previous_before);
    CHECK(isaac_vita_png_unfilter_guest_try(&fixture->cpu) == 0);
    CHECK(memcmp(&fixture->cpu, &before, sizeof before) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture->stack,
                 stack_before, sizeof stack_before) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture->cpu.edx,
                 row_info_before, sizeof row_info_before) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture->row,
                 row_before, sizeof row_before) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture->previous,
                 previous_before, sizeof previous_before) == 0);
    CHECK(s_helper_mutations == 0U && s_fault_calls == 0U);
    CHECK(s_note_calls == 1U && s_last_outcome == outcome);
    CHECK(oracle_no_live_leases());
    return 0;
}

static int fallback_oracle(void)
{
    oracle_fixture fixture;

    oracle_setup(&fixture);
    fixture.cpu.stack_ceiling = fixture.stack + 12U;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_STACK) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack, 0x005b1801U);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ABI) == 0);

    oracle_setup(&fixture);
    ++fixture.cpu.edx;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ABI) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 12U, 5U);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_FILTER) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.cpu.edx + 4U, 0U);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_METADATA) == 0);

    oracle_setup(&fixture);
    *(uint8_t *)(uintptr_t)(fixture.cpu.edx + 11U) = 65U;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_METADATA) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.cpu.edx + 4U, 1U);
    *(uint8_t *)(uintptr_t)(fixture.cpu.edx + 11U) = 16U;
    store_u32le(fixture.stack + 12U, 1U);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_METADATA) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 4U, UINT32_MAX);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_RANGE) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 8U, UINT32_MAX);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_RANGE) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 4U, fixture.cpu.edx);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ALIAS) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 8U, fixture.row);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ALIAS) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 8U, fixture.cpu.edx);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ALIAS) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 4U, fixture.stack);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ALIAS) == 0);

    oracle_setup(&fixture);
    store_u32le(fixture.stack + 8U, fixture.stack);
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ALIAS) == 0);

    oracle_setup(&fixture);
    fixture.png = fixture.stack - 0xf8U;
    fixture.cpu.ecx = fixture.png;
    fixture.cpu.edx = fixture.stack;
    oracle_register(0U, fixture.png, 0x200U);
    store_u32le(fixture.cpu.edx + 4U, ORACLE_ROW_BYTES);
    *(uint8_t *)(uintptr_t)(fixture.cpu.edx + 11U) = 32U;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_ALIAS) == 0);

    oracle_setup(&fixture);
    s_allocations[1].base++;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_RANGE) == 0);

    oracle_setup(&fixture);
    s_allocations[2].base++;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_RANGE) == 0);

    oracle_setup(&fixture);
    s_helper_result = ISAAC_VITA_PNG_UNFILTER_FALLBACK;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_HELPER) == 0);
    CHECK(s_helper_calls == 1U);

    oracle_setup(&fixture);
    s_fail_lease_call = 2U;
    CHECK(expect_fallback(&fixture, KAGE_VITA_PNG_NATIVE_REJECT_RANGE) == 0);
    return 0;
}

static int handled_oracle(void)
{
    oracle_fixture fixture;
    CPU expected;
    uint8_t stack_before[16U];
    uint8_t previous_before[ORACLE_ROW_BYTES];
    uint8_t before;

    oracle_setup(&fixture);
    expected = fixture.cpu;
    expected.esp += 4U;
    memcpy(stack_before, (const void *)(uintptr_t)fixture.stack,
           sizeof stack_before);
    memcpy(previous_before, (const void *)(uintptr_t)fixture.previous,
           sizeof previous_before);
    before = *(uint8_t *)(uintptr_t)fixture.row;
    CHECK(isaac_vita_png_unfilter_guest_try(&fixture.cpu) == 1);
    CHECK(memcmp(&fixture.cpu, &expected, sizeof expected) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture.stack,
                 stack_before, sizeof stack_before) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture.previous,
                 previous_before, sizeof previous_before) == 0);
    CHECK(*(uint8_t *)(uintptr_t)fixture.row == (uint8_t)(before ^ 0x5aU));
    CHECK(s_helper_calls == 1U && s_helper_mutations == 1U);
    CHECK(s_note_calls == 1U &&
          s_last_outcome == KAGE_VITA_PNG_NATIVE_HANDLED);
    CHECK(s_fault_calls == 0U && oracle_no_live_leases());

    /* None must not inspect or lease the unused previous-row argument. */
    oracle_setup(&fixture);
    store_u32le(fixture.stack + 8U, UINT32_MAX);
    store_u32le(fixture.stack + 12U, 0U);
    expected = fixture.cpu;
    expected.esp += 4U;
    memcpy(stack_before, (const void *)(uintptr_t)fixture.stack,
           sizeof stack_before);
    CHECK(isaac_vita_png_unfilter_guest_try(&fixture.cpu) == 1);
    CHECK(memcmp(&fixture.cpu, &expected, sizeof expected) == 0);
    CHECK(memcmp((const void *)(uintptr_t)fixture.stack,
                 stack_before, sizeof stack_before) == 0);
    CHECK(s_lease_calls == 2U && s_release_calls == 2U);
    CHECK(s_helper_calls == 1U && s_helper_mutations == 1U);
    CHECK(oracle_no_live_leases());
    return 0;
}

static int release_failure_oracle(void)
{
    oracle_fixture fixture;
    uint8_t before;

    oracle_setup(&fixture);
    s_fail_release_call = 1U;
    before = *(uint8_t *)(uintptr_t)fixture.row;
    CHECK(isaac_vita_png_unfilter_guest_try(&fixture.cpu) == 1);
    CHECK(s_helper_calls == 1U && s_helper_mutations == 1U);
    CHECK(*(uint8_t *)(uintptr_t)fixture.row == (uint8_t)(before ^ 0x5aU));
    CHECK(s_fault_calls == 1U && fixture.cpu.fault != NULL);
    CHECK(fixture.cpu.esp == fixture.stack + 4U);
    CHECK(s_note_calls == 1U &&
          s_last_outcome == KAGE_VITA_PNG_NATIVE_REJECT_RELEASE);
    CHECK(s_release_calls == 3U);

    oracle_setup(&fixture);
    s_helper_result = ISAAC_VITA_PNG_UNFILTER_FALLBACK;
    s_fail_release_call = 1U;
    before = *(uint8_t *)(uintptr_t)fixture.row;
    CHECK(isaac_vita_png_unfilter_guest_try(&fixture.cpu) == 1);
    CHECK(s_helper_calls == 1U && s_helper_mutations == 0U);
    CHECK(*(uint8_t *)(uintptr_t)fixture.row == before);
    CHECK(s_fault_calls == 1U && fixture.cpu.fault != NULL);
    CHECK(fixture.cpu.esp == fixture.stack + 4U);
    CHECK(s_note_calls == 1U &&
          s_last_outcome == KAGE_VITA_PNG_NATIVE_REJECT_RELEASE);
    CHECK(s_release_calls == 3U);
    return 0;
}

int main(void)
{
    CHECK(oracle_map());
    oracle_reset_runtime();
    CHECK(isaac_vita_png_unfilter_guest_try(NULL) == 0);
    CHECK(s_note_calls == 1U &&
          s_last_outcome == KAGE_VITA_PNG_NATIVE_REJECT_STACK);
    CHECK(fallback_oracle() == 0);
    CHECK(handled_oracle() == 0);
    CHECK(release_failure_oracle() == 0);
    oracle_unmap();
    puts("PNG native guest shim oracle: PASS; hostile=22; release-fail=2");
    return 0;
}
