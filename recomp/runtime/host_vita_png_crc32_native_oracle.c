#include "host_vita_png_crc32_native.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        fprintf(stderr, "PNG CRC32 native oracle failed at %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t s_table[256];
static uint32_t s_checks;

static void make_table(void)
{
    uint32_t value;

    for (value = 0U; value < 256U; ++value) {
        uint32_t crc = value;
        unsigned bit;
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        s_table[value] = crc;
    }
}

static uint32_t reference_crc32(
    uint32_t crc, const uint8_t *data, uint32_t size)
{
    uint32_t index;

    if (data == NULL)
        return 0U;
    crc = ~crc;
    for (index = 0U; index < size; ++index) {
        unsigned bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

static uint32_t random32(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

int main(void)
{
    uint8_t data[4096];
    uint32_t random = 0x6c8e9cf5U;
    uint32_t size;
    uint32_t iteration;

    make_table();
    for (size = 0U; size < sizeof data; ++size)
        data[size] = (uint8_t)random32(&random);

    CHECK(isaac_vita_png_crc32_native(
              0x12345678U, NULL, 777U, s_table) == 0U);
    CHECK(isaac_vita_png_crc32_native(
              0x89abcdefU, data, 0U, s_table) == 0x89abcdefU);
    CHECK(isaac_vita_png_crc32_native(
              0U, (const uint8_t *)"123456789", 9U, s_table) ==
          0xcbf43926U);

    for (size = 0U; size <= sizeof data; ++size) {
        uint32_t seed = random32(&random);
        CHECK(isaac_vita_png_crc32_native(seed, data, size, s_table) ==
              reference_crc32(seed, data, size));
    }
    for (iteration = 0U; iteration < 512U; ++iteration) {
        uint32_t total = random32(&random) % (sizeof data + 1U);
        uint32_t split = total == 0U ? 0U : random32(&random) % total;
        uint32_t seed = random32(&random);
        uint32_t chained = isaac_vita_png_crc32_native(
            seed, data, split, s_table);
        chained = isaac_vita_png_crc32_native(
            chained, data + split, total - split, s_table);
        CHECK(chained == reference_crc32(seed, data, total));
    }

    printf("PNG CRC32 native oracle: PASS; checks=%u; bytes=0..4096\n",
           s_checks);
    return 0;
}
