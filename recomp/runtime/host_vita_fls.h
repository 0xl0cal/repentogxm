#ifndef ISAAC_HOST_VITA_FLS_H
#define ISAAC_HOST_VITA_FLS_H

#include <stdint.h>

#include "guest.h"

/* Exact direct-IAT KERNEL32 FLS imports in the selected PE.  This binary does
 * not import FlsGetValue: RVA 0x00606074 is GetCurrentProcessorNumber. */
#define ISAAC_VITA_FLS_FREE_NAME "KERNEL32.dll!FlsFree"
#define ISAAC_VITA_FLS_FREE_IAT_RVA     0x00606078U
#define ISAAC_VITA_FLS_FREE_IAT_VA      0x98606078U
#define ISAAC_VITA_FLS_FREE_CALL_RVA    0x005e6e9fU
#define ISAAC_VITA_FLS_FREE_RETURN_RVA  0x005e6ea5U

#define ISAAC_VITA_FLS_SETVALUE_NAME "KERNEL32.dll!FlsSetValue"
#define ISAAC_VITA_FLS_SETVALUE_IAT_RVA      0x0060607cU
#define ISAAC_VITA_FLS_SETVALUE_IAT_VA       0x9860607cU
#define ISAAC_VITA_FLS_SETVALUE_CALL_0_RVA   0x005e6bfeU
#define ISAAC_VITA_FLS_SETVALUE_RETURN_0_RVA 0x005e6c04U
#define ISAAC_VITA_FLS_SETVALUE_CALL_1_RVA   0x005e6c62U
#define ISAAC_VITA_FLS_SETVALUE_RETURN_1_RVA 0x005e6c68U
#define ISAAC_VITA_FLS_SETVALUE_CALL_2_RVA   0x005e6d23U
#define ISAAC_VITA_FLS_SETVALUE_RETURN_2_RVA 0x005e6d29U
#define ISAAC_VITA_FLS_SETVALUE_CALL_3_RVA   0x005e6f30U
#define ISAAC_VITA_FLS_SETVALUE_RETURN_3_RVA 0x005e6f36U
#define ISAAC_VITA_FLS_SETVALUE_CALL_4_RVA   0x005e6fdaU
#define ISAAC_VITA_FLS_SETVALUE_RETURN_4_RVA 0x005e6fe0U
#define ISAAC_VITA_FLS_SETVALUE_CALL_5_RVA   0x005e7211U
#define ISAAC_VITA_FLS_SETVALUE_RETURN_5_RVA 0x005e7217U
#define ISAAC_VITA_FLS_SETVALUE_CALL_6_RVA   0x005e7234U
#define ISAAC_VITA_FLS_SETVALUE_RETURN_6_RVA 0x005e723aU

#define ISAAC_VITA_FLS_ALLOC_NAME "KERNEL32.dll!FlsAlloc"
#define ISAAC_VITA_FLS_ALLOC_IAT_RVA      0x00606080U
#define ISAAC_VITA_FLS_ALLOC_IAT_VA       0x98606080U
#define ISAAC_VITA_FLS_ALLOC_CALL_0_RVA   0x005e6bd3U
#define ISAAC_VITA_FLS_ALLOC_RETURN_0_RVA 0x005e6bd9U
#define ISAAC_VITA_FLS_ALLOC_CALL_1_RVA   0x005e6f05U
#define ISAAC_VITA_FLS_ALLOC_RETURN_1_RVA 0x005e6f0bU

#define ISAAC_VITA_FLS_IMPORT_COUNT 3U
#define ISAAC_VITA_FLS_PHYSICAL_CALLSITE_COUNT 10U
#define ISAAC_VITA_FLS_BOOT_CALL_COUNT 3U
#define ISAAC_VITA_FLS_SLOT_COUNT 128U
#define ISAAC_VITA_FLS_OUT_OF_INDEXES UINT32_MAX

/* Frozen live boot arguments and order: Alloc(callback), Set(index, value),
 * then later Set(index, 0). */
#define ISAAC_VITA_FLS_BOOT_CALLBACK_VA   0x985e6e00U
#define ISAAC_VITA_FLS_BOOT_FIRST_VALUE   0x987a9880U
#define ISAAC_VITA_FLS_CALLBACK_SENTINEL  0xfff15a11U

/* Returns one only for the three exact names above.  A rejected name leaves
 * both the CPU and the optional count untouched. */
int isaac_vita_fls_import(CPU *__restrict c, const char *name);

/* A recognized call is counted before its handler.  This is required because
 * FlsFree may enter translated guest code whose fault unwinds nonlocally. */
int isaac_vita_fls_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count);

#endif
