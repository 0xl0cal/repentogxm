/* Evidence-bounded Vita adapters for the proven x86 UCRT math ABIs. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_crt.h"
#include "host_vita_import_id.h"
#include "host_vita_math.h"

typedef void (*vita_math_handler)(CPU *__restrict c);

typedef struct vita_math_import_entry {
    const char *name;
    vita_math_handler handler;
} vita_math_import_entry;

typedef double (*vita_math_unary_double_fn)(double);
typedef double (*vita_math_binary_double_fn)(double, double);

static uint64_t vita_math_double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double vita_math_double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float vita_math_float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t vita_math_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_math_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

/* UCRT x86 passes binary64 ceil/floor arguments on the cdecl stack and
 * returns binary64 in x87 ST(0).  Copy the established PC boundary exactly. */
static void vita_math_ceil(CPU *__restrict c)
{
    fpush(c, ceil(ldd(guest_stack_address(c, c->esp + 4U, 8U, 0U))));
    vita_math_cdecl_return(c);
}

/* All four PE sites pass a cdecl binary64 followed by a signed int and consume
 * the binary64 result from ST(0).  Measured UCRT policy is deliberately not
 * newlib's generic errno policy: only finite overflow publishes ERANGE;
 * subnormal and zero underflow leave the guest errno cell unchanged. */
static void vita_math_ldexp(CPU *__restrict c)
{
    uint64_t input_bits = ld64(
        guest_stack_address(c, c->esp + 4U, 8U, 0U));
    uint64_t magnitude = input_bits & UINT64_C(0x7fffffffffffffff);
    double input = vita_math_double_from_bits(input_bits);
    double result;
    uint64_t result_bits;
    int saved_errno = errno;

    if (magnitude >= UINT64_C(0x7ff0000000000000)) {
        /* UCRT preserves infinities and quiet NaNs, and quiets an sNaN while
         * preserving its sign and payload. */
        result_bits = input_bits;
        if (magnitude > UINT64_C(0x7ff0000000000000))
            result_bits |= UINT64_C(0x0008000000000000);
        result = vita_math_double_from_bits(result_bits);
    } else {
        errno = 0;
        result = ldexp(input, (int32_t)vita_math_arg(c, 2U));
        errno = saved_errno;
        result_bits = vita_math_double_bits(result);
        if (magnitude != 0U &&
            (result_bits & UINT64_C(0x7fffffffffffffff)) ==
                UINT64_C(0x7ff0000000000000))
            g_isaac_vita_crt_errno = ERANGE;
    }
    errno = saved_errno;
    fpush(c, result);
    vita_math_cdecl_return(c);
}

static void vita_math_floor(CPU *__restrict c)
{
    fpush(c, floor(ldd(guest_stack_address(c, c->esp + 4U, 8U, 0U))));
    vita_math_cdecl_return(c);
}

/* The sole PE site passes two cdecl binary32 values and stores the x87 return
 * as binary32.  UCRT copies the sign bit, preserves a qNaN payload and quiets
 * an sNaN magnitude without publishing errno. */
static void vita_math_copysignf(CPU *__restrict c)
{
    uint32_t magnitude = vita_math_arg(c, 0U) & 0x7fffffffU;
    uint32_t result_bits;

    if (magnitude > 0x7f800000U)
        magnitude |= 0x00400000U;
    result_bits = magnitude | (vita_math_arg(c, 1U) & 0x80000000U);
    fpush(c, (double)vita_math_float_from_bits(result_bits));
    vita_math_cdecl_return(c);
}

/* `_fdclass(float)` returns a signed short classification through AX.  The
 * complete EAX is sign-extended as in the PC adapter so stale upper bits can
 * never leak into a generated caller that snapshots the full register. */
static void vita_math_fdclass(CPU *__restrict c)
{
    uint32_t bits = vita_math_arg(c, 0U);
    uint32_t magnitude = bits & 0x7fffffffU;
    int16_t classification;

    if (magnitude > 0x7f800000U)
        classification = 2;       /* FP_NAN */
    else if (magnitude == 0x7f800000U)
        classification = 1;       /* FP_INFINITE */
    else if (magnitude >= 0x00800000U)
        classification = -1;      /* FP_NORMAL */
    else if (magnitude != 0U)
        classification = -2;      /* FP_SUBNORMAL */
    else
        classification = 0;       /* FP_ZERO */

    c->eax = (uint32_t)(int32_t)classification;
    vita_math_cdecl_return(c);
}

