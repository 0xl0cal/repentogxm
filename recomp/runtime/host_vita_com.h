#ifndef ISAAC_HOST_VITA_COM_H
#define ISAAC_HOST_VITA_COM_H

#include "guest.h"

/* The selected PE imports only apartment lifecycle, never COM object
 * creation or marshaling.  Keep the same target-local policy as host_win32. */
#define ISAAC_VITA_COM_INITIALIZE_NAME "ole32.dll!CoInitialize"
#define ISAAC_VITA_COM_INITIALIZE_IAT_RVA     0x006066a0U
#define ISAAC_VITA_COM_INITIALIZE_CALL_RVA    0x0048bcddU
#define ISAAC_VITA_COM_INITIALIZE_RETURN_RVA  0x0048bce3U
#define ISAAC_VITA_COM_INITIALIZE_ORDINAL     308U

#define ISAAC_VITA_COM_INITIALIZE_EX_NAME "ole32.dll!CoInitializeEx"
#define ISAAC_VITA_COM_INITIALIZE_EX_IAT_RVA     0x0060669cU
#define ISAAC_VITA_COM_INITIALIZE_EX_CALL0_RVA   0x0059ca29U
#define ISAAC_VITA_COM_INITIALIZE_EX_RETURN0_RVA 0x0059ca2fU
#define ISAAC_VITA_COM_INITIALIZE_EX_CALL1_RVA   0x0059ca3fU
#define ISAAC_VITA_COM_INITIALIZE_EX_RETURN1_RVA 0x0059ca45U

#define ISAAC_VITA_COM_UNINITIALIZE_NAME "ole32.dll!CoUninitialize"
#define ISAAC_VITA_COM_UNINITIALIZE_IAT_RVA     0x006066a4U
#define ISAAC_VITA_COM_UNINITIALIZE_CALL_RVA    0x0048bec5U
#define ISAAC_VITA_COM_UNINITIALIZE_RETURN_RVA  0x0048becbU

#define ISAAC_VITA_COM_IMPORT_COUNT 3U
#define ISAAC_VITA_COM_BOOT_CALL_COUNT 1U

int isaac_vita_com_import(CPU *__restrict c, const char *name);
int isaac_vita_com_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count);

#endif
