/* Executable host oracle plus softfp source oracle for the complete Vita UCRT
 * math family.  The ARM ELF is linked and inspected but deliberately not run:
 * arm-vita-eabi-run is not a Vita execution oracle. */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_math.h"

enum math_policy {
    MATH_PROVEN = 1
};

typedef struct math_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t thunk_rva;
    uint32_t call_count;
    uint64_t call_hash;
    uint32_t policy;
} math_evidence;

static const math_evidence s_family[ISAAC_VITA_MATH_FAMILY_SLOT_COUNT] = {
    { ISAAC_VITA_MATH_CEIL_NAME, ISAAC_VITA_MATH_CEIL_IAT_RVA,
      ISAAC_VITA_MATH_CEIL_THUNK_RVA, ISAAC_VITA_MATH_CEIL_CALL_COUNT,
      ISAAC_VITA_MATH_CEIL_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_LDEXP_NAME, ISAAC_VITA_MATH_LDEXP_IAT_RVA,
      ISAAC_VITA_MATH_LDEXP_THUNK_RVA, ISAAC_VITA_MATH_LDEXP_CALL_COUNT,
      ISAAC_VITA_MATH_LDEXP_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_FLOOR_NAME, ISAAC_VITA_MATH_FLOOR_IAT_RVA,
      ISAAC_VITA_MATH_FLOOR_THUNK_RVA, ISAAC_VITA_MATH_FLOOR_CALL_COUNT,
      ISAAC_VITA_MATH_FLOOR_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_COPYSIGNF_NAME, ISAAC_VITA_MATH_COPYSIGNF_IAT_RVA,
      ISAAC_VITA_MATH_COPYSIGNF_THUNK_RVA,
      ISAAC_VITA_MATH_COPYSIGNF_CALL_COUNT,
      ISAAC_VITA_MATH_COPYSIGNF_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_FDCLASS_NAME, ISAAC_VITA_MATH_FDCLASS_IAT_RVA,
      ISAAC_VITA_MATH_FDCLASS_THUNK_RVA, ISAAC_VITA_MATH_FDCLASS_CALL_COUNT,
      ISAAC_VITA_MATH_FDCLASS_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_ACOS_NAME, ISAAC_VITA_MATH_ACOS_IAT_RVA,
      ISAAC_VITA_MATH_ACOS_THUNK_RVA, ISAAC_VITA_MATH_ACOS_CALL_COUNT,
      ISAAC_VITA_MATH_ACOS_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_CIFMOD_NAME, ISAAC_VITA_MATH_CIFMOD_IAT_RVA,
      ISAAC_VITA_MATH_CIFMOD_THUNK_RVA, ISAAC_VITA_MATH_CIFMOD_CALL_COUNT,
      ISAAC_VITA_MATH_CIFMOD_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_SQRT_NAME, ISAAC_VITA_MATH_SQRT_IAT_RVA,
      ISAAC_VITA_MATH_SQRT_THUNK_RVA, ISAAC_VITA_MATH_SQRT_CALL_COUNT,
      ISAAC_VITA_MATH_SQRT_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_SIN_NAME, ISAAC_VITA_MATH_SIN_IAT_RVA,
      ISAAC_VITA_MATH_SIN_THUNK_RVA, ISAAC_VITA_MATH_SIN_CALL_COUNT,
      ISAAC_VITA_MATH_SIN_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_POW_NAME, ISAAC_VITA_MATH_POW_IAT_RVA,
      ISAAC_VITA_MATH_POW_THUNK_RVA, ISAAC_VITA_MATH_POW_CALL_COUNT,
      ISAAC_VITA_MATH_POW_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_ROUNDF_NAME, ISAAC_VITA_MATH_ROUNDF_IAT_RVA,
      ISAAC_VITA_MATH_ROUNDF_THUNK_RVA, ISAAC_VITA_MATH_ROUNDF_CALL_COUNT,
      ISAAC_VITA_MATH_ROUNDF_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_LOG_NAME, ISAAC_VITA_MATH_LOG_IAT_RVA,
      ISAAC_VITA_MATH_LOG_THUNK_RVA, ISAAC_VITA_MATH_LOG_CALL_COUNT,
      ISAAC_VITA_MATH_LOG_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_MODF_NAME, ISAAC_VITA_MATH_MODF_IAT_RVA,
      ISAAC_VITA_MATH_MODF_THUNK_RVA, ISAAC_VITA_MATH_MODF_CALL_COUNT,
      ISAAC_VITA_MATH_MODF_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_NEXTAFTERF_NAME, ISAAC_VITA_MATH_NEXTAFTERF_IAT_RVA,
      ISAAC_VITA_MATH_NEXTAFTERF_THUNK_RVA,
      ISAAC_VITA_MATH_NEXTAFTERF_CALL_COUNT,
      ISAAC_VITA_MATH_NEXTAFTERF_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_CIATAN2_NAME, ISAAC_VITA_MATH_CIATAN2_IAT_RVA,
      ISAAC_VITA_MATH_CIATAN2_THUNK_RVA, ISAAC_VITA_MATH_CIATAN2_CALL_COUNT,
      ISAAC_VITA_MATH_CIATAN2_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_ASIN_NAME, ISAAC_VITA_MATH_ASIN_IAT_RVA,
      ISAAC_VITA_MATH_ASIN_THUNK_RVA, ISAAC_VITA_MATH_ASIN_CALL_COUNT,
      ISAAC_VITA_MATH_ASIN_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_ATAN_NAME, ISAAC_VITA_MATH_ATAN_IAT_RVA,
      ISAAC_VITA_MATH_ATAN_THUNK_RVA, ISAAC_VITA_MATH_ATAN_CALL_COUNT,
      ISAAC_VITA_MATH_ATAN_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_SETUSERMATHERR_NAME,
      ISAAC_VITA_MATH_SETUSERMATHERR_IAT_RVA,
      ISAAC_VITA_MATH_SETUSERMATHERR_THUNK_RVA,
      ISAAC_VITA_MATH_SETUSERMATHERR_CALL_COUNT,
      ISAAC_VITA_MATH_SETUSERMATHERR_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_LOG10_NAME, ISAAC_VITA_MATH_LOG10_IAT_RVA,
      ISAAC_VITA_MATH_LOG10_THUNK_RVA, ISAAC_VITA_MATH_LOG10_CALL_COUNT,
      ISAAC_VITA_MATH_LOG10_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_COS_NAME, ISAAC_VITA_MATH_COS_IAT_RVA,
      ISAAC_VITA_MATH_COS_THUNK_RVA, ISAAC_VITA_MATH_COS_CALL_COUNT,
      ISAAC_VITA_MATH_COS_CALL_FNV64, MATH_PROVEN },
    { ISAAC_VITA_MATH_EXP_NAME, ISAAC_VITA_MATH_EXP_IAT_RVA,
      ISAAC_VITA_MATH_EXP_THUNK_RVA, ISAAC_VITA_MATH_EXP_CALL_COUNT,
      ISAAC_VITA_MATH_EXP_CALL_FNV64, MATH_PROVEN }
};

