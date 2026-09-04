#ifndef ISAAC_HOST_VITA_SYNC_H
#define ISAAC_HOST_VITA_SYNC_H

#include <stdint.h>

#include "guest.h"

/* Six measured calls after the portable CRT batch.  Repeated imports remain
 * separate records because their argument/result order is the evidence. */
#define ISAAC_VITA_SYNC_INIT_CS_NAME \
    "KERNEL32.dll!InitializeCriticalSectionAndSpinCount"
#define ISAAC_VITA_SYNC_INIT_CS_IAT_RVA     0x0060606cU
#define ISAAC_VITA_SYNC_INIT_CS_CALL_RVA    0x005eb16fU
#define ISAAC_VITA_SYNC_INIT_CS_RETURN_RVA  0x005eb175U
#define ISAAC_VITA_SYNC_BOOT_CS_RVA         0x007fd2ccU
#define ISAAC_VITA_SYNC_BOOT_CS_SPIN        4000U

/* The normal initializer table later creates 32 more recursive critical
 * sections, all through the first live site at 0x00562db2.  The selected PE
 * contains seven direct sites in total.  The first live attempt is call #105;
 * all 32 succeed before Steam callback registration at call #255. */
#define ISAAC_VITA_SYNC_PLAIN_CS_NAME \
    "KERNEL32.dll!InitializeCriticalSection"
#define ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA          0x006060f4U
#define ISAAC_VITA_SYNC_PLAIN_CS_IAT_VA           0x986060f4U
#define ISAAC_VITA_SYNC_PLAIN_CS_FIRST_CALL_RVA   0x00562db2U
#define ISAAC_VITA_SYNC_PLAIN_CS_FIRST_RETURN_RVA 0x00562db8U
#define ISAAC_VITA_SYNC_PLAIN_CS_FIRST_ORDINAL    105U
#define ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT  32U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE_COUNT       7U

#define ISAAC_VITA_SYNC_PLAIN_CS_SITE0_CALL_RVA   0x0055e39cU
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE0_RETURN_RVA 0x0055e3a2U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE1_CALL_RVA   0x00562db2U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE1_RETURN_RVA 0x00562db8U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE2_CALL_RVA   0x00595027U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE2_RETURN_RVA 0x0059502dU
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE3_CALL_RVA   0x00598af7U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE3_RETURN_RVA 0x00598afdU
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE4_CALL_RVA   0x005a9f0cU
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE4_RETURN_RVA 0x005a9f12U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE5_CALL_RVA   0x005aa3b1U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE5_RETURN_RVA 0x005aa3b7U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE6_CALL_RVA   0x005ac119U
#define ISAAC_VITA_SYNC_PLAIN_CS_SITE6_RETURN_RVA 0x005ac11fU

/* The logger reached these lifecycle calls immediately after its dynamically
 * allocated critical section.  They use the same recursive-mutex object
 * created by InitializeCriticalSection above and are void stdcall(1). */
#define ISAAC_VITA_SYNC_ENTER_CS_NAME \
    "KERNEL32.dll!EnterCriticalSection"
#define ISAAC_VITA_SYNC_ENTER_CS_IAT_RVA       0x006060fcU
#define ISAAC_VITA_SYNC_ENTER_CS_CALL_RVA      0x0055e3d3U
#define ISAAC_VITA_SYNC_ENTER_CS_RETURN_RVA    0x0055e3d9U
#define ISAAC_VITA_SYNC_ENTER_CS_FIRST_ORDINAL 315U

#define ISAAC_VITA_SYNC_LEAVE_CS_NAME \
    "KERNEL32.dll!LeaveCriticalSection"
#define ISAAC_VITA_SYNC_LEAVE_CS_IAT_RVA       0x006060f8U
#define ISAAC_VITA_SYNC_LEAVE_CS_CALL_RVA      0x0055e53fU
#define ISAAC_VITA_SYNC_LEAVE_CS_RETURN_RVA    0x0055e545U
#define ISAAC_VITA_SYNC_LEAVE_CS_FIRST_ORDINAL 322U

#define ISAAC_VITA_SYNC_DELETE_CS_NAME \
    "KERNEL32.dll!DeleteCriticalSection"
#define ISAAC_VITA_SYNC_DELETE_CS_IAT_RVA      0x006060f0U

#define ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME \
    "KERNEL32.dll!TryEnterCriticalSection"
#define ISAAC_VITA_SYNC_TRY_ENTER_CS_IAT_RVA       0x00606100U
#define ISAAC_VITA_SYNC_TRY_ENTER_CS_CALL_RVA      0x00562e74U
#define ISAAC_VITA_SYNC_TRY_ENTER_CS_RETURN_RVA    0x00562e7aU
#define ISAAC_VITA_SYNC_TRY_ENTER_CS_REFERENCE_COUNT 1U

