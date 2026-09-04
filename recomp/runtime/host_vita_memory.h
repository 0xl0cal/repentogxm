#ifndef ISAAC_HOST_VITA_MEMORY_H
#define ISAAC_HOST_VITA_MEMORY_H

#include <stdint.h>

#include "guest.h"

/* Exact VCRUNTIME entries from the selected PE's sorted guest import table.
 * CALL/RETURN pairs below are real translated call sites that also pin the
 * x86 cdecl ABI.  STRCHR is called through the IAT directly; the other five
 * representative sites use the listed one-instruction import thunks. */
#define ISAAC_VITA_MEMORY_STRCHR_NAME "VCRUNTIME140.dll!strchr"
#define ISAAC_VITA_MEMORY_STRCHR_IAT_RVA     0x00606470U
#define ISAAC_VITA_MEMORY_STRCHR_CALL_RVA    0x0056b837U
#define ISAAC_VITA_MEMORY_STRCHR_RETURN_RVA  0x0056b83dU

#define ISAAC_VITA_MEMORY_MEMCHR_NAME "VCRUNTIME140.dll!memchr"
#define ISAAC_VITA_MEMORY_MEMCHR_IAT_RVA     0x0060647cU
#define ISAAC_VITA_MEMORY_MEMCHR_THUNK_RVA   0x005ec15eU
#define ISAAC_VITA_MEMORY_MEMCHR_CALL_RVA    0x0010c407U
#define ISAAC_VITA_MEMORY_MEMCHR_RETURN_RVA  0x0010c40cU

#define ISAAC_VITA_MEMORY_MEMSET_NAME "VCRUNTIME140.dll!memset"
#define ISAAC_VITA_MEMORY_MEMSET_IAT_RVA     0x00606484U
#define ISAAC_VITA_MEMORY_MEMSET_IAT_VA      0x98606484U
#define ISAAC_VITA_MEMORY_MEMSET_THUNK_RVA   0x005ec152U
#define ISAAC_VITA_MEMORY_MEMSET_CALL_RVA    0x005e8fafU
#define ISAAC_VITA_MEMORY_MEMSET_RETURN_RVA  0x005e8fb4U
#define ISAAC_VITA_MEMORY_MEMSET_LIVE_DST    0x987a9e34U
#define ISAAC_VITA_MEMORY_MEMSET_LIVE_VALUE  0U
#define ISAAC_VITA_MEMORY_MEMSET_LIVE_SIZE   0x78U

#define ISAAC_VITA_MEMORY_MEMCPY_NAME "VCRUNTIME140.dll!memcpy"
#define ISAAC_VITA_MEMORY_MEMCPY_IAT_RVA     0x00606488U
#define ISAAC_VITA_MEMORY_MEMCPY_THUNK_RVA   0x005ec14cU
#define ISAAC_VITA_MEMORY_MEMCPY_CALL_RVA    0x000038cdU
#define ISAAC_VITA_MEMORY_MEMCPY_RETURN_RVA  0x000038d2U

#define ISAAC_VITA_MEMORY_STRSTR_NAME "VCRUNTIME140.dll!strstr"
#define ISAAC_VITA_MEMORY_STRSTR_IAT_RVA     0x00606490U
#define ISAAC_VITA_MEMORY_STRSTR_THUNK_RVA   0x005ec146U
#define ISAAC_VITA_MEMORY_STRSTR_CALL_RVA    0x005e3973U
#define ISAAC_VITA_MEMORY_STRSTR_RETURN_RVA  0x005e3978U

#define ISAAC_VITA_MEMORY_MEMMOVE_NAME "VCRUNTIME140.dll!memmove"
#define ISAAC_VITA_MEMORY_MEMMOVE_IAT_RVA     0x00606494U
#define ISAAC_VITA_MEMORY_MEMMOVE_THUNK_RVA   0x005ec358U
#define ISAAC_VITA_MEMORY_MEMMOVE_CALL_RVA    0x00007ca7U
#define ISAAC_VITA_MEMORY_MEMMOVE_RETURN_RVA  0x00007cacU

#define ISAAC_VITA_MEMORY_IMPORT_COUNT 6U
/* Three memset calls have been reached before the final frozen frontier.  The
 * other five exact handlers remain proactive and outside its denominator. */
#define ISAAC_VITA_MEMORY_EARLY_BOOT_CALL_COUNT 1U
#define ISAAC_VITA_MEMORY_BOOT_CALL_COUNT 3U
/* After the absent USERENV procedure path, std::string copies the empty
 * profile buffer through the exact MEMMOVE site above even though n == 0. */
#define ISAAC_VITA_MEMORY_USERENV_MEMMOVE_BOOT_CALL_COUNT 1U

/* Returns one only for the six exact names above.  A rejected name leaves the
 * CPU and count untouched so the outer dispatcher can emit its exact fault. */
int isaac_vita_memory_import(CPU *__restrict c, const char *name);

/* The production dispatcher must make a recognized call observable before
 * entering its handler: guest_fault may longjmp without returning here. */
int isaac_vita_memory_import_counted(CPU *__restrict c, const char *name,
                                     unsigned *call_count);

#endif