_Static_assert((ISAAC_VITA_MATH_FAMILY_LAST_IAT_RVA -
                ISAAC_VITA_MATH_FAMILY_FIRST_IAT_RVA) / 4U + 1U == 21U,
               "math IAT arithmetic must independently remain 21");
_Static_assert(sizeof s_family / sizeof s_family[0] == 21U,
               "math policy table must cover every IAT slot");
_Static_assert(ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT == 21U &&
                   ISAAC_VITA_MATH_LOUD_IMPORT_COUNT == 0U,
               "math policy must remain complete 21/21");
_Static_assert(ISAAC_VITA_MATH_PROVEN_PHYSICAL_CALL_COUNT == 593U &&
                   ISAAC_VITA_MATH_LOUD_PHYSICAL_CALL_COUNT == 0U &&
                   ISAAC_VITA_MATH_PHYSICAL_CALL_COUNT == 593U,
               "math physical-site denominator drifted");
_Static_assert(ISAAC_VITA_MATH_FLOOR_FIRST_IMPORT_CALL_RVA == 0x00047b1dU &&
                   ISAAC_VITA_MATH_FLOOR_FIRST_IMPORT_RETURN_RVA ==
                       0x00047b22U,
               "first normal floor import boundary drifted");
_Static_assert(ISAAC_VITA_MATH_LDEXP_CALL_A_RVA == 0x005bbf86U &&
                   ISAAC_VITA_MATH_LDEXP_RETURN_A_RVA == 0x005bbf8cU &&
                   ISAAC_VITA_MATH_LDEXP_CALL_B_RVA == 0x005bbfe5U &&
                   ISAAC_VITA_MATH_LDEXP_RETURN_B_RVA == 0x005bbfebU &&
                   ISAAC_VITA_MATH_LDEXP_CALL_C_RVA == 0x005c81e3U &&
                   ISAAC_VITA_MATH_LDEXP_RETURN_C_RVA == 0x005c81e9U &&
                   ISAAC_VITA_MATH_LDEXP_CALL_D_RVA == 0x005c8224U &&
                   ISAAC_VITA_MATH_LDEXP_RETURN_D_RVA == 0x005c822aU,
               "ldexp four-site ABI evidence drifted");
