#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "host_vita_save_checksum_native.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STATE_BYTES = ISAAC_VITA_SAVE_CHECKSUM_STATE_BYTES,
    TABLE_BYTES = ISAAC_VITA_SAVE_CHECKSUM_TABLE_BYTES,
    SYNTHETIC_CASES = 20000U,
    SAVE_HEADER_BYTES = 16U,
    SAVE_FOOTER_BYTES = 4U
};

static const uint8_t s_save_header[SAVE_HEADER_BYTES] = {
    'I', 'S', 'A', 'A', 'C', 'N', 'G', '_',
    'G', 'S', 'R', '0', '1', '4', '6', 0
};

static uint32_t s_random = UINT32_C(0x6d2b79f5);
static unsigned s_checks;

#define CHECK(condition) do {                                            \
    ++s_checks;                                                          \
    if (!(condition)) {                                                  \
        fprintf(stderr, "checksum oracle failed at %s:%d: %s\n",       \
                __FILE__, __LINE__, #condition);                         \
        exit(1);                                                         \
    }                                                                    \
} while (0)

static uint32_t next_random(void)
{
    uint32_t value = s_random;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_random = value;
    return value;
}

static uint32_t load_u32le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void store_u32le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t reference_sar1(uint32_t value)
{
    return (value >> 1) | (value & UINT32_C(0x80000000));
}

static void reference_prepare_table(uint8_t *table)
{
    uint32_t index;

    if (load_u32le(table + 4U) != 0U)
        return;
    for (index = 0U; index < 256U; ++index) {
        uint32_t value = index;
        unsigned bit;

        value = (value >> 1) ^
                ((value & 1U) ? UINT32_C(0xedb88320) : 0U);
        for (bit = 1U; bit < 8U; ++bit)
            value = reference_sar1(value) ^
                    ((value & 1U) ? UINT32_C(0xedb88320) : 0U);
        store_u32le(table + index * 4U, value);
    }
}

static void reference_feed(
    uint8_t *state, const uint8_t *data, size_t size, uint8_t *table)
{
    const uint32_t mode = load_u32le(state + 12U);
    size_t index;

    if (mode == 0U) {
        for (index = 0U; index < size; ++index) {
            uint8_t fill = state[4];

            state[fill] = data[index];
            state[4] = (uint8_t)(fill + 1U);
            if (state[4] >= 4U) {
                uint32_t value = load_u32le(state + 8U);

                value = (value >> 1) + (value << 31);
                value += load_u32le(state);
                state[4] = 0U;
                store_u32le(state + 8U, value);
            }
        }
    } else {
        uint32_t value;

        reference_prepare_table(table);
        value = ~load_u32le(state + 8U);
        store_u32le(state + 8U, value);
        for (index = 0U; index < size; ++index) {
            const uint32_t slot = (value ^ data[index]) & 0xffU;

            value >>= 8;
            value ^= load_u32le(table + slot * 4U);
            store_u32le(state + 8U, value);
        }
        store_u32le(state + 8U, ~value);
    }
}

static void compare_one_feed(
    uint8_t *reference_state, uint8_t *native_state,
    uint8_t *reference_table, uint8_t *native_table,
    const uint8_t *data, size_t size)
{
    reference_feed(reference_state, data, size, reference_table);
    isaac_vita_save_checksum_native_feed(
        native_state, data, size, native_table);
    CHECK(memcmp(reference_state, native_state, STATE_BYTES) == 0);
    CHECK(memcmp(reference_table, native_table, TABLE_BYTES) == 0);
}

