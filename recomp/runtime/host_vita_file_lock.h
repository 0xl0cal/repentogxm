#ifndef ISAAC_HOST_VITA_FILE_LOCK_H
#define ISAAC_HOST_VITA_FILE_LOCK_H

#include "guest.h"

/* The frozen PE has one file-lock bridge.  Its caller first converts a CRT
 * FILE token through _fileno and _get_osfhandle, then releases/replaces the
 * byte-range lock at offset zero.  The Vita implementation deliberately owns
 * only the two KERNEL32 boundaries; CRT token/fd ownership stays in
 * host_vita_crt. */
#define ISAAC_VITA_FILE_LOCK_LOCK_NAME "KERNEL32.dll!LockFileEx"
#define ISAAC_VITA_FILE_LOCK_LOCK_IAT_RVA       0x006060d0U
#define ISAAC_VITA_FILE_LOCK_LOCK_CALL_RVA      0x00596660U
#define ISAAC_VITA_FILE_LOCK_LOCK_RETURN_RVA    0x00596666U

#define ISAAC_VITA_FILE_LOCK_UNLOCK_NAME "KERNEL32.dll!UnlockFileEx"
#define ISAAC_VITA_FILE_LOCK_UNLOCK_IAT_RVA     0x006060d4U
#define ISAAC_VITA_FILE_LOCK_UNLOCK_CALL_RVA    0x00596614U
#define ISAAC_VITA_FILE_LOCK_UNLOCK_RETURN_RVA  0x0059661aU

#define ISAAC_VITA_FILE_LOCK_GET_OSFHANDLE_IAT_RVA    0x006065f0U
#define ISAAC_VITA_FILE_LOCK_GET_OSFHANDLE_CALL_RVA   0x005965dbU
#define ISAAC_VITA_FILE_LOCK_GET_OSFHANDLE_RETURN_RVA 0x005965e1U

#define ISAAC_VITA_FILE_LOCK_IMPORT_COUNT       2U
#define ISAAC_VITA_FILE_LOCK_STATIC_CALL_COUNT  2U

#define ISAAC_VITA_FILE_LOCK_FAIL_IMMEDIATELY   0x00000001U
#define ISAAC_VITA_FILE_LOCK_EXCLUSIVE          0x00000002U
#define ISAAC_VITA_FILE_LOCK_ALLOWED_FLAGS      0x00000003U

#define ISAAC_VITA_FILE_LOCK_ERROR_INVALID_HANDLE    6U
#define ISAAC_VITA_FILE_LOCK_ERROR_INVALID_PARAMETER 87U

int isaac_vita_file_lock_import(CPU *__restrict c, const char *name);
int isaac_vita_file_lock_import_counted(CPU *__restrict c, const char *name,
                                        unsigned *call_count);

#endif
