#include "host_vita_zlib_inflate_flush_native.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "zlib inflate_flush native oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

enum {
    WINDOW_BYTES = 0x8000U,
    OUTPUT_BYTES = 0x2000U,
    RANDOM_CASES = 8192U,
    ADLER_CASES = 1024U
};

static uint32_t s_random = 0x8a31d5e7U;

static uint32_t random_u32(void)
{
    uint32_t value = s_random;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_random = value;
    return value;
}

static uint32_t reference_adler32(
    uint32_t adler, const uint8_t *buffer, uint32_t length)
{
    uint32_t s1 = adler & 0xffffU;
    uint32_t s2 = (adler >> 16) & 0xffffU;
    uint32_t index;

    if (buffer == NULL)
        return 1U;
    for (index = 0U; index < length; ++index) {
        s1 = (s1 + buffer[index]) % 65521U;
        s2 = (s2 + s1) % 65521U;
    }
    return s1 | (s2 << 16);
}

static void reference_segment(
    isaac_vita_zlib_inflate_flush_state *state,
    uint32_t source_offset, uint32_t count)
{
    if (count != 0U && state->result == -5)
        state->result = 0;
    state->avail_out -= count;
    state->total_out += count;
    if (state->check_enabled != 0U) {
        state->check = reference_adler32(
            state->check, state->window + source_offset, count);
    }
    memmove(state->output + state->next_out_offset,
            state->window + source_offset, count);
    state->next_out_offset += count;
    state->read_offset += count;
}

static uint32_t reference_flush(
    isaac_vita_zlib_inflate_flush_state *state)
{
    uint32_t extent = state->read_offset <= state->write_offset
        ? state->write_offset : (uint32_t)state->window_capacity;
    uint32_t count = extent - state->read_offset;
    uint32_t segments = 1U;

    if (count > state->avail_out)
        count = state->avail_out;
    reference_segment(state, state->read_offset, count);
    if (state->read_offset == state->window_capacity) {
        state->read_offset = 0U;
        if (state->write_offset == state->window_capacity)
            state->write_offset = 0U;
        count = state->write_offset;
        if (count > state->avail_out)
            count = state->avail_out;
        reference_segment(state, 0U, count);
        segments = 2U;
    }
    return segments;
}

static int run_adler_corpus(void)
{
    static const uint32_t boundaries[] = {
        0U, 1U, 15U, 16U, 17U, 5551U, 5552U, 5553U,
        11104U, 16384U, 19999U, 20000U
    };
    static uint8_t bytes[20000];
    uint32_t index;
    uint32_t case_index;

    for (index = 0U; index < sizeof bytes; ++index)
        bytes[index] = (uint8_t)random_u32();
    CHECK(isaac_vita_zlib_114_adler32(0U, NULL, 77U) == 1U);
    for (case_index = 0U; case_index < ADLER_CASES; ++case_index) {
        uint32_t length = random_u32() % (uint32_t)(sizeof bytes + 1U);
        uint32_t seed = random_u32();
        CHECK(isaac_vita_zlib_114_adler32(seed, bytes, length) ==
              reference_adler32(seed, bytes, length));
    }
    for (index = 0U;
            index < sizeof boundaries / sizeof boundaries[0]; ++index) {
        uint32_t length = boundaries[index];
        CHECK(isaac_vita_zlib_114_adler32(1U, bytes, length) ==
              reference_adler32(1U, bytes, length));
    }
    return 0;
}

