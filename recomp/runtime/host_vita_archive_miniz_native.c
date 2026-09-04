/*
 * The decompressor below is a fixed-width adaptation of Rich Geldreich's
 * public-domain miniz.c v1.15 tinfl_decompress(), last updated 2013-10-13.
 * It keeps the original coroutine state numbers and 32-bit object layout used
 * by the frozen Isaac PE.  See the provenance note in THIRD_PARTY.md.
 */
#include "host_vita_archive_miniz_native.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
    TINFL_FLAG_HAS_MORE_INPUT = 2,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
    TINFL_FLAG_COMPUTE_ADLER32 = 8,
    TINFL_MAX_HUFF_TABLES = 3,
    TINFL_MAX_HUFF_SYMBOLS_0 = 288,
    TINFL_MAX_HUFF_SYMBOLS_1 = 32,
    TINFL_FAST_LOOKUP_BITS = 10,
    TINFL_FAST_LOOKUP_SIZE = 1 << TINFL_FAST_LOOKUP_BITS
};

typedef struct archive_tinfl_huff_table {
    uint8_t code_size[TINFL_MAX_HUFF_SYMBOLS_0];
    int16_t look_up[TINFL_FAST_LOOKUP_SIZE];
    int16_t tree[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
} archive_tinfl_huff_table;

typedef struct archive_tinfl_state {
    uint32_t state;
    uint32_t num_bits;
    uint32_t zhdr0;
    uint32_t zhdr1;
    uint32_t z_adler32;
    uint32_t final;
    uint32_t type;
    uint32_t check_adler32;
    uint32_t dist;
    uint32_t counter;
    uint32_t num_extra;
    uint32_t table_sizes[TINFL_MAX_HUFF_TABLES];
    uint32_t bit_buf;
    uint32_t dist_from_out_buf_start;
    archive_tinfl_huff_table tables[TINFL_MAX_HUFF_TABLES];
    uint8_t raw_header[4];
    uint8_t len_codes[
        TINFL_MAX_HUFF_SYMBOLS_0 + TINFL_MAX_HUFF_SYMBOLS_1 + 137];
} archive_tinfl_state;

_Static_assert(sizeof(archive_tinfl_huff_table) == 0xda0U,
               "frozen tinfl Huffman table layout changed");
_Static_assert(offsetof(archive_tinfl_state, bit_buf) == 0x38U,
               "frozen tinfl bit-buffer offset changed");
_Static_assert(offsetof(archive_tinfl_state, tables) == 0x40U,
               "frozen tinfl table offset changed");
_Static_assert(offsetof(archive_tinfl_state, raw_header) == 0x2920U,
               "frozen tinfl raw-header offset changed");
_Static_assert(offsetof(archive_tinfl_state, len_codes) == 0x2924U,
               "frozen tinfl code-length offset changed");
_Static_assert(sizeof(archive_tinfl_state) ==
                   ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES,
               "frozen tinfl state size changed");

#define TINFL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TINFL_MAX(a, b) ((a) > (b) ? (a) : (b))
#define TINFL_CLEAR(value) memset(&(value), 0, sizeof(value))
#define TINFL_MACRO_END while (0)

#define TINFL_CR_BEGIN switch (r->state) { case 0:
#define TINFL_CR_RETURN(state_index, result) do { \
    status = (result); \
    r->state = (state_index); \
    goto common_exit; \
    case state_index:; \
} TINFL_MACRO_END
#define TINFL_CR_RETURN_FOREVER(state_index, result) do { \
    for (;;) { \
        TINFL_CR_RETURN(state_index, result); \
    } \
} TINFL_MACRO_END
#define TINFL_CR_FINISH }

#define TINFL_GET_BYTE(state_index, value) do { \
    if (input_cur >= input_end) { \
        for (;;) { \
            if (flags & TINFL_FLAG_HAS_MORE_INPUT) { \
                TINFL_CR_RETURN(state_index, \
                                ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT); \
                if (input_cur < input_end) { \
                    (value) = *input_cur++; \
                    break; \
                } \
            } else { \
                (value) = 0; \
                break; \
            } \
        } \
    } else { \
        (value) = *input_cur++; \
    } \
} TINFL_MACRO_END

