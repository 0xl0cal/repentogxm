#ifndef ISAAC_HOST_VITA_FIND_H
#define ISAAC_HOST_VITA_FIND_H

#include <stdint.h>

#include "guest.h"

/* Exact import slots in the selected x86 PE.  FindFirstFileW was the first
 * content-dependent call after the historical 345-call Vita prefix. */
#define ISAAC_VITA_FIND_FIRST_NAME \
    "KERNEL32.dll!FindFirstFileW"
#define ISAAC_VITA_FIND_FIRST_IAT_RVA        0x006060ecU
#define ISAAC_VITA_FIND_FIRST_CALL_RVA       0x00563036U
#define ISAAC_VITA_FIND_FIRST_RETURN_RVA     0x0056303cU
#define ISAAC_VITA_FIND_FIRST_ATTEMPT_ORDINAL 346U
#define ISAAC_VITA_FIND_FIRST_PATTERN        \
    "ux0:data/isaacr001\\*"

#define ISAAC_VITA_FIND_NEXT_NAME \
    "KERNEL32.dll!FindNextFileW"
#define ISAAC_VITA_FIND_NEXT_IAT_RVA         0x006060e4U
#define ISAAC_VITA_FIND_NEXT_CALL_RVA        0x005632d8U
#define ISAAC_VITA_FIND_NEXT_RETURN_RVA      0x005632daU

#define ISAAC_VITA_FIND_CLOSE_NAME \
    "KERNEL32.dll!FindClose"
#define ISAAC_VITA_FIND_CLOSE_IAT_RVA        0x006060e0U
#define ISAAC_VITA_FIND_CLOSE_FAILURE_CALL_RVA   0x00563071U
#define ISAAC_VITA_FIND_CLOSE_FAILURE_RETURN_RVA 0x00563077U
#define ISAAC_VITA_FIND_CLOSE_CALL_RVA           0x00563488U
#define ISAAC_VITA_FIND_CLOSE_RETURN_RVA         0x0056348eU
#define ISAAC_VITA_FIND_CLOSE_SAFETY_CALL_RVA    0x005634b4U
#define ISAAC_VITA_FIND_CLOSE_SAFETY_RETURN_RVA  0x005634baU

/* x86 WIN32_FIND_DATAW has no host-pointer-sized fields. */
#define ISAAC_VITA_FIND_DATA_ATTRIBUTES_OFFSET  0U
#define ISAAC_VITA_FIND_DATA_CREATION_OFFSET    4U
#define ISAAC_VITA_FIND_DATA_ACCESS_OFFSET      12U
#define ISAAC_VITA_FIND_DATA_WRITE_OFFSET       20U
#define ISAAC_VITA_FIND_DATA_SIZE_HIGH_OFFSET   28U
#define ISAAC_VITA_FIND_DATA_SIZE_LOW_OFFSET    32U
#define ISAAC_VITA_FIND_DATA_RESERVED0_OFFSET   36U
#define ISAAC_VITA_FIND_DATA_RESERVED1_OFFSET   40U
#define ISAAC_VITA_FIND_DATA_NAME_OFFSET        44U
#define ISAAC_VITA_FIND_DATA_ALT_NAME_OFFSET    564U
#define ISAAC_VITA_FIND_DATA_NAME_UNITS         260U
#define ISAAC_VITA_FIND_DATA_ALT_NAME_UNITS     14U
#define ISAAC_VITA_FIND_DATA_SIZE               592U

#define ISAAC_VITA_FIND_FILE_ATTRIBUTE_DIRECTORY 0x00000010U
#define ISAAC_VITA_FIND_FILE_ATTRIBUTE_ARCHIVE   0x00000020U
#define ISAAC_VITA_FIND_INVALID_HANDLE_VALUE      UINT32_MAX

/* Native sceIoDopen missing-path result and the Win32 ERROR_* values exposed
 * through CPU.last_error/GetLastError. */
#define ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND     0x80010002U
#define ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY 0x80010014U
#define ISAAC_VITA_FIND_ERROR_FILE_NOT_FOUND            2U
#define ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND            3U
#define ISAAC_VITA_FIND_ERROR_INVALID_HANDLE            6U
#define ISAAC_VITA_FIND_ERROR_NO_MORE_FILES            18U

/* Native directory descriptors never escape to the x86 guest.  The guest
 * sees bounded, generation-tagged tokens from this registry instead. */
#define ISAAC_VITA_FIND_HANDLE_CAPACITY 16U
#define ISAAC_VITA_FIND_IMPORT_COUNT     3U

/* Expected Win32 failures publish CPU.last_error: a missing directory is
 * PATH_NOT_FOUND, an opened search with no match is FILE_NOT_FOUND, exhausted
 * iteration is NO_MORE_FILES, and stale tokens are INVALID_HANDLE.  Success
 * preserves the previous value, as Win32 requires.  Unexpected Sony I/O
 * errors remain loud rather than being collapsed into EOF/FALSE. */
#define ISAAC_VITA_FIND_HAS_LAST_ERROR             1U
#define ISAAC_VITA_FIND_DREAD_NEGATIVE_IS_EOF      0U
#define ISAAC_VITA_FIND_NATIVE_ERROR_IS_LOUD       1U
#define ISAAC_VITA_FIND_STALE_TOKEN_IS_LOUD        0U
#define ISAAC_VITA_FIND_REGISTRY_IS_LOCKED          1U
#define ISAAC_VITA_FIND_SYNTHETIC_DOT_ENTRIES        0U
#define ISAAC_VITA_FIND_EXACT_PATH_USES_STAT          1U

/* Returns one only for the three exact import names above. */
int isaac_vita_find_import(CPU *__restrict c, const char *name);

/* A recognized name increments call_count before entering its handler, so a
 * policy/range fault remains visible across production guest_fault unwind. */
int isaac_vita_find_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count);

#endif