static int run_flush_corpus(void)
{
    static uint8_t window_a[WINDOW_BYTES];
    static uint8_t window_b[WINDOW_BYTES];
    static uint8_t output_a[OUTPUT_BYTES];
    static uint8_t output_b[OUTPUT_BYTES];
    uint32_t case_index;

    for (case_index = 0U; case_index < RANDOM_CASES; ++case_index) {
        isaac_vita_zlib_inflate_flush_state actual;
        isaac_vita_zlib_inflate_flush_state expected;
        uint32_t next_out = random_u32() % (OUTPUT_BYTES + 1U);
        uint32_t segments = 0U;
        uint32_t expected_segments;
        uint32_t index;

        for (index = 0U; index < WINDOW_BYTES; ++index)
            window_a[index] = (uint8_t)random_u32();
        for (index = 0U; index < OUTPUT_BYTES; ++index)
            output_a[index] = (uint8_t)random_u32();
        memcpy(window_b, window_a, sizeof window_a);
        memcpy(output_b, output_a, sizeof output_a);

        actual.window = window_a;
        actual.window_capacity = WINDOW_BYTES;
        actual.read_offset = random_u32() % (WINDOW_BYTES + 1U);
        actual.write_offset = random_u32() % (WINDOW_BYTES + 1U);
        actual.output = output_a;
        actual.output_capacity = OUTPUT_BYTES;
        actual.next_out_offset = next_out;
        actual.avail_out = random_u32() % (OUTPUT_BYTES - next_out + 1U);
        actual.total_out = random_u32();
        actual.check = random_u32();
        actual.result = (int32_t[]){ -5, 0, 1, -3 }[random_u32() & 3U];
        actual.check_enabled = random_u32() & 1U;
        expected = actual;
        expected.window = window_b;
        expected.output = output_b;

        expected_segments = reference_flush(&expected);
        CHECK(isaac_vita_zlib_inflate_flush_native_try(
                  &actual, &segments) ==
              ISAAC_VITA_ZLIB_INFLATE_FLUSH_HANDLED);
        CHECK(segments == expected_segments);
        CHECK(actual.read_offset == expected.read_offset);
        CHECK(actual.write_offset == expected.write_offset);
        CHECK(actual.next_out_offset == expected.next_out_offset);
        CHECK(actual.avail_out == expected.avail_out);
        CHECK(actual.total_out == expected.total_out);
        CHECK(actual.check == expected.check);
        CHECK(actual.result == expected.result);
        CHECK(memcmp(window_a, window_b, sizeof window_a) == 0);
        CHECK(memcmp(output_a, output_b, sizeof output_a) == 0);
    }
    return 0;
}

static int run_hostile_cases(void)
{
    uint8_t arena[96] = { 0 };
    isaac_vita_zlib_inflate_flush_state state = { 0 };
    isaac_vita_zlib_inflate_flush_state snapshot;
    uint32_t segments = 99U;

    CHECK(isaac_vita_zlib_inflate_flush_native_try(NULL, &segments) == 0);
    CHECK(segments == 0U);
    state.window = arena;
    state.window_capacity = 64U;
    state.output = arena + 64U;
    state.output_capacity = 32U;
    state.avail_out = 32U;
    snapshot = state;
    CHECK(isaac_vita_zlib_inflate_flush_native_try(&state, NULL) == 0);
    CHECK(memcmp(&state, &snapshot, sizeof state) == 0);

    state.read_offset = 65U;
    snapshot = state;
    CHECK(isaac_vita_zlib_inflate_flush_native_try(&state, &segments) == 0);
    CHECK(segments == 0U && memcmp(&state, &snapshot, sizeof state) == 0);
    state = snapshot;
    state.read_offset = 0U;
    state.write_offset = 64U;
    state.next_out_offset = 31U;
    state.avail_out = 2U;
    snapshot = state;
    CHECK(isaac_vita_zlib_inflate_flush_native_try(&state, &segments) == 0);
    CHECK(segments == 0U && memcmp(&state, &snapshot, sizeof state) == 0);
    state = snapshot;
    state.next_out_offset = 0U;
    state.avail_out = 32U;
    state.output = arena + 32U;
    snapshot = state;
    CHECK(isaac_vita_zlib_inflate_flush_native_try(&state, &segments) == 0);
    CHECK(segments == 0U && memcmp(&state, &snapshot, sizeof state) == 0);
    return 0;
}

int main(void)
{
    CHECK(run_adler_corpus() == 0);
    CHECK(run_flush_corpus() == 0);
    CHECK(run_hostile_cases() == 0);
    printf("zlib 1.1.4 inflate_flush native oracle: PASS; "
           "flush=%u adler=%u\n", RANDOM_CASES, ADLER_CASES);
    return 0;
}
