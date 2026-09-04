#include "host_vita_png_unfilter_native.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    ORACLE_ARENA_BYTES = 1024,
    ORACLE_MAX_ROWBYTES = 271,
    ORACLE_PATTERN_COUNT = 4,
    ORACLE_LAYOUT_COUNT = 5
};

_Static_assert(CHAR_BIT == 8, "PNG byte arithmetic requires 8-bit bytes");
_Static_assert(ISAAC_VITA_PNG_ROW_INFO_BYTES == 12,
               "frozen 32-bit png_row_info layout drifted");

static unsigned long g_cases;

#define CHECK(expression) do {                                                \
    if (!(expression)) {                                                      \
        fprintf(stderr, "PNG unfilter oracle failed at line %d: %s\n",      \
                __LINE__, #expression);                                       \
        return 0;                                                             \
    }                                                                         \
} while (0)

static void store_u32le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

#if ISAAC_VITA_PNG_NATIVE_UNFILTER
static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void fill_arena(uint8_t *arena, unsigned int pattern, uint32_t salt)
{
    size_t index;
    uint32_t random_state = 0x9e3779b9U ^ salt;

    for (index = 0; index < ORACLE_ARENA_BYTES; ++index) {
        switch (pattern) {
        case 0:
            arena[index] = 0;
            break;
        case 1:
            arena[index] = 0xff;
            break;
        case 2:
            arena[index] = (uint8_t)(index * 37U + salt * 13U);
            break;
        default:
            arena[index] = (uint8_t)next_random(&random_state);
            break;
        }
    }
}

static uint8_t reference_paeth(
    uint8_t left, uint8_t above, uint8_t upper_left)
{
    int base = (int)left + (int)above - (int)upper_left;
    int distance_left = base - (int)left;
    int distance_above = base - (int)above;
    int distance_upper_left = base - (int)upper_left;

    if (distance_left < 0)
        distance_left = -distance_left;
    if (distance_above < 0)
        distance_above = -distance_above;
    if (distance_upper_left < 0)
        distance_upper_left = -distance_upper_left;
    if (distance_left <= distance_above &&
        distance_left <= distance_upper_left)
        return left;
    if (distance_above <= distance_upper_left)
        return above;
    return upper_left;
}

/* Deliberately independent, direct transcription of PNG filter equations. */
static void reference_unfilter(
    uint8_t *row, const uint8_t *previous_row,
    uint32_t rowbytes, uint32_t bpp, uint32_t filter_type)
{
    uint32_t index;

    switch (filter_type) {
    case 0:
        return;
    case 1:
        for (index = bpp; index != rowbytes; ++index)
            row[index] = (uint8_t)(row[index] + row[index - bpp]);
        return;
    case 2:
        for (index = 0; index != rowbytes; ++index)
            row[index] = (uint8_t)(row[index] + previous_row[index]);
        return;
    case 3:
        for (index = 0; index != rowbytes; ++index) {
            unsigned int left = index < bpp ? 0U : row[index - bpp];
            unsigned int above = previous_row[index];
            row[index] = (uint8_t)(row[index] + ((left + above) / 2U));
        }
        return;
    case 4:
        for (index = 0; index != rowbytes; ++index) {
            uint8_t left = index < bpp ? 0 : row[index - bpp];
            uint8_t upper_left = index < bpp ? 0 : previous_row[index - bpp];
            row[index] = (uint8_t)(row[index] + reference_paeth(
                left, previous_row[index], upper_left));
        }
        return;
    default:
        return;
    }
}

static void layout_offsets(
    unsigned int layout, uint32_t rowbytes,
    size_t *row_offset, size_t *previous_offset)
{
    switch (layout) {
    case 0: /* separated by a gap */
        *row_offset = 64;
        *previous_offset = 512;
        break;
    case 1: /* exactly adjacent */
        *row_offset = 64;
        *previous_offset = 64 + rowbytes;
        break;
    case 2: /* exact alias */
        *row_offset = 256;
        *previous_offset = 256;
        break;
    case 3: /* row starts one byte after previous */
        *row_offset = 257;
        *previous_offset = 256;
        break;
    default: /* previous starts one byte after row */
        *row_offset = 256;
        *previous_offset = 257;
        break;
    }
}
#endif

