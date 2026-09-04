#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "host_vita_archive_miniz_guest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_heap.h"
#include "host_vita_archive_miniz_native.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "archive MiniZ guest oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        oracle_unmap(); \
        return 1; \
    } \
} while (0)

enum {
    ARENA_ADDRESS = 0x22400000U,
    ARENA_BYTES = 0x00020000U,
    STATE_OFFSET = 0x2000U,
    OBJECT_OFFSET = 0x6000U,
    STACK_OFFSET = 0x10000U,
    OBJECT_BYTES = 0x0c1cU,
    INPUT_OFFSET = 0x1cU,
    OUTPUT_OFFSET = 0x81cU,
    INPUT_SIZE_FRAME_OFFSET = 0x90U,
    OUTPUT_SIZE_FRAME_OFFSET = 0x98U,
    ALLOCATION_COUNT = 2U
};

typedef struct oracle_allocation {
    uint32_t base;
    uint32_t size;
    uint32_t token;
} oracle_allocation;

typedef struct oracle_fixture {
    CPU cpu;
    uint32_t state;
    uint32_t object;
    uint32_t input;
    uint32_t output;
    uint32_t stack;
    uint32_t frame;
    uint32_t input_size_address;
    uint32_t output_size_address;
} oracle_fixture;

static uint8_t *s_arena;
static oracle_allocation s_allocations[ALLOCATION_COUNT];
static uint32_t s_next_token;
static uint32_t s_lease_calls;
static uint32_t s_release_calls;
static uint32_t s_fail_lease_call;
static uint32_t s_fail_release_call;
static uint32_t s_fault_calls;

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

static void oracle_setup(oracle_fixture *fixture)
{
    static const uint8_t raw_stored[] = {
        0x01, 0x0c, 0x00, 0xf3, 0xff,
        'h', 'e', 'l', 'l', 'o', ' ', 'm', 'i', 'n', 'i', 'z', '!'
    };
    uint32_t base = (uint32_t)(uintptr_t)s_arena;

    memset(s_arena, 0x6d, ARENA_BYTES);
    memset(fixture, 0, sizeof *fixture);
    memset(s_allocations, 0, sizeof s_allocations);
    s_next_token = 1U;
    s_lease_calls = 0U;
    s_release_calls = 0U;
    s_fail_lease_call = 0U;
    s_fail_release_call = 0U;
    s_fault_calls = 0U;

    fixture->state = base + STATE_OFFSET;
    fixture->object = base + OBJECT_OFFSET;
    fixture->input = fixture->object + INPUT_OFFSET;
    fixture->output = fixture->object + OUTPUT_OFFSET;
    fixture->stack = base + STACK_OFFSET + 0x40U;
    fixture->frame = base + STACK_OFFSET + 0x200U;
    fixture->input_size_address = fixture->frame - INPUT_SIZE_FRAME_OFFSET;
    fixture->output_size_address = fixture->frame - OUTPUT_SIZE_FRAME_OFFSET;

    memset((void *)(uintptr_t)fixture->state, 0,
           ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES);
    memcpy((void *)(uintptr_t)fixture->input, raw_stored, sizeof raw_stored);
    memset((void *)(uintptr_t)fixture->output, 0xcc,
           ISAAC_VITA_ARCHIVE_MINIZ_OUTPUT_BYTES);
    store_u32le(fixture->input_size_address, (uint32_t)sizeof raw_stored);
    store_u32le(fixture->output_size_address,
                ISAAC_VITA_ARCHIVE_MINIZ_OUTPUT_BYTES);
    store_u32le(fixture->stack, 0x0059c3f8U);
    store_u32le(fixture->stack + 4U, fixture->input_size_address);
    store_u32le(fixture->stack + 8U, fixture->output);
    store_u32le(fixture->stack + 12U, fixture->output);
    store_u32le(fixture->stack + 16U, fixture->output_size_address);
    store_u32le(fixture->stack + 20U, 0U);

    fixture->cpu.eax = 0x11111111U;
    fixture->cpu.ecx = fixture->state;
    fixture->cpu.edx = fixture->input;
    fixture->cpu.ebx = fixture->output;
    fixture->cpu.esp = fixture->stack;
    fixture->cpu.ebp = fixture->frame;
    fixture->cpu.esi = 0x22222222U;
    fixture->cpu.edi = fixture->object;
    fixture->cpu.stack_owner = &fixture->cpu;
    fixture->cpu.stack_floor = base + STACK_OFFSET;
    fixture->cpu.stack_ceiling = base + STACK_OFFSET + 0x400U;
    fixture->cpu.stack_low_water = fixture->cpu.stack_floor;

    s_allocations[0].base = fixture->state;
    s_allocations[0].size = ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES;
    s_allocations[1].base = fixture->object;
    s_allocations[1].size = OBJECT_BYTES;
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
    CPU before;

    oracle_setup(&fixture);
    before = fixture.cpu;
    CHECK(isaac_vita_archive_miniz_guest_try(&fixture.cpu) == 1);
    CHECK(fixture.cpu.fault == NULL && s_fault_calls == 0U);
    CHECK(fixture.cpu.esp == fixture.stack + 4U);
    CHECK(fixture.cpu.eax == ISAAC_VITA_ARCHIVE_MINIZ_DONE);
    CHECK(fixture.cpu.ecx == before.ecx && fixture.cpu.edx == before.edx &&
          fixture.cpu.ebx == before.ebx && fixture.cpu.ebp == before.ebp &&
          fixture.cpu.esi == before.esi && fixture.cpu.edi == before.edi);
    CHECK(s_lease_calls == 2U && s_release_calls == 2U);
    CHECK(load_u32le(fixture.input_size_address) == 17U);
    CHECK(load_u32le(fixture.output_size_address) == 12U);
    CHECK(memcmp((const void *)(uintptr_t)fixture.output,
                 "hello miniz!", 12U) == 0);
    return 0;
}

