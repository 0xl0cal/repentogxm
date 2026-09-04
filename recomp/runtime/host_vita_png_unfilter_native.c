#include "host_vita_png_unfilter_native.h"

#include <stdint.h>

#if ISAAC_VITA_PNG_NATIVE_UNFILTER && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define ISAAC_VITA_PNG_UNFILTER_HAS_NEON 1
#else
#define ISAAC_VITA_PNG_UNFILTER_HAS_NEON 0
#endif

#if ISAAC_VITA_PNG_NATIVE_UNFILTER && defined(__vita__) && \
    !ISAAC_VITA_PNG_UNFILTER_HAS_NEON
#error "Vita PNG native unfilter requires the target-wide ARM NEON contract"
#endif

enum {
    ISAAC_PNG_FILTER_NONE = 0,
    ISAAC_PNG_FILTER_SUB = 1,
    ISAAC_PNG_FILTER_UP = 2,
    ISAAC_PNG_FILTER_AVG = 3,
    ISAAC_PNG_FILTER_PAETH = 4
};

#if ISAAC_VITA_PNG_NATIVE_UNFILTER
static uint32_t load_u32le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static int equal_length_ranges_overlap(
    const uint8_t *left, const uint8_t *right, size_t length)
{
    uintptr_t left_address;
    uintptr_t right_address;

    if (length == 0 || left == right)
        return left == right && length != 0;

    left_address = (uintptr_t)left;
    right_address = (uintptr_t)right;
    if (left_address < right_address)
        return right_address - left_address < length;
    return left_address - right_address < length;
}

/*
 * Unlike libpng's internal NEON kernels, this loop never rounds rowbytes up:
 * every vector load/store covers sixteen logical bytes and the scalar tail is
 * bounded by rowbytes.  Aliases are rejected before this function is called.
 */
static void filter_up_nonoverlap(
    uint8_t *row, const uint8_t *previous_row, uint32_t rowbytes)
{
    uint32_t index = 0;

#if ISAAC_VITA_PNG_UNFILTER_HAS_NEON
    for (; rowbytes - index >= 16; index += 16) {
        uint8x16_t current = vld1q_u8(row + index);
        uint8x16_t above = vld1q_u8(previous_row + index);
        vst1q_u8(row + index, vaddq_u8(current, above));
    }
#endif
    for (; index < rowbytes; ++index)
        row[index] = (uint8_t)(row[index] + previous_row[index]);
}

static uint8_t paeth_predictor(uint8_t left, uint8_t above, uint8_t upper_left)
{
    int pa = (int)above - (int)upper_left;
    int pb = (int)left - (int)upper_left;
    int pc = pa + pb;

    if (pa < 0)
        pa = -pa;
    if (pb < 0)
        pb = -pb;
    if (pc < 0)
        pc = -pc;
    if (pa <= pb && pa <= pc)
        return left;
    if (pb <= pc)
        return above;
    return upper_left;
}

static void filter_sub(uint8_t *row, uint32_t rowbytes, uint32_t bpp)
{
    uint32_t index;

    for (index = bpp; index < rowbytes; ++index)
        row[index] = (uint8_t)(row[index] + row[index - bpp]);
}

static void filter_average(
    uint8_t *row, const uint8_t *previous_row,
    uint32_t rowbytes, uint32_t bpp)
{
    uint32_t index;

    for (index = 0; index < bpp; ++index)
        row[index] = (uint8_t)(row[index] + (previous_row[index] >> 1));
    for (; index < rowbytes; ++index) {
        unsigned int prediction =
            ((unsigned int)previous_row[index] + row[index - bpp]) >> 1;
        row[index] = (uint8_t)(row[index] + prediction);
    }
}

static void filter_paeth(
    uint8_t *row, const uint8_t *previous_row,
    uint32_t rowbytes, uint32_t bpp)
{
    uint32_t index;

    for (index = 0; index < bpp; ++index)
        row[index] = (uint8_t)(row[index] + previous_row[index]);
    for (; index < rowbytes; ++index) {
        uint8_t prediction = paeth_predictor(
            row[index - bpp], previous_row[index],
            previous_row[index - bpp]);
        row[index] = (uint8_t)(row[index] + prediction);
    }
}
#endif

int isaac_vita_png_unfilter_try(
    const void *row_info_pointer,
    size_t row_info_capacity,
    uint8_t *row,
    size_t row_capacity,
    const uint8_t *previous_row,
    size_t previous_row_capacity,
    uint32_t filter_type)
{
#if !ISAAC_VITA_PNG_NATIVE_UNFILTER
    (void)row_info_pointer;
    (void)row_info_capacity;
    (void)row;
    (void)row_capacity;
    (void)previous_row;
    (void)previous_row_capacity;
    (void)filter_type;
    return ISAAC_VITA_PNG_UNFILTER_FALLBACK;
#else
    const uint8_t *row_info = (const uint8_t *)row_info_pointer;
    uint32_t rowbytes;
    uint32_t pixel_depth;
    uint32_t bpp;

    if (filter_type > ISAAC_PNG_FILTER_PAETH ||
        row_info == NULL ||
        row_info_capacity < ISAAC_VITA_PNG_ROW_INFO_BYTES)
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;

    rowbytes = load_u32le(row_info + 4);
    pixel_depth = row_info[11];
    bpp = (pixel_depth + 7U) >> 3;

    /* PNG permits at most 64 bits/pixel.  Empty/malformed rows fall back. */
    if (rowbytes == 0 || bpp == 0 || bpp > 8 ||
        row == NULL || row_capacity < rowbytes)
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;

    if ((filter_type == ISAAC_PNG_FILTER_SUB ||
         filter_type == ISAAC_PNG_FILTER_AVG ||
         filter_type == ISAAC_PNG_FILTER_PAETH) && rowbytes < bpp)
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;

    if (filter_type >= ISAAC_PNG_FILTER_UP &&
        (previous_row == NULL || previous_row_capacity < rowbytes))
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;
    if (filter_type >= ISAAC_PNG_FILTER_UP &&
        equal_length_ranges_overlap(row, previous_row, rowbytes))
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;

    switch (filter_type) {
    case ISAAC_PNG_FILTER_NONE:
        break;
    case ISAAC_PNG_FILTER_SUB:
        filter_sub(row, rowbytes, bpp);
        break;
    case ISAAC_PNG_FILTER_UP:
        filter_up_nonoverlap(row, previous_row, rowbytes);
        break;
    case ISAAC_PNG_FILTER_AVG:
        filter_average(row, previous_row, rowbytes, bpp);
        break;
    case ISAAC_PNG_FILTER_PAETH:
        filter_paeth(row, previous_row, rowbytes, bpp);
        break;
    default:
        return ISAAC_VITA_PNG_UNFILTER_FALLBACK;
    }
    return ISAAC_VITA_PNG_UNFILTER_HANDLED;
#endif
}
