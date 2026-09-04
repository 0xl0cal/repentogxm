#ifndef ISAAC_HOST_VITA_STEAM_H
#define ISAAC_HOST_VITA_STEAM_H

#include <stddef.h>
#include <stdint.h>

#include "guest.h"

/* First Steam import reached by the measured disabled-Steam boot path.  The
 * selected PE is rebased from 0x00400000 to GUEST_IMAGE_BASE 0x98000000. */
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME \
    "steam_api.dll!SteamAPI_RegisterCallback"
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_RVA     0x006066bcU
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_VA      0x986066bcU
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_CALL_RVA    0x00001deaU
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_RETURN_RVA  0x00001df0U
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_RVA  0x007e815cU
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA   0x987e815cU
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID          0x0000044dU
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_ORDINAL     255U

/* The selected PE has one contiguous 11-slot Steam import family.  Vita
 * deliberately owns the eight disabled-lifecycle exports, with SteamAPI_Init
 * implemented by host_vita_post_com and the other seven here.  The three
 * interface-producing exports remain loud: reaching them after Init returned
 * false is a new platform-policy boundary, not a lifecycle no-op. */
#define ISAAC_VITA_STEAM_CONTEXT_INIT_NAME \
    "steam_api.dll!SteamInternal_ContextInit"
#define ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_RVA       0x006066acU
#define ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_VA        0x986066acU
#define ISAAC_VITA_STEAM_CONTEXT_INIT_CALL_RVA      0x004ad6b5U
#define ISAAC_VITA_STEAM_CONTEXT_INIT_RETURN_RVA    0x004ad6bbU
#define ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_RVA     0x007aa3c8U
#define ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA      0x987aa3c8U
#define ISAAC_VITA_STEAM_CONTEXT_CALLBACK_RVA       0x000189a0U
#define ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA        0x980189a0U
#define ISAAC_VITA_STEAM_CONTEXT_GENERATION         0U
#define ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT        21U
/* Complete frozen-PE census.  Register loads are IAT references, not calls;
 * the physical count is the duplicate-free union of direct and unique
 * register-call RVAs.  The current translated corpus contains only 69 direct
 * calls, 22 loads and 61 register calls because sub_00595df0 and
 * sub_005a7ba0 are absent; that 130-site subset is not the PE denominator. */
#define ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_COUNT  71U
#define ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_FNV64 \
    UINT64_C(0xc6d821ad60ab1e85)
#define ISAAC_VITA_STEAM_CONTEXT_REGISTER_LOAD_COUNT 24U
#define ISAAC_VITA_STEAM_CONTEXT_REGISTER_LOAD_FNV64 \
    UINT64_C(0x6ca3efba9cdf0795)
#define ISAAC_VITA_STEAM_CONTEXT_REGISTER_CALL_COUNT 67U
#define ISAAC_VITA_STEAM_CONTEXT_REGISTER_CALL_FNV64 \
    UINT64_C(0x34aa83223fda02c7)
#define ISAAC_VITA_STEAM_CONTEXT_PHYSICAL_CALL_COUNT 138U
#define ISAAC_VITA_STEAM_CONTEXT_PHYSICAL_CALL_FNV64 \
    UINT64_C(0x10c1bf5a3766bc87)

#define ISAAC_VITA_STEAM_GET_PIPE_NAME \
    "steam_api.dll!SteamAPI_GetHSteamPipe"
#define ISAAC_VITA_STEAM_GET_PIPE_IAT_RVA           0x006066b0U
#define ISAAC_VITA_STEAM_GET_PIPE_CALL_RVA          0x00018a32U
#define ISAAC_VITA_STEAM_GET_PIPE_RETURN_RVA        0x00018a38U
#define ISAAC_VITA_STEAM_GET_PIPE_DIRECT_CALL_COUNT 2U

#define ISAAC_VITA_STEAM_GET_USER_NAME \
    "steam_api.dll!SteamAPI_GetHSteamUser"
#define ISAAC_VITA_STEAM_GET_USER_IAT_RVA           0x006066b4U
#define ISAAC_VITA_STEAM_GET_USER_CALL_RVA          0x00018a55U
#define ISAAC_VITA_STEAM_GET_USER_RETURN_RVA        0x00018a5bU
#define ISAAC_VITA_STEAM_GET_USER_DIRECT_CALL_COUNT 1U

