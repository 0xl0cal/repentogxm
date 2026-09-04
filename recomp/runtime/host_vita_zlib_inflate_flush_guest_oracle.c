#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "host_vita_zlib_inflate_flush_guest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest_pe.h"
#include "host_vita_heap.h"
#include "host_vita_zlib_inflate_flush_native.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "zlib inflate_flush guest oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        oracle_unmap(); \
        return 1; \
    } \
} while (0)

enum {
    ARENA_ADDRESS = 0x22000000U,
    ARENA_BYTES = 0x00020000U,
    PNG_OFFSET = 0x1000U,
    INFLATE_STATE_OFFSET = 0x2000U,
    BLOCKS_OFFSET = 0x3000U,
    WINDOW_OFFSET = 0x4000U,
    OUTPUT_OFFSET = 0xd000U,
    STACK_OFFSET = 0x10000U,
    WINDOW_BYTES = 0x8000U,
    OUTPUT_BYTES = 0x2000U,
    ALLOCATION_COUNT = 5U
};

typedef struct oracle_allocation {
    uint32_t base;
    uint32_t size;
    uint32_t token;
} oracle_allocation;

typedef struct oracle_fixture {
    CPU cpu;
    uint32_t png;
    uint32_t zstream;
    uint32_t inflate_state;
    uint32_t blocks;
    uint32_t window;
    uint32_t output;
    uint32_t stack;
} oracle_fixture;

static uint8_t *s_arena;
static oracle_allocation s_allocations[ALLOCATION_COUNT];
static uint32_t s_next_token;
static uint32_t s_lease_calls;
static uint32_t s_release_calls;
static uint32_t s_fail_lease_call;
static uint32_t s_fail_release_call;
static uint32_t s_fault_calls;
static uint32_t s_translated_notes;
static uint32_t s_import_notes;
static uint32_t s_fail_translated_note;
static uint32_t s_fail_import_note;

static void oracle_unmap(void);

