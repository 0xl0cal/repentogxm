#ifndef HOST_VITA_NATIVE_PNG_ADLER_H
#define HOST_VITA_NATIVE_PNG_ADLER_H

#include <stdint.h>

#ifndef ISAAC_VITA_NATIVE_PNG_ADLER_NEON
#define ISAAC_VITA_NATIVE_PNG_ADLER_NEON 0
#endif
#if ISAAC_VITA_NATIVE_PNG_ADLER_NEON && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define ISAAC_NP_ADLER_NEON_AVAILABLE 1
#else
#define ISAAC_NP_ADLER_NEON_AVAILABLE 0
#if ISAAC_VITA_NATIVE_PNG_ADLER_NEON && defined(__vita__)
#error "Native PNG Adler NEON requires the target ARM NEON contract"
#endif
#endif

/* Exact tinfl incremental Adler semantics. data names bytes readable bytes;
 * bytes==0 returns seed unchanged without dereferencing data. No allocation,
 * state, alignment/padding requirement, or checksum-validation bypass. */
uint32_t isaac_np_adler32_update(uint32_t seed, const uint8_t *data,
                                uint32_t bytes);

/* Same signature/state layout as isaac_vita_archive_miniz_native, compiled
 * from that same source with the Adler update changed and optionally bounded
 * LZ copies. PNG opt-in only; archive callers keep their existing symbol and
 * scalar implementation. */
int isaac_vita_png_miniz_native(
    void *state, const uint8_t *input, uint32_t *input_size,
    uint8_t *output_start, uint8_t *output_next, uint32_t *output_size,
    uint32_t flags);

/* Optional PNG-only observation. Caller zeroes history_unsafe once at stream
 * start and retains it across coroutine calls. Never changes decoder status
 * or output; nonzero forbids caching that result. Archive ABI is untouched. */
int isaac_vita_png_miniz_reuse(
    void *state, const uint8_t *input, uint32_t *input_size,
    uint8_t *output_start, uint8_t *output_next, uint32_t *output_size,
    uint32_t flags, uint32_t *history_unsafe);

#endif