#define ISAAC_VITA_STEAM_CREATE_INTERFACE_NAME \
    "steam_api.dll!SteamInternal_CreateInterface"
#define ISAAC_VITA_STEAM_CREATE_INTERFACE_IAT_RVA   0x006066b8U
#define ISAAC_VITA_STEAM_CREATE_INTERFACE_CALL_RVA  0x00018a72U
#define ISAAC_VITA_STEAM_CREATE_INTERFACE_RETURN_RVA 0x00018a78U
#define ISAAC_VITA_STEAM_CREATE_INTERFACE_DIRECT_CALL_COUNT 1U

#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_NAME \
    "steam_api.dll!SteamAPI_RegisterCallResult"
#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_IAT_RVA 0x006066c0U
#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_CALL_RVA 0x003f808bU
#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_RETURN_RVA 0x003f8091U
#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_COUNT 9U
#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_FNV64 \
    UINT64_C(0xa29fad5a3632dfaa)
#define ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_SITES(X) \
    X(0x003f808bU) X(0x0059581eU) X(0x005958e4U) \
    X(0x005959abU) X(0x00596250U) X(0x005962a0U) \
    X(0x005a799dU) X(0x005a7a55U) X(0x005a7b0eU)

#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_NAME \
    "steam_api.dll!SteamAPI_UnregisterCallResult"
#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_IAT_RVA 0x006066c4U
#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_CALL_RVA 0x003f8055U
#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_RETURN_RVA 0x003f805bU
#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_COUNT 9U
#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_FNV64 \
    UINT64_C(0x6a2504d524123382)
#define ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_SITES(X) \
    X(0x003f8055U) X(0x005957f8U) X(0x005958beU) \
    X(0x00595985U) X(0x00596225U) X(0x00596275U) \
    X(0x005a7977U) X(0x005a7a2fU) X(0x005a7ae8U)

#define ISAAC_VITA_STEAM_RUN_CALLBACKS_NAME \
    "steam_api.dll!SteamAPI_RunCallbacks"
#define ISAAC_VITA_STEAM_RUN_CALLBACKS_IAT_RVA      0x006066c8U
#define ISAAC_VITA_STEAM_RUN_CALLBACKS_CALL_RVA     0x00598d9eU
#define ISAAC_VITA_STEAM_RUN_CALLBACKS_RETURN_RVA   0x00598da4U
#define ISAAC_VITA_STEAM_RUN_CALLBACKS_DIRECT_CALL_COUNT 1U
#define ISAAC_VITA_STEAM_RUN_CALLBACKS_DIRECT_CALL_FNV64 \
    UINT64_C(0x15bf825fe4442605)

#define ISAAC_VITA_STEAM_SHUTDOWN_NAME \
    "steam_api.dll!SteamAPI_Shutdown"
#define ISAAC_VITA_STEAM_SHUTDOWN_IAT_RVA           0x006066ccU
#define ISAAC_VITA_STEAM_SHUTDOWN_CALL_RVA          0x00598e91U
#define ISAAC_VITA_STEAM_SHUTDOWN_RETURN_RVA        0x00598e97U
#define ISAAC_VITA_STEAM_SHUTDOWN_DIRECT_CALL_COUNT 1U
#define ISAAC_VITA_STEAM_SHUTDOWN_DIRECT_CALL_FNV64 \
    UINT64_C(0xfdddf7251ff62067)

#define ISAAC_VITA_STEAM_INIT_NAME \
    "steam_api.dll!SteamAPI_Init"
#define ISAAC_VITA_STEAM_INIT_IAT_RVA               0x006066d0U
#define ISAAC_VITA_STEAM_INIT_CALL_RVA              0x00598c80U
#define ISAAC_VITA_STEAM_INIT_RETURN_RVA            0x00598c86U
#define ISAAC_VITA_STEAM_INIT_DIRECT_CALL_COUNT     1U
#define ISAAC_VITA_STEAM_INIT_DIRECT_CALL_FNV64 \
    UINT64_C(0x383880acf298c250)

#define ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_NAME \
    "steam_api.dll!SteamAPI_UnregisterCallback"
#define ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_IAT_RVA 0x006066d4U
#define ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_CALL_RVA 0x0025e05dU
#define ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_RETURN_RVA 0x0025e063U
#define ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_DIRECT_CALL_COUNT 3U
#define ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_DIRECT_CALL_FNV64 \
    UINT64_C(0xe66813e289f7462f)

