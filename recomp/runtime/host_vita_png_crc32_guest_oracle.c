#if !defined(_WIN32)
#define _GNU_SOURCE 1
#endif

#include "host_vita_png_crc32_guest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        fprintf(stderr, "PNG CRC32 guest oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        oracle_unmap(); \
        return 1; \
    } \
} while (0)

enum {
    ARENA_ADDRESS = 0x22000000U,
    ARENA_BYTES = 0x00020000U,
    DATA_OFFSET = 0x00001000U,
    TABLE_OFFSET = 0x00010000U,
    STACK_OFFSET = 0x00018000U,
    RETURN_READ = 0x005c3cadU,
    RETURN_SKIP = 0x005c3d48U
};

static uint8_t *s_arena;
static uint32_t s_checks;

static void oracle_unmap(void);

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
        (void *)(uintptr_t)ARENA_ADDRESS, ARENA_BYTES,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void *mapped;
#if defined(MAP_FIXED_NOREPLACE)
    flags |= MAP_FIXED_NOREPLACE;
#else
    flags |= MAP_FIXED;
#endif
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

uint32_t gpop_generated(CPU *__restrict c)
{
    uint32_t value = ld32(c->esp);

    c->esp += 4U;
    return value;
}

static void make_table(void)
{
    uint32_t *table = (uint32_t *)(void *)(s_arena + TABLE_OFFSET);
    uint32_t value;

    for (value = 0U; value < 256U; ++value) {
        uint32_t crc = value;
        unsigned bit;
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        table[value] = crc;
    }
}

static uint32_t reference_crc32(
    uint32_t crc, const uint8_t *data, uint32_t size)
{
    const uint32_t *table =
        (const uint32_t *)(const void *)(s_arena + TABLE_OFFSET);

    if (data == NULL)
        return 0U;
    crc = ~crc;
    while (size-- != 0U)
        crc = table[(uint8_t)(crc ^ *data++)] ^ (crc >> 8);
    return ~crc;
}

static void fixture(CPU *c, uint32_t return_rva, uint32_t size)
{
    uint32_t data = ARENA_ADDRESS + DATA_OFFSET;
    uint32_t stack = ARENA_ADDRESS + STACK_OFFSET;
    uint32_t index;

    memset(c, 0xa5, sizeof *c);
    c->eax = 0x10203040U;
    c->ecx = 0x89abcdefU;
    c->edx = data;
    c->ebx = 0x31415926U;
    c->esp = stack;
    c->ebp = 0x27182818U;
    c->esi = 0x11223344U;
    c->edi = 0x55667788U;
    c->f_a = 0x01020304U;
    c->f_b = 0x11121314U;
    c->f_r = 0x21222324U;
    c->f_op = FLAG_ADD;
    c->f_sz = 2U;
    c->stack_owner = c;
    c->stack_floor = stack;
    c->stack_ceiling = stack + 0x1000U;
    c->stack_low_water = c->stack_ceiling;
    store_u32le(stack, return_rva);
    store_u32le(stack + 4U, size);
    for (index = 0U; index < 4096U; ++index)
        s_arena[DATA_OFFSET + index] = (uint8_t)(index * 37U + 11U);
}

static int success_case(uint32_t return_rva, uint32_t size)
{
    CPU c;
    CPU before;
    uint32_t expected;

    fixture(&c, return_rva, size);
    before = c;
    expected = reference_crc32(
        c.ecx, s_arena + DATA_OFFSET, size);
    CHECK(isaac_vita_png_crc32_guest_try(&c) == 1);
    CHECK(c.eax == expected && c.ecx == expected);
    CHECK(c.edx == before.edx + size && c.esp == before.esp + 4U);
    CHECK(c.ebx == before.ebx && c.ebp == before.ebp &&
          c.esi == before.esi && c.edi == before.edi);
    if (size == 0U) {
        CHECK(c.f_a == before.f_a && c.f_b == before.f_b &&
              c.f_r == before.f_r && c.f_op == before.f_op &&
              c.f_sz == before.f_sz);
    } else {
        CHECK(c.f_a == 1U && c.f_b == 1U && c.f_r == 0U &&
              c.f_op == FLAG_SUB && c.f_sz == 4U);
    }
    return 0;
}

static int rejection_cases(void)
{
    CPU c;
    CPU before;

    fixture(&c, 0x12345678U, 64U);
    before = c;
    CHECK(isaac_vita_png_crc32_guest_try(&c) == 0);
    CHECK(memcmp(&c, &before, sizeof c) == 0);

    fixture(&c, RETURN_READ, 4U);
    c.edx = 0xfffffffeU;
    before = c;
    CHECK(isaac_vita_png_crc32_guest_try(&c) == 0);
    CHECK(memcmp(&c, &before, sizeof c) == 0);

    fixture(&c, RETURN_READ, 4U);
    c.stack_ceiling = c.esp + 4U;
    before = c;
    CHECK(isaac_vita_png_crc32_guest_try(&c) == 0);
    CHECK(memcmp(&c, &before, sizeof c) == 0);
    CHECK(isaac_vita_png_crc32_guest_try(NULL) == 0);
    return 0;
}

static int null_case(void)
{
    CPU c;
    CPU before;

    fixture(&c, RETURN_SKIP, 777U);
    c.edx = 0U;
    before = c;
    CHECK(isaac_vita_png_crc32_guest_try(&c) == 1);
    CHECK(c.eax == 0U && c.ecx == before.ecx && c.edx == 0U);
    CHECK(c.esp == before.esp + 4U);
    CHECK(c.f_a == before.f_a && c.f_b == before.f_b &&
          c.f_r == before.f_r && c.f_op == before.f_op &&
          c.f_sz == before.f_sz);
    return 0;
}

int main(void)
{
    static const uint32_t sizes[] = { 0U, 1U, 7U, 8U, 9U, 255U, 4096U };
    unsigned index;

    CHECK(oracle_map());
    make_table();
    for (index = 0U; index < sizeof sizes / sizeof sizes[0]; ++index) {
        CHECK(success_case(RETURN_READ, sizes[index]) == 0);
        CHECK(success_case(RETURN_SKIP, sizes[index]) == 0);
    }
    CHECK(null_case() == 0);
    CHECK(rejection_cases() == 0);
    oracle_unmap();
    printf("PNG CRC32 guest oracle: PASS; checks=%u; callers=2\n", s_checks);
    return 0;
}