_Static_assert(ISAAC_VITA_MATH_COPYSIGNF_CALL_RVA == 0x0002a0e7U &&
                   ISAAC_VITA_MATH_COPYSIGNF_RETURN_RVA == 0x0002a0edU &&
                   ISAAC_VITA_MATH_ROUNDF_CALL_A_RVA == 0x0003f1e5U &&
                   ISAAC_VITA_MATH_ROUNDF_RETURN_A_RVA == 0x0003f1eaU &&
                   ISAAC_VITA_MATH_ROUNDF_CALL_B_RVA == 0x004b81daU &&
                   ISAAC_VITA_MATH_ROUNDF_RETURN_B_RVA == 0x004b81dfU &&
                   ISAAC_VITA_MATH_MODF_CALL_RVA == 0x0047facdU &&
                   ISAAC_VITA_MATH_MODF_RETURN_RVA == 0x0047fad3U,
               "binary32/modf callsite evidence drifted");
_Static_assert(ISAAC_VITA_MATH_SETUSERMATHERR_CALL_RVA == 0x005eb65cU &&
                   ISAAC_VITA_MATH_SETUSERMATHERR_RETURN_RVA ==
                       0x005eb661U &&
                   ISAAC_VITA_MATH_SETUSERMATHERR_CALLBACK_RVA ==
                       0x00542a20U &&
                   ISAAC_VITA_MATH_SETUSERMATHERR_CALLBACK_SIZE == 3U &&
                   ISAAC_VITA_MATH_SETUSERMATHERR_CALLBACK_FNV64 ==
                       UINT64_C(0x58389c18233078ff),
               "__setusermatherr exact callback proof drifted");

int32_t g_isaac_vita_crt_errno;

_Alignas(8) static uint32_t s_frame[16];

void guest_fault(CPU *__restrict c, uint32_t addr, const char *what)
{
    if (!c->fault) {
        c->fault = what;
        c->fault_addr = addr;
    }
}