#if !ISAAC_VITA_PNG_NATIVE_UNFILTER
static int test_default_off(void)
{
    uint8_t row_info_storage[ISAAC_VITA_PNG_ROW_INFO_BYTES + 1];
    uint8_t row[32];
    uint8_t before[32];
    int result;

    memset(row_info_storage, 0, sizeof row_info_storage);
    store_u32le(row_info_storage + 1 + 4, sizeof row);
    row_info_storage[1 + 11] = 32;
    memset(row, 0xa5, sizeof row);
    memcpy(before, row, sizeof row);
    result = isaac_vita_png_unfilter_try(
        row_info_storage + 1, ISAAC_VITA_PNG_ROW_INFO_BYTES,
        row, sizeof row, row, sizeof row, 4);
    CHECK(result == ISAAC_VITA_PNG_UNFILTER_FALLBACK);
    CHECK(memcmp(row, before, sizeof row) == 0);

    /* Disabled means no pointer is inspected, including deliberately bad ones. */
    result = isaac_vita_png_unfilter_try(
        (const void *)(uintptr_t)1, 0,
        (uint8_t *)(uintptr_t)1, 0,
        (const uint8_t *)(uintptr_t)1, 0, UINT32_MAX);
    CHECK(result == ISAAC_VITA_PNG_UNFILTER_FALLBACK);
    g_cases += 2;
    return 1;
}
#endif

#if ISAAC_VITA_PNG_NATIVE_UNFILTER
static int expect_fallback(
    const void *row_info, size_t row_info_capacity,
    uint8_t *row, size_t row_capacity,
    const uint8_t *previous_row, size_t previous_row_capacity,
    uint32_t filter_type, const uint8_t *arena, const uint8_t *before)
{
    int result = isaac_vita_png_unfilter_try(
        row_info, row_info_capacity, row, row_capacity,
        previous_row, previous_row_capacity, filter_type);

    CHECK(result == ISAAC_VITA_PNG_UNFILTER_FALLBACK);
    CHECK(memcmp(arena, before, ORACLE_ARENA_BYTES) == 0);
    ++g_cases;
    return 1;
}

static int test_valid_and_alias_matrix(void)
{
    uint8_t row_info_storage[ISAAC_VITA_PNG_ROW_INFO_BYTES + 2];
    uint8_t actual[ORACLE_ARENA_BYTES];
    uint8_t expected[ORACLE_ARENA_BYTES];
    uint8_t *row_info = row_info_storage + 1; /* prove unaligned metadata */
    uint32_t filter_type;
    uint32_t bpp;

    for (filter_type = 0; filter_type <= 4; ++filter_type) {
        for (bpp = 1; bpp <= 8; ++bpp) {
            unsigned int depth_variant;
            for (depth_variant = 0; depth_variant < 2; ++depth_variant) {
                uint32_t pixel_depth = depth_variant == 0
                    ? (bpp - 1U) * 8U + 1U : bpp * 8U;
                uint32_t rowbytes;
                for (rowbytes = 1; rowbytes <= ORACLE_MAX_ROWBYTES;
                     ++rowbytes) {
                    unsigned int pattern;
                    for (pattern = 0; pattern < ORACLE_PATTERN_COUNT;
                         ++pattern) {
                        unsigned int layout;
                        for (layout = 0; layout < ORACLE_LAYOUT_COUNT;
                             ++layout) {
                            size_t row_offset;
                            size_t previous_offset;
                            int overlaps;
                            int too_short;
                            int result;

                            layout_offsets(layout, rowbytes,
                                           &row_offset, &previous_offset);
                            CHECK(row_offset + rowbytes <= sizeof actual);
                            CHECK(previous_offset + rowbytes <= sizeof actual);
                            fill_arena(actual, pattern,
                                       filter_type * 0x10000U +
                                       bpp * 0x1000U +
                                       rowbytes * 7U + layout);
                            memcpy(expected, actual, sizeof actual);
                            memset(row_info_storage, 0xcc,
                                   sizeof row_info_storage);
                            store_u32le(row_info + 4, rowbytes);
                            row_info[11] = (uint8_t)pixel_depth;

                            overlaps = layout == 2 ||
                                ((layout == 3 || layout == 4) &&
                                 rowbytes > 1);
                            too_short = (filter_type == 1 ||
                                         filter_type == 3 ||
                                         filter_type == 4) &&
                                        rowbytes < bpp;
                            result = isaac_vita_png_unfilter_try(
                                row_info, ISAAC_VITA_PNG_ROW_INFO_BYTES,
                                actual + row_offset, rowbytes,
                                actual + previous_offset, rowbytes,
                                filter_type);

                            if (too_short ||
                                (filter_type >= 2 && overlaps)) {
                                CHECK(result ==
                                      ISAAC_VITA_PNG_UNFILTER_FALLBACK);
                            } else {
                                CHECK(result ==
                                      ISAAC_VITA_PNG_UNFILTER_HANDLED);
                                reference_unfilter(
                                    expected + row_offset,
                                    expected + previous_offset,
                                    rowbytes, bpp, filter_type);
                            }
                            CHECK(memcmp(actual, expected,
                                         sizeof actual) == 0);
                            ++g_cases;
                        }
                    }
                }
            }
        }
    }
    return 1;
}

