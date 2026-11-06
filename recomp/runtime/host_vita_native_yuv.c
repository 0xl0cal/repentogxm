/* ISAAC_VITA_NATIVE_THEORA_RGB: theoraplayer's YUV->RGB conversion of a decoded
 * video frame runs natively (NEON) instead of through the translated libyuv.
 *
 * Why.  With the native theora decoder (ISAAC_VITA_NATIVE_THEORA) a 960x544
 * packet costs about 17 ms, but TheoraVideoFrame::decode still spent 86 ms per
 * frame (nt2 device run, 2026-09-10: rgb_us mean=85876 over 59 frames) in the
 * translated libyuv I420ToRGB24 path, which is the single largest item of a
 * decode slice.  The same integer math in NEON is a few milliseconds.
 *
 * How.  Two theoraplayer conversion functions (005c2ad0 and 005c2c30, the
 * RGB and BGR entries of its conversion table) are the only callers of
 * libyuv's I420ToRGB24Matrix (005ce3f0): two direct calls at 005c2afd and
 * 005c2c5d, no address-taken reference (pinned by
 * test_native_theora_sites.py), so GNU ld --wrap hands both to the wrapper
 * below.  MSVC passes the first two arguments in ecx/edx and the other nine on
 * the stack, caller-cleaned:
 *   int I420ToRGB24Matrix(const uint8 *src_y /ecx/, int src_stride_y /edx/,
 *                         const uint8 *src_u, int src_stride_u,
 *                         const uint8 *src_v, int src_stride_v,
 *                         uint8 *dst_rgb24, int dst_stride_rgb24,
 *                         const struct YuvConstants *yuvconstants,
 *                         int width, int height);
 * It answers -1 for a NULL plane, width <= 0 or height == 0, flips the output
 * for a negative height, and advances the chroma rows every second luma row.
 * The per-pixel math is libyuv's C reference YuvPixel, reproduced bit for bit
 * (the translated build's C row function at 005d5270 does exactly this):
 *   y1 = (uint32)(y * 0x0101 * YG) >> 16
 *   b  = clamp((y1 - u * UB + BB) >> 6)
 *   g  = clamp((y1 - u * UG - v * VG + BG) >> 6)
 *   r  = clamp((y1 - v * VR + BR) >> 6)
 * with the coefficients read from the constants table the caller passes, at
 * the offsets that row function reads them from (int8 UB +0x00, UG +0x20,
 * VG +0x21, VR +0x41; int16 BB +0x60, BG +0x80, BR +0xa0, YG +0xc0).  The
 * RGB entry hands the YVU I601 table (rva 0x73d260: UB=-102 UG=52 VG=25
 * VR=-128, biases -14216/8696/-17544) with the u and v planes swapped, the
 * BGR entry the YUV table (rva 0x73d340) with the planes straight; the wrapper
 * reproduces whatever it is given and interprets nothing.  The arithmetic
 * shift, the clamp and the odd-width tail follow the C code.  libyuv picks
 * its SSSE3/AVX2 rows from cpuid leaf 1, and the Vita's cpuid seam
 * (host_vita_first_fault.c guest_cpuid) answers ecx = 0 for that leaf, so the
 * translated build always ran the C row: the native frame is bit-identical
 * to the translated one (pinned by test_native_theora_sites.py).
 *
 * Speed.  The row runs in int16 NEON lanes, sixteen pixels per step, when the
 * loaded constants pass vita_ny_consts_fit_s16 (both frozen tables do; the
 * exhaustive proof is recomp/test_native_yuv_s16.py); a table that fails the
 * guard takes the 32-bit-lane row.  The nt3 device run (32-bit lanes, eight
 * pixels per step, a counter in the loop) converted a 960x544 frame in
 * 18.6 ms; the int16 row halves the lane work and drops the loop counter.
 *
 * Guest address == host address: the planes are read and the frame written in
 * place.  Compiled only with the knob ON; the OFF build has no object, no link
 * option and byte-identical units. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>

#include "guest.h"
#include "kage_vita_backend.h"

#if !defined(ISAAC_VITA_NATIVE_THEORA_RGB)
#error "host_vita_native_yuv.c is compiled only with ISAAC_VITA_NATIVE_THEORA_RGB"
#endif

/* recomp/vita/platform.h; declared here so a host oracle can stub it. */
void isaac_vita_log(const char *format, ...);

