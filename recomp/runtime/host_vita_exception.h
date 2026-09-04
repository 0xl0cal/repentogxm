#ifndef ISAAC_HOST_VITA_EXCEPTION_H
#define ISAAC_HOST_VITA_EXCEPTION_H

#include <stdint.h>

#include "guest.h"

/* Exact current boot boundary in the selected PE.  The handler is a one-
 * argument KERNEL32 stdcall and receives a guest callback address. */
#define ISAAC_VITA_EXCEPTION_SET_FILTER_NAME \
    "KERNEL32.dll!SetUnhandledExceptionFilter"
#define ISAAC_VITA_EXCEPTION_SET_FILTER_IAT_RVA      0x0060603cU
#define ISAAC_VITA_EXCEPTION_SET_FILTER_IAT_VA       0x9860603cU
#define ISAAC_VITA_EXCEPTION_SET_FILTER_CALL_RVA     0x005ebf7dU
#define ISAAC_VITA_EXCEPTION_SET_FILTER_RETURN_RVA   0x005ebf83U
#define ISAAC_VITA_EXCEPTION_SET_FILTER_CALLBACK_VA  0x985ebf84U
#define ISAAC_VITA_EXCEPTION_SET_FILTER_ORDINAL      68U

/* The measured call immediately after the registration returns. */
#define ISAAC_VITA_EXCEPTION_NEXT_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!_set_new_mode"
#define ISAAC_VITA_EXCEPTION_NEXT_IAT_RVA      0x00606508U
#define ISAAC_VITA_EXCEPTION_NEXT_IAT_VA       0x98606508U
#define ISAAC_VITA_EXCEPTION_NEXT_CALL_RVA     0x005eb6b5U
#define ISAAC_VITA_EXCEPTION_NEXT_RETURN_RVA   0x005eb6baU
#define ISAAC_VITA_EXCEPTION_NEXT_ARGUMENT     0U
#define ISAAC_VITA_EXCEPTION_NEXT_ORDINAL      69U

/* The PE's two `_setjmp3(env, 0)` calls are lowered inline in their generated
 * owner frames.  This is a lifetime requirement, not an ordinary import: a
 * native setjmp inside this dispatcher would leave a dead destination when
 * the dispatcher returned.  Keep the exact companion import and both owner
 * sites visible here so the target oracle pins the complete family. */
#define ISAAC_VITA_EXCEPTION_SETJMP3_NAME \
    "VCRUNTIME140.dll!_setjmp3"
#define ISAAC_VITA_EXCEPTION_SETJMP3_IAT_RVA       0x0060646cU
#define ISAAC_VITA_EXCEPTION_SETJMP3_IAT_VA        0x9860646cU
#define ISAAC_VITA_EXCEPTION_SETJMP3_THUNK_RVA     0x005ec352U
#define ISAAC_VITA_EXCEPTION_SETJMP3_CALL0_RVA     0x005a0f52U
#define ISAAC_VITA_EXCEPTION_SETJMP3_RETURN0_RVA   0x005a0f57U
#define ISAAC_VITA_EXCEPTION_SETJMP3_CALL1_RVA     0x005b1094U
#define ISAAC_VITA_EXCEPTION_SETJMP3_RETURN1_RVA   0x005b1099U

/* `longjmp` is the only dispatched half of that family.  The four frozen
 * callers are PNG error recovery: three in the image loader and one in
 * libpng's fatal-error helper.  The 0x005a12d8 caller is the observed
 * language-cycle failure after a 16 MiB texel allocation returned NULL. */
#define ISAAC_VITA_EXCEPTION_LONGJMP_NAME \
    "VCRUNTIME140.dll!longjmp"
#define ISAAC_VITA_EXCEPTION_LONGJMP_IAT_RVA       0x0060648cU
#define ISAAC_VITA_EXCEPTION_LONGJMP_IAT_VA        0x9860648cU
#define ISAAC_VITA_EXCEPTION_LONGJMP_CALL0_RVA     0x005a1100U
#define ISAAC_VITA_EXCEPTION_LONGJMP_RETURN0_RVA   0x005a1106U
#define ISAAC_VITA_EXCEPTION_LONGJMP_CALL1_RVA     0x005a12d8U
#define ISAAC_VITA_EXCEPTION_LONGJMP_RETURN1_RVA   0x005a12deU
#define ISAAC_VITA_EXCEPTION_LONGJMP_CALL2_RVA     0x005a1552U
#define ISAAC_VITA_EXCEPTION_LONGJMP_RETURN2_RVA   0x005a1558U
#define ISAAC_VITA_EXCEPTION_LONGJMP_CALL3_RVA     0x005c3570U
#define ISAAC_VITA_EXCEPTION_LONGJMP_RETURN3_RVA   0x005c3576U
#define ISAAC_VITA_EXCEPTION_LONGJMP_CALL_COUNT    4U

#define ISAAC_VITA_EXCEPTION_IMPORT_COUNT    2U
#define ISAAC_VITA_EXCEPTION_BOOT_CALL_COUNT 1U
#define ISAAC_VITA_EXCEPTION_STATE_SLOT_COUNT 1U

/* Returns one only for SetUnhandledExceptionFilter.  The recognized longjmp
 * path transfers nonlocally through guest_longjmp and therefore never returns.
 * A rejected name leaves the CPU, process-global filter and optional counter
 * untouched. */
int isaac_vita_exception_import(CPU *__restrict c, const char *name);

/* A recognized call is counted before its handler.  This preserves the
 * dynamic-call denominator if a checked guest-stack read faults nonlocally. */
int isaac_vita_exception_import_counted(CPU *__restrict c, const char *name,
                                        unsigned *call_count);

#endif
