#include "host_vita_zlib_inflate_flush_native.h"

#include <stdint.h>
#include <string.h>

enum {
    ZLIB_ADLER_BASE = 65521U,
    ZLIB_ADLER_NMAX = 5552U
};

#define ADLER_DO1(buffer, index) \
    do { \
        s1 += (buffer)[index]; \
        s2 += s1; \
    } while (0)
#define ADLER_DO2(buffer, index) \
    do { \
        ADLER_DO1(buffer, index); \
        ADLER_DO1(buffer, (index) + 1U); \
    } while (0)
#define ADLER_DO4(buffer, index) \
    do { \
        ADLER_DO2(buffer, index); \
        ADLER_DO2(buffer, (index) + 2U); \
    } while (0)
#define ADLER_DO8(buffer, index) \
    do { \
        ADLER_DO4(buffer, index); \
        ADLER_DO4(buffer, (index) + 4U); \
    } while (0)
#define ADLER_DO16(buffer) \
    do { \
        ADLER_DO8(buffer, 0U); \
        ADLER_DO8(buffer, 8U); \
    } while (0)

uint32_t isaac_vita_zlib_114_adler32(
    uint32_t adler, const uint8_t *buffer, uint32_t length)
{
    uint32_t s1 = adler & 0xffffU;
    uint32_t s2 = (adler >> 16) & 0xffffU;

    if (buffer == NULL)
        return 1U;
    while (length != 0U) {
        uint32_t block = length < ZLIB_ADLER_NMAX
            ? length : ZLIB_ADLER_NMAX;
        length -= block;
        while (block >= 16U) {
            ADLER_DO16(buffer);
            buffer += 16U;
            block -= 16U;
        }
        while (block != 0U) {
            ADLER_DO1(buffer, 0U);
            ++buffer;
            --block;
        }
        s1 %= ZLIB_ADLER_BASE;
        s2 %= ZLIB_ADLER_BASE;
    }
    return s1 | (s2 << 16);
}

#undef ADLER_DO16
#undef ADLER_DO8
#undef ADLER_DO4
#undef ADLER_DO2
#undef ADLER_DO1

static int ranges_overlap(
    const void *left_pointer, size_t left_size,
    const void *right_pointer, size_t right_size)
{
    uintptr_t left = (uintptr_t)left_pointer;
    uintptr_t right = (uintptr_t)right_pointer;

    if (left_size == 0U || right_size == 0U)
        return 0;
    if (left > UINTPTR_MAX - (left_size - 1U) ||
            right > UINTPTR_MAX - (right_size - 1U))
        return 1;
    if (left < right)
        return right - left < left_size;
    return left - right < right_size;
}

static void flush_segment(
    isaac_vita_zlib_inflate_flush_state *state,
    uint32_t source_offset, uint32_t count)
{
    if (count != 0U && state->result == -5)
        state->result = 0;
    state->avail_out -= count;
    state->total_out += count;
    if (state->check_enabled != 0U) {
        state->check = isaac_vita_zlib_114_adler32(
            state->check, state->window + source_offset, count);
    }
    memcpy(state->output + state->next_out_offset,
           state->window + source_offset, count);
    state->next_out_offset += count;
    state->read_offset += count;
}

int isaac_vita_zlib_inflate_flush_native_try(
    isaac_vita_zlib_inflate_flush_state *state,
    uint32_t *segment_count_out)
{
    uint32_t count;
    uint32_t extent;
    uint32_t segments = 1U;

    if (segment_count_out != NULL)
        *segment_count_out = 0U;
#if SIZE_MAX > UINT32_MAX
    if (state != NULL &&
            (state->window_capacity > UINT32_MAX ||
             state->output_capacity > UINT32_MAX)) {
        return ISAAC_VITA_ZLIB_INFLATE_FLUSH_FALLBACK;
    }
#endif
    if (state == NULL || segment_count_out == NULL ||
            state->window == NULL || state->output == NULL ||
            state->window_capacity == 0U ||
            state->output_capacity == 0U ||
            state->read_offset > state->window_capacity ||
            state->write_offset > state->window_capacity ||
            state->next_out_offset > state->output_capacity ||
            state->avail_out >
                state->output_capacity - state->next_out_offset ||
            state->check_enabled > 1U ||
            ranges_overlap(state->window, state->window_capacity,
                           state->output, state->output_capacity)) {
        return ISAAC_VITA_ZLIB_INFLATE_FLUSH_FALLBACK;
    }

    extent = state->read_offset <= state->write_offset
        ? state->write_offset : (uint32_t)state->window_capacity;
    count = extent - state->read_offset;
    if (count > state->avail_out)
        count = state->avail_out;
    flush_segment(state, state->read_offset, count);

    if (state->read_offset == state->window_capacity) {
        state->read_offset = 0U;
        if (state->write_offset == state->window_capacity)
            state->write_offset = 0U;
        count = state->write_offset;
        if (count > state->avail_out)
            count = state->avail_out;
        flush_segment(state, 0U, count);
        segments = 2U;
    }
    *segment_count_out = segments;
    return ISAAC_VITA_ZLIB_INFLATE_FLUSH_HANDLED;
}