/* Frozen PE facts, pinned by recomp/test_native_theora_sites.py. */
#define VITA_NY_I420_TO_RGB24_MATRIX_RVA 0x005ce3f0U

/* Offsets the C row function reads from the YuvConstants table. */
#define VITA_NY_CONST_UB 0x00U
#define VITA_NY_CONST_UG 0x20U
#define VITA_NY_CONST_VG 0x21U
#define VITA_NY_CONST_VR 0x41U
#define VITA_NY_CONST_BB 0x60U
#define VITA_NY_CONST_BG 0x80U
#define VITA_NY_CONST_BR 0xa0U
#define VITA_NY_CONST_YG 0xc0U

typedef struct vita_ny_consts {
    int32_t ub, ug, vg, vr, bb, bg, br;
    uint32_t yg;
} vita_ny_consts;

static struct {
    uint32_t frames, invalid, flipped, s16_rows, neon_rows, tail_pixels;
    uint32_t us_last, us_max;
    uint64_t us_sum;
    uint32_t width, height;
    int32_t stride_y, stride_u, stride_v, stride_dst;
    uint32_t consts_table;
    vita_ny_consts consts;
} s_ny;

static void vita_ny_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
}

static uint64_t vita_ny_now_us(void)
{
    double seconds = 0.0;

    if (!kage_vita_backend_time_seconds(&seconds) || seconds < 0.0)
        return 0U;
    return (uint64_t)(seconds * 1000000.0);
}

/* cdecl stack argument `index` (0-based) of the wrapped call: [esp + 4 + 4*index]
 * with the return word still on the stack. */
static uint32_t vita_ny_arg(CPU *__restrict c, uint32_t index)
{
    uint32_t address = guest_stack_address(
        c, c->esp + 4U + 4U * index, 4U, VITA_NY_I420_TO_RGB24_MATRIX_RVA);

    if (!address) {
        if (!c->fault)
            guest_fault(c, GUEST_IMAGE_BASE + VITA_NY_I420_TO_RGB24_MATRIX_RVA,
                        "native yuv: argument off the guest stack");
        return 0U;
    }
    return ld32(address);
}

static int16_t vita_ny_ld16(uint32_t address)
{
    return (int16_t)(uint16_t)(ld8(address) | ((uint32_t)ld8(address + 1U) << 8));
}

static void vita_ny_load_consts(uint32_t table, vita_ny_consts *k)
{
    k->ub = (int8_t)ld8(table + VITA_NY_CONST_UB);
    k->ug = (int8_t)ld8(table + VITA_NY_CONST_UG);
    k->vg = (int8_t)ld8(table + VITA_NY_CONST_VG);
    k->vr = (int8_t)ld8(table + VITA_NY_CONST_VR);
    k->bb = vita_ny_ld16(table + VITA_NY_CONST_BB);
    k->bg = vita_ny_ld16(table + VITA_NY_CONST_BG);
    k->br = vita_ny_ld16(table + VITA_NY_CONST_BR);
    k->yg = (uint32_t)(uint16_t)vita_ny_ld16(table + VITA_NY_CONST_YG);
}

static uint8_t vita_ny_clamp(int32_t v)
{
    return v < 0 ? 0U : (v > 255 ? 255U : (uint8_t)v);
}

/* libyuv YuvPixel, C reference. */
static void vita_ny_pixel(uint32_t y, uint32_t u, uint32_t v, uint8_t *dst,
                          const vita_ny_consts *k)
{
    int32_t y1 = (int32_t)((y * 0x0101U * k->yg) >> 16);

    dst[0] = vita_ny_clamp((y1 - (int32_t)u * k->ub + k->bb) >> 6);
    dst[1] = vita_ny_clamp((y1 - (int32_t)u * k->ug - (int32_t)v * k->vg + k->bg) >> 6);
    dst[2] = vita_ny_clamp((y1 - (int32_t)v * k->vr + k->br) >> 6);
}