#define TINFL_NEED_BITS(state_index, count) do { \
    uint32_t byte_value; \
    TINFL_GET_BYTE(state_index, byte_value); \
    bit_buf |= byte_value << num_bits; \
    num_bits += 8; \
} while (num_bits < (uint32_t)(count))

#define TINFL_SKIP_BITS(state_index, count) do { \
    if (num_bits < (uint32_t)(count)) \
        TINFL_NEED_BITS(state_index, count); \
    bit_buf >>= (count); \
    num_bits -= (count); \
} TINFL_MACRO_END

#define TINFL_GET_BITS(state_index, value, count) do { \
    if (num_bits < (uint32_t)(count)) \
        TINFL_NEED_BITS(state_index, count); \
    (value) = bit_buf & ((1U << (count)) - 1U); \
    bit_buf >>= (count); \
    num_bits -= (count); \
} TINFL_MACRO_END

#define TINFL_HUFF_BITBUF_FILL(state_index, table) do { \
    temp = (table)->look_up[bit_buf & (TINFL_FAST_LOOKUP_SIZE - 1)]; \
    if (temp >= 0) { \
        code_len = (uint32_t)temp >> 9; \
        if (code_len && num_bits >= code_len) \
            break; \
    } else if (num_bits > TINFL_FAST_LOOKUP_BITS) { \
        code_len = TINFL_FAST_LOOKUP_BITS; \
        do { \
            temp = (table)->tree[~temp + \
                ((bit_buf >> code_len++) & 1U)]; \
        } while (temp < 0 && num_bits >= code_len + 1U); \
        if (temp >= 0) \
            break; \
    } \
    TINFL_GET_BYTE(state_index, byte_value); \
    bit_buf |= byte_value << num_bits; \
    num_bits += 8; \
} while (num_bits < 15U)

#define TINFL_HUFF_DECODE(state_index, symbol, table) do { \
    int temp; \
    uint32_t code_len; \
    uint32_t byte_value; \
    if (num_bits < 15U) { \
        if ((uint32_t)(input_end - input_cur) < 2U) { \
            TINFL_HUFF_BITBUF_FILL(state_index, table); \
        } else { \
            bit_buf |= (uint32_t)input_cur[0] << num_bits; \
            bit_buf |= (uint32_t)input_cur[1] << (num_bits + 8U); \
            input_cur += 2; \
            num_bits += 16; \
        } \
    } \
    temp = (table)->look_up[bit_buf & (TINFL_FAST_LOOKUP_SIZE - 1)]; \
    if (temp >= 0) { \
        code_len = (uint32_t)temp >> 9; \
        temp &= 511; \
    } else { \
        code_len = TINFL_FAST_LOOKUP_BITS; \
        do { \
            temp = (table)->tree[~temp + \
                ((bit_buf >> code_len++) & 1U)]; \
        } while (temp < 0); \
    } \
    (symbol) = (uint32_t)temp; \
    bit_buf >>= code_len; \
    num_bits -= code_len; \
} TINFL_MACRO_END

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

