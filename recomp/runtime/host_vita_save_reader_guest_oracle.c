#if !defined(_WIN32) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE 1
#endif

#include "host_vita_save_reader_guest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif

#define CHECK(condition) do {                                            \
    ++s_checks;                                                          \
    if (!(condition)) {                                                  \
        fprintf(stderr, "Save Reader oracle failed at %s:%d: %s\n",    \
                __FILE__, __LINE__, #condition);                         \
        return 1;                                                        \
    }                                                                    \
} while (0)

enum {
    ARENA_ADDRESS = 0x21000000U,
    ARENA_BYTES = 0x00010000U,
    SOURCE_OFFSET = 0x1000U,
    DEST_OFFSET = 0x2000U,
    READER_OFFSET = 0x3000U,
    STACK_FLOOR_OFFSET = 0x4000U,
    STACK_ENTRY_OFFSET = 0x5000U,
    STACK_CEILING_OFFSET = 0x6000U,
    READER_VTABLE_RVA = 0x00746d3cU,
    MEMCPY_FUNCTION_ID = 12570U,
    MEMCPY_IMPORT_ID = 281U
};

static uint8_t *s_arena;
static uint8_t s_function_coverage[13000];
static uint8_t s_import_coverage[413];
static uint8_t s_case_coverage[1];
static unsigned s_checks;
static unsigned s_faults;
static unsigned s_range_queries;
static unsigned s_audio_polls;

unsigned char *g_guest_coverage_functions = s_function_coverage;
unsigned char *g_guest_coverage_imports = s_import_coverage;
unsigned char *g_guest_coverage_cases = s_case_coverage;
unsigned g_host_import_calls;
GuestPhaseProfileCounters g_guest_phase_profile_counters;