/* Fast row precondition.  vita_ny_row16 keeps every intermediate of the C
 * pixel in int16 lanes: y1 = ((y * 0x0101) * yg) >> 16 is a u16 x u16 -> u32
 * product narrowed by 16 (exact while yg <= 32767), the biased sums y1 + bias
 * must fit, the chroma products u * ub etc. fit for any int8 coefficient
 * (255 * 128 = 32640), and g's first step (y1 + bg) - u * ug is a plain
 * multiply-subtract that must not wrap.  The final steps are saturating
 * subtracts: a saturated lane is >= 32767 or <= -32768, and (32767 >> 6) = 511
 * clamps to 255, (-32768 >> 6) = -512 clamps to 0, exactly as the unsaturated
 * 32-bit value would, so they need no condition.  Both frozen tables pass
 * (exhaustive proof over 256^3 inputs: recomp/test_native_yuv_s16.py); any
 * other table takes the 32-bit row. */
static int vita_ny_consts_fit_s16(const vita_ny_consts *k)
{
    int32_t y1max, lo, hi;

    if (k->yg > 32767U)
        return 0;
    y1max = (int32_t)((65535U * k->yg) >> 16);
    if (y1max + k->bb > 32767 || y1max + k->bg > 32767 || y1max + k->br > 32767)
        return 0;
    lo = k->bg - (k->ug > 0 ? 255 * k->ug : 0);
    hi = y1max + k->bg - (k->ug < 0 ? 255 * k->ug : 0);
    if (lo < -32768 || hi > 32767)
        return 0;
    return 1;
}

/* The constants the row keeps in registers: the four int8 coefficients as the
 * lanes of one int16x4 (the by-scalar multiply forms read a lane), yg and the
 * 0x0101 multiplier in one uint16x4, the three biases as full vectors.  Five
 * registers instead of eight keeps the sixteen-pixel step free of spills. */
typedef struct vita_ny_s16 {
    int16x4_t coef;     /* lanes: ub, ug, vg, vr */
    uint16x4_t ymul;    /* lanes: yg, 256, 0, 0 */
    int16x8_t bb, bg, br;
} vita_ny_s16;

static void vita_ny_s16_init(vita_ny_s16 *s, const vita_ny_consts *k)
{
    int16_t coef[4] = { (int16_t)k->ub, (int16_t)k->ug, (int16_t)k->vg, (int16_t)k->vr };
    uint16_t ymul[4] = { (uint16_t)k->yg, 256U, 0U, 0U };

    s->coef = vld1_s16(coef);
    s->ymul = vld1_u16(ymul);
    s->bb = vdupq_n_s16((int16_t)k->bb);
    s->bg = vdupq_n_s16((int16_t)k->bg);
    s->br = vdupq_n_s16((int16_t)k->br);
}

/* Eight pixels, int16 lanes; uz/vz hold the pair-duplicated chroma bytes. */
static inline void vita_ny_block8(uint8x8_t y8, uint8x8_t uz, uint8x8_t vz,
                                  uint8_t *dst, const vita_ny_s16 *k)
{
    uint16x8_t y16 = vmovl_u8(y8);
    uint16x8_t y257 = vmlaq_lane_u16(y16, y16, k->ymul, 1);   /* y * 0x0101 */
    uint16x4_t y1lo = vshrn_n_u32(vmull_lane_u16(vget_low_u16(y257), k->ymul, 0), 16);
    uint16x4_t y1hi = vshrn_n_u32(vmull_lane_u16(vget_high_u16(y257), k->ymul, 0), 16);
    int16x8_t y1 = vreinterpretq_s16_u16(vcombine_u16(y1lo, y1hi));
    int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(uz));
    int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(vz));
    int16x8_t b = vqsubq_s16(vaddq_s16(y1, k->bb), vmulq_lane_s16(u, k->coef, 0));
    int16x8_t g = vqsubq_s16(vmlsq_lane_s16(vaddq_s16(y1, k->bg), u, k->coef, 1),
                             vmulq_lane_s16(v, k->coef, 2));
    int16x8_t r = vqsubq_s16(vaddq_s16(y1, k->br), vmulq_lane_s16(v, k->coef, 3));
    uint8x8x3_t out;

    out.val[0] = vqmovun_s16(vshrq_n_s16(b, 6));
    out.val[1] = vqmovun_s16(vshrq_n_s16(g, 6));
    out.val[2] = vqmovun_s16(vshrq_n_s16(r, 6));
    vst3_u8(dst, out);
}