static void run_synthetic(void)
{
    uint8_t data[513];
    unsigned test;

    for (test = 0U; test < SYNTHETIC_CASES; ++test) {
        uint8_t reference_state[STATE_BYTES];
        uint8_t native_state[STATE_BYTES];
        uint8_t reference_table[TABLE_BYTES];
        uint8_t native_table[TABLE_BYTES];
        size_t size = next_random() % sizeof data;
        size_t index;
        uint32_t mode = next_random() & 1U;

        for (index = 0U; index < sizeof data; ++index)
            data[index] = (uint8_t)next_random();
        for (index = 0U; index < STATE_BYTES; ++index)
            reference_state[index] = (uint8_t)next_random();
        store_u32le(reference_state + 12U, mode);
        if (mode == 0U)
            reference_state[4] = (uint8_t)(next_random() & 3U);
        memcpy(native_state, reference_state, sizeof native_state);

        memset(reference_table, 0, sizeof reference_table);
        if ((test % 7U) == 0U) {
            for (index = 0U; index < TABLE_BYTES; ++index)
                reference_table[index] = (uint8_t)next_random();
            if (load_u32le(reference_table + 4U) == 0U)
                reference_table[4] = 1U;
        }
        memcpy(native_table, reference_table, sizeof native_table);
        compare_one_feed(reference_state, native_state,
                         reference_table, native_table, data, size);
    }

    {
        uint8_t table[TABLE_BYTES] = {0};
        reference_prepare_table(table);
        CHECK(load_u32le(table + 0U) == UINT32_C(0x00000000));
        CHECK(load_u32le(table + 4U) == UINT32_C(0x09073096));
        CHECK(load_u32le(table + 8U) == UINT32_C(0x120e612c));
        CHECK(load_u32le(table + 1020U) == UINT32_C(0x0702ef8d));
    }
}

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *stream = fopen(path, "rb");
    uint8_t *data;
    long length;

    CHECK(stream != NULL);
    CHECK(fseek(stream, 0, SEEK_END) == 0);
    length = ftell(stream);
    CHECK(length >= 0);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);
    data = (uint8_t *)malloc(length != 0 ? (size_t)length : 1U);
    CHECK(data != NULL);
    CHECK(fread(data, 1U, (size_t)length, stream) == (size_t)length);
    CHECK(fclose(stream) == 0);
    *size_out = (size_t)length;
    return data;
}

static size_t run_real_file(const char *path)
{
    static const size_t widths[] = {1U, 2U, 4U, 1U, 4U, 2U, 4U};
    uint8_t whole_reference_state[STATE_BYTES] = {0};
    uint8_t whole_native_state[STATE_BYTES] = {0};
    uint8_t whole_reference_table[TABLE_BYTES] = {0};
    uint8_t whole_native_table[TABLE_BYTES] = {0};
    uint8_t chunk_reference_state[STATE_BYTES] = {0};
    uint8_t chunk_native_state[STATE_BYTES] = {0};
    uint8_t chunk_reference_table[TABLE_BYTES] = {0};
    uint8_t chunk_native_table[TABLE_BYTES] = {0};
    uint8_t *data;
    size_t size;
    size_t payload_size;
    size_t offset = 0U;
    size_t ordinal = 0U;
    uint32_t expected;

    data = read_file(path, &size);
    CHECK(size >= SAVE_HEADER_BYTES + SAVE_FOOTER_BYTES);
    CHECK(memcmp(data, s_save_header, sizeof s_save_header) == 0);
    payload_size = size - SAVE_HEADER_BYTES - SAVE_FOOTER_BYTES;
    expected = load_u32le(data + size - SAVE_FOOTER_BYTES) ^
               UINT32_C(0x96696996);

    store_u32le(whole_reference_state + 8U, UINT32_C(0xfedcba76));
    store_u32le(whole_reference_state + 12U, 1U);
    memcpy(whole_native_state, whole_reference_state,
           sizeof whole_native_state);
    compare_one_feed(
        whole_reference_state, whole_native_state,
        whole_reference_table, whole_native_table,
        data + SAVE_HEADER_BYTES, payload_size);
    CHECK(load_u32le(whole_reference_state + 8U) == expected);

    store_u32le(chunk_reference_state + 8U, UINT32_C(0xfedcba76));
    store_u32le(chunk_reference_state + 12U, 1U);
    memcpy(chunk_native_state, chunk_reference_state,
           sizeof chunk_native_state);
    while (offset < payload_size) {
        size_t count = widths[ordinal %
                              (sizeof widths / sizeof widths[0])];

        if (count > payload_size - offset)
            count = payload_size - offset;
        compare_one_feed(chunk_reference_state, chunk_native_state,
                         chunk_reference_table, chunk_native_table,
                         data + SAVE_HEADER_BYTES + offset, count);
        offset += count;
        ++ordinal;
    }
    CHECK(load_u32le(chunk_reference_state + 8U) == expected);
    CHECK(memcmp(whole_reference_state, chunk_reference_state,
                 STATE_BYTES) == 0);
    CHECK(memcmp(whole_native_state, chunk_native_state,
                 STATE_BYTES) == 0);
    CHECK(memcmp(whole_reference_table, chunk_reference_table,
                 TABLE_BYTES) == 0);
    CHECK(memcmp(whole_native_table, chunk_native_table,
                 TABLE_BYTES) == 0);
    free(data);
    return size;
}

int main(int argc, char **argv)
{
    size_t real_bytes = 0U;
    int index;

    run_synthetic();
    for (index = 1; index < argc; ++index)
        real_bytes += run_real_file(argv[index]);
    printf("Vita save checksum oracle: PASS; synthetic=%u; real=%d; "
           "bytes=%lu; checks=%u\n",
           SYNTHETIC_CASES, argc - 1, (unsigned long)real_bytes, s_checks);
    return 0;
}
