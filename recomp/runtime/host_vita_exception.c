/* Process-global exception registration and nonlocal guest recovery for Vita.
 *
 * The guest registers an x86 callback but no SEH bridge exists on Vita yet,
 * so the callback is deliberately not installed into the native process.
 * Windows SetUnhandledExceptionFilter still has useful observable state: it
 * atomically replaces one process-wide pointer and returns the previous one.
 *
 * `_setjmp3` itself is emitted inside each generated owner frame; only the
 * matching VCRUNTIME longjmp crosses this ordinary import dispatcher.  The
 * shared guest runtime owns validation, x86 nonvolatile-register restoration,
 * and the native jump into that still-live owner frame.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_exception.h"
#include "host_vita_import_id.h"

static atomic_uint_least32_t s_vita_unhandled_filter = ATOMIC_VAR_INIT(0U);

static uint32_t vita_exception_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_exception_stdcall_return(CPU *__restrict c,
                                          uint32_t argument_count)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_count * 4U, 0U);
}

static void vita_exception_SetUnhandledExceptionFilter(CPU *__restrict c)
{
    /* Read the complete guest argument before mutating process state.  In a
     * checked build this read can fault and unwind through the dispatcher. */
    uint32_t replacement = vita_exception_arg(c, 0U);
    uint32_t previous = (uint32_t)atomic_exchange_explicit(
        &s_vita_unhandled_filter, replacement, memory_order_acq_rel);

    c->eax = previous;
    vita_exception_stdcall_return(c, 1U);
}

static GUEST_NORETURN void vita_exception_longjmp(CPU *__restrict c)
{
    uint32_t guest_env = vita_exception_arg(c, 0U);
    int32_t value = (int32_t)vita_exception_arg(c, 1U);

    /* This is cdecl but deliberately has no stack cleanup: returning would
     * continue a guest path which guest_longjmp has already unwound. */
    guest_longjmp(c, guest_env, value);
}

const char *isaac_vita_exception_import_name(uint32_t index)
{
    static const char *const names[ISAAC_VITA_EXCEPTION_IMPORT_COUNT] = {
        ISAAC_VITA_EXCEPTION_SET_FILTER_NAME,
        ISAAC_VITA_EXCEPTION_LONGJMP_NAME
    };

    return index < ISAAC_VITA_EXCEPTION_IMPORT_COUNT ? names[index] : NULL;
}

int isaac_vita_exception_import_indexed(CPU *__restrict c, uint32_t index,
                                        unsigned *call_count)
{
    if (index >= ISAAC_VITA_EXCEPTION_IMPORT_COUNT)
        return 0;
    /* Must precede either handler: both may transfer control nonlocally. */
    if (call_count)
        ++*call_count;
    if (index == 0U) {
        vita_exception_SetUnhandledExceptionFilter(c);
        return 1;
    }
    vita_exception_longjmp(c);
}

static int vita_exception_dispatch(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    if (!name)
        return 0;

    if (strcmp(name, ISAAC_VITA_EXCEPTION_SET_FILTER_NAME) == 0) {
        return isaac_vita_exception_import_indexed(c, 0U, call_count);
    }
    if (strcmp(name, ISAAC_VITA_EXCEPTION_LONGJMP_NAME) == 0) {
        return isaac_vita_exception_import_indexed(c, 1U, call_count);
    }
    return 0;
}

int isaac_vita_exception_import(CPU *__restrict c, const char *name)
{
    return vita_exception_dispatch(c, name, NULL);
}

int isaac_vita_exception_import_counted(CPU *__restrict c, const char *name,
                                        unsigned *call_count)
{
    return vita_exception_dispatch(c, name, call_count);
}
