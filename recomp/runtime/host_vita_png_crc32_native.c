#include "host_vita_png_crc32_native.h"

#include <stddef.h>

#define CRC_BYTE() do { \
    crc = table[(uint8_t)(crc ^ *data++)] ^ (crc >> 8); \
} while (0)

uint32_t isaac_vita_png_crc32_native(
    uint32_t crc, const uint8_t *data, uint32_t size,
    const uint32_t table[256])
{
    if (data == NULL)
        return 0U;

    crc = ~crc;
    while (size >= 8U) {
        CRC_BYTE();
        CRC_BYTE();
        CRC_BYTE();
        CRC_BYTE();
        CRC_BYTE();
        CRC_BYTE();
        CRC_BYTE();
        CRC_BYTE();
        size -= 8U;
    }
    while (size != 0U) {
        CRC_BYTE();
        --size;
    }
    return ~crc;
}

#undef CRC_BYTE
