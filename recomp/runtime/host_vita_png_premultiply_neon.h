#ifndef HOST_VITA_PNG_PREMULTIPLY_NEON_H
#define HOST_VITA_PNG_PREMULTIPLY_NEON_H

#include <stdint.h>

#ifndef ISAAC_VITA_PNG_PREMULTIPLY_NEON
#define ISAAC_VITA_PNG_PREMULTIPLY_NEON 0
#endif
#ifndef ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON
#define ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON 0
#endif
#ifndef ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON
#define ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON 0
#endif
#if ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON != 0 && ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON != 1
#error ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON must be zero or one
#endif
#if ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON && !ISAAC_VITA_PNG_PREMULTIPLY_NEON
#error PNG linear premultiply requires the existing uniform-alpha NEON path
#endif
#if ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON != 0 && ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON != 1
#error ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON must be zero or one
#endif
#if ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON && !ISAAC_VITA_PNG_PREMULTIPLY_NEON
#error PNG binary premultiply requires the existing uniform-alpha NEON path
#endif
#if ISAAC_VITA_PNG_PREMULTIPLY_NEON != 0 && ISAAC_VITA_PNG_PREMULTIPLY_NEON != 1
#error ISAAC_VITA_PNG_PREMULTIPLY_NEON must be zero or one
#endif
#if ISAAC_VITA_PNG_PREMULTIPLY_NEON && !defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE)
#error PNG premultiply NEON requires the native scratch-owned premultiply seam
#endif

#if ISAAC_VITA_PNG_PREMULTIPLY_NEON && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
#define ISAAC_PNG_PREMULTIPLY_NEON_AVAILABLE 1
#include <arm_neon.h>

#if ISAAC_VITA_PNG_PREMULTIPLY_LINEAR_NEON
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error PNG linear premultiply word lanes require the Vita little-endian layout
#endif
/* Exhaustively equal to the frozen LINEAR table for all 256*256 inputs:
 * round(a*c/255) = (t + (t >> 8)) >> 8, t = a*c + 128.
 * Largest intermediate is 65407, so unsigned 16-bit arithmetic is exact.
 * This is not the game's gamma table and must never be selected for it. */
static inline uint8x8_t isaac_png_premultiply_linear8(uint8x8_t color,
                                                    uint8x8_t alpha)
{
    const uint16x8_t t = vaddq_u16(vmull_u8(color, alpha), vdupq_n_u16(128U));
    return vshrn_n_u16(vaddq_u16(t, vshrq_n_u16(t, 8)), 8);
}

static inline void isaac_png_premultiply_linear_block4(uint8_t *pixel)
{
    /* Interleaved lanes avoid the per-block tuple spills produced by Vita
     * GCC 10.3 for vld4/vst4 arithmetic. The alpha byte is the high byte of
     * each little-endian word; multiplication replicates it without carry. */
    const uint8x16_t input = vld1q_u8(pixel);
    const uint32x4_t alpha_word = vshrq_n_u32(vreinterpretq_u32_u8(input), 24);
    const uint8x16_t alpha = vreinterpretq_u8_u32(
        vmulq_n_u32(alpha_word, 0x01010101U));
    const uint8x16_t multiplied = vcombine_u8(
        isaac_png_premultiply_linear8(vget_low_u8(input), vget_low_u8(alpha)),
        isaac_png_premultiply_linear8(vget_high_u8(input), vget_high_u8(alpha)));
    const uint8x16_t alpha_mask = vreinterpretq_u8_u32(vdupq_n_u32(0xff000000U));
    vst1q_u8(pixel, vbslq_u8(alpha_mask, input, multiplied));
}

static inline void isaac_png_premultiply_linear16(uint8_t *pixel)
{
    /* The private scratch span admits rewriting unchanged alpha/opaque RGB.
     * Four blocks cover exactly sixteen logical pixels, never row padding. */
    isaac_png_premultiply_linear_block4(pixel);
    isaac_png_premultiply_linear_block4(pixel + 16U);
    isaac_png_premultiply_linear_block4(pixel + 32U);
    isaac_png_premultiply_linear_block4(pixel + 48U);
}
#endif

/* ARM ACLE vld4q_u8 deinterleaves exactly sixteen RGBA pixels. The caller
 * proves these 64 logical bytes remain in one row; no alignment or padding
 * promise is made. OR/AND of both alpha halves proves all-zero/all-255 bits
 * without an ARMv8 horizontal reduction. The separate binary option also
 * handles proven mixed 0/255 blocks; partial-alpha blocks remain unchanged.
 * Selected by ISAAC_VITA_PNG_PREMULTIPLY_NEON and _BINARY_NEON. */
static inline int isaac_png_premultiply_uniform16(uint8_t *pixel)
{
    const uint8x16x4_t rgba = vld4q_u8(pixel);
    const uint8x8_t low = vget_low_u8(rgba.val[3]);
    const uint8x8_t high = vget_high_u8(rgba.val[3]);
    if (vget_lane_u64(vreinterpret_u64_u8(vorr_u8(low, high)), 0) == 0U) {
        const uint8x16_t zero = vdupq_n_u8(0U);
        /* Alpha is already zero. Writing it with RGB is exact, but writes
         * more logical bytes than the scalar RGB-only stores. */
        vst1q_u8(pixel, zero);
        vst1q_u8(pixel + 16U, zero);
        vst1q_u8(pixel + 32U, zero);
        vst1q_u8(pixel + 48U, zero);
        return 1;
    }
#if ISAAC_VITA_PNG_PREMULTIPLY_BINARY_NEON
    if (vget_lane_u64(vreinterpret_u64_u8(vand_u8(low, high)), 0) ==
            UINT64_MAX)
        return 1;
    {
        const uint8x16_t opaque = vceqq_u8(rgba.val[3], vdupq_n_u8(255U));
        const uint8x16_t binary = vorrq_u8(opaque,
            vceqq_u8(rgba.val[3], vdupq_n_u8(0U)));
        const uint8x8_t all_binary = vand_u8(vget_low_u8(binary),
                                            vget_high_u8(binary));
        if (vget_lane_u64(vreinterpret_u64_u8(all_binary), 0) == UINT64_MAX) {
            uint8x16x4_t result;
            /* Every alpha is proved 0 or 255 before writing. Opaque RGB and
             * all alpha bytes retain their values. This exact full-block
             * write is permitted only in the existing private scratch owner;
             * partial-alpha blocks have performed no store on rejection. */
            result.val[0] = vandq_u8(rgba.val[0], opaque);
            result.val[1] = vandq_u8(rgba.val[1], opaque);
            result.val[2] = vandq_u8(rgba.val[2], opaque);
            result.val[3] = rgba.val[3];
            vst4q_u8(pixel, result);
            return 1;
        }
    }
    return 0;
#else
    return vget_lane_u64(vreinterpret_u64_u8(vand_u8(low, high)), 0) ==
           UINT64_MAX;
#endif
}
#else
#define ISAAC_PNG_PREMULTIPLY_NEON_AVAILABLE 0
#if ISAAC_VITA_PNG_PREMULTIPLY_NEON && defined(__vita__)
#error PNG premultiply NEON requires the target ARM NEON contract
#endif
#endif

#endif