/* `/fp:precise` helpers are register ABI leaves: unary input/output is the
 * low binary64 lane of XMM0; pow reads XMM0/XMM1 and returns through XMM0.
 * Keep native errno private and mirror only the operation's math errno into
 * the shared guest UCRT cell. */
static void vita_math_sse2_unary(CPU *__restrict c,
                                 vita_math_unary_double_fn operation)
{
    int saved_errno = errno;
    int math_errno;
    double result;

    errno = 0;
    result = operation(c->x[0].d[0]);
    math_errno = errno;
    errno = saved_errno;
    c->x[0].d[0] = result;
    if (math_errno != 0)
        g_isaac_vita_crt_errno = math_errno;
    vita_math_cdecl_return(c);
}

static void vita_math_acos(CPU *__restrict c)
{
    vita_math_sse2_unary(c, acos);
}

static void vita_math_asin(CPU *__restrict c)
{
    vita_math_sse2_unary(c, asin);
}

static void vita_math_atan(CPU *__restrict c)
{
    vita_math_sse2_unary(c, atan);
}

static void vita_math_cos(CPU *__restrict c)
{
    vita_math_sse2_unary(c, cos);
}

static void vita_math_exp(CPU *__restrict c)
{
    vita_math_sse2_unary(c, exp);
}

static void vita_math_log10(CPU *__restrict c)
{
    vita_math_sse2_unary(c, log10);
}

static void vita_math_log(CPU *__restrict c)
{
    vita_math_sse2_unary(c, log);
}

static void vita_math_sin(CPU *__restrict c)
{
    vita_math_sse2_unary(c, sin);
}

static void vita_math_sqrt(CPU *__restrict c)
{
    vita_math_sse2_unary(c, sqrt);
}

static void vita_math_pow(CPU *__restrict c)
{
    int saved_errno = errno;
    int math_errno;
    double result;

    errno = 0;
    result = pow(c->x[0].d[0], c->x[1].d[0]);
    math_errno = errno;
    errno = saved_errno;
    c->x[0].d[0] = result;
    if (math_errno != 0)
        g_isaac_vita_crt_errno = math_errno;
    vita_math_cdecl_return(c);
}

/* Both thunk callers pass one cdecl binary32 and consume an x87 binary32
 * result.  Implement the UCRT/C round-away-from-zero rule in the binary32
 * representation so it is independent of the host rounding mode and libc. */
static void vita_math_roundf(CPU *__restrict c)
{
    uint32_t input_bits = vita_math_arg(c, 0U);
    uint32_t sign = input_bits & 0x80000000U;
    uint32_t magnitude = input_bits & 0x7fffffffU;
    uint32_t exponent_bits = magnitude & 0x7f800000U;

    if (exponent_bits == 0x7f800000U) {
        if ((magnitude & 0x007fffffU) != 0U)
            magnitude |= 0x00400000U;
    } else {
        int exponent = (int)(exponent_bits >> 23) - 127;
        if (exponent < -1) {
            magnitude = 0U;
        } else if (exponent == -1) {
            magnitude = 0x3f800000U;
        } else if (exponent < 23) {
            uint32_t fractional_mask = 0x007fffffU >> exponent;
            if ((magnitude & fractional_mask) != 0U) {
                magnitude += 1U << (22 - exponent);
                magnitude &= ~fractional_mask;
            }
        }
    }
    fpush(c, (double)vita_math_float_from_bits(sign | magnitude));
    vita_math_cdecl_return(c);
}

/* The sole call passes cdecl (double, guest double*) and consumes the signed
 * fractional result from ST(0).  Split finite values without calling libc;
 * this also freezes UCRT's infinity, signed-zero and NaN-payload policy. */
