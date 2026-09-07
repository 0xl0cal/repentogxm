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
    ALLOCATION_COUNT = 3U
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

#if GUEST_GENERATED_STACK_GUARD
uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value = load_u32le(c->esp);

    c->esp += 4U;
    return value;
}
#endif

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_fault_calls;
    c->fault_addr = address;
    c->fault = what;
}

#if !defined(ISAAC_VITA_ARCHIVE_XOR_FASTPATH)
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
#else
/* Same lease/CPU fixture, but compare the opt-in XOR wrapper against both
 * functions freshly translated by test_vita_archive_miniz_fastpath.py. */
#include "archive_xor_generated.h"

int isaac_vita_archive_xor_try(CPU *__restrict c);
void __wrap_sub_005b06f0(CPU *__restrict c);
void sub_005b06f0(CPU *__restrict c);
void archive_xor_original_refresh_body(CPU *__restrict c);
#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
int isaac_vita_archive_xor_refresh_oracle(CPU *__restrict c);
#endif
unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;
static unsigned s_original_calls;
static unsigned s_original_refresh_calls;
static unsigned s_table_mutated;
static void *s_table_page;

void __real_sub_005b06f0(CPU *__restrict c)
{
    ++s_original_calls;
    sub_005b06f0(c);
}

void sub_005c2fd0(CPU *__restrict c)
{
    ++s_original_refresh_calls;
    archive_xor_original_refresh_body(c);
}

void guest_call(CPU *__restrict c, uint32_t target)
{
    guest_fault(c, target, "unexpected generated archive indirect call");
}

#if GUEST_GENERATED_STACK_GUARD
void gpush_generated(CPU *__restrict c, uint32_t value)
{
    if (c->esp < 4U || !guest_stack_contains(c, c->esp - 4U, 4U)) {
        guest_fault(c, c->esp, "oracle stack push");
        return;
    }
    c->esp -= 4U;
    guest_stack_note_low(c, c->esp);
    store_u32le(c->esp, value);
}

