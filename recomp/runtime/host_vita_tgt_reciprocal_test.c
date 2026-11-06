/* Host test B1 of host_vita_tgt_reciprocal.h (RENDER60 S6 leg 2).
 *
 * isaac_vita_tgt_ms_from_us(t) must equal (uint32_t)(t / 1000) for every
 * uint64_t t.  The header carries the complete proof; this program checks the
 * shipped function (and, through recomp/test_vita_clear_expiry_latch.py, its
 * mutants) against the division:
 *
 *   W1  every t in [0, 2^32)
 *   W2  every t in [2^32, 2^33)
 *   W3  every t in [2^53, 2^53 + 2^32)
 *   W4  every t in [2^64 - 2^32, 2^64)
 *   MUL t = 1000 k - 1, 1000 k, 1000 k + 1 for 1 <= k <= floor(2^40 / 1000)
 *   RND 10^9 pseudo-random values (half uniform 64-bit, half x >> (y & 63)
 *       so every magnitude is sampled), against a volatile-divisor division
 *
 * The windows use an incremental quotient/remainder reference (one real
 * division at each chunk start, re-checked against a real division at each
 * chunk end), so the 2^34 window values cost no division each.
 *
 * usage: host_vita_tgt_reciprocal_test PART NPARTS full|quick
 *   PART in [0, NPARTS): this process checks the PART-th contiguous slice of
 *   every range (the driver runs NPARTS processes).  quick checks the first
 *   2^24 values of W1/W2/W3, the last 2^24 of W4, the first 2^20 k of MUL and
 *   10^6 RND values (mutant runs), split the same way.
 * Prints one line:
 *   tgt-reciprocal part=P/N mode=M checked=C mismatches=X first=T
 * and exits 1 when X > 0 or a chunk-end cross-check fails. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_tgt_reciprocal.h"

static volatile uint64_t s_divisor = UINT64_C(1000);
static uint64_t s_checked, s_mismatches, s_first = UINT64_MAX;
static int s_reference_broken;

static void note(uint64_t t, uint32_t got, uint32_t want)
{
    ++s_checked;
    if (got != want) {
        if (!s_mismatches)
            s_first = t;
        ++s_mismatches;
    }
}

/* [lo, lo + count) without wrapping past 2^64 - 1 (count >= 1). */
static void window(uint64_t lo, uint64_t count)
{
    const uint64_t divisor = s_divisor;
    uint64_t q = lo / divisor, i;
    uint32_t r = (uint32_t)(lo % divisor);
    uint64_t t = lo;

    for (i = 0; i < count; ++i, ++t) {
        uint32_t got = isaac_vita_tgt_ms_from_us(t);

        ++s_checked;
        if (got != (uint32_t)q) {
            if (!s_mismatches)
                s_first = t;
            ++s_mismatches;
        }
        if (i + 1 == count)
            break;
        if (++r == 1000U) {
            r = 0U;
            ++q;
        }
    }
    /* the incremental reference itself, at the slice end */
    if (q != t / divisor || r != (uint32_t)(t % divisor))
        s_reference_broken = 1;
}

static void slice(uint64_t lo, uint64_t count, uint32_t part, uint32_t parts,
                  uint64_t *out_lo, uint64_t *out_count)
{
    const uint64_t step = count / parts;
    const uint64_t begin = lo + step * part;
    const uint64_t end = part + 1U == parts ? lo + count : begin + step;

    *out_lo = begin;
    *out_count = end - begin;
}

static void window_part(uint64_t lo, uint64_t count, uint32_t part,
                        uint32_t parts)
{
    uint64_t a, n;

    slice(lo, count, part, parts, &a, &n);
    if (n)
        window(a, n);
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));

    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

int main(int argc, char **argv)
{
    uint32_t part, parts;
    int quick;
    uint64_t span, a, n, k, kmax, rnd, state, i;

    if (argc != 4) {
        fprintf(stderr, "usage: %s PART NPARTS full|quick\n", argv[0]);
        return 2;
    }
    part = (uint32_t)strtoul(argv[1], NULL, 10);
    parts = (uint32_t)strtoul(argv[2], NULL, 10);
    quick = strcmp(argv[3], "quick") == 0;
    if (!parts || part >= parts || (!quick && strcmp(argv[3], "full") != 0)) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }
    span = quick ? (UINT64_C(1) << 24) : (UINT64_C(1) << 32);
    window_part(0, span, part, parts);                              /* W1 */
    window_part(UINT64_C(1) << 32, span, part, parts);              /* W2 */
    window_part(UINT64_C(1) << 53, span, part, parts);              /* W3 */
    window_part(UINT64_MAX - span + 1U, span, part, parts);         /* W4 */

    kmax = quick ? (UINT64_C(1) << 20) : ((UINT64_C(1) << 40) / 1000U);
    slice(1U, kmax, part, parts, &a, &n);                           /* MUL */
    for (k = a; k < a + n; ++k) {
        const uint64_t t = UINT64_C(1000) * k;

        note(t - 1U, isaac_vita_tgt_ms_from_us(t - 1U), (uint32_t)(k - 1U));
        note(t, isaac_vita_tgt_ms_from_us(t), (uint32_t)k);
        note(t + 1U, isaac_vita_tgt_ms_from_us(t + 1U), (uint32_t)k);
    }

    rnd = quick ? UINT64_C(1000000) : UINT64_C(1000000000);
    slice(0U, rnd, part, parts, &a, &n);                            /* RND */
    state = UINT64_C(0x20261026) ^ (UINT64_C(0x9E3779B97F4A7C15) * (part + 1U));
    for (i = 0; i < n; ++i) {
        uint64_t t = splitmix64(&state);

        if (i & 1U)
            t >>= splitmix64(&state) & 63U;
        note(t, isaac_vita_tgt_ms_from_us(t), (uint32_t)(t / s_divisor));
    }

    printf("tgt-reciprocal part=%u/%u mode=%s checked=%" PRIu64
           " mismatches=%" PRIu64 " first=%" PRIu64 "%s\n",
           (unsigned)part, (unsigned)parts, quick ? "quick" : "full",
           s_checked, s_mismatches,
           s_mismatches ? s_first : UINT64_C(0),
           s_reference_broken ? " REFERENCE-BROKEN" : "");
    return s_mismatches || s_reference_broken ? 1 : 0;
}