static uint32_t pointer32(void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint32_t prepare_call(CPU *c)
{
    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[4] = 0x0badc0deU;
    c->esp = pointer32(&s_frame[4]);
    return c->esp;
}

static uint64_t double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int equal_double(double actual, double expected)
{
    return isnan(expected) ? isnan(actual) :
           double_bits(actual) == double_bits(expected);
}

static int check_policy(void)
{
    unsigned i;
    unsigned proven = 0U;
    unsigned proven_sites = 0U;

    for (i = 0U; i < ISAAC_VITA_MATH_FAMILY_SLOT_COUNT; ++i) {
        if (!s_family[i].name || s_family[i].iat_rva !=
                ISAAC_VITA_MATH_FAMILY_FIRST_IAT_RVA + i * 4U ||
            s_family[i].call_count == 0U || s_family[i].call_hash == 0U)
            return 0;
        if (s_family[i].policy != MATH_PROVEN)
            return 0;
        ++proven;
        proven_sites += s_family[i].call_count;
    }
    return proven == ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT &&
           proven_sites == ISAAC_VITA_MATH_PROVEN_PHYSICAL_CALL_COUNT &&
           ISAAC_VITA_MATH_LOUD_IMPORT_COUNT == 0U &&
           ISAAC_VITA_MATH_LOUD_PHYSICAL_CALL_COUNT == 0U;
}

static int check_rounding(const char *name, double input,
                          double (*operation)(double), unsigned *calls)
{
    static const double sentinel = 3.14159265358979323846;
    CPU c;
    uint32_t esp = prepare_call(&c);
    unsigned before = *calls;
    double expected = operation(input);

    memcpy(&s_frame[5], &input, sizeof input);
    c.st_top = 3;
    *fst(&c, 0) = sentinel;
    g_isaac_vita_crt_errno = 91;
    errno = EACCES;
    if (!isaac_vita_math_import_counted(&c, name, calls) ||
        *calls != before + 1U || c.fault || c.esp != esp + 4U ||
        c.st_top != 2 || !equal_double(*fst(&c, 0), expected) ||
        double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
        g_isaac_vita_crt_errno != 91 || errno != EACCES)
        return 0;
    return 1;
}

static int check_ldexp(unsigned *calls)
{
    static const struct {
        uint64_t input;
        int32_t exponent;
        uint64_t output;
        int overflow;
    } cases[] = {
        { UINT64_C(0x0000000000000000), 4000,
          UINT64_C(0x0000000000000000), 0 },
        { UINT64_C(0x8000000000000000), 4000,
          UINT64_C(0x8000000000000000), 0 },
        { UINT64_C(0x3ff0000000000000), 1,
          UINT64_C(0x4000000000000000), 0 },
        { UINT64_C(0x0010000000000000), -1,
          UINT64_C(0x0008000000000000), 0 },
        { UINT64_C(0x0000000000000001), -1,
          UINT64_C(0x0000000000000000), 0 },
        { UINT64_C(0x8000000000000001), -1,
          UINT64_C(0x8000000000000000), 0 },
        { UINT64_C(0x7fefffffffffffff), 1,
          UINT64_C(0x7ff0000000000000), 1 },
        { UINT64_C(0xffefffffffffffff), 1,
          UINT64_C(0xfff0000000000000), 1 },
        { UINT64_C(0x7ff0000000000000), -4000,
          UINT64_C(0x7ff0000000000000), 0 },
        { UINT64_C(0x7ff8000000001234), 9,
          UINT64_C(0x7ff8000000001234), 0 },
        { UINT64_C(0x7ff0000000001234), 9,
          UINT64_C(0x7ff8000000001234), 0 }
    };
    static const double sentinel = 6.28318530717958647692;
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CPU c;
        uint32_t esp = prepare_call(&c);
        unsigned before = *calls;
        double input = double_from_bits(cases[i].input);

        memcpy(&s_frame[5], &input, sizeof input);
        s_frame[7] = (uint32_t)cases[i].exponent;
        c.st_top = 3;
        *fst(&c, 0) = sentinel;
        g_isaac_vita_crt_errno = 81;
        errno = EACCES;
        if (!isaac_vita_math_import_counted(
                &c, ISAAC_VITA_MATH_LDEXP_NAME, calls) ||
            *calls != before + 1U || c.fault || c.esp != esp + 4U ||
            c.st_top != 2 || double_bits(*fst(&c, 0)) != cases[i].output ||
            double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
            g_isaac_vita_crt_errno !=
                (cases[i].overflow ? ERANGE : 81) ||
            errno != EACCES)
            return 0;
    }
    return 1;
}