int isaac_vita_memory_range_oracle_query(
    uint32_t address, uint32_t count, uint32_t *mapped_base,
    uint32_t *mapped_size, uint32_t *access)
{
    (void)address;
    (void)count;
    ++s_range_queries;
    *mapped_base = 0U;
    *mapped_size = 0U;
    *access = 0U;
    return -1;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

void isaac_vita_audio_cooperative_save_poll(CPU *__restrict c)
{
    (void)c;
    ++s_audio_polls;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_faults;
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
# if defined(MAP_32BIT)
    flags |= MAP_32BIT;
# endif
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

static void store_u32(uint32_t address, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)(uintptr_t)address;

    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t load_u32(uint32_t address)
{
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;

    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

typedef struct flag_snapshot {
    uint32_t a, b, r;
    uint8_t op, size, cf, of, zf, sf, pf, df;
} flag_snapshot;

static flag_snapshot snapshot_flags(const CPU *c)
{
    flag_snapshot value = {
        c->f_a, c->f_b, c->f_r, c->f_op, c->f_sz,
        c->f_cf, c->f_of, c->f_zf, c->f_sf, c->f_pf, c->df
    };
    return value;
}

static void fixture(CPU *c, uint32_t destination, uint32_t buffer,
                    uint32_t cursor, uint32_t end,
                    uint32_t element_size, uint32_t element_count)
{
    uint32_t reader = ARENA_ADDRESS + READER_OFFSET;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;
    unsigned index;

    memset(s_arena, 0xa5, ARENA_BYTES);
    memset(c, 0, sizeof *c);
    c->eax = 0x11111111U;
    c->ecx = reader;
    c->edx = 0x33333333U;
    c->ebx = 0x44444444U;
    c->esp = entry;
    c->ebp = 0x66666666U;
    c->esi = 0x77777777U;
    c->edi = 0x88888888U;
    c->f_a = 0x89abcdefU;
    c->f_b = 0x10203040U;
    c->f_r = 0x55667788U;
    c->f_op = FLAG_PARTIAL;
    c->f_sz = 4U;
    c->f_cf = 1U;
    c->f_of = 1U;
    c->f_zf = 0U;
    c->f_sf = 1U;
    c->f_pf = 0U;
    c->df = 1U;
    c->stack_owner = c;
    c->stack_floor = ARENA_ADDRESS + STACK_FLOOR_OFFSET;
    c->stack_ceiling = ARENA_ADDRESS + STACK_CEILING_OFFSET;
    c->stack_low_water = entry;

    store_u32(reader, GUEST_IMAGE_BASE + READER_VTABLE_RVA);
    store_u32(reader + 0x10U, buffer);
    store_u32(reader + 0x18U, cursor);
    store_u32(reader + 0x1cU, end);
    *(uint8_t *)(uintptr_t)(reader + 0x21U) = 0x5aU;
    store_u32(entry, 0xabcdef01U);
    store_u32(entry + 4U, destination);
    store_u32(entry + 8U, element_size);
    store_u32(entry + 12U, element_count);
    for (index = 0U; index < 512U; ++index)
        *(uint8_t *)(uintptr_t)(ARENA_ADDRESS + SOURCE_OFFSET + index) =
            (uint8_t)(index * 37U + 11U);

    memset(s_function_coverage, 0, sizeof s_function_coverage);
    memset(s_import_coverage, 0, sizeof s_import_coverage);
    memset(&g_guest_phase_profile_counters, 0,
           sizeof g_guest_phase_profile_counters);
    g_host_import_calls = 0U;
    s_faults = 0U;
    s_range_queries = 0U;
    s_audio_polls = 0U;
}

static int run_handled(uint32_t destination, uint32_t buffer,
                       uint32_t cursor, uint32_t end,
                       uint32_t element_size, uint32_t element_count)
{
    CPU c;
    flag_snapshot flags;
    flag_snapshot flags_after;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;
    uint32_t reader = ARENA_ADDRESS + READER_OFFSET;
    uint32_t requested = element_size * element_count;
    uint32_t copied = requested < end - cursor ? requested : end - cursor;
    uint32_t source = buffer + cursor;
    uint32_t new_cursor = cursor + copied;
    uint32_t old_ebp;
    uint32_t old_esi;
    uint32_t old_edi;
    unsigned index;

    fixture(&c, destination, buffer, cursor, end,
            element_size, element_count);
    flags = snapshot_flags(&c);
    old_ebp = c.ebp;
    old_esi = c.esi;
    old_edi = c.edi;
    CHECK(isaac_vita_save_reader_guest_try(&c) == 1);
    CHECK(c.fault == NULL && s_faults == 0U);
    CHECK(c.eax == copied);
    CHECK(c.ecx == ((new_cursor & UINT32_C(0xffffff00)) |
                    (new_cursor >= end ? 1U : 0U)));
    CHECK(c.edx == 0x33333333U && c.ebx == 0x44444444U);
    CHECK(c.ebp == old_ebp && c.esi == old_esi && c.edi == old_edi);
    CHECK(c.esp == entry + 16U);
    flags_after = snapshot_flags(&c);
    CHECK(memcmp(&flags, &flags_after, sizeof flags) == 0);
    CHECK(load_u32(reader + 0x18U) == new_cursor);
    CHECK(*(uint8_t *)(uintptr_t)(reader + 0x21U) ==
          (uint8_t)(new_cursor >= end));
    CHECK(c.stack_low_water == entry - 28U);
    CHECK(load_u32(entry - 4U) == old_ebp);
    CHECK(load_u32(entry - 8U) == old_esi);
    CHECK(load_u32(entry - 12U) == old_edi);
    CHECK(load_u32(entry - 16U) == copied);
    CHECK(load_u32(entry - 20U) == source);
    CHECK(load_u32(entry - 24U) == destination);
    CHECK(load_u32(entry - 28U) == 0x0025ba89U);
    CHECK(g_host_import_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(s_function_coverage[MEMCPY_FUNCTION_ID] == 1U);
    CHECK(s_import_coverage[MEMCPY_IMPORT_ID] == 1U);
    CHECK(s_audio_polls == 1U);
    for (index = 0U; index < copied; ++index)
        CHECK(*(uint8_t *)(uintptr_t)(destination + index) ==
              *(uint8_t *)(uintptr_t)(source + index));
    return 0;
}

static int expect_rejected(const CPU *configured)
{
    CPU c = *configured;
    CPU before_cpu = c;
    uint8_t before_arena[ARENA_BYTES];

    memcpy(before_arena, s_arena, sizeof before_arena);
    CHECK(isaac_vita_save_reader_guest_try(&c) == 0);
    CHECK(memcmp(&c, &before_cpu, sizeof c) == 0);
    CHECK(memcmp(s_arena, before_arena, sizeof before_arena) == 0);
    CHECK(g_host_import_calls == 0U && s_faults == 0U);
    CHECK(s_audio_polls == 0U);
    return 0;
}

static int run_rejections(void)
{
    CPU c;
    uint32_t source = ARENA_ADDRESS + SOURCE_OFFSET;
    uint32_t dest = ARENA_ADDRESS + DEST_OFFSET;
    uint32_t reader = ARENA_ADDRESS + READER_OFFSET;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;

    fixture(&c, dest, source, 9U, 8U, 1U, 1U);
    CHECK(expect_rejected(&c) == 0);                    /* cursor > end */
    fixture(&c, dest, UINT32_MAX - 1U, 4U, 8U, 1U, 1U);
    CHECK(expect_rejected(&c) == 0);                    /* base + cursor */
    fixture(&c, UINT32_MAX - 1U, source, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* dst range wrap */
    fixture(&c, dest, UINT32_MAX - 1U, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* src range wrap */
    fixture(&c, source + 1U, source, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* dst/src alias */
    fixture(&c, reader + 4U, source, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* dst/object */
    fixture(&c, dest, reader, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* src/object */
    fixture(&c, entry - 4U, source, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* dst/stack */
    fixture(&c, dest, entry - 4U, 0U, 8U, 1U, 4U);
    CHECK(expect_rejected(&c) == 0);                    /* src/stack */
    fixture(&c, dest, source, 0U, 8U, 1U, 1U);
    c.ecx = entry;
    CHECK(expect_rejected(&c) == 0);                    /* object/stack */
    fixture(&c, dest, source, 0U, 8U, 1U, 1U);
    store_u32(reader, 0x12345678U);
    CHECK(expect_rejected(&c) == 0);                    /* foreign vtable */
    fixture(&c, dest, source, 0U, 8U, 1U, 1U);
    c.stack_owner = NULL;
    CHECK(expect_rejected(&c) == 0);                    /* unbound stack */
    return 0;
}

static int run_import_fault(void)
{
    CPU c;
    flag_snapshot flags;
    flag_snapshot flags_after;
    uint32_t reader = ARENA_ADDRESS + READER_OFFSET;
    uint32_t entry = ARENA_ADDRESS + STACK_ENTRY_OFFSET;
    uint32_t destination = 0x10000000U;
    uint32_t source = 0x30000000U;
    uint32_t copied = 0x01000001U;

    fixture(&c, destination, source, 0U, copied, copied, 1U);
    flags = snapshot_flags(&c);
    CHECK(isaac_vita_save_reader_guest_try(&c) == 1);
    flags_after = snapshot_flags(&c);
    CHECK(s_faults == 1U && c.fault != NULL && c.fault_addr == destination);
    CHECK(s_range_queries == 1U);
    CHECK(c.eax == source && c.ecx == reader);
    CHECK(c.edx == 0x33333333U && c.ebx == 0x44444444U);
    CHECK(c.esp == entry - 28U && c.ebp == entry - 4U);
    CHECK(c.esi == copied && c.edi == reader);
    CHECK(memcmp(&flags, &flags_after, sizeof flags) == 0);
    CHECK(load_u32(reader + 0x18U) == 0U);
    CHECK(*(uint8_t *)(uintptr_t)(reader + 0x21U) == 0x5aU);
    CHECK(load_u32(entry - 4U) == 0x66666666U);
    CHECK(load_u32(entry - 8U) == 0x77777777U);
    CHECK(load_u32(entry - 12U) == 0x88888888U);
    CHECK(load_u32(entry - 16U) == copied);
    CHECK(load_u32(entry - 20U) == source);
    CHECK(load_u32(entry - 24U) == destination);
    CHECK(load_u32(entry - 28U) == 0x0025ba89U);
    CHECK(g_host_import_calls == 1U);
    CHECK(g_guest_phase_profile_counters.guest_calls == 1U);
    CHECK(s_function_coverage[MEMCPY_FUNCTION_ID] == 1U);
    CHECK(s_import_coverage[MEMCPY_IMPORT_ID] == 1U);
    CHECK(s_audio_polls == 0U);
    return 0;
}

int main(void)
{
    int result = 0;

    if (!oracle_map()) {
        fputs("Save Reader oracle could not map a 32-bit arena\n", stderr);
        return 1;
    }
    result |= run_handled(
        ARENA_ADDRESS + DEST_OFFSET, ARENA_ADDRESS + SOURCE_OFFSET,
        3U, 20U, 1U, 4U);
    result |= run_handled(
        ARENA_ADDRESS + DEST_OFFSET, ARENA_ADDRESS + SOURCE_OFFSET,
        8U, 20U, 4U, 4U);                              /* truncated EOF */
    result |= run_handled(
        0U, 0U, 0U, 0U, 0x80000000U, 2U);             /* zero/null */
    result |= run_handled(
        ARENA_ADDRESS + DEST_OFFSET, ARENA_ADDRESS + SOURCE_OFFSET,
        0U, 8U, UINT32_MAX, UINT32_MAX);               /* IMUL low32=1 */
    result |= run_import_fault();                       /* import boundary */
    result |= run_rejections();
    oracle_unmap();
    if (result != 0)
        return 1;
    printf("Vita Save Reader guest oracle: PASS; checks=%u; "
           "handled=4; import-fault=1; rejected=12\n", s_checks);
    return 0;
}
