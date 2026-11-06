#ifndef ISAAC_RGB_SUBUPLOAD_H
#define ISAAC_RGB_SUBUPLOAD_H

#include <stddef.h>
#include <stdint.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define ISAAC_RGB_SUBUPLOAD_NEON 1U
#elif defined(__vita__)
#error "Isaac RGB subupload requires the target ARM NEON contract"
#else
#define ISAAC_RGB_SUBUPLOAD_NEON 0U
#endif

/* No format conversion in the mathematical sense: pinned vitaGL's
 * read_rgb888 -> write_rgba8888 copies R,G,B and supplies opaque alpha.
 * vld3/vst4 is the same interleaved-byte operation used by libyuv's NEON rows.
 * The scalar branch is for host comparison; a Vita build must use NEON. */
static void isaac_rgb_subupload_row(
    uint8_t *dst, const uint8_t *src, uint32_t width)
{
    uint32_t x = 0U;
#if ISAAC_RGB_SUBUPLOAD_NEON
    const uint8x16_t alpha = vdupq_n_u8(255U);
    for (; width - x >= 16U; x += 16U) {
        uint8x16x3_t rgb = vld3q_u8(src + (size_t)x * 3U);
        uint8x16x4_t rgba;
        rgba.val[0] = rgb.val[0];
        rgba.val[1] = rgb.val[1];
        rgba.val[2] = rgb.val[2];
        rgba.val[3] = alpha;
        vst4q_u8(dst + (size_t)x * 4U, rgba);
    }
#endif
    for (; x < width; ++x) {
        dst[(size_t)x * 4U] = src[(size_t)x * 3U];
        dst[(size_t)x * 4U + 1U] = src[(size_t)x * 3U + 1U];
        dst[(size_t)x * 4U + 2U] = src[(size_t)x * 3U + 2U];
        dst[(size_t)x * 4U + 3U] = 255U;
    }
}

static int isaac_rgb_subupload_extent(uintptr_t base, uint32_t stride,
    uint32_t row_bytes, uint32_t height, uintptr_t *end)
{
    uint64_t tail;
    if (!base || !height || !row_bytes || stride < row_bytes)
        return 0;
    tail = (uint64_t)(height - 1U) * stride + row_bytes;
    if (tail > UINTPTR_MAX || base > UINTPTR_MAX - (uintptr_t)tail)
        return 0;
    *end = base + (uintptr_t)tail;
    return 1;
}

/* Refuse aliases/overflow before writing anything. The original callback loop
 * remains the fallback, including unusual overlapping/short-stride inputs.
 * Padding in either image is neither read nor written by the NEON rows. */
static int isaac_rgb_subupload_rows(uint8_t *dst, uint32_t dst_stride,
    const uint8_t *src, uint32_t src_row_length, int32_t width, int32_t height)
{
    uint32_t src_stride, y;
    uintptr_t src_end, dst_end;
    if (width <= 0 || height <= 0 || (uint32_t)width > UINT32_MAX / 4U)
        return 0;
    if (!src_row_length)
        src_row_length = (uint32_t)width;
    if (src_row_length > UINT32_MAX / 3U)
        return 0;
    src_stride = src_row_length * 3U;
    if (!isaac_rgb_subupload_extent((uintptr_t)src, src_stride,
            (uint32_t)width * 3U, (uint32_t)height, &src_end) ||
        !isaac_rgb_subupload_extent((uintptr_t)dst, dst_stride,
            (uint32_t)width * 4U, (uint32_t)height, &dst_end) ||
        !((uintptr_t)dst >= src_end || (uintptr_t)src >= dst_end))
        return 0;
    for (y = 0U; y < (uint32_t)height; ++y)
        isaac_rgb_subupload_row(dst + (size_t)y * dst_stride,
            src + (size_t)y * src_stride, (uint32_t)width);
    return 1;
}

/* compatible authenticates native target/type/status/format at the call site.
 * The old COW allocation is still retired exactly as before. Only its copy to
 * the NEW allocation is elided, when the following valid upload overwrites
 * every byte: full level zero, no additional mips, no row-alignment padding.
 * Invalid formats/NULL pixels must keep the old copy even if GL rejects later. */
static int isaac_rgb_subupload_full_replace(uint32_t compatible,
    int32_t level, int32_t x, int32_t y, int32_t width, int32_t height,
    uint32_t orig_w, uint32_t orig_h, uint32_t mip_count,
    uint32_t unpack_row_length, const void *pixels)
{
    if (!compatible || !pixels || level != 0 || x != 0 || y != 0 ||
            width <= 0 || height <= 0 || mip_count > 1U ||
            (uint32_t)width != orig_w || (uint32_t)height != orig_h ||
            (orig_w & 7U) || orig_w > UINT32_MAX / 4U ||
            (uint64_t)orig_h * (orig_w * 4U) > UINT32_MAX)
        return 0;
    return unpack_row_length == 0U ||
        (unpack_row_length >= orig_w && unpack_row_length <= UINT32_MAX / 3U);
}

#endif
