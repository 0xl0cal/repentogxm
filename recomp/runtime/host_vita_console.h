#ifndef ISAAC_HOST_VITA_CONSOLE_H
#define ISAAC_HOST_VITA_CONSOLE_H

#include <stdint.h>

#include "guest.h"

#define ISAAC_VITA_CONSOLE_GET_STD_HANDLE_NAME \
    "KERNEL32.dll!GetStdHandle"
#define ISAAC_VITA_CONSOLE_GET_STD_HANDLE_IAT_RVA     0x00606098U
#define ISAAC_VITA_CONSOLE_GET_STD_HANDLE_CALL_RVA    0x005e348bU
#define ISAAC_VITA_CONSOLE_GET_STD_HANDLE_RETURN_RVA  0x005e3491U
#define ISAAC_VITA_CONSOLE_STD_ERROR_ARGUMENT          UINT32_C(0xfffffff4)

#define ISAAC_VITA_CONSOLE_WRITE_NAME "KERNEL32.dll!WriteConsoleA"
#define ISAAC_VITA_CONSOLE_WRITE_IAT_RVA     0x00606090U
#define ISAAC_VITA_CONSOLE_WRITE_CALL_RVA    0x005e34cbU
#define ISAAC_VITA_CONSOLE_WRITE_RETURN_RVA  0x005e34d1U
#define ISAAC_VITA_CONSOLE_WRITE_LIVE_BUFFER 0x987edd88U

#define ISAAC_VITA_CONSOLE_NEXT_NAME \
    "KERNEL32.dll!GetEnvironmentVariableA"
#define ISAAC_VITA_CONSOLE_NEXT_IAT_RVA     0x00606094U
#define ISAAC_VITA_CONSOLE_NEXT_CALL_RVA    0x005e3849U
#define ISAAC_VITA_CONSOLE_NEXT_RETURN_RVA  0x005e384fU

/* Opaque to the guest: stable, non-null, and deliberately not a native UID. */
#define ISAAC_VITA_CONSOLE_STDERR_TOKEN 0x49534345U
#define ISAAC_VITA_CONSOLE_LOG_CHUNK     240U
#define ISAAC_VITA_CONSOLE_MAX_WRITE     0x00010000U
#define ISAAC_VITA_CONSOLE_IMPORT_COUNT  2U
/* Current BSS makes the measured WriteConsoleA call conditional and skipped. */
#define ISAAC_VITA_CONSOLE_BOOT_CALL_COUNT 1U

int isaac_vita_console_import(CPU *__restrict c, const char *name);
int isaac_vita_console_import_counted(CPU *__restrict c, const char *name,
                                      unsigned *call_count);

#endif
