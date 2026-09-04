/*
 * SPDX-License-Identifier: MIT
 *
 * Reduced and altered from musl libc 1.2.5 src/stdio/vfprintf.c fmt_fp.
 * Copyright © 2005-2020 Rich Felker, et al.
 * See LICENSE and PROVENANCE.md in this directory.
 */
#ifndef ISAAC_VITA_MUSL_FMT_FP_H
#define ISAAC_VITA_MUSL_FMT_FP_H

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 &&
                   DBL_MAX_EXP == 1024,
               "musl_fmt_fp requires IEEE-754 binary64 double");
_Static_assert(sizeof(double) == 8 && sizeof(uint32_t) == 4 &&
                   sizeof(uint64_t) == 8,
               "musl_fmt_fp fixed-width type mismatch");

#if defined(__GNUC__) || defined(__clang__)
#define ISAAC_VITA_MUSL_FP_NOINLINE __attribute__((noinline))
#define ISAAC_VITA_MUSL_FP_INLINE static inline __attribute__((always_inline))
#else
#define ISAAC_VITA_MUSL_FP_NOINLINE
#define ISAAC_VITA_MUSL_FP_INLINE static inline
#endif

typedef struct isaac_vita_musl_fp_sink {
    char *buffer;
    size_t capacity;
    size_t length;
} isaac_vita_musl_fp_sink;

ISAAC_VITA_MUSL_FP_INLINE void
isaac_vita_musl_fp_write(isaac_vita_musl_fp_sink *sink,
                         const char *text, size_t length)
{
    size_t available = 0U;

    if (sink->capacity && sink->length < sink->capacity - 1U)
        available = sink->capacity - 1U - sink->length;
    if (available > length)
        available = length;
    if (available)
        memcpy(sink->buffer + sink->length, text, available);
    sink->length += length;
}

ISAAC_VITA_MUSL_FP_INLINE void
isaac_vita_musl_fp_repeat(isaac_vita_musl_fp_sink *sink,
                          char value, unsigned count)
{
    static const char zeroes[32] = {
        '0', '0', '0', '0', '0', '0', '0',
        '0', '0', '0', '0', '0', '0', '0',
        '0', '0', '0', '0', '0', '0', '0',
        '0', '0', '0', '0', '0', '0', '0',
        '0', '0', '0', '0'
    };

    while (count) {
        unsigned chunk = count > sizeof zeroes ? (unsigned)sizeof zeroes : count;
        if (value == '0') {
            isaac_vita_musl_fp_write(sink, zeroes, chunk);
        } else {
            char local[32];
            unsigned i;
            for (i = 0U; i < chunk; ++i)
                local[i] = value;
            isaac_vita_musl_fp_write(sink, local, chunk);
        }
        count -= chunk;
    }
}