static int check_copysignf(unsigned *calls)
{
    static const struct {
        uint32_t magnitude;
        uint32_t sign;
        uint32_t output;
    } cases[] = {
        { 0x3f800000U, 0x80000000U, 0xbf800000U },
        { 0xbf800000U, 0x00000000U, 0x3f800000U },
        { 0x00000000U, 0xbf800000U, 0x80000000U },
        { 0x7fc01234U, 0xbf800000U, 0xffc01234U },
        { 0x7f801234U, 0x3f800000U, 0x7fc01234U },
        { 0x00000001U, 0xff801234U, 0x80000001U }
    };
    static const double sentinel = 1.73205080756887729353;
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CPU c;
        uint32_t esp = prepare_call(&c);
        unsigned before = *calls;

        s_frame[5] = cases[i].magnitude;
        s_frame[6] = cases[i].sign;
        c.st_top = 3;
        *fst(&c, 0) = sentinel;
        g_isaac_vita_crt_errno = 82;
        errno = EACCES;
        if (!isaac_vita_math_import_counted(
                &c, ISAAC_VITA_MATH_COPYSIGNF_NAME, calls) ||
            *calls != before + 1U || c.fault || c.esp != esp + 4U ||
            c.st_top != 2 ||
            float_bits((float)*fst(&c, 0)) != cases[i].output ||
            double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
            g_isaac_vita_crt_errno != 82 || errno != EACCES)
            return 0;
    }
    return 1;
}

static int check_roundf(unsigned *calls)
{
    static const struct {
        uint32_t input;
        uint32_t output;
    } cases[] = {
        { 0x00000000U, 0x00000000U },
        { 0x80000000U, 0x80000000U },
        { 0x3effffffU, 0x00000000U },
        { 0xbeffffffU, 0x80000000U },
        { 0x3f000000U, 0x3f800000U },
        { 0xbf000000U, 0xbf800000U },
        { 0x3fc00000U, 0x40000000U },
        { 0xbfc00000U, 0xc0000000U },
        { 0x00000001U, 0x00000000U },
        { 0x80000001U, 0x80000000U },
        { 0x4affffffU, 0x4b000000U },
        { 0xcaffffffU, 0xcb000000U },
        { 0x7f800000U, 0x7f800000U },
        { 0xff800000U, 0xff800000U },
        { 0x7fc01234U, 0x7fc01234U },
        { 0x7f801234U, 0x7fc01234U }
    };
    static const double sentinel = 1.61803398874989484820;
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CPU c;
        uint32_t esp = prepare_call(&c);
        unsigned before = *calls;

        s_frame[5] = cases[i].input;
        c.st_top = 3;
        *fst(&c, 0) = sentinel;
        g_isaac_vita_crt_errno = 83;
        errno = EACCES;
        if (!isaac_vita_math_import_counted(
                &c, ISAAC_VITA_MATH_ROUNDF_NAME, calls) ||
            *calls != before + 1U || c.fault || c.esp != esp + 4U ||
            c.st_top != 2 ||
            float_bits((float)*fst(&c, 0)) != cases[i].output ||
            double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
            g_isaac_vita_crt_errno != 83 || errno != EACCES)
            return 0;
    }
    return 1;
}

static int check_modf(unsigned *calls)
{
    static const struct {
        uint64_t input;
        uint64_t fraction;
        uint64_t integer;
    } cases[] = {
        { UINT64_C(0x0000000000000000), UINT64_C(0x0000000000000000),
          UINT64_C(0x0000000000000000) },
        { UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000),
          UINT64_C(0x8000000000000000) },
        { UINT64_C(0x4004000000000000), UINT64_C(0x3fe0000000000000),
          UINT64_C(0x4000000000000000) },
        { UINT64_C(0xc004000000000000), UINT64_C(0xbfe0000000000000),
          UINT64_C(0xc000000000000000) },
        { UINT64_C(0x4000000000000000), UINT64_C(0x0000000000000000),
          UINT64_C(0x4000000000000000) },
        { UINT64_C(0xc000000000000000), UINT64_C(0x8000000000000000),
          UINT64_C(0xc000000000000000) },
        { UINT64_C(0x7ff0000000000000), UINT64_C(0x0000000000000000),
          UINT64_C(0x7ff0000000000000) },
        { UINT64_C(0xfff0000000000000), UINT64_C(0x8000000000000000),
          UINT64_C(0xfff0000000000000) },
        { UINT64_C(0x7ff8000000001234), UINT64_C(0x7ff8000000001234),
          UINT64_C(0x7ff8000000001234) },
        { UINT64_C(0x7ff0000000001234), UINT64_C(0x7ff8000000001234),
          UINT64_C(0x7ff8000000001234) }
    };
    static const double sentinel = 1.41421356237309504880;
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CPU c;
        uint32_t esp = prepare_call(&c);
        uint32_t integer_pointer = pointer32(&s_frame[12]);
        unsigned before = *calls;
        double input = double_from_bits(cases[i].input);
        double poison = double_from_bits(UINT64_C(0x7ff800000000dead));

        memcpy(&s_frame[5], &input, sizeof input);
        s_frame[7] = integer_pointer;
        memcpy(&s_frame[12], &poison, sizeof poison);
        c.st_top = 3;
        *fst(&c, 0) = sentinel;
        g_isaac_vita_crt_errno = 84;
        errno = EACCES;
        if (!isaac_vita_math_import_counted(
                &c, ISAAC_VITA_MATH_MODF_NAME, calls) ||
            *calls != before + 1U || c.fault || c.esp != esp + 4U ||
            c.st_top != 2 ||
            double_bits(*fst(&c, 0)) != cases[i].fraction ||
            double_bits(ldd(integer_pointer)) != cases[i].integer ||
            double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
            g_isaac_vita_crt_errno != 84 || errno != EACCES)
            return 0;
    }
    return 1;
}

