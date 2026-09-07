#ifndef ISAAC_HOST_VITA_ATOF_SMALLINT_H
#define ISAAC_HOST_VITA_ATOF_SMALLINT_H

#include <float.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(double) == sizeof(uint64_t), "smallint requires binary64 storage");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "smallint requires IEEE binary64");

/* Only the already copied, NUL-terminated CRT conversion snapshot may enter.
 * Accept the entire optional-minus/1..9-digit string, not a numeric prefix.
 * Magnitudes <= 999999999 are exact in binary64 under every rounding mode.
 * Decimal points, exponents, whitespace, plus signs and longer inputs retain
 * the original strtod path. This is not a replacement XML parser or cache.
 */
static inline int isaac_vita_atof_smallint(const char *text, double *out)
{
    uint32_t magnitude = 0u, negative = 0u, i;
    if (*text == '-') {
        negative = 1u;
        ++text;
    }
    for (i = 0u; i < 9u; ++i) {
        const uint32_t digit = (uint32_t)(unsigned char)text[i] - (uint32_t)'0';
        if (digit > 9u)
            return 0;
        magnitude = magnitude * 10u + digit;
        if (text[i + 1u] == '\0') {
            double value = (double)magnitude;
            uint64_t bits;
            /* Preserve -0 without relying on floating-point negation. */
            memcpy(&bits, &value, sizeof bits);
            bits |= (uint64_t)negative << 63u;
            memcpy(out, &bits, sizeof bits);
            return 1;
        }
    }
    return 0;
}

#endif