ISAAC_VITA_MUSL_FP_INLINE char *
isaac_vita_musl_fp_u32(uint32_t value, char *end)
{
    do {
        *--end = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    return end;
}

/*
 * Format one binary64 value using exactly one of the runtime's supported
 * contracts: %f, %g, %.f, %.1f, %.2f or %.4f.  Return the full length as
 * snprintf would, NUL-terminate when capacity is nonzero, and never allocate.
 * Invalid style/precision pairs return -1.  errno is unchanged in all cases.
 */
static ISAAC_VITA_MUSL_FP_NOINLINE int
isaac_vita_musl_format_double(char *buffer, size_t capacity,
                              double original, char style,
                              unsigned precision)
{
    uint32_t big[(DBL_MANT_DIG + 28) / 29 + 1 +
                 (DBL_MAX_EXP + DBL_MANT_DIG + 28 + 8) / 9];
    uint32_t *a;
    uint32_t *d;
    uint32_t *r;
    uint32_t *z;
    isaac_vita_musl_fp_sink sink = { buffer, capacity, 0U };
    double value = original;
    int exponent2 = 0;
    int exponent;
    int i;
    int digits_after_radix;
    int p = (int)precision;
    char digits[9 + DBL_MANT_DIG / 4];
    char exponent_buffer[3 * sizeof(int)];
    char *exponent_end = exponent_buffer + sizeof exponent_buffer;
    char *exponent_text = exponent_end;
    int negative = signbit(value) != 0;
    int saved_errno = errno;
    int result = -1;

    if (!buffer && capacity)
        goto done;
    if (!((style == 'f' &&
           (precision == 0U || precision == 1U || precision == 2U ||
            precision == 4U || precision == 6U)) ||
          (style == 'g' && precision == 6U)))
        goto done;

    if (negative) {
        isaac_vita_musl_fp_write(&sink, "-", 1U);
        value = -value;
    }

    if (!isfinite(value)) {
        const char *special = value != value ? "nan" : "inf";
        isaac_vita_musl_fp_write(&sink, special, 3U);
        goto success;
    }

    value = frexp(value, &exponent2) * 2.0;
    if (value != 0.0)
        --exponent2;
    if (value != 0.0) {
        value *= 0x1p28;
        exponent2 -= 28;
    }

    if (exponent2 < 0)
        a = r = z = big;
    else
        a = r = z = big + sizeof big / sizeof *big - DBL_MANT_DIG - 1;

    do {
        *z = (uint32_t)value;
        value = 1000000000.0 * (value - *z++);
    } while (value != 0.0);

    while (exponent2 > 0) {
        uint32_t carry = 0U;
        int shift = exponent2 < 29 ? exponent2 : 29;
        d = z;
        while (d != a) {
            uint64_t expanded;
            --d;
            expanded = ((uint64_t)*d << shift) + carry;
            *d = (uint32_t)(expanded % 1000000000U);
            carry = (uint32_t)(expanded / 1000000000U);
        }
        if (carry)
            *--a = carry;
        while (z > a && !z[-1])
            --z;
        exponent2 -= shift;
    }

    while (exponent2 < 0) {
        uint32_t carry = 0U;
        uint32_t *limit;
        int shift = -exponent2 < 9 ? -exponent2 : 9;
        int needed = 1 + (p + DBL_MANT_DIG / 3 + 8) / 9;
        for (d = a; d < z; ++d) {
            uint32_t remainder = *d & ((1U << shift) - 1U);
            *d = (*d >> shift) + carry;
            carry = (1000000000U >> shift) * remainder;
        }
        if (!*a)
            ++a;
        if (carry)
            *z++ = carry;
        limit = style == 'f' ? r : a;
        if (z - limit > needed)
            z = limit + needed;
        exponent2 += shift;
    }

    if (a < z) {
        for (i = 10, exponent = 9 * (int)(r - a); *a >= (uint32_t)i;
             i *= 10, ++exponent) {
        }
    } else {
        exponent = 0;
    }

    /* precision after the radix; it may be negative while rounding. */
    digits_after_radix = p - (style != 'f') * exponent -
                         (style == 'g' && p);
    if (digits_after_radix < 9 * (int)(z - r - 1)) {
        uint32_t unit;
        uint32_t discarded;
        double round;
        double small;

        d = r + 1 +
            ((digits_after_radix + 9 * DBL_MAX_EXP) / 9 - DBL_MAX_EXP);
        digits_after_radix += 9 * DBL_MAX_EXP;
        digits_after_radix %= 9;
        for (i = 10, ++digits_after_radix;
             digits_after_radix < 9; i *= 10, ++digits_after_radix) {
        }
        unit = (uint32_t)i;
        discarded = *d % unit;
        if (discarded || d + 1 != z) {
            round = 2.0 / DBL_EPSILON;
            if (((*d / unit) & 1U) ||
                (unit == 1000000000U && d > a && (d[-1] & 1U)))
                round += 2.0;
            if (discarded < unit / 2U)
                small = 0x0.8p0;
            else if (discarded == unit / 2U && d + 1 == z)
                small = 0x1.0p0;
            else
                small = 0x1.8p0;
            if (negative) {
                round = -round;
                small = -small;
            }
            *d -= discarded;
            if (round + small != round) {
                *d += unit;
                while (*d > 999999999U) {
                    *d-- = 0U;
                    if (d < a)
                        *--a = 0U;
                    ++*d;
                }
                for (i = 10, exponent = 9 * (int)(r - a);
                     *a >= (uint32_t)i; i *= 10, ++exponent) {
                }
            }
        }
        if (z > d + 1)
            z = d + 1;
    }
    while (z > a && !z[-1])
        --z;

    if (style == 'g') {
        int trailing;
        if (!p)
            ++p;
        if (p > exponent && exponent >= -4) {
            style = 'f';
            p -= exponent + 1;
        } else {
            style = 'e';
            --p;
        }
        if (z > a && z[-1]) {
            uint32_t power;
            for (power = 10U, trailing = 0;
                 power <= 1000000000U && z[-1] % power == 0U;
                 power *= 10U, ++trailing) {
            }
        } else {
            trailing = 9;
        }
        if (style == 'f') {
            int available = 9 * (int)(z - r - 1) - trailing;
            if (available < 0)
                available = 0;
            if (p > available)
                p = available;
        } else {
            int available = 9 * (int)(z - r - 1) + exponent - trailing;
            if (available < 0)
                available = 0;
            if (p > available)
                p = available;
        }
    }

    if (style == 'f') {
        if (a > r)
            a = r;
        for (d = a; d <= r; ++d) {
            char *text = isaac_vita_musl_fp_u32(*d, digits + 9);
            if (d != a) {
                while (text > digits)
                    *--text = '0';
            } else if (text == digits + 9) {
                *--text = '0';
            }
            isaac_vita_musl_fp_write(&sink, text,
                                     (size_t)(digits + 9 - text));
        }
        if (p)
            isaac_vita_musl_fp_write(&sink, ".", 1U);
        for (; d < z && p > 0; ++d) {
            char *text = isaac_vita_musl_fp_u32(*d, digits + 9);
            unsigned chunk = p < 9 ? (unsigned)p : 9U;
            while (text > digits)
                *--text = '0';
            isaac_vita_musl_fp_write(&sink, text, chunk);
            p -= 9;
        }
        if (p > 0)
            isaac_vita_musl_fp_repeat(&sink, '0', (unsigned)p);
    } else {
        if (z <= a)
            z = a + 1;
        for (d = a; d < z && p >= 0; ++d) {
            char *text = isaac_vita_musl_fp_u32(*d, digits + 9);
            size_t available;
            size_t chunk;
            if (text == digits + 9)
                *--text = '0';
            if (d != a) {
                while (text > digits)
                    *--text = '0';
            } else {
                isaac_vita_musl_fp_write(&sink, text++, 1U);
                if (p > 0)
                    isaac_vita_musl_fp_write(&sink, ".", 1U);
            }
            available = (size_t)(digits + 9 - text);
            chunk = available < (size_t)p ? available : (size_t)p;
            isaac_vita_musl_fp_write(&sink, text, chunk);
            p -= (int)available;
        }
        if (p > 0)
            isaac_vita_musl_fp_repeat(&sink, '0', (unsigned)p);
        exponent_text = isaac_vita_musl_fp_u32(
            (uint32_t)(exponent < 0 ? -exponent : exponent), exponent_end);
        while (exponent_end - exponent_text < 2)
            *--exponent_text = '0';
        *--exponent_text = exponent < 0 ? '-' : '+';
        *--exponent_text = 'e';
        isaac_vita_musl_fp_write(&sink, exponent_text,
                                 (size_t)(exponent_end - exponent_text));
    }

success:
    if (sink.capacity)
        sink.buffer[sink.length < sink.capacity
                        ? sink.length
                        : sink.capacity - 1U] = '\0';
    if (sink.length <= INT_MAX)
        result = (int)sink.length;

done:
    if (result < 0 && sink.buffer && sink.capacity)
        sink.buffer[0] = '\0';
    errno = saved_errno;
    return result;
}

#undef ISAAC_VITA_MUSL_FP_NOINLINE
#undef ISAAC_VITA_MUSL_FP_INLINE

#endif