int isaac_vita_archive_miniz_native(
    void *state,
    const uint8_t *input,
    uint32_t *input_size,
    uint8_t *output_start,
    uint8_t *output_next,
    uint32_t *output_size,
    uint32_t flags)
{
    static const int length_base[31] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0
    };
    static const int length_extra[31] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0
    };
    static const int dist_base[32] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
        12289, 16385, 24577, 0, 0
    };
    static const int dist_extra[32] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 0, 0
    };
    static const uint8_t length_dezigzag[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    static const int min_table_sizes[3] = { 257, 1, 4 };
    archive_tinfl_state *r = (archive_tinfl_state *)state;
    int status = ISAAC_VITA_ARCHIVE_MINIZ_FAILED;
    uint32_t num_bits;
    uint32_t dist;
    uint32_t counter;
    uint32_t num_extra;
    uint32_t bit_buf;
    const uint8_t *input_cur = input;
    const uint8_t *const input_end = input + *input_size;
    uint8_t *output_cur = output_next;
    uint8_t *const output_end = output_next + *output_size;
    uint32_t output_mask =
        (flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) ? UINT32_MAX :
        (uint32_t)(output_next - output_start) + *output_size - 1U;
    uint32_t dist_from_output_start;

    if (((output_mask + 1U) & output_mask) || output_next < output_start) {
        *input_size = 0U;
        *output_size = 0U;
        return ISAAC_VITA_ARCHIVE_MINIZ_BAD_PARAM;
    }

    num_bits = r->num_bits;
    bit_buf = r->bit_buf;
    dist = r->dist;
    counter = r->counter;
    num_extra = r->num_extra;
    dist_from_output_start = r->dist_from_out_buf_start;

    TINFL_CR_BEGIN

    bit_buf = 0U;
    num_bits = 0U;
    dist = 0U;
    counter = 0U;
    num_extra = 0U;
    r->zhdr0 = 0U;
    r->zhdr1 = 0U;
    r->z_adler32 = 1U;
    r->check_adler32 = 1U;
    if (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) {
        TINFL_GET_BYTE(1, r->zhdr0);
        TINFL_GET_BYTE(2, r->zhdr1);
        counter = ((r->zhdr0 * 256U + r->zhdr1) % 31U != 0U) ||
                  (r->zhdr1 & 32U) || ((r->zhdr0 & 15U) != 8U);
        if (!(flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)) {
            counter |= (1U << (8U + (r->zhdr0 >> 4U))) > 32768U ||
                output_mask + 1U < (1U << (8U + (r->zhdr0 >> 4U)));
        }
        if (counter)
            TINFL_CR_RETURN_FOREVER(
                36, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
    }

    do {
        TINFL_GET_BITS(3, r->final, 3);
        r->type = r->final >> 1;
        if (r->type == 0U) {
            TINFL_SKIP_BITS(5, num_bits & 7U);
            for (counter = 0U; counter < 4U; ++counter) {
                if (num_bits)
                    TINFL_GET_BITS(6, r->raw_header[counter], 8);
                else
                    TINFL_GET_BYTE(7, r->raw_header[counter]);
            }
            counter = r->raw_header[0] | (r->raw_header[1] << 8U);
            if (counter != (0xffffU ^
                    (r->raw_header[2] | (r->raw_header[3] << 8U)))) {
                TINFL_CR_RETURN_FOREVER(
                    39, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
            }
            while (counter && num_bits) {
                TINFL_GET_BITS(51, dist, 8);
                while (output_cur >= output_end)
                    TINFL_CR_RETURN(
                        52, ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT);
                *output_cur++ = (uint8_t)dist;
                --counter;
            }
            while (counter) {
                uint32_t count;
                while (output_cur >= output_end)
                    TINFL_CR_RETURN(
                        9, ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT);
                while (input_cur >= input_end) {
                    if (flags & TINFL_FLAG_HAS_MORE_INPUT) {
                        TINFL_CR_RETURN(
                            38, ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT);
                    } else {
                        TINFL_CR_RETURN_FOREVER(
                            40, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
                    }
                }
                count = TINFL_MIN(
                    TINFL_MIN((uint32_t)(output_end - output_cur),
                              (uint32_t)(input_end - input_cur)), counter);
                memcpy(output_cur, input_cur, count);
                input_cur += count;
                output_cur += count;
                counter -= count;
            }
        } else if (r->type == 3U) {
            TINFL_CR_RETURN_FOREVER(10, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
        } else {
            if (r->type == 1U) {
                uint8_t *code = r->tables[0].code_size;
                uint32_t index;
                r->table_sizes[0] = 288U;
                r->table_sizes[1] = 32U;
                memset(r->tables[1].code_size, 5, 32);
                for (index = 0U; index <= 143U; ++index)
                    *code++ = 8U;
                for (; index <= 255U; ++index)
                    *code++ = 9U;
                for (; index <= 279U; ++index)
                    *code++ = 7U;
                for (; index <= 287U; ++index)
                    *code++ = 8U;
            } else {
                static const uint8_t table_bits[3] = { 5, 5, 4 };
                for (counter = 0U; counter < 3U; ++counter) {
                    TINFL_GET_BITS(
                        11, r->table_sizes[counter], table_bits[counter]);
                    r->table_sizes[counter] +=
                        (uint32_t)min_table_sizes[counter];
                }
                TINFL_CLEAR(r->tables[2].code_size);
                for (counter = 0U;
                     counter < r->table_sizes[2]; ++counter) {
                    uint32_t symbol;
                    TINFL_GET_BITS(14, symbol, 3);
                    r->tables[2].code_size[length_dezigzag[counter]] =
                        (uint8_t)symbol;
                }
                r->table_sizes[2] = 19U;
            }

            for (; (int32_t)r->type >= 0; --r->type) {
                int tree_next;
                int tree_cur;
                archive_tinfl_huff_table *table = &r->tables[r->type];
                uint32_t index;
                uint32_t bit_index;
                uint32_t used_symbols;
                uint32_t total;
                uint32_t symbol_index;
                uint32_t next_code[17];
                uint32_t total_symbols[16];

                TINFL_CLEAR(total_symbols);
                TINFL_CLEAR(table->look_up);
                TINFL_CLEAR(table->tree);
                for (index = 0U; index < r->table_sizes[r->type]; ++index)
                    ++total_symbols[table->code_size[index]];
                used_symbols = 0U;
                total = 0U;
                next_code[0] = 0U;
                next_code[1] = 0U;
                for (index = 1U; index <= 15U; ++index) {
                    used_symbols += total_symbols[index];
                    total = (total + total_symbols[index]) << 1;
                    next_code[index + 1U] = total;
                }
                if (total != 65536U && used_symbols > 1U)
                    TINFL_CR_RETURN_FOREVER(
                        35, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);

                tree_next = -1;
                for (symbol_index = 0U;
                     symbol_index < r->table_sizes[r->type]; ++symbol_index) {
                    uint32_t reversed = 0U;
                    uint32_t current;
                    uint32_t size = table->code_size[symbol_index];
                    if (!size)
                        continue;
                    current = next_code[size]++;
                    for (bit_index = size; bit_index > 0U;
                         --bit_index, current >>= 1U) {
                        reversed = (reversed << 1U) | (current & 1U);
                    }
                    if (size <= TINFL_FAST_LOOKUP_BITS) {
                        int16_t value =
                            (int16_t)((size << 9U) | symbol_index);
                        while (reversed < TINFL_FAST_LOOKUP_SIZE) {
                            table->look_up[reversed] = value;
                            reversed += 1U << size;
                        }
                        continue;
                    }
                    tree_cur = table->look_up[
                        reversed & (TINFL_FAST_LOOKUP_SIZE - 1)];
                    if (!tree_cur) {
                        table->look_up[
                            reversed & (TINFL_FAST_LOOKUP_SIZE - 1)] =
                            (int16_t)tree_next;
                        tree_cur = tree_next;
                        tree_next -= 2;
                    }
                    reversed >>= TINFL_FAST_LOOKUP_BITS - 1;
                    for (bit_index = size;
                         bit_index > TINFL_FAST_LOOKUP_BITS + 1U;
                         --bit_index) {
                        tree_cur -= (int)((reversed >>= 1U) & 1U);
                        if (!table->tree[-tree_cur - 1]) {
                            table->tree[-tree_cur - 1] =
                                (int16_t)tree_next;
                            tree_cur = tree_next;
                            tree_next -= 2;
                        } else {
                            tree_cur = table->tree[-tree_cur - 1];
                        }
                    }
                    tree_cur -= (int)((reversed >>= 1U) & 1U);
                    table->tree[-tree_cur - 1] = (int16_t)symbol_index;
                }

                if (r->type == 2U) {
                    for (counter = 0U;
                         counter < r->table_sizes[0] + r->table_sizes[1];) {
                        static const uint8_t extra_count[3] = { 2, 3, 7 };
                        static const uint8_t extra_base[3] = { 3, 3, 11 };
                        uint32_t repeat;
                        TINFL_HUFF_DECODE(16, dist, &r->tables[2]);
                        if (dist < 16U) {
                            r->len_codes[counter++] = (uint8_t)dist;
                            continue;
                        }
                        if (dist == 16U && !counter)
                            TINFL_CR_RETURN_FOREVER(
                                17, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
                        num_extra = extra_count[dist - 16U];
                        TINFL_GET_BITS(18, repeat, num_extra);
                        repeat += extra_base[dist - 16U];
                        memset(r->len_codes + counter,
                               dist == 16U ? r->len_codes[counter - 1U] : 0,
                               repeat);
                        counter += repeat;
                    }
                    if (r->table_sizes[0] + r->table_sizes[1] != counter)
                        TINFL_CR_RETURN_FOREVER(
                            21, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
                    memcpy(r->tables[0].code_size, r->len_codes,
                           r->table_sizes[0]);
                    memcpy(r->tables[1].code_size,
                           r->len_codes + r->table_sizes[0],
                           r->table_sizes[1]);
                }
            }

            for (;;) {
                uint8_t *source;
                for (;;) {
                    if ((uint32_t)(input_end - input_cur) < 4U ||
                            (uint32_t)(output_end - output_cur) < 2U) {
                        TINFL_HUFF_DECODE(23, counter, &r->tables[0]);
                        if (counter >= 256U)
                            break;
                        while (output_cur >= output_end)
                            TINFL_CR_RETURN(
                                24,
                                ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT);
                        *output_cur++ = (uint8_t)counter;
                    } else {
                        int symbol2;
                        uint32_t code_len;
                        if (num_bits < 15U) {
                            bit_buf |= (uint32_t)read_le16(input_cur) <<
                                num_bits;
                            input_cur += 2;
                            num_bits += 16U;
                        }
                        symbol2 = r->tables[0].look_up[
                            bit_buf & (TINFL_FAST_LOOKUP_SIZE - 1)];
                        if (symbol2 >= 0) {
                            code_len = (uint32_t)symbol2 >> 9;
                        } else {
                            code_len = TINFL_FAST_LOOKUP_BITS;
                            do {
                                symbol2 = r->tables[0].tree[
                                    ~symbol2 +
                                    ((bit_buf >> code_len++) & 1U)];
                            } while (symbol2 < 0);
                        }
                        counter = (uint32_t)symbol2;
                        bit_buf >>= code_len;
                        num_bits -= code_len;
                        if (counter & 256U)
                            break;

                        if (num_bits < 15U) {
                            bit_buf |= (uint32_t)read_le16(input_cur) <<
                                num_bits;
                            input_cur += 2;
                            num_bits += 16U;
                        }
                        symbol2 = r->tables[0].look_up[
                            bit_buf & (TINFL_FAST_LOOKUP_SIZE - 1)];
                        if (symbol2 >= 0) {
                            code_len = (uint32_t)symbol2 >> 9;
                        } else {
                            code_len = TINFL_FAST_LOOKUP_BITS;
                            do {
                                symbol2 = r->tables[0].tree[
                                    ~symbol2 +
                                    ((bit_buf >> code_len++) & 1U)];
                            } while (symbol2 < 0);
                        }
                        bit_buf >>= code_len;
                        num_bits -= code_len;
                        output_cur[0] = (uint8_t)counter;
                        if (symbol2 & 256) {
                            ++output_cur;
                            counter = (uint32_t)symbol2;
                            break;
                        }
                        output_cur[1] = (uint8_t)symbol2;
                        output_cur += 2;
                    }
                }
                counter &= 511U;
                if (counter == 256U)
                    break;

                num_extra = (uint32_t)length_extra[counter - 257U];
                counter = (uint32_t)length_base[counter - 257U];
                if (num_extra) {
                    uint32_t extra_bits;
                    TINFL_GET_BITS(25, extra_bits, num_extra);
                    counter += extra_bits;
                }

                TINFL_HUFF_DECODE(26, dist, &r->tables[1]);
                num_extra = (uint32_t)dist_extra[dist];
                dist = (uint32_t)dist_base[dist];
                if (num_extra) {
                    uint32_t extra_bits;
                    TINFL_GET_BITS(27, extra_bits, num_extra);
                    dist += extra_bits;
                }

                dist_from_output_start =
                    (uint32_t)(output_cur - output_start);
                if (dist > dist_from_output_start &&
                        (flags & TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)) {
                    TINFL_CR_RETURN_FOREVER(
                        37, ISAAC_VITA_ARCHIVE_MINIZ_FAILED);
                }
                source = output_start +
                    ((dist_from_output_start - dist) & output_mask);

                if ((uint32_t)(output_end - TINFL_MAX(output_cur, source)) <
                        counter) {
                    while (counter--) {
                        while (output_cur >= output_end)
                            TINFL_CR_RETURN(
                                53,
                                ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT);
                        *output_cur++ = output_start[
                            (dist_from_output_start++ - dist) & output_mask];
                    }
                    continue;
                }
                if (counter >= 9U && counter <= dist) {
                    const uint8_t *source_end = source + (counter & ~7U);
                    do {
                        memcpy(output_cur, source, 8U);
                        output_cur += 8;
                        source += 8;
                    } while (source < source_end);
                    counter &= 7U;
                    if (counter < 3U) {
                        if (counter) {
                            output_cur[0] = source[0];
                            if (counter > 1U)
                                output_cur[1] = source[1];
                            output_cur += counter;
                        }
                        continue;
                    }
                }
                do {
                    output_cur[0] = source[0];
                    output_cur[1] = source[1];
                    output_cur[2] = source[2];
                    output_cur += 3;
                    source += 3;
                    counter -= 3U;
                } while ((int32_t)counter > 2);
                if ((int32_t)counter > 0) {
                    output_cur[0] = source[0];
                    if ((int32_t)counter > 1)
                        output_cur[1] = source[1];
                    output_cur += counter;
                }
            }
        }
    } while (!(r->final & 1U));

    if (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) {
        TINFL_SKIP_BITS(32, num_bits & 7U);
        for (counter = 0U; counter < 4U; ++counter) {
            uint32_t symbol;
            if (num_bits)
                TINFL_GET_BITS(41, symbol, 8);
            else
                TINFL_GET_BYTE(42, symbol);
            r->z_adler32 = (r->z_adler32 << 8U) | symbol;
        }
    }
    TINFL_CR_RETURN_FOREVER(34, ISAAC_VITA_ARCHIVE_MINIZ_DONE);
    TINFL_CR_FINISH

common_exit:
    r->num_bits = num_bits;
    r->bit_buf = bit_buf;
    r->dist = dist;
    r->counter = counter;
    r->num_extra = num_extra;
    r->dist_from_out_buf_start = dist_from_output_start;
    *input_size = (uint32_t)(input_cur - input);
    *output_size = (uint32_t)(output_cur - output_next);

    if ((flags & (TINFL_FLAG_PARSE_ZLIB_HEADER |
                  TINFL_FLAG_COMPUTE_ADLER32)) && status >= 0) {
        const uint8_t *cursor = output_next;
        uint32_t remaining = *output_size;
        uint32_t sum1 = r->check_adler32 & 0xffffU;
        uint32_t sum2 = r->check_adler32 >> 16U;
        uint32_t block = remaining % 5552U;
        while (remaining) {
            uint32_t index;
            for (index = 0U; index + 7U < block;
                 index += 8U, cursor += 8) {
                sum1 += cursor[0]; sum2 += sum1;
                sum1 += cursor[1]; sum2 += sum1;
                sum1 += cursor[2]; sum2 += sum1;
                sum1 += cursor[3]; sum2 += sum1;
                sum1 += cursor[4]; sum2 += sum1;
                sum1 += cursor[5]; sum2 += sum1;
                sum1 += cursor[6]; sum2 += sum1;
                sum1 += cursor[7]; sum2 += sum1;
            }
            for (; index < block; ++index) {
                sum1 += *cursor++;
                sum2 += sum1;
            }
            sum1 %= 65521U;
            sum2 %= 65521U;
            remaining -= block;
            block = 5552U;
        }
        r->check_adler32 = (sum2 << 16U) + sum1;
        if (status == ISAAC_VITA_ARCHIVE_MINIZ_DONE &&
                (flags & TINFL_FLAG_PARSE_ZLIB_HEADER) &&
                r->check_adler32 != r->z_adler32) {
            status = ISAAC_VITA_ARCHIVE_MINIZ_ADLER32_MISMATCH;
        }
    }
    return status;
}

#undef TINFL_HUFF_DECODE
#undef TINFL_HUFF_BITBUF_FILL
#undef TINFL_GET_BITS
#undef TINFL_SKIP_BITS
#undef TINFL_NEED_BITS
#undef TINFL_GET_BYTE
#undef TINFL_CR_FINISH
#undef TINFL_CR_RETURN_FOREVER
#undef TINFL_CR_RETURN
#undef TINFL_CR_BEGIN
#undef TINFL_MACRO_END
#undef TINFL_CLEAR
#undef TINFL_MAX
#undef TINFL_MIN
