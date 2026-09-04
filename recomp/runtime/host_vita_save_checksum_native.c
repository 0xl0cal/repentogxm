#include "host_vita_save_checksum_native.h"

#include <stddef.h>
#include <stdint.h>

enum {
    CHECKSUM_WORD_OFFSET = 0U,
    CHECKSUM_FILL_OFFSET = 4U,
    CHECKSUM_VALUE_OFFSET = 8U,
    CHECKSUM_MODE_OFFSET = 12U
};

static uint32_t checksum_load_u32le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void checksum_store_u32le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t checksum_arithmetic_shift_right_one(uint32_t value)
{
    return (value >> 1) | (value & UINT32_C(0x80000000));
}

static uint32_t checksum_table_word(uint32_t index)
{
    const uint32_t polynomial = UINT32_C(0xedb88320);
    uint32_t value = index;
    unsigned bit;

    /* Copy the frozen x86 exactly.  Its first step is SHR and the following
     * seven are SAR, so this is deliberately not the standard CRC32 table. */
    value = (value >> 1) ^ ((0U - (value & 1U)) & polynomial);
    for (bit = 1U; bit < 8U; ++bit)
        value = checksum_arithmetic_shift_right_one(value) ^
                ((0U - (value & 1U)) & polynomial);
    return value;
}

static void checksum_prepare_table(uint8_t *table)
{
    uint32_t index;

    /* sub_0025b340 uses table[1] itself as the lazy-init sentinel. */
    if (checksum_load_u32le(table + 4U) != 0U)
        return;
    for (index = 0U; index < ISAAC_VITA_SAVE_CHECKSUM_TABLE_WORDS; ++index)
        checksum_store_u32le(table + index * 4U,
                             checksum_table_word(index));
}

static void checksum_feed_word_mode(
    uint8_t *state, const uint8_t *data, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        uint8_t fill = state[CHECKSUM_FILL_OFFSET];

        state[fill] = data[index];
        fill = (uint8_t)(fill + 1U);
        state[CHECKSUM_FILL_OFFSET] = fill;
        if (fill >= 4U) {
            uint32_t value = checksum_load_u32le(
                state + CHECKSUM_VALUE_OFFSET);

            value = (value >> 1) + (value << 31);
            value += checksum_load_u32le(state + CHECKSUM_WORD_OFFSET);
            state[CHECKSUM_FILL_OFFSET] = 0U;
            checksum_store_u32le(state + CHECKSUM_VALUE_OFFSET, value);
        }
    }
}

static void checksum_feed_crc_mode(
    uint8_t *state, const uint8_t *data, size_t size, uint8_t *table)
{
    uint32_t value;
    size_t index;

    checksum_prepare_table(table);
    value = ~checksum_load_u32le(state + CHECKSUM_VALUE_OFFSET);
    checksum_store_u32le(state + CHECKSUM_VALUE_OFFSET, value);
    for (index = 0U; index < size; ++index) {
        const uint32_t table_index =
            ((uint32_t)data[index] ^ (value & 0xffU)) & 0xffU;

        value = (value >> 8) ^
                checksum_load_u32le(table + table_index * 4U);
        checksum_store_u32le(state + CHECKSUM_VALUE_OFFSET, value);
    }
    checksum_store_u32le(state + CHECKSUM_VALUE_OFFSET, ~value);
}

void isaac_vita_save_checksum_native_feed(
    uint8_t *state, const uint8_t *data, size_t size, uint8_t *table)
{
    const uint32_t mode = checksum_load_u32le(
        state + CHECKSUM_MODE_OFFSET);

    if (mode == 0U)
        checksum_feed_word_mode(state, data, size);
    else
        checksum_feed_crc_mode(state, data, size, table);
}