static int check_fdclass(unsigned *calls)
{
    static const struct {
        uint32_t bits;
        int32_t result;
    } cases[] = {
        { 0x00000000U, 0 }, { 0x00000001U, -2 },
        { 0x3f800000U, -1 }, { 0x7f800000U, 1 },
        { 0x7fc01234U, 2 }
    };
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CPU c;
        uint32_t esp = prepare_call(&c);
        unsigned before = *calls;

        s_frame[5] = cases[i].bits;
        c.eax = 0xa5a5a5a5U;
        if (!isaac_vita_math_import_counted(
                &c, ISAAC_VITA_MATH_FDCLASS_NAME, calls) ||
            *calls != before + 1U || c.fault || c.esp != esp + 4U ||
            c.eax != (uint32_t)cases[i].result)
            return 0;
    }
    return 1;
}

typedef double (*unary_fn)(double);

typedef struct unary_case {
    const char *name;
    unary_fn operation;
    double safe_input;
    double error_input;
} unary_case;

static int check_unary_case(const unary_case *spec, double input,
                            unsigned *calls)
{
    CPU c;
    CPU snapshot;
    uint32_t esp = prepare_call(&c);
    unsigned before = *calls;
    double expected;
    int expected_errno;
    int saved_errno = errno;
    unsigned i;

    errno = 0;
    expected = spec->operation(input);
    expected_errno = errno;
    errno = saved_errno;

    for (i = 0U; i < sizeof c.x / sizeof c.x[0]; ++i) {
        c.x[i].u32[0] = 0x11110000U + i;
        c.x[i].u32[1] = 0x22220000U + i;
        c.x[i].u32[2] = 0x33330000U + i;
        c.x[i].u32[3] = 0x44440000U + i;
    }
    c.x[0].d[0] = input;
    c.st_top = 5;
    for (i = 0U; i < 8U; ++i)
        c.st[i] = 10.0 + i;
    memcpy(&snapshot, &c, sizeof snapshot);
    g_isaac_vita_crt_errno = 73;
    errno = EACCES;
    if (!isaac_vita_math_import_counted(&c, spec->name, calls) ||
        *calls != before + 1U || c.fault || c.esp != esp + 4U ||
        !equal_double(c.x[0].d[0], expected) ||
        c.x[0].u64[1] != snapshot.x[0].u64[1] ||
        memcmp(&c.x[1], &snapshot.x[1],
               sizeof c.x - sizeof c.x[0]) != 0 ||
        c.st_top != snapshot.st_top ||
        memcmp(c.st, snapshot.st, sizeof c.st) != 0 || errno != EACCES ||
        g_isaac_vita_crt_errno !=
            (expected_errno != 0 ? expected_errno : 73))
        return 0;
    return 1;
}