static void store_u32le(uint32_t address, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)(uintptr_t)address;

    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t load_u32le(uint32_t address)
{
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;

    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int oracle_map(void)
{
#if defined(_WIN32)
    s_arena = (uint8_t *)VirtualAlloc(
        (void *)(uintptr_t)ARENA_ADDRESS, ARENA_BYTES,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void *mapped;
#if defined(MAP_32BIT)
    flags |= MAP_32BIT;
#endif
    mapped = mmap((void *)(uintptr_t)ARENA_ADDRESS, ARENA_BYTES,
                  PROT_READ | PROT_WRITE, flags, -1, 0);
    s_arena = mapped == MAP_FAILED ? NULL : (uint8_t *)mapped;
#endif
    return s_arena != NULL &&
           (uintptr_t)s_arena <= UINT32_MAX - ARENA_BYTES;
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

static void oracle_runtime_reset(void)
{
    memset(s_allocations, 0, sizeof s_allocations);
    s_next_token = 1U;
    s_lease_calls = 0U;
    s_release_calls = 0U;
    s_fail_lease_call = 0U;
    s_fail_release_call = 0U;
    s_fault_calls = 0U;
    s_translated_notes = 0U;
    s_import_notes = 0U;
    s_fail_translated_note = 0U;
    s_fail_import_note = 0U;
}

static void oracle_register(uint32_t index, uint32_t base, uint32_t size)
{
    s_allocations[index].base = base;
    s_allocations[index].size = size;
}

static void oracle_setup(oracle_fixture *fixture)
{
    uint32_t base = (uint32_t)(uintptr_t)s_arena;
    uint32_t index;

    memset(s_arena, 0x6d, ARENA_BYTES);
    oracle_runtime_reset();
    memset(fixture, 0, sizeof *fixture);
    fixture->png = base + PNG_OFFSET;
    fixture->zstream = fixture->png + 0x6cU;
    fixture->inflate_state = base + INFLATE_STATE_OFFSET;
    fixture->blocks = base + BLOCKS_OFFSET;
    fixture->window = base + WINDOW_OFFSET;
    fixture->output = base + OUTPUT_OFFSET;
    fixture->stack = base + STACK_OFFSET + 0x40U;

    fixture->cpu.ecx = fixture->blocks;
    fixture->cpu.edx = fixture->zstream;
    fixture->cpu.esp = fixture->stack;
    fixture->cpu.stack_owner = &fixture->cpu;
    fixture->cpu.stack_floor = fixture->stack - 0x40U;
    fixture->cpu.stack_ceiling = fixture->stack + 0x80U;
    fixture->cpu.stack_low_water = fixture->cpu.stack_floor;

    store_u32le(fixture->stack, 0x005d70d9U);
    store_u32le(fixture->stack + 4U, (uint32_t)-5);
    store_u32le(fixture->png + 0xa4U, fixture->output);
    store_u32le(fixture->png + 0xa8U, OUTPUT_BYTES);
    store_u32le(fixture->zstream + 0x0cU, fixture->output);
    store_u32le(fixture->zstream + 0x10U, 64U);
    store_u32le(fixture->zstream + 0x14U, 0xfffffff0U);
    store_u32le(fixture->zstream + 0x1cU, fixture->inflate_state);
    store_u32le(fixture->zstream + 0x30U, 1U);
    store_u32le(fixture->inflate_state + 0x14U, fixture->blocks);
    store_u32le(fixture->blocks + 0x28U, fixture->window);
    store_u32le(fixture->blocks + 0x2cU,
                fixture->window + WINDOW_BYTES);
    store_u32le(fixture->blocks + 0x30U,
                fixture->window + WINDOW_BYTES - 16U);
    store_u32le(fixture->blocks + 0x34U, fixture->window + 32U);
    store_u32le(fixture->blocks + 0x38U,
                GUEST_PE_VITA_TARGET_BASE + 0x005cf3d0U);
    store_u32le(fixture->blocks + 0x3cU, 1U);
    for (index = 0U; index < WINDOW_BYTES; ++index)
        *(uint8_t *)(uintptr_t)(fixture->window + index) =
            (uint8_t)(index * 13U + 7U);
    memset((void *)(uintptr_t)fixture->output, 0xcc, OUTPUT_BYTES);

    oracle_register(0U, fixture->png, 0x20cU);
    oracle_register(1U, fixture->inflate_state, 0x18U);
    oracle_register(2U, fixture->blocks, 0x40U);
    oracle_register(3U, fixture->window, WINDOW_BYTES);
    oracle_register(4U, fixture->output, OUTPUT_BYTES);
}

uint32_t isaac_vita_guest_heap_lease_exact_range(
    const void *allocation_pointer, const void *range_pointer, size_t size)
{
    uintptr_t base = (uintptr_t)allocation_pointer;
    uintptr_t range = (uintptr_t)range_pointer;
    uint32_t index;

    ++s_lease_calls;
    if (s_fail_lease_call == s_lease_calls)
        return 0U;
    for (index = 0U; index < ALLOCATION_COUNT; ++index) {
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
    for (index = 0U; index < ALLOCATION_COUNT; ++index) {
        if (s_allocations[index].token == token) {
            s_allocations[index].token = 0U;
            return 1;
        }
    }
    return 0;
}

int guest_note_authenticated_translated_call(
    uint32_t address, uint32_t coverage_function_id)
{
    ++s_translated_notes;
    if (address != 0x005cf3d0U || coverage_function_id != 12124U)
        return 0;
    return s_fail_translated_note != s_translated_notes;
}

int guest_note_authenticated_import_call(
    uint32_t address, uint32_t import_id, uint32_t thunk_function_id)
{
    ++s_import_notes;
    if (address != 0x00606488U || import_id != 281U ||
            thunk_function_id != 12570U)
        return 0;
    return s_fail_import_note != s_import_notes;
}

uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value = load_u32le(c->esp);

    c->esp += 4U;
    return value;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
    c->fault_addr = address;
    c->fault = what;
}

static int run_success_case(void)
{
    oracle_fixture fixture;
    uint8_t expected[48];
    uint32_t expected_check;

    oracle_setup(&fixture);
    memcpy(expected,
           (const void *)(uintptr_t)(fixture.window + WINDOW_BYTES - 16U),
           16U);
    memcpy(expected + 16U, (const void *)(uintptr_t)fixture.window, 32U);
    expected_check = isaac_vita_zlib_114_adler32(
        1U, (const uint8_t *)(uintptr_t)(
            fixture.window + WINDOW_BYTES - 16U), 16U);
    expected_check = isaac_vita_zlib_114_adler32(
        expected_check, (const uint8_t *)(uintptr_t)fixture.window, 32U);

    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 1);
    CHECK(fixture.cpu.fault == NULL && s_fault_calls == 0U);
    CHECK(fixture.cpu.esp == fixture.stack + 4U && fixture.cpu.eax == 0U);
    CHECK(s_lease_calls == 5U && s_release_calls == 5U);
    CHECK(s_translated_notes == 2U && s_import_notes == 2U);
    CHECK(memcmp((const void *)(uintptr_t)fixture.output,
                 expected, sizeof expected) == 0);
    CHECK(load_u32le(fixture.blocks + 0x30U) == fixture.window + 32U);
    CHECK(load_u32le(fixture.blocks + 0x34U) == fixture.window + 32U);
    CHECK(load_u32le(fixture.zstream + 0x0cU) == fixture.output + 48U);
    CHECK(load_u32le(fixture.zstream + 0x10U) == 16U);
    CHECK(load_u32le(fixture.zstream + 0x14U) == 0x20U);
    CHECK(load_u32le(fixture.blocks + 0x3cU) == expected_check);
    CHECK(load_u32le(fixture.zstream + 0x30U) == expected_check);
    return 0;
}

static int run_no_check_case(void)
{
    oracle_fixture fixture;

    oracle_setup(&fixture);
    store_u32le(fixture.blocks + 0x30U, fixture.window);
    store_u32le(fixture.blocks + 0x34U, fixture.window + 8U);
    store_u32le(fixture.blocks + 0x38U, 0U);
    store_u32le(fixture.blocks + 0x3cU, 0x12345678U);
    store_u32le(fixture.zstream + 0x30U, 0x87654321U);
    store_u32le(fixture.zstream + 0x10U, 8U);
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 1);
    CHECK(s_translated_notes == 0U && s_import_notes == 1U);
    CHECK(load_u32le(fixture.blocks + 0x3cU) == 0x12345678U);
    CHECK(load_u32le(fixture.zstream + 0x30U) == 0x87654321U);
    return 0;
}

static int run_lease_rejections(void)
{
    oracle_fixture fixture;
    CPU cpu_before;
    static uint8_t arena_before[ARENA_BYTES];
    uint32_t failed;

    for (failed = 1U; failed <= 5U; ++failed) {
        oracle_setup(&fixture);
        s_fail_lease_call = failed;
        cpu_before = fixture.cpu;
        memcpy(arena_before, s_arena, ARENA_BYTES);
        CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 0);
        CHECK(memcmp(&fixture.cpu, &cpu_before, sizeof cpu_before) == 0);
        CHECK(memcmp(s_arena, arena_before, ARENA_BYTES) == 0);
        CHECK(s_fault_calls == 0U && s_translated_notes == 0U &&
              s_import_notes == 0U);
    }
    return 0;
}