static void vita_math_modf(CPU *__restrict c)
{
    uint64_t input_bits = ld64(
        guest_stack_address(c, c->esp + 4U, 8U, 0U));
    uint64_t sign = input_bits & UINT64_C(0x8000000000000000);
    uint64_t magnitude = input_bits & UINT64_C(0x7fffffffffffffff);
    uint64_t exponent_bits = magnitude & UINT64_C(0x7ff0000000000000);
    uint64_t integer_bits;
    uint64_t fraction_bits;

    if (exponent_bits == UINT64_C(0x7ff0000000000000)) {
        if ((magnitude & UINT64_C(0x000fffffffffffff)) != 0U) {
            input_bits |= UINT64_C(0x0008000000000000);
            integer_bits = input_bits;
            fraction_bits = input_bits;
        } else {
            integer_bits = input_bits;
            fraction_bits = sign;
        }
    } else {
        int exponent = (int)(exponent_bits >> 52) - 1023;
        if (exponent < 0) {
            integer_bits = sign;
            fraction_bits = input_bits;
        } else if (exponent >= 52) {
            integer_bits = input_bits;
            fraction_bits = sign;
        } else {
            uint64_t fractional_mask =
                (UINT64_C(1) << (52 - exponent)) - 1U;
            integer_bits = input_bits & ~fractional_mask;
            if ((input_bits & fractional_mask) == 0U) {
                fraction_bits = sign;
            } else {
                fraction_bits = vita_math_double_bits(
                    vita_math_double_from_bits(input_bits) -
                    vita_math_double_from_bits(integer_bits));
            }
        }
    }
    std_(vita_math_arg(c, 2U), vita_math_double_from_bits(integer_bits));
    fpush(c, vita_math_double_from_bits(fraction_bits));
    vita_math_cdecl_return(c);
}

/* MSVC `_CI*` binary helpers receive x in ST(0), y in ST(1), pop both and
 * compute op(y,x).  One result replaces them at ST(0); there are no stack
 * arguments. */
static void vita_math_ci_binary(CPU *__restrict c,
                                vita_math_binary_double_fn operation)
{
    int saved_errno = errno;
    int math_errno;
    double x = fpop(c);
    double y = fpop(c);
    double result;

    errno = 0;
    result = operation(y, x);
    math_errno = errno;
    errno = saved_errno;
    if (math_errno != 0)
        g_isaac_vita_crt_errno = math_errno;
    fpush(c, result);
    vita_math_cdecl_return(c);
}

static void vita_math_ciatan2(CPU *__restrict c)
{
    vita_math_ci_binary(c, atan2);
}

static void vita_math_cifmod(CPU *__restrict c)
{
    vita_math_ci_binary(c, fmod);
}

/* The sole measured nextafterf caller passes two binary32 cdecl arguments and
 * consumes a binary32 value from x87 ST(0). */
static void vita_math_nextafterf(CPU *__restrict c)
{
    int saved_errno = errno;
    int math_errno;
    float result;

    errno = 0;
    result = nextafterf(
        ldf(guest_stack_address(c, c->esp + 4U, 4U, 0U)),
        ldf(guest_stack_address(c, c->esp + 8U, 4U, 0U)));
    math_errno = errno;
    errno = saved_errno;
    if (math_errno == ERANGE) {
        g_isaac_vita_crt_errno = ERANGE;
    } else if (math_errno != 0) {
        guest_fault(c, (uint32_t)math_errno,
                    "nextafterf produced an unexpected UCRT math error");
        return;
    }
    fpush(c, (double)result);
    vita_math_cdecl_return(c);
}

/* The one startup call registers guest RVA 0x542a20.  Frozen PE bytes there
 * are exactly `33 c0 c3` (`xor eax,eax; ret`): it cannot inspect or mutate the UCRT
 * exception record and always requests default handling.  Accepting exactly
 * that callback is therefore behaviorally equivalent to registration for
 * every later math error while an unexpected target still faults loudly. */
static void vita_math_setusermatherr(CPU *__restrict c)
{
    uint32_t callback = vita_math_arg(c, 0U);
    uint32_t expected =
        (uint32_t)(GUEST_IMAGE_BASE +
                   ISAAC_VITA_MATH_SETUSERMATHERR_CALLBACK_RVA);

    if (callback != expected) {
        guest_fault(c, callback,
                    "__setusermatherr received an unproven callback");
        return;
    }
    vita_math_cdecl_return(c);
}