int guest_stack_set_generated(CPU *__restrict c, uint32_t value)
{
    if (value < c->stack_floor || value > c->stack_ceiling) {
        guest_fault(c, value, "oracle stack set");
        return 0;
    }
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

int guest_stack_adjust_generated(CPU *__restrict c, uint32_t amount)
{
    return guest_stack_set_generated(c, c->esp + amount);
}
#endif

static int xor_map_table(void)
{
    uintptr_t page = ARCHIVE_XOR_TABLE_ADDRESS & ~(uintptr_t)0xffffU;
#if defined(_WIN32)
    s_table_page = VirtualAlloc((void *)page, 0x10000U,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    s_table_page = mmap((void *)page, 0x10000U, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (s_table_page != (void *)page) {
#if !defined(_WIN32)
        if (s_table_page != MAP_FAILED)
            (void)munmap(s_table_page, 0x10000U);
#endif
        return 0;
    }
    memcpy((void *)(uintptr_t)ARCHIVE_XOR_TABLE_ADDRESS,
           archive_xor_table, sizeof archive_xor_table);
    return 1;
}

static uint32_t xor_random(uint32_t *seed)
{
    *seed = *seed * 1664525U + 1013904223U;
    return *seed;
}

static void xor_setup(oracle_fixture *f, uint32_t count, uint32_t index,
                      uint32_t seed, uint32_t alignment)
{
    uint32_t i, state;
    oracle_setup(f);
    f->object += alignment;
    f->output = f->object + OUTPUT_OFFSET;
    state = f->state + 0x1000U + alignment;
    f->state += alignment;
    f->cpu.ecx = f->state;
    f->cpu.edi = f->object;
    f->cpu.ebx = f->output;
    f->cpu.esi = count;
    f->cpu.stack_low_water = f->cpu.stack_ceiling;
    memset(&f->cpu.fl, 0x51, sizeof f->cpu.fl);
    store_u32le(f->stack, 0x0059c427U);
    store_u32le(f->stack + 4U, f->output);
    store_u32le(f->stack + 8U, count);
    store_u32le(f->object + 0x14U, f->state);
    store_u32le(f->state, state);
    *(uint8_t *)(uintptr_t)(f->state + 0x10U) = 1U;
    for (i = 4U; i < 0x810U; i += 4U)
        store_u32le(state + i, xor_random(&seed));
    store_u32le(state, index);
    for (i = 0U; i < 0x400U; ++i)
        *(uint8_t *)(uintptr_t)(f->output + i) = (uint8_t)xor_random(&seed);
    s_allocations[0].base = f->object;
    s_allocations[0].size = 0xc1cU;
    s_allocations[1].base = f->state;
    s_allocations[1].size = 0x14U;
    s_allocations[2].base = state;
    s_allocations[2].size = 0x810U;
    s_original_calls = 0U;
    s_original_refresh_calls = 0U;
}

static int xor_differential(uint32_t count, uint32_t index,
                            uint32_t seed, uint32_t alignment)
{
    oracle_fixture f;
    CPU expected;
    static uint8_t original[ARENA_BYTES];
    xor_setup(&f, count, index, seed, alignment);
    sub_005b06f0(&f.cpu);
    CHECK(f.cpu.fault == NULL);
    expected = f.cpu;
    memcpy(original, s_arena, sizeof original);
    xor_setup(&f, count, index, seed, alignment);
    __wrap_sub_005b06f0(&f.cpu);
    if (memcmp(&expected, &f.cpu, sizeof expected) ||
        memcmp(original, s_arena, sizeof original)) {
        fprintf(stderr, "XOR differential count=%u index=%u seed=%u align=%u\n",
                count, index, seed, alignment);
        CHECK(memcmp(&expected, &f.cpu, sizeof expected) == 0);
        CHECK(memcmp(original, s_arena, sizeof original) == 0);
    }
    CHECK(s_original_calls == (count == 0U ? 1U : 0U));
    CHECK(s_release_calls == (count == 0U ? 0U : 3U));
    {
        unsigned refreshes = index + (count + 3U) / 4U >= 256U;
#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
        CHECK(s_original_refresh_calls == (s_table_mutated ? refreshes : 0U));
#else
        CHECK(s_original_refresh_calls == refreshes);
#endif
    }
    return 0;
}

static int xor_reject_unchanged(oracle_fixture *f)
{
    CPU before = f->cpu;
    static uint8_t original[ARENA_BYTES];
    memcpy(original, s_arena, sizeof original);
    return !isaac_vita_archive_xor_try(&f->cpu) &&
           !memcmp(&before, &f->cpu, sizeof before) &&
           !memcmp(original, s_arena, sizeof original);
}

static int xor_rejections(void)
{
    oracle_fixture f;
    unsigned i;
#define XOR_REJECT(change) do { \
    xor_setup(&f, 1024U, 255U, 7U, 0U); \
    change; \
    CHECK(xor_reject_unchanged(&f)); \
} while (0)
    CHECK(!isaac_vita_archive_xor_try(NULL));
    XOR_REJECT(store_u32le(f.stack, 0x12345678U));
    XOR_REJECT(f.cpu.stack_floor = f.stack - 32U);
    XOR_REJECT(f.cpu.stack_ceiling = f.stack + 8U);
    XOR_REJECT(f.cpu.esi = 1023U);
    XOR_REJECT(f.cpu.ebx++);
    XOR_REJECT(f.cpu.edi++);
    XOR_REJECT(store_u32le(f.stack + 8U, 1025U); f.cpu.esi = 1025U);
    XOR_REJECT(store_u32le(f.object + 0x14U, f.state + 4U));
    XOR_REJECT(*(uint8_t *)(uintptr_t)(f.state + 0x10U) = 0U);
    XOR_REJECT(store_u32le(f.state, f.output));
    XOR_REJECT(store_u32le(f.state, f.state));
    XOR_REJECT(store_u32le(f.state, 0xfffffff0U));
    XOR_REJECT(store_u32le(load_u32le(f.state), 256U));
    XOR_REJECT(store_u32le(load_u32le(f.state), UINT32_MAX));
    for (i = 0; i < 3U; ++i) {
        XOR_REJECT(--s_allocations[i].size);
        XOR_REJECT(s_fail_lease_call = i + 1U);
    }
    g_guest_coverage_functions = (unsigned char *)1;
    XOR_REJECT((void)0);
    g_guest_coverage_functions = NULL;
    g_guest_coverage_cases = (unsigned char *)1;
    XOR_REJECT((void)0);
    g_guest_coverage_cases = NULL;
#undef XOR_REJECT
    for (i = 1U; i <= 3U; ++i) {
        xor_setup(&f, 1024U, 255U, 7U, 0U);
        s_fail_release_call = i;
        CHECK(isaac_vita_archive_xor_try(&f.cpu) == 1);
        CHECK(f.cpu.fault != NULL && s_fault_calls == 1U);
        CHECK(f.cpu.esp == f.stack + 12U && s_original_calls == 0U);
    }
    xor_setup(&f, 1024U, 255U, 7U, 0U);
    s_fail_lease_call = 3U;
    s_fail_release_call = 1U;
    CHECK(isaac_vita_archive_xor_try(&f.cpu) == 1);
    CHECK(f.cpu.fault != NULL && s_original_calls == 0U);
    return 0;
}

static int xor_repeated_calls(void)
{
    oracle_fixture f;
    CPU expected;
    static uint8_t original[ARENA_BYTES];
    unsigned pass, i;
    for (pass = 0U; pass < 2U; ++pass) {
        xor_setup(&f, 0U, 255U, UINT32_MAX, 3U);
        for (i = 0U; i < 513U; ++i) {
            uint32_t length = (i * 127U) % 1025U;
            f.cpu.esp = f.stack;
            f.cpu.ecx = f.state;
            f.cpu.esi = length;
            store_u32le(f.stack, 0x0059c427U);
            store_u32le(f.stack + 4U, f.output);
            store_u32le(f.stack + 8U, length);
            if (pass)
                __wrap_sub_005b06f0(&f.cpu);
            else
                sub_005b06f0(&f.cpu);
            CHECK(f.cpu.fault == NULL);
        }
        if (!pass) {
            expected = f.cpu;
            memcpy(original, s_arena, sizeof original);
        } else {
            CHECK(memcmp(&expected, &f.cpu, sizeof expected) == 0);
            CHECK(memcmp(original, s_arena, sizeof original) == 0);
        }
    }
    return 0;
}

static int xor_refresh_table_mutations(void)
{
    unsigned entry;
    for (entry = 0U; entry < 4U; ++entry) {
        uint32_t replacement = load_u32le(ARCHIVE_XOR_TABLE_ADDRESS +
                                         ((entry + 1U) & 3U) * 4U);
        store_u32le(ARCHIVE_XOR_TABLE_ADDRESS + entry * 4U, replacement);
        s_table_mutated = 1U;
#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
        {
            oracle_fixture f;
            CPU before;
            static uint8_t arena_before[ARENA_BYTES];
            xor_setup(&f, 1024U, 255U, entry + 1U, entry);
            f.cpu.ecx = load_u32le(f.state);
            before = f.cpu;
            memcpy(arena_before, s_arena, sizeof arena_before);
            CHECK(isaac_vita_archive_xor_refresh_oracle(&f.cpu) == 0);
            CHECK(memcmp(&before, &f.cpu, sizeof before) == 0);
            CHECK(memcmp(arena_before, s_arena, sizeof arena_before) == 0);
        }
#endif
        /* Alternate VALID destinations change the actual generated algorithm.
         * Candidate must call that original, not execute hardcoded shifts. */
        CHECK(xor_differential(1024U, 255U, entry + 1U, entry) == 0);
        memcpy((void *)(uintptr_t)ARCHIVE_XOR_TABLE_ADDRESS,
               archive_xor_table, sizeof archive_xor_table);
        s_table_mutated = 0U;
        CHECK(xor_differential(1024U, 255U, entry + 1U, entry) == 0);
    }
    return 0;
}

#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
static int xor_refresh_cpu_exit(void)
{
    unsigned alignment, seed;
    for (alignment = 0U; alignment < 4U; ++alignment)
        for (seed = 0U; seed < 16U; ++seed) {
            oracle_fixture f;
            CPU expected;
            static uint8_t original[ARENA_BYTES];
            unsigned pass;
            for (pass = 0U; pass < 2U; ++pass) {
                xor_setup(&f, 1024U, 255U, seed * 0x1020304U, alignment);
                f.cpu.ecx = load_u32le(f.state);
                if (seed == 0U) {
                    /* Exercise wraparound of cc and the initial bb addition,
                     * in addition to the varied full-word random states. */
                    store_u32le(f.cpu.ecx + 0x804U, UINT32_MAX);
                    store_u32le(f.cpu.ecx + 0x808U, UINT32_MAX);
                    store_u32le(f.cpu.ecx + 0x80cU, UINT32_MAX);
                } else if (seed == 1U) {
                    store_u32le(f.cpu.ecx + 0x808U, UINT32_MAX);
                    store_u32le(f.cpu.ecx + 0x80cU, 0U);
                }
                store_u32le(f.stack, 0x005b072fU);
                if (pass)
                    CHECK(isaac_vita_archive_xor_refresh_oracle(&f.cpu) == 1);
                else
                    sub_005c2fd0(&f.cpu);
                CHECK(f.cpu.fault == NULL);
                if (!pass) {
                    expected = f.cpu;
                    memcpy(original, s_arena, sizeof original);
                } else {
                    CHECK(memcmp(&expected, &f.cpu, sizeof expected) == 0);
                    CHECK(memcmp(original, s_arena, sizeof original) == 0);
                }
            }
        }
    return 0;
}
#endif

int main(void)
{
    static const uint32_t lengths[] = {0, 1, 2, 3, 4, 5, 7, 8, 15,
                                       511, 512, 513, 1021, 1022, 1023, 1024};
    uint32_t index, length, alignment, cases = 0;
    CHECK(oracle_map());
    CHECK(xor_map_table());
    for (index = 0; index < 256U; ++index)
        for (length = 0; length < sizeof lengths / sizeof lengths[0]; ++length)
            for (alignment = 0; alignment < 4U; ++alignment) {
                CHECK(xor_differential(lengths[length], index,
                      index * 7U + length * 19U + alignment, alignment) == 0);
                ++cases;
            }
    CHECK(xor_rejections() == 0);
    CHECK(xor_repeated_calls() == 0);
    CHECK(xor_refresh_table_mutations() == 0);
#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
    CHECK(xor_refresh_cpu_exit() == 0);
#endif
    oracle_unmap();
#if defined(_WIN32)
    (void)VirtualFree(s_table_page, 0U, MEM_RELEASE);
#else
    (void)munmap(s_table_page, 0x10000U);
#endif
    printf("archive XOR generated differential: PASS; cases=%u fullCPU+arena, "
           "hostile+release, sequence=513, validTableMutations=4, "
           "refresh=%s\n", cases,
#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
           "native+directCPU64"
#else
           "original"
#endif
           );
    return 0;
}
#endif
