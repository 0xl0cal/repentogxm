#ifndef ISAAC_HOST_VITA_TGT_RECIPROCAL_H
#define ISAAC_HOST_VITA_TGT_RECIPROCAL_H

/* ISAAC_VITA_TIMEGETTIME_RECIPROCAL: (uint32_t)(t / 1000) for every uint64_t
 * t without a library division.  vita_post_com_timeGetTime
 * (host_vita_post_com.c) narrows sceKernelGetProcessTimeWide microseconds to
 * WinMM milliseconds with `(uint32_t)(t / UINT64_C(1000))`; GCC 10.3 for the
 * Cortex-A9 (no UDIV) lowers that to a __aeabi_uldivmod call on every guest
 * timeGetTime (about 376 per render in fight rooms, ph120.a tgt, A2 P2).
 *
 * Proof (complete, for all 0 <= t < 2^64):
 *   1. floor(t / 1000) = floor(floor(t / 8) / 125) (nested floor division of
 *      positive integers); n = t >> 3 < 2^61.
 *   2. M = ceil(2^68 / 125) = 0x20C49BA5E353F7CF, e = 125 M - 2^68 = 19.
 *      Write n = 125 q + r, 0 <= r <= 124.  Then
 *      n M / 2^68 = q + (r + n e / 2^68) / 125, and n e / 2^68 < 19/128 < 1,
 *      so 0 <= r + n e / 2^68 < 125 and floor(n M / 2^68) = q.
 *   3. n M < 2^61 * 2^62 = 2^123.  With n = n1 2^32 + n0, M = m1 2^32 + m0
 *      and the four 32x32->64 products pXY = nX mY, nM = p11 2^64 +
 *      (p01 + p10) 2^32 + p00.  Splitting p01, p10, p00 into 32-bit halves,
 *      mid = hi32(p00) + lo32(p01) + lo32(p10) < 3 2^32 and
 *      nM = (p11 + hi32(p01) + hi32(p10) + hi32(mid)) 2^64 +
 *           lo32(mid) 2^32 + lo32(p00), whose low part is < 2^64, so
 *      hi = floor(nM / 2^64) = p11 + hi32(p01) + hi32(p10) + hi32(mid)
 *      (< 2^59, no 64-bit overflow) and floor(nM / 2^68) = hi >> 4.
 *   4. The OFF expression narrows the same integer q with the same (uint32_t)
 *      conversion, so the two results are identical bit for bit.
 * recomp/test_vita_clear_expiry_latch.py re-derives M and e with Python
 * integers, and recomp/runtime/host_vita_tgt_reciprocal_test.c checks this
 * function against `(uint32_t)(t / 1000)` exhaustively over [0, 2^32), over
 * 2^32-long windows at 2^32, 2^53 and 2^64 - 2^32, at every multiple of 1000
 * (and +-1) below 2^40 and on 10^9 random values; mutants of each constant,
 * of the shift and of the carry must fail it. */

#include <stdint.h>

#define ISAAC_VITA_TGT_RECIPROCAL_M_LO 0xE353F7CFU
#define ISAAC_VITA_TGT_RECIPROCAL_M_HI 0x20C49BA5U
#define ISAAC_VITA_TGT_RECIPROCAL_PRESHIFT 3U
#define ISAAC_VITA_TGT_RECIPROCAL_POSTSHIFT 4U

static inline uint32_t isaac_vita_tgt_ms_from_us(uint64_t t)
{
    const uint64_t n = t >> ISAAC_VITA_TGT_RECIPROCAL_PRESHIFT;
    const uint32_t n0 = (uint32_t)n, n1 = (uint32_t)(n >> 32);
    const uint64_t p00 = (uint64_t)n0 * ISAAC_VITA_TGT_RECIPROCAL_M_LO;
    const uint64_t p01 = (uint64_t)n0 * ISAAC_VITA_TGT_RECIPROCAL_M_HI;
    const uint64_t p10 = (uint64_t)n1 * ISAAC_VITA_TGT_RECIPROCAL_M_LO;
    const uint64_t p11 = (uint64_t)n1 * ISAAC_VITA_TGT_RECIPROCAL_M_HI;
    const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    const uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);

    return (uint32_t)(hi >> ISAAC_VITA_TGT_RECIPROCAL_POSTSHIFT);
}

#endif
