/* Native camera ring average (ISAAC_VITA_CAMERA_RING_NATIVE, default OFF).
 *
 * sub_0049df90 (`ret 8`; [esp+4] Vector2 *out, [esp+8] ring, xmm3 a float
 * fraction the caller loads) is Camera::UpdateUltrasmooth's weighted mean of
 * a Vector2 history ring {data +0, cap +4, count +8, start +0xc}: with
 * n = count, k = (int)(n * frac) and a = (i <= k ? LOW : n + ONE),
 *   w_i = (i - a) / (k - a),  out = ONE / sum(w_i) * sum(w_i * data[(start + i) % cap])
 * accumulated from the reset vector.  12 calls and ~1,178 loop steps per game
 * tick on the PC build of this image (hcount.dll, 2026-10-12); every step of
 * the translated body pays an int64 __aeabi_ldivmod for the ring index.
 *
 * gen_all.py emits the call to isaac_vita_camera_ring_try at the root of the
 * authenticated body (all seven callers are in Camera::UpdateUltrasmooth, in
 * the same generated unit, so --wrap cannot see them).  The replay reproduces
 * what the translated body (guest_0142.c, generated-int3-off) leaves: the
 * guest stack words, the out vector, EAX/ECX/EDX/ESP and xmm0-xmm5 lane for
 * lane, with the same single-precision operations.  It declines, before any
 * write, into the translated body when
 *   - the guest stack cannot hold the frame [e-0x1c, e+0xc);
 *   - out overlaps the frame, the ring header or a constant the body reads,
 *     or the ring header overlaps the frame, or (n > 0) the data range
 *     [data, data + 8 * cap) overlaps out or the frame: then a body write
 *     could reach a value the replay holds in a register;
 *   - (n > 0) cap <= 0 (the body's idiv faults or divides by a negative),
 *     start < 0 or start + n - 1 overflows, or the data range wraps;
 *   - n * frac is NaN or outside int32 (cvttss2si is undefined there in C);
 *   - a step would reach the InverseLerp logger (0x49e00c/0x49e03b: an
 *     ordered k - a == 0 compare);
 *   - an input float (frac, the four constants, an element read) is a NaN:
 *     GCC reorders commutative float operands freely (the translated vmul at
 *     +0x1c6 is w * y), so only a default NaN has an order-free payload.
 * Nothing the body writes is read back once these hold, so the loop runs in
 * registers and every store happens after the last decline check.
 *
 * Counters (ph120.cr, host_vita_camera_ring_native.h): entries, replays,
 * replayed loop steps and declines by reason.  The seam sits at the root, so
 * the guest sampler books the replay's time on sub_0049df90 either way. */
#include "guest.h"
#include "host_vita_camera_ring_native.h"

#if !defined(GUEST_FLAGS_LOCAL) || !GUEST_FLAGS_LOCAL
#error The camera ring replay leaves c->fl untouched, which equals the translated body only in the GUEST_FLAGS_LOCAL=1 corpus
#endif

int isaac_vita_camera_ring_try(CPU *__restrict c);

#define CAM_VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (uint32_t)(rva)))
/* 0049df9c movss xmm0, [0x98802fec]; 0049dfb8 movss xmm0, [0x98802ff0] */
#define CAM_RESET_X 0x00802FECU
#define CAM_RESET_Y 0x00802FF0U
/* 0049dfd0 movss xmm3, [0x9876a614] (1.0f in the PE) */
#define CAM_ONE 0x0076A614U
/* 0049dff4 movss xmm2, [0x9876ae44] (-1.0f in the PE) */
#define CAM_LOW 0x0076AE44U
/* 0049e072 [ecx], 0049e06c [ecx+4], 0049dfa6 [ecx+8], 0049e05d [ecx+0xc] */
#define CAM_RING_DATA 0x0U
#define CAM_RING_CAP 0x4U
#define CAM_RING_COUNT 0x8U
#define CAM_RING_START 0xCU
#define CAM_RING_BYTES 0x10U
/* Below entry ESP e: push ebp e-4, [ebp-4] e-8, [ebp-8] e-0xc, push ebx,
 * esi, edi e-0x14..e-0x1c.  Above: return word, out (then the weight sum)
 * at e+4, ring at e+8; `ret 8` leaves ESP at e+0xc. */