/* One row, sixteen pixels per step (eight chroma bytes per plane, no read past
 * the row), then the C pixel for the tail; the tail covers an odd width the
 * way I420ToRGB24Row_C does: the last pixel uses the chroma of its pair. */
static void vita_ny_row16(const uint8_t *sy, const uint8_t *su, const uint8_t *sv,
                          uint8_t *dst, int width, const vita_ny_s16 *s,
                          const vita_ny_consts *k)
{
    int x = 0;

    for (; x + 16 <= width; x += 16) {
        uint8x16_t y16 = vld1q_u8(sy + x);
        uint8x8_t u8 = vld1_u8(su + (x >> 1));
        uint8x8_t v8 = vld1_u8(sv + (x >> 1));
        uint8x8x2_t uz = vzip_u8(u8, u8);   /* u0 u0 u1 u1 ... | u4 u4 ... u7 u7 */
        uint8x8x2_t vz = vzip_u8(v8, v8);

        vita_ny_block8(vget_low_u8(y16), uz.val[0], vz.val[0], dst + 3 * x, s);
        vita_ny_block8(vget_high_u8(y16), uz.val[1], vz.val[1], dst + 3 * x + 24, s);
    }
    for (; x < width; ++x)
        vita_ny_pixel(sy[x], su[x >> 1], sv[x >> 1], dst + 3 * x, k);
}

/* Fallback row for a constants table that fails vita_ny_consts_fit_s16: eight
 * pixels per NEON step in 32-bit lanes, then the C pixel for the tail. */
static void vita_ny_row32(const uint8_t *sy, const uint8_t *su, const uint8_t *sv,
                          uint8_t *dst, int width, const vita_ny_consts *k)
{
    const int16x4_t ub4 = vdup_n_s16((int16_t)k->ub);
    const int16x4_t ug4 = vdup_n_s16((int16_t)k->ug);
    const int16x4_t vg4 = vdup_n_s16((int16_t)k->vg);
    const int16x4_t vr4 = vdup_n_s16((int16_t)k->vr);
    const int32x4_t bb4 = vdupq_n_s32(k->bb);
    const int32x4_t bg4 = vdupq_n_s32(k->bg);
    const int32x4_t br4 = vdupq_n_s32(k->br);
    const uint32x4_t yg4 = vdupq_n_u32(k->yg);
    int x = 0;

    for (; x + 8 <= width; x += 8) {
        uint8x8_t y8 = vld1_u8(sy + x);
        uint32_t uw, vw;
        uint8x8_t u8, v8;
        uint8x8x2_t uz, vz;
        uint16x8_t y16, u16, v16;
        uint32x4_t ylo, yhi;
        int32x4_t y1lo, y1hi, blo, bhi, glo, ghi, rlo, rhi;
        int16x4_t ulo, uhi, vlo, vhi;
        uint8x8x3_t out;

        memcpy(&uw, su + (x >> 1), 4U);
        memcpy(&vw, sv + (x >> 1), 4U);
        u8 = vreinterpret_u8_u32(vdup_n_u32(uw));
        v8 = vreinterpret_u8_u32(vdup_n_u32(vw));
        uz = vzip_u8(u8, u8);   /* u0 u0 u1 u1 u2 u2 u3 u3 */
        vz = vzip_u8(v8, v8);
        y16 = vmovl_u8(y8);
        u16 = vmovl_u8(uz.val[0]);
        v16 = vmovl_u8(vz.val[0]);
        ylo = vmull_n_u16(vget_low_u16(y16), 0x0101U);
        yhi = vmull_n_u16(vget_high_u16(y16), 0x0101U);
        ylo = vshrq_n_u32(vmulq_u32(ylo, yg4), 16);
        yhi = vshrq_n_u32(vmulq_u32(yhi, yg4), 16);
        y1lo = vreinterpretq_s32_u32(ylo);
        y1hi = vreinterpretq_s32_u32(yhi);
        ulo = vreinterpret_s16_u16(vget_low_u16(u16));
        uhi = vreinterpret_s16_u16(vget_high_u16(u16));
        vlo = vreinterpret_s16_u16(vget_low_u16(v16));
        vhi = vreinterpret_s16_u16(vget_high_u16(v16));

        blo = vmlsl_s16(vaddq_s32(y1lo, bb4), ulo, ub4);
        bhi = vmlsl_s16(vaddq_s32(y1hi, bb4), uhi, ub4);
        glo = vmlsl_s16(vmlsl_s16(vaddq_s32(y1lo, bg4), ulo, ug4), vlo, vg4);
        ghi = vmlsl_s16(vmlsl_s16(vaddq_s32(y1hi, bg4), uhi, ug4), vhi, vg4);
        rlo = vmlsl_s16(vaddq_s32(y1lo, br4), vlo, vr4);
        rhi = vmlsl_s16(vaddq_s32(y1hi, br4), vhi, vr4);

        out.val[0] = vqmovn_u16(vcombine_u16(vqmovun_s32(vshrq_n_s32(blo, 6)),
                                             vqmovun_s32(vshrq_n_s32(bhi, 6))));
        out.val[1] = vqmovn_u16(vcombine_u16(vqmovun_s32(vshrq_n_s32(glo, 6)),
                                             vqmovun_s32(vshrq_n_s32(ghi, 6))));
        out.val[2] = vqmovn_u16(vcombine_u16(vqmovun_s32(vshrq_n_s32(rlo, 6)),
                                             vqmovun_s32(vshrq_n_s32(rhi, 6))));
        vst3_u8(dst + 3 * x, out);
    }
    for (; x < width; ++x)
        vita_ny_pixel(sy[x], su[x >> 1], sv[x >> 1], dst + 3 * x, k);
}