#define ISAAC_VITA_STEAM_FAMILY_FIRST_IAT_RVA \
    ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_RVA
#define ISAAC_VITA_STEAM_FAMILY_LAST_IAT_RVA \
    ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_IAT_RVA
#define ISAAC_VITA_STEAM_FAMILY_SLOT_COUNT          11U
#define ISAAC_VITA_STEAM_FAMILY_HANDLED_COUNT        8U
#define ISAAC_VITA_STEAM_FAMILY_LOCAL_COUNT          7U
#define ISAAC_VITA_STEAM_FAMILY_EXTERNAL_COUNT       1U
#define ISAAC_VITA_STEAM_FAMILY_LOUD_COUNT           3U

#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_DIRECT_CALL_COUNT 1U
#define ISAAC_VITA_STEAM_REGISTER_CALLBACK_DIRECT_CALL_FNV64 \
    UINT64_C(0x8e68c5084f3572e0)

/* RegisterCallback is cdecl.  Its constructor immediately registers one CRT
 * exit callback, then the six remaining initializer-table entries do the
 * same.  Thus seven already-supported _crt_atexit calls (#256..#262) follow
 * before the outer _initterm returns.  The table layout and first callback
 * are pinned so "seven" remains PE evidence rather than a remembered count. */
#define ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_NAME \
    "api-ms-win-crt-runtime-l1-1-0.dll!_crt_atexit"
#define ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_IAT_RVA       0x006065a0U
#define ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_RVA      0x005eb05bU
#define ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_RETURN_RVA    0x005eb060U
#define ISAAC_VITA_STEAM_POST_REGISTER_FIRST_CALLBACK_VA    0x98605a10U
#define ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_COUNT    7U
#define ISAAC_VITA_STEAM_POST_REGISTER_FIRST_ORDINAL        256U
#define ISAAC_VITA_STEAM_POST_REGISTER_LAST_ORDINAL         262U
#define ISAAC_VITA_STEAM_INIT_TABLE_CURRENT_ENTRY_RVA       0x00606874U
#define ISAAC_VITA_STEAM_INIT_TABLE_CURRENT_FUNCTION_RVA    0x00001de0U
#define ISAAC_VITA_STEAM_INIT_TABLE_LAST_ENTRY_RVA          0x0060688cU
#define ISAAC_VITA_STEAM_INIT_TABLE_LAST_FUNCTION_RVA       0x00001e50U
#define ISAAC_VITA_STEAM_INIT_TABLE_TERMINATOR_RVA          0x00606890U

/* Exact first import after the outer _initterm returns.  The intervening TLS
 * initializer range is empty and its destructor pointer is zero in this PE. */
#define ISAAC_VITA_STEAM_NEXT_NAME \
    "api-ms-win-crt-runtime-l1-1-0.dll!_get_initial_narrow_environment"
#define ISAAC_VITA_STEAM_NEXT_IAT_RVA       0x00606574U
#define ISAAC_VITA_STEAM_NEXT_IAT_VA        0x98606574U
#define ISAAC_VITA_STEAM_NEXT_CALL_RVA      0x005eb79aU
#define ISAAC_VITA_STEAM_NEXT_RETURN_RVA    0x005eb79fU
#define ISAAC_VITA_STEAM_NEXT_ORDINAL       263U

#define ISAAC_VITA_STEAM_IMPORT_COUNT       7U
/* Historical pre-main denominator: only RegisterCallback is reached here.
 * ContextInit belongs to the much later post-archive configuration path. */
#define ISAAC_VITA_STEAM_BOOT_CALL_COUNT    1U
#define ISAAC_VITA_STEAM_CALLBACK_CAPACITY 16U

int isaac_vita_steam_import(CPU *__restrict c, const char *name);
int isaac_vita_steam_import_counted(CPU *__restrict c, const char *name,
                                    unsigned *call_count);

/* Steam is unavailable on Vita: this registry records guest listener
 * lifecycle only.  Nothing here claims SteamAPI_Init succeeded or dispatches
 * a callback.  Unregister/clear are shared with the later lifecycle imports. */
size_t isaac_vita_steam_callback_count(void);
int isaac_vita_steam_callback_lookup(uint32_t object, int32_t *callback_id);
int isaac_vita_steam_callback_unregister(uint32_t object);
void isaac_vita_steam_callback_clear(void);

#endif
