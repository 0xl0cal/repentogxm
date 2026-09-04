#ifndef ISAAC_VITA_BOOT_IMPORTS_H
#define ISAAC_VITA_BOOT_IMPORTS_H

/* Copied from the frozen PE import table and the measured startup path in
 * sub_005ebfe2.  Keep the implemented import and the next loud frontier in
 * one place so the handler, boundary verdict, and focused ARM oracle cannot
 * silently disagree. */
#define ISAAC_VITA_SYSTEM_TIME_IMPORT_RVA     0x00606084U
#define ISAAC_VITA_SYSTEM_TIME_IMPORT_RETURN  0x005ebffaU
#define ISAAC_VITA_SYSTEM_TIME_IMPORT_NAME \
    "KERNEL32.dll!GetSystemTimeAsFileTime"

#define ISAAC_VITA_THREAD_ID_IMPORT_RVA       0x0060612cU
#define ISAAC_VITA_THREAD_ID_IMPORT_RETURN    0x005ec009U
#define ISAAC_VITA_THREAD_ID_IMPORT_NAME \
    "KERNEL32.dll!GetCurrentThreadId"

#define ISAAC_VITA_PROCESS_ID_IMPORT_RVA      0x00606114U
#define ISAAC_VITA_PROCESS_ID_IMPORT_RETURN   0x005ec012U
#define ISAAC_VITA_PROCESS_ID_IMPORT_NAME \
    "KERNEL32.dll!GetCurrentProcessId"

#define ISAAC_VITA_QPC_IMPORT_RVA             0x006060c4U
#define ISAAC_VITA_QPC_IMPORT_RETURN          0x005ec01fU
#define ISAAC_VITA_QPC_IMPORT_NAME \
    "KERNEL32.dll!QueryPerformanceCounter"
#define ISAAC_VITA_QPC_SEED_CALL_RVA       0x005e8b7aU
#define ISAAC_VITA_QPC_SEED_RETURN_RVA     0x005e8b80U
#define ISAAC_VITA_QPC_SEED_BOOT_ORDINAL   23U

/* Measured after the first four clock/identity calls and before _initterm_e.
 * Values 6 and 10 mirror the PC virtual-CPU policy (SSE and SSE2). */
#define ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_RVA     0x00606030U
#define ISAAC_VITA_PROCESSOR_FEATURE_CALL_RVA       0x005ebaf1U
#define ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_RETURN  0x005ebaf7U
#define ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME \
    "KERNEL32.dll!IsProcessorFeaturePresent"
#define ISAAC_VITA_PROCESSOR_FEATURE_SSE            6U
#define ISAAC_VITA_PROCESSOR_FEATURE_SSE2           10U
#define ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT  10U

#define ISAAC_VITA_FIRST4_NEXT_IMPORT_RVA \
    ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_RVA
#define ISAAC_VITA_FIRST4_NEXT_IMPORT_RETURN \
    ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_RETURN
#define ISAAC_VITA_FIRST4_NEXT_IMPORT_NAME \
    ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME

#define ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT   4U
#define ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_COUNT 1U
/* The initial four calls plus four later QPC samples before the final
 * SetUnhandledExceptionFilter frontier. */
#define ISAAC_VITA_BOOT_BASE_CALL_COUNT 8U

#endif