static void vita_ny_add(uint32_t *counter, uint32_t n)
{
    *counter = (UINT32_MAX - *counter < n) ? UINT32_MAX : *counter + n;
}

/* libyuv I420ToRGB24Matrix, plane loop. */
static int vita_ny_convert(uint32_t src_y, int32_t stride_y, uint32_t src_u,
                           int32_t stride_u, uint32_t src_v, int32_t stride_v,
                           uint32_t dst, int32_t stride_dst, uint32_t table,
                           int32_t width, int32_t height)
{
    const uint8_t *sy = (const uint8_t *)(uintptr_t)src_y;
    const uint8_t *su = (const uint8_t *)(uintptr_t)src_u;
    const uint8_t *sv = (const uint8_t *)(uintptr_t)src_v;
    uint8_t *d = (uint8_t *)(uintptr_t)dst;
    vita_ny_consts k;
    int32_t y;

    if (!src_y || !src_u || !src_v || !dst || width <= 0 || height == 0)
        return -1;
    if (height < 0) {
        height = -height;
        d = d + (intptr_t)(height - 1) * stride_dst;
        stride_dst = -stride_dst;
        vita_ny_count(&s_ny.flipped);
    }
    vita_ny_load_consts(table, &k);
    s_ny.consts = k;
    if (vita_ny_consts_fit_s16(&k)) {
        vita_ny_s16 s;

        vita_ny_s16_init(&s, &k);
        for (y = 0; y < height; ++y) {
            vita_ny_row16(sy, su, sv, d, width, &s, &k);
            d += stride_dst;
            sy += stride_y;
            if (y & 1) {
                su += stride_u;
                sv += stride_v;
            }
        }
        vita_ny_add(&s_ny.s16_rows, (uint32_t)height);
        vita_ny_add(&s_ny.tail_pixels, (uint32_t)(width & 15) * (uint32_t)height);
    }
    else {
        for (y = 0; y < height; ++y) {
            vita_ny_row32(sy, su, sv, d, width, &k);
            d += stride_dst;
            sy += stride_y;
            if (y & 1) {
                su += stride_u;
                sv += stride_v;
            }
        }
        vita_ny_add(&s_ny.neon_rows, (uint32_t)height);
        vita_ny_add(&s_ny.tail_pixels, (uint32_t)(width & 7) * (uint32_t)height);
    }
    return 0;
}

