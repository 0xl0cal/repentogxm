#ifndef ISAAC_MANAGER_TINFL_H
#define ISAAC_MANAGER_TINFL_H

#include <stdint.h>

/* Status codes (ISAAC_VITA_ARCHIVE_MINIZ_*) and the 0x2af0-byte state size
 * of the runtime's tinfl adaptation. */
#include "host_vita_archive_miniz_native.h"

/* tinfl flags of miniz v1.15, copied from host_vita_archive_miniz_native.c
 * (enum at the top of that file; the same values as NP_TINFL_* in
 * host_vita_native_png.c).  COMPUTE_ADLER32 is implied by PARSE_ZLIB_HEADER
 * there, so the manager never passes it. */
enum {
    MANAGER_TINFL_PARSE_ZLIB_HEADER = 1,
    MANAGER_TINFL_HAS_MORE_INPUT = 2,
    MANAGER_TINFL_NON_WRAPPING_OUTPUT = 4
};

/*
 * host_vita_archive_miniz_native.c's tinfl_decompress() compiled once more
 * under this name for the manager (manager_tinfl.c explains the one hook
 * that differs).  Arguments and results are those of
 * isaac_vita_archive_miniz_native(); *reserved_symbol is set to 1 when the
 * stream used a reserved length (286/287) or distance (30/31) symbol, which
 * the manager's callers treat as a corrupt stream.  The hook is consulted
 * only with MANAGER_TINFL_NON_WRAPPING_OUTPUT, which every manager caller
 * passes.  The caller clears *reserved_symbol and zeroes the state blob
 * (ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES, 4-byte aligned) before a stream.
 */
int manager_tinfl_decompress(void *state, const uint8_t *input,
                             uint32_t *input_size, uint8_t *output_start,
                             uint8_t *output_next, uint32_t *output_size,
                             uint32_t flags, uint32_t *reserved_symbol);

#endif