#define ISAAC_VITA_MATH_TABLE_ENTRY(                                  \
    id, name, iat_rva, thunk_rva, call_count, call_hash)              \
    { name, vita_math_##id },
static const vita_math_import_entry s_vita_math_imports[] = {
    ISAAC_VITA_MATH_PROVEN_IMPORTS(ISAAC_VITA_MATH_TABLE_ENTRY)
};
#undef ISAAC_VITA_MATH_TABLE_ENTRY

_Static_assert(sizeof s_vita_math_imports / sizeof s_vita_math_imports[0] ==
                   ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT,
               "Vita proven math import inventory drifted");
_Static_assert((ISAAC_VITA_MATH_FAMILY_LAST_IAT_RVA -
                ISAAC_VITA_MATH_FAMILY_FIRST_IAT_RVA) / 4U + 1U ==
                   ISAAC_VITA_MATH_FAMILY_SLOT_COUNT,
               "Vita math IAT range is no longer 21 contiguous slots");
_Static_assert(ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT +
                   ISAAC_VITA_MATH_LOUD_IMPORT_COUNT ==
                   ISAAC_VITA_MATH_FAMILY_SLOT_COUNT,
               "Vita math 21/21 policy drifted");
_Static_assert(ISAAC_VITA_MATH_PROVEN_PHYSICAL_CALL_COUNT +
                   ISAAC_VITA_MATH_LOUD_PHYSICAL_CALL_COUNT ==
                   ISAAC_VITA_MATH_PHYSICAL_CALL_COUNT,
               "Vita math physical-call denominator drifted");
_Static_assert(ISAAC_VITA_MATH_CEIL_CALL_COUNT +
                   ISAAC_VITA_MATH_LDEXP_CALL_COUNT +
                   ISAAC_VITA_MATH_FLOOR_CALL_COUNT +
                   ISAAC_VITA_MATH_COPYSIGNF_CALL_COUNT +
                   ISAAC_VITA_MATH_FDCLASS_CALL_COUNT +
                   ISAAC_VITA_MATH_ACOS_CALL_COUNT +
                   ISAAC_VITA_MATH_CIFMOD_CALL_COUNT +
                   ISAAC_VITA_MATH_SQRT_CALL_COUNT +
                   ISAAC_VITA_MATH_SIN_CALL_COUNT +
                   ISAAC_VITA_MATH_POW_CALL_COUNT +
                   ISAAC_VITA_MATH_ROUNDF_CALL_COUNT +
                   ISAAC_VITA_MATH_LOG_CALL_COUNT +
                   ISAAC_VITA_MATH_MODF_CALL_COUNT +
                   ISAAC_VITA_MATH_NEXTAFTERF_CALL_COUNT +
                   ISAAC_VITA_MATH_CIATAN2_CALL_COUNT +
                   ISAAC_VITA_MATH_ASIN_CALL_COUNT +
                   ISAAC_VITA_MATH_ATAN_CALL_COUNT +
                   ISAAC_VITA_MATH_SETUSERMATHERR_CALL_COUNT +
                   ISAAC_VITA_MATH_LOG10_CALL_COUNT +
                   ISAAC_VITA_MATH_COS_CALL_COUNT +
                   ISAAC_VITA_MATH_EXP_CALL_COUNT ==
                   ISAAC_VITA_MATH_PROVEN_PHYSICAL_CALL_COUNT,
               "Vita proven math physical census drifted");
_Static_assert(ISAAC_VITA_MATH_LOUD_IMPORT_COUNT == 0U &&
                   ISAAC_VITA_MATH_LOUD_PHYSICAL_CALL_COUNT == 0U,
               "Vita math imports unexpectedly became loud again");
_Static_assert(ISAAC_VITA_MATH_FLOOR_CALL_COUNT +
                   ISAAC_VITA_MATH_SIN_CALL_COUNT +
                   ISAAC_VITA_MATH_POW_CALL_COUNT +
                   ISAAC_VITA_MATH_NEXTAFTERF_CALL_COUNT +
                   ISAAC_VITA_MATH_COS_CALL_COUNT ==
                   ISAAC_VITA_MATH_NORMAL_HIT_PHYSICAL_CALL_COUNT,
               "Vita normal-hit math denominator drifted");

const char *isaac_vita_math_import_name(uint32_t index)
{
    return index < ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT
        ? s_vita_math_imports[index].name : NULL;
}

int isaac_vita_math_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    if (index >= ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_math_imports[index].handler(c);
    return 1;
}

static int vita_math_dispatch(CPU *__restrict c, const char *name,
                              unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_MATH_PROVEN_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_math_imports[index].name) == 0)
            return isaac_vita_math_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_math_import(CPU *__restrict c, const char *name)
{
    return vita_math_dispatch(c, name, NULL);
}

int isaac_vita_math_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    return vita_math_dispatch(c, name, call_count);
}