static int check_precise_unary(unsigned *calls)
{
    static const unary_case cases[] = {
        { ISAAC_VITA_MATH_ACOS_NAME, acos, 0.5, 2.0 },
        { ISAAC_VITA_MATH_SQRT_NAME, sqrt, 4.0, -1.0 },
        { ISAAC_VITA_MATH_SIN_NAME, sin, 0.5, INFINITY },
        { ISAAC_VITA_MATH_LOG_NAME, log, 0.8, -1.0 },
        { ISAAC_VITA_MATH_ASIN_NAME, asin, 0.5, 2.0 },
        { ISAAC_VITA_MATH_ATAN_NAME, atan, 0.5, INFINITY },
        { ISAAC_VITA_MATH_LOG10_NAME, log10, 10.0, -1.0 },
        { ISAAC_VITA_MATH_COS_NAME, cos, 0.5, INFINITY },
        { ISAAC_VITA_MATH_EXP_NAME, exp, 0.5, 1000.0 }
    };
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        if (!check_unary_case(&cases[i], cases[i].safe_input, calls) ||
            !check_unary_case(&cases[i], cases[i].error_input, calls))
            return 0;
    }
    return 1;
}

static int check_pow_case(double base, double exponent, unsigned *calls)
{
    CPU c;
    CPU snapshot;
    uint32_t esp = prepare_call(&c);
    unsigned before = *calls;
    double expected;
    int expected_errno;
    int saved_errno = errno;
    unsigned i;

    errno = 0;
    expected = pow(base, exponent);
    expected_errno = errno;
    errno = saved_errno;
    for (i = 0U; i < sizeof c.x / sizeof c.x[0]; ++i) {
        c.x[i].u32[0] = 0x55550000U + i;
        c.x[i].u32[1] = 0x66660000U + i;
        c.x[i].u32[2] = 0x77770000U + i;
        c.x[i].u32[3] = 0x88880000U + i;
    }
    c.x[0].d[0] = base;
    c.x[1].d[0] = exponent;
    c.st_top = 6;
    for (i = 0U; i < 8U; ++i)
        c.st[i] = 20.0 + i;
    memcpy(&snapshot, &c, sizeof snapshot);
    g_isaac_vita_crt_errno = 74;
    errno = EACCES;
    if (!isaac_vita_math_import_counted(
            &c, ISAAC_VITA_MATH_POW_NAME, calls) ||
        *calls != before + 1U || c.fault || c.esp != esp + 4U ||
        !equal_double(c.x[0].d[0], expected) ||
        c.x[0].u64[1] != snapshot.x[0].u64[1] ||
        memcmp(&c.x[1], &snapshot.x[1],
               sizeof c.x - sizeof c.x[0]) != 0 ||
        c.st_top != snapshot.st_top ||
        memcmp(c.st, snapshot.st, sizeof c.st) != 0 || errno != EACCES ||
        g_isaac_vita_crt_errno !=
            (expected_errno != 0 ? expected_errno : 74))
        return 0;
    return 1;
}

static int check_ci_case(const char *name, double y, double x,
                         double (*operation)(double, double),
                         unsigned *calls)
{
    static const double sentinel = 2.71828182845904523536;
    CPU c;
    uint32_t esp = prepare_call(&c);
    unsigned before = *calls;
    double expected;
    int expected_errno;
    int saved_errno = errno;

    errno = 0;
    expected = operation(y, x);
    expected_errno = errno;
    errno = saved_errno;
    c.st_top = 5;
    *fst(&c, 0) = sentinel;
    fpush(&c, y);
    fpush(&c, x);
    g_isaac_vita_crt_errno = 75;
    errno = EACCES;
    if (!isaac_vita_math_import_counted(&c, name, calls) ||
        *calls != before + 1U || c.fault || c.esp != esp + 4U ||
        c.st_top != 4 || !equal_double(*fst(&c, 0), expected) ||
        double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
        errno != EACCES ||
        g_isaac_vita_crt_errno !=
            (expected_errno != 0 ? expected_errno : 75))
        return 0;
    return 1;
}

