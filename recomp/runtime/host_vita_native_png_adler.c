/* PNG-only exact Adler-32 acceleration. The column/prefix decomposition is
 * established by zlib-ng's ARM NEON Adler source. This bounded 16-byte / 4096-
 * byte implementation uses only ARMv7 NEON, no padding or alignment promise.
 * Selected by ISAAC_VITA_NATIVE_PNG_ADLER_NEON; prior-art attribution is in
 * THIRD_PARTY.md (zlib section). */
#include "host_vita_native_png_adler.h"

#if ISAAC_NP_ADLER_NEON_AVAILABLE
#include <arm_neon.h>

static uint32_t np_adler_sum4(uint32x4_t value)
{
    uint32x2_t sum = vadd_u32(vget_low_u32(value), vget_high_u32(value));
    sum = vpadd_u32(sum, sum);
    return vget_lane_u32(sum, 0);
}
#endif

uint32_t isaac_np_adler32_update(uint32_t seed, const uint8_t *data,
                                uint32_t bytes)
{
    uint32_t sum1 = seed & 0xffffU;
    uint32_t sum2 = seed >> 16U;

    /* tinfl leaves even a noncanonical input seed unchanged at length zero. */
    if (bytes == 0U)
        return seed;

#if ISAAC_NP_ADLER_NEON_AVAILABLE
    while (bytes >= 16U) {
        uint32_t block = bytes < 4096U ? bytes & ~15U : 4096U;
        uint32_t left = block;
        uint32x4_t sums = vdupq_n_u32(0);
        uint32x4_t prefixes = vdupq_n_u32(0);
        uint16x8_t columns_lo = vdupq_n_u16(0);
        uint16x8_t columns_hi = vdupq_n_u16(0);
        uint32x4_t weighted;
        /* A column accumulates at most 256*255=65280. Prefix and weighted
         * totals, including arbitrary seed halves, stay below 2^32. */
        do {
            uint8x16_t value = vld1q_u8(data);
            prefixes = vaddq_u32(prefixes, sums);
            sums = vpadalq_u16(sums, vpaddlq_u8(value));
            columns_lo = vaddw_u8(columns_lo, vget_low_u8(value));
            columns_hi = vaddw_u8(columns_hi, vget_high_u8(value));
            data += 16U;
            left -= 16U;
        } while (left != 0U);

        /* Each column's coefficient within one 16-byte block: 16..1.
         * Prior complete blocks contribute 16*their running byte sums. */
        weighted = vmull_u16(vget_low_u16(columns_lo),
                             (uint16x4_t){16U, 15U, 14U, 13U});
        weighted = vmlal_u16(weighted, vget_high_u16(columns_lo),
                             (uint16x4_t){12U, 11U, 10U, 9U});
        weighted = vmlal_u16(weighted, vget_low_u16(columns_hi),
                             (uint16x4_t){8U, 7U, 6U, 5U});
        weighted = vmlal_u16(weighted, vget_high_u16(columns_hi),
                             (uint16x4_t){4U, 3U, 2U, 1U});
        sum2 += block * sum1 + 16U * np_adler_sum4(prefixes) +
                np_adler_sum4(weighted);
        sum1 += np_adler_sum4(sums);
        sum1 %= 65521U;
        sum2 %= 65521U;
        bytes -= block;
    }
#endif
    /* NEON leaves <16 bytes. The host/OFF test path uses the same bounded
     * scalar recurrence for the entire input; neither path reads past bytes. */
    while (bytes != 0U) {
        uint32_t block = bytes < 4096U ? bytes : 4096U;
        uint32_t i;
        for (i = 0U; i < block; ++i) {
            sum1 += *data++;
            sum2 += sum1;
        }
        sum1 %= 65521U;
        sum2 %= 65521U;
        bytes -= block;
    }
    return (sum2 << 16U) | sum1;
}