static int run_lease_rejections(void)
{
    oracle_fixture fixture;
    CPU before;
    static uint8_t arena_before[ARENA_BYTES];
    uint32_t failed;

    for (failed = 1U; failed <= 2U; ++failed) {
        oracle_setup(&fixture);
        s_fail_lease_call = failed;
        before = fixture.cpu;
        memcpy(arena_before, s_arena, ARENA_BYTES);
        CHECK(isaac_vita_archive_miniz_guest_try(&fixture.cpu) == 0);
        CHECK(memcmp(&fixture.cpu, &before, sizeof before) == 0);
        CHECK(memcmp(s_arena, arena_before, ARENA_BYTES) == 0);
        CHECK(s_fault_calls == 0U);
    }
    return 0;
}

static int reject_unchanged(oracle_fixture *fixture)
{
    CPU before = fixture->cpu;
    static uint8_t arena_before[ARENA_BYTES];

    memcpy(arena_before, s_arena, ARENA_BYTES);
    if (isaac_vita_archive_miniz_guest_try(&fixture->cpu) != 0 ||
            memcmp(&fixture->cpu, &before, sizeof before) != 0 ||
            memcmp(s_arena, arena_before, ARENA_BYTES) != 0 ||
            s_fault_calls != 0U) {
        return 0;
    }
    return 1;
}

static int run_metadata_rejections(void)
{
    oracle_fixture fixture;

#define REJECT_AFTER(change) do { \
    oracle_setup(&fixture); \
    change; \
    CHECK(reject_unchanged(&fixture)); \
} while (0)

    CHECK(isaac_vita_archive_miniz_guest_try(NULL) == 0);
    REJECT_AFTER(fixture.cpu.stack_ceiling = fixture.stack + 20U);
    REJECT_AFTER(store_u32le(fixture.stack, 0x12345678U));
    REJECT_AFTER(store_u32le(fixture.stack + 4U,
                             fixture.input_size_address + 4U));
    REJECT_AFTER(store_u32le(fixture.stack + 16U,
                             fixture.output_size_address + 4U));
    REJECT_AFTER(fixture.cpu.edi += 1U);
    REJECT_AFTER(store_u32le(fixture.stack + 8U, fixture.output + 1U));
    REJECT_AFTER(store_u32le(fixture.stack + 12U, fixture.output + 1U));
    REJECT_AFTER(fixture.cpu.ebx += 1U);
    REJECT_AFTER(store_u32le(fixture.stack + 20U, 1U));
    REJECT_AFTER(store_u32le(fixture.input_size_address, 0x800U));
    REJECT_AFTER(store_u32le(fixture.output_size_address, 0x3ffU));
    REJECT_AFTER(fixture.cpu.ecx = 0U);
    REJECT_AFTER(fixture.cpu.ebp = 0x40U);
#undef REJECT_AFTER
    return 0;
}

static int run_returning_fault_cases(void)
{
    oracle_fixture fixture;

    oracle_setup(&fixture);
    s_fail_lease_call = 2U;
    s_fail_release_call = 1U;
    CHECK(isaac_vita_archive_miniz_guest_try(&fixture.cpu) == 1);
    CHECK(s_fault_calls == 1U && fixture.cpu.esp == fixture.stack + 4U);

    oracle_setup(&fixture);
    s_fail_release_call = 1U;
    CHECK(isaac_vita_archive_miniz_guest_try(&fixture.cpu) == 1);
    CHECK(s_fault_calls == 1U && fixture.cpu.esp == fixture.stack + 4U);
    return 0;
}

int main(void)
{
    CHECK(oracle_map());
    CHECK(run_success_case() == 0);
    CHECK(run_lease_rejections() == 0);
    CHECK(run_metadata_rejections() == 0);
    CHECK(run_returning_fault_cases() == 0);
    oracle_unmap();
    printf("archive MiniZ guest oracle: PASS; leases=2 hostile=18\n");
    return 0;
}