static int check_nextafter(unsigned *calls)
{
    static const struct {
        float from;
        float toward;
    } cases[] = {
        { 1.0f, 2.0f },
        { 0.0f, 1.0f }
    };
    static const double sentinel = 1.41421356237309504880;
    unsigned i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        CPU c;
        uint32_t esp = prepare_call(&c);
        unsigned before = *calls;
        float expected;
        int expected_errno;
        int saved_errno = errno;

        errno = 0;
        expected = nextafterf(cases[i].from, cases[i].toward);
        expected_errno = errno;
        errno = saved_errno;
        memcpy(&s_frame[5], &cases[i].from, sizeof cases[i].from);
        memcpy(&s_frame[6], &cases[i].toward, sizeof cases[i].toward);
        c.st_top = 3;
        *fst(&c, 0) = sentinel;
        g_isaac_vita_crt_errno = 76;
        errno = EACCES;
        if (!isaac_vita_math_import_counted(
                &c, ISAAC_VITA_MATH_NEXTAFTERF_NAME, calls) ||
            *calls != before + 1U || c.fault || c.esp != esp + 4U ||
            c.st_top != 2 ||
            float_bits((float)*fst(&c, 0)) != float_bits(expected) ||
            double_bits(*fst(&c, 1)) != double_bits(sentinel) ||
            errno != EACCES ||
            g_isaac_vita_crt_errno !=
                (expected_errno == ERANGE ? ERANGE : 76))
            return 0;
    }
    return 1;
}

static int check_setusermatherr(unsigned *calls)
{
    uint32_t expected =
        (uint32_t)(GUEST_IMAGE_BASE +
                   ISAAC_VITA_MATH_SETUSERMATHERR_CALLBACK_RVA);
    CPU c;
    uint32_t esp = prepare_call(&c);
    unsigned before = *calls;

    s_frame[5] = expected;
    g_isaac_vita_crt_errno = 85;
    errno = EACCES;
    if (!isaac_vita_math_import_counted(
            &c, ISAAC_VITA_MATH_SETUSERMATHERR_NAME, calls) ||
        *calls != before + 1U || c.fault || c.esp != esp + 4U ||
        g_isaac_vita_crt_errno != 85 || errno != EACCES)
        return 0;

    esp = prepare_call(&c);
    before = *calls;
    s_frame[5] = expected + 4U;
    if (!isaac_vita_math_import_counted(
            &c, ISAAC_VITA_MATH_SETUSERMATHERR_NAME, calls) ||
        *calls != before + 1U || !c.fault || c.esp != esp ||
        c.fault_addr != expected + 4U ||
        strcmp(c.fault,
               "__setusermatherr received an unproven callback") != 0)
        return 0;
    return 1;
}

static int check_unknown_handoff(unsigned calls)
{
    CPU c;
    CPU snapshot;
    unsigned before = calls;

    (void)prepare_call(&c);
    c.eax = 0x13579bdfU;
    memcpy(&snapshot, &c, sizeof snapshot);
    return isaac_vita_math_import_counted(
               &c, "api-ms-win-crt-math-l1-1-0.dll!not-a-real-import",
               &calls) == 0 &&
           calls == before && memcmp(&c, &snapshot, sizeof c) == 0;
}

int main(void)
{
    unsigned calls = 0U;

    if ((uintptr_t)s_frame > UINT32_MAX || !check_policy())
        return 1;
    if (!check_rounding(ISAAC_VITA_MATH_CEIL_NAME, -1.25, ceil, &calls) ||
        !check_rounding(ISAAC_VITA_MATH_FLOOR_NAME, -1.25, floor, &calls))
        return 2;
    if (!check_ldexp(&calls) || !check_copysignf(&calls) ||
        !check_roundf(&calls) || !check_modf(&calls))
        return 3;
    if (!check_fdclass(&calls) || !check_precise_unary(&calls) ||
        !check_pow_case(4.0, 2.0, &calls) ||
        !check_pow_case(-2.0, 0.5, &calls))
        return 4;
    if (!check_ci_case(ISAAC_VITA_MATH_CIATAN2_NAME, 1.0, -1.0,
                       atan2, &calls) ||
        !check_ci_case(ISAAC_VITA_MATH_CIFMOD_NAME, 5.5, 2.0,
                       fmod, &calls) ||
        !check_ci_case(ISAAC_VITA_MATH_CIFMOD_NAME, INFINITY, 2.0,
                       fmod, &calls) ||
        !check_nextafter(&calls))
        return 5;
    if (!check_setusermatherr(&calls) || calls != 77U ||
        !check_unknown_handoff(calls))
        return 6;
    return 0;
}
