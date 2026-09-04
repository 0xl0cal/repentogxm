#ifndef ISAAC_HOST_VITA_ZLIB_INFLATE_FLUSH_NATIVE_H
#define ISAAC_HOST_VITA_ZLIB_INFLATE_FLUSH_NATIVE_H

#include <stddef.h>
#include <stdint.h>

enum {
    ISAAC_VITA_ZLIB_INFLATE_FLUSH_FALLBACK = 0,
    ISAAC_VITA_ZLIB_INFLATE_FLUSH_HANDLED = 1
};

/* Offset-bounded host view of zlib 1.1.4's inflate_blocks window and
 * z_stream output cursor.  The guest shim authenticates the backing
 * allocations; this helper owns only the bounded flush algorithm and is
 * differential-tested against an independent bytewise infutil.c reference. */
typedef struct isaac_vita_zlib_inflate_flush_state {
    uint8_t *window;
    size_t window_capacity;
    uint32_t read_offset;
    uint32_t write_offset;
    uint8_t *output;
    size_t output_capacity;
    uint32_t next_out_offset;
    uint32_t avail_out;
    uint32_t total_out;
    uint32_t check;
    int32_t result;
    uint32_t check_enabled;
} isaac_vita_zlib_inflate_flush_state;

int isaac_vita_zlib_inflate_flush_native_try(
    isaac_vita_zlib_inflate_flush_state *state,
    uint32_t *segment_count_out);

/* Exact zlib 1.1.4 Adler-32 leaf used by inflate_flush's authenticated
 * checkfn==adler32 path. */
uint32_t isaac_vita_zlib_114_adler32(
    uint32_t adler, const uint8_t *buffer, uint32_t length);

#endif