static void vita_ny_receipt(const char *why)
{
    isaac_vita_log(
        "KAGE VITA NATIVE YUV: %s frames=%u (invalid=%u flipped=%u) us last=%u "
        "mean=%u max=%u size=%ux%u stride(y,u,v,dst)=%d,%d,%d,%d table=0x%08x "
        "k(ub,ug,vg,vr,bb,bg,br,yg)=%d,%d,%d,%d,%d,%d,%d,%u s16_rows=%u neon_rows=%u "
        "tail_px=%u",
        why, (unsigned)s_ny.frames, (unsigned)s_ny.invalid,
        (unsigned)s_ny.flipped, (unsigned)s_ny.us_last,
        (unsigned)(s_ny.frames ? s_ny.us_sum / s_ny.frames : 0U),
        (unsigned)s_ny.us_max, (unsigned)s_ny.width, (unsigned)s_ny.height,
        (int)s_ny.stride_y, (int)s_ny.stride_u, (int)s_ny.stride_v,
        (int)s_ny.stride_dst, (unsigned)s_ny.consts_table,
        (int)s_ny.consts.ub, (int)s_ny.consts.ug, (int)s_ny.consts.vg,
        (int)s_ny.consts.vr, (int)s_ny.consts.bb, (int)s_ny.consts.bg,
        (int)s_ny.consts.br, (unsigned)s_ny.consts.yg, (unsigned)s_ny.s16_rows,
        (unsigned)s_ny.neon_rows, (unsigned)s_ny.tail_pixels);
}

/* int I420ToRGB24Matrix(src_y /ecx/, src_stride_y /edx/, src_u, src_stride_u,
 *                       src_v, src_stride_v, dst, dst_stride, yuvconstants,
 *                       width, height): caller-cleaned stack, result in eax. */
void __wrap_sub_005ce3f0(CPU *__restrict c)
{
    uint32_t src_y = c->ecx;
    int32_t stride_y = (int32_t)c->edx;
    uint32_t src_u = vita_ny_arg(c, 0U);
    int32_t stride_u = (int32_t)vita_ny_arg(c, 1U);
    uint32_t src_v = vita_ny_arg(c, 2U);
    int32_t stride_v = (int32_t)vita_ny_arg(c, 3U);
    uint32_t dst = vita_ny_arg(c, 4U);
    int32_t stride_dst = (int32_t)vita_ny_arg(c, 5U);
    uint32_t table = vita_ny_arg(c, 6U);
    int32_t width = (int32_t)vita_ny_arg(c, 7U);
    int32_t height = (int32_t)vita_ny_arg(c, 8U);
    uint64_t t0, t1;
    uint32_t dt;
    int result;

    if (c->fault)
        return;
    if (!table) {
        vita_ny_count(&s_ny.invalid);
        c->eax = (uint32_t)-1;
        (void)gpop(c);
        return;
    }
    t0 = vita_ny_now_us();
    result = vita_ny_convert(src_y, stride_y, src_u, stride_u, src_v, stride_v,
                             dst, stride_dst, table, width, height);
    t1 = vita_ny_now_us();
    dt = (t0 && t1 > t0) ? (uint32_t)(t1 - t0) : 0U;
    if (result == 0) {
        vita_ny_count(&s_ny.frames);
        s_ny.us_last = dt;
        if (dt > s_ny.us_max)
            s_ny.us_max = dt;
        s_ny.us_sum += dt;
        s_ny.width = (uint32_t)width;
        s_ny.height = (uint32_t)(height < 0 ? -height : height);
        s_ny.stride_y = stride_y;
        s_ny.stride_u = stride_u;
        s_ny.stride_v = stride_v;
        s_ny.stride_dst = stride_dst;
        s_ny.consts_table = table;
        if (s_ny.frames <= 4U || (s_ny.frames & 63U) == 0U)
            vita_ny_receipt("frame");
    }
    else {
        vita_ny_count(&s_ny.invalid);
        if (s_ny.invalid <= 4U)
            isaac_vita_log(
                "KAGE VITA NATIVE YUV: invalid arguments y=0x%08x u=0x%08x "
                "v=0x%08x dst=0x%08x size=%dx%d answers -1 like libyuv",
                (unsigned)src_y, (unsigned)src_u, (unsigned)src_v,
                (unsigned)dst, (int)width, (int)height);
    }
    c->eax = (uint32_t)result;
    (void)gpop(c);
}