#define CAM_FRAME_BELOW 0x1CU
#define CAM_FRAME_ABOVE 0xCU

static IsaacVitaCameraRingCounters s_counters;

void isaac_vita_camera_ring_take(IsaacVitaCameraRingCounters *out)
{
    if (out)
        *out = s_counters;
    memset(&s_counters, 0, sizeof s_counters);
}

static int cam_decline(int reason)
{
    ++s_counters.declined[reason];
    return 0;
}

static int cam_overlap(uint32_t a, uint64_t a_bytes, uint32_t b, uint64_t b_bytes)
{
    return (uint64_t)a < (uint64_t)b + b_bytes &&
           (uint64_t)b < (uint64_t)a + a_bytes;
}

static int cam_nan_bits(uint32_t bits)
{
    return (bits & 0x7FFFFFFFU) > 0x7F800000U;
}

static float cam_float(uint32_t bits)
{
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

int isaac_vita_camera_ring_try(CPU *__restrict c)
{
    const uint32_t e = c->esp;
    const uint32_t frame = e - CAM_FRAME_BELOW;
    const uint64_t frame_bytes = CAM_FRAME_BELOW + CAM_FRAME_ABOVE;
    uint32_t capacity, offset, out, ring, bad;
    uint32_t x0_bits, y0_bits, one_bits, low_bits, frac_bits;
    int32_t count, k;
    float frac, prod, x0, y0, one, low;
    float fb = 0.0f, hi = 0.0f, sum = 0.0f, w = 0.0f, ox, oy, r;
    uint32_t data = 0U, rem = 0U;

    ++s_counters.calls;
    if (!guest_stack_fast_bound(c))
        return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_FRAME);
    capacity = c->stack_ceiling - c->stack_floor;
    offset = e - c->stack_floor;
    if (capacity < (uint32_t)frame_bytes || offset < CAM_FRAME_BELOW ||
            offset > capacity - CAM_FRAME_ABOVE)
        return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_FRAME);
    out = ld32(e + 4U);                                 /* 0049dfac */
    ring = ld32(e + 8U);                                /* 0049df96 */
    if (out == 0U || ring == 0U || out > 0xFFFFFFF8U || ring > 0xFFFFFFF0U ||
            cam_overlap(out, 8U, frame, frame_bytes) ||
            cam_overlap(ring, CAM_RING_BYTES, frame, frame_bytes) ||
            cam_overlap(out, 8U, ring, CAM_RING_BYTES) ||
            cam_overlap(out, 8U, CAM_VA(CAM_RESET_X), 8U) ||
            cam_overlap(out, 8U, CAM_VA(CAM_ONE), 4U) ||
            cam_overlap(out, 8U, CAM_VA(CAM_LOW), 4U))
        return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_POINTER);
    x0_bits = ld32(CAM_VA(CAM_RESET_X));
    y0_bits = ld32(CAM_VA(CAM_RESET_Y));
    one_bits = ld32(CAM_VA(CAM_ONE));
    low_bits = ld32(CAM_VA(CAM_LOW));
    frac = c->x[3].f[0];
    memcpy(&frac_bits, &frac, 4);
    if (cam_nan_bits(x0_bits) || cam_nan_bits(y0_bits) ||
            cam_nan_bits(one_bits) || cam_nan_bits(low_bits) ||
            cam_nan_bits(frac_bits))
        return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_NAN);
    x0 = cam_float(x0_bits);
    y0 = cam_float(y0_bits);
    one = cam_float(one_bits);
    low = cam_float(low_bits);
    count = (int32_t)ld32(ring + CAM_RING_COUNT);       /* 0049dfa6 */
    prod = (float)((float)count * frac);                /* 0049dfc5..cc */
    if (!(prod >= -2147483648.0f && prod < 2147483648.0f))
        return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_TRUNC);
    k = (int32_t)prod;                                  /* 0049dfd8 */
    ox = x0;
    oy = y0;
    if (count > 0) {
        int32_t cap = (int32_t)ld32(ring + CAM_RING_CAP);
        int32_t start = (int32_t)ld32(ring + CAM_RING_START);
        int32_t i;
        data = ld32(ring + CAM_RING_DATA);
        if (cap <= 0 || start < 0 || start > INT32_MAX - (count - 1))
            return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_RING);
        if ((uint64_t)data + 8U * (uint64_t)(uint32_t)cap > 0x100000000ULL ||
                cam_overlap(out, 8U, data, 8U * (uint64_t)(uint32_t)cap) ||
                cam_overlap(frame, frame_bytes, data,
                            8U * (uint64_t)(uint32_t)cap))
            return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_POINTER);
        fb = (float)k;                                  /* 0049dfe4..eb */
        hi = (float)((float)count + one);               /* 0049e01b..22 */
        /* 0049dffc/0049e026 ucomiss, compared as the listing does (double) */
        if ((k >= 0 && (double)low == (double)fb) ||
                (k < count - 1 && (double)hi == (double)fb))
            return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_LOGGER);
        bad = 0U;
        for (i = 0; i < count; ++i) {                   /* 0049dff0..e0b8 */
            const float a = i <= k ? low : hi;
            const int32_t d = start + i;
            uint32_t at, xb, yb;
            float den, x, y;
            rem = (uint32_t)(d < cap ? d : d - cap < cap ? d - cap : d % cap);
            at = data + rem * 8U;
            xb = ld32(at);
            yb = ld32(at + 4U);
            bad |= (uint32_t)cam_nan_bits(xb) | (uint32_t)cam_nan_bits(yb);
            x = cam_float(xb);
            y = cam_float(yb);
            den = (float)(fb - a);                      /* 0049e060 */
            w = (float)((float)i - a);                  /* 0049e07c */
            w = (float)(w / den);                       /* 0049e087 */
            x = (float)(x * w);                         /* 0049e08b */
            y = (float)(y * w);                         /* 0049e092 */
            sum = (float)(sum + w);                     /* 0049e096 */
            ox = (float)(x + ox);                       /* 0049e09f */
            oy = (float)(y + oy);                       /* 0049e0a3 */
        }
        if (bad)
            return cam_decline(ISAAC_VITA_CAMERA_RING_DECLINE_NAN);
        s_counters.steps += (uint32_t)count;
    }
    r = (float)(one / sum);                             /* 0049e0c9 */

    st32(e - 0x04U, c->ebp);                            /* 0049df90 */
    st32(e - 0x14U, c->ebx);                            /* 0049dfa4 */
    st32(e - 0x18U, c->esi);                            /* 0049dfa5 */
    st32(e - 0x1CU, c->edi);                            /* 0049dfab */
    stf(e + 4U, sum);                                   /* 0049dfaf, e0a8 */
    if (count > 0) {
        stf(e - 0x08U, fb);                             /* 0049dfeb */
        if (k < count - 1)
            stf(e - 0x0CU, hi);                         /* 0049e029 */
    }
    stf(out + 4U, (float)(r * oy));                     /* 0049e0d2, da */
    stf(out, (float)(r * ox));                          /* 0049e0d6, df */

    c->x[0] = (xmm_t){0};                               /* 0049e0cf */
    c->x[0].f[0] = (float)(r * ox);
    if (count > 0) {
        c->x[1] = (xmm_t){0};                           /* 0049e066 */
        c->x[1].f[0] = w;
        if (k < count - 1)
            c->x[2] = (xmm_t){0};                       /* 0049e01b */
        c->edx = rem;                                   /* 0049e06c */
    }
    c->x[2].f[0] = ox;
    c->x[3] = (xmm_t){0};                               /* 0049dfd0 */
    c->x[3].f[0] = (float)(r * oy);
    c->x[4].f[0] = oy;
    c->x[5] = (xmm_t){0};                               /* 0049df99 */
    c->x[5].f[0] = sum;
    c->eax = out;                                       /* 0049e0cd */
    c->ecx = ring;
    c->esp = e + 12U;                                   /* 0049e0e9 */
    ++s_counters.handled;
    return 1;
}