#define ISAAC_VITA_SYNC_LATE_NEXT_NAME \
    "steam_api.dll!SteamAPI_RegisterCallback"
#define ISAAC_VITA_SYNC_LATE_NEXT_IAT_RVA       0x006066bcU
#define ISAAC_VITA_SYNC_LATE_NEXT_CALL_RVA      0x00001deaU
#define ISAAC_VITA_SYNC_LATE_NEXT_RETURN_RVA    0x00001df0U
#define ISAAC_VITA_SYNC_LATE_NEXT_OBJECT_VA     0x987e815cU
#define ISAAC_VITA_SYNC_LATE_NEXT_CALLBACK_ID   0x0000044dU
#define ISAAC_VITA_SYNC_LATE_NEXT_ORDINAL       255U

#define ISAAC_VITA_SYNC_GET_MODULE_NAME \
    "KERNEL32.dll!GetModuleHandleW"
#define ISAAC_VITA_SYNC_GET_MODULE_IAT_RVA       0x00606060U
#define ISAAC_VITA_SYNC_GET_APISET_CALL_RVA      0x005eb17aU
#define ISAAC_VITA_SYNC_GET_APISET_RETURN_RVA    0x005eb180U
#define ISAAC_VITA_SYNC_APISET_STRING_RVA        0x006080c8U
#define ISAAC_VITA_SYNC_GET_KERNEL32_CALL_RVA    0x005eb18bU
#define ISAAC_VITA_SYNC_GET_KERNEL32_RETURN_RVA  0x005eb191U
#define ISAAC_VITA_SYNC_KERNEL32_STRING_RVA      0x0060810cU
#define ISAAC_VITA_SYNC_KERNEL32_TOKEN           0x7f000001U

#define ISAAC_VITA_SYNC_GET_PROC_NAME \
    "KERNEL32.dll!GetProcAddress"
#define ISAAC_VITA_SYNC_GET_PROC_IAT_RVA          0x00606118U
#define ISAAC_VITA_SYNC_GET_SLEEP_CALL_RVA        0x005eb19dU
#define ISAAC_VITA_SYNC_GET_SLEEP_RETURN_RVA      0x005eb1a3U
#define ISAAC_VITA_SYNC_SLEEP_STRING_RVA          0x00608128U
#define ISAAC_VITA_SYNC_GET_WAKE_ALL_CALL_RVA     0x005eb1abU
#define ISAAC_VITA_SYNC_GET_WAKE_ALL_RETURN_RVA   0x005eb1b1U
#define ISAAC_VITA_SYNC_WAKE_ALL_STRING_RVA       0x00608144U

/* Save-path setup arrives here after LoadLibraryA("userenv") deliberately
 * returned NULL.  Returning NULL for this one exact procedure preserves the
 * guest's USERPROFILE/getenv fallback without manufacturing an HMODULE. */
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_PROCEDURE \
    "GetUserProfileDirectoryA"
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_CALL_RVA   0x0050a886U
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_RETURN_RVA 0x0050a88cU
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_STRING_RVA 0x0075a988U
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_STRING_VA  0x9875a988U
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE     0U
#define ISAAC_VITA_SYNC_GET_USER_PROFILE_ORDINAL    270U
#define ISAAC_VITA_SYNC_LATE_GET_PROC_CALL_COUNT    1U

#define ISAAC_VITA_SYNC_CREATE_EVENT_NAME \
    "KERNEL32.dll!CreateEventW"
#define ISAAC_VITA_SYNC_CREATE_EVENT_IAT_RVA     0x00606064U
#define ISAAC_VITA_SYNC_CREATE_EVENT_CALL_RVA    0x005eb1ceU
#define ISAAC_VITA_SYNC_CREATE_EVENT_RETURN_RVA  0x005eb1d4U
#define ISAAC_VITA_SYNC_EVENT_STORAGE_RVA        0x007fd2c8U

/* The CRT fallback's process-exit callback closes the sole event created
 * above.  CloseHandle has other whole-PE sites for thread and mapping handles;
 * this frontier owns only that exact event and keeps every other handle loud.
 * The complete frozen-PE census is ten direct calls plus two register calls. */
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME \
    "KERNEL32.dll!CloseHandle"
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_IAT_RVA          0x00606120U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_IAT_VA           0x98606120U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_EXIT_CALLBACK_RVA 0x005eb1e5U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_EXIT_CALL_RVA    0x005eb1faU
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_EXIT_RETURN_RVA  0x005eb200U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_REFERENCE_COUNT  12U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_DIRECT_COUNT     10U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_LOAD_COUNT       2U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_REGISTER_CALL_COUNT 2U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_COUNT       12U
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_REFERENCE_FNV64 \
    UINT64_C(0x0dd4810fd19df2ed)
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_DIRECT_FNV64 \
    UINT64_C(0xa744c140fb2b2e7c)
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_LOAD_FNV64 \
    UINT64_C(0x772f21bc6f332dbc)
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_REGISTER_CALL_FNV64 \
    UINT64_C(0x8ae68c048b9cd30e)
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_FNV64 \
    UINT64_C(0x35252dc51fc0f47b)
