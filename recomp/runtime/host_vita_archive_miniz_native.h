#ifndef HOST_VITA_ARCHIVE_MINIZ_NATIVE_H
#define HOST_VITA_ARCHIVE_MINIZ_NATIVE_H

#include <stdint.h>

enum {
    ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES = 0x2af0U,
    /* All coroutine scalar fields precede the first Huffman table. */
    ISAAC_VITA_ARCHIVE_MINIZ_HEADER_BYTES = 0x40U,
    ISAAC_VITA_ARCHIVE_MINIZ_INPUT_MAX = 0x7ffU,
    ISAAC_VITA_ARCHIVE_MINIZ_OUTPUT_BYTES = 0x400U
};

enum isaac_vita_archive_miniz_status {
    ISAAC_VITA_ARCHIVE_MINIZ_BAD_PARAM = -3,
    ISAAC_VITA_ARCHIVE_MINIZ_ADLER32_MISMATCH = -2,
    ISAAC_VITA_ARCHIVE_MINIZ_FAILED = -1,
    ISAAC_VITA_ARCHIVE_MINIZ_DONE = 0,
    ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT = 1,
    ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT = 2
};

/* Fixed-width form of miniz.c v1.15's 32-bit tinfl_decompress().  The caller
 * owns the 0x2af0-byte state blob; input/output sizes are consumed/produced. */
#if defined(_WIN32) && defined(ISAAC_VITA_ARCHIVE_MINIZ_TEST_EXPORT)
#define ISAAC_VITA_ARCHIVE_MINIZ_API __declspec(dllexport)
#else
#define ISAAC_VITA_ARCHIVE_MINIZ_API
#endif

ISAAC_VITA_ARCHIVE_MINIZ_API int isaac_vita_archive_miniz_native(
    void *state,
    const uint8_t *input,
    uint32_t *input_size,
    uint8_t *output_start,
    uint8_t *output_next,
    uint32_t *output_size,
    uint32_t flags);

#undef ISAAC_VITA_ARCHIVE_MINIZ_API

#endif