static int run_metadata_rejections(void)
{
    oracle_fixture fixture;
    CPU cpu_before;
    static uint8_t arena_before[ARENA_BYTES];

    oracle_setup(&fixture);
    store_u32le(fixture.stack, 0x12345678U);
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 0);
    CHECK(s_lease_calls == 0U);

#define REJECT_AFTER(change) do { \
    oracle_setup(&fixture); \
    change; \
    cpu_before = fixture.cpu; \
    memcpy(arena_before, s_arena, ARENA_BYTES); \
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 0); \
    CHECK(memcmp(&fixture.cpu, &cpu_before, sizeof cpu_before) == 0); \
    CHECK(memcmp(s_arena, arena_before, ARENA_BYTES) == 0); \
    CHECK(s_fault_calls == 0U); \
} while (0)

    REJECT_AFTER(store_u32le(fixture.png + 0xa8U, 0x1000U));
    REJECT_AFTER(store_u32le(fixture.inflate_state + 0x14U,
                             fixture.blocks + 4U));
    REJECT_AFTER(store_u32le(fixture.blocks + 0x2cU,
                             fixture.window + WINDOW_BYTES - 1U));
    REJECT_AFTER(store_u32le(fixture.blocks + 0x30U,
                             fixture.window - 1U));
    REJECT_AFTER(store_u32le(fixture.blocks + 0x38U, 0x985cf3d4U));
    REJECT_AFTER(store_u32le(fixture.zstream + 0x0cU,
                             fixture.output + OUTPUT_BYTES + 1U));
    REJECT_AFTER(store_u32le(fixture.zstream + 0x10U,
                             OUTPUT_BYTES + 1U));
#undef REJECT_AFTER
    return 0;
}

static int run_returning_fault_cases(void)
{
    oracle_fixture fixture;

    /* Release failure while rejecting metadata must consume the return word;
     * a returning guest_fault stub must never enter the translated body with
     * an uncertain live lease. */
    oracle_setup(&fixture);
    store_u32le(fixture.png + 0xa8U, 0x1000U);
    s_fail_release_call = 1U;
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 1);
    CHECK(s_fault_calls == 1U && fixture.cpu.esp == fixture.stack + 4U);

    oracle_setup(&fixture);
    s_fail_release_call = 1U;
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 1);
    CHECK(s_fault_calls == 1U && fixture.cpu.esp == fixture.stack + 4U);
    CHECK(s_translated_notes == 2U && s_import_notes == 2U);

    oracle_setup(&fixture);
    s_fail_translated_note = 1U;
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 1);
    CHECK(s_fault_calls == 1U && fixture.cpu.esp == fixture.stack + 4U);
    CHECK(s_import_notes == 0U);

    oracle_setup(&fixture);
    s_fail_import_note = 1U;
    CHECK(isaac_vita_zlib_inflate_flush_guest_try(&fixture.cpu) == 1);
    CHECK(s_fault_calls == 1U && fixture.cpu.esp == fixture.stack + 4U);
    CHECK(s_translated_notes == 1U && s_import_notes == 1U);
    return 0;
}

int main(void)
{
    CHECK(oracle_map());
    CHECK(run_success_case() == 0);
    CHECK(run_no_check_case() == 0);
    CHECK(run_lease_rejections() == 0);
    CHECK(run_metadata_rejections() == 0);
    CHECK(run_returning_fault_cases() == 0);
    oracle_unmap();
    printf("zlib inflate_flush guest oracle: PASS; leases=5 hostile=16\n");
    return 0;
}
