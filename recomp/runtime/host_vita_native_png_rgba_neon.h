#ifndef HOST_VITA_NATIVE_PNG_RGBA_NEON_H
#define HOST_VITA_NATIVE_PNG_RGBA_NEON_H

#include <stdint.h>

#ifndef ISAAC_VITA_NATIVE_PNG_RGBA_NEON
#define ISAAC_VITA_NATIVE_PNG_RGBA_NEON 0
#endif

#if ISAAC_VITA_NATIVE_PNG_RGBA_NEON && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define ISAAC_NP_RGBA_NEON_AVAILABLE 1

/* Four independent channel recurrences, in four-pixel blocks. Unlike libpng
 * 1.6's padded-row kernels, each load/store here stays inside n logical bytes;
 * the caller handles the incomplete block with its original scalar equations.
 * Only the low four lanes of a predictor are used. vzip joins those lanes
 * without aligned uint32_t pointer casts. No row padding/alignment is assumed.
 *
 * This uses the PNG equations and libpng's lane-wise NEON Paeth approach;
 * the bounded load/store schedule is project code, not the padded kernels.
 * Sources and licence: THIRD_PARTY.md (libpng section). */
static inline uint8x8_t np_rgba_paeth(uint8x8_t left, uint8x8_t above,
                                     uint8x8_t upper_left)
{
    uint16x8_t distance_left = vabdl_u8(above, upper_left);
    uint16x8_t distance_above = vabdl_u8(left, upper_left);
    uint16x8_t distance_corner = vabdq_u16(vaddl_u8(left, above),
                                          vaddl_u8(upper_left, upper_left));
    uint16x8_t choose_left = vandq_u16(
        vcleq_u16(distance_left, distance_above),
        vcleq_u16(distance_left, distance_corner));
    uint8x8_t other = vbsl_u8(
        vmovn_u16(vcleq_u16(distance_above, distance_corner)),
        above, upper_left);
    return vbsl_u8(vmovn_u16(choose_left), left, other);
}

static inline uint8x8_t np_rgba_join_pixels(uint8x8_t first, uint8x8_t second)
{
    return vreinterpret_u8_u32(vzip_u32(vreinterpret_u32_u8(first),
                                       vreinterpret_u32_u8(second)).val[0]);
}

#define NP_RGBA_SUB(a, b, c) (a)
#define NP_RGBA_AVG(a, b, c) vhadd_u8((a), (b))
#define NP_RGBA_PAETH(a, b, c) np_rgba_paeth((a), (b), (c))

#define NP_RGBA_KERNEL(name, predict)                                      \
static inline uint32_t name(uint8_t *row, const uint8_t *prev, uint32_t n)   \
{                                                                         \
    uint32_t i = 0U;                                                      \
    uint8x8_t left = vdup_n_u8(0);                                        \
    uint8x8_t corner = vdup_n_u8(0);                                      \
    for (; n - i >= 16U; i += 16U) {                                     \
        uint8x16_t packed = vld1q_u8(row + i);                            \
        uint8x16_t upper = prev ? vld1q_u8(prev + i) : vdupq_n_u8(0);     \
        uint8x8_t r0 = vget_low_u8(packed);                               \
        uint8x8_t r2 = vget_high_u8(packed);                              \
        uint8x8_t r1 = vext_u8(r0, r2, 4);                               \
        uint8x8_t r3 = vext_u8(r2, r2, 4);                               \
        uint8x8_t u0 = vget_low_u8(upper);                                \
        uint8x8_t u2 = vget_high_u8(upper);                               \
        uint8x8_t u1 = vext_u8(u0, u2, 4);                               \
        uint8x8_t u3 = vext_u8(u2, u2, 4);                               \
        uint8x8_t d0 = vadd_u8(r0, predict(left, u0, corner));             \
        uint8x8_t d1 = vadd_u8(r1, predict(d0, u1, u0));                   \
        uint8x8_t d2 = vadd_u8(r2, predict(d1, u2, u1));                   \
        uint8x8_t d3 = vadd_u8(r3, predict(d2, u3, u2));                   \
        vst1_u8(row + i, np_rgba_join_pixels(d0, d1));                    \
        vst1_u8(row + i + 8U, np_rgba_join_pixels(d2, d3));               \
        left = d3;                                                       \
        corner = u3;                                                     \
        (void)u1;                                                        \
    }                                                                     \
    (void)corner;                                                        \
    return i;                                                            \
}

NP_RGBA_KERNEL(np_rgba_sub_blocks, NP_RGBA_SUB)
NP_RGBA_KERNEL(np_rgba_avg_blocks, NP_RGBA_AVG)
NP_RGBA_KERNEL(np_rgba_paeth_blocks, NP_RGBA_PAETH)
#undef NP_RGBA_KERNEL
#undef NP_RGBA_SUB
#undef NP_RGBA_AVG
#undef NP_RGBA_PAETH

#else
#define ISAAC_NP_RGBA_NEON_AVAILABLE 0
#if ISAAC_VITA_NATIVE_PNG_RGBA_NEON && defined(__vita__)
#error "Native PNG RGBA NEON requires the target ARM NEON contract"
#endif
#endif

#endif