#define ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_RETURN_FNV64 \
    UINT64_C(0xea4e5437fb919611)

/* Four later KERNEL32 boundaries already exercised by the PC build.  Keep
 * both the IAT-reference census (direct calls plus register loads) and the
 * physical-call census: GetModuleHandleA has three loads but one loaded ESI
 * is called twice, hence eight references and nine actual calls. */
#define ISAAC_VITA_SYNC_SET_EVENT_NAME \
    "KERNEL32.dll!SetEvent"
#define ISAAC_VITA_SYNC_SET_EVENT_IAT_RVA       0x006060a8U
#define ISAAC_VITA_SYNC_SET_EVENT_CALL_RVA      0x005eb2e3U
#define ISAAC_VITA_SYNC_SET_EVENT_RETURN_RVA    0x005eb2e9U
#define ISAAC_VITA_SYNC_SET_EVENT_REFERENCE_COUNT 1U
#define ISAAC_VITA_SYNC_SET_EVENT_CALL_COUNT      1U
#define ISAAC_VITA_SYNC_SET_EVENT_REFERENCE_FNV64 \
    UINT64_C(0x8cfb95bf53e717ce)
#define ISAAC_VITA_SYNC_SET_EVENT_CALL_FNV64 \
    UINT64_C(0x8cfb95bf53e717ce)
#define ISAAC_VITA_SYNC_SET_EVENT_CALL_RETURN_FNV64 \
    UINT64_C(0xa1b04ad689e2bea3)

#define ISAAC_VITA_SYNC_RESET_EVENT_NAME \
    "KERNEL32.dll!ResetEvent"
#define ISAAC_VITA_SYNC_RESET_EVENT_IAT_RVA       0x006060a0U
#define ISAAC_VITA_SYNC_RESET_EVENT_CALL_RVA      0x005eb2efU
#define ISAAC_VITA_SYNC_RESET_EVENT_RETURN_RVA    0x005eb2f5U
#define ISAAC_VITA_SYNC_RESET_EVENT_REFERENCE_COUNT 1U
#define ISAAC_VITA_SYNC_RESET_EVENT_CALL_COUNT      1U
#define ISAAC_VITA_SYNC_RESET_EVENT_REFERENCE_FNV64 \
    UINT64_C(0x0ccb15e0aca6f0ca)
#define ISAAC_VITA_SYNC_RESET_EVENT_CALL_FNV64 \
    UINT64_C(0x0ccb15e0aca6f0ca)
#define ISAAC_VITA_SYNC_RESET_EVENT_CALL_RETURN_FNV64 \
    UINT64_C(0xb889764cb42087cb)

#define ISAAC_VITA_SYNC_GET_MODULE_A_NAME \
    "KERNEL32.dll!GetModuleHandleA"
#define ISAAC_VITA_SYNC_GET_MODULE_A_IAT_RVA       0x00606110U
#define ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_COUNT 8U
#define ISAAC_VITA_SYNC_GET_MODULE_A_DIRECT_COUNT    5U
#define ISAAC_VITA_SYNC_GET_MODULE_A_LOAD_COUNT      3U
#define ISAAC_VITA_SYNC_GET_MODULE_A_REGISTER_CALL_COUNT 4U
#define ISAAC_VITA_SYNC_GET_MODULE_A_CALL_COUNT      9U
#define ISAAC_VITA_SYNC_GET_MODULE_A_NULL_CALL_COUNT 9U
#define ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_FNV64 \
    UINT64_C(0x7c9088e6e1137f4d)
#define ISAAC_VITA_SYNC_GET_MODULE_A_DIRECT_FNV64 \
    UINT64_C(0x65c473a70f33580c)
#define ISAAC_VITA_SYNC_GET_MODULE_A_LOAD_FNV64 \
    UINT64_C(0xfd6e7c922be4e140)
#define ISAAC_VITA_SYNC_GET_MODULE_A_REGISTER_CALL_FNV64 \
    UINT64_C(0xa7afb9ab401de6be)
#define ISAAC_VITA_SYNC_GET_MODULE_A_CALL_FNV64 \
    UINT64_C(0xf6227e7adceb7eb7)
#define ISAAC_VITA_SYNC_GET_MODULE_A_CALL_RETURN_FNV64 \
    UINT64_C(0xa48b9af43622b593)

