#ifndef HOST_VITA_NATIVE_PNG_LZ_NEON_H
#define HOST_VITA_NATIVE_PNG_LZ_NEON_H

#include <stdint.h>
#include <string.h>

#ifndef ISAAC_VITA_NATIVE_PNG_LZ_NEON
#define ISAAC_VITA_NATIVE_PNG_LZ_NEON 0
#endif
#ifndef ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON
#define ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON 0
#endif

#if ISAAC_VITA_NATIVE_PNG_LZ_NEON
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define ISAAC_NP_LZ_HAS_NEON 1
#else
#define ISAAC_NP_LZ_HAS_NEON 0
#if defined(__vita__)
#error "Native PNG LZ NEON requires the target ARM NEON contract"
#endif
#endif

/* This is a DEFLATE forward recurrence, not memmove: newly written bytes
 * become input when length exceeds distance. The caller has already proved
 * a nonwrapping source within output and the whole match within output_end.
 *
 * Dist=1 repeats one initialized byte. Dist>=16 permits independent 16-byte
 * loads/stores, even for an overlapping match, because every loaded chunk
 * precedes the corresponding store. Neither access crosses the logical
 * match end; the short tail remains a byte recurrence. No alignment, padding,
 * restrictive alias, or Cortex-A9 unaligned scalar-word contract is needed.
 *
 * Prior art: zlib-ng 2.2.4 chunkset_tpl.h uses the same dist=1 / wide-distance
 * split. Its padded chunk-copy assumptions are deliberately not used here.
 * Short matches and distances 2..15 keep the original tinfl implementation. */
static inline int isaac_np_lz_match_try(uint8_t *output, const uint8_t *source,
                                       uint32_t length, uint32_t distance)
{
    uint32_t i = 0U;
    if (length < 16U || (distance != 1U && distance < 16U))
        return 0;

    if (distance == 1U) {
#if ISAAC_NP_LZ_HAS_NEON
        uint8x16_t repeated = vdupq_n_u8(source[0]);
        for (; length - i >= 16U; i += 16U)
            vst1q_u8(output + i, repeated);
        for (; i < length; ++i)
            output[i] = source[0];
#else
        memset(output, source[0], length);
#endif
    } else {
        for (; length - i >= 16U; i += 16U) {
#if ISAAC_NP_LZ_HAS_NEON
            uint8x16_t bytes = vld1q_u8(source + i);
            vst1q_u8(output + i, bytes);
#else
            /* Each individual chunk has disjoint source/destination ranges;
             * calling memcpy for the whole overlapping match would be UB. */
            memcpy(output + i, source + i, 16U);
#endif
        }
        for (; i < length; ++i)
            output[i] = source[i];
    }
    return 1;
}

#if ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON
/* Separate opt-in from the existing four-argument helper. The inflater passes
 * its actual initialized, nonwrapping output prefix, not available capacity.
 * Loading the previous 16 bytes avoids scalar-word alignment/alias promises;
 * only their final four bytes seed the distance-four recurrence. Since 4
 * divides 16, every full output vector repeats that same seed. No lookup
 * table or reads of future output are needed. Short/early matches reject
 * without any load or store and keep the original scalar coroutine path. */
static inline int isaac_np_lz_d4_try(uint8_t *output, const uint8_t *source,
                                    uint32_t length, uint32_t distance,
                                    uint32_t initialized_history)
{
    uint32_t i = 0U;
    if (distance != 4U || length < 32U || initialized_history < 16U)
        return 0;
#if ISAAC_NP_LZ_HAS_NEON
    uint8x16_t previous = vld1q_u8(output - 16U);
    uint32x2_t last_words = vreinterpret_u32_u8(vget_high_u8(previous));
    uint8x16_t repeated = vreinterpretq_u8_u32(vdupq_lane_u32(last_words, 1));
    for (; length - i >= 16U; i += 16U)
        vst1q_u8(output + i, repeated);
#endif
    /* The original byte recurrence also handles every exact 0..15-byte tail.
     * Non-ARM host builds use it for the complete match as a byte reference. */
    for (; i < length; ++i)
        output[i] = source[i];
    return 1;
}
#endif
#endif
#endif
