#ifndef ISAAC_HOST_VITA_PNG_CRC32_NATIVE_H
#define ISAAC_HOST_VITA_PNG_CRC32_NATIVE_H

#include <stdint.h>

/* Exact zlib crc32 semantics.  table must contain the frozen 256-entry
 * reflected CRC-32 table authenticated by gen_all.py. */
uint32_t isaac_vita_png_crc32_native(
    uint32_t crc, const uint8_t *data, uint32_t size,
    const uint32_t table[256]);

#endif
