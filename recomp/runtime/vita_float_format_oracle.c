/* Host differential oracle for the allocation-free Vita double formatter. */
#include <errno.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/musl_fmt_fp/musl_fmt_fp.h"

#if defined(ISAAC_VITA_FLOAT_FORMAT_IMPORT_PROBE)

int isaac_vita_float_format_import_probe(char *buffer, size_t capacity,
                                         double value, char style,
                                         unsigned precision)
{
    return isaac_vita_musl_format_double(
        buffer, capacity, value, style, precision);
}

#else

typedef struct oracle_format {
    const char *native;
    char style;
    unsigned precision;
} oracle_format;

static const oracle_format s_formats[] = {
    { "%f", 'f', 6U },
    { "%g", 'g', 6U },
    { "%.f", 'f', 0U },
    { "%.1f", 'f', 1U },
    { "%.2f", 'f', 2U },
    { "%.4f", 'f', 4U }
};

static unsigned s_failures;
static unsigned long long s_comparisons;
static int s_deny_allocations;
static unsigned s_denied_allocations;

#if defined(ISAAC_VITA_FLOAT_FORMAT_ALLOC_WRAP)
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

void *__wrap_malloc(size_t size)
{
    if (s_deny_allocations) {
        ++s_denied_allocations;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (s_deny_allocations) {
        ++s_denied_allocations;
        return NULL;
    }
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (s_deny_allocations) {
        ++s_denied_allocations;
        return NULL;
    }
    return __real_realloc(pointer, size);
}

void __wrap_free(void *pointer)
{
    if (s_deny_allocations) {
        ++s_denied_allocations;
        return;
    }
    __real_free(pointer);
}
#endif

static void oracle_failure(const char *what, const oracle_format *format,
                           uint64_t bits, const char *expected,
                           const char *actual, int expected_length,
                           int actual_length)
{
    if (s_failures < 12U) {
        fprintf(stderr,
                "float formatter %s: format=%s bits=%016llx "
                "expected(%d)='%s' actual(%d)='%s'\n",
                what, format ? format->native : "(none)",
                (unsigned long long)bits, expected_length,
                expected ? expected : "(none)", actual_length,
                actual ? actual : "(none)");
    }
    ++s_failures;
}

static uint64_t oracle_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double oracle_double(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int oracle_format_once(const oracle_format *format, double value,
                              int test_truncation)
{
    enum { BUFFER_SIZE = 512, ERRNO_SENTINEL = EDOM };
    unsigned char storage[BUFFER_SIZE + 16];
    char expected[BUFFER_SIZE];
    char actual[BUFFER_SIZE];
    uint64_t bits = oracle_bits(value);
    int expected_length;
    int actual_length;
    size_t capacity;

    if (isnan(value)) {
        memcpy(expected, signbit(value) ? "-nan" : "nan",
               signbit(value) ? sizeof "-nan" : sizeof "nan");
        expected_length = signbit(value) ? 4 : 3;
    } else {
        expected_length = snprintf(
            expected, sizeof expected, format->native, value);
    }
    if (expected_length < 0 || expected_length >= (int)sizeof expected) {
        oracle_failure("native overflow", format, bits, expected, NULL,
                       expected_length, -1);
        return 0;
    }

    memset(actual, 0x5a, sizeof actual);
    errno = ERRNO_SENTINEL;
    s_deny_allocations = 1;
    actual_length = isaac_vita_musl_format_double(
        actual, sizeof actual, value, format->style, format->precision);
    s_deny_allocations = 0;
    ++s_comparisons;
    if (errno != ERRNO_SENTINEL || actual_length != expected_length ||
        strcmp(actual, expected) != 0) {
        oracle_failure("parity", format, bits, expected, actual,
                       expected_length, actual_length);
        return 0;
    }

    if (!test_truncation)
        return 1;

    errno = ERRNO_SENTINEL;
    actual_length = isaac_vita_musl_format_double(
        NULL, 0U, value, format->style, format->precision);
    if (errno != ERRNO_SENTINEL || actual_length != expected_length) {
        oracle_failure("zero-capacity", format, bits, expected, NULL,
                       expected_length, actual_length);
        return 0;
    }

    for (capacity = 1U; capacity <= (size_t)expected_length + 2U;
         ++capacity) {
        size_t prefix = capacity - 1U;
        size_t i;
        if (prefix > (size_t)expected_length)
            prefix = (size_t)expected_length;
        memset(storage, 0xa5, sizeof storage);
        errno = ERRNO_SENTINEL;
        s_deny_allocations = 1;
        actual_length = isaac_vita_musl_format_double(
            (char *)storage, capacity, value,
            format->style, format->precision);
        s_deny_allocations = 0;
        ++s_comparisons;
        if (errno != ERRNO_SENTINEL || actual_length != expected_length ||
            memcmp(storage, expected, prefix) != 0 ||
            storage[prefix] != '\0') {
            oracle_failure("truncation", format, bits, expected,
                           (const char *)storage, expected_length,
                           actual_length);
            return 0;
        }
        for (i = capacity; i < sizeof storage; ++i) {
            if (storage[i] != 0xa5) {
                oracle_failure("truncation canary", format, bits,
                               expected, (const char *)storage,
                               expected_length, actual_length);
                return 0;
            }
        }
    }
    return 1;
}

static int oracle_literal(const oracle_format *format, double value,
                          const char *expected)
{
    char actual[512];
    int length;

    errno = ERANGE;
    s_deny_allocations = 1;
    length = isaac_vita_musl_format_double(
        actual, sizeof actual, value, format->style, format->precision);
    s_deny_allocations = 0;
    ++s_comparisons;
    if (errno != ERANGE || length != (int)strlen(expected) ||
        strcmp(actual, expected) != 0) {
        oracle_failure("literal", format, oracle_bits(value), expected,
                       actual, (int)strlen(expected), length);
        return 0;
    }
    return 1;
}

static uint64_t oracle_random(void)
{
    static uint64_t state = UINT64_C(0x4d595df4d0f33173);
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

int main(void)
{
    static const uint64_t selected_bits[] = {
        UINT64_C(0x0000000000000000), /* +0 */
        UINT64_C(0x8000000000000000), /* -0 */
        UINT64_C(0x0000000000000001), /* minimum subnormal */
        UINT64_C(0x8000000000000001),
        UINT64_C(0x000fffffffffffff), /* maximum subnormal */
        UINT64_C(0x0010000000000000), /* DBL_MIN */
        UINT64_C(0x7fefffffffffffff), /* DBL_MAX */
        UINT64_C(0xffefffffffffffff),
        UINT64_C(0x7ff0000000000000), /* +inf */
        UINT64_C(0xfff0000000000000), /* -inf */
        UINT64_C(0x7ff8000000000000), /* +nan */
        UINT64_C(0xfff8000000000000)  /* -nan */
    };
    static const double rounding_values[] = {
        0.5, -0.5, 1.5, -1.5, 2.5, -2.5,
        9.999, -9.999, 99.9999, -99.9999,
        999999.5, -999999.5, 0.00009999995, -0.00009999995,
        1.2344499999999999, 1.2344500000000001,
        2.25001, -2.25001
    };
    double nextafter_values[] = {
        nextafter(0.5, 0.0), nextafter(0.5, 1.0),
        nextafter(1.0, 0.0), nextafter(1.0, 2.0),
        nextafter(1.5, 1.0), nextafter(1.5, 2.0),
        nextafter(2.5, 2.0), nextafter(2.5, 3.0),
        nextafter(9.995, 0.0), nextafter(9.995, INFINITY),
        nextafter(DBL_MAX, 0.0), nextafter(DBL_MIN, 0.0)
    };
    char invalid[8] = "dirty";
    size_t i;
    size_t j;
    unsigned random_index;

    if (!setlocale(LC_ALL, "C")) {
        fputs("float formatter could not select C locale\n", stderr);
        return 2;
    }

    oracle_literal(&s_formats[0],
                   oracle_double(UINT64_C(0x4002000540000000)),
                   "2.250010");
    oracle_literal(&s_formats[0], 0.0, "0.000000");
    oracle_literal(&s_formats[0], -0.0, "-0.000000");
    oracle_literal(&s_formats[1], 0.0, "0");
    oracle_literal(&s_formats[1], -0.0, "-0");
    oracle_literal(&s_formats[2], 0.5, "0");
    oracle_literal(&s_formats[2], 1.5, "2");
    oracle_literal(&s_formats[2], 2.5, "2");
    oracle_literal(&s_formats[2],
                   oracle_double(UINT64_C(0x3fefffffffffffff)), "1");
    oracle_literal(&s_formats[2],
                   oracle_double(UINT64_C(0xbfefffffffffffff)), "-1");
    oracle_literal(&s_formats[4], 9.999, "10.00");
    oracle_literal(&s_formats[0], INFINITY, "inf");
    oracle_literal(&s_formats[0], -INFINITY, "-inf");
    oracle_literal(&s_formats[1], NAN, "nan");
    oracle_literal(&s_formats[1], -NAN, "-nan");

    for (i = 0U; i < sizeof selected_bits / sizeof selected_bits[0]; ++i) {
        double value = oracle_double(selected_bits[i]);
        for (j = 0U; j < sizeof s_formats / sizeof s_formats[0]; ++j)
            oracle_format_once(&s_formats[j], value, 1);
    }
    for (i = 0U; i < sizeof rounding_values / sizeof rounding_values[0]; ++i) {
        for (j = 0U; j < sizeof s_formats / sizeof s_formats[0]; ++j)
            oracle_format_once(&s_formats[j], rounding_values[i], 1);
    }
    for (i = 0U; i < sizeof nextafter_values / sizeof nextafter_values[0]; ++i) {
        for (j = 0U; j < sizeof s_formats / sizeof s_formats[0]; ++j)
            oracle_format_once(&s_formats[j], nextafter_values[i], 1);
    }

    for (random_index = 0U; random_index < 65536U; ++random_index) {
        double value = oracle_double(oracle_random());
        for (j = 0U; j < sizeof s_formats / sizeof s_formats[0]; ++j)
            oracle_format_once(&s_formats[j], value, 0);
    }

    errno = EILSEQ;
    if (isaac_vita_musl_format_double(invalid, sizeof invalid,
                                      1.0, 'e', 6U) != -1 ||
        invalid[0] != '\0' || errno != EILSEQ) {
        oracle_failure("invalid style", NULL, oracle_bits(1.0), "", invalid,
                       -1, -1);
    }
    memcpy(invalid, "dirty", sizeof "dirty");
    errno = EILSEQ;
    if (isaac_vita_musl_format_double(invalid, sizeof invalid,
                                      1.0, 'f', 3U) != -1 ||
        invalid[0] != '\0' || errno != EILSEQ) {
        oracle_failure("invalid precision", NULL, oracle_bits(1.0), "",
                       invalid, -1, -1);
    }
    errno = EILSEQ;
    if (isaac_vita_musl_format_double(NULL, 1U,
                                      1.0, 'f', 6U) != -1 ||
        errno != EILSEQ) {
        oracle_failure("invalid buffer", NULL, oracle_bits(1.0), NULL, NULL,
                       -1, -1);
    }

    if (s_denied_allocations) {
        fprintf(stderr, "float formatter attempted %u allocation(s)\n",
                s_denied_allocations);
        ++s_failures;
    }
    if (s_failures) {
        fprintf(stderr,
                "Vita float formatter oracle: FAIL (%u failures after "
                "%llu comparisons)\n",
                s_failures, s_comparisons);
        return 1;
    }

    printf("Vita float formatter oracle: PASS (%llu comparisons; "
           "65536 randomized binary64 values x 6 formats; no allocation)\n",
           s_comparisons);
    return 0;
}

#endif