static int test_hostile_inputs(void)
{
    uint8_t row_info_storage[ISAAC_VITA_PNG_ROW_INFO_BYTES + 1];
    uint8_t arena[ORACLE_ARENA_BYTES];
    uint8_t before[ORACLE_ARENA_BYTES];
    uint8_t *row_info = row_info_storage + 1;
    uint8_t *row = arena + 64;
    uint8_t *previous = arena + 512;

    memset(row_info_storage, 0, sizeof row_info_storage);
    store_u32le(row_info + 4, 32);
    row_info[11] = 32;
    fill_arena(arena, 3, 0x12345678U);
    memcpy(before, arena, sizeof arena);

    CHECK(expect_fallback(NULL, 12, row, 32, previous, 32, 2,
                          arena, before));
    CHECK(expect_fallback(row_info, 11, row, 32, previous, 32, 2,
                          arena, before));
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 32, 5,
                          arena, before));
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 32, UINT32_MAX,
                          arena, before));
    CHECK(expect_fallback(row_info, 12, NULL, 32, previous, 32, 2,
                          arena, before));
    CHECK(expect_fallback(row_info, 12, row, 31, previous, 32, 2,
                          arena, before));
    CHECK(expect_fallback(row_info, 12, row, 32, NULL, 32, 2,
                          arena, before));
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 31, 2,
                          arena, before));

    store_u32le(row_info + 4, 0);
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 32, 0,
                          arena, before));
    store_u32le(row_info + 4, 32);
    row_info[11] = 0;
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 32, 2,
                          arena, before));
    row_info[11] = 65;
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 32, 2,
                          arena, before));
    row_info[11] = 64;
    store_u32le(row_info + 4, 7);
    CHECK(expect_fallback(row_info, 12, row, 7, previous, 7, 4,
                          arena, before));
    row_info[11] = 32;
    store_u32le(row_info + 4, UINT32_MAX);
    CHECK(expect_fallback(row_info, 12, row, 32, previous, 32, 2,
                          arena, before));
    return 1;
}
#endif

int main(void)
{
#if ISAAC_VITA_PNG_NATIVE_UNFILTER
    if (!test_valid_and_alias_matrix() || !test_hostile_inputs())
        return 1;
    printf("PNG native unfilter oracle: PASS; cases=%lu; mode=enabled\n",
           g_cases);
#else
    if (!test_default_off())
        return 1;
    printf("PNG native unfilter oracle: PASS; cases=%lu; mode=default-off\n",
           g_cases);
#endif
    return 0;
}