#define ISAAC_VITA_SYNC_SLEEP_NAME \
    "KERNEL32.dll!Sleep"
#define ISAAC_VITA_SYNC_SLEEP_IAT_RVA       0x00606138U
#define ISAAC_VITA_SYNC_SLEEP_DIRECT_COUNT    5U
#define ISAAC_VITA_SYNC_SLEEP_LOAD_COUNT      8U
#define ISAAC_VITA_SYNC_SLEEP_REGISTER_CALL_COUNT 8U
#define ISAAC_VITA_SYNC_SLEEP_REFERENCE_COUNT 13U
#define ISAAC_VITA_SYNC_SLEEP_CALL_COUNT      13U
#define ISAAC_VITA_SYNC_SLEEP_REFERENCE_FNV64 \
    UINT64_C(0x2864a3ac2ebb6c8c)
#define ISAAC_VITA_SYNC_SLEEP_DIRECT_FNV64 \
    UINT64_C(0x4537a49f323f4021)
#define ISAAC_VITA_SYNC_SLEEP_LOAD_FNV64 \
    UINT64_C(0x0dc940c6a36f75a0)
#define ISAAC_VITA_SYNC_SLEEP_REGISTER_CALL_FNV64 \
    UINT64_C(0xe110f935af5458c4)
#define ISAAC_VITA_SYNC_SLEEP_CALL_FNV64 \
    UINT64_C(0x0261a1d1397a5930)
#define ISAAC_VITA_SYNC_SLEEP_CALL_RETURN_FNV64 \
    UINT64_C(0x5bfd26f971a968bf)

#define ISAAC_VITA_SYNC_PC_HIT_GAP_IMPORT_COUNT 4U
#define ISAAC_VITA_SYNC_PC_HIT_REFERENCE_COUNT \
    (ISAAC_VITA_SYNC_SET_EVENT_REFERENCE_COUNT + \
     ISAAC_VITA_SYNC_RESET_EVENT_REFERENCE_COUNT + \
     ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_COUNT + \
     ISAAC_VITA_SYNC_SLEEP_REFERENCE_COUNT)
#define ISAAC_VITA_SYNC_PC_HIT_CALL_COUNT \
    (ISAAC_VITA_SYNC_SET_EVENT_CALL_COUNT + \
     ISAAC_VITA_SYNC_RESET_EVENT_CALL_COUNT + \
     ISAAC_VITA_SYNC_GET_MODULE_A_CALL_COUNT + \
     ISAAC_VITA_SYNC_SLEEP_CALL_COUNT)
#define ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE 6U

#define ISAAC_VITA_SYNC_NEXT_NAME \
    "KERNEL32.dll!GetStdHandle"
#define ISAAC_VITA_SYNC_NEXT_IAT_RVA     0x00606098U
#define ISAAC_VITA_SYNC_NEXT_CALL_RVA    0x005e348bU
#define ISAAC_VITA_SYNC_NEXT_RETURN_RVA  0x005e3491U

#define ISAAC_VITA_SYNC_IMPORT_COUNT 14U
#define ISAAC_VITA_SYNC_CALL_COUNT   6U

/* Opaque x86 CRITICAL_SECTION storage.  Nothing outside this module should
 * interpret the six dwords; constants are public so the oracle can freeze the
 * exact 24-byte contract and its write boundaries. */
#define ISAAC_VITA_SYNC_CS_SIZE       24U
#define ISAAC_VITA_SYNC_CS_MAGIC      0x31534356U  /* little-endian "VCS1" */
#define ISAAC_VITA_SYNC_CS_VERSION    2U

int isaac_vita_sync_import(CPU *__restrict c, const char *name);
int isaac_vita_sync_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count);

/* Bind only the production runner's stack-owned CPU.  A failed/non-positive
 * query leaves zero and therefore preserves the old per-operation path. */
void isaac_vita_sync_bind_current_thread(CPU *__restrict c);

/* Lifecycle helpers for the later Enter/Leave/Delete imports.  They do not
 * alter the x86 call frame; zero means they emitted an attributed guest fault. */
int isaac_vita_sync_cs_initialize(CPU *__restrict c, uint32_t address,
                                  uint32_t spin_count);
/* Exact plain InitializeCriticalSection variant.  Its success state matches
 * the zero-spin helper above. */
int isaac_vita_sync_cs_initialize_plain(CPU *__restrict c,
                                        uint32_t address);
int isaac_vita_sync_cs_enter(CPU *__restrict c, uint32_t address);
int isaac_vita_sync_cs_leave(CPU *__restrict c, uint32_t address);
int isaac_vita_sync_cs_delete(CPU *__restrict c, uint32_t address);

#endif
