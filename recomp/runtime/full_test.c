/* Link the WHOLE recompiled binary and run it.
 *
 * `ladder_test.c` proved four functions against oracles, but it built them in
 * isolation and defined its own stubs for their callees -- including
 * `sub_0055e330`, the logger. This driver links all 145 generated translation
 * units instead, so:
 *
 *   * linking is itself a test, and a real one: duplicate definitions, missing
 *     symbols and a malformed dispatch table all surface here and nowhere
 *     earlier;
 *   * the oracles now run through the REAL callees. Where the ladder counted
 *     calls to a stub, this executes the function the game actually has, which
 *     is a strictly stronger claim and may well fault -- and a fault here is
 *     information, not a failure of the experiment;
 *   * the dispatch table can be exercised for the first time.
 *
 * Everything is referenced by its canonical `sub_%08x` name. A friendly alias
 * breaks the link, because a callee is referenced by address.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <math.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gl_bridge.h"
#include "guest.h"
#include "manual_kage.h"
#include "manual_portable.h"

#ifndef EXE_PATH
#define EXE_PATH  "isaac-ng.exe.unpacked.exe"
#endif

/* RVAs, not friendly names -- see above. */
#define RVA_NEXT        0x003a7e20u
#define RVA_RANDINT     0x003a7d50u
#define RVA_RANDFLOAT   0x003a7db0u
#define RVA_PICK        0x00089420u
#define RVA_GREED       0x002cba40u
#define RVA_CHAOS       0x00304ad0u
#define RVA_I64_TO_DBL  0x005eb9d0u
#define RVA_U64_TO_DBL  0x005eb970u
#define RVA_F32_TO_U32  0x005eb8a0u
#define RVA_F64_TO_U32  0x005eb8f0u

/* Copied from ladder_test.c, not recalled. The first version of this file had
 * VA_SHIFTS as 0x3076a1a0 -- a mangling of the *scale* address -- and the
 * image guard correctly refused to report anything. */
#define VA_SHIFTS   (GUEST_IMAGE_BASE + 0x608aa0u) /* 81 x 12-byte triples */
#define VA_SCALE    (GUEST_IMAGE_BASE + 0x76a198u) /* 0x2F7FFFFE scale     */
#define VA_TLS_TEMPLATE (GUEST_IMAGE_BASE + 0x77262cu) /* PE TLS raw bytes */
#define VA_TLS_INDEX    (GUEST_IMAGE_BASE + 0x7fd618u) /* AddressOfIndex   */
#define VA_SECURITY_COOKIE (GUEST_IMAGE_BASE + 0x7aa3b4u)

/* A real relocation-backed function pointer in .rdata. In the input PE this
 * slot is 0x00401070; after loading at GUEST_IMAGE_BASE it must be
 * GUEST_IMAGE_BASE + 0x1070. This catches a bug the old raw-RVA probes could
 * not see: the table stores RVAs but actual vtables contain relocated VAs. */
#define RVA_RELOCATED_FN_PTR 0x006066f4u
#define RVA_RELOCATED_FN     0x00001070u
#define RVA_BUFFEREDSTREAM_SLOT0 0x0076787cu
#define RVA_BUFFEREDSTREAM_SLOT1 0x00767880u
#define RVA_BUFFEREDSTREAM_SLOT2 0x00767888u
#define RVA_BUFFEREDSTREAM_ZERO  0x005a9a10u
#define RVA_BUFFEREDSTREAM_DTOR  0x005a9a15u

/* Exact metadata generated from the PE import directory.  The loader must
 * replace this slot's old contents with its relocated slot address, and an
 * attempted call must fault with this exact name rather than an anonymous
 * "untranslated address". */
#define RVA_IAT_LUA_ABSINDEX 0x00606220u
#define NAME_LUA_ABSINDEX    "Lua5.3.3r.dll!lua_absindex"
#define RVA_IAT_SYSTEM_TIME  0x00606084u
#define RVA_IAT_THREAD_ID    0x0060612cu
#define RVA_IAT_PROCESS_ID   0x00606114u
#define RVA_IAT_QPC          0x006060c4u
#define RVA_IAT_QPF          0x006060c8u
#define RVA_IAT_CPU_FEATURE  0x00606030u
#define RVA_IAT_VIRTUAL_UNLOCK 0x00606048u
#define RVA_IAT_LARGE_PAGE_MIN 0x0060604cu
#define RVA_IAT_VIRTUAL_QUERY  0x00606050u
#define RVA_IAT_VIRTUAL_FREE   0x00606054u
#define RVA_IAT_VIRTUAL_ALLOC  0x00606058u
#define RVA_IAT_GET_LAST_ERROR 0x006060dcu
#define RVA_IAT_INITTERM     0x00606580u
#define RVA_IAT_INITTERM_E   0x00606584u
#define RVA_IAT_SET_NEW_MODE 0x00606508u
#define RVA_IAT_FREE         0x006064fcu
#define RVA_IAT_CALLOC       0x00606500u
#define RVA_IAT_MALLOC       0x00606504u
#define RVA_IAT_REALLOC      0x0060650cu
#define RVA_IAT_CEIL         0x0060651cu
#define RVA_IAT_FLOOR        0x00606524u
#define RVA_IAT_FDCLASS      0x0060652cu
#define RVA_IAT_LIBM_SSE2_ACOS_PRECISE 0x00606530u
#define RVA_IAT_LIBM_SSE2_SQRT_PRECISE 0x00606538u
#define RVA_IAT_LIBM_SSE2_SIN_PRECISE 0x0060653cu
#define RVA_IAT_LIBM_SSE2_POW_PRECISE 0x00606540u
#define RVA_IAT_LIBM_SSE2_LOG_PRECISE 0x00606548u
#define RVA_IAT_LIBM_SSE2_ASIN_PRECISE 0x00606558u
#define RVA_IAT_LIBM_SSE2_ATAN_PRECISE 0x0060655cu
#define RVA_IAT_LIBM_SSE2_LOG10_PRECISE 0x00606564u
#define RVA_IAT_LIBM_SSE2_COS_PRECISE 0x00606568u
#define RVA_IAT_LIBM_SSE2_EXP_PRECISE 0x0060656cu
#define RVA_IAT_NEXTAFTERF   0x00606550u
#define RVA_IAT_CIATAN2      0x00606554u
#define RVA_IAT_CIFMOD       0x00606534u
#define RVA_IAT_ATOI         0x006064d8u
#define RVA_IAT_ATOF         0x006064dcu
#define NAME_ATOI "api-ms-win-crt-convert-l1-1-0.dll!atoi"
#define NAME_ATOF "api-ms-win-crt-convert-l1-1-0.dll!atof"
#define RVA_ATOI_ABI_CALL    0x0000a057u
#define RVA_ATOF_ABI_CALL    0x004b279du
#define ATOI_CALL_COUNT      191u
#define ATOF_CALL_COUNT      160u
#define ATOI_CALL_FNV64      0x12df3796e35d8810ULL
#define ATOF_CALL_FNV64      0xdbaef5daea0a3908ULL
#define NAME_CEIL "api-ms-win-crt-math-l1-1-0.dll!ceil"
#define NAME_FLOOR "api-ms-win-crt-math-l1-1-0.dll!floor"
#define NAME_FDCLASS "api-ms-win-crt-math-l1-1-0.dll!_fdclass"
#define NAME_LIBM_SSE2_ACOS_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_acos_precise"
#define NAME_LIBM_SSE2_ASIN_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_asin_precise"
#define NAME_LIBM_SSE2_ATAN_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_atan_precise"
#define NAME_LIBM_SSE2_COS_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_cos_precise"
#define NAME_LIBM_SSE2_EXP_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_exp_precise"
#define NAME_LIBM_SSE2_LOG10_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_log10_precise"
#define NAME_LIBM_SSE2_LOG_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_log_precise"
#define NAME_LIBM_SSE2_SIN_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_sin_precise"
#define NAME_LIBM_SSE2_POW_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_pow_precise"
#define NAME_LIBM_SSE2_SQRT_PRECISE \
    "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_sqrt_precise"
#define NAME_NEXTAFTERF "api-ms-win-crt-math-l1-1-0.dll!nextafterf"
#define NAME_CIATAN2 "api-ms-win-crt-math-l1-1-0.dll!_CIatan2"
#define NAME_CIFMOD "api-ms-win-crt-math-l1-1-0.dll!_CIfmod"
#define RVA_THUNK_CEIL       0x005ec3acu
#define RVA_THUNK_FLOOR      0x005ec3b2u
#define RVA_THUNK_FDCLASS    0x005ec36au
#define RVA_FDCLASS_ABI_WINDOW 0x004c5cd4u
#define FDCLASS_CALL_COUNT   8u
#define FDCLASS_CALL_FNV64   0x585e3d3c1629566cULL
#define RVA_CEIL_ABI_CALL    0x000325b4u
#define RVA_CEIL_DECODE_GAP_CALL 0x00182c30u
#define RVA_FLOOR_STARTUP_CALL 0x0048bd85u
#define RVA_TEXT_START       0x00001000u
#define RVA_TEXT_END         0x00605b34u
#define CEIL_CALL_COUNT      40u
#define FLOOR_CALL_COUNT     438u
#define CEIL_CALL_FNV64      0xb7582eedd2206c4cULL
#define FLOOR_CALL_FNV64     0x9bacdc17ac4fe873ULL
#define RVA_THUNK_LIBM_SSE2_ACOS_PRECISE 0x005ec370u
#define RVA_THUNK_LIBM_SSE2_ASIN_PRECISE 0x005ec376u
#define RVA_THUNK_LIBM_SSE2_ATAN_PRECISE 0x005ec37cu
#define RVA_THUNK_LIBM_SSE2_COS_PRECISE 0x005ec382u
#define RVA_THUNK_LIBM_SSE2_EXP_PRECISE 0x005ec388u
#define RVA_THUNK_LIBM_SSE2_LOG10_PRECISE 0x005ec38eu
#define RVA_THUNK_LIBM_SSE2_LOG_PRECISE 0x005ec394u
#define RVA_THUNK_LIBM_SSE2_POW_PRECISE 0x005ec39au
#define RVA_THUNK_LIBM_SSE2_SIN_PRECISE 0x005ec3a0u
#define RVA_THUNK_LIBM_SSE2_SQRT_PRECISE 0x005ec3a6u
#define RVA_LIBM_ACOS_FLOAT_WRAPPER 0x001ef080u
#define RVA_LIBM_ASIN_FLOAT_WRAPPER 0x0031fdd0u
#define RVA_LIBM_ATAN_FLOAT_CONTEXT 0x004c08adu
#define RVA_LIBM_COS_FLOAT_WRAPPER 0x00011fe0u
#define RVA_LIBM_EXP_FLOAT_WRAPPER 0x0031fdb0u
#define RVA_LIBM_LOG10_FLOAT_CONTEXT 0x0028ef32u
#define RVA_LIBM_LOG_FLOAT_WRAPPER 0x0028f440u
#define RVA_LIBM_SIN_FLOAT_WRAPPER 0x00012010u
#define RVA_LIBM_SQRT_FLOAT_WRAPPER 0x000228c0u
#define RVA_LIBM_POW_LIVE_WRAPPER 0x004a9750u
#define LIBM_SSE2_ACOS_CALL_COUNT 1u
#define LIBM_SSE2_ASIN_CALL_COUNT 1u
#define LIBM_SSE2_ATAN_CALL_COUNT 5u
#define LIBM_SSE2_COS_CALL_COUNT 16u
#define LIBM_SSE2_EXP_CALL_COUNT 2u
#define LIBM_SSE2_LOG10_CALL_COUNT 2u
#define LIBM_SSE2_LOG_CALL_COUNT 3u
#define LIBM_SSE2_POW_CALL_COUNT 35u
#define LIBM_SSE2_SIN_CALL_COUNT 21u
#define LIBM_SSE2_SQRT_CALL_COUNT 7u
#define LIBM_SSE2_ACOS_CALL_FNV64 0x3f0d0eff9f1ce351ULL
#define LIBM_SSE2_ASIN_CALL_FNV64 0x9dd0ec24384973e3ULL
#define LIBM_SSE2_ATAN_CALL_FNV64 0xe0e20053f3544c50ULL
#define LIBM_SSE2_COS_CALL_FNV64 0x72e9ffaa1307cb5cULL
#define LIBM_SSE2_EXP_CALL_FNV64 0xdcd4a003887b665eULL
#define LIBM_SSE2_LOG10_CALL_FNV64 0x29f689a9d0314568ULL
#define LIBM_SSE2_LOG_CALL_FNV64 0x236a303c7e881795ULL
#define LIBM_SSE2_POW_CALL_FNV64 0xca49c4f83f78f520ULL
#define LIBM_SSE2_SIN_CALL_FNV64 0x9c6f7ee551cc2b9bULL
#define LIBM_SSE2_SQRT_CALL_FNV64 0x8a27bd553cc5277aULL
#define RVA_NEXTAFTERF_CALL  0x00003c2au
#define RVA_NEXTAFTERF_WINDOW 0x00003c0fu
#define NEXTAFTERF_CALL_COUNT 1u
#define NEXTAFTERF_CALL_FNV64 0x85e1d9c70ed02f6bULL
#define RVA_THUNK_CIATAN2    0x005ec35eu
#define RVA_THUNK_CIFMOD     0x005ec364u
#define RVA_CIATAN2_FLOAT_WRAPPER 0x00047a80u
#define RVA_CIATAN2_X87_WRAPPER_A 0x000f4307u
#define RVA_CIATAN2_X87_WRAPPER_B 0x004f7978u
#define RVA_CIFMOD_FLOAT_WRAPPER  0x00047b40u
#define CIATAN2_CALL_COUNT   3u
#define CIFMOD_CALL_COUNT    1u
#define CIATAN2_CALL_FNV64   0xff9c38c9ae56c11dULL
#define CIFMOD_CALL_FNV64    0x2dc77666fec2cefcULL
#define RVA_IAT_STEAM_REGISTER   0x006066bcu
#define RVA_IAT_STEAM_RUN        0x006066c8u
#define RVA_IAT_STEAM_SHUTDOWN   0x006066ccu
#define RVA_IAT_STEAM_INIT       0x006066d0u
#define RVA_IAT_STEAM_UNREGISTER 0x006066d4u
#define RVA_IAT_STEAM_CONTEXT_INIT 0x006066acu
#define RVA_IAT_STEAM_REGISTER_CALL_RESULT 0x006066c0u
#define RVA_IAT_STEAM_UNREGISTER_CALL_RESULT 0x006066c4u
#define NAME_STEAM_CONTEXT_INIT "steam_api.dll!SteamInternal_ContextInit"
#define NAME_STEAM_REGISTER_CALL_RESULT \
    "steam_api.dll!SteamAPI_RegisterCallResult"
#define NAME_STEAM_UNREGISTER_CALL_RESULT \
    "steam_api.dll!SteamAPI_UnregisterCallResult"
#define RVA_STEAM_CONTEXT_CALL   0x004ad6b5u
#define VA_STEAM_CONTEXT_DESCRIPTOR (GUEST_IMAGE_BASE + 0x007aa3c8u)
#define VA_STEAM_CONTEXT_CALLBACK   (GUEST_IMAGE_BASE + 0x000189a0u)
#define STEAM_CONTEXT_DWORDS     21u
#define STEAM_CALL_RESULT_CALL_COUNT 9u
#define STEAM_REGISTER_CALL_RESULT_FNV64 0xa29fad5a3632dfaaULL
#define STEAM_UNREGISTER_CALL_RESULT_FNV64 0x6a2504d524123382ULL
#define RVA_IAT_MEMCPY       0x00606488u
#define RVA_IAT_MEMMOVE      0x00606494u
#define RVA_IAT_RTDYNAMICCAST 0x00606464u
#define NAME_RTDYNAMICCAST "VCRUNTIME140.dll!__RTDynamicCast"
#define RVA_THUNK_RTDYNAMICCAST 0x005ec34cu
#define RTDYNAMICCAST_PHYSICAL_CALL_COUNT 662u
#define RTDYNAMICCAST_PHYSICAL_CALL_FNV64 0x6acddf0a2f8c8863ULL
#define VA_RTTI_GRID_ENTITY_TD (GUEST_IMAGE_BASE + 0x007eac24u)
#define VA_RTTI_GRID_ENTITY_DOOR_TD (GUEST_IMAGE_BASE + 0x007ead38u)
#define VA_RTTI_GRID_ENTITY_DOOR_VFPTR (GUEST_IMAGE_BASE + 0x0074a0a4u)
#define VA_RTTI_GRID_ENTITY_VFPTR (GUEST_IMAGE_BASE + 0x0074a0d4u)
#define RVA_IAT_MEMCHR       0x0060647cu
#define NAME_MEMCHR "VCRUNTIME140.dll!memchr"
#define RVA_IAT_STRCHR       0x00606470u
#define NAME_STRCHR "VCRUNTIME140.dll!strchr"
#define STRCHR_CALL_COUNT 5u
#define STRCHR_CALL_FNV64 0x9431a4b833f8a441ULL
#define RVA_IAT_STRSTR       0x00606490u
#define NAME_STRSTR "VCRUNTIME140.dll!strstr"
#define STRSTR_DIRECT_CALL_COUNT 3u
#define STRSTR_DIRECT_CALL_FNV64 0xac08292cff120de8ULL
#define STRSTR_REFERENCE_COUNT 7u
#define STRSTR_REFERENCE_FNV64 0x2ff682e6e427a9d1ULL
#define STRSTR_ALL_CALL_COUNT 5u
#define STRSTR_ALL_CALL_FNV64 0x2283d2c323df12b3ULL
#define RVA_THUNK_MEMCHR     0x005ec15eu
#define RVA_MEMCHR_STARTUP_WINDOW 0x00144a9fu
#define RVA_MEMCHR_STARTUP_CALL 0x00144aa8u
#define MEMCHR_CALL_COUNT    16u
#define MEMCHR_CALL_FNV64    0x72cc1fb3b8f5dae5ULL
#define RVA_IAT_LONGJMP      0x0060648cu
#define RVA_IAT_MBSTOWCS_S   0x006064ccu
#define RVA_IAT_WCSTOMBS_S   0x006064d0u
#define RVA_IAT_GETENV       0x006064e4u
#define RVA_IAT_REMOVE       0x006064ecu
#define RVA_IAT_ACCESS       0x006064f0u
#define NAME_REMOVE "api-ms-win-crt-filesystem-l1-1-0.dll!remove"
#define NAME_ACCESS "api-ms-win-crt-filesystem-l1-1-0.dll!_access"
#define RVA_REMOVE_ABI_WINDOW 0x00567cc0u
#define RVA_ACCESS_ABI_WINDOW 0x0059633du
#define REMOVE_CALL_COUNT    8u
#define ACCESS_CALL_COUNT    1u
#define REMOVE_CALL_FNV64    0x7450e89437d00438ULL
#define ACCESS_CALL_FNV64    0x7223dd96f4295075ULL
#define RVA_IAT_SLEEP        0x00606138u
#define NAME_SLEEP "KERNEL32.dll!Sleep"
#define SLEEP_DIRECT_CALL_COUNT 5u
#define SLEEP_DIRECT_CALL_FNV64 0x4537a49f323f4021ULL
#define SLEEP_REFERENCE_COUNT 13u
#define SLEEP_REFERENCE_FNV64 0x2864a3ac2ebb6c8cULL
#define RVA_IAT_THRD_YIELD   0x006062e8u
#define NAME_THRD_YIELD "MSVCP140.dll!_Thrd_yield"
#define RVA_THUNK_THRD_YIELD 0x005eacd1u
#define THRD_YIELD_CALL_COUNT 8u
#define THRD_YIELD_CALL_FNV64 0xcef859fd75fd0198ULL
#define RVA_IAT_VSPRINTF     0x00606614u
#define NAME_VSPRINTF \
    "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vsprintf"
#define RVA_WIDTH3_FORMAT           0x0074e7a8u
#define RVA_WIDTH3_CALLER_WINDOW    0x005049acu
#define RVA_WIDTH3_GAME_CALL        0x005049c2u
#define RVA_WIDTH3_GAME_RETURN      0x005049c7u
#define RVA_WIDTH3_SPRINTF_WRAPPER  0x00018980u
#define RVA_WIDTH3_WRAPPER_CALL     0x00018992u
#define RVA_WIDTH3_WRAPPER_RETURN   0x00018997u
#define RVA_WIDTH3_STDIO_ADAPTER    0x00011f70u
#define RVA_WIDTH3_IAT_CALL         0x00011f8eu
#define RVA_WIDTH3_IAT_RETURN       0x00011f94u
#define WIDTH3_FORMAT_FNV64         0xb1d135458ef32825ULL
#define WIDTH3_CALLER_FNV64         0x66b2d8ae35860b6cULL
#define WIDTH3_WRAPPER_FNV64        0x7a370044e2f71d39ULL
#define WIDTH3_ADAPTER_FNV64        0x42ca574a34e8cc5aULL
#define RVA_IAT_VSPRINTF_S   0x00606618u
#define RVA_IAT_VSSCANF      0x00606610u
#define RVA_IAT_FILE_ATTRS   0x00606108u
#define RVA_IAT_CREATE_DIR   0x00606134u
#define RVA_IAT_CURRENT_DIR  0x0060610cu
#define RVA_IAT_FIND_CLOSE   0x006060e0u
#define RVA_IAT_FIND_NEXT_W  0x006060e4u
#define RVA_IAT_FULL_PATH_W  0x006060e8u
#define RVA_IAT_FIND_FIRST_W 0x006060ecu
#define RVA_IAT_LOCK_FILE_EX 0x006060d0u
#define RVA_IAT_UNLOCK_FILE_EX 0x006060d4u
#define RVA_IAT_FILENO       0x006065e0u
#define RVA_IAT_FREAD        0x006065e4u
#define RVA_IAT_FWRITE       0x006065e8u
#define RVA_IAT_FSEEK        0x006065ecu
#define RVA_IAT_GET_OSFHANDLE 0x006065f0u
#define RVA_IAT_FTELL        0x006065f4u
#define RVA_IAT_VFPRINTF     0x006065fcu
#define RVA_IAT_FFLUSH       0x00606600u
#define RVA_IAT_FOPEN        0x00606608u
#define RVA_IAT_FCLOSE       0x0060661cu
#define RVA_IAT_STRDUP       0x00606624u
#define RVA_IAT_STRPBRK      0x00606628u
#define RVA_IAT_ISPUNCT      0x0060662cu
#define RVA_IAT_TOLOWER      0x00606630u
#define RVA_IAT_STRNICMP     0x00606634u
#define RVA_IAT_STRNCPY      0x00606638u
#define RVA_IAT_STRNCMP      0x00606640u
#define RVA_IAT_ISWSPACE     0x00606644u
#define RVA_IAT_STRNCPY_S    0x00606648u
#define RVA_IAT_TOUPPER      0x0060664cu
#define RVA_IAT_STRCAT_S     0x00606650u
#define RVA_IAT_STRCPY_S     0x00606654u
#define RVA_IAT_ISSPACE      0x00606658u
#define RVA_IAT_COINIT_EX    0x0060669cu
#define RVA_IAT_COINIT       0x006066a0u
#define RVA_IAT_COUNINIT     0x006066a4u
#define RVA_IAT_EXEC_STATE   0x0060609cu
#define RVA_IAT_TIME_END     0x006064b4u
#define RVA_IAT_TIME_CAPS    0x006064b8u
#define RVA_IAT_TIME_BEGIN   0x006064bcu
#define RVA_IAT_TIME_GET     0x006064c0u
#define RVA_IAT_SYSTEM_PARAMETERS 0x006063fcu
#define RVA_IAT_GET_SYSTEM_METRICS 0x006063c8u
#define NAME_GET_SYSTEM_METRICS "USER32.dll!GetSystemMetrics"
#define GET_SYSTEM_METRICS_REFERENCE_COUNT 6u
#define GET_SYSTEM_METRICS_REFERENCE_FNV64 0x930064299035cc6dULL
#define GET_SYSTEM_METRICS_DIRECT_COUNT 4u
#define GET_SYSTEM_METRICS_DIRECT_FNV64 0x75869290ef23886dULL
#define GET_SYSTEM_METRICS_CALL_COUNT 10u
#define GET_SYSTEM_METRICS_CALL_FNV64 0x3332eed930aa3203ULL
#define RVA_IAT_LOAD_IMAGE_A 0x006063bcu
#define NAME_LOAD_IMAGE_A "USER32.dll!LoadImageA"
#define LOAD_IMAGE_A_REFERENCE_COUNT 2u
#define LOAD_IMAGE_A_REFERENCE_FNV64 0x50472814e62208d9ULL
#define LOAD_IMAGE_A_CALL_COUNT 3u
#define LOAD_IMAGE_A_CALL_FNV64 0x795e1b7c41f8a24bULL
#define RVA_IAT_SEND_MESSAGE_A 0x006063c0u
#define NAME_SEND_MESSAGE_A "USER32.dll!SendMessageA"
#define SEND_MESSAGE_A_REFERENCE_COUNT 1u
#define SEND_MESSAGE_A_REFERENCE_FNV64 0xa60c72b8d38e0322ULL
#define SEND_MESSAGE_A_CALL_COUNT 2u
#define SEND_MESSAGE_A_CALL_FNV64 0xe4c7dc2ff8a1ac40ULL
#define RVA_IAT_GLOBAL_UNLOCK 0x0060613cu
#define RVA_IAT_GLOBAL_LOCK   0x00606140u
#define RVA_IAT_GLOBAL_ALLOC  0x00606144u
#define RVA_IAT_CLOSE_CLIPBOARD 0x006063d4u
#define RVA_IAT_EMPTY_CLIPBOARD 0x006063d8u
#define RVA_IAT_GET_CLIPBOARD_DATA 0x006063dcu
#define RVA_IAT_OPEN_CLIPBOARD 0x006063d0u
#define RVA_IAT_SET_CLIPBOARD_DATA 0x006063e0u
#define NAME_GLOBAL_UNLOCK "KERNEL32.dll!GlobalUnlock"
#define NAME_GLOBAL_LOCK   "KERNEL32.dll!GlobalLock"
#define NAME_GLOBAL_ALLOC  "KERNEL32.dll!GlobalAlloc"
#define NAME_CLOSE_CLIPBOARD "USER32.dll!CloseClipboard"
#define NAME_EMPTY_CLIPBOARD "USER32.dll!EmptyClipboard"
#define NAME_GET_CLIPBOARD_DATA "USER32.dll!GetClipboardData"
#define NAME_OPEN_CLIPBOARD "USER32.dll!OpenClipboard"
#define NAME_SET_CLIPBOARD_DATA "USER32.dll!SetClipboardData"
#define GLOBAL_UNLOCK_CALL_COUNT 2u
#define GLOBAL_UNLOCK_CALL_FNV64 0x5376e17d69d8c8e3ULL
#define GLOBAL_LOCK_CALL_COUNT 2u
#define GLOBAL_LOCK_CALL_FNV64 0xf17a6d0f6d3d093dULL
#define GLOBAL_ALLOC_CALL_COUNT 1u
#define GLOBAL_ALLOC_CALL_FNV64 0x8a1383f3596069bdULL
#define CLOSE_CLIPBOARD_CALL_COUNT 2u
#define CLOSE_CLIPBOARD_CALL_FNV64 0x4dacac8ae82ad370ULL
#define CLOSE_CLIPBOARD_REFERENCE_COUNT 3u
#define CLOSE_CLIPBOARD_REFERENCE_FNV64 0x16a95cb0fdb75429ULL
#define EMPTY_CLIPBOARD_CALL_COUNT 1u
#define EMPTY_CLIPBOARD_CALL_FNV64 0x58fbe9018b29377eULL
#define GET_CLIPBOARD_DATA_CALL_COUNT 1u
#define GET_CLIPBOARD_DATA_CALL_FNV64 0xddee88198c40073dULL
#define OPEN_CLIPBOARD_CALL_COUNT 2u
#define OPEN_CLIPBOARD_CALL_FNV64 0xec863f82f50f8c09ULL
#define SET_CLIPBOARD_DATA_CALL_COUNT 1u
#define SET_CLIPBOARD_DATA_CALL_FNV64 0x20936337085b9883ULL
#define RVA_IAT_MODULE_HANDLE_A   0x00606110u
#define RVA_IAT_LOAD_ICON_A       0x006063acu
#define RVA_IAT_GET_WINDOW_LONG_A 0x006063b8u
#define RVA_IAT_SET_WINDOW_LONG_A 0x006063ccu
#define RVA_IAT_SET_CLASS_LONG_A  0x00606458u
#define RVA_IAT_ACRT_IOB          0x00606604u
#define RVA_IAT_TLS_EXIT_REGISTER 0x00606578u
#define RVA_IAT__EXIT             0x0060659cu
#define RVA_IAT_CRT_ATEXIT        0x006065a0u
#define RVA_IAT_ERRNO             0x006065a4u
#define RVA_IAT_EXIT              0x006065a8u
#define RVA_IAT_SET_ERRNO         0x006065d8u
#define RVA_IAT_FREE_LIBRARY      0x00606104u
#define RVA_IAT_GET_PROC_ADDRESS  0x00606118u
#define RVA_IAT_LOAD_LIBRARY_A    0x00606124u
#define RVA_IAT_WGL_GET_PROC_ADDRESS 0x006062fcu
#define RVA_IAT_ISDIGIT           0x0060663cu
#define RVA_IAT_STRFTIME          0x00606660u
#define RVA_IAT_GMTIME64          0x00606664u
#define RVA_IAT_TIME64            0x00606668u
#define RVA_IAT_MKGMTIME64        0x0060666cu
#define RVA_IAT_LOCALTIME64       0x00606670u
#define RVA_IAT_QSORT             0x00606678u
#define NAME_QSORT "api-ms-win-crt-utility-l1-1-0.dll!qsort"
#define QSORT_TEST_CALLBACK       0x00f10200u

void sub_003a7e20(CPU *__restrict c);
void sub_003a7d50(CPU *__restrict c);
void sub_003a7db0(CPU *__restrict c);
void sub_005eb9d0(CPU *__restrict c);
void sub_005eb970(CPU *__restrict c);
void sub_005eb8a0(CPU *__restrict c);
void sub_005eb8f0(CPU *__restrict c);
void sub_005ebcb0(CPU *__restrict c);
void sub_005eacd7(CPU *__restrict c);
void sub_00007be0(CPU *__restrict c);
void sub_00007c80(CPU *__restrict c);
void sub_0000f760(CPU *__restrict c);
void guest_register_all(void);
void guest_register_all_imports(void);
extern const uint32_t guest_table_len;
extern const uint32_t guest_import_table_len;
extern unsigned g_fault_count;

/* Win32 x86 MEMORY_BASIC_INFORMATION, copied field-for-field from
 * memoryapi.h.  Keeping the guest layout explicit makes this test reject an
 * accidental native 64-bit build instead of silently checking the wrong ABI. */
typedef struct guest_mbi32 {
    uint32_t base_address;
    uint32_t allocation_base;
    uint32_t allocation_protect;
    uint32_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t type;
} guest_mbi32;
typedef char guest_mbi32_must_be_28_bytes[
    sizeof(guest_mbi32) == 28U ? 1 : -1];

static CPU cpu;
static CPU nested_fault_cpu;
static uint32_t jump_test_cross_stack[64];
static uint32_t guarded_import_token;
static int continued_after_guarded_fault;
static int nested_inner_seen, nested_outer_resumed;
static int init_zero_calls, init_seven_calls, init_after_calls;
static int tls_attach_result;
static int continued_after_checkpoint;
static int continued_after_kage_boundary;
static guest_fn guarded_kage_boundary;
static uint32_t guarded_kage_return;
static uint32_t guarded_exit_token, guarded_exit_code, guarded_exit_return;
static int continued_after_exit;
static unsigned exit_callback_count;
static uint32_t exit_callback_order[3];
static uint32_t tls_exit_args[3];
static uint32_t guarded_scan_token;
static uint32_t guarded_scan_args[7];
static int continued_after_guarded_scan;
static uint32_t guarded_format_token;
static uint32_t guarded_format_args[7];
static int continued_after_guarded_format;
static uint32_t guarded_qsort_token;
static uint32_t guarded_qsort_args[4];
static int continued_after_guarded_qsort;
static uint32_t guarded_numeric_token;
static int continued_after_guarded_numeric;
static unsigned qsort_compare_calls;
static int qsort_compare_bad_pointer;
static uint32_t qsort_compare_base, qsort_compare_end, qsort_compare_width;
static uint32_t jump_test_env, jump_test_token, jump_test_value;
static int jump_test_do_jump, jump_test_cross_cpu;
static int jump_test_initial, jump_test_resumed;
static int jump_test_after_longjmp, jump_test_caller_after;
static int jump_test_stale_after, jump_test_cross_cpu_ok;

#define JUMP_TEST_EBP 0x13572468U
#define JUMP_TEST_EBX 0x24681357U
#define JUMP_TEST_EDI 0x89abcdefU
#define JUMP_TEST_ESI 0xfedcba98U
#define JUMP_TEST_RETURN 0x5a0f57U
#define JUMP_TEST_LONGJMP_RETURN 0xabc0648cU
#define JUMP_TEST_FILL 0xa5a5a5a5U

static guest_gl_addr preserved_glGetString(guest_gl_enum name)
{
    static const char version[] = "preserved installed backend";
    return name == 0x1F02U
        ? (guest_gl_addr)(uintptr_t)version : (guest_gl_addr)0U;
}

static void call_tls_attach(CPU *__restrict c)
{
    tls_attach_result = guest_tls_process_attach(c);
}

static void call_bringup_checkpoint(CPU *__restrict c)
{
    if (!guest_checkpoint(c, 0x00C0FFEEU))
        continued_after_checkpoint = 1;
}

static void call_guarded_kage_boundary(CPU *__restrict c)
{
    c->ecx = GUEST_IMAGE_BASE + 0x007c7a30U; /* exact Manager singleton */
    gpush(c, guarded_kage_return);
    guarded_kage_boundary(c);
    continued_after_kage_boundary = 1;
}

static void exit_callback_a(CPU *__restrict c)
{
    (void)gpop(c);
    exit_callback_order[exit_callback_count++] = 1U;
}

static void exit_callback_b(CPU *__restrict c)
{
    (void)gpop(c);
    exit_callback_order[exit_callback_count++] = 2U;
}

static void tls_exit_callback(CPU *__restrict c)
{
    tls_exit_args[0] = ld32(c->esp + 4U);
    tls_exit_args[1] = ld32(c->esp + 8U);
    tls_exit_args[2] = ld32(c->esp + 12U);
    (void)gpop(c);
    c->esp += 12U;
    exit_callback_order[exit_callback_count++] = 3U;
}

static void call_guarded_exit(CPU *__restrict c)
{
    gpush(c, guarded_exit_code);
    gpush(c, guarded_exit_return);
    guest_call(c, guarded_exit_token);
    continued_after_exit = 1;
}

static void call_guarded_scan(CPU *__restrict c)
{
    unsigned i = 7U;
    while (i) {
        --i;
        gpush(c, guarded_scan_args[i]);
    }
    gpush(c, 0xABC06610U);
    guest_call(c, guarded_scan_token);
    continued_after_guarded_scan = 1;
}

static void call_guarded_format(CPU *__restrict c)
{
    unsigned i = 7U;
    while (i) {
        --i;
        gpush(c, guarded_format_args[i]);
    }
    gpush(c, 0xABC06614U);
    guest_call(c, guarded_format_token);
    continued_after_guarded_format = 1;
}

static void qsort_compare_i32(CPU *__restrict c)
{
    uint32_t left = ld32(c->esp + 4U);
    uint32_t right = ld32(c->esp + 8U);
    int32_t left_key, right_key;

    qsort_compare_calls++;
    if (left < qsort_compare_base || left >= qsort_compare_end ||
        right < qsort_compare_base || right >= qsort_compare_end ||
        (left - qsort_compare_base) % qsort_compare_width != 0U ||
        (right - qsort_compare_base) % qsort_compare_width != 0U)
        qsort_compare_bad_pointer = 1;
    left_key = (int32_t)ld32(left);
    right_key = (int32_t)ld32(right);
    (void)gpop(c);                   /* comparator is cdecl: caller cleans */
    c->eax = (uint32_t)((left_key > right_key) -
                        (left_key < right_key));
}

static void call_guarded_qsort(CPU *__restrict c)
{
    unsigned i = 4U;
    while (i) {
        --i;
        gpush(c, guarded_qsort_args[i]);
    }
    gpush(c, 0xABC06678U);
    guest_call(c, guarded_qsort_token);
    continued_after_guarded_qsort = 1;
}

static void call_guarded_numeric(CPU *__restrict c)
{
    gpush(c, 0U);
    gpush(c, 0xABC064D8U);
    guest_call(c, guarded_numeric_token);
    continued_after_guarded_numeric = 1;
}

static void init_zero(CPU *__restrict c)
{
    (void)gpop(c);
    init_zero_calls++;
    c->eax = 0;
}

static void init_seven(CPU *__restrict c)
{
    (void)gpop(c);
    init_seven_calls++;
    c->eax = 7;
}

static void init_after(CPU *__restrict c)
{
    (void)gpop(c);
    init_after_calls++;
    c->eax = 0;
}

static void call_guarded_import(CPU *__restrict c)
{
    guest_call(c, guarded_import_token);
    continued_after_guarded_fault = 1;
}

static void nested_inner_fault(CPU *__restrict c)
{
    nested_inner_seen++;
    guest_fault(c, 0xF00D0002u, "nested CPU fault");
}

static void nested_outer_fault(CPU *__restrict c)
{
    int stopped = guest_run_until_stop(&nested_fault_cpu, nested_inner_fault);
    if (stopped == GUEST_RUN_FAULT &&
        nested_fault_cpu.fault_addr == 0xF00D0002u)
        nested_outer_resumed++;
    guest_fault(c, 0xF00D0001u, "outer CPU fault");
}

static void jump_test_stale_call(CPU *__restrict c)
{
    gpush(c, jump_test_value);
    gpush(c, jump_test_env);
    gpush(c, 0xABC0648CU);
    guest_call(c, jump_test_token);
    jump_test_stale_after++;
}

static void jump_test_deeper(CPU *__restrict c)
{
    /* These mutations must disappear at longjmp.  ECX/EDX and flags are not
     * checked because the real MSVC x86 jump buffer does not preserve them.
     * Deliberately keep the current owner-frame ESP: the three pushes below
     * put longjmp's return on `_setjmp3`'s dead return slot, exactly as the
     * live libpng fatal path does.  That slot is not part of the saved state. */
    c->ebp = 0xeeee0001U;
    c->ebx = 0xeeee0002U;
    c->edi = 0xeeee0003U;
    c->esi = 0xeeee0004U;
    gpush(c, jump_test_value);
    gpush(c, jump_test_env);
    gpush(c, JUMP_TEST_LONGJMP_RETURN);
    guest_call(c, jump_test_token);
    jump_test_after_longjmp++;       /* a correct longjmp never reaches this */
}

static void jump_test_owner(CPU *__restrict c)
{
    guest_jump_site site;

    c->ebp = JUMP_TEST_EBP;
    c->ebx = JUMP_TEST_EBX;
    c->edi = JUMP_TEST_EDI;
    c->esi = JUMP_TEST_ESI;
    gpush(c, 0U);                    /* `_setjmp3` count */
    gpush(c, jump_test_env);
    gpush(c, JUMP_TEST_RETURN);
    guest_setjmp_prepare(c, &site);
    /* Native setjmp must be a controlling expression in this owner frame.
     * Hiding it in a helper would save the helper's already-dead frame. */
    if (setjmp(site.native_env) == 0) {
        guest_setjmp_finish(c, &site, 0);
        jump_test_initial++;
    } else {
        guest_setjmp_finish(c, &site, 1);
        jump_test_resumed++;
    }
    c->esp += 8U;                    /* original cdecl caller cleanup */

    if (c->eax == 0U && jump_test_do_jump) {
        if (jump_test_cross_cpu) {
            uintptr_t nested_base = (uintptr_t)jump_test_cross_stack;
            int stopped;
            guest_cpu_init(&nested_fault_cpu);
            jump_test_stale_after = 0;
            if (nested_base > (uintptr_t)UINT32_MAX -
                                  sizeof jump_test_cross_stack ||
                guest_stack_bind(
                    &nested_fault_cpu, (uint32_t)nested_base,
                    (uint32_t)nested_base +
                        (uint32_t)sizeof jump_test_cross_stack) != 0) {
                jump_test_cross_cpu_ok = 0;
            } else {
                stopped = guest_run_until_stop(&nested_fault_cpu,
                                               jump_test_stale_call);
                jump_test_cross_cpu_ok =
                    stopped == GUEST_RUN_FAULT && nested_fault_cpu.fault &&
                    strcmp(nested_fault_cpu.fault,
                           "longjmp has no active guest setjmp") == 0 &&
                    nested_fault_cpu.fault_addr == jump_test_env &&
                    !nested_fault_cpu.jump_sites &&
                    jump_test_stale_after == 0;
            }
            /* The nested stack is test-owned static storage, not the global
             * allocation released by guest_stack_free(). */
            guest_cpu_init(&nested_fault_cpu);
        }
        jump_test_deeper(c);
    }
    guest_setjmp_leave(c, &site);
}

static void jump_test_caller(CPU *__restrict c)
{
    jump_test_owner(c);
    jump_test_caller_after++;
}

static void jump_test_fill_buffer(void)
{
    unsigned i;
    for (i = 0U; i < 16U; ++i)
        st32(jump_test_env + i * 4U, JUMP_TEST_FILL);
}

static int jump_test_buffer_exact(uint32_t saved_esp)
{
    unsigned i;
    uint32_t fs = guest_fs_base(&cpu);
    int ok = fs && ld32(fs) == 0xffffffffU &&
             ld32(jump_test_env + 0x00U) == JUMP_TEST_EBP &&
             ld32(jump_test_env + 0x04U) == JUMP_TEST_EBX &&
             ld32(jump_test_env + 0x08U) == JUMP_TEST_EDI &&
             ld32(jump_test_env + 0x0cU) == JUMP_TEST_ESI &&
             ld32(jump_test_env + 0x10U) == saved_esp - 12U &&
             ld32(jump_test_env + 0x14U) == JUMP_TEST_RETURN &&
             ld32(jump_test_env + 0x18U) == 0xffffffffU &&
             ld32(jump_test_env + 0x1cU) == 0xffffffffU &&
             ld32(jump_test_env + 0x20U) == 0x56433230U &&
             ld32(jump_test_env + 0x24U) == 0U;
    for (i = 10U; i < 16U; ++i)
        if (ld32(jump_test_env + i * 4U) != JUMP_TEST_FILL) ok = 0;
    return ok;
}

static int check_guest_setjmp_longjmp(void)
{
    static const struct {
        uint32_t value;
        uint32_t expected;
        int cross_cpu;
    } nonlocal_cases[] = {
        { 0U, 1U, 1 },              /* ISO/MSVC zero normalises to one */
        { 7U, 7U, 0 },
    };
    uint32_t saved = cpu.esp;
    unsigned calls_before = g_host_import_calls;
    unsigned faults_before = g_fault_count;
    unsigned i;
    int stopped, ok = 1;

    jump_test_env = saved - 512U;
    jump_test_token = ld32(GUEST_IMAGE_BASE + RVA_IAT_LONGJMP);

    /* Normal `_setjmp3` return: exact x86 buffer and cdecl stack, with no
     * import call and no native destination surviving the owner return. */
    jump_test_fill_buffer();
    jump_test_do_jump = jump_test_cross_cpu = 0;
    jump_test_initial = jump_test_resumed = 0;
    jump_test_after_longjmp = jump_test_caller_after = 0;
    cpu.esp = saved;
    stopped = guest_run_until_stop(&cpu, jump_test_caller);
    ok = ok && stopped == GUEST_RUN_RETURNED && cpu.esp == saved &&
         cpu.eax == 0U && jump_test_initial == 1 &&
         jump_test_resumed == 0 && jump_test_after_longjmp == 0 &&
         jump_test_caller_after == 1 && !cpu.jump_sites &&
         jump_test_buffer_exact(saved);

    for (i = 0U; i < sizeof nonlocal_cases / sizeof nonlocal_cases[0]; ++i) {
        jump_test_fill_buffer();
        jump_test_value = nonlocal_cases[i].value;
        jump_test_do_jump = 1;
        jump_test_cross_cpu = nonlocal_cases[i].cross_cpu;
        jump_test_cross_cpu_ok = !jump_test_cross_cpu;
        jump_test_initial = jump_test_resumed = 0;
        jump_test_after_longjmp = jump_test_caller_after = 0;
        cpu.esp = saved;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;
        stopped = guest_run_until_stop(&cpu, jump_test_caller);
        ok = ok && stopped == GUEST_RUN_RETURNED && !cpu.fault &&
             cpu.esp == saved && cpu.eax == nonlocal_cases[i].expected &&
             cpu.ebp == JUMP_TEST_EBP && cpu.ebx == JUMP_TEST_EBX &&
             cpu.edi == JUMP_TEST_EDI && cpu.esi == JUMP_TEST_ESI &&
             jump_test_initial == 1 && jump_test_resumed == 1 &&
             jump_test_after_longjmp == 0 && jump_test_caller_after == 1 &&
             ld32(saved - 12U) == JUMP_TEST_LONGJMP_RETURN &&
             jump_test_cross_cpu_ok && !cpu.jump_sites &&
             jump_test_buffer_exact(saved);
    }

    /* A buffer whose owner returned is not a native continuation.  It must
     * fault at the import boundary instead of jumping into recycled stack. */
    jump_test_value = 1U;
    jump_test_stale_after = 0;
    cpu.esp = saved;
    cpu.fault = NULL;
    cpu.fault_addr = 0U;
    stopped = guest_run_until_stop(&cpu, jump_test_stale_call);
    ok = ok && stopped == GUEST_RUN_FAULT && cpu.fault &&
         strcmp(cpu.fault, "longjmp has no active guest setjmp") == 0 &&
         cpu.fault_addr == jump_test_env && jump_test_stale_after == 0 &&
         !cpu.jump_sites;
    cpu.esp = saved;
    cpu.fault = NULL;
    cpu.fault_addr = 0U;

    /* Two successful nonlocal calls, one cross-CPU rejection, and the final
     * stale-buffer rejection. */
    ok = ok && g_host_import_calls - calls_before == 4U &&
         g_fault_count - faults_before == 2U;
    return ok;
}

static int call_host_noarg(uint32_t slot_rva)
{
    uint32_t saved = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    gpush(&cpu, 0xABC00000u | (slot_rva & 0xFFFFu));
    guest_call(&cpu, token);
    if (cpu.fault || cpu.esp != saved || !cpu.eax) {
        cpu.esp = saved;
        return 0;
    }
    return 1;
}

static int call_host_out64(uint32_t slot_rva, int returns_bool)
{
    uint32_t saved = cpu.esp;
    uint32_t scratch = saved - 16U;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    int ok;
    cpu.esp = scratch;
    st32(scratch, 0U);
    st32(scratch + 4U, 0U);
    gpush(&cpu, scratch);
    gpush(&cpu, 0xABC00000u | (slot_rva & 0xFFFFu));
    guest_call(&cpu, token);
    ok = !cpu.fault && cpu.esp == scratch &&
         (ld32(scratch) || ld32(scratch + 4U)) &&
         (!returns_bool || cpu.eax != 0U);
    cpu.esp = saved;
    return ok;
}

static int call_host_onearg(uint32_t slot_rva, uint32_t argument)
{
    uint32_t saved = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    gpush(&cpu, argument);
    gpush(&cpu, 0xABC00000u | (slot_rva & 0xFFFFu));
    guest_call(&cpu, token);
    if (cpu.fault || cpu.esp != saved) {
        cpu.esp = saved;
        return 0;
    }
    return 1;
}

static uint32_t call_host_stdcall_args(uint32_t slot_rva,
                                       const uint32_t *arguments,
                                       unsigned argument_count, int *ok)
{
    uint32_t saved = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    unsigned i = argument_count;
    while (i) {
        --i;
        gpush(&cpu, arguments[i]);
    }
    gpush(&cpu, 0xABC00000u | (slot_rva & 0xFFFFu));
    guest_call(&cpu, token);
    *ok = *ok && !cpu.fault && cpu.esp == saved;
    cpu.esp = saved;
    return cpu.eax;
}

static uint32_t call_host_cdecl_args(uint32_t slot_rva,
                                     const uint32_t *arguments,
                                     unsigned argument_count, int *ok)
{
    uint32_t saved = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    unsigned i = argument_count;
    while (i) {
        --i;
        gpush(&cpu, arguments[i]);
    }
    gpush(&cpu, 0xABC00000u | (slot_rva & 0xFFFFu));
    guest_call(&cpu, token);
    *ok = *ok && !cpu.fault && cpu.esp == saved - argument_count * 4U;
    cpu.esp = saved;                /* translated cdecl caller's cleanup */
    return cpu.eax;
}

static uint64_t rounding_double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double rounding_double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* Scan the input PE bytes, not generated function membership.  One valid
 * ceil call at 00182c30 lives after embedded data in a large function and a
 * linear Capstone walk used by an earlier audit skipped it.  Relative calls
 * are unchanged by image relocation, so count + ordered-RVA FNV pins every
 * physical call without inheriting that decoder blind spot. */
static uint64_t relative_call_census(uint32_t target_rva, unsigned *count)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    uint32_t rva;
    *count = 0U;
    for (rva = RVA_TEXT_START; rva + 5U <= RVA_TEXT_END; ++rva) {
        unsigned byte_index;
        if (ld8(GUEST_IMAGE_BASE + rva) != 0xE8U ||
            rva + 5U + ld32(GUEST_IMAGE_BASE + rva + 1U) != target_rva)
            continue;
        ++*count;
        for (byte_index = 0U; byte_index < 4U; ++byte_index) {
            hash ^= (uint8_t)(rva >> (byte_index * 8U));
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

static int call_rounding_case(uint32_t slot_rva, uint64_t input_bits,
                              uint64_t expected_bits, int expected_nan,
                              uint8_t initial_top)
{
    static const uint64_t preserved_bits = 0x400921fb54442d18ULL;
    uint32_t arguments[2];
    uint64_t result_bits;
    int ok = 1;

    arguments[0] = (uint32_t)input_bits;
    arguments[1] = (uint32_t)(input_bits >> 32);
    cpu.st_top = initial_top;
    *fst(&cpu, 0) = rounding_double_from_bits(preserved_bits);
    (void)call_host_cdecl_args(slot_rva, arguments, 2U, &ok);
    ok = ok && cpu.st_top == ((initial_top - 1U) & 7U) &&
         rounding_double_bits(*fst(&cpu, 1)) == preserved_bits;
    result_bits = rounding_double_bits(*fst(&cpu, 0));
    if (expected_nan) {
        ok = ok && (result_bits & 0x7ff0000000000000ULL) ==
                   0x7ff0000000000000ULL &&
                   (result_bits & 0x000fffffffffffffULL) != 0U;
    } else {
        ok = ok && result_bits == expected_bits;
    }
    (void)fpop(&cpu);
    return ok && cpu.st_top == initial_top &&
           rounding_double_bits(*fst(&cpu, 0)) == preserved_bits;
}

static int check_guest_rounding_imports(void)
{
    static const struct {
        uint64_t input_bits;
        uint64_t ceil_bits;
        uint64_t floor_bits;
        int nan;
    } cases[] = {
        { 0x0000000000000000ULL, 0x0000000000000000ULL,
          0x0000000000000000ULL, 0 },
        { 0x8000000000000000ULL, 0x8000000000000000ULL,
          0x8000000000000000ULL, 0 },
        { 0x3fd0000000000000ULL, 0x3ff0000000000000ULL,
          0x0000000000000000ULL, 0 },
        { 0xbfd0000000000000ULL, 0x8000000000000000ULL,
          0xbff0000000000000ULL, 0 },
        { 0x3ffc000000000000ULL, 0x4000000000000000ULL,
          0x3ff0000000000000ULL, 0 },
        { 0xbffc000000000000ULL, 0xbff0000000000000ULL,
          0xc000000000000000ULL, 0 },
        { 0x4045000000000000ULL, 0x4045000000000000ULL,
          0x4045000000000000ULL, 0 },
        { 0xc045000000000000ULL, 0xc045000000000000ULL,
          0xc045000000000000ULL, 0 },
        { 0x4330000000000000ULL, 0x4330000000000000ULL,
          0x4330000000000000ULL, 0 },
        { 0xc330000000000000ULL, 0xc330000000000000ULL,
          0xc330000000000000ULL, 0 },
        { 0x432fffffffffffffULL, 0x4330000000000000ULL,
          0x432ffffffffffffeULL, 0 },
        { 0xc32fffffffffffffULL, 0xc32ffffffffffffeULL,
          0xc330000000000000ULL, 0 },
        { 0x7ff0000000000000ULL, 0x7ff0000000000000ULL,
          0x7ff0000000000000ULL, 0 },
        { 0xfff0000000000000ULL, 0xfff0000000000000ULL,
          0xfff0000000000000ULL, 0 },
        { 0x7ff8000000001234ULL, 0U, 0U, 1 },
    };
    double saved_st[8];
    uint32_t ceil_slot = GUEST_IMAGE_BASE + RVA_IAT_CEIL;
    uint32_t floor_slot = GUEST_IMAGE_BASE + RVA_IAT_FLOOR;
    const char *ceil_by_rva = guest_import_name(RVA_IAT_CEIL);
    const char *ceil_by_va = guest_import_name(ceil_slot);
    const char *floor_by_rva = guest_import_name(RVA_IAT_FLOOR);
    const char *floor_by_va = guest_import_name(floor_slot);
    uint8_t saved_top = cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    unsigned ceil_count, floor_count, i;
    unsigned calls_before = g_host_import_calls;
    uint64_t ceil_hash, floor_hash;
    int evidence_ok, values_ok = 1;

    memcpy(saved_st, cpu.st, sizeof saved_st);
    ceil_hash = relative_call_census(RVA_THUNK_CEIL, &ceil_count);
    floor_hash = relative_call_census(RVA_THUNK_FLOOR, &floor_count);
    evidence_ok = ld32(ceil_slot) == ceil_slot &&
                  ld32(floor_slot) == floor_slot &&
                  ceil_by_rva && ceil_by_va && floor_by_rva && floor_by_va &&
                  strcmp(ceil_by_rva, NAME_CEIL) == 0 &&
                  strcmp(ceil_by_va, NAME_CEIL) == 0 &&
                  strcmp(floor_by_rva, NAME_FLOOR) == 0 &&
                  strcmp(floor_by_va, NAME_FLOOR) == 0 &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_CEIL) == 0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_CEIL + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_CEIL + 2U) == ceil_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_FLOOR) == 0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_FLOOR + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_FLOOR + 2U) == floor_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_CEIL_ABI_CALL - 3U) == 0xDDU &&
                  ld8(GUEST_IMAGE_BASE + RVA_CEIL_ABI_CALL - 2U) == 0x1CU &&
                  ld8(GUEST_IMAGE_BASE + RVA_CEIL_ABI_CALL - 1U) == 0x24U &&
                  ld8(GUEST_IMAGE_BASE + RVA_CEIL_ABI_CALL) == 0xE8U &&
                  RVA_CEIL_ABI_CALL + 5U +
                      ld32(GUEST_IMAGE_BASE + RVA_CEIL_ABI_CALL + 1U) ==
                      RVA_THUNK_CEIL &&
                  ld8(GUEST_IMAGE_BASE + RVA_CEIL_DECODE_GAP_CALL) == 0xE8U &&
                  RVA_CEIL_DECODE_GAP_CALL + 5U +
                      ld32(GUEST_IMAGE_BASE + RVA_CEIL_DECODE_GAP_CALL + 1U) ==
                      RVA_THUNK_CEIL &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL - 3U) == 0xDDU &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL - 2U) == 0x1CU &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL - 1U) == 0x24U &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL) == 0xE8U &&
                  RVA_FLOOR_STARTUP_CALL + 5U +
                      ld32(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL + 1U) ==
                      RVA_THUNK_FLOOR &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL + 5U) == 0x83U &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL + 6U) == 0xC4U &&
                  ld8(GUEST_IMAGE_BASE + RVA_FLOOR_STARTUP_CALL + 7U) == 0x08U &&
                  ceil_count == CEIL_CALL_COUNT &&
                  floor_count == FLOOR_CALL_COUNT &&
                  ceil_hash == CEIL_CALL_FNV64 &&
                  floor_hash == FLOOR_CALL_FNV64;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        values_ok = call_rounding_case(
            RVA_IAT_CEIL, cases[i].input_bits, cases[i].ceil_bits,
            cases[i].nan, (uint8_t)(i & 7U)) && values_ok;
        values_ok = call_rounding_case(
            RVA_IAT_FLOOR, cases[i].input_bits, cases[i].floor_bits,
            cases[i].nan, (uint8_t)(i & 7U)) && values_ok;
    }
    values_ok = values_ok &&
                g_host_import_calls - calls_before ==
                    2U * (unsigned)(sizeof cases / sizeof cases[0]);
    memcpy(cpu.st, saved_st, sizeof saved_st);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT ceil/floor ABI          : %s, %s, %u/%u physical calls\n",
           evidence_ok ? "IAT/thunk/census exact" : "evidence FAILED",
           values_ok ? "cdecl+x87 values exact" : "values FAILED",
           ceil_count, floor_count);
    return evidence_ok && values_ok;
}

/* UCRT's public contract is `short __cdecl _fdclass(float)`: the binary32
 * argument is a four-byte stack value, the classification is returned in AX,
 * and the caller removes the argument.  The adapter also sign-extends that
 * short through EAX.  The constants below are FP_SUBNORMAL (-2), FP_NORMAL
 * (-1), FP_ZERO (0), FP_INFINITE (1), and FP_NAN (2), not the unrelated
 * `_FPCLASS_*` masks from float.h. */
static int check_guest_fdclass(void)
{
    static const struct {
        uint32_t input_bits;
        int16_t expected;
    } cases[] = {
        { 0x00000000U,  0 }, /* +zero */
        { 0x80000000U,  0 }, /* -zero */
        { 0x00000001U, -2 }, /* +minimum subnormal */
        { 0x80000001U, -2 }, /* -minimum subnormal */
        { 0x3f800000U, -1 }, /* +normal */
        { 0xbf800000U, -1 }, /* -normal */
        { 0x7f800000U,  1 }, /* +infinity */
        { 0xff800000U,  1 }, /* -infinity */
        { 0x7fc01234U,  2 }, /* +quiet NaN */
        { 0xffc01234U,  2 }, /* -quiet NaN */
        { 0x7f801234U,  2 }, /* +signaling NaN */
        { 0xff801234U,  2 }, /* -signaling NaN */
    };
    static const struct {
        uint32_t call_rva;
        uint8_t cleanup_offset;
        uint8_t test_ax_offset;
    } sites[] = {
        { 0x00354176U, 14U, 32U },
        { 0x004c5cdcU,  5U,  8U },
        { 0x004c81c9U,  5U,  8U },
        { 0x004cf964U,  5U,  8U },
        { 0x004cf9f0U,  5U,  8U },
        { 0x004cff14U,  5U,  8U },
        { 0x004cff3fU, 13U, 16U },
        { 0x004d5025U,  5U,  8U },
    };
    static const uint8_t abi_window[] = {
        0xd9,0x44,0x24,0x10,             /* fld dword ptr [esp+10h] */
        0x51,                            /* push ecx: four-byte argument */
        0xd9,0x1c,0x24,                 /* fstp dword ptr [esp] */
        0xe8,0x89,0x66,0x12,0x00,       /* call the shared thunk */
        0x83,0xc4,0x04,                 /* caller removes four bytes */
        0x66,0x85,0xc0,                 /* consume short result from AX */
        0x7f,0x7a,
    };
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_FDCLASS;
    const char *by_rva = guest_import_name(RVA_IAT_FDCLASS);
    const char *by_va = guest_import_name(slot);
    xmm_t saved_xmm[8];
    double saved_st[8];
    uint8_t saved_top = (uint8_t)cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    int saved_native_errno = errno;
    uint32_t errno_cell;
    uint32_t saved_guest_errno;
    unsigned physical_count, calls_before, i;
    uint64_t physical_hash;
    int call_ok = 1, evidence_ok = 1, values_ok = 1;

    physical_hash = relative_call_census(RVA_THUNK_FDCLASS,
                                         &physical_count);
    evidence_ok = ld32(slot) == slot && by_rva && by_va &&
                  strcmp(by_rva, NAME_FDCLASS) == 0 &&
                  strcmp(by_va, NAME_FDCLASS) == 0 &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_FDCLASS) == 0xffU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_FDCLASS + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_FDCLASS + 2U) == slot &&
                  physical_count == FDCLASS_CALL_COUNT &&
                  physical_hash == FDCLASS_CALL_FNV64 &&
                  sizeof sites / sizeof sites[0] == FDCLASS_CALL_COUNT;
    for (i = 0U; i < sizeof abi_window; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_FDCLASS_ABI_WINDOW + i) ==
                abi_window[i];
    for (i = 0U; i < sizeof sites / sizeof sites[0]; ++i) {
        uint32_t call = sites[i].call_rva;
        uint32_t cleanup = call + sites[i].cleanup_offset;
        uint32_t test_ax = call + sites[i].test_ax_offset;

        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + call) == 0xe8U &&
            call + 5U + ld32(GUEST_IMAGE_BASE + call + 1U) ==
                RVA_THUNK_FDCLASS &&
            ld8(GUEST_IMAGE_BASE + cleanup) == 0x83U &&
            ld8(GUEST_IMAGE_BASE + cleanup + 1U) == 0xc4U &&
            ld8(GUEST_IMAGE_BASE + cleanup + 2U) == 0x04U &&
            ld8(GUEST_IMAGE_BASE + test_ax) == 0x66U &&
            ld8(GUEST_IMAGE_BASE + test_ax + 1U) == 0x85U &&
            ld8(GUEST_IMAGE_BASE + test_ax + 2U) == 0xc0U;
    }

    memcpy(saved_xmm, cpu.x, sizeof saved_xmm);
    memcpy(saved_st, cpu.st, sizeof saved_st);
    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    values_ok = call_ok && errno_cell != 0U;
    saved_guest_errno = ld32(errno_cell);
    calls_before = g_host_import_calls;
    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        uint32_t returned;
        uint32_t initial_guest_errno = 0x5a170000U + i;

        cpu.eax = 0xa5a55a5aU;
        errno = 1234;
        st32(errno_cell, initial_guest_errno);
        returned = call_host_cdecl_args(RVA_IAT_FDCLASS,
                                        &cases[i].input_bits, 1U, &call_ok);
        values_ok = values_ok && call_ok &&
                    returned == (uint32_t)(int32_t)cases[i].expected &&
                    errno == 1234 &&
                    ld32(errno_cell) == initial_guest_errno &&
                    memcmp(cpu.x, saved_xmm, sizeof saved_xmm) == 0 &&
                    memcmp(cpu.st, saved_st, sizeof saved_st) == 0 &&
                    cpu.st_top == saved_top && cpu.fsw == saved_fsw;
    }
    values_ok = values_ok &&
        g_host_import_calls - calls_before ==
            (unsigned)(sizeof cases / sizeof cases[0]);
    st32(errno_cell, saved_guest_errno);
    errno = saved_native_errno;
    memcpy(cpu.x, saved_xmm, sizeof saved_xmm);
    memcpy(cpu.st, saved_st, sizeof saved_st);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT _fdclass ABI           : %s, %s, %u physical calls\n",
           evidence_ok ? "IAT/thunk/window/census exact" : "evidence FAILED",
           values_ok ? "EAX+cdecl+errno values exact" : "values FAILED",
           physical_count);
    return evidence_ok && values_ok;
}

typedef double (__cdecl *test_unary_double_fn)(double);
typedef double (__cdecl *test_binary_double_fn)(double, double);

static double call_native_unary_double(test_unary_double_fn operation,
                                       double input, int *math_errno)
{
    int saved_errno = errno;
    double result;

    errno = 0;
    result = operation(input);
    *math_errno = errno;
    errno = saved_errno;
    return result;
}

static double call_native_binary_double(test_binary_double_fn operation,
                                        double left, double right,
                                        int *math_errno)
{
    int saved_errno = errno;
    double result;

    errno = 0;
    result = operation(left, right);
    *math_errno = errno;
    errno = saved_errno;
    return result;
}

static int double_bits_are_nan(uint64_t bits)
{
    return (bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
           (bits & 0x000fffffffffffffULL) != 0U;
}

static int call_sse2_unary_case(uint32_t slot_rva,
                                test_unary_double_fn operation,
                                uint64_t input_bits, uint32_t errno_cell,
                                unsigned case_index)
{
    xmm_t before_xmm[8];
    double before_st[8];
    uint32_t saved_esp = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    uint32_t initial_guest_errno = 173U + case_index;
    uint64_t expected_bits, result_bits;
    uint8_t before_top = (uint8_t)cpu.st_top;
    uint16_t before_fsw = cpu.fsw;
    int saved_host_errno = errno;
    int expected_errno;
    unsigned i;
    int ok;

    expected_bits = rounding_double_bits(call_native_unary_double(
        operation, rounding_double_from_bits(input_bits), &expected_errno));
    for (i = 0U; i < 8U; ++i) {
        cpu.x[i].u64[0] = 0x0123456789abcdefULL ^
                          ((uint64_t)i << 40U) ^ case_index;
        cpu.x[i].u64[1] = 0xfedcba9876543210ULL ^
                          ((uint64_t)i << 36U) ^ case_index;
    }
    cpu.x[0].u64[0] = input_bits;
    memcpy(before_xmm, cpu.x, sizeof before_xmm);
    memcpy(before_st, cpu.st, sizeof before_st);
    st32(errno_cell, initial_guest_errno);

    errno = EILSEQ;
    gpush(&cpu, 0xABC00000U | (slot_rva & 0xFFFFU));
    guest_call(&cpu, token);
    result_bits = cpu.x[0].u64[0];
    ok = !cpu.fault && cpu.esp == saved_esp && errno == EILSEQ &&
         cpu.x[0].u64[1] == before_xmm[0].u64[1] &&
         memcmp(&cpu.x[1], &before_xmm[1],
                7U * sizeof before_xmm[0]) == 0 &&
         cpu.st_top == before_top && cpu.fsw == before_fsw &&
         memcmp(cpu.st, before_st, sizeof before_st) == 0 &&
         ld32(errno_cell) == (expected_errno != 0
                              ? (uint32_t)expected_errno
                              : initial_guest_errno) &&
         (double_bits_are_nan(expected_bits)
          ? double_bits_are_nan(result_bits)
          : result_bits == expected_bits);
    errno = saved_host_errno;
    return ok;
}

static int call_sse2_binary_case(uint32_t slot_rva,
                                 test_binary_double_fn operation,
                                 uint64_t left_bits, uint64_t right_bits,
                                 uint32_t errno_cell, unsigned case_index)
{
    xmm_t before_xmm[8];
    double before_st[8];
    uint32_t saved_esp = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    uint32_t initial_guest_errno = 211U + case_index;
    uint64_t expected_bits, result_bits;
    uint8_t before_top = (uint8_t)cpu.st_top;
    uint16_t before_fsw = cpu.fsw;
    int saved_host_errno = errno;
    int expected_errno;
    unsigned i;
    int ok;

    expected_bits = rounding_double_bits(call_native_binary_double(
        operation, rounding_double_from_bits(left_bits),
        rounding_double_from_bits(right_bits), &expected_errno));
    for (i = 0U; i < 8U; ++i) {
        cpu.x[i].u64[0] = 0x13579bdf2468ace0ULL ^
                          ((uint64_t)i << 40U) ^ case_index;
        cpu.x[i].u64[1] = 0xeca86420fdb97531ULL ^
                          ((uint64_t)i << 36U) ^ case_index;
    }
    cpu.x[0].u64[0] = left_bits;
    cpu.x[1].u64[0] = right_bits;
    memcpy(before_xmm, cpu.x, sizeof before_xmm);
    memcpy(before_st, cpu.st, sizeof before_st);
    st32(errno_cell, initial_guest_errno);

    errno = EILSEQ;
    gpush(&cpu, 0xABD00000U | (slot_rva & 0xFFFFU));
    guest_call(&cpu, token);
    result_bits = cpu.x[0].u64[0];
    ok = !cpu.fault && cpu.esp == saved_esp && errno == EILSEQ &&
         cpu.x[0].u64[1] == before_xmm[0].u64[1] &&
         memcmp(&cpu.x[1], &before_xmm[1],
                7U * sizeof before_xmm[0]) == 0 &&
         cpu.st_top == before_top && cpu.fsw == before_fsw &&
         memcmp(cpu.st, before_st, sizeof before_st) == 0 &&
         ld32(errno_cell) == (expected_errno != 0
                              ? (uint32_t)expected_errno
                              : initial_guest_errno) &&
         (double_bits_are_nan(expected_bits)
          ? double_bits_are_nan(result_bits)
          : result_bits == expected_bits);
    errno = saved_host_errno;
    return ok;
}

/* Pin the complete representative wrappers, exact IAT thunks and every raw
 * E8 call in the PE so a future compiler/input change cannot silently turn
 * these XMM register ABIs into guessed cdecl+x87 calls. */
static int check_guest_sse2_precise_math(void)
{
    static const uint8_t cos_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf3,0x0f,0x5a,0xc0,0xe8,0x93,
        0xa3,0x5d,0x00,0xf2,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t log_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf3,0x0f,0x5a,0xc0,0xe8,0x45,
        0xcf,0x35,0x00,0xf2,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t sin_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf3,0x0f,0x5a,0xc0,0xe8,0x81,
        0xa3,0x5d,0x00,0xf2,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t pow_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf2,0x0f,0x10,0x0d,0xb8,0xa7,
        0x76,0x30,0x66,0x0f,0x6e,0xc1,0xf3,0x0f,0xe6,0xc0,0xe8,0x2f,
        0x2c,0x14,0x00,0x8b,0xe5,0x5d,0xc3
    };
    static const uint64_t inputs[] = {
        0x0000000000000000ULL, /* +0 */
        0x8000000000000000ULL, /* -0 */
        0x3fe0000000000000ULL, /* finite, +0.5 */
        0xbfe0000000000000ULL, /* finite, -0.5 */
        0x7ff0000000000000ULL, /* +Inf: trig domain, log identity */
        0x7ff8000000001234ULL, /* quiet NaN */
    };
    static const uint64_t pow_inputs[][2] = {
        { 0x4010000000000000ULL, 0x4000000000000000ULL }, /* live 4^2 */
        { 0x4000000000000000ULL, 0x4008000000000000ULL }, /* 2^3 */
        { 0x4000000000000000ULL, 0xc000000000000000ULL }, /* 2^-2 */
        { 0xc000000000000000ULL, 0x4008000000000000ULL }, /* -2^3 */
        { 0xc000000000000000ULL, 0x3fe0000000000000ULL }, /* domain */
        { 0x0000000000000000ULL, 0xbff0000000000000ULL }, /* range */
        { 0x4000000000000000ULL, 0x4090000000000000ULL }, /* overflow */
        { 0x7ff8000000001234ULL, 0x4000000000000000ULL }, /* quiet NaN */
    };
    uint32_t cos_slot = GUEST_IMAGE_BASE + RVA_IAT_LIBM_SSE2_COS_PRECISE;
    uint32_t log_slot = GUEST_IMAGE_BASE + RVA_IAT_LIBM_SSE2_LOG_PRECISE;
    uint32_t pow_slot = GUEST_IMAGE_BASE + RVA_IAT_LIBM_SSE2_POW_PRECISE;
    uint32_t sin_slot = GUEST_IMAGE_BASE + RVA_IAT_LIBM_SSE2_SIN_PRECISE;
    const char *cos_by_rva = guest_import_name(
        RVA_IAT_LIBM_SSE2_COS_PRECISE);
    const char *cos_by_va = guest_import_name(cos_slot);
    const char *log_by_rva = guest_import_name(
        RVA_IAT_LIBM_SSE2_LOG_PRECISE);
    const char *log_by_va = guest_import_name(log_slot);
    const char *pow_by_rva = guest_import_name(
        RVA_IAT_LIBM_SSE2_POW_PRECISE);
    const char *pow_by_va = guest_import_name(pow_slot);
    const char *sin_by_rva = guest_import_name(
        RVA_IAT_LIBM_SSE2_SIN_PRECISE);
    const char *sin_by_va = guest_import_name(sin_slot);
    xmm_t saved_xmm[8];
    double saved_st[8];
    uint8_t saved_top = (uint8_t)cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    uint32_t errno_cell;
    unsigned cos_count, log_count, pow_count, sin_count, i;
    unsigned calls_before;
    uint64_t cos_hash, log_hash, pow_hash, sin_hash;
    int call_ok = 1, evidence_ok = 1, values_ok = 1;

    cos_hash = relative_call_census(RVA_THUNK_LIBM_SSE2_COS_PRECISE,
                                    &cos_count);
    log_hash = relative_call_census(RVA_THUNK_LIBM_SSE2_LOG_PRECISE,
                                    &log_count);
    pow_hash = relative_call_census(RVA_THUNK_LIBM_SSE2_POW_PRECISE,
                                    &pow_count);
    sin_hash = relative_call_census(RVA_THUNK_LIBM_SSE2_SIN_PRECISE,
                                    &sin_count);
    evidence_ok = ld32(cos_slot) == cos_slot && ld32(log_slot) == log_slot &&
                  ld32(pow_slot) == pow_slot && ld32(sin_slot) == sin_slot &&
                  cos_by_rva && cos_by_va && log_by_rva && log_by_va &&
                  pow_by_rva && pow_by_va && sin_by_rva && sin_by_va &&
                  strcmp(cos_by_rva, NAME_LIBM_SSE2_COS_PRECISE) == 0 &&
                  strcmp(cos_by_va, NAME_LIBM_SSE2_COS_PRECISE) == 0 &&
                  strcmp(log_by_rva, NAME_LIBM_SSE2_LOG_PRECISE) == 0 &&
                  strcmp(log_by_va, NAME_LIBM_SSE2_LOG_PRECISE) == 0 &&
                  strcmp(pow_by_rva, NAME_LIBM_SSE2_POW_PRECISE) == 0 &&
                  strcmp(pow_by_va, NAME_LIBM_SSE2_POW_PRECISE) == 0 &&
                  strcmp(sin_by_rva, NAME_LIBM_SSE2_SIN_PRECISE) == 0 &&
                  strcmp(sin_by_va, NAME_LIBM_SSE2_SIN_PRECISE) == 0 &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_COS_PRECISE) ==
                      0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_COS_PRECISE +
                      1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_COS_PRECISE +
                       2U) == cos_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_LOG_PRECISE) ==
                      0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_LOG_PRECISE +
                      1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_LOG_PRECISE +
                       2U) == log_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_POW_PRECISE) ==
                      0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_POW_PRECISE +
                      1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_POW_PRECISE +
                       2U) == pow_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_SIN_PRECISE) ==
                      0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_SIN_PRECISE +
                      1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_LIBM_SSE2_SIN_PRECISE +
                       2U) == sin_slot &&
                  cos_count == LIBM_SSE2_COS_CALL_COUNT &&
                  log_count == LIBM_SSE2_LOG_CALL_COUNT &&
                  pow_count == LIBM_SSE2_POW_CALL_COUNT &&
                  sin_count == LIBM_SSE2_SIN_CALL_COUNT &&
                  cos_hash == LIBM_SSE2_COS_CALL_FNV64 &&
                  log_hash == LIBM_SSE2_LOG_CALL_FNV64 &&
                  pow_hash == LIBM_SSE2_POW_CALL_FNV64 &&
                  sin_hash == LIBM_SSE2_SIN_CALL_FNV64;
    for (i = 0U; i < sizeof cos_wrapper; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_LIBM_COS_FLOAT_WRAPPER + i) ==
                cos_wrapper[i];
    for (i = 0U; i < sizeof log_wrapper; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_LIBM_LOG_FLOAT_WRAPPER + i) ==
                log_wrapper[i];
    for (i = 0U; i < sizeof sin_wrapper; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_LIBM_SIN_FLOAT_WRAPPER + i) ==
                sin_wrapper[i];
    for (i = 0U; i < sizeof pow_wrapper; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_LIBM_POW_LIVE_WRAPPER + i) ==
                pow_wrapper[i];

    memcpy(saved_xmm, cpu.x, sizeof saved_xmm);
    memcpy(saved_st, cpu.st, sizeof saved_st);
    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    values_ok = call_ok && errno_cell != 0U;
    calls_before = g_host_import_calls;
    for (i = 0U; i < sizeof inputs / sizeof inputs[0]; ++i) {
        values_ok = call_sse2_unary_case(
            RVA_IAT_LIBM_SSE2_COS_PRECISE, cos, inputs[i], errno_cell,
            3U * i) && values_ok;
        values_ok = call_sse2_unary_case(
            RVA_IAT_LIBM_SSE2_LOG_PRECISE, log, inputs[i], errno_cell,
            3U * i + 1U) && values_ok;
        values_ok = call_sse2_unary_case(
            RVA_IAT_LIBM_SSE2_SIN_PRECISE, sin, inputs[i], errno_cell,
            3U * i + 2U) && values_ok;
    }
    for (i = 0U; i < sizeof pow_inputs / sizeof pow_inputs[0]; ++i)
        values_ok = call_sse2_binary_case(
            RVA_IAT_LIBM_SSE2_POW_PRECISE, pow, pow_inputs[i][0],
            pow_inputs[i][1], errno_cell, 3U * (unsigned)(sizeof inputs /
            sizeof inputs[0]) + i) && values_ok;
    values_ok = values_ok &&
                g_host_import_calls - calls_before ==
                    3U * (unsigned)(sizeof inputs / sizeof inputs[0]) +
                    (unsigned)(sizeof pow_inputs / sizeof pow_inputs[0]);
    memcpy(cpu.x, saved_xmm, sizeof saved_xmm);
    memcpy(cpu.st, saved_st, sizeof saved_st);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT precise cos/sin/log/pow : %s, %s, %u/%u/%u/%u physical calls\n",
           evidence_ok ? "IAT/thunk/wrapper/census exact" : "evidence FAILED",
           values_ok ? "XMM register+errno values exact" : "values FAILED",
           cos_count, sin_count, log_count, pow_count);
    return evidence_ok && values_ok;
}

/* Close the remaining six unary `/fp:precise` imports as one exact ABI
 * family.  Each row independently pins its IAT identity, JMP thunk, complete
 * physical E8 census and one caller window that consumes the binary64 XMM0
 * result.  The value loop then exercises every host adapter while checking
 * the same register, x87, stack and guest/native errno invariants as the live
 * log path above. */
static int check_guest_sse2_remaining_unary(void)
{
    static const uint8_t acos_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf3,0x0f,0x5a,0xc0,0xe8,0xe1,
        0xd2,0x3f,0x00,0xf2,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t asin_wrapper[] = {
        0xf3,0x0f,0x5a,0xc0,0xe8,0x9d,0xc5,0x2c,0x00,0xf2,0x0f,0x5a,
        0xc0,0xc3
    };
    static const uint8_t atan_context[] = {
        0xf3,0x0f,0x5a,0xc0,0xe8,0xc6,0xba,0x12,0x00,0x0f,0x57,0xc9,
        0x8b,0x45,0x08,0xf2,0x0f,0x5a,0xc8
    };
    static const uint8_t exp_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf3,0x0f,0x5a,0xc0,0xe8,0xc9,
        0xc5,0x2c,0x00,0xf2,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t log10_context[] = {
        0x0f,0x5a,0xc0,0xe8,0x54,0xd4,0x35,0x00,0xf3,0x0f,0x10,0x0d,
        0x14,0xa6,0x76,0x30,0xb8,0x01,0x00,0x00,0x00,0xf2,0x0f,0x5a,
        0xc0
    };
    static const uint8_t sqrt_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0xf3,0x0f,0x5a,0xc0,0x0f,0x57,
        0xc9,0x66,0x0f,0x2e,0xc8,0x77,0x0c,0xf2,0x0f,0x51,0xc0,0xf2,
        0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3,0xe8,0xc2,0x9a,0x5c,0x00,
        0xf2,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint64_t inputs[] = {
        0x0000000000000000ULL, /* +0 */
        0x8000000000000000ULL, /* -0 */
        0x3fe0000000000000ULL, /* +0.5 */
        0xbfe0000000000000ULL, /* -0.5 */
        0x7ff0000000000000ULL, /* +Inf */
        0x7ff8000000001234ULL, /* quiet NaN */
        0x408f400000000000ULL, /* +1000: exp overflow */
        0xc08f400000000000ULL, /* -1000: exp underflow */
    };
    struct unary_spec {
        const char *name;
        uint32_t iat_rva;
        uint32_t thunk_rva;
        uint32_t window_rva;
        const uint8_t *window;
        unsigned window_size;
        unsigned call_count;
        uint64_t call_hash;
        test_unary_double_fn operation;
    };
    static const struct unary_spec specs[] = {
        { NAME_LIBM_SSE2_ACOS_PRECISE,
          RVA_IAT_LIBM_SSE2_ACOS_PRECISE,
          RVA_THUNK_LIBM_SSE2_ACOS_PRECISE,
          RVA_LIBM_ACOS_FLOAT_WRAPPER, acos_wrapper, sizeof acos_wrapper,
          LIBM_SSE2_ACOS_CALL_COUNT, LIBM_SSE2_ACOS_CALL_FNV64, acos },
        { NAME_LIBM_SSE2_ASIN_PRECISE,
          RVA_IAT_LIBM_SSE2_ASIN_PRECISE,
          RVA_THUNK_LIBM_SSE2_ASIN_PRECISE,
          RVA_LIBM_ASIN_FLOAT_WRAPPER, asin_wrapper, sizeof asin_wrapper,
          LIBM_SSE2_ASIN_CALL_COUNT, LIBM_SSE2_ASIN_CALL_FNV64, asin },
        { NAME_LIBM_SSE2_ATAN_PRECISE,
          RVA_IAT_LIBM_SSE2_ATAN_PRECISE,
          RVA_THUNK_LIBM_SSE2_ATAN_PRECISE,
          RVA_LIBM_ATAN_FLOAT_CONTEXT, atan_context, sizeof atan_context,
          LIBM_SSE2_ATAN_CALL_COUNT, LIBM_SSE2_ATAN_CALL_FNV64, atan },
        { NAME_LIBM_SSE2_EXP_PRECISE,
          RVA_IAT_LIBM_SSE2_EXP_PRECISE,
          RVA_THUNK_LIBM_SSE2_EXP_PRECISE,
          RVA_LIBM_EXP_FLOAT_WRAPPER, exp_wrapper, sizeof exp_wrapper,
          LIBM_SSE2_EXP_CALL_COUNT, LIBM_SSE2_EXP_CALL_FNV64, exp },
        { NAME_LIBM_SSE2_LOG10_PRECISE,
          RVA_IAT_LIBM_SSE2_LOG10_PRECISE,
          RVA_THUNK_LIBM_SSE2_LOG10_PRECISE,
          RVA_LIBM_LOG10_FLOAT_CONTEXT, log10_context, sizeof log10_context,
          LIBM_SSE2_LOG10_CALL_COUNT, LIBM_SSE2_LOG10_CALL_FNV64, log10 },
        { NAME_LIBM_SSE2_SQRT_PRECISE,
          RVA_IAT_LIBM_SSE2_SQRT_PRECISE,
          RVA_THUNK_LIBM_SSE2_SQRT_PRECISE,
          RVA_LIBM_SQRT_FLOAT_WRAPPER, sqrt_wrapper, sizeof sqrt_wrapper,
          LIBM_SSE2_SQRT_CALL_COUNT, LIBM_SSE2_SQRT_CALL_FNV64, sqrt },
    };
    xmm_t saved_xmm[8];
    double saved_st[8];
    uint8_t saved_top = (uint8_t)cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    unsigned counts[sizeof specs / sizeof specs[0]];
    uint32_t errno_cell;
    unsigned calls_before;
    unsigned i, j;
    int call_ok = 1, evidence_ok = 1, values_ok = 1;

    for (j = 0U; j < sizeof specs / sizeof specs[0]; ++j) {
        const struct unary_spec *spec = &specs[j];
        uint32_t slot = GUEST_IMAGE_BASE + spec->iat_rva;
        const char *by_rva = guest_import_name(spec->iat_rva);
        const char *by_va = guest_import_name(slot);
        uint64_t hash = relative_call_census(spec->thunk_rva, &counts[j]);

        evidence_ok = evidence_ok && ld32(slot) == slot && by_rva && by_va &&
            strcmp(by_rva, spec->name) == 0 &&
            strcmp(by_va, spec->name) == 0 &&
            ld8(GUEST_IMAGE_BASE + spec->thunk_rva) == 0xFFU &&
            ld8(GUEST_IMAGE_BASE + spec->thunk_rva + 1U) == 0x25U &&
            ld32(GUEST_IMAGE_BASE + spec->thunk_rva + 2U) == slot &&
            counts[j] == spec->call_count && hash == spec->call_hash;
        for (i = 0U; i < spec->window_size; ++i)
            evidence_ok = evidence_ok &&
                ld8(GUEST_IMAGE_BASE + spec->window_rva + i) == spec->window[i];
    }

    memcpy(saved_xmm, cpu.x, sizeof saved_xmm);
    memcpy(saved_st, cpu.st, sizeof saved_st);
    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    values_ok = call_ok && errno_cell != 0U;
    calls_before = g_host_import_calls;
    for (i = 0U; i < sizeof inputs / sizeof inputs[0]; ++i) {
        for (j = 0U; j < sizeof specs / sizeof specs[0]; ++j) {
            values_ok = call_sse2_unary_case(
                specs[j].iat_rva, specs[j].operation, inputs[i], errno_cell,
                i * (unsigned)(sizeof specs / sizeof specs[0]) + j) &&
                values_ok;
        }
    }
    values_ok = values_ok &&
        g_host_import_calls - calls_before ==
            (unsigned)(sizeof inputs / sizeof inputs[0]) *
            (unsigned)(sizeof specs / sizeof specs[0]);
    memcpy(cpu.x, saved_xmm, sizeof saved_xmm);
    memcpy(cpu.st, saved_st, sizeof saved_st);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT precise remaining unary: %s, %s, %u/%u/%u/%u/%u/%u calls\n",
           evidence_ok ? "IAT/thunk/windows/census exact" : "evidence FAILED",
           values_ok ? "XMM register+errno values exact" : "values FAILED",
           counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    return evidence_ok && values_ok;
}

/* Count the exact six-byte `call dword ptr [IAT]` forms in the input image.
 * These imports are called directly rather than through CRT thunks, so the
 * ordered physical-RVA hash is the stable callsite census. */
static uint64_t absolute_iat_call_census(uint32_t slot_rva, unsigned *count)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    uint32_t slot = GUEST_IMAGE_BASE + slot_rva;
    uint32_t rva;
    *count = 0U;
    for (rva = RVA_TEXT_START; rva + 6U <= RVA_TEXT_END; ++rva) {
        unsigned byte_index;
        if (ld8(GUEST_IMAGE_BASE + rva) != 0xFFU ||
            ld8(GUEST_IMAGE_BASE + rva + 1U) != 0x15U ||
            ld32(GUEST_IMAGE_BASE + rva + 2U) != slot)
            continue;
        ++*count;
        for (byte_index = 0U; byte_index < 4U; ++byte_index) {
            hash ^= (uint8_t)(rva >> (byte_index * 8U));
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

/* Raw absolute-address references, including both `call [IAT]` and
 * `mov reg,[IAT]`.  Every measured Sleep reference has a two-byte opcode/
 * ModRM prefix, so hash the instruction RVA rather than the operand RVA. */
static uint64_t absolute_iat_reference_census(uint32_t slot_rva,
                                              unsigned *count)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    uint32_t slot = GUEST_IMAGE_BASE + slot_rva;
    uint32_t operand_rva;
    *count = 0U;
    for (operand_rva = RVA_TEXT_START + 2U;
         operand_rva + 4U <= RVA_TEXT_END; ++operand_rva) {
        uint32_t instruction_rva;
        unsigned byte_index;
        if (ld32(GUEST_IMAGE_BASE + operand_rva) != slot)
            continue;
        instruction_rva = operand_rva - 2U;
        ++*count;
        for (byte_index = 0U; byte_index < 4U; ++byte_index) {
            hash ^= (uint8_t)(instruction_rva >> (byte_index * 8U));
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

/* Freeze ordered physical-site lists separately from raw IAT-reference
 * discovery.  Register-mediated calls do not encode the slot address at the
 * call itself, so their exact FF /2 locations are part of the evidence. */
static uint64_t rva_sequence_fnv64(const uint32_t *sites, unsigned count)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    unsigned i;
    for (i = 0U; i < count; ++i) {
        unsigned byte_index;
        for (byte_index = 0U; byte_index < 4U; ++byte_index) {
            hash ^= (uint8_t)(sites[i] >> (byte_index * 8U));
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}

static int guest_image_bytes_equal(uint32_t rva, const uint8_t *expected,
                                   unsigned count)
{
    unsigned i;
    for (i = 0U; i < count; ++i)
        if (ld8(GUEST_IMAGE_BASE + rva + i) != expected[i])
            return 0;
    return 1;
}

static uint64_t guest_image_bytes_fnv64(uint32_t rva, unsigned count)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    unsigned i;
    for (i = 0U; i < count; ++i) {
        hash ^= ld8(GUEST_IMAGE_BASE + rva + i);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static int call_ci_binary_case(uint32_t slot_rva,
                               test_binary_double_fn operation,
                               uint64_t y_bits, uint64_t x_bits,
                               uint32_t errno_cell, unsigned case_index)
{
    static const uint64_t sentinel0 = 0x400921fb54442d18ULL;
    static const uint64_t sentinel1 = 0xc005bf0a8b145769ULL;
    xmm_t before_xmm[8];
    uint32_t saved_esp = cpu.esp;
    uint32_t token = ld32(GUEST_IMAGE_BASE + slot_rva);
    uint32_t initial_guest_errno = 0x5100U + case_index;
    uint8_t anchor_top = (uint8_t)((case_index + 3U) & 7U);
    uint8_t entry_top;
    uint16_t before_fsw;
    uint64_t expected_bits, result_bits;
    int expected_errno;
    int saved_host_errno = errno;
    unsigned i;
    int ok;

    expected_bits = rounding_double_bits(call_native_binary_double(
        operation, rounding_double_from_bits(y_bits),
        rounding_double_from_bits(x_bits), &expected_errno));

    for (i = 0U; i < 8U; ++i) {
        cpu.st[i] = rounding_double_from_bits(
            0x3ff0000000000000ULL + ((uint64_t)i << 44U));
        cpu.x[i].u64[0] = 0x0123456789abcdefULL ^
                          ((uint64_t)i << 36U) ^ case_index;
        cpu.x[i].u64[1] = 0xfedcba9876543210ULL ^
                          ((uint64_t)i << 32U) ^ case_index;
    }
    memcpy(before_xmm, cpu.x, sizeof before_xmm);
    cpu.st_top = anchor_top;
    *fst(&cpu, 0) = rounding_double_from_bits(sentinel0);
    *fst(&cpu, 1) = rounding_double_from_bits(sentinel1);
    fpush(&cpu, rounding_double_from_bits(y_bits));
    fpush(&cpu, rounding_double_from_bits(x_bits));
    entry_top = (uint8_t)cpu.st_top;
    cpu.fsw = (uint16_t)(0x2500U ^ (case_index << 1U));
    before_fsw = cpu.fsw;
    st32(errno_cell, initial_guest_errno);

    errno = EILSEQ;
    gpush(&cpu, 0xA7A20000U | (slot_rva & 0xFFFFU));
    guest_call(&cpu, token);
    result_bits = rounding_double_bits(*fst(&cpu, 0));
    ok = !cpu.fault && cpu.esp == saved_esp && errno == EILSEQ &&
         cpu.st_top == ((entry_top + 1U) & 7U) &&
         cpu.fsw == before_fsw &&
         rounding_double_bits(*fst(&cpu, 1)) == sentinel0 &&
         rounding_double_bits(*fst(&cpu, 2)) == sentinel1 &&
         memcmp(cpu.x, before_xmm, sizeof before_xmm) == 0 &&
         ld32(errno_cell) == (expected_errno != 0
                              ? (uint32_t)expected_errno
                              : initial_guest_errno) &&
         (double_bits_are_nan(expected_bits)
          ? double_bits_are_nan(result_bits)
          : result_bits == expected_bits);
    errno = saved_host_errno;
    return ok;
}

/* `_CIatan2` and `_CIfmod` are compiler helpers with an x87-only binary ABI:
 * wrappers push y then x, so import entry is ST0=x/ST1=y; one result replaces
 * both and no cdecl argument bytes exist.  Freeze every thunk caller and the
 * independently shaped wrappers before exercising stack order, ring depth,
 * preserved sentinels, signed zero, infinities, NaNs and errno isolation. */
static int check_guest_ci_binary_math(void)
{
    static const uint8_t atan2_float_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x08,0xf3,0x0f,0x5a,
        0xc0,0xf2,0x0f,0x11,0x04,0x24,0x0f,0x57,0xc0,0xdd,0x04,0x24,
        0xf3,0x0f,0x5a,0xc1,0xf2,0x0f,0x11,0x04,0x24,0xdd,0x04,0x24,
        0xe8,0xb5,0x48,0x5a,0x00,0xdd,0x1c,0x24,0xf2,0x0f,0x10,0x04,
        0x24,0x66,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t atan2_x87_wrapper_a[] = {
        0xdd,0x44,0x24,0x40,0xf3,0x0f,0x5a,0xc0,0xf2,0x0f,0x11,0x44,
        0x24,0x48,0xdd,0x44,0x24,0x48,0xe8,0x40,0x80,0x4f,0x00,0x8b,
        0x47,0x70,0xdd,0x5c,0x24,0x48,0xf2,0x0f,0x10,0x44,0x24,0x48,
        0x66,0x0f,0x5a,0xc0
    };
    static const uint8_t atan2_x87_wrapper_b[] = {
        0xdd,0x44,0x24,0x30,0x0f,0x5a,0xc1,0xf2,0x0f,0x11,0x44,0x24,
        0x30,0xdd,0x44,0x24,0x30,0xe8,0xd0,0x49,0x0f,0x00,0xf3,0x0f,
        0x10,0x44,0x24,0x1c,0x33,0xc0,0xf3,0x0f,0x59,0x05,0x3c,0xa6,
        0x76,0x30,0x89,0x44,0x24,0x28,0xdd,0x5c,0x24,0x30,0xf2,0x0f,
        0x10,0x4c,0x24,0x30,0x66,0x0f,0x5a,0xc9
    };
    static const uint8_t fmod_float_wrapper[] = {
        0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x08,0xf3,0x0f,0x5a,
        0xc0,0xf2,0x0f,0x11,0x04,0x24,0x0f,0x57,0xc0,0xdd,0x04,0x24,
        0xf3,0x0f,0x5a,0xc1,0xf2,0x0f,0x11,0x04,0x24,0xdd,0x04,0x24,
        0xe8,0xfb,0x47,0x5a,0x00,0xdd,0x1c,0x24,0xf2,0x0f,0x10,0x04,
        0x24,0x66,0x0f,0x5a,0xc0,0x8b,0xe5,0x5d,0xc3
    };
    static const uint8_t live_atan2_context[] = {
        0x0f,0x28,0xcd,0x0f,0x28,0xc7,0xe8,0x5f,0x56,0xd0,0xff,
        0xf3,0x0f,0x59,0x05,0x84,0xaa,0x76,0x30
    };
    static const uint64_t atan2_cases[][2] = {
        {0x3ff0000000000000ULL,0x3ff0000000000000ULL}, /* +1,+1 */
        {0x3ff0000000000000ULL,0xbff0000000000000ULL}, /* +1,-1 */
        {0xbff0000000000000ULL,0xbff0000000000000ULL}, /* -1,-1 */
        {0xbff0000000000000ULL,0x3ff0000000000000ULL}, /* -1,+1 */
        {0xbff0000000000000ULL,0x0000000000000000ULL}, /* live: -1,+0 */
        {0x0000000000000000ULL,0x0000000000000000ULL}, /* +0,+0 */
        {0x8000000000000000ULL,0x0000000000000000ULL}, /* -0,+0 */
        {0x0000000000000000ULL,0x8000000000000000ULL}, /* +0,-0 */
        {0x8000000000000000ULL,0x8000000000000000ULL}, /* -0,-0 */
        {0x7ff0000000000000ULL,0x7ff0000000000000ULL}, /* +Inf,+Inf */
        {0x7ff0000000000000ULL,0xfff0000000000000ULL}, /* +Inf,-Inf */
        {0xfff0000000000000ULL,0x7ff0000000000000ULL}, /* -Inf,+Inf */
        {0xfff0000000000000ULL,0xfff0000000000000ULL}, /* -Inf,-Inf */
        {0x7ff8000000001234ULL,0x3ff0000000000000ULL}, /* NaN,+1 */
        {0x3ff0000000000000ULL,0x7ff8000000005678ULL}, /* +1,NaN */
    };
    static const uint64_t fmod_cases[][2] = {
        {0x4016000000000000ULL,0x4000000000000000ULL}, /* +5.5,+2 */
        {0xc016000000000000ULL,0x4000000000000000ULL}, /* -5.5,+2 */
        {0x4016000000000000ULL,0xc000000000000000ULL}, /* +5.5,-2 */
        {0x8000000000000000ULL,0x4008000000000000ULL}, /* -0,+3 */
        {0x4000000000000000ULL,0x7ff0000000000000ULL}, /* +2,+Inf */
        {0x7ff0000000000000ULL,0x4000000000000000ULL}, /* domain */
        {0x3ff0000000000000ULL,0x0000000000000000ULL}, /* domain */
        {0x7ff8000000001234ULL,0x3ff0000000000000ULL}, /* NaN,+1 */
        {0x3ff0000000000000ULL,0x7ff8000000005678ULL}, /* +1,NaN */
    };
    uint32_t atan2_slot = GUEST_IMAGE_BASE + RVA_IAT_CIATAN2;
    uint32_t fmod_slot = GUEST_IMAGE_BASE + RVA_IAT_CIFMOD;
    const char *atan2_by_rva = guest_import_name(RVA_IAT_CIATAN2);
    const char *atan2_by_va = guest_import_name(atan2_slot);
    const char *fmod_by_rva = guest_import_name(RVA_IAT_CIFMOD);
    const char *fmod_by_va = guest_import_name(fmod_slot);
    double saved_st[8];
    xmm_t saved_xmm[8];
    uint8_t saved_top = (uint8_t)cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    uint32_t errno_cell;
    unsigned atan2_count, fmod_count, calls_before, i;
    uint64_t atan2_hash, fmod_hash;
    int call_ok = 1, evidence_ok, values_ok;

    atan2_hash = relative_call_census(RVA_THUNK_CIATAN2, &atan2_count);
    fmod_hash = relative_call_census(RVA_THUNK_CIFMOD, &fmod_count);
    evidence_ok = ld32(atan2_slot) == atan2_slot &&
                  ld32(fmod_slot) == fmod_slot &&
                  atan2_by_rva && atan2_by_va && fmod_by_rva && fmod_by_va &&
                  strcmp(atan2_by_rva, NAME_CIATAN2) == 0 &&
                  strcmp(atan2_by_va, NAME_CIATAN2) == 0 &&
                  strcmp(fmod_by_rva, NAME_CIFMOD) == 0 &&
                  strcmp(fmod_by_va, NAME_CIFMOD) == 0 &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_CIATAN2) == 0xffU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_CIATAN2 + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_CIATAN2 + 2U) ==
                      atan2_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_CIFMOD) == 0xffU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_CIFMOD + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_CIFMOD + 2U) ==
                      fmod_slot &&
                  atan2_count == CIATAN2_CALL_COUNT &&
                  fmod_count == CIFMOD_CALL_COUNT &&
                  atan2_hash == CIATAN2_CALL_FNV64 &&
                  fmod_hash == CIFMOD_CALL_FNV64 &&
                  guest_image_bytes_equal(RVA_CIATAN2_FLOAT_WRAPPER,
                      atan2_float_wrapper, sizeof atan2_float_wrapper) &&
                  guest_image_bytes_equal(RVA_CIATAN2_X87_WRAPPER_A,
                      atan2_x87_wrapper_a, sizeof atan2_x87_wrapper_a) &&
                  guest_image_bytes_equal(RVA_CIATAN2_X87_WRAPPER_B,
                      atan2_x87_wrapper_b, sizeof atan2_x87_wrapper_b) &&
                  guest_image_bytes_equal(RVA_CIFMOD_FLOAT_WRAPPER,
                      fmod_float_wrapper, sizeof fmod_float_wrapper) &&
                  guest_image_bytes_equal(0x00342416U, live_atan2_context,
                      sizeof live_atan2_context);

    memcpy(saved_st, cpu.st, sizeof saved_st);
    memcpy(saved_xmm, cpu.x, sizeof saved_xmm);
    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    values_ok = call_ok && errno_cell != 0U;
    calls_before = g_host_import_calls;
    for (i = 0U; i < sizeof atan2_cases / sizeof atan2_cases[0]; ++i)
        values_ok = call_ci_binary_case(
            RVA_IAT_CIATAN2, atan2, atan2_cases[i][0], atan2_cases[i][1],
            errno_cell, i) && values_ok;
    for (i = 0U; i < sizeof fmod_cases / sizeof fmod_cases[0]; ++i)
        values_ok = call_ci_binary_case(
            RVA_IAT_CIFMOD, fmod, fmod_cases[i][0], fmod_cases[i][1],
            errno_cell, (unsigned)(sizeof atan2_cases /
            sizeof atan2_cases[0]) + i) && values_ok;
    values_ok = values_ok && g_host_import_calls - calls_before ==
        (unsigned)(sizeof atan2_cases / sizeof atan2_cases[0] +
                   sizeof fmod_cases / sizeof fmod_cases[0]);
    memcpy(cpu.st, saved_st, sizeof saved_st);
    memcpy(cpu.x, saved_xmm, sizeof saved_xmm);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT _CI binary x87 ABI     : %s, %s, %u/%u physical calls\n",
           evidence_ok ? "IAT/thunk/wrappers/census exact" :
                         "evidence FAILED",
           values_ok ? "order/depth/sentinels/errno exact" :
                       "values FAILED",
           atan2_count, fmod_count);
    return evidence_ok && values_ok;
}

static uint32_t numeric_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float numeric_float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static int check_guest_nextafterf(void)
{
    static const uint8_t caller_bytes[] = {
        0x83,0xec,0x08,0x8b,0x47,0x04,0xc7,0x44,0x24,0x04,0x00,
        0x00,0x00,0x00,0x66,0x0f,0x6e,0x40,0x30,0x0f,0x5b,0xc0,
        0xf3,0x0f,0x11,0x04,0x24,0xff,0x15,0x50,0x65,0x60,0x30,
        0xd9,0x5d,0xf8,0xf3,0x0f,0x10,0x45,0xf8,0x83,0xc4,0x08
    };
    static const struct {
        uint32_t from_bits;
        uint32_t toward_bits;
        uint32_t result_bits;
        int range_error;
    } cases[] = {
        { 0x3f800000u, 0x00000000u, 0x3f7fffffu, 0 },
        { 0x3f800000u, 0x40000000u, 0x3f800001u, 0 },
        { 0xbf800000u, 0x00000000u, 0xbf7fffffu, 0 },
        { 0x00000000u, 0x3f800000u, 0x00000001u, 1 },
        { 0x80000000u, 0xbf800000u, 0x80000001u, 1 },
        { 0x00000000u, 0x80000000u, 0x80000000u, 0 },
        { 0x7f800000u, 0x00000000u, 0x7f7fffffu, 0 },
        { 0x429e0000u, 0x00000000u, 0x429dffffu, 0 },
        { 0x00000001u, 0x00000000u, 0x00000000u, 1 },
        { 0x7f7fffffu, 0x7f800000u, 0x7f800000u, 1 },
    };
    static const uint64_t preserved_bits = 0x4005bf0a8b145769ULL;
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_NEXTAFTERF;
    const char *by_rva = guest_import_name(RVA_IAT_NEXTAFTERF);
    const char *by_va = guest_import_name(slot);
    uint32_t errno_cell;
    double saved_st[8];
    uint8_t saved_top = (uint8_t)cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    unsigned count, i, byte_index;
    unsigned calls_before;
    uint64_t hash;
    int evidence_ok = 1, values_ok = 1, call_ok = 1;

    hash = absolute_iat_call_census(RVA_IAT_NEXTAFTERF, &count);
    evidence_ok = ld32(slot) == slot && by_rva && by_va &&
                  strcmp(by_rva, NAME_NEXTAFTERF) == 0 &&
                  strcmp(by_va, NAME_NEXTAFTERF) == 0 &&
                  RVA_NEXTAFTERF_WINDOW + 27U == RVA_NEXTAFTERF_CALL &&
                  count == NEXTAFTERF_CALL_COUNT &&
                  hash == NEXTAFTERF_CALL_FNV64;
    for (byte_index = 0U; byte_index < sizeof caller_bytes; ++byte_index)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_NEXTAFTERF_WINDOW + byte_index) ==
                caller_bytes[byte_index];

    memcpy(saved_st, cpu.st, sizeof saved_st);
    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    values_ok = call_ok && errno_cell != 0U;
    calls_before = g_host_import_calls;
    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        uint32_t arguments[2];
        uint32_t initial_errno = 101U + i;
        uint8_t initial_top = (uint8_t)(i & 7U);

        arguments[0] = cases[i].from_bits;
        arguments[1] = cases[i].toward_bits;
        st32(errno_cell, initial_errno);
        cpu.st_top = initial_top;
        *fst(&cpu, 0) = rounding_double_from_bits(preserved_bits);
        (void)call_host_cdecl_args(RVA_IAT_NEXTAFTERF, arguments, 2U,
                                   &call_ok);
        values_ok = values_ok && call_ok &&
                    cpu.st_top == ((initial_top - 1U) & 7U) &&
                    rounding_double_bits(*fst(&cpu, 1)) == preserved_bits &&
                    numeric_float_bits((float)*fst(&cpu, 0)) ==
                        cases[i].result_bits &&
                    ld32(errno_cell) ==
                        (cases[i].range_error ? ERANGE : initial_errno);
        (void)fpop(&cpu);
        values_ok = values_ok && cpu.st_top == initial_top &&
                    rounding_double_bits(*fst(&cpu, 0)) == preserved_bits &&
                    numeric_float_bits(numeric_float_from_bits(
                        cases[i].from_bits)) == cases[i].from_bits;
    }
    values_ok = values_ok &&
                g_host_import_calls - calls_before ==
                    (unsigned)(sizeof cases / sizeof cases[0]);
    memcpy(cpu.st, saved_st, sizeof saved_st);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT nextafterf ABI         : %s, %s, %u physical call\n",
           evidence_ok ? "IAT/name/caller/census exact" : "evidence FAILED",
           values_ok ? "cdecl+x87+errno values exact" : "values FAILED",
           count);
    return evidence_ok && values_ok;
}

static int check_guest_memchr(void)
{
    static const uint8_t caller_bytes[] = {
        0x0f,0xbe,0x16,0x50,0x52,0x51,0x89,0x55,0xfc,0xe8,0xb1,
        0x76,0x4a,0x00,0x8b,0xf8,0x83,0xc4,0x0c,0x85,0xff
    };
    static const uint8_t original[] = {
        0x41,0x00,0xff,0x42,0x41,0x7f,0x42,0xcc
    };
    static const struct {
        uint32_t source_offset;
        uint32_t count;
        uint32_t value;
        int32_t expected_offset;
    } cases[] = {
        { 0U,          8U, 0x41U,  0 },
        { 0U,          8U, 0x00U,  1 },
        { 0U,          8U, 0x1ffU, 2 },
        { 0U,          8U, 0x42U,  3 },
        { 0U,          8U, 0xccU,  7 },
        { 0U,          8U, 0x55U, -1 },
        { 0U,          3U, 0x42U, -1 },
        { 4U,          4U, 0x41U,  4 },
        { UINT32_MAX,  0U, 0x41U, -1 },
        { 0U,          0U, 0x41U, -1 },
    };
    uint8_t buffer[sizeof original];
    uint32_t base = (uint32_t)(uintptr_t)buffer;
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_MEMCHR;
    const char *by_rva = guest_import_name(RVA_IAT_MEMCHR);
    const char *by_va = guest_import_name(slot);
    unsigned count, i, byte_index;
    unsigned calls_before = g_host_import_calls;
    uint64_t hash = relative_call_census(RVA_THUNK_MEMCHR, &count);
    int evidence_ok, values_ok = 1, call_ok = 1;

    evidence_ok = ld32(slot) == slot && by_rva && by_va &&
                  strcmp(by_rva, NAME_MEMCHR) == 0 &&
                  strcmp(by_va, NAME_MEMCHR) == 0 &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_MEMCHR) == 0xffU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_MEMCHR + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_MEMCHR + 2U) == slot &&
                  RVA_MEMCHR_STARTUP_WINDOW + 9U == RVA_MEMCHR_STARTUP_CALL &&
                  count == MEMCHR_CALL_COUNT && hash == MEMCHR_CALL_FNV64;
    for (byte_index = 0U; byte_index < sizeof caller_bytes; ++byte_index)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_MEMCHR_STARTUP_WINDOW + byte_index) ==
                caller_bytes[byte_index];

    memcpy(buffer, original, sizeof buffer);
    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        uint32_t args[3];
        uint32_t expected;
        uint32_t result;
        args[0] = cases[i].source_offset == UINT32_MAX
                ? 0U : base + cases[i].source_offset;
        args[1] = cases[i].value;
        args[2] = cases[i].count;
        expected = cases[i].expected_offset < 0
                 ? 0U : base + (uint32_t)cases[i].expected_offset;
        result = call_host_cdecl_args(RVA_IAT_MEMCHR, args, 3U, &call_ok);
        values_ok = values_ok && call_ok && result == expected &&
                    memcmp(buffer, original, sizeof buffer) == 0;
    }
    values_ok = values_ok &&
                g_host_import_calls - calls_before ==
                    (unsigned)(sizeof cases / sizeof cases[0]);
    printf("VCRUNTIME memchr boundary  : %s, %s, %u physical calls\n",
           evidence_ok ? "IAT/thunk/caller/census exact" : "evidence FAILED",
           values_ok ? "pointer/bounds/cdecl exact" : "values FAILED",
           count);
    return evidence_ok && values_ok;
}

static int check_guest_rtdynamiccast(void)
{
    static const uint8_t live_null_caller[] = {
        0x6a,0x00,0x68,0x38,0xad,0x7e,0x30,0x68,
        0x24,0xac,0x7e,0x30,0x6a,0x00,0xff,0x74,
        0xb7,0x24,0xe8,0xc6,0xee,0x23,0x00,0x8b,
        0xf0,0x83,0xc4,0x14,
    };
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_RTDYNAMICCAST;
    const char *by_rva = guest_import_name(RVA_IAT_RTDYNAMICCAST);
    const char *by_va = guest_import_name(slot);
    uint32_t object[8] = { 0U };
    uint32_t original[8] = { 0U };
    uint32_t object_address = (uint32_t)(uintptr_t)object;
    uint32_t args[5];
    uint32_t result;
    unsigned physical_count, calls_before = g_host_import_calls;
    uint64_t physical_hash = relative_call_census(
        RVA_THUNK_RTDYNAMICCAST, &physical_count);
    int evidence_ok, values_ok = 1, call_ok = 1;

    evidence_ok = ld32(slot) == slot && by_rva && by_va &&
        strcmp(by_rva, NAME_RTDYNAMICCAST) == 0 &&
        strcmp(by_va, NAME_RTDYNAMICCAST) == 0 &&
        ld8(GUEST_IMAGE_BASE + RVA_THUNK_RTDYNAMICCAST) == 0xffU &&
        ld8(GUEST_IMAGE_BASE + RVA_THUNK_RTDYNAMICCAST + 1U) == 0x25U &&
        ld32(GUEST_IMAGE_BASE + RVA_THUNK_RTDYNAMICCAST + 2U) == slot &&
        physical_count == RTDYNAMICCAST_PHYSICAL_CALL_COUNT &&
        physical_hash == RTDYNAMICCAST_PHYSICAL_CALL_FNV64 &&
        guest_image_bytes_equal(0x003ad46fu, live_null_caller,
                                (unsigned)sizeof live_null_caller) &&
        ld32(VA_RTTI_GRID_ENTITY_DOOR_VFPTR - 4U) ==
            GUEST_IMAGE_BASE + 0x0076dc00u &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc00u) == 0U &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc04u) == 0U &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc08u) == 0U &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc0cu) ==
            VA_RTTI_GRID_ENTITY_DOOR_TD &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc10u) ==
            GUEST_IMAGE_BASE + 0x0076dc7cu &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc80u) == 0U &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc84u) == 2U &&
        ld32(GUEST_IMAGE_BASE + 0x0076dc88u) ==
            GUEST_IMAGE_BASE + 0x0076db24u &&
        ld32(GUEST_IMAGE_BASE + 0x0076db24u) ==
            GUEST_IMAGE_BASE + 0x0076db3cu &&
        ld32(GUEST_IMAGE_BASE + 0x0076db28u) ==
            GUEST_IMAGE_BASE + 0x0076dad8u;

    /* NULL must short-circuit before touching even deliberately invalid type
     * pointers, matching the first live Room::MakeDoor call exactly. */
    args[0] = 0U;
    args[1] = 0U;
    args[2] = 0xDEAD0001U;
    args[3] = 0xDEAD0002U;
    args[4] = 0U;
    result = call_host_cdecl_args(RVA_IAT_RTDYNAMICCAST, args, 5U, &call_ok);
    values_ok = values_ok && call_ok && result == 0U;

    object[0] = VA_RTTI_GRID_ENTITY_DOOR_VFPTR;
    memcpy(original, object, sizeof object);
    args[0] = object_address;
    args[1] = 0U;
    args[2] = VA_RTTI_GRID_ENTITY_TD;
    args[3] = VA_RTTI_GRID_ENTITY_DOOR_TD;
    args[4] = 0U;
    result = call_host_cdecl_args(RVA_IAT_RTDYNAMICCAST, args, 5U, &call_ok);
    values_ok = values_ok && call_ok && result == object_address &&
                memcmp(object, original, sizeof object) == 0;

    object[0] = VA_RTTI_GRID_ENTITY_VFPTR;
    memcpy(original, object, sizeof object);
    result = call_host_cdecl_args(RVA_IAT_RTDYNAMICCAST, args, 5U, &call_ok);
    values_ok = values_ok && call_ok && result == 0U &&
                memcmp(object, original, sizeof object) == 0 &&
                g_host_import_calls - calls_before == 3U;

    printf("VCRUNTIME dynamic_cast SI : %s, %s, %u physical / 662 emitted calls\n",
           evidence_ok ? "IAT/thunk/live RTTI exact" : "evidence FAILED",
           values_ok ? "null/success/failure/cdecl exact" : "values FAILED",
           physical_count);
    return evidence_ok && values_ok;
}

static int check_guest_strchr(void)
{
    static const uint32_t call_sites[] = {
        0x003ef96du, 0x00475de8u, 0x00475dfcu, 0x0056b827u, 0x0056b837u,
    };
    static const uint8_t live_caller[] = {
        0x6a,0x2f,0x56,0xff,0x15,0x70,0x64,0x60,0x30,
        0x83,0xc4,0x10,0x85,0xc0,0x75,0x25,
        0x6a,0x5c,0x56,0xff,0x15,0x70,0x64,0x60,0x30,
        0x83,0xc4,0x08,0x85,0xc0,0x75,0x15,
    };
    static const uint8_t original[] = {
        0x61,0x62,0x2d,0x00,0xff,0x78,0x00,
    };
    static const struct {
        uint32_t source_offset;
        uint32_t needle;
        int32_t expected_offset;
    } cases[] = {
        { 0U, 0x61U, 0 },
        { 0U, 0x2dU, 2 },
        { 0U, 0x78U, -1 },
        { 0U, 0x00U, 3 },
        { 0U, 0x12dU, 2 },
        { 4U, 0x1ffU, 4 },
        { 4U, 0x78U, 5 },
        { 4U, 0x00U, 6 },
    };
    uint8_t buffer[sizeof original];
    uint32_t base = (uint32_t)(uintptr_t)buffer;
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_STRCHR;
    const char *by_rva = guest_import_name(RVA_IAT_STRCHR);
    const char *by_va = guest_import_name(slot);
    unsigned physical_count, i;
    uint64_t physical_hash = absolute_iat_call_census(
        RVA_IAT_STRCHR, &physical_count);
    unsigned calls_before = g_host_import_calls;
    int evidence_ok, values_ok = 1, call_ok = 1;

    evidence_ok = ld32(slot) == slot && by_rva && by_va &&
                  strcmp(by_rva, NAME_STRCHR) == 0 &&
                  strcmp(by_va, NAME_STRCHR) == 0 &&
                  physical_count == STRCHR_CALL_COUNT &&
                  physical_hash == STRCHR_CALL_FNV64 &&
                  sizeof call_sites / sizeof call_sites[0] ==
                      STRCHR_CALL_COUNT &&
                  rva_sequence_fnv64(call_sites,
                      (unsigned)(sizeof call_sites / sizeof call_sites[0])) ==
                      STRCHR_CALL_FNV64 &&
                  guest_image_bytes_equal(0x0056b824u, live_caller,
                                            (unsigned)sizeof live_caller);
    for (i = 0U; i < sizeof call_sites / sizeof call_sites[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + call_sites[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + call_sites[i] + 1U) == 0x15U &&
            ld32(GUEST_IMAGE_BASE + call_sites[i] + 2U) == slot;

    memcpy(buffer, original, sizeof buffer);
    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        uint32_t args[2];
        uint32_t expected = cases[i].expected_offset < 0
            ? 0U : base + (uint32_t)cases[i].expected_offset;
        uint32_t result;
        args[0] = base + cases[i].source_offset;
        args[1] = cases[i].needle;
        result = call_host_cdecl_args(RVA_IAT_STRCHR, args, 2U, &call_ok);
        values_ok = values_ok && call_ok && result == expected &&
                    memcmp(buffer, original, sizeof buffer) == 0;
    }
    values_ok = values_ok &&
                g_host_import_calls - calls_before ==
                    (unsigned)(sizeof cases / sizeof cases[0]);
    printf("VCRUNTIME strchr boundary  : %s, %s, %u physical calls\n",
           evidence_ok ? "IAT/sites/live caller exact" : "evidence FAILED",
           values_ok ? "NUL/truncate/pointer/cdecl exact" : "values FAILED",
           physical_count);
    return evidence_ok && values_ok;
}

static int check_guest_strstr(void)
{
    static const uint32_t direct_call_sites[] = {
        0x0054281bu, 0x0059ac59u, 0x0059ac93u,
    };
    static const uint32_t register_load_sites[] = {
        0x005991f7u, 0x0059cc51u,
    };
    static const uint32_t register_call_sites[] = {
        0x00599201u, 0x0059ccdcu,
    };
    static const uint32_t all_call_sites[] = {
        0x0054281bu, 0x00599201u, 0x0059ac59u,
        0x0059ac93u, 0x0059ccdcu,
    };
    static const struct {
        const char *haystack;
        const char *needle;
        int32_t expected_offset;
    } cases[] = {
        { "abcabc",  "",       0 },
        { "abcabc",  "abc",    0 },
        { "abcabc",  "bca",    1 },
        { "abcabc",  "cab",    2 },
        { "abcabc",  "bc",     1 },
        { "abcabc",  "abcd",  -1 },
        { "abcabc",  "z",     -1 },
        { "",        "",       0 },
        { "",        "a",     -1 },
        { "abababa", "ababa",  0 },
        { "abababa", "babab",  1 },
        { "aaaaab",  "aaab",   2 },
    };
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_STRSTR;
    const char *by_rva = guest_import_name(RVA_IAT_STRSTR);
    const char *by_va = guest_import_name(slot);
    unsigned direct_count, reference_count, i;
    uint64_t direct_hash = absolute_iat_call_census(
        RVA_IAT_STRSTR, &direct_count);
    uint64_t reference_hash = absolute_iat_reference_census(
        RVA_IAT_STRSTR, &reference_count);
    unsigned calls_before = g_host_import_calls;
    int evidence_ok, values_ok = 1, call_ok = 1;

    evidence_ok = ld32(slot) == slot && by_rva && by_va &&
        strcmp(by_rva, NAME_STRSTR) == 0 &&
        strcmp(by_va, NAME_STRSTR) == 0 &&
        direct_count == STRSTR_DIRECT_CALL_COUNT &&
        direct_hash == STRSTR_DIRECT_CALL_FNV64 &&
        reference_count == STRSTR_REFERENCE_COUNT &&
        reference_hash == STRSTR_REFERENCE_FNV64 &&
        sizeof direct_call_sites / sizeof direct_call_sites[0] ==
            STRSTR_DIRECT_CALL_COUNT &&
        sizeof all_call_sites / sizeof all_call_sites[0] ==
            STRSTR_ALL_CALL_COUNT &&
        rva_sequence_fnv64(direct_call_sites, STRSTR_DIRECT_CALL_COUNT) ==
            STRSTR_DIRECT_CALL_FNV64 &&
        rva_sequence_fnv64(all_call_sites, STRSTR_ALL_CALL_COUNT) ==
            STRSTR_ALL_CALL_FNV64;
    for (i = 0U; i < STRSTR_DIRECT_CALL_COUNT; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + direct_call_sites[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + direct_call_sites[i] + 1U) == 0x15U &&
            ld32(GUEST_IMAGE_BASE + direct_call_sites[i] + 2U) == slot;
    for (i = 0U; i < 2U; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + register_load_sites[i]) == 0x8bU &&
            ld8(GUEST_IMAGE_BASE + register_load_sites[i] + 1U) == 0x1dU &&
            ld32(GUEST_IMAGE_BASE + register_load_sites[i] + 2U) == slot &&
            ld8(GUEST_IMAGE_BASE + register_call_sites[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + register_call_sites[i] + 1U) == 0xd3U;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        uint32_t args[2];
        uint32_t base = (uint32_t)(uintptr_t)cases[i].haystack;
        uint32_t expected = cases[i].expected_offset < 0
            ? 0U : base + (uint32_t)cases[i].expected_offset;
        uint32_t result;
        args[0] = base;
        args[1] = (uint32_t)(uintptr_t)cases[i].needle;
        result = call_host_cdecl_args(RVA_IAT_STRSTR, args, 2U, &call_ok);
        values_ok = values_ok && call_ok && result == expected;
    }
    values_ok = values_ok &&
        g_host_import_calls - calls_before ==
            (unsigned)(sizeof cases / sizeof cases[0]);
    printf("VCRUNTIME strstr boundary  : %s, %s, %u direct / %u total calls\n",
           evidence_ok ? "IAT/direct+register census exact" :
                         "evidence FAILED",
           values_ok ? "empty/repeat/pointer/cdecl exact" : "values FAILED",
           direct_count, STRSTR_ALL_CALL_COUNT);
    return evidence_ok && values_ok;
}

static void store_guest_text(uint32_t address, const char *text)
{
    do {
        st8(address++, (uint8_t)*text);
    } while (*text++);
}

static int check_guest_numeric_conversions(void)
{
    static const struct {
        const char *text;
        int32_t result;
        uint32_t initial_errno;
        uint32_t final_errno;
    } atoi_cases[] = {
        { "",                   0,           71U, 71U },
        { "\t\r\n +42xyz",        42,          72U, 72U },
        { "-17tail",           -17,          73U, 73U },
        { "+-3",                 0,           74U, 74U },
        { "2147483647",         INT32_MAX,   75U, 75U },
        { "2147483648",         INT32_MAX,   76U, ERANGE },
        { "-2147483648",        INT32_MIN,   77U, 77U },
        { "-2147483649",        INT32_MIN,   78U, ERANGE },
    };
    static const struct {
        const char *text;
        uint64_t result_bits;
    } atof_cases[] = {
        { "\t -12.5e2tail",      0xc093880000000000ULL },
        { "+.25E+2x",           0x4039000000000000ULL },
        { "1e",                 0x3ff0000000000000ULL },
        { "-0",                 0x8000000000000000ULL },
        { "3.141592653589793",  0x400921fb54442d18ULL },
        { "1e309",              0x7ff0000000000000ULL },
    };
    static const uint64_t x87_sentinel = 0x4005bf0a8b145769ULL;
    uint32_t atoi_slot = GUEST_IMAGE_BASE + RVA_IAT_ATOI;
    uint32_t atof_slot = GUEST_IMAGE_BASE + RVA_IAT_ATOF;
    const char *atoi_by_rva = guest_import_name(RVA_IAT_ATOI);
    const char *atoi_by_va = guest_import_name(atoi_slot);
    const char *atof_by_rva = guest_import_name(RVA_IAT_ATOF);
    const char *atof_by_va = guest_import_name(atof_slot);
    uint32_t saved_esp = cpu.esp;
    uint32_t text_address = saved_esp - 8192U;
    uint32_t errno_cell;
    double saved_st[8];
    uint8_t saved_top = (uint8_t)cpu.st_top;
    uint16_t saved_fsw = cpu.fsw;
    unsigned atoi_count, atof_count, i;
    unsigned calls_before = g_host_import_calls;
    unsigned faults_before;
    uint64_t atoi_hash, atof_hash;
    int evidence_ok, values_ok = 1, guard_ok, call_ok = 1, stopped;

    atoi_hash = absolute_iat_call_census(RVA_IAT_ATOI, &atoi_count);
    atof_hash = absolute_iat_call_census(RVA_IAT_ATOF, &atof_count);
    evidence_ok = ld32(atoi_slot) == atoi_slot &&
                  ld32(atof_slot) == atof_slot &&
                  atoi_by_rva && atoi_by_va && atof_by_rva && atof_by_va &&
                  strcmp(atoi_by_rva, NAME_ATOI) == 0 &&
                  strcmp(atoi_by_va, NAME_ATOI) == 0 &&
                  strcmp(atof_by_rva, NAME_ATOF) == 0 &&
                  strcmp(atof_by_va, NAME_ATOF) == 0 &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL - 1U) == 0x51U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL) == 0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 1U) == 0x15U &&
                  ld32(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 2U) == atoi_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 6U) == 0x8BU &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 7U) == 0xF8U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 8U) == 0x83U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 9U) == 0xC4U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOI_ABI_CALL + 10U) == 0x04U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL - 1U) == 0x51U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL) == 0xFFU &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 1U) == 0x15U &&
                  ld32(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 2U) == atof_slot &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 6U) == 0xD9U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 7U) == 0x5DU &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 8U) == 0x90U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 9U) == 0x83U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 10U) == 0xC4U &&
                  ld8(GUEST_IMAGE_BASE + RVA_ATOF_ABI_CALL + 11U) == 0x04U &&
                  atoi_count == ATOI_CALL_COUNT &&
                  atof_count == ATOF_CALL_COUNT &&
                  atoi_hash == ATOI_CALL_FNV64 &&
                  atof_hash == ATOF_CALL_FNV64;

    memcpy(saved_st, cpu.st, sizeof saved_st);
    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    values_ok = call_ok && errno_cell != 0U;
    for (i = 0U; i < sizeof atoi_cases / sizeof atoi_cases[0]; ++i) {
        uint32_t argument = text_address;
        uint32_t result;
        store_guest_text(text_address, atoi_cases[i].text);
        st32(errno_cell, atoi_cases[i].initial_errno);
        cpu.st_top = 3;
        *fst(&cpu, 0) = rounding_double_from_bits(x87_sentinel);
        result = call_host_cdecl_args(RVA_IAT_ATOI, &argument, 1U, &call_ok);
        values_ok = values_ok && call_ok &&
                    (int32_t)result == atoi_cases[i].result &&
                    ld32(errno_cell) == atoi_cases[i].final_errno &&
                    cpu.st_top == 3 &&
                    rounding_double_bits(*fst(&cpu, 0)) == x87_sentinel;
    }
    for (i = 0U; i < sizeof atof_cases / sizeof atof_cases[0]; ++i) {
        uint32_t argument = text_address;
        store_guest_text(text_address, atof_cases[i].text);
        st32(errno_cell, 91U);
        cpu.st_top = 5;
        *fst(&cpu, 0) = rounding_double_from_bits(x87_sentinel);
        (void)call_host_cdecl_args(RVA_IAT_ATOF, &argument, 1U, &call_ok);
        values_ok = values_ok && call_ok && cpu.st_top == 4 &&
                    rounding_double_bits(*fst(&cpu, 0)) ==
                        atof_cases[i].result_bits &&
                    rounding_double_bits(*fst(&cpu, 1)) == x87_sentinel &&
                    ld32(errno_cell) == 91U;
        (void)fpop(&cpu);
    }

    guarded_numeric_token = ld32(atoi_slot);
    continued_after_guarded_numeric = 0;
    faults_before = g_fault_count;
    stopped = guest_run_until_stop(&cpu, call_guarded_numeric);
    guard_ok = stopped == GUEST_RUN_FAULT && cpu.fault &&
               strcmp(cpu.fault, "atoi received a null guest pointer") == 0 &&
               cpu.fault_addr == 0U && !continued_after_guarded_numeric &&
               cpu.esp == saved_esp - 8U &&
               ld32(cpu.esp) == 0xABC064D8U &&
               g_fault_count == faults_before + 1U;
    cpu.esp = saved_esp;
    cpu.fault = NULL;
    cpu.fault_addr = 0U;
    memcpy(cpu.st, saved_st, sizeof saved_st);
    cpu.st_top = saved_top;
    cpu.fsw = saved_fsw;

    printf("CRT atoi/atof PE evidence  : %s, %u/%u direct calls\n",
           evidence_ok ? "IAT/names/census/caller ABI exact" : "FAILED",
           atoi_count, atof_count);
    printf("CRT numeric conversions    : %s, %u import calls\n",
           values_ok && guard_ok && g_host_import_calls - calls_before == 16U
               ? "C-locale values/errno/EAX/x87/ESP/null guard exact"
               : "FAILED",
           g_host_import_calls - calls_before);
    return evidence_ok && values_ok && guard_ok &&
           g_host_import_calls - calls_before == 16U;
}

static int check_guest_scheduler_imports(void)
{
    static const uint32_t sleep_direct_sites[] = {
        0x00476c08u, 0x0048bea9u, 0x00562e98u, 0x00598b15u,
        0x005ad302u
    };
    static const struct {
        uint32_t load_rva;
        uint32_t call_rva;
        uint8_t load_modrm;
        uint8_t call_modrm;
    } sleep_register_sites[] = {
        { 0x0055e3dfu, 0x0055e3eau, 0x3dU, 0xd7U }, /* EDI */
        { 0x00562e35u, 0x00562e45u, 0x3dU, 0xd7U }, /* EDI */
        { 0x00569e01u, 0x00569e50u, 0x35U, 0xd6U }, /* ESI */
        { 0x0056e73eu, 0x0056e753u, 0x3dU, 0xd7U }, /* EDI */
        { 0x005a565fu, 0x005a5667u, 0x35U, 0xd6U }, /* ESI */
        { 0x005a7720u, 0x005a772bu, 0x1dU, 0xd3U }, /* EBX */
        { 0x005a7795u, 0x005a77a5u, 0x3dU, 0xd7U }, /* EDI */
        { 0x005c2e07u, 0x005c2ea1u, 0x3dU, 0xd7U }, /* EDI */
    };
    static const uint32_t yield_call_sites[] = {
        0x005e3d5au, 0x005e4adau, 0x005e5ce1u, 0x005e606au,
        0x005e60bbu, 0x005e629bu, 0x005e634cu, 0x005e6371u
    };
    uint32_t sleep_slot = GUEST_IMAGE_BASE + RVA_IAT_SLEEP;
    uint32_t yield_slot = GUEST_IMAGE_BASE + RVA_IAT_THRD_YIELD;
    const char *sleep_by_rva = guest_import_name(RVA_IAT_SLEEP);
    const char *sleep_by_va = guest_import_name(sleep_slot);
    const char *yield_by_rva = guest_import_name(RVA_IAT_THRD_YIELD);
    const char *yield_by_va = guest_import_name(yield_slot);
    unsigned direct_count, reference_count, yield_count, i;
    uint64_t direct_hash = absolute_iat_call_census(
        RVA_IAT_SLEEP, &direct_count);
    uint64_t reference_hash = absolute_iat_reference_census(
        RVA_IAT_SLEEP, &reference_count);
    uint64_t yield_hash = relative_call_census(
        RVA_THUNK_THRD_YIELD, &yield_count);
    unsigned calls_before = g_host_import_calls;
    uint32_t zero = 0U;
    int evidence_ok, abi_ok = 1, call_ok = 1;

    evidence_ok = ld32(sleep_slot) == sleep_slot &&
                  ld32(yield_slot) == yield_slot &&
                  sleep_by_rva && sleep_by_va &&
                  yield_by_rva && yield_by_va &&
                  strcmp(sleep_by_rva, NAME_SLEEP) == 0 &&
                  strcmp(sleep_by_va, NAME_SLEEP) == 0 &&
                  strcmp(yield_by_rva, NAME_THRD_YIELD) == 0 &&
                  strcmp(yield_by_va, NAME_THRD_YIELD) == 0 &&
                  direct_count == SLEEP_DIRECT_CALL_COUNT &&
                  direct_hash == SLEEP_DIRECT_CALL_FNV64 &&
                  reference_count == SLEEP_REFERENCE_COUNT &&
                  reference_hash == SLEEP_REFERENCE_FNV64 &&
                  yield_count == THRD_YIELD_CALL_COUNT &&
                  yield_hash == THRD_YIELD_CALL_FNV64 &&
                  sizeof sleep_direct_sites / sizeof sleep_direct_sites[0] ==
                      SLEEP_DIRECT_CALL_COUNT &&
                  sizeof sleep_direct_sites / sizeof sleep_direct_sites[0] +
                      sizeof sleep_register_sites /
                          sizeof sleep_register_sites[0] ==
                      SLEEP_REFERENCE_COUNT &&
                  sizeof yield_call_sites / sizeof yield_call_sites[0] ==
                      THRD_YIELD_CALL_COUNT &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_THRD_YIELD) == 0xffU &&
                  ld8(GUEST_IMAGE_BASE + RVA_THUNK_THRD_YIELD + 1U) == 0x25U &&
                  ld32(GUEST_IMAGE_BASE + RVA_THUNK_THRD_YIELD + 2U) ==
                      yield_slot;

    for (i = 0U;
         i < sizeof sleep_direct_sites / sizeof sleep_direct_sites[0]; ++i) {
        uint32_t site = sleep_direct_sites[i];
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + site) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + site + 1U) == 0x15U &&
            ld32(GUEST_IMAGE_BASE + site + 2U) == sleep_slot;
    }
    for (i = 0U;
         i < sizeof sleep_register_sites /
                 sizeof sleep_register_sites[0]; ++i) {
        uint32_t load = sleep_register_sites[i].load_rva;
        uint32_t call = sleep_register_sites[i].call_rva;
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + load) == 0x8bU &&
            ld8(GUEST_IMAGE_BASE + load + 1U) ==
                sleep_register_sites[i].load_modrm &&
            ld32(GUEST_IMAGE_BASE + load + 2U) == sleep_slot &&
            ld8(GUEST_IMAGE_BASE + call) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + call + 1U) ==
                sleep_register_sites[i].call_modrm;
    }
    for (i = 0U;
         i < sizeof yield_call_sites / sizeof yield_call_sites[0]; ++i) {
        uint32_t site = yield_call_sites[i];
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + site) == 0xe8U &&
            site + 5U + ld32(GUEST_IMAGE_BASE + site + 1U) ==
                RVA_THUNK_THRD_YIELD;
    }

    /* These are void interfaces: exercise only their scheduling action and
     * exact stack ownership, never turn the incidental EAX bits into an ABI. */
    (void)call_host_stdcall_args(RVA_IAT_SLEEP, &zero, 1U, &call_ok);
    (void)call_host_cdecl_args(RVA_IAT_THRD_YIELD, NULL, 0U, &call_ok);
    abi_ok = call_ok && g_host_import_calls - calls_before == 2U;

    printf("Scheduler PE evidence       : %s, %u direct/%u total Sleep, %u yield\n",
           evidence_ok ? "IAT/names/load-call/thunk/census exact" : "FAILED",
           direct_count, reference_count, yield_count);
    printf("Scheduler import ABI        : %s, %u import calls\n",
           abi_ok ? "Sleep(0) stdcall + yield cdecl ESP exact" : "FAILED",
           g_host_import_calls - calls_before);
    return evidence_ok && abi_ok;
}

static int check_guest_window_icon_imports(void)
{
    typedef int (WINAPI *native_metrics_fn)(int);
    static const struct {
        uint32_t rva;
        uint8_t opcode;
        uint8_t modrm;
    } metric_references[] = {
        { 0x0048112fu, 0x8bU, 0x35U },
        { 0x004a4b7fu, 0x8bU, 0x35U },
        { 0x004a4bafu, 0xffU, 0x15U },
        { 0x004a4bb8u, 0xffU, 0x15U },
        { 0x004a4bebu, 0xffU, 0x15U },
        { 0x004a4bf4u, 0xffU, 0x15U },
    };
    static const uint32_t metric_register_calls[] = {
        0x00481137u, 0x0048113du, 0x004a4b89u,
        0x004a4b8eu, 0x004a4bc5u, 0x004a4bcau,
    };
    static const uint32_t metric_calls[] = {
        0x00481137u, 0x0048113du, 0x004a4b89u, 0x004a4b8eu,
        0x004a4bafu, 0x004a4bb8u, 0x004a4bc5u, 0x004a4bcau,
        0x004a4bebu, 0x004a4bf4u,
    };
    static const uint32_t load_references[] = {
        0x004a4b9fu, 0x004a4bdbu,
    };
    static const uint32_t load_calls[] = {
        0x004a4ba6u, 0x004a4be2u, 0x004a4c04u,
    };
    static const uint32_t send_references[] = { 0x004a4c09u };
    static const uint32_t send_calls[] = { 0x004a4c18u, 0x004a4c25u };
    static const uint8_t screen_window[] = {
        0x8b,0x35,0xc8,0x63,0x60,0x30,0x6a,0x00,
        0xff,0xd6,0x6a,0x01,0x8b,0xf8,0xff,0xd6,
    };
    static const uint8_t alternate_icon_window[] = {
        0x6a,0x68,0x6a,0x00,0xff,0xd7,0x8b,0x35,
        0xbc,0x63,0x60,0x30,0x50,0xff,0xd6,
    };
    static const uint8_t live_icon_window[] = {
        0x50,0x6a,0x01,0x6a,0x65,0x6a,0x00,0xff,0xd7,
        0x8b,0x35,0xbc,0x63,0x60,0x30,0x50,0xff,0xd6,
    };
    static const uint8_t send_window[] = {
        0x8b,0x7d,0xb8,0x8b,0x35,0xc0,0x63,0x60,0x30,
        0x50,0x6a,0x00,0x68,0x80,0x00,0x00,0x00,0x57,0xff,0xd6,
        0xff,0x75,0xb4,0x6a,0x01,0x68,0x80,0x00,0x00,0x00,0x57,
        0xff,0xd6,
    };
    static const int metric_indices[] = {
        SM_CXSCREEN, SM_CYSCREEN, SM_CXICON,
        SM_CYICON, SM_CXSMICON, SM_CYSMICON,
    };
    uint32_t metric_slot = GUEST_IMAGE_BASE + RVA_IAT_GET_SYSTEM_METRICS;
    uint32_t load_slot = GUEST_IMAGE_BASE + RVA_IAT_LOAD_IMAGE_A;
    uint32_t send_slot = GUEST_IMAGE_BASE + RVA_IAT_SEND_MESSAGE_A;
    const char *metric_rva_name = guest_import_name(RVA_IAT_GET_SYSTEM_METRICS);
    const char *metric_va_name = guest_import_name(metric_slot);
    const char *load_rva_name = guest_import_name(RVA_IAT_LOAD_IMAGE_A);
    const char *load_va_name = guest_import_name(load_slot);
    const char *send_rva_name = guest_import_name(RVA_IAT_SEND_MESSAGE_A);
    const char *send_va_name = guest_import_name(send_slot);
    unsigned metric_reference_count, metric_direct_count;
    unsigned load_reference_count, send_reference_count;
    uint64_t metric_reference_hash = absolute_iat_reference_census(
        RVA_IAT_GET_SYSTEM_METRICS, &metric_reference_count);
    uint64_t metric_direct_hash = absolute_iat_call_census(
        RVA_IAT_GET_SYSTEM_METRICS, &metric_direct_count);
    uint64_t load_reference_hash = absolute_iat_reference_census(
        RVA_IAT_LOAD_IMAGE_A, &load_reference_count);
    uint64_t send_reference_hash = absolute_iat_reference_census(
        RVA_IAT_SEND_MESSAGE_A, &send_reference_count);
    uint32_t metrics[sizeof metric_indices / sizeof metric_indices[0]];
    uint32_t icons[2][2];
    uint32_t args[6], module;
    HMODULE native_user32 = LoadLibraryA("user32.dll");
    native_metrics_fn native_metrics = native_user32
        ? (native_metrics_fn)GetProcAddress(native_user32, "GetSystemMetrics")
        : NULL;
    unsigned calls_before = g_host_import_calls;
    unsigned i, resource;
    int evidence_ok = 1, values_ok = native_metrics != NULL, call_ok = 1;

    evidence_ok = ld32(metric_slot) == metric_slot &&
                  ld32(load_slot) == load_slot && ld32(send_slot) == send_slot &&
                  metric_rva_name && metric_va_name &&
                  load_rva_name && load_va_name && send_rva_name && send_va_name &&
                  strcmp(metric_rva_name, NAME_GET_SYSTEM_METRICS) == 0 &&
                  strcmp(metric_va_name, NAME_GET_SYSTEM_METRICS) == 0 &&
                  strcmp(load_rva_name, NAME_LOAD_IMAGE_A) == 0 &&
                  strcmp(load_va_name, NAME_LOAD_IMAGE_A) == 0 &&
                  strcmp(send_rva_name, NAME_SEND_MESSAGE_A) == 0 &&
                  strcmp(send_va_name, NAME_SEND_MESSAGE_A) == 0 &&
                  metric_reference_count == GET_SYSTEM_METRICS_REFERENCE_COUNT &&
                  metric_reference_hash == GET_SYSTEM_METRICS_REFERENCE_FNV64 &&
                  metric_direct_count == GET_SYSTEM_METRICS_DIRECT_COUNT &&
                  metric_direct_hash == GET_SYSTEM_METRICS_DIRECT_FNV64 &&
                  load_reference_count == LOAD_IMAGE_A_REFERENCE_COUNT &&
                  load_reference_hash == LOAD_IMAGE_A_REFERENCE_FNV64 &&
                  send_reference_count == SEND_MESSAGE_A_REFERENCE_COUNT &&
                  send_reference_hash == SEND_MESSAGE_A_REFERENCE_FNV64 &&
                  sizeof metric_calls / sizeof metric_calls[0] ==
                      GET_SYSTEM_METRICS_CALL_COUNT &&
                  rva_sequence_fnv64(metric_calls,
                      (unsigned)(sizeof metric_calls / sizeof metric_calls[0])) ==
                      GET_SYSTEM_METRICS_CALL_FNV64 &&
                  sizeof load_calls / sizeof load_calls[0] ==
                      LOAD_IMAGE_A_CALL_COUNT &&
                  rva_sequence_fnv64(load_calls,
                      (unsigned)(sizeof load_calls / sizeof load_calls[0])) ==
                      LOAD_IMAGE_A_CALL_FNV64 &&
                  sizeof send_calls / sizeof send_calls[0] ==
                      SEND_MESSAGE_A_CALL_COUNT &&
                  rva_sequence_fnv64(send_calls,
                      (unsigned)(sizeof send_calls / sizeof send_calls[0])) ==
                      SEND_MESSAGE_A_CALL_FNV64 &&
                  guest_image_bytes_equal(0x0048112fu, screen_window,
                      (unsigned)sizeof screen_window) &&
                  guest_image_bytes_equal(0x004a4b99u, alternate_icon_window,
                      (unsigned)sizeof alternate_icon_window) &&
                  guest_image_bytes_equal(0x004a4bd2u, live_icon_window,
                      (unsigned)sizeof live_icon_window) &&
                  guest_image_bytes_equal(0x004a4c06u, send_window,
                      (unsigned)sizeof send_window) &&
                  ld8(GUEST_IMAGE_BASE + 0x004a4b7du) == 0x85U &&
                  ld8(GUEST_IMAGE_BASE + 0x004a4b7eu) == 0xf6U &&
                  ld8(GUEST_IMAGE_BASE + 0x004a4b87u) == 0x7eU &&
                  ld8(GUEST_IMAGE_BASE + 0x004a4b88u) == 0x3cU &&
                  ld8(GUEST_IMAGE_BASE + 0x004a4bc3u) == 0xebU &&
                  ld8(GUEST_IMAGE_BASE + 0x004a4bc4u) == 0x3aU;

    for (i = 0U; i < sizeof metric_references / sizeof metric_references[0]; ++i) {
        uint32_t site = metric_references[i].rva;
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + site) == metric_references[i].opcode &&
            ld8(GUEST_IMAGE_BASE + site + 1U) == metric_references[i].modrm &&
            ld32(GUEST_IMAGE_BASE + site + 2U) == metric_slot;
    }
    for (i = 0U;
         i < sizeof metric_register_calls / sizeof metric_register_calls[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + metric_register_calls[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + metric_register_calls[i] + 1U) == 0xd6U;
    for (i = 0U; i < sizeof load_references / sizeof load_references[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + load_references[i]) == 0x8bU &&
            ld8(GUEST_IMAGE_BASE + load_references[i] + 1U) == 0x35U &&
            ld32(GUEST_IMAGE_BASE + load_references[i] + 2U) == load_slot;
    for (i = 0U; i < sizeof load_calls / sizeof load_calls[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + load_calls[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + load_calls[i] + 1U) == 0xd6U;
    for (i = 0U; i < sizeof send_references / sizeof send_references[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + send_references[i]) == 0x8bU &&
            ld8(GUEST_IMAGE_BASE + send_references[i] + 1U) == 0x35U &&
            ld32(GUEST_IMAGE_BASE + send_references[i] + 2U) == send_slot;
    for (i = 0U; i < sizeof send_calls / sizeof send_calls[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + send_calls[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + send_calls[i] + 1U) == 0xd6U;

    for (i = 0U; i < sizeof metric_indices / sizeof metric_indices[0]; ++i) {
        args[0] = (uint32_t)metric_indices[i];
        metrics[i] = call_host_stdcall_args(RVA_IAT_GET_SYSTEM_METRICS,
                                             args, 1U, &call_ok);
        values_ok = values_ok && call_ok &&
                    native_metrics &&
                    (int32_t)metrics[i] == native_metrics(metric_indices[i]) &&
                    (int32_t)metrics[i] > 0;
    }
    args[0] = 0U;
    module = call_host_stdcall_args(RVA_IAT_MODULE_HANDLE_A,
                                     args, 1U, &call_ok);
    values_ok = values_ok && call_ok && module == GUEST_IMAGE_BASE;
    for (resource = 0U; resource < 2U; ++resource) {
        args[0] = module;
        args[1] = resource ? 104U : 101U;
        args[2] = IMAGE_ICON;
        args[3] = metrics[2];
        args[4] = metrics[3];
        args[5] = 0U;
        icons[resource][0] = call_host_stdcall_args(
            RVA_IAT_LOAD_IMAGE_A, args, 6U, &call_ok);
        args[3] = metrics[4];
        args[4] = metrics[5];
        icons[resource][1] = call_host_stdcall_args(
            RVA_IAT_LOAD_IMAGE_A, args, 6U, &call_ok);
        values_ok = values_ok && call_ok && icons[resource][0] != 0U &&
                    icons[resource][1] != 0U &&
                    icons[resource][0] != icons[resource][1];

        args[0] = 0U;
        args[1] = WM_SETICON;
        args[2] = ICON_SMALL;
        args[3] = icons[resource][1];
        values_ok = values_ok &&
            call_host_stdcall_args(RVA_IAT_SEND_MESSAGE_A,
                                   args, 4U, &call_ok) == 0U;
        args[2] = ICON_BIG;
        args[3] = icons[resource][0];
        values_ok = values_ok &&
            call_host_stdcall_args(RVA_IAT_SEND_MESSAGE_A,
                                   args, 4U, &call_ok) == 0U;
    }
    values_ok = values_ok && call_ok &&
                icons[0][0] != icons[1][0] &&
                icons[0][1] != icons[1][1] &&
                g_host_import_calls - calls_before == 15U;
    if (native_user32)
        FreeLibrary(native_user32);

    printf("window icon USER32 evidence : %s, %u/%u/%u calls\n",
           evidence_ok ? "IAT/names/refs/windows exact" : "FAILED",
           GET_SYSTEM_METRICS_CALL_COUNT, LOAD_IMAGE_A_CALL_COUNT,
           SEND_MESSAGE_A_CALL_COUNT);
    printf("window icon USER32 ABI      : %s, %u import calls\n",
           values_ok ? "metrics/tokens/WM_SETICON/ESP exact" : "FAILED",
           g_host_import_calls - calls_before);
    return evidence_ok && values_ok;
}

static int check_guest_clipboard_imports(void)
{
    static const uint32_t global_unlock_sites[] = {
        0x00568170u, 0x00568223u,
    };
    static const uint32_t global_lock_sites[] = {
        0x00568160u, 0x0056820du,
    };
    static const uint32_t global_alloc_sites[] = { 0x00568155u };
    static const uint32_t close_clipboard_sites[] = {
        0x005681e1u, 0x00568229u,
    };
    static const uint32_t empty_clipboard_sites[] = { 0x00568176u };
    static const uint32_t get_clipboard_sites[] = { 0x005681d5u };
    static const uint32_t open_clipboard_sites[] = {
        0x00568123u, 0x005681b3u,
    };
    static const uint32_t set_clipboard_sites[] = { 0x0056817fu };
    static const struct clipboard_import_spec {
        uint32_t rva;
        const char *name;
        unsigned call_count;
        uint64_t call_hash;
        const uint32_t *sites;
        unsigned site_count;
    } specs[] = {
        { RVA_IAT_GLOBAL_UNLOCK, NAME_GLOBAL_UNLOCK,
          GLOBAL_UNLOCK_CALL_COUNT, GLOBAL_UNLOCK_CALL_FNV64,
          global_unlock_sites, 2U },
        { RVA_IAT_GLOBAL_LOCK, NAME_GLOBAL_LOCK,
          GLOBAL_LOCK_CALL_COUNT, GLOBAL_LOCK_CALL_FNV64,
          global_lock_sites, 2U },
        { RVA_IAT_GLOBAL_ALLOC, NAME_GLOBAL_ALLOC,
          GLOBAL_ALLOC_CALL_COUNT, GLOBAL_ALLOC_CALL_FNV64,
          global_alloc_sites, 1U },
        { RVA_IAT_CLOSE_CLIPBOARD, NAME_CLOSE_CLIPBOARD,
          CLOSE_CLIPBOARD_CALL_COUNT, CLOSE_CLIPBOARD_CALL_FNV64,
          close_clipboard_sites, 2U },
        { RVA_IAT_EMPTY_CLIPBOARD, NAME_EMPTY_CLIPBOARD,
          EMPTY_CLIPBOARD_CALL_COUNT, EMPTY_CLIPBOARD_CALL_FNV64,
          empty_clipboard_sites, 1U },
        { RVA_IAT_GET_CLIPBOARD_DATA, NAME_GET_CLIPBOARD_DATA,
          GET_CLIPBOARD_DATA_CALL_COUNT, GET_CLIPBOARD_DATA_CALL_FNV64,
          get_clipboard_sites, 1U },
        { RVA_IAT_OPEN_CLIPBOARD, NAME_OPEN_CLIPBOARD,
          OPEN_CLIPBOARD_CALL_COUNT, OPEN_CLIPBOARD_CALL_FNV64,
          open_clipboard_sites, 2U },
        { RVA_IAT_SET_CLIPBOARD_DATA, NAME_SET_CLIPBOARD_DATA,
          SET_CLIPBOARD_DATA_CALL_COUNT, SET_CLIPBOARD_DATA_CALL_FNV64,
          set_clipboard_sites, 1U },
    };
    static const char payload[] = "gift clipboard ABI\n";
    uint32_t args[2];
    uint32_t block = 0U, first_lock = 0U, second_lock = 0U;
    uint32_t set_result = 0U, open_result = 0U, unlock_result = 0U;
    unsigned calls_before = g_host_import_calls;
    unsigned open_attempts = 0U;
    unsigned i, j;
    int call_ok = 1, evidence_ok = 1, values_ok = 1;

    for (i = 0U; i < sizeof specs / sizeof specs[0]; ++i) {
        const struct clipboard_import_spec *spec = &specs[i];
        uint32_t slot = GUEST_IMAGE_BASE + spec->rva;
        const char *by_rva = guest_import_name(spec->rva);
        const char *by_va = guest_import_name(slot);
        unsigned count = 0U;
        uint64_t hash = absolute_iat_call_census(spec->rva, &count);

        evidence_ok = evidence_ok && ld32(slot) == slot && by_rva && by_va &&
                      strcmp(by_rva, spec->name) == 0 &&
                      strcmp(by_va, spec->name) == 0 &&
                      count == spec->call_count &&
                      hash == spec->call_hash &&
                      spec->site_count == spec->call_count &&
                      rva_sequence_fnv64(spec->sites, spec->site_count) ==
                          spec->call_hash;
        for (j = 0U; j < spec->site_count; ++j) {
            uint32_t site = spec->sites[j];
            evidence_ok = evidence_ok &&
                ld8(GUEST_IMAGE_BASE + site) == 0xffU &&
                ld8(GUEST_IMAGE_BASE + site + 1U) == 0x15U &&
                ld32(GUEST_IMAGE_BASE + site + 2U) == slot;
        }
    }
    {
        unsigned reference_count = 0U;
        uint64_t reference_hash = absolute_iat_reference_census(
            RVA_IAT_CLOSE_CLIPBOARD, &reference_count);
        uint32_t close_slot = GUEST_IMAGE_BASE + RVA_IAT_CLOSE_CLIPBOARD;
        evidence_ok = evidence_ok &&
            reference_count == CLOSE_CLIPBOARD_REFERENCE_COUNT &&
            reference_hash == CLOSE_CLIPBOARD_REFERENCE_FNV64 &&
            ld8(GUEST_IMAGE_BASE + 0x00568188u) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + 0x00568189u) == 0x25U &&
            ld32(GUEST_IMAGE_BASE + 0x0056818au) == close_slot;
    }

    args[0] = GMEM_MOVEABLE | GMEM_ZEROINIT;
    args[1] = 64U;
    block = call_host_stdcall_args(
        RVA_IAT_GLOBAL_ALLOC, args, 2U, &call_ok);
    args[0] = block;
    first_lock = call_host_stdcall_args(
        RVA_IAT_GLOBAL_LOCK, args, 1U, &call_ok);
    values_ok = values_ok && call_ok && block != 0U && first_lock != 0U;
    if (first_lock)
        memcpy((void *)(uintptr_t)first_lock, payload, sizeof payload);
    SetLastError(ERROR_SUCCESS);
    unlock_result = call_host_stdcall_args(
        RVA_IAT_GLOBAL_UNLOCK, args, 1U, &call_ok);
    values_ok = values_ok && call_ok && unlock_result == 0U &&
                GetLastError() == ERROR_SUCCESS;
    second_lock = call_host_stdcall_args(
        RVA_IAT_GLOBAL_LOCK, args, 1U, &call_ok);
    values_ok = values_ok && call_ok && second_lock != 0U &&
                memcmp((const void *)(uintptr_t)second_lock,
                       payload, sizeof payload) == 0;
    SetLastError(ERROR_SUCCESS);
    unlock_result = call_host_stdcall_args(
        RVA_IAT_GLOBAL_UNLOCK, args, 1U, &call_ok);
    values_ok = values_ok && call_ok && unlock_result == 0U &&
                GetLastError() == ERROR_SUCCESS;

    /* Exercise failure paths without changing the user's clipboard.  A failed
     * SetClipboardData call does not transfer ownership of `block`. */
    (void)call_host_stdcall_args(
        RVA_IAT_CLOSE_CLIPBOARD, NULL, 0U, &call_ok);
    args[0] = CF_TEXT;
    values_ok = values_ok &&
        call_host_stdcall_args(
            RVA_IAT_GET_CLIPBOARD_DATA, args, 1U, &call_ok) == 0U;
    values_ok = values_ok &&
        call_host_stdcall_args(
            RVA_IAT_EMPTY_CLIPBOARD, NULL, 0U, &call_ok) == 0U;
    args[0] = CF_TEXT;
    args[1] = block;
    set_result = call_host_stdcall_args(
        RVA_IAT_SET_CLIPBOARD_DATA, args, 2U, &call_ok);
    values_ok = values_ok && set_result == 0U;

    /* Clipboard ownership can briefly belong to another desktop process.
     * Retry only the non-mutating open; the exact import-call count below
     * includes every attempt and remains an ABI assertion. */
    for (open_attempts = 0U; open_attempts < 200U;) {
        ++open_attempts;
        args[0] = 0U;
        open_result = call_host_stdcall_args(
            RVA_IAT_OPEN_CLIPBOARD, args, 1U, &call_ok);
        if (open_result)
            break;
        Sleep(5U);
    }
    values_ok = values_ok && open_result != 0U;
    if (open_result) {
        HANDLE native_text;
        uint32_t guest_text, close_result;
        SIZE_T native_size = 0U, guest_size = 0U;
        const void *native_bytes = NULL, *guest_bytes = NULL;
        int contents_equal = 0;
        args[0] = CF_TEXT;
        native_text = GetClipboardData(CF_TEXT);
        guest_text = call_host_stdcall_args(
            RVA_IAT_GET_CLIPBOARD_DATA, args, 1U, &call_ok);
        if (native_text && guest_text) {
            native_size = GlobalSize((HGLOBAL)native_text);
            guest_size = GlobalSize((HGLOBAL)(uintptr_t)guest_text);
            native_bytes = GlobalLock((HGLOBAL)native_text);
            guest_bytes = GlobalLock((HGLOBAL)(uintptr_t)guest_text);
            contents_equal = native_size != 0U &&
                native_size == guest_size && native_bytes && guest_bytes &&
                memcmp(native_bytes, guest_bytes, native_size) == 0;
            if (guest_bytes)
                (void)GlobalUnlock((HGLOBAL)(uintptr_t)guest_text);
            if (native_bytes)
                (void)GlobalUnlock((HGLOBAL)native_text);
        }
        close_result = call_host_stdcall_args(
            RVA_IAT_CLOSE_CLIPBOARD, NULL, 0U, &call_ok);
        if (!contents_equal)
            printf("clipboard content mismatch : native=%u guest=%u bytes\n",
                   (unsigned)native_size, (unsigned)guest_size);
        values_ok = values_ok &&
            contents_equal && close_result != 0U;
    }
    values_ok = values_ok && call_ok &&
        g_host_import_calls - calls_before ==
            9U + open_attempts + (open_result ? 2U : 0U);

    if (block && !set_result)
        values_ok = values_ok && GlobalFree((HGLOBAL)(uintptr_t)block) == NULL;

    printf("clipboard Win32 evidence    : %s, 13 exact physical sites\n",
           evidence_ok ? "IAT/names/census/tail exact" : "FAILED");
    printf("clipboard Win32 ABI         : %s, %u import calls\n",
           values_ok ? "HGLOBAL/lock/stdcall/non-mutating exact" : "FAILED",
           g_host_import_calls - calls_before);
    return evidence_ok && values_ok;
}

static int check_guest_filesystem_crt(void)
{
    /* Independent raw-.text census.  These are all eight physical `remove`
     * sites and their current generated owners, not the stale four-site
     * direct-closure subset:
     *
     *   25e5da (25e3b0), 25f2c7 (25f240), 40e357 (40e150),
     *   469f39 (469d40), 47531b (474ad0), 520b6b (520ac0),
     *   520c22 (520b80), 567cc6 (567cc0).
     *
     * `_access` has one physical site: 596340 (owner 596320). */
    static const uint32_t remove_sites[] = {
        0x0025e5dau, 0x0025f2c7u, 0x0040e357u, 0x00469f39u,
        0x0047531bu, 0x00520b6bu, 0x00520c22u, 0x00567cc6u
    };
    static const uint32_t access_sites[] = { 0x00596340u };
    static const uint8_t remove_abi[] = {
        0x55,0x8b,0xec,0xff,0x75,0x08,0xff,0x15,0xec,0x64,0x60,0x30,
        0x83,0xc4,0x04,0x83,0xf8,0xff,0x0f,0x95,0xc0,0x5d,0xc2,0x04,0x00
    };
    /* MSVC deliberately defers `_access`'s eight-byte caller cleanup until
     * after the following one-argument call: the final add esp,0xc belongs to
     * both calls.  Pinning that real sequence prevents a false stdcall read. */
    static const uint8_t access_abi[] = {
        0x6a,0x00,0x56,0xff,0x15,0xf0,0x64,0x60,0x30,0x83,0xf8,0xff,
        0x56,0x0f,0x95,0xc3,0xe8,0x93,0x49,0x05,0x00,0x83,0xc4,0x0c
    };
    uint32_t remove_slot = GUEST_IMAGE_BASE + RVA_IAT_REMOVE;
    uint32_t access_slot = GUEST_IMAGE_BASE + RVA_IAT_ACCESS;
    const char *remove_by_rva = guest_import_name(RVA_IAT_REMOVE);
    const char *remove_by_va = guest_import_name(remove_slot);
    const char *access_by_rva = guest_import_name(RVA_IAT_ACCESS);
    const char *access_by_va = guest_import_name(access_slot);
    unsigned remove_count, access_count, byte_index, i;
    uint64_t remove_hash = absolute_iat_call_census(
        RVA_IAT_REMOVE, &remove_count);
    uint64_t access_hash = absolute_iat_call_census(
        RVA_IAT_ACCESS, &access_count);
    unsigned calls_before = g_host_import_calls;
    uint32_t errno_cell, args[2], result;
    char path[96] = { 0 };
    FILE *created = NULL;
    int evidence_ok, lifecycle_ok = 1, call_ok = 1;

    evidence_ok = ld32(remove_slot) == remove_slot &&
                  ld32(access_slot) == access_slot &&
                  remove_by_rva && remove_by_va &&
                  access_by_rva && access_by_va &&
                  strcmp(remove_by_rva, NAME_REMOVE) == 0 &&
                  strcmp(remove_by_va, NAME_REMOVE) == 0 &&
                  strcmp(access_by_rva, NAME_ACCESS) == 0 &&
                  strcmp(access_by_va, NAME_ACCESS) == 0 &&
                  remove_count == REMOVE_CALL_COUNT &&
                  access_count == ACCESS_CALL_COUNT &&
                  remove_hash == REMOVE_CALL_FNV64 &&
                  access_hash == ACCESS_CALL_FNV64;
    for (i = 0U; i < sizeof remove_sites / sizeof remove_sites[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + remove_sites[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + remove_sites[i] + 1U) == 0x15U &&
            ld32(GUEST_IMAGE_BASE + remove_sites[i] + 2U) == remove_slot;
    for (i = 0U; i < sizeof access_sites / sizeof access_sites[0]; ++i)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + access_sites[i]) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + access_sites[i] + 1U) == 0x15U &&
            ld32(GUEST_IMAGE_BASE + access_sites[i] + 2U) == access_slot;
    for (byte_index = 0U; byte_index < sizeof remove_abi; ++byte_index)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_REMOVE_ABI_WINDOW + byte_index) ==
                remove_abi[byte_index];
    for (byte_index = 0U; byte_index < sizeof access_abi; ++byte_index)
        evidence_ok = evidence_ok &&
            ld8(GUEST_IMAGE_BASE + RVA_ACCESS_ABI_WINDOW + byte_index) ==
                access_abi[byte_index];

    if (sprintf_s(path, sizeof path, "full_runtime_filesystem_%lu.tmp",
                  (unsigned long)_getpid()) < 0)
        lifecycle_ok = 0;
    if (lifecycle_ok) {
        errno_t open_error;
        (void)remove(path);             /* recover a stale interrupted run */
        open_error = fopen_s(&created, path, "wb");
        if (open_error || !created) {
            lifecycle_ok = 0;
        } else {
            int wrote = fputc('x', created) != EOF;
            int closed = fclose(created) == 0;
            created = NULL;
            if (!wrote || !closed) lifecycle_ok = 0;
        }
    }

    errno_cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &call_ok);
    lifecycle_ok = lifecycle_ok && call_ok && errno_cell != 0U;
    args[0] = (uint32_t)(uintptr_t)path;
    args[1] = 0U;                       /* existence only */

    if (errno_cell) st32(errno_cell, 0x1357U);
    result = call_host_cdecl_args(RVA_IAT_ACCESS, args, 2U, &call_ok);
    lifecycle_ok = lifecycle_ok && call_ok && result == 0U && errno_cell &&
                   ld32(errno_cell) == 0x1357U;

    if (errno_cell) st32(errno_cell, 0x2468U);
    result = call_host_cdecl_args(RVA_IAT_REMOVE, args, 1U, &call_ok);
    lifecycle_ok = lifecycle_ok && call_ok && result == 0U && errno_cell &&
                   ld32(errno_cell) == 0x2468U;

    if (errno_cell) st32(errno_cell, 0x3579U);
    result = call_host_cdecl_args(RVA_IAT_ACCESS, args, 2U, &call_ok);
    lifecycle_ok = lifecycle_ok && call_ok && result == UINT32_MAX &&
                   errno_cell && ld32(errno_cell) == ENOENT;

    if (errno_cell) st32(errno_cell, 0x468aU);
    result = call_host_cdecl_args(RVA_IAT_REMOVE, args, 1U, &call_ok);
    lifecycle_ok = lifecycle_ok && call_ok && result == UINT32_MAX &&
                   errno_cell && ld32(errno_cell) == ENOENT;

    if (created) (void)fclose(created);
    (void)remove(path);                 /* cleanup even after an assertion */
    lifecycle_ok = lifecycle_ok &&
                   g_host_import_calls - calls_before == 5U;

    printf("CRT filesystem PE evidence : %s, %u/%u physical calls\n",
           evidence_ok ? "IAT/names/sites/caller ABI exact" : "FAILED",
           remove_count, access_count);
    printf("CRT filesystem lifecycle   : %s, %u import calls\n",
           lifecycle_ok
               ? "access exists/missing; remove success/missing; errno/ESP exact"
               : "FAILED",
           g_host_import_calls - calls_before);
    return evidence_ok && lifecycle_ok;
}

static int expect_qsort_fault(uint32_t base, uint32_t count, uint32_t width,
                              uint32_t comparator, uint32_t fault_address,
                              const char *fault_text)
{
    uint32_t saved = cpu.esp;
    unsigned faults_before = g_fault_count;
    int stopped, ok;

    guarded_qsort_args[0] = base;
    guarded_qsort_args[1] = count;
    guarded_qsort_args[2] = width;
    guarded_qsort_args[3] = comparator;
    guarded_qsort_token = ld32(GUEST_IMAGE_BASE + RVA_IAT_QSORT);
    continued_after_guarded_qsort = 0;
    stopped = guest_run_until_stop(&cpu, call_guarded_qsort);
    ok = stopped == GUEST_RUN_FAULT && cpu.fault &&
         strcmp(cpu.fault, fault_text) == 0 &&
         cpu.fault_addr == fault_address &&
         !continued_after_guarded_qsort &&
         cpu.esp == saved - 20U && ld32(cpu.esp) == 0xABC06678U &&
         g_fault_count == faults_before + 1U;
    cpu.esp = saved;
    cpu.fault = NULL;
    cpu.fault_addr = 0U;
    return ok;
}

static int check_guest_qsort(void)
{
    static const struct {
        uint32_t call_rva;
        uint32_t comparator_push_rva;
        uint32_t comparator_rva;
        uint8_t width;
    } evidence[] = {
        { 0x005a614fU, 0x005a6146U, 0x0059b480U, 32U },
        { 0x005b6f5aU, 0x005b6f4cU, 0x005b6df0U,  4U },
        { 0x005bc7e8U, 0x005bc7dbU, 0x005b7530U,  4U },
        { 0x005c8603U, 0x005c85f8U, 0x005c84b0U,  4U },
        { 0x005c9dfaU, 0x005c9defU, 0x005c9aa0U,  4U },
        { 0x005d30d1U, 0x005d30c2U, 0x005d2d40U,  4U },
        { 0x005d3341U, 0x005d3332U, 0x005d2d40U,  4U },
    };
    static const int32_t input_keys[] = {
        5, -3, 5, 0, -3, 9, -1, 0
    };
    static const int32_t sorted_keys[] = {
        -3, -3, -1, 0, 0, 5, 5, 9
    };
    uint32_t slot = GUEST_IMAGE_BASE + RVA_IAT_QSORT;
    uint32_t token = ld32(slot);
    const char *by_rva = guest_import_name(RVA_IAT_QSORT);
    const char *by_va = guest_import_name(slot);
    uint32_t callback_address = QSORT_TEST_CALLBACK;
    guest_fn callback_function = qsort_compare_i32;
    uint32_t saved = cpu.esp;
    uint32_t base = saved - 192U;
    uint32_t args[4];
    uint32_t seen = 0U;
    unsigned imports_before = g_host_import_calls;
    unsigned sort_calls, i;
    int evidence_ok, no_callback_ok, sort_ok = 1, guards_ok;

    evidence_ok = token == slot && by_rva && by_va &&
                  strcmp(by_rva, NAME_QSORT) == 0 &&
                  strcmp(by_va, NAME_QSORT) == 0;
    for (i = 0U; i < sizeof evidence / sizeof evidence[0]; ++i) {
        uint32_t call = GUEST_IMAGE_BASE + evidence[i].call_rva;
        uint32_t push = GUEST_IMAGE_BASE + evidence[i].comparator_push_rva;
        guest_fn by_comparator_rva = guest_lookup(evidence[i].comparator_rva);
        guest_fn by_comparator_va = guest_lookup(
            GUEST_IMAGE_BASE + evidence[i].comparator_rva);
        if (ld8(call) != 0xFFU || ld8(call + 1U) != 0x15U ||
            ld32(call + 2U) != slot ||
            ld8(push) != 0x68U ||
            ld32(push + 1U) != GUEST_IMAGE_BASE + evidence[i].comparator_rva ||
            ld8(push + 5U) != 0x6AU ||
            ld8(push + 6U) != evidence[i].width ||
            !by_comparator_rva || by_comparator_rva != by_comparator_va)
            evidence_ok = 0;
    }

    st32(base - 4U, 0x13579BDFU);
    st32(base + 64U, 0x2468ACE0U);
    for (i = 0U; i < 8U; ++i) {
        st32(base + i * 8U, (uint32_t)input_keys[i]);
        st32(base + i * 8U + 4U, i);
    }
    qsort_compare_base = base;
    qsort_compare_end = base + 64U;
    qsort_compare_width = 8U;
    qsort_compare_calls = 0U;
    qsort_compare_bad_pointer = 0;
    guest_register(&callback_address, &callback_function, 1U);

    args[0] = 0U;
    args[1] = 0U;
    args[2] = 8U;
    args[3] = QSORT_TEST_CALLBACK;
    (void)call_host_cdecl_args(RVA_IAT_QSORT, args, 4U, &sort_ok);
    args[0] = base;
    args[1] = 1U;
    (void)call_host_cdecl_args(RVA_IAT_QSORT, args, 4U, &sort_ok);
    no_callback_ok = sort_ok && qsort_compare_calls == 0U &&
                     ld32(base) == (uint32_t)input_keys[0] &&
                     ld32(base + 4U) == 0U;

    qsort_compare_calls = 0U;
    args[1] = 8U;
    (void)call_host_cdecl_args(RVA_IAT_QSORT, args, 4U, &sort_ok);
    sort_calls = qsort_compare_calls;
    for (i = 0U; i < 8U; ++i) {
        int32_t key = (int32_t)ld32(base + i * 8U);
        uint32_t id = ld32(base + i * 8U + 4U);
        if (key != sorted_keys[i] || id >= 8U ||
            (id < 8U && (seen & (1U << id))) ||
            (id < 8U && key != input_keys[id])) {
            sort_ok = 0;
        } else {
            seen |= 1U << id;
        }
    }
    sort_ok = sort_ok && seen == 0xFFU && sort_calls != 0U &&
              !qsort_compare_bad_pointer &&
              ld32(base - 4U) == 0x13579BDFU &&
              ld32(base + 64U) == 0x2468ACE0U;

    guards_ok = 1;
    guards_ok = expect_qsort_fault(
        0U, 2U, 8U, QSORT_TEST_CALLBACK, 0U,
        "qsort received a null base") && guards_ok;
    guards_ok = expect_qsort_fault(
        base, 2U, 0U, QSORT_TEST_CALLBACK, 0U,
        "qsort received a zero element width") && guards_ok;
    guards_ok = expect_qsort_fault(
        base, 2U, 8U, 0U, 0U,
        "qsort received a null comparator") && guards_ok;
    guards_ok = expect_qsort_fault(
        base, 0x80000000U, 8U, QSORT_TEST_CALLBACK, base,
        "qsort guest range overflows 32-bit address space") && guards_ok;
    guards_ok = expect_qsort_fault(
        0xFFFFFFFCU, 2U, 4U, QSORT_TEST_CALLBACK, 0xFFFFFFFCU,
        "qsort guest range overflows 32-bit address space") && guards_ok;
    guest_register_all();
    cpu.esp = saved;

    printf("qsort PE/IAT evidence       : %s, 7 calls / 6 comparators\n",
           evidence_ok ? "exact" : "FAILED");
    printf("qsort guest callback sort   : %s, %u comparisons\n",
           no_callback_ok && sort_ok
               ? "signed duplicates/permutation/canaries exact; 0/1 skipped"
               : "FAILED",
           sort_calls);
    printf("qsort guards + cdecl ABI    : %s, %u import calls\n",
           guards_ok && g_host_import_calls - imports_before == 8U
               ? "null/width/two overflows exact; ESP restored"
               : "FAILED",
           g_host_import_calls - imports_before);
    return evidence_ok && no_callback_ok && sort_ok && guards_ok &&
           g_host_import_calls - imports_before == 8U;
}

static int call_host_cdecl_onearg(uint32_t slot_rva, uint32_t argument,
                                  uint32_t expected_return)
{
    int ok = 1;
    uint32_t result = call_host_cdecl_args(slot_rva, &argument, 1U, &ok);
    return ok && result == expected_return;
}

static int check_guest_initializers(void)
{
    static const uint32_t addresses[] = { 0x00F10010u, 0x00F10020u, 0x00F10030u };
    static const guest_fn functions[] = { init_zero, init_seven, init_after };
    uint32_t saved = cpu.esp;
    uint32_t array = saved - 64U;
    uint32_t token_e = ld32(GUEST_IMAGE_BASE + RVA_IAT_INITTERM_E);
    uint32_t token_v = ld32(GUEST_IMAGE_BASE + RVA_IAT_INITTERM);
    int ok_e, ok_v;

    guest_register(addresses, functions, 3U);
    st32(array + 0U, 0U);
    st32(array + 4U, addresses[0]);
    st32(array + 8U, addresses[1]);
    st32(array + 12U, addresses[2]);

    init_zero_calls = init_seven_calls = init_after_calls = 0;
    cpu.esp = array;
    gpush(&cpu, array + 16U);
    gpush(&cpu, array);
    gpush(&cpu, 0xABC0E001u);
    guest_call(&cpu, token_e);
    ok_e = !cpu.fault && cpu.esp == array - 8U && cpu.eax == 7U &&
           init_zero_calls == 1 && init_seven_calls == 1 && init_after_calls == 0;
    cpu.esp += 8U;                  /* the translated cdecl caller's cleanup */

    init_zero_calls = init_seven_calls = init_after_calls = 0;
    gpush(&cpu, array + 16U);
    gpush(&cpu, array);
    gpush(&cpu, 0xABC0E002u);
    guest_call(&cpu, token_v);
    ok_v = !cpu.fault && cpu.esp == array - 8U &&
           init_zero_calls == 1 && init_seven_calls == 1 && init_after_calls == 1;
    cpu.esp += 8U;

    cpu.esp = saved;
    guest_register_all();
    return ok_e && ok_v;
}

static int check_guest_exit(void)
{
    static const uint32_t addresses[] = {
        0x00F10040u, 0x00F10050u, 0x00F10060u
    };
    static const guest_fn functions[] = {
        exit_callback_a, exit_callback_b, tls_exit_callback
    };
    uint32_t saved = cpu.esp;
    uint32_t faults_before = g_fault_count;
    unsigned calls_before = g_host_import_calls;
    uint32_t arg;
    int call_ok = 1, stopped;
    int no_cleanup_ok, full_cleanup_ok;

    guest_register(addresses, functions, 3U);
    exit_callback_count = 0U;
    memset(exit_callback_order, 0, sizeof exit_callback_order);
    memset(tls_exit_args, 0xff, sizeof tls_exit_args);

    arg = 0U;                    /* UCRT accepts an optional null callback. */
    (void)call_host_cdecl_args(RVA_IAT_CRT_ATEXIT, &arg, 1U, &call_ok);
    arg = addresses[0];
    (void)call_host_cdecl_args(RVA_IAT_CRT_ATEXIT, &arg, 1U, &call_ok);
    arg = addresses[1];
    (void)call_host_cdecl_args(RVA_IAT_CRT_ATEXIT, &arg, 1U, &call_ok);
    arg = addresses[2];
    (void)call_host_cdecl_args(RVA_IAT_TLS_EXIT_REGISTER, &arg, 1U, &call_ok);

    guarded_exit_token = ld32(GUEST_IMAGE_BASE + RVA_IAT__EXIT);
    guarded_exit_code = 9U;
    guarded_exit_return = 0xABC0E901U;
    continued_after_exit = 0;
    cpu.esp = saved;
    stopped = guest_run_until_stop(&cpu, call_guarded_exit);
    no_cleanup_ok = stopped == GUEST_RUN_EXIT &&
                    cpu.stop_kind == GUEST_RUN_EXIT &&
                    cpu.exit_code == 9 && cpu.exit_api &&
                    strcmp(cpu.exit_api, "_exit") == 0 && !cpu.fault &&
                    !continued_after_exit && exit_callback_count == 0U &&
                    cpu.esp == saved - 8U &&
                    ld32(cpu.esp) == guarded_exit_return;

    guarded_exit_token = ld32(GUEST_IMAGE_BASE + RVA_IAT_EXIT);
    guarded_exit_code = 1U;
    guarded_exit_return = 0xABC0E902U;
    continued_after_exit = 0;
    cpu.esp = saved;
    stopped = guest_run_until_stop(&cpu, call_guarded_exit);
    full_cleanup_ok = stopped == GUEST_RUN_EXIT &&
                      cpu.stop_kind == GUEST_RUN_EXIT &&
                      cpu.exit_code == 1 && cpu.exit_api &&
                      strcmp(cpu.exit_api, "exit") == 0 && !cpu.fault &&
                      !continued_after_exit && cpu.esp == saved - 8U &&
                      ld32(cpu.esp) == guarded_exit_return &&
                      exit_callback_count == 3U &&
                      exit_callback_order[0] == 3U &&
                      exit_callback_order[1] == 2U &&
                      exit_callback_order[2] == 1U &&
                      tls_exit_args[0] == 0U &&
                      tls_exit_args[1] == 0U &&
                      tls_exit_args[2] == 0U;

    cpu.esp = saved;
    cpu.exit_api = NULL;
    cpu.exit_code = 0;
    cpu.stop_kind = GUEST_RUN_RETURNED;
    guest_register_all();
    return call_ok && no_cleanup_ok && full_cleanup_ok &&
           g_fault_count == faults_before &&
           g_host_import_calls - calls_before == 6U;
}

/* COPIED from ladder_test.c, not reconstructed. The first version of this
 * file declared `struct { uint32_t seed; }` and a shl/shr/shl reference,
 * because that is what I remembered. Both were wrong, and the run blamed the
 * recompiled code for it:
 *
 *   RNG::Next reads its shift amounts from the OBJECT -- [esi+4], [esi+8],
 *   [esi+0xc] -- not from the global table, and the order is shr, shl, shr.
 *
 * With a one-field struct the function read whatever followed `got` on the
 * stack, and the "reference" it was compared against was a different
 * algorithm. Two wrongs that looked exactly like a translator bug. */
typedef struct { uint32_t seed, s1, s2, s3; } RNG;

static uint32_t ref_next(RNG *r)
{
    uint32_t s = r->seed;
    s ^= s >> (r->s1 & 31);
    s ^= s << (r->s2 & 31);
    s ^= s >> (r->s3 & 31);
    return r->seed = s;
}

static void call_this(void (*fn)(CPU *__restrict), uint32_t self,
                      const uint32_t *args, int nargs)
{
    int i;
    uint32_t save = cpu.esp;
    for (i = nargs - 1; i >= 0; i--) gpush(&cpu, args[i]);
    gpush(&cpu, 0xDEADBEEFu);
    cpu.ecx = self;
    fn(&cpu);
    cpu.esp = save;
}

static void test_string_init(uint32_t string)
{
    st32(string, 0U);
    st32(string + 0x10U, 0U);
    st32(string + 0x14U, 15U);
    st8(string, 0U);
}

static int test_string_assign(uint32_t string, uint32_t bytes, uint32_t length)
{
    uint32_t saved = cpu.esp;
    gpush(&cpu, length);
    gpush(&cpu, bytes);
    gpush(&cpu, 0xABC07C80U);
    cpu.ecx = string;
    sub_00007c80(&cpu);
    return !cpu.fault && cpu.esp == saved;
}

static int test_string_destroy(uint32_t string)
{
    uint32_t saved = cpu.esp;
    gpush(&cpu, 0xABC07BE0U);
    cpu.ecx = string;
    sub_00007be0(&cpu);
    return !cpu.fault && cpu.esp == saved;
}

static int test_vector_destroy(uint32_t vector)
{
    uint32_t saved = cpu.esp;
    gpush(&cpu, 0xABC0F760U);
    cpu.ecx = vector;
    sub_0000f760(&cpu);
    return !cpu.fault && cpu.esp == saved &&
           ld32(vector) == 0U && ld32(vector + 4U) == 0U &&
           ld32(vector + 8U) == 0U;
}

static uint32_t test_string_bytes(uint32_t string)
{
    return ld32(string + 0x14U) < 16U ? string : ld32(string);
}

static int test_split_token(uint32_t vector, uint32_t index,
                            const void *expected, uint32_t length)
{
    uint32_t begin = ld32(vector);
    uint32_t end = ld32(vector + 4U);
    uint32_t item = begin + index * 24U;
    if (!begin || item < begin || item + 24U > end ||
        ld32(item + 0x10U) != length)
        return 0;
    return !length || memcmp((const void *)(uintptr_t)test_string_bytes(item),
                             expected, length) == 0;
}

static int test_split_count(uint32_t vector, uint32_t count)
{
    uint32_t begin = ld32(vector);
    uint32_t end = ld32(vector + 4U);
    return (!count && begin == end) ||
           (begin && end >= begin && end - begin == count * 24U);
}

static int test_split_construct(uint32_t vector, uint32_t source,
                                uint32_t delimiter)
{
    uint32_t saved = cpu.esp;
    uint32_t ebp = cpu.ebp, ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi;
    gpush(&cpu, delimiter);
    gpush(&cpu, 0xABC2597BU);
    cpu.ecx = vector;
    cpu.edx = source;
    sub_002597b0(&cpu);
    return !cpu.fault && cpu.eax == vector &&
           cpu.esp == saved - 4U && ld32(cpu.esp) == delimiter &&
           cpu.ebp == ebp && cpu.ebx == ebx &&
           cpu.esi == esi && cpu.edi == edi;
}

static int test_split_append(uint32_t vector, uint32_t source,
                             uint32_t delimiter)
{
    uint32_t saved = cpu.esp;
    uint32_t ebp = cpu.ebp, ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi;
    gpush(&cpu, vector);
    gpush(&cpu, 0xABC25965U);
    cpu.ecx = source;
    cpu.edx = delimiter;
    sub_00259650(&cpu);
    return !cpu.fault && cpu.eax == vector &&
           cpu.esp == saved - 4U && ld32(cpu.esp) == vector &&
           cpu.ebp == ebp && cpu.ebx == ebx &&
           cpu.esi == esi && cpu.edi == edi;
}

static int check_portable_split(void)
{
    static const unsigned char composite[] =
        ",a,,0123456789abcdef,b,";
    static const unsigned char embedded[] = { 'a', 0, 'b', ',', 'c' };
    static const unsigned char embedded_first[] = { 'a', 0, 'b' };
    static const unsigned char append_input[] = "x,,y,z,w,";
    uint32_t saved = cpu.esp;
    uint32_t raw = saved - 0x3000U;
    uint32_t source = saved - 0x2800U;
    uint32_t vector = saved - 0x2700U;
    int ok = 1;

    ok = ok && guest_lookup(0x00259650U) == sub_00259650 &&
         guest_lookup(0x30259650U) == sub_00259650 &&
         guest_lookup(0x00259650U) != guest_original_00259650 &&
         guest_lookup(0x002597b0U) == sub_002597b0 &&
         guest_lookup(0x302597b0U) == sub_002597b0 &&
         guest_lookup(0x002597b0U) != guest_original_002597b0;

    memcpy((void *)(uintptr_t)raw, composite, sizeof composite - 1U);
    test_string_init(source);
    ok = ok && test_string_assign(source, raw, sizeof composite - 1U);
    st32(vector, 0xDEADBEEFU);
    st32(vector + 4U, 0xA5A5A5A5U);
    st32(vector + 8U, 0x13579BDFU);
    ok = ok && test_split_construct(vector, source, 0x1234002cU);
    cpu.esp = saved;
    ok = ok && test_split_count(vector, 5U) &&
         test_split_token(vector, 0U, "", 0U) &&
         test_split_token(vector, 1U, "a", 1U) &&
         test_split_token(vector, 2U, "", 0U) &&
         test_split_token(vector, 3U, "0123456789abcdef", 16U) &&
         test_split_token(vector, 4U, "b", 1U) &&
         test_vector_destroy(vector) && test_string_destroy(source);

    memcpy((void *)(uintptr_t)raw, embedded, sizeof embedded);
    test_string_init(source);
    ok = ok && test_string_assign(source, raw, sizeof embedded) &&
         test_split_construct(vector, source, ',');
    cpu.esp = saved;
    ok = ok && test_split_count(vector, 2U) &&
         test_split_token(vector, 0U, embedded_first,
                          sizeof embedded_first) &&
         test_split_token(vector, 1U, "c", 1U) &&
         test_vector_destroy(vector);
    ok = ok && test_split_construct(vector, source, 0U);
    cpu.esp = saved;
    ok = ok && test_split_count(vector, 2U) &&
         test_split_token(vector, 0U, "a", 1U) &&
         test_split_token(vector, 1U, "b,c", 3U) &&
         test_vector_destroy(vector) && test_string_destroy(source);

    test_string_init(source);
    ok = ok && test_string_assign(source, raw, 0U) &&
         test_split_construct(vector, source, ',');
    cpu.esp = saved;
    ok = ok && test_split_count(vector, 0U) &&
         test_vector_destroy(vector) && test_string_destroy(source);

    memcpy((void *)(uintptr_t)raw, "pre", 3U);
    test_string_init(source);
    ok = ok && test_string_assign(source, raw, 3U) &&
         test_split_construct(vector, source, ',');
    cpu.esp = saved;
    ok = ok && test_string_destroy(source);
    memcpy((void *)(uintptr_t)raw, append_input, sizeof append_input - 1U);
    test_string_init(source);
    ok = ok && test_string_assign(source, raw, sizeof append_input - 1U) &&
         test_split_append(vector, source, 0x1234002cU);
    cpu.esp = saved;
    ok = ok && test_split_count(vector, 6U) &&
         test_split_token(vector, 0U, "pre", 3U) &&
         test_split_token(vector, 1U, "x", 1U) &&
         test_split_token(vector, 2U, "", 0U) &&
         test_split_token(vector, 3U, "y", 1U) &&
         test_split_token(vector, 4U, "z", 1U) &&
         test_split_token(vector, 5U, "w", 1U) &&
         test_string_destroy(source);
    test_string_init(source);
    ok = ok && test_string_assign(source, raw, 0U) &&
         test_split_append(vector, source, '.') &&
         test_split_count(vector, 6U);
    cpu.esp = saved;
    ok = ok && test_vector_destroy(vector) && test_string_destroy(source);

    cpu.esp = saved;
    return ok;
}

static int test_path_from_base(uint32_t base, uint32_t relative,
                               uint32_t output, uint32_t ignored)
{
    uint32_t saved = cpu.esp;
    uint32_t ebp = cpu.ebp, ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi;
    int ok;
    gpush(&cpu, ignored);
    gpush(&cpu, output);
    gpush(&cpu, 0xABC599C0U);
    cpu.ecx = base;
    cpu.edx = relative;
    sub_002599c0(&cpu);
    ok = !cpu.fault && cpu.esp == saved - 8U &&
         ld32(cpu.esp) == output && ld32(cpu.esp + 4U) == ignored &&
         cpu.ebp == ebp && cpu.ebx == ebx &&
         cpu.esi == esi && cpu.edi == edi;
    cpu.esp = saved;
    return ok;
}

static int test_path_case(uint32_t base, uint32_t relative, uint32_t output,
                          const char *base_text, const char *relative_text,
                          const char *expected)
{
    uint32_t i, length = (uint32_t)strlen(expected);
    int ok;
    memcpy((void *)(uintptr_t)base, base_text, strlen(base_text) + 1U);
    memcpy((void *)(uintptr_t)relative, relative_text,
           strlen(relative_text) + 1U);
    memset((void *)(uintptr_t)output, 0xA5, 264U);
    ok = test_path_from_base(base, relative, output, 0x13579BDFU) &&
         memcmp((const void *)(uintptr_t)output, expected, length + 1U) == 0;
    for (i = length + 1U; ok && i < 260U; ++i)
        ok = ld8(output + i) == 0U;
    return ok && ld8(output + 260U) == 0xA5U;
}

static int check_portable_path_utilities(void)
{
    uint32_t saved = cpu.esp;
    uint32_t base = saved - 0x7000U;
    uint32_t relative = saved - 0x6000U;
    uint32_t output = saved - 0x4000U;
    uint32_t i;
    int ok = 1;

    ok = ok && guest_lookup(0x002599c0U) == sub_002599c0 &&
         guest_lookup(0x302599c0U) == sub_002599c0 &&
         guest_lookup(0x002599c0U) != guest_original_002599c0 &&
         guest_lookup(0x002c8700U) == sub_002c8700 &&
         guest_lookup(0x302c8700U) == sub_002c8700 &&
         guest_lookup(0x002c8700U) != guest_original_002c8700;

    ok = ok && test_path_case(base, relative, output,
         "resources/packed/file.anm2", "../gfx/foo.png",
         "resources/gfx/foo.png");
    ok = ok && test_path_case(base, relative, output,
         "root\\dir\\base.xml", ".\\sub//leaf.anm2",
         "root/dir/./sub//leaf.anm2");
    ok = ok && test_path_case(base, relative, output,
         "/a//base", "//leaf", "/a////leaf");
    ok = ok && test_path_case(base, relative, output,
         "basefile", "../../leaf", "leaf");
    ok = ok && test_path_case(base, relative, output,
         "basefile", "../../../leaf", "../leaf");

    memcpy((void *)(uintptr_t)base, "b", 2U);
    for (i = 0U; i < 270U; ++i)
        st8(relative + i, (uint8_t)'x');
    st8(relative + 270U, 0U);
    memset((void *)(uintptr_t)output, 0xA5, 264U);
    ok = ok && test_path_from_base(base, relative, output, 0xDEADBEEFU);
    for (i = 0U; ok && i < 260U; ++i)
        ok = ld8(output + i) == (uint8_t)'x';
    ok = ok && ld8(output + 260U) == 0xA5U;

    {
        uint32_t eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx;
        uint32_t ebx = cpu.ebx, ebp = cpu.ebp;
        uint32_t esi = cpu.esi, edi = cpu.edi;
        gpush(&cpu, 0xABC28700U);
        sub_002c8700(&cpu);
        ok = ok && !cpu.fault && cpu.esp == saved &&
             cpu.eax == eax && cpu.ecx == ecx && cpu.edx == edx &&
             cpu.ebx == ebx && cpu.ebp == ebp &&
             cpu.esi == esi && cpu.edi == edi;
    }
    cpu.esp = saved;
    return ok;
}

static uint32_t fnv(uint32_t h, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFFu; h *= 16777619u; }
    return h;
}

static int64_t call_signed_div(int64_t dividend, int64_t divisor, int *stack_ok)
{
    uint64_t a = (uint64_t)dividend, b = (uint64_t)divisor;
    uint32_t saved = cpu.esp;
    uint64_t result;
    gpush(&cpu, (uint32_t)(b >> 32));
    gpush(&cpu, (uint32_t)b);
    gpush(&cpu, (uint32_t)(a >> 32));
    gpush(&cpu, (uint32_t)a);
    gpush(&cpu, 0xABC0D1F0U);
    sub_005ebcb0(&cpu);
    *stack_ok = *stack_ok && cpu.esp == saved;
    cpu.esp = saved;
    result = ((uint64_t)cpu.edx << 32) | cpu.eax;
    return (int64_t)result;
}

int main(void)
{
    uint32_t *shifts;
    int idx, k, fail = 0, checks = 0, rng_mismatches = 0;
    uint32_t h_ref, h_got;

    printf("=== full-binary link and run ===\n");

    guest_register_all_imports();
    if (guest_image_load(EXE_PATH)) return 2;
    if (guest_stack_init(&cpu))     return 2;

    guest_register_all();
    printf("dispatch table              : %u entries\n", guest_table_len);
    printf("IAT metadata                : %u entries\n", guest_import_table_len);

    {
        uint32_t fs = guest_fs_base(&cpu);
        uint32_t tls_array = fs ? ld32(fs + 0x2CU) : 0U;
        uint32_t tls_index = ld32(VA_TLS_INDEX);
        uint32_t tls_block = tls_array && tls_index < 64U
            ? ld32(tls_array + tls_index * 4U) : 0U;
        int tib_ok = fs && ld32(fs) == 0xFFFFFFFFu &&
                     ld32(fs + 0x18U) == fs &&
                     /* An empty downward-growing stack starts exactly at its
                      * exclusive StackBase/checked ceiling. */
                     ld32(fs + 4U) >= cpu.esp && ld32(fs + 8U) < cpu.esp &&
                     tls_array != 0U && tls_index < 64U && tls_block != 0U &&
                     memcmp((void *)(uintptr_t)tls_block,
                            (void *)(uintptr_t)VA_TLS_TEMPLATE, 17U) == 0;
        printf("single-thread guest TIB/TLS : %s\n",
               tib_ok ? "self/stack/static template valid" : "FAILED");
        if (!tib_ok) fail++;
    }

    {
        uint32_t saved = cpu.esp;
        int first, second, tls_ok;
        tls_attach_result = -1;
        first = guest_run_until_stop(&cpu, call_tls_attach);
        tls_ok = first == GUEST_RUN_RETURNED &&
                 tls_attach_result == 0 && cpu.esp == saved;
        tls_attach_result = -1;
        second = guest_run_until_stop(&cpu, call_tls_attach);
        tls_ok = tls_ok && second == GUEST_RUN_RETURNED &&
                 tls_attach_result == 0 &&
                 cpu.esp == saved;
        printf("static TLS callbacks        : %s\n",
               tls_ok ? "attach + idempotence exact" : "FAILED");
        if (!tls_ok) fail++;
    }

    /* The image must be mapped before anything is claimed about behaviour. */
    shifts = (uint32_t *)(uintptr_t)VA_SHIFTS;
    if (shifts[0] != 1 || shifts[1] != 3 || shifts[2] != 10) {
        printf("IMAGE NOT MAPPED -- refusing to report anything\n");
        return 2;
    }

    /* ---- exact IAT identity and loud named failure -------------------- */
    {
        uint32_t slot_va = GUEST_IMAGE_BASE + RVA_IAT_LUA_ABSINDEX;
        uint32_t token = ld32(slot_va);
        const char *by_rva = guest_import_name(RVA_IAT_LUA_ABSINDEX);
        const char *by_va = guest_import_name(slot_va);
        unsigned faults_before = g_fault_count;
        int metadata_ok = guest_import_table_len == 413U &&
                          token == slot_va && by_rva && by_va &&
                          strcmp(by_rva, NAME_LUA_ABSINDEX) == 0 &&
                          strcmp(by_va, NAME_LUA_ABSINDEX) == 0 &&
                          guest_import_name(slot_va + 1U) == NULL;

        guarded_import_token = token;
        continued_after_guarded_fault = 0;
        if (guest_run_until_stop(&cpu, call_guarded_import) != GUEST_RUN_FAULT)
            fail++;
        printf("IAT exact-name fault        : [%08x] = %08x, %s, %s\n",
               slot_va, token, metadata_ok ? "metadata exact" : "metadata FAILED",
               cpu.fault && strcmp(cpu.fault, NAME_LUA_ABSINDEX) == 0 &&
               cpu.fault_addr == token && g_fault_count == faults_before + 1U &&
               !continued_after_guarded_fault
               ? "named fault exact" : "named fault FAILED");
        if (!metadata_ok || !cpu.fault ||
            strcmp(cpu.fault, NAME_LUA_ABSINDEX) != 0 ||
            cpu.fault_addr != token || g_fault_count != faults_before + 1U ||
            continued_after_guarded_fault)
            fail++;
        /* This is the expected negative test, not a fault subsequent oracle
         * checks should inherit.  The monotonic counter remains evidence that
         * the path really ran. */
        cpu.fault = NULL;
        cpu.fault_addr = 0;
    }

    if (!check_guest_qsort())
        fail++;

    if (!check_guest_rounding_imports())
        fail++;

    if (!check_guest_fdclass())
        fail++;

    if (!check_guest_sse2_precise_math())
        fail++;

    if (!check_guest_sse2_remaining_unary())
        fail++;

    if (!check_guest_ci_binary_math())
        fail++;

    if (!check_guest_nextafterf())
        fail++;

    if (!check_guest_memchr())
        fail++;

    if (!check_guest_rtdynamiccast())
        fail++;

    if (!check_guest_strchr())
        fail++;

    if (!check_guest_strstr())
        fail++;

    if (!check_guest_numeric_conversions())
        fail++;

    if (!check_guest_scheduler_imports())
        fail++;

    if (!check_guest_window_icon_imports())
        fail++;

    if (!check_guest_clipboard_imports())
        fail++;

    if (!check_guest_filesystem_crt())
        fail++;

    /* A process-global jmp_buf lets CPU2 overwrite CPU1's native destination;
     * in two threads that can jump into another stack. Nesting two distinct
     * CPUs in one thread is a deterministic regression for the same ownership
     * rule and also proves that both scopes are cleared after their faults. */
    {
        unsigned faults_before = g_fault_count;
        int stopped;
        guest_cpu_init(&nested_fault_cpu);
        nested_inner_seen = nested_outer_resumed = 0;
        stopped = guest_run_until_stop(&cpu, nested_outer_fault);
        printf("per-CPU fault scopes        : %s\n",
               stopped == GUEST_RUN_FAULT &&
               cpu.fault_addr == 0xF00D0001u &&
               nested_fault_cpu.fault_addr == 0xF00D0002u &&
               nested_inner_seen == 1 && nested_outer_resumed == 1 &&
               !cpu.run_scope && !nested_fault_cpu.run_scope &&
               g_fault_count == faults_before + 2U
               ? "nested isolation exact" : "FAILED");
        if (stopped != GUEST_RUN_FAULT ||
            cpu.fault_addr != 0xF00D0001u ||
            nested_fault_cpu.fault_addr != 0xF00D0002u ||
            nested_inner_seen != 1 || nested_outer_resumed != 1 ||
            cpu.run_scope || nested_fault_cpu.run_scope ||
            g_fault_count != faults_before + 2U)
            fail++;
        cpu.fault = NULL;
        cpu.fault_addr = 0;
        nested_fault_cpu.fault = NULL;
        nested_fault_cpu.fault_addr = 0;
    }
    {
        unsigned calls_before = g_host_import_calls;
        int jump_ok = check_guest_setjmp_longjmp();
        printf("guest setjmp/longjmp        : %s, %u import calls\n",
               jump_ok
               ? "buffer/cdecl/nonlocal restore/stale/CPU isolation exact"
               : "FAILED",
               g_host_import_calls - calls_before);
        if (!jump_ok || g_host_import_calls - calls_before != 4U)
            fail++;
    }
    {
        unsigned faults_before = g_fault_count;
        int stopped;
        continued_after_checkpoint = 0;
        g_guest_checkpoint_rva = 0x00C0FFEEU;
        stopped = guest_run_until_stop(&cpu, call_bringup_checkpoint);
        printf("bring-up checkpoint         : %s\n",
               stopped == GUEST_RUN_FAULT &&
               cpu.fault_addr == 0x00C0FFEEU &&
               cpu.fault && strcmp(cpu.fault, "bring-up checkpoint") == 0 &&
               !continued_after_checkpoint &&
               g_fault_count == faults_before + 1U
               ? "controlled stop exact" : "FAILED");
        if (stopped != GUEST_RUN_FAULT ||
            cpu.fault_addr != 0x00C0FFEEU || !cpu.fault ||
            strcmp(cpu.fault, "bring-up checkpoint") != 0 ||
            continued_after_checkpoint || g_fault_count != faults_before + 1U)
            fail++;
        g_guest_checkpoint_rva = 0U;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;
    }
    {
        static const struct {
            uint32_t rva;
            guest_fn public_fn;
            guest_fn original_fn;
            const char *fault;
            uint32_t return_word;
        } cases[] = {
            { GUEST_KAGE_INITIALIZE_RVA, sub_00560c60,
              guest_original_00560c60, GUEST_KAGE_INITIALIZE_FAULT,
              0xABC0A6E0U },
            { GUEST_KAGE_RENDER_DISPLAY_RVA, sub_00561830,
              guest_original_00561830, GUEST_KAGE_RENDER_DISPLAY_FAULT,
              0xABC0A6E1U },
        };
        uint32_t saved = cpu.esp;
        unsigned faults_before = g_fault_count;
        int boundary_ok = 1;
        size_t i;

        g_guest_gl_inventory_mode = 0;
        for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            guest_fn by_rva = guest_lookup(cases[i].rva);
            guest_fn by_va = guest_lookup(GUEST_IMAGE_BASE + cases[i].rva);
            int lookup_ok = by_rva == cases[i].public_fn &&
                            by_va == by_rva &&
                            by_rva != cases[i].original_fn;
            int stopped;
            cpu.esp = saved;
            cpu.fault = NULL;
            cpu.fault_addr = 0U;
            guarded_kage_boundary = cases[i].public_fn;
            guarded_kage_return = cases[i].return_word;
            continued_after_kage_boundary = 0;
            stopped = guest_run_until_stop(&cpu, call_guarded_kage_boundary);
            {
                int case_ok = lookup_ok &&
                stopped == GUEST_RUN_FAULT &&
                cpu.fault_addr == cases[i].rva && cpu.fault &&
                strcmp(cpu.fault, cases[i].fault) == 0 &&
                !continued_after_kage_boundary &&
                cpu.esp == saved - 4U &&
                ld32(cpu.esp) == cases[i].return_word;
                boundary_ok = boundary_ok && case_ok;
            }
        }
        printf("manual KAGE boundary        : %s\n",
               boundary_ok && g_fault_count == faults_before + 2U
               ? "2/2 public dispatch + exact high-level faults"
               : "FAILED");
        if (!boundary_ok || g_fault_count != faults_before + 2U)
            fail++;
        cpu.esp = saved;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;
    }
    {
        int split_ok = check_portable_split();
        printf("portable delimiter split    : %s\n",
               split_ok
               ? "2 public seams; SSO/heap/NUL/empty/append ABI exact"
               : "FAILED");
        if (!split_ok)
            fail++;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;
    }
    {
        int path_ok = check_portable_path_utilities();
        printf("portable path/diagnostics  : %s\n",
               path_ok
               ? "2 public seams; normalize/../260-byte ABI exact"
               : "FAILED");
        if (!path_ok)
            fail++;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;
    }
    printf("image guard                 : shift table 1,3,10 OK\n");

    /* Startup must select code that the translated CPU actually implements,
     * independent of whether this particular PC happens to have AVX-512. */
    {
        CPU probe;
        int cpu_ok;
        guest_cpu_init(&probe);
        probe.eax = 0U;
        guest_cpuid(&probe);
        cpu_ok = probe.eax == 1U && probe.ebx == 0x756e6547U &&
                 probe.edx == 0x49656e69U && probe.ecx == 0x6c65746eU;
        probe.eax = 1U;
        probe.ecx = 0U;
        guest_cpuid(&probe);
        cpu_ok = cpu_ok && (probe.edx & (1U << 26)) != 0U &&
                 (probe.ecx & ((1U << 20) | (1U << 27) | (1U << 28))) == 0U;
        printf("virtual guest CPU          : %s\n",
               cpu_ok ? "stable SSE2 profile" : "FAILED");
        if (!cpu_ok) fail++;
    }
    {
        uint32_t saved = cpu.esp;
        int cookie_ok;
        cpu.ecx = ld32(VA_SECURITY_COOKIE);
        gpush(&cpu, 0xABC0C00CU);
        sub_005eacd7(&cpu);
        cookie_ok = !cpu.fault && cpu.esp == saved;
        cpu.esp = saved;
        printf("security-cookie normal path : %s\n",
               cookie_ok ? "match returns; fail interrupt stays local"
                         : "FAILED");
        if (!cookie_ok) fail++;
    }

    /* These two MSVC conversion helpers contain an EVEX fast path followed
     * by an SSE2 fallback.  A whole-function translator must emit both even
     * though our virtual CPU normally selects the fallback.  Force level 6
     * here so vmovd/vpinsrd/vcvt[q,uq]q2pd are compared with C's conversions. */
    {
        static const int64_t signed_values[] = {
            0, 1, -1, INT64_MIN, INT64_MAX, INT64_C(0x123456789abcdef)
        };
        static const uint64_t unsigned_values[] = {
            0, 1, UINT64_MAX, UINT64_C(0x8000000000000000),
            UINT64_C(0x123456789abcdef0)
        };
        uint32_t *isa_level = (uint32_t *)(uintptr_t)
            (GUEST_IMAGE_BASE + 0x7fd61cu);
        uint32_t saved_level = *isa_level;
        uint32_t saved_esp = cpu.esp;
        int vector_ok = 1;
        size_t i;
        *isa_level = 6U;
        for (i = 0; i < sizeof signed_values / sizeof signed_values[0]; ++i) {
            uint64_t bits = (uint64_t)signed_values[i];
            cpu.ecx = (uint32_t)bits;
            cpu.edx = (uint32_t)(bits >> 32);
            gpush(&cpu, 0xABC0D001U);
            sub_005eb9d0(&cpu);
            vector_ok = vector_ok && cpu.esp == saved_esp &&
                        cpu.x[0].d[0] == (double)signed_values[i];
            cpu.esp = saved_esp;
        }
        for (i = 0; i < sizeof unsigned_values / sizeof unsigned_values[0]; ++i) {
            uint64_t bits = unsigned_values[i];
            cpu.ecx = (uint32_t)bits;
            cpu.edx = (uint32_t)(bits >> 32);
            gpush(&cpu, 0xABC0D002U);
            sub_005eb970(&cpu);
            vector_ok = vector_ok && cpu.esp == saved_esp &&
                        cpu.x[0].d[0] == (double)unsigned_values[i];
            cpu.esp = saved_esp;
        }
        *isa_level = saved_level;
        printf("EVEX int64 conversions      : %s\n",
               vector_ok ? "11/11 exact" : "FAILED");
        if (!vector_ok) fail++;
    }

    {
        static const int64_t cases[][2] = {
            { INT64_MAX, INT64_C(0x100000001) },
            { -INT64_C(0x7000000000000000), INT64_C(0x100000001) },
            { INT64_C(0x7000000000000000), -INT64_C(0x100000001) },
            { -INT64_C(0x7000000000000000), -INT64_C(0x100000001) },
            { INT64_C(1234567890123456), 1000 },
            { -INT64_C(1234567890123456), 1000 },
        };
        int divide_ok = 1;
        size_t i;
        for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            int64_t got = call_signed_div(cases[i][0], cases[i][1], &divide_ok);
            divide_ok = divide_ok && got == cases[i][0] / cases[i][1];
        }
        printf("signed 64-bit division      : %s\n",
               divide_ok ? "6/6 exact incl. RCR path" : "FAILED");
        if (!divide_ok) fail++;
    }
    {
        static const float fv[] = {
            0.0f, 1.75f, 123456.75f, 2147483648.0f, 4294967040.0f
        };
        static const double dv[] = {
            0.0, 1.9, 1234567890.75, 2147483648.0, 4294967295.0
        };
        static const uint32_t invalid_fv[] = {
            0xbf800000U, 0x4f800000U, 0x7fc00000U, 0x7f800000U
        };
        static const uint64_t invalid_dv[] = {
            UINT64_C(0xbff0000000000000), UINT64_C(0x41f0000000000000),
            UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff0000000000000)
        };
        uint32_t *isa_level = (uint32_t *)(uintptr_t)
            (GUEST_IMAGE_BASE + 0x7fd61cu);
        uint32_t saved_level = *isa_level, saved_esp = cpu.esp;
        int cvt_ok = 1;
        size_t i;
        *isa_level = 6U;
        for (i = 0; i < sizeof fv / sizeof fv[0]; ++i) {
            cpu.x[0] = (xmm_t){0};
            cpu.x[0].f[0] = fv[i];
            gpush(&cpu, 0xABC0C001U);
            sub_005eb8a0(&cpu);
            cvt_ok = cvt_ok && cpu.esp == saved_esp &&
                     cpu.eax == (uint32_t)fv[i];
            cpu.esp = saved_esp;
        }
        for (i = 0; i < sizeof dv / sizeof dv[0]; ++i) {
            cpu.x[0] = (xmm_t){0};
            cpu.x[0].d[0] = dv[i];
            gpush(&cpu, 0xABC0C002U);
            sub_005eb8f0(&cpu);
            cvt_ok = cvt_ok && cpu.esp == saved_esp &&
                     cpu.eax == (uint32_t)dv[i];
            cpu.esp = saved_esp;
        }
        for (i = 0; i < sizeof invalid_fv / sizeof invalid_fv[0]; ++i) {
            cpu.x[0] = (xmm_t){0};
            cpu.x[0].u32[0] = invalid_fv[i];
            gpush(&cpu, 0xABC0C003U);
            sub_005eb8a0(&cpu);
            cvt_ok = cvt_ok && cpu.esp == saved_esp &&
                     cpu.eax == UINT32_MAX;
            cpu.esp = saved_esp;
        }
        for (i = 0; i < sizeof invalid_dv / sizeof invalid_dv[0]; ++i) {
            cpu.x[0] = (xmm_t){0};
            cpu.x[0].u64[0] = invalid_dv[i];
            gpush(&cpu, 0xABC0C004U);
            sub_005eb8f0(&cpu);
            cvt_ok = cvt_ok && cpu.esp == saved_esp &&
                     cpu.eax == UINT32_MAX;
            cpu.esp = saved_esp;
        }
        *isa_level = saved_level;
        printf("EVEX float to uint32        : %s\n",
               cvt_ok ? "18/18 exact incl. invalid" : "FAILED");
        if (!cvt_ok) fail++;
    }
    {
        static const uint32_t fallback_fv[][2] = {
            { 0xbf000000U, 0U }, { 0xbf800000U, UINT32_MAX },
            { 0x7fc00000U, UINT32_MAX }, { 0x3fc00000U, 1U }
        };
        static const uint64_t fallback_dv[][2] = {
            { UINT64_C(0xbff0000000000000), UINT32_MAX },
            { UINT64_C(0x7ff8000000000000), UINT32_MAX }
        };
        uint32_t *isa_level = (uint32_t *)(uintptr_t)
            (GUEST_IMAGE_BASE + 0x7fd61cu);
        uint32_t saved_level = *isa_level, saved_esp = cpu.esp;
        int fallback_ok = 1;
        size_t i;
        *isa_level = 0U;
        for (i = 0; i < sizeof fallback_fv / sizeof fallback_fv[0]; ++i) {
            cpu.x[0] = (xmm_t){0};
            cpu.x[0].u32[0] = fallback_fv[i][0];
            gpush(&cpu, 0xABC0C101U);
            sub_005eb8a0(&cpu);
            fallback_ok = fallback_ok && cpu.esp == saved_esp &&
                          cpu.eax == fallback_fv[i][1];
            cpu.esp = saved_esp;
        }
        for (i = 0; i < sizeof fallback_dv / sizeof fallback_dv[0]; ++i) {
            cpu.x[0] = (xmm_t){0};
            cpu.x[0].u64[0] = fallback_dv[i][0];
            gpush(&cpu, 0xABC0C102U);
            sub_005eb8f0(&cpu);
            fallback_ok = fallback_ok && cpu.esp == saved_esp &&
                          cpu.eax == (uint32_t)fallback_dv[i][1];
            cpu.esp = saved_esp;
        }
        *isa_level = saved_level;
        printf("legacy float to uint32      : %s\n",
               fallback_ok ? "6/6 exact incl. CMC/SBB" : "FAILED");
        if (!fallback_ok) fail++;
    }

    /* ---- reached startup host shims, including exact stdcall cleanup --- */
    {
        unsigned calls_before = g_host_import_calls;
        int host_ok = 0;
        host_ok += call_host_out64(RVA_IAT_SYSTEM_TIME, 0);
        host_ok += call_host_noarg(RVA_IAT_THREAD_ID);
        host_ok += call_host_noarg(RVA_IAT_PROCESS_ID);
        host_ok += call_host_out64(RVA_IAT_QPC, 1);
        host_ok += call_host_out64(RVA_IAT_QPF, 1);
        host_ok += call_host_onearg(RVA_IAT_CPU_FEATURE, 10U);
        printf("startup host shims         : %d/6, %u calls, stack cleanup exact\n",
               host_ok, g_host_import_calls - calls_before);
        if (host_ok != 6 || g_host_import_calls - calls_before != 6U)
            fail++;
    }

    {
        /* Exercise the exact mimalloc Win32 contract, including its unusual
         * aligned-interior release fallback.  Numeric constants are copied
         * from memoryapi.h so this driver does not depend on a host SDK layout
         * beyond the deliberately explicit 28-byte structure above. */
        unsigned calls_before = g_host_import_calls;
        uint32_t args[4], p, result, query_size, unlock_result;
        uint32_t large_page, last_error, release_base;
        guest_mbi32 info;
        int vm_ok = 1, vm_stage = 0;
        unsigned i;

        large_page = call_host_stdcall_args(
            RVA_IAT_LARGE_PAGE_MIN, NULL, 0U, &vm_ok);
        vm_ok = vm_ok &&
                (large_page == 0U ||
                 (large_page >= 0x10000U &&
                  (large_page & 0xffffU) == 0U));
        if (vm_ok) vm_stage = 1;

        args[0] = 0U;
        args[1] = 0x20000U;
        args[2] = 0x3000U;       /* MEM_RESERVE | MEM_COMMIT */
        args[3] = 4U;            /* PAGE_READWRITE */
        p = call_host_stdcall_args(
            RVA_IAT_VIRTUAL_ALLOC, args, 4U, &vm_ok);
        vm_ok = vm_ok && p != 0U && (p & 0xffffU) == 0U;
        if (vm_ok) vm_stage = 2;

        if (p) {
            for (i = 0U; i < 32U; ++i)
                if (ld8(p + i) != 0U) vm_ok = 0;
            st32(p, 0x51aac33cU);

            args[0] = p;
            args[1] = 0x10000U;
            args[2] = 0x80000U;  /* MEM_RESET */
            args[3] = 4U;
            result = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_ALLOC, args, 4U, &vm_ok);
            vm_ok = vm_ok && result == p;
            if (vm_ok) vm_stage = 3;
            st32(p, 0x51aac33cU); /* contents after RESET are unspecified */

            memset(&info, 0xcc, sizeof info);
            args[0] = p + 0x1000U;
            args[1] = (uint32_t)(uintptr_t)&info;
            args[2] = (uint32_t)sizeof info;
            query_size = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_QUERY, args, 3U, &vm_ok);
            vm_ok = vm_ok && query_size == sizeof info &&
                    info.base_address <= p + 0x1000U &&
                    p + 0x1000U - info.base_address < info.region_size &&
                    info.allocation_base == p &&
                    info.allocation_protect == 4U &&
                    info.state == 0x1000U && /* MEM_COMMIT */
                    info.protect == 4U &&     /* PAGE_READWRITE */
                    info.type == 0x20000U;    /* MEM_PRIVATE */
            if (vm_ok) vm_stage = 4;

            args[0] = p;
            args[1] = 0x1000U;
            unlock_result = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_UNLOCK, args, 2U, &vm_ok);
            vm_ok = vm_ok && unlock_result <= 1U;
            if (vm_ok) vm_stage = 5;

            args[0] = p + 0x10000U;
            args[1] = 0x10000U;
            args[2] = 0x4000U;   /* MEM_DECOMMIT */
            result = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_FREE, args, 3U, &vm_ok);
            vm_ok = vm_ok && result == 1U;
            if (vm_ok) vm_stage = 6;

            memset(&info, 0xcc, sizeof info);
            args[0] = p + 0x10000U;
            args[1] = (uint32_t)(uintptr_t)&info;
            args[2] = (uint32_t)sizeof info;
            query_size = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_QUERY, args, 3U, &vm_ok);
            vm_ok = vm_ok && query_size == sizeof info &&
                    info.base_address == p + 0x10000U &&
                    info.allocation_base == p &&
                    info.state == 0x2000U && /* MEM_RESERVE */
                    info.protect == 0U;
            if (vm_ok) vm_stage = 7;

            args[0] = p + 0x10000U;
            args[1] = 0x10000U;
            args[2] = 0x1000U;   /* MEM_COMMIT */
            args[3] = 4U;
            result = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_ALLOC, args, 4U, &vm_ok);
            vm_ok = vm_ok && result == p + 0x10000U &&
                    ld32(p + 0x10000U) == 0U;
            if (vm_ok) vm_stage = 8;

            /* This failure and GetLastError are intentionally adjacent:
             * mimalloc branches on ERROR_INVALID_ADDRESS before querying the
             * allocation base of its over-aligned interior pointer. */
            args[0] = p + 0x10000U;
            args[1] = 0U;
            args[2] = 0x8000U;   /* MEM_RELEASE */
            result = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_FREE, args, 3U, &vm_ok);
            last_error = call_host_stdcall_args(
                RVA_IAT_GET_LAST_ERROR, NULL, 0U, &vm_ok);
            vm_ok = vm_ok && result == 0U && last_error == 487U;
            if (vm_ok) vm_stage = 9;

            memset(&info, 0xcc, sizeof info);
            args[0] = p + 0x10000U;
            args[1] = (uint32_t)(uintptr_t)&info;
            args[2] = (uint32_t)sizeof info;
            query_size = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_QUERY, args, 3U, &vm_ok);
            vm_ok = vm_ok && query_size == sizeof info &&
                    info.allocation_base == p &&
                    info.state == 0x1000U;
            if (vm_ok) vm_stage = 10;

            release_base = query_size == sizeof info &&
                           info.allocation_base == p
                         ? info.allocation_base : p;
            args[0] = release_base;
            args[1] = 0U;
            args[2] = 0x8000U;   /* MEM_RELEASE */
            result = call_host_stdcall_args(
                RVA_IAT_VIRTUAL_FREE, args, 3U, &vm_ok);
            vm_ok = vm_ok && result == 1U;
            if (vm_ok) vm_stage = 11;
        }

        printf("native virtual-memory family: %s, stage %d/11,"
               " %u import calls\n",
               vm_ok ? "reserve/reset/decommit/query/fallback exact"
                     : "FAILED",
               vm_stage, g_host_import_calls - calls_before);
        if (!vm_ok || g_host_import_calls - calls_before != 12U)
            fail++;
    }

    {
        unsigned calls_before = g_host_import_calls;
        int init_ok = check_guest_initializers();
        printf("guest-aware CRT init arrays : %s, %u import calls\n",
               init_ok ? "stop/continue semantics exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!init_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }

    {
        unsigned calls_before = g_host_import_calls;
        int exit_ok = check_guest_exit();
        printf("guest CRT termination       : %s, %u import calls\n",
               exit_ok ? "_exit skips; exit runs TLS+B+A; controlled noreturn"
                       : "FAILED",
               g_host_import_calls - calls_before);
        if (!exit_ok || g_host_import_calls - calls_before != 6U)
            fail++;
    }

    {
        unsigned calls_before = g_host_import_calls;
        int mode_ok = call_host_cdecl_onearg(RVA_IAT_SET_NEW_MODE, 1U, 0U) &&
                      call_host_cdecl_onearg(RVA_IAT_SET_NEW_MODE, 0U, 1U);
        printf("CRT new-handler mode        : %s, %u import calls\n",
               mode_ok ? "state/return/cdecl exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!mode_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t argument, cell, same_cell, result;
        int errno_ok = 1;

        argument = 2U;                 /* ENOENT at current FindFirstFileW */
        result = call_host_cdecl_args(RVA_IAT_SET_ERRNO,
                                      &argument, 1U, &errno_ok);
        cell = call_host_cdecl_args(RVA_IAT_ERRNO, NULL, 0U, &errno_ok);
        errno_ok = errno_ok && result == 0U && cell != 0U &&
                   ld32(cell) == 2U;
        if (cell) st32(cell, 9U);       /* EBADF at the fifth reached site */
        same_cell = call_host_cdecl_args(RVA_IAT_ERRNO,
                                         NULL, 0U, &errno_ok);
        argument = 22U;
        result = call_host_cdecl_args(RVA_IAT_SET_ERRNO,
                                      &argument, 1U, &errno_ok);
        errno_ok = errno_ok && result == 0U && same_cell == cell &&
                   cell != 0U && ld32(cell) == 22U;
        printf("guest errno state           : %s, %u import calls\n",
               errno_ok ? "stable writable pointer/setter/cdecl exact"
                        : "FAILED",
               g_host_import_calls - calls_before);
        if (!errno_ok || g_host_import_calls - calls_before != 4U)
            fail++;
    }
    {
        static const uint32_t expected_tm[9] = {
            56U, 34U, 12U, 29U, 1U, 100U, 2U, 59U, 0U
        };
        static const char format[] = "%Y-%m-%d %H:%M:%S %a %j";
        static const char formatted_expected[] =
            "2000-02-29 12:34:56 Tue 060";
        unsigned calls_before = g_host_import_calls;
        uint32_t known_time[2] = { 951827696U, 0U };
        uint32_t written_time[4] = {
            0x13579BDFU, 0xCCCCCCCCU, 0xCCCCCCCCU, 0x2468ACE0U
        };
        uint32_t args[4], low, tm_pointer, local_pointer, length;
        uint64_t now_without_output, now_with_output, written;
        uint64_t roundtrip;
        char formatted[64];
        const char *old_tz = getenv("TZ");
        char *saved_tz = NULL;
        int time_ok = 1;
        unsigned i;

        if (old_tz) {
            size_t old_tz_length = strlen(old_tz) + 1U;
            saved_tz = (char *)malloc(old_tz_length);
            if (saved_tz) memcpy(saved_tz, old_tz, old_tz_length);
            else time_ok = 0;
        }
        if (time_ok && _putenv_s("TZ", "UTC") == 0) _tzset();
        else time_ok = 0;

        args[0] = 0U;
        low = call_host_cdecl_args(RVA_IAT_TIME64, args, 1U, &time_ok);
        now_without_output = (uint64_t)low | ((uint64_t)cpu.edx << 32U);
        args[0] = (uint32_t)(uintptr_t)&written_time[1];
        low = call_host_cdecl_args(RVA_IAT_TIME64, args, 1U, &time_ok);
        now_with_output = (uint64_t)low | ((uint64_t)cpu.edx << 32U);
        written = (uint64_t)written_time[1] |
                  ((uint64_t)written_time[2] << 32U);
        time_ok = time_ok && now_without_output <= now_with_output &&
                  now_with_output - now_without_output <= 5U &&
                  written == now_with_output &&
                  written_time[0] == 0x13579BDFU &&
                  written_time[3] == 0x2468ACE0U;

        args[0] = (uint32_t)(uintptr_t)known_time;
        tm_pointer = call_host_cdecl_args(RVA_IAT_GMTIME64,
                                          args, 1U, &time_ok);
        time_ok = time_ok && tm_pointer != 0U;
        if (tm_pointer)
            for (i = 0U; i < 9U; ++i)
                if (ld32(tm_pointer + i * 4U) != expected_tm[i])
                    time_ok = 0;

        args[0] = tm_pointer;
        low = call_host_cdecl_args(RVA_IAT_MKGMTIME64,
                                   args, 1U, &time_ok);
        roundtrip = (uint64_t)low | ((uint64_t)cpu.edx << 32U);
        time_ok = time_ok && roundtrip == 951827696U;

        args[0] = (uint32_t)(uintptr_t)known_time;
        local_pointer = call_host_cdecl_args(RVA_IAT_LOCALTIME64,
                                             args, 1U, &time_ok);
        time_ok = time_ok && local_pointer == tm_pointer &&
                  local_pointer != 0U;
        if (local_pointer)
            for (i = 0U; i < 9U; ++i)
                if (ld32(local_pointer + i * 4U) != expected_tm[i])
                    time_ok = 0;

        args[0] = local_pointer;
        low = call_host_cdecl_args(RVA_IAT_MKGMTIME64,
                                   args, 1U, &time_ok);
        roundtrip = (uint64_t)low | ((uint64_t)cpu.edx << 32U);
        time_ok = time_ok && roundtrip == 951827696U;

        memset(formatted, 0xCC, sizeof formatted);
        args[0] = (uint32_t)(uintptr_t)formatted;
        args[1] = (uint32_t)sizeof formatted;
        args[2] = (uint32_t)(uintptr_t)format;
        args[3] = local_pointer;
        length = call_host_cdecl_args(RVA_IAT_STRFTIME,
                                      args, 4U, &time_ok);
        time_ok = time_ok && length == sizeof formatted_expected - 1U &&
                  strcmp(formatted, formatted_expected) == 0;

        if (saved_tz) {
            if (_putenv_s("TZ", saved_tz) != 0) time_ok = 0;
            free(saved_tz);
        } else if (_putenv_s("TZ", "") != 0) {
            time_ok = 0;
        }
        _tzset();

        printf("guest UCRT time family      : %s, %u import calls\n",
               time_ok
                   ? "EDX:EAX/write/tm9/UTC roundtrip/C locale exact"
                   : "FAILED",
               g_host_import_calls - calls_before);
        if (!time_ok || g_host_import_calls - calls_before != 7U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char duplicate_source[] = "guest-owned copy";
        uint32_t args[2], p, q, z, d;
        int heap_ok = 1;
        unsigned i;
        args[0] = 32U;
        p = call_host_cdecl_args(RVA_IAT_MALLOC, args, 1U, &heap_ok);
        if (!p) heap_ok = 0;
        if (p)
            for (i = 0; i < 32U; ++i) st8(p + i, (uint8_t)(i ^ 0xA5U));
        args[0] = p;
        args[1] = 64U;
        q = call_host_cdecl_args(RVA_IAT_REALLOC, args, 2U, &heap_ok);
        if (!q) {
            heap_ok = 0;
            q = p;                  /* realloc failure leaves p allocated */
        } else {
            for (i = 0; i < 32U; ++i)
                if (ld8(q + i) != (uint8_t)(i ^ 0xA5U)) heap_ok = 0;
        }
        args[0] = 8U;
        args[1] = 4U;
        z = call_host_cdecl_args(RVA_IAT_CALLOC, args, 2U, &heap_ok);
        if (!z) heap_ok = 0;
        if (z)
            for (i = 0; i < 32U; ++i)
                if (ld8(z + i) != 0U) heap_ok = 0;
        args[0] = (uint32_t)(uintptr_t)duplicate_source;
        d = call_host_cdecl_args(RVA_IAT_STRDUP, args, 1U, &heap_ok);
        heap_ok = heap_ok && d != 0U &&
                  d != (uint32_t)(uintptr_t)duplicate_source &&
                  strcmp((const char *)(uintptr_t)d, duplicate_source) == 0;
        args[0] = q;
        (void)call_host_cdecl_args(RVA_IAT_FREE, args, 1U, &heap_ok);
        args[0] = z;
        (void)call_host_cdecl_args(RVA_IAT_FREE, args, 1U, &heap_ok);
        args[0] = d;
        (void)call_host_cdecl_args(RVA_IAT_FREE, args, 1U, &heap_ok);
        printf("single guest CRT heap       : %s, %u import calls\n",
               heap_ok ? "alloc/realloc/zero/duplicate/free exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!heap_ok || g_host_import_calls - calls_before != 7U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t args[3] = { 0x30001000U, 0x44DU, 0U };
        uint32_t init_result, context_result, before[23];
        const char *context_name =
            guest_import_name(RVA_IAT_STEAM_CONTEXT_INIT);
        const char *register_result_name =
            guest_import_name(RVA_IAT_STEAM_REGISTER_CALL_RESULT);
        const char *unregister_result_name =
            guest_import_name(RVA_IAT_STEAM_UNREGISTER_CALL_RESULT);
        const char *register_result_va_name = guest_import_name(
            GUEST_IMAGE_BASE + RVA_IAT_STEAM_REGISTER_CALL_RESULT);
        const char *unregister_result_va_name = guest_import_name(
            GUEST_IMAGE_BASE + RVA_IAT_STEAM_UNREGISTER_CALL_RESULT);
        unsigned register_result_count, unregister_result_count, i;
        uint64_t register_result_hash = absolute_iat_call_census(
            RVA_IAT_STEAM_REGISTER_CALL_RESULT, &register_result_count);
        uint64_t unregister_result_hash = absolute_iat_call_census(
            RVA_IAT_STEAM_UNREGISTER_CALL_RESULT, &unregister_result_count);
        int steam_ok = 1;
        for (i = 0; i < 23U; ++i)
            before[i] = ld32(VA_STEAM_CONTEXT_DESCRIPTOR + i * 4U);
        steam_ok = steam_ok &&
                   context_name &&
                   strcmp(context_name, NAME_STEAM_CONTEXT_INIT) == 0 &&
                   register_result_name && unregister_result_name &&
                   register_result_va_name && unregister_result_va_name &&
                   strcmp(register_result_name,
                          NAME_STEAM_REGISTER_CALL_RESULT) == 0 &&
                   strcmp(unregister_result_name,
                          NAME_STEAM_UNREGISTER_CALL_RESULT) == 0 &&
                   strcmp(register_result_va_name,
                          NAME_STEAM_REGISTER_CALL_RESULT) == 0 &&
                   strcmp(unregister_result_va_name,
                          NAME_STEAM_UNREGISTER_CALL_RESULT) == 0 &&
                   register_result_count == STEAM_CALL_RESULT_CALL_COUNT &&
                   unregister_result_count == STEAM_CALL_RESULT_CALL_COUNT &&
                   register_result_hash ==
                       STEAM_REGISTER_CALL_RESULT_FNV64 &&
                   unregister_result_hash ==
                       STEAM_UNREGISTER_CALL_RESULT_FNV64 &&
                   before[0] == VA_STEAM_CONTEXT_CALLBACK &&
                   before[1] == 0U &&
                   ld8(GUEST_IMAGE_BASE + RVA_STEAM_CONTEXT_CALL) == 0xFFU &&
                   ld8(GUEST_IMAGE_BASE + RVA_STEAM_CONTEXT_CALL + 1U) == 0x15U &&
                   ld32(GUEST_IMAGE_BASE + RVA_STEAM_CONTEXT_CALL + 2U) ==
                       GUEST_IMAGE_BASE + RVA_IAT_STEAM_CONTEXT_INIT;
        for (i = 0; i < STEAM_CONTEXT_DWORDS; ++i)
            steam_ok = steam_ok && before[i + 2U] == 0U;
        (void)call_host_cdecl_args(RVA_IAT_STEAM_REGISTER,
                                   args, 2U, &steam_ok);
        (void)call_host_cdecl_args(RVA_IAT_STEAM_UNREGISTER,
                                   args, 1U, &steam_ok);
        args[1] = 0x76543210U;
        args[2] = 0xfedcba98U;
        (void)call_host_cdecl_args(RVA_IAT_STEAM_REGISTER_CALL_RESULT,
                                   args, 3U, &steam_ok);
        (void)call_host_cdecl_args(RVA_IAT_STEAM_UNREGISTER_CALL_RESULT,
                                   args, 3U, &steam_ok);
        init_result = call_host_cdecl_args(RVA_IAT_STEAM_INIT,
                                           NULL, 0U, &steam_ok);
        (void)call_host_cdecl_args(RVA_IAT_STEAM_RUN,
                                   NULL, 0U, &steam_ok);
        (void)call_host_cdecl_args(RVA_IAT_STEAM_SHUTDOWN,
                                   NULL, 0U, &steam_ok);
        args[0] = VA_STEAM_CONTEXT_DESCRIPTOR;
        context_result = call_host_cdecl_args(RVA_IAT_STEAM_CONTEXT_INIT,
                                              args, 1U, &steam_ok);
        steam_ok = steam_ok && init_result == 0U &&
                   context_result == VA_STEAM_CONTEXT_DESCRIPTOR + 8U;
        for (i = 0; i < 23U; ++i)
            steam_ok = steam_ok &&
                       ld32(VA_STEAM_CONTEXT_DESCRIPTOR + i * 4U) == before[i];
        printf("Steam-disabled lifecycle    : %s, %u imports; %u/%u call-result sites\n",
               steam_ok ? "cdecl + Init=false + zero context exact" : "FAILED",
               g_host_import_calls - calls_before,
               register_result_count, unregister_result_count);
        if (!steam_ok || g_host_import_calls - calls_before != 8U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        unsigned char buffer[64];
        uint32_t args[3], base = (uint32_t)(uintptr_t)buffer, result;
        int memory_ok = 1;
        unsigned i;
        for (i = 0; i < sizeof buffer; ++i) buffer[i] = (uint8_t)i;
        args[0] = base + 32U;
        args[1] = base;
        args[2] = 16U;
        result = call_host_cdecl_args(RVA_IAT_MEMCPY, args, 3U, &memory_ok);
        memory_ok = memory_ok && result == base + 32U;
        for (i = 0; i < 16U; ++i)
            if (buffer[32U + i] != (uint8_t)i) memory_ok = 0;
        for (i = 0; i < sizeof buffer; ++i) buffer[i] = (uint8_t)i;
        args[0] = base + 4U;
        args[1] = base;
        args[2] = 24U;
        result = call_host_cdecl_args(RVA_IAT_MEMMOVE, args, 3U, &memory_ok);
        memory_ok = memory_ok && result == base + 4U;
        for (i = 0; i < 24U; ++i)
            if (buffer[4U + i] != (uint8_t)i) memory_ok = 0;
        printf("VCRUNTIME memory moves      : %s, %u import calls\n",
               memory_ok ? "copy + overlapping move exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!memory_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char source[] = "assets";
        uint16_t output[16];
        uint32_t converted = 0U, args[5], result;
        int convert_ok = 1;
        memset(output, 0xCC, sizeof output);
        args[0] = (uint32_t)(uintptr_t)&converted;
        args[1] = (uint32_t)(uintptr_t)output;
        args[2] = 16U;
        args[3] = (uint32_t)(uintptr_t)source;
        args[4] = 16U;
        result = call_host_cdecl_args(RVA_IAT_MBSTOWCS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == 0U && converted == 7U &&
                     output[0] == 'a' && output[5] == 's' && output[6] == 0U;
        memset(output, 0xCC, sizeof output);
        converted = 0U;
        args[2] = 4U;
        args[4] = UINT32_MAX;       /* _TRUNCATE */
        result = call_host_cdecl_args(RVA_IAT_MBSTOWCS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == 80U && converted == 4U &&
                     output[0] == 'a' && output[1] == 's' &&
                     output[2] == 's' && output[3] == 0U;
        printf("guest path conversion       : %s, %u import calls\n",
               convert_ok ? "UTF-16 width/count/truncate exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!convert_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const uint16_t source[] = {
            'a', 's', 's', 'e', 't', 's', 0U
        };
        char output[16];
        uint32_t converted = 0U, args[5], result;
        int convert_ok = 1;
        memset(output, 0xCC, sizeof output);
        args[0] = (uint32_t)(uintptr_t)&converted;
        args[1] = (uint32_t)(uintptr_t)output;
        args[2] = (uint32_t)sizeof output;
        args[3] = (uint32_t)(uintptr_t)source;
        args[4] = (uint32_t)sizeof output;
        result = call_host_cdecl_args(RVA_IAT_WCSTOMBS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == 0U && converted == 7U &&
                     strcmp(output, "assets") == 0;
        memset(output, 0xCC, sizeof output);
        converted = 0U;
        args[2] = 4U;
        args[4] = UINT32_MAX;       /* _TRUNCATE */
        result = call_host_cdecl_args(RVA_IAT_WCSTOMBS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == 80U && converted == 4U &&
                     memcmp(output, "ass\0", 4U) == 0;
        printf("guest filename conversion   : %s, %u import calls\n",
               convert_ok ? "UTF-16 input/count/truncate exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!convert_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const uint8_t narrow_source[] = {
            0x80U, 0x81U, 0x9fU, 0xc0U, 0xffU, 0U
        };
        static const uint16_t expected_wide[] = {
            0x0080U, 0x0081U, 0x009fU, 0x00c0U, 0x00ffU, 0U
        };
        uint16_t wide[sizeof narrow_source / sizeof narrow_source[0]];
        uint8_t roundtrip[sizeof narrow_source];
        uint32_t converted = 0U, args[5], result;
        int convert_ok = 1;

        memset(wide, 0xcc, sizeof wide);
        args[0] = (uint32_t)(uintptr_t)&converted;
        args[1] = (uint32_t)(uintptr_t)wide;
        args[2] = (uint32_t)(sizeof wide / sizeof wide[0]);
        args[3] = (uint32_t)(uintptr_t)narrow_source;
        args[4] = (uint32_t)sizeof narrow_source;
        result = call_host_cdecl_args(RVA_IAT_MBSTOWCS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == 0U && converted == 6U &&
                     memcmp(wide, expected_wide, sizeof wide) == 0;

        memset(roundtrip, 0xcc, sizeof roundtrip);
        converted = 0U;
        args[1] = (uint32_t)(uintptr_t)roundtrip;
        args[2] = (uint32_t)sizeof roundtrip;
        args[3] = (uint32_t)(uintptr_t)wide;
        args[4] = (uint32_t)sizeof roundtrip;
        result = call_host_cdecl_args(RVA_IAT_WCSTOMBS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == 0U && converted == 6U &&
                     memcmp(roundtrip, narrow_source,
                            sizeof narrow_source) == 0;

        wide[0] = 0x0100U;
        wide[1] = 0U;
        memset(roundtrip, 0xcc, sizeof roundtrip);
        converted = 0xccccccccU;
        result = call_host_cdecl_args(RVA_IAT_WCSTOMBS_S,
                                      args, 5U, &convert_ok);
        convert_ok = convert_ok && result == EILSEQ && converted == 0U &&
                     roundtrip[0] == 0U;
        printf("C-locale byte conversion    : %s, %u import calls\n",
               convert_ok ? "0x80..0xff identity; U+0100 EILSEQ" : "FAILED",
               g_host_import_calls - calls_before);
        if (!convert_ok || g_host_import_calls - calls_before != 3U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char userprofile[] = "USERPROFILE";
        static const char unknown[] = "REPENTOGXM_TEST_VALUE";
        uint32_t args[1];
        uint32_t known_result, unknown_result;
        int environment_ok = 1;
        args[0] = (uint32_t)(uintptr_t)userprofile;
        known_result = call_host_cdecl_args(RVA_IAT_GETENV,
                                            args, 1U, &environment_ok);
        args[0] = (uint32_t)(uintptr_t)unknown;
        unknown_result = call_host_cdecl_args(RVA_IAT_GETENV,
                                              args, 1U, &environment_ok);
        environment_ok = environment_ok && known_result == 0U &&
                         unknown_result == 0U;
        printf("target process environment  : %s, %u import calls\n",
               environment_ok ? "absent + cdecl exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!environment_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        int ctype_ok = call_host_cdecl_onearg(RVA_IAT_ISDIGIT, '0', 0x04U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISDIGIT, '9', 0x04U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISDIGIT, '/', 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISDIGIT, ':', 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISDIGIT,
                                              UINT32_MAX, 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISSPACE, ' ', 0x08U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISSPACE, '\t', 0x08U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISSPACE, 'A', 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISSPACE,
                                              UINT32_C(0xFFFFFF80), 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISPUNCT, '/', 0x10U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISPUNCT, '@', 0x10U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISPUNCT, 'A', 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISPUNCT,
                                              UINT32_C(0xFFFFFF80), 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_TOLOWER, 'A', 'a') &&
                       call_host_cdecl_onearg(RVA_IAT_TOLOWER, 'Z', 'z') &&
                       call_host_cdecl_onearg(RVA_IAT_TOLOWER, 'z', 'z') &&
                       call_host_cdecl_onearg(RVA_IAT_TOLOWER,
                                              UINT32_C(0xFFFFFF80),
                                              UINT32_C(0xFFFFFF80)) &&
                       call_host_cdecl_onearg(RVA_IAT_TOUPPER, 'a', 'A') &&
                       call_host_cdecl_onearg(RVA_IAT_TOUPPER, 'z', 'Z') &&
                       call_host_cdecl_onearg(RVA_IAT_TOUPPER, 'Z', 'Z') &&
                       call_host_cdecl_onearg(RVA_IAT_TOUPPER,
                                              UINT32_C(0xFFFFFF80),
                                              UINT32_C(0xFFFFFF80)) &&
                       call_host_cdecl_onearg(RVA_IAT_ISWSPACE, 0x20U, 0x08U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISWSPACE, 0xA0U, 0x08U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISWSPACE,
                                              0x2007U, 0x08U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISWSPACE, 'A', 0U) &&
                       call_host_cdecl_onearg(RVA_IAT_ISWSPACE, 0xFEFFU, 0U);
        printf("guest C-locale ctype family : %s, %u import calls\n",
               ctype_ok ? "masks/folding/Unicode-space/cdecl exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!ctype_ok || g_host_import_calls - calls_before != 26U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char path_format[] = "%s%s/";
        static const char null_format[] = "%s/";
        static const char drive[] = "X:";
        static const char path[] = "\\save";
        static const char long_text[] = "abcdef";
        static const char string_format[] = "%s";
        static const char integer_format[] = "%d/%d";
        static const char integer_expected[] = "42/-2147483648";
        static const char hex_format[] = "%X";
        static const char hex_expected[] = "80004005";
        char output[32];
        uint32_t varargs[2], args[7], result;
        int format_ok = 1;

        varargs[0] = (uint32_t)(uintptr_t)drive;
        varargs[1] = (uint32_t)(uintptr_t)path;
        args[0] = 1U; args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = (uint32_t)sizeof output;
        args[4] = (uint32_t)(uintptr_t)path_format;
        args[5] = 0U;
        args[6] = (uint32_t)(uintptr_t)varargs;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == 8U &&
                    strcmp(output, "X:\\save/") == 0;

        varargs[0] = 0U;
        args[4] = (uint32_t)(uintptr_t)null_format;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == 7U &&
                    strcmp(output, "(null)/") == 0;

        varargs[0] = 42U;
        varargs[1] = (uint32_t)INT32_MIN;
        args[0] = 2U;
        args[3] = (uint32_t)sizeof output;
        args[4] = (uint32_t)(uintptr_t)integer_format;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == sizeof integer_expected - 1U &&
                    strcmp(output, integer_expected) == 0;

        varargs[0] = 0x80004005U;
        args[4] = (uint32_t)(uintptr_t)hex_format;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == sizeof hex_expected - 1U &&
                    strcmp(output, hex_expected) == 0;

        memset(output, 0xCC, sizeof output);
        varargs[0] = (uint32_t)(uintptr_t)long_text;
        args[0] = 1U;                /* legacy _vsnprintf behaviour */
        args[3] = 4U;
        args[4] = (uint32_t)(uintptr_t)string_format;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == UINT32_MAX &&
                    memcmp(output, "abcd", 4U) == 0;
        memset(output, 0xCC, sizeof output);
        args[3] = 6U;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == 6U &&
                    memcmp(output, "abcdef", 6U) == 0 &&
                    (unsigned char)output[6] == 0xCCU;

        memset(output, 0xCC, sizeof output);
        args[0] = 2U;                /* STANDARD_SNPRINTF_BEHAVIOR */
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = 4U;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == 6U &&
                    memcmp(output, "abc\0", 4U) == 0;

        args[2] = 0U;
        args[3] = 0U;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == 6U;
        args[0] = 1U;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &format_ok);
        format_ok = format_ok && result == 6U;
        printf("guest narrow formatting    : %s, %u import calls\n",
               format_ok ? "strings/int/hex/null/legacy/standard exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!format_ok || g_host_import_calls - calls_before != 9U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char format[] =
            "%u|%02u|%03u|%08x|%.16s|% d|%2d|%02d|%02d|%4d|%ld|"
            "%f|%.f|%.1f|%.2f|%.4f|%g|%llu|%016llx|%08llx|%u|%%";
        static const char text[] = "abcdefghijklmnopQRST";
        static const char expected[] =
            "4294967295|07|007|00001a2b|abcdefghijklmnop| 7| 7|03|-3|"
            "   7|-2147483648|1.500000|12|2.5|-2.25|0.1250|123.5|"
            "1234605616436508552|0000000000001234|00001234|77|%";
        char output[256];
        uint32_t varargs[30] = {
            UINT32_MAX, 7U, 7U, 0x1A2BU,
            (uint32_t)(uintptr_t)text,
            7U, 7U, 3U, (uint32_t)-3, 7U, (uint32_t)INT32_MIN,
            0x00000000U, 0x3FF80000U, /* 1.5 */
            0x00000000U, 0x40280000U, /* 12.0 */
            0x00000000U, 0x40040000U, /* 2.5 */
            0x00000000U, 0xC0020000U, /* -2.25 */
            0x00000000U, 0x3FC00000U, /* 0.125 */
            0x00000000U, 0x405EE000U, /* 123.5 */
            0x55667788U, 0x11223344U, /* %llu */
            0x00001234U, 0x00000000U, /* %016llx */
            0x00001234U, 0x00000000U, /* %08llx */
            77U
        };
        uint32_t args[7], result;
        int measured_ok = 1;

        args[0] = 2U; args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = (uint32_t)sizeof output;
        args[4] = (uint32_t)(uintptr_t)format;
        args[5] = 0U;
        args[6] = (uint32_t)(uintptr_t)varargs;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &measured_ok);
        measured_ok = measured_ok && result == sizeof expected - 1U &&
                      strcmp(output, expected) == 0;
        printf("guest measured formatting  : %s, %u import calls\n",
               measured_ok ? "measured widths/precision/double/u64 + va_list exact"
                           : "FAILED",
               g_host_import_calls - calls_before);
        if (!measured_ok || g_host_import_calls - calls_before != 1U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char format[] = "ISAACNG_GSR%04u";
        static const char expected[] = "ISAACNG_GSR0146";
        char output[sizeof expected];
        uint32_t varargs[1] = { 0x92U };
        uint32_t args[7], result;
        int gsr_ok = 1;

        memset(output, 0xCC, sizeof output);
        /* Exact live sprintf lowering: legacy options, unbounded count,
         * 32-bit x86 va_list and one unsigned game-state version. */
        args[0] = 0x25U; args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = UINT32_MAX;
        args[4] = (uint32_t)(uintptr_t)format;
        args[5] = 0U;
        args[6] = (uint32_t)(uintptr_t)varargs;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &gsr_ok);
        gsr_ok = gsr_ok && result == sizeof expected - 1U &&
                 memcmp(output, expected, sizeof expected) == 0;
        printf("guest GSR formatting       : %s, %u import call\n",
               gsr_ok ? "ISAACNG_GSR%04u -> ISAACNG_GSR0146 exact"
                      : "FAILED",
               g_host_import_calls - calls_before);
        if (!gsr_ok || g_host_import_calls - calls_before != 1U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const uint8_t format_bytes[] = {
            0x25U, 0x30U, 0x33U, 0x64U, 0x00U,
        };
        static const uint8_t caller_window[] = {
            0xb9U, 0xa8U, 0xe7U, 0x74U, 0x30U, 0x0fU, 0x44U, 0xcaU,
            0xffU, 0xb6U, 0xb8U, 0x12U, 0x00U, 0x00U, 0x51U, 0x6aU,
            0x10U, 0x68U, 0xfcU, 0x70U, 0x80U, 0x30U, 0xe8U, 0xb9U,
            0x3fU, 0xb1U, 0xffU,
        };
        static const uint8_t sprintf_wrapper[] = {
            0x55U, 0x8bU, 0xecU, 0x51U, 0x8bU, 0x55U, 0x0cU, 0x8dU,
            0x45U, 0x14U, 0x50U, 0x51U, 0xffU, 0x75U, 0x10U, 0x8bU,
            0x4dU, 0x08U, 0xe8U, 0xd9U, 0x95U, 0xffU, 0xffU, 0x83U,
            0xc4U, 0x0cU, 0x59U, 0x5dU, 0xc3U,
        };
        static const uint8_t stdio_adapter[] = {
            0x55U, 0x8bU, 0xecU, 0x83U, 0xe4U, 0xf8U, 0xffU, 0x75U,
            0x10U, 0x6aU, 0x00U, 0xffU, 0x75U, 0x08U, 0x52U, 0x51U,
            0xe8U, 0xdbU, 0xffU, 0xffU, 0xffU, 0x8bU, 0x08U, 0xffU,
            0x70U, 0x04U, 0x83U, 0xc9U, 0x01U, 0x51U, 0xffU, 0x15U,
            0x14U, 0x66U, 0x60U, 0x30U, 0x83U, 0xc9U, 0xffU, 0x83U,
            0xc4U, 0x1cU, 0x85U, 0xc0U, 0x0fU, 0x48U, 0xc1U, 0x8bU,
            0xe5U, 0x5dU, 0xc3U,
        };
        static const char pair_format[] = "%03d|%03d";
        static const char expected[] = "007|-07";
        char output[16];
        uint32_t varargs[2] = { 7U, (uint32_t)-7 };
        uint32_t args[7], result;
        uint32_t slot = ld32(GUEST_IMAGE_BASE + RVA_IAT_VSPRINTF);
        const char *name_by_rva = guest_import_name(RVA_IAT_VSPRINTF);
        const char *name_by_va = guest_import_name(slot);
        int evidence_ok, width_ok = 1;

        evidence_ok =
            guest_image_bytes_equal(RVA_WIDTH3_FORMAT, format_bytes,
                                    (unsigned)sizeof format_bytes) &&
            guest_image_bytes_fnv64(RVA_WIDTH3_FORMAT,
                                    (unsigned)sizeof format_bytes) ==
                WIDTH3_FORMAT_FNV64 &&
            guest_image_bytes_equal(RVA_WIDTH3_CALLER_WINDOW, caller_window,
                                    (unsigned)sizeof caller_window) &&
            guest_image_bytes_fnv64(RVA_WIDTH3_CALLER_WINDOW,
                                    (unsigned)sizeof caller_window) ==
                WIDTH3_CALLER_FNV64 &&
            guest_image_bytes_equal(RVA_WIDTH3_SPRINTF_WRAPPER,
                                    sprintf_wrapper,
                                    (unsigned)sizeof sprintf_wrapper) &&
            guest_image_bytes_fnv64(RVA_WIDTH3_SPRINTF_WRAPPER,
                                    (unsigned)sizeof sprintf_wrapper) ==
                WIDTH3_WRAPPER_FNV64 &&
            guest_image_bytes_equal(RVA_WIDTH3_STDIO_ADAPTER, stdio_adapter,
                                    (unsigned)sizeof stdio_adapter) &&
            guest_image_bytes_fnv64(RVA_WIDTH3_STDIO_ADAPTER,
                                    (unsigned)sizeof stdio_adapter) ==
                WIDTH3_ADAPTER_FNV64 &&
            RVA_WIDTH3_CALLER_WINDOW + sizeof caller_window ==
                RVA_WIDTH3_GAME_RETURN &&
            RVA_WIDTH3_GAME_CALL + 5U == RVA_WIDTH3_GAME_RETURN &&
            ld8(GUEST_IMAGE_BASE + RVA_WIDTH3_GAME_CALL) == 0xe8U &&
            RVA_WIDTH3_GAME_RETURN +
                ld32(GUEST_IMAGE_BASE + RVA_WIDTH3_GAME_CALL + 1U) ==
                    RVA_WIDTH3_SPRINTF_WRAPPER &&
            RVA_WIDTH3_WRAPPER_CALL + 5U == RVA_WIDTH3_WRAPPER_RETURN &&
            ld8(GUEST_IMAGE_BASE + RVA_WIDTH3_WRAPPER_CALL) == 0xe8U &&
            RVA_WIDTH3_WRAPPER_RETURN +
                ld32(GUEST_IMAGE_BASE + RVA_WIDTH3_WRAPPER_CALL + 1U) ==
                    RVA_WIDTH3_STDIO_ADAPTER &&
            RVA_WIDTH3_IAT_CALL + 6U == RVA_WIDTH3_IAT_RETURN &&
            ld8(GUEST_IMAGE_BASE + RVA_WIDTH3_IAT_CALL) == 0xffU &&
            ld8(GUEST_IMAGE_BASE + RVA_WIDTH3_IAT_CALL + 1U) == 0x15U &&
            ld32(GUEST_IMAGE_BASE + RVA_WIDTH3_IAT_CALL + 2U) ==
                GUEST_IMAGE_BASE + RVA_IAT_VSPRINTF &&
            slot == GUEST_IMAGE_BASE + RVA_IAT_VSPRINTF &&
            name_by_rva && name_by_va &&
            strcmp(name_by_rva, NAME_VSPRINTF) == 0 &&
            strcmp(name_by_va, NAME_VSPRINTF) == 0;

        /* Reproduce the collected set-070 boundary with the format bytes from
         * the loaded PE: options 0x25, 16-byte destination and x86 va_list. */
        memset(output, 0xCC, sizeof output);
        args[0] = 0x25U; args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = (uint32_t)sizeof output;
        args[4] = GUEST_IMAGE_BASE + RVA_WIDTH3_FORMAT;
        args[5] = 0U;
        args[6] = (uint32_t)(uintptr_t)varargs;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &width_ok);
        width_ok = width_ok && result == 3U &&
                   memcmp(output, "007\0", 4U) == 0 &&
                   (unsigned char)output[4] == 0xCCU;

        /* Keep both sign positions explicit without pretending a second live
         * token exists in the PE. */
        memset(output, 0xCC, sizeof output);
        args[3] = UINT32_MAX;
        args[4] = (uint32_t)(uintptr_t)pair_format;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &width_ok);
        width_ok = width_ok && result == sizeof expected - 1U &&
                   memcmp(output, expected, sizeof expected) == 0;

        varargs[0] = 7U;
        args[0] = 2U;                /* STANDARD_SNPRINTF_BEHAVIOR */
        args[3] = 3U;
        args[4] = GUEST_IMAGE_BASE + RVA_WIDTH3_FORMAT;
        memset(output, 0xCC, sizeof output);
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &width_ok);
        width_ok = width_ok && result == 3U &&
                   memcmp(output, "00\0", 3U) == 0 &&
                   (unsigned char)output[3] == 0xCCU;

        args[2] = 0U;
        args[3] = 0U;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &width_ok);
        width_ok = width_ok && result == 3U;

        args[0] = 1U;                /* legacy _vsnprintf behaviour */
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = 3U;
        memset(output, 0xCC, sizeof output);
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &width_ok);
        width_ok = width_ok && result == 3U &&
                   memcmp(output, "007", 3U) == 0 &&
                   (unsigned char)output[3] == 0xCCU;

        args[3] = 2U;
        memset(output, 0xCC, sizeof output);
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF,
                                      args, 7U, &width_ok);
        width_ok = width_ok && result == UINT32_MAX &&
                   memcmp(output, "00", 2U) == 0 &&
                   (unsigned char)output[2] == 0xCCU;

        printf("guest %%03d formatting      : %s, %u import calls\n",
               evidence_ok && width_ok
                   ? "PE chain/sign/ABI/count/legacy/standard exact"
                   : "FAILED",
               g_host_import_calls - calls_before);
        if (!evidence_ok || !width_ok ||
            g_host_import_calls - calls_before != 6U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        unsigned faults_before = g_fault_count;
        static const char format[] = "%05u";
        char output[8];
        uint32_t varargs[1] = { 0x92U };
        uint32_t saved = cpu.esp;
        int stopped, narrow_ok;

        guarded_format_args[0] = 0x25U;
        guarded_format_args[1] = 0U;
        guarded_format_args[2] = (uint32_t)(uintptr_t)output;
        guarded_format_args[3] = UINT32_MAX;
        guarded_format_args[4] = (uint32_t)(uintptr_t)format;
        guarded_format_args[5] = 0U;
        guarded_format_args[6] = (uint32_t)(uintptr_t)varargs;
        guarded_format_token = ld32(GUEST_IMAGE_BASE + RVA_IAT_VSPRINTF);
        continued_after_guarded_format = 0;
        stopped = guest_run_until_stop(&cpu, call_guarded_format);
        narrow_ok = stopped == GUEST_RUN_FAULT && cpu.fault &&
                    strcmp(cpu.fault, "guest stdio unsupported format") == 0 &&
                    cpu.fault_addr == (uint32_t)(uintptr_t)format &&
                    !continued_after_guarded_format &&
                    cpu.esp == saved - 32U &&
                    ld32(cpu.esp) == 0xABC06614U &&
                    g_fault_count == faults_before + 1U &&
                    g_host_import_calls == calls_before + 1U;
        cpu.esp = saved;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;

        printf("guest GSR narrow grammar   : %s, %u import call\n",
               narrow_ok ? "%05u remains a loud fault" : "FAILED",
               g_host_import_calls - calls_before);
        if (!narrow_ok)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char format[] = "%d-%s";
        static const char text[] = "ok";
        char output[16];
        uint32_t varargs[2] = { 7U, (uint32_t)(uintptr_t)text };
        uint32_t args[7], result;
        int secure_ok = 1;
        /* Exact local option installed by this PE's CRT startup. */
        args[0] = 2U; args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)output;
        args[3] = (uint32_t)sizeof output;
        args[4] = (uint32_t)(uintptr_t)format;
        args[5] = 0U;
        args[6] = (uint32_t)(uintptr_t)varargs;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF_S,
                                      args, 7U, &secure_ok);
        secure_ok = secure_ok && result == 4U && strcmp(output, "7-ok") == 0;
        memset(output, 0xCC, sizeof output);
        args[3] = 4U;
        result = call_host_cdecl_args(RVA_IAT_VSPRINTF_S,
                                      args, 7U, &secure_ok);
        secure_ok = secure_ok && result == UINT32_MAX && output[0] == '\0';
        printf("guest secure formatting    : %s, %u import calls\n",
               secure_ok ? "success/overflow reset exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!secure_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        unsigned faults_before = g_fault_count;
        static const char version_input[] = "2.0 VitaGL inventory";
        static const char version_format[] = "%i.%i";
        static const char shader_input[] = "ISAACNG_GSR4294967295";
        static const char shader_format[] = "ISAACNG_GSR%u";
        static const char triple_input[] = "-3.12.0 NVIDIA";
        static const char triple_format[] = "%d.%d.%d";
        static const char partial_input[] = "7.x";
        static const char unknown_format[] = "%f";
        uint32_t outputs[3], varargs[3], args[7], result;
        uint32_t saved = cpu.esp;
        int scan_ok = 1, stopped;

        args[0] = 0U; args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)version_input;
        args[3] = UINT32_MAX;
        args[4] = (uint32_t)(uintptr_t)version_format;
        args[5] = 0U;
        args[6] = (uint32_t)(uintptr_t)varargs;
        outputs[0] = outputs[1] = UINT32_MAX;
        varargs[0] = (uint32_t)(uintptr_t)&outputs[0];
        varargs[1] = (uint32_t)(uintptr_t)&outputs[1];
        result = call_host_cdecl_args(RVA_IAT_VSSCANF, args, 7U, &scan_ok);
        scan_ok = scan_ok && result == 2U &&
                  outputs[0] == 2U && outputs[1] == 0U;

        args[2] = (uint32_t)(uintptr_t)shader_input;
        args[4] = (uint32_t)(uintptr_t)shader_format;
        outputs[0] = 0U;
        varargs[0] = (uint32_t)(uintptr_t)&outputs[0];
        result = call_host_cdecl_args(RVA_IAT_VSSCANF, args, 7U, &scan_ok);
        scan_ok = scan_ok && result == 1U && outputs[0] == UINT32_MAX;

        args[2] = (uint32_t)(uintptr_t)triple_input;
        args[4] = (uint32_t)(uintptr_t)triple_format;
        outputs[0] = outputs[1] = outputs[2] = UINT32_MAX;
        varargs[0] = (uint32_t)(uintptr_t)&outputs[0];
        varargs[1] = (uint32_t)(uintptr_t)&outputs[1];
        varargs[2] = (uint32_t)(uintptr_t)&outputs[2];
        result = call_host_cdecl_args(RVA_IAT_VSSCANF, args, 7U, &scan_ok);
        scan_ok = scan_ok && result == 3U &&
                  outputs[0] == (uint32_t)-3 && outputs[1] == 12U &&
                  outputs[2] == 0U;

        args[2] = (uint32_t)(uintptr_t)partial_input;
        args[4] = (uint32_t)(uintptr_t)version_format;
        outputs[0] = outputs[1] = UINT32_MAX;
        varargs[0] = (uint32_t)(uintptr_t)&outputs[0];
        varargs[1] = (uint32_t)(uintptr_t)&outputs[1];
        result = call_host_cdecl_args(RVA_IAT_VSSCANF, args, 7U, &scan_ok);
        scan_ok = scan_ok && result == 1U && outputs[0] == 7U &&
                  outputs[1] == UINT32_MAX;

        memcpy(guarded_scan_args, args, sizeof guarded_scan_args);
        guarded_scan_args[4] = (uint32_t)(uintptr_t)unknown_format;
        guarded_scan_token = ld32(GUEST_IMAGE_BASE + RVA_IAT_VSSCANF);
        continued_after_guarded_scan = 0;
        cpu.esp = saved;
        stopped = guest_run_until_stop(&cpu, call_guarded_scan);
        scan_ok = scan_ok && stopped == GUEST_RUN_FAULT && cpu.fault &&
                  strcmp(cpu.fault, "guest stdio unsupported scan format") == 0 &&
                  cpu.fault_addr == (uint32_t)(uintptr_t)unknown_format &&
                  !continued_after_guarded_scan && cpu.esp == saved - 32U &&
                  ld32(cpu.esp) == 0xABC06610U &&
                  g_fault_count == faults_before + 1U;
        cpu.esp = saved;
        cpu.fault = NULL;
        cpu.fault_addr = 0U;

        printf("guest narrow scanning      : %s, %u import calls\n",
               scan_ok ? "legacy option + three formats/partial/loud exact"
                       : "FAILED",
               g_host_import_calls - calls_before);
        if (!scan_ok || g_host_import_calls - calls_before != 5U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char current_directory[] = ".";
        static const char empty_path[] = "";
        uint32_t args[2];
        uint32_t current, missing;
        int attributes_ok = 1;
        args[0] = (uint32_t)(uintptr_t)current_directory;
        current = call_host_stdcall_args(RVA_IAT_FILE_ATTRS,
                                         args, 1U, &attributes_ok);
        args[0] = (uint32_t)(uintptr_t)empty_path;
        missing = call_host_stdcall_args(RVA_IAT_FILE_ATTRS,
                                         args, 1U, &attributes_ok);
        attributes_ok = attributes_ok &&
                        current != UINT32_MAX &&
                        (current & 0x10U) != 0U && /* FILE_ATTRIBUTE_DIRECTORY */
                        missing == UINT32_MAX;    /* INVALID_FILE_ATTRIBUTES */
        printf("PC filesystem attributes   : %s, %u import calls\n",
               attributes_ok ? "directory/missing + stdcall exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!attributes_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char current_directory[] = ".";
        uint32_t args[2], result;
        int create_ok = 1;
        args[0] = (uint32_t)(uintptr_t)current_directory;
        args[1] = 0U;
        result = call_host_stdcall_args(RVA_IAT_CREATE_DIR,
                                        args, 2U, &create_ok);
        create_ok = create_ok && result == 0U; /* already exists */
        printf("PC directory creation      : %s, %u import calls\n",
               create_ok ? "existing-path + stdcall exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!create_ok || g_host_import_calls - calls_before != 1U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        char directory[260];
        uint32_t args[2], result;
        int current_ok = 1;
        memset(directory, 0xCC, sizeof directory);
        args[0] = (uint32_t)sizeof directory;
        args[1] = (uint32_t)(uintptr_t)directory;
        result = call_host_stdcall_args(RVA_IAT_CURRENT_DIR,
                                        args, 2U, &current_ok);
        current_ok = current_ok && result > 0U &&
                     result < (uint32_t)sizeof directory &&
                     directory[result] == '\0' && strlen(directory) == result;
        printf("PC current directory       : %s, %u import calls\n",
               current_ok ? "bounded string + stdcall exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!current_ok || g_host_import_calls - calls_before != 1U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const uint16_t dot_path[] = { '.', 0U };
        uint16_t output[260];
        uint32_t file_part = 0U, args[4], required, result;
        uint32_t output_addr = (uint32_t)(uintptr_t)output;
        int full_path_ok = 1;
        args[0] = (uint32_t)(uintptr_t)dot_path;
        args[1] = 0U;
        args[2] = 0U;
        args[3] = 0U;
        required = call_host_stdcall_args(RVA_IAT_FULL_PATH_W,
                                          args, 4U, &full_path_ok);
        memset(output, 0xCC, sizeof output);
        args[1] = 260U;
        args[2] = output_addr;
        args[3] = (uint32_t)(uintptr_t)&file_part;
        result = call_host_stdcall_args(RVA_IAT_FULL_PATH_W,
                                        args, 4U, &full_path_ok);
        full_path_ok = full_path_ok && result > 0U && result < 260U &&
                       required == result + 1U && output[result] == 0U &&
                       file_part >= output_addr &&
                       file_part <= output_addr + result * 2U;
        printf("PC full-path boundary       : %s, %u import calls\n",
               full_path_ok ? "size/UTF-16/file-part stdcall exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!full_path_ok || g_host_import_calls - calls_before != 2U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const uint16_t wildcard[] = { '*', 0U };
        /* Exact x86 WIN32_FIND_DATAW: 11 dwords followed by MAX_PATH UTF-16
         * code units and 14 alternate-name units, total 592 bytes. */
        unsigned char data[592];
        uint32_t args[2], handle, next, closed;
        int find_ok = 1;
        memset(&data, 0, sizeof data);
        args[0] = (uint32_t)(uintptr_t)wildcard;
        args[1] = (uint32_t)(uintptr_t)&data;
        handle = call_host_stdcall_args(RVA_IAT_FIND_FIRST_W,
                                        args, 2U, &find_ok);
        find_ok = find_ok && handle != UINT32_MAX &&
                  ld16((uint32_t)(uintptr_t)data + 44U) != 0U;
        args[0] = handle;
        args[1] = (uint32_t)(uintptr_t)&data;
        next = call_host_stdcall_args(RVA_IAT_FIND_NEXT_W,
                                      args, 2U, &find_ok);
        find_ok = find_ok && (next == 0U || next == 1U);
        args[0] = handle;
        closed = call_host_stdcall_args(RVA_IAT_FIND_CLOSE,
                                        args, 1U, &find_ok);
        find_ok = find_ok && closed == 1U;
        printf("PC directory iterator      : %s, %u import calls\n",
               find_ok ? "first/next/FindClose family exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!find_ok || g_host_import_calls - calls_before != 3U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char nul_name[] = "NUL";
        static const char write_mode[] = "w";
        static const char format[] = "Save Data Path: %s\n";
        static const char path[] = "./Documents";
        static const char expected[] = "Save Data Path: ./Documents\n";
        uint32_t args[6], varargs[1], handle, result;
        FILE *scratch;
        char line[64];
        int file_ok = 1;

        args[0] = (uint32_t)(uintptr_t)nul_name;
        args[1] = (uint32_t)(uintptr_t)write_mode;
        handle = call_host_cdecl_args(RVA_IAT_FOPEN, args, 2U, &file_ok);
        file_ok = file_ok && handle != 0U;
        args[0] = handle;
        result = call_host_cdecl_args(RVA_IAT_FCLOSE, args, 1U, &file_ok);
        file_ok = file_ok && result == 0U;

        scratch = tmpfile();
        file_ok = file_ok && scratch != NULL;
        if (scratch) {
            varargs[0] = (uint32_t)(uintptr_t)path;
            args[0] = 2U;       /* STANDARD_SNPRINTF_BEHAVIOR */
            args[1] = 0U;
            args[2] = (uint32_t)(uintptr_t)scratch;
            args[3] = (uint32_t)(uintptr_t)format;
            args[4] = 0U;
            args[5] = (uint32_t)(uintptr_t)varargs;
            result = call_host_cdecl_args(RVA_IAT_VFPRINTF,
                                          args, 6U, &file_ok);
            file_ok = file_ok && result == sizeof expected - 1U &&
                      fflush(scratch) == 0;
            rewind(scratch);
            memset(line, 0, sizeof line);
            file_ok = file_ok && fgets(line, sizeof line, scratch) != NULL &&
                      strcmp(line, expected) == 0;
            args[0] = (uint32_t)(uintptr_t)scratch;
            result = call_host_cdecl_args(RVA_IAT_FCLOSE,
                                          args, 1U, &file_ok);
            file_ok = file_ok && result == 0U;
        }
        printf("single native FILE boundary : %s, %u import calls\n",
               file_ok ? "open/format/read/close exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!file_ok || g_host_import_calls - calls_before != 4U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char file_name[] = "guest_stdio_boundary_test.bin";
        static const char update_mode[] = "w+b";
        static const unsigned char payload[] = "ABCdef012xyz";
        unsigned char received[6];
        uint32_t overlapped[5] = { 0U, 0U, 0U, 0U, 0U };
        uint32_t args[6], stream, result, fd, os_handle;
        int io_ok = 1;

        (void)remove(file_name);
        args[0] = (uint32_t)(uintptr_t)file_name;
        args[1] = (uint32_t)(uintptr_t)update_mode;
        stream = call_host_cdecl_args(RVA_IAT_FOPEN, args, 2U, &io_ok);
        io_ok = io_ok && stream != 0U;
        if (stream) {
            /* size=3 proves fread/fwrite return complete ITEMS, while the
             * destination comparison proves the guest buffer itself crossed
             * the native FILE boundary. */
            args[0] = (uint32_t)(uintptr_t)payload;
            args[1] = 3U;
            args[2] = 4U;
            args[3] = stream;
            result = call_host_cdecl_args(RVA_IAT_FWRITE,
                                          args, 4U, &io_ok);
            io_ok = io_ok && result == 4U;

            args[0] = stream;
            result = call_host_cdecl_args(RVA_IAT_FFLUSH,
                                          args, 1U, &io_ok);
            io_ok = io_ok && result == 0U;
            result = call_host_cdecl_args(RVA_IAT_FTELL,
                                          args, 1U, &io_ok);
            io_ok = io_ok && result == 12U;

            args[0] = stream;
            args[1] = (uint32_t)(int32_t)-6;
            args[2] = SEEK_END;
            result = call_host_cdecl_args(RVA_IAT_FSEEK,
                                          args, 3U, &io_ok);
            io_ok = io_ok && result == 0U;
            args[0] = stream;
            result = call_host_cdecl_args(RVA_IAT_FTELL,
                                          args, 1U, &io_ok);
            io_ok = io_ok && result == 6U;

            memset(received, 0, sizeof received);
            args[0] = (uint32_t)(uintptr_t)received;
            args[1] = 3U;
            args[2] = 2U;
            args[3] = stream;
            result = call_host_cdecl_args(RVA_IAT_FREAD,
                                          args, 4U, &io_ok);
            io_ok = io_ok && result == 2U &&
                    memcmp(received, payload + 6, sizeof received) == 0;

            args[0] = stream;
            fd = call_host_cdecl_args(RVA_IAT_FILENO,
                                      args, 1U, &io_ok);
            io_ok = io_ok && (int32_t)fd >= 0;
            args[0] = fd;
            os_handle = call_host_cdecl_args(RVA_IAT_GET_OSFHANDLE,
                                             args, 1U, &io_ok);
            io_ok = io_ok && os_handle != UINT32_MAX;

            /* Exact x86 OVERLAPPED: five dwords, with Offset/OffsetHigh at
             * words 2/3.  Lock one byte at offset zero, then release it via
             * the same CRT fd -> native HANDLE bridge used by the guest. */
            args[0] = os_handle;
            args[1] = 3U;       /* EXCLUSIVE | FAIL_IMMEDIATELY */
            args[2] = 0U;
            args[3] = 1U;
            args[4] = 0U;
            args[5] = (uint32_t)(uintptr_t)overlapped;
            result = call_host_stdcall_args(RVA_IAT_LOCK_FILE_EX,
                                            args, 6U, &io_ok);
            io_ok = io_ok && result == 1U;
            args[0] = os_handle;
            args[1] = 0U;
            args[2] = 1U;
            args[3] = 0U;
            args[4] = (uint32_t)(uintptr_t)overlapped;
            result = call_host_stdcall_args(RVA_IAT_UNLOCK_FILE_EX,
                                            args, 5U, &io_ok);
            io_ok = io_ok && result == 1U;

            args[0] = stream;
            result = call_host_cdecl_args(RVA_IAT_FCLOSE,
                                          args, 1U, &io_ok);
            io_ok = io_ok && result == 0U;
        }
        io_ok = io_ok && remove(file_name) == 0;
        printf("native FILE/fd lock lifecycle: %s, %u import calls\n",
               io_ok ? "items/buffers/signed seek/HANDLE exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!io_ok || g_host_import_calls - calls_before != 12U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        static const char source[] = "abcdef";
        static const char suffix[] = "/xy";
        static const char path[] = "root/child/file";
        static const char slash[] = "/";
        static const char absent_delimiters[] = ":\\";
        static const char case_left[] = "AbC";
        static const char case_right[] = "aBc";
        static const char punct_left[] = "A@";
        static const char punct_right[] = "a[";
        static const unsigned char high_left[] = { 0xFFU, 0U };
        static const unsigned char high_right[] = { 0x01U, 0U };
        static const unsigned char fold_high_left[] = { 0xC0U, 0U };
        static const unsigned char fold_high_right[] = { 0xE0U, 0U };
        char output[16];
        char zero_capacity_sentinel[sizeof output];
        uint32_t args[4], result;
        int strings_ok = 1;

        memset(output, 0xCC, sizeof output);
        args[0] = (uint32_t)(uintptr_t)output;
        args[1] = (uint32_t)(uintptr_t)source;
        args[2] = 9U;
        result = call_host_cdecl_args(RVA_IAT_STRNCPY,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == (uint32_t)(uintptr_t)output &&
                     memcmp(output, "abcdef\0\0\0", 9U) == 0 &&
                     (unsigned char)output[9] == 0xCCU;

        memset(output, 0xCC, sizeof output);
        args[2] = 3U;
        result = call_host_cdecl_args(RVA_IAT_STRNCPY,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == (uint32_t)(uintptr_t)output &&
                     memcmp(output, "abc", 3U) == 0 &&
                     (unsigned char)output[3] == 0xCCU;

        args[0] = (uint32_t)(uintptr_t)source;
        args[1] = (uint32_t)(uintptr_t)"abcdef";
        args[2] = 7U;
        result = call_host_cdecl_args(RVA_IAT_STRNCMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 0U;
        args[0] = (uint32_t)(uintptr_t)high_left;
        args[1] = (uint32_t)(uintptr_t)high_right;
        args[2] = 1U;
        result = call_host_cdecl_args(RVA_IAT_STRNCMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && (int32_t)result > 0;
        args[0] = (uint32_t)(uintptr_t)source;
        args[1] = (uint32_t)(uintptr_t)suffix;
        args[2] = 0U;
        result = call_host_cdecl_args(RVA_IAT_STRNCMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 0U;

        args[0] = (uint32_t)(uintptr_t)path;
        args[1] = (uint32_t)(uintptr_t)slash;
        result = call_host_cdecl_args(RVA_IAT_STRPBRK,
                                      args, 2U, &strings_ok);
        strings_ok = strings_ok &&
                     result == (uint32_t)(uintptr_t)(path + 4U);
        args[0] = result + 1U;
        result = call_host_cdecl_args(RVA_IAT_STRPBRK,
                                      args, 2U, &strings_ok);
        strings_ok = strings_ok &&
                     result == (uint32_t)(uintptr_t)(path + 10U);
        args[0] = (uint32_t)(uintptr_t)path;
        args[1] = (uint32_t)(uintptr_t)absent_delimiters;
        result = call_host_cdecl_args(RVA_IAT_STRPBRK,
                                      args, 2U, &strings_ok);
        strings_ok = strings_ok && result == 0U;

        args[0] = (uint32_t)(uintptr_t)case_left;
        args[1] = (uint32_t)(uintptr_t)case_right;
        args[2] = 3U;
        result = call_host_cdecl_args(RVA_IAT_STRNICMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 0U;
        args[0] = (uint32_t)(uintptr_t)punct_left;
        args[1] = (uint32_t)(uintptr_t)punct_right;
        args[2] = 2U;
        result = call_host_cdecl_args(RVA_IAT_STRNICMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && (int32_t)result == -27;
        args[0] = (uint32_t)(uintptr_t)"abc";
        args[1] = (uint32_t)(uintptr_t)"abd";
        args[2] = 2U;
        result = call_host_cdecl_args(RVA_IAT_STRNICMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 0U;
        args[0] = (uint32_t)(uintptr_t)fold_high_left;
        args[1] = (uint32_t)(uintptr_t)fold_high_right;
        args[2] = 1U;
        result = call_host_cdecl_args(RVA_IAT_STRNICMP,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && (int32_t)result == -32;

        memset(output, 0xCC, sizeof output);
        args[0] = (uint32_t)(uintptr_t)output;
        args[1] = (uint32_t)sizeof output;
        args[2] = (uint32_t)(uintptr_t)source;
        args[3] = 4U;
        result = call_host_cdecl_args(RVA_IAT_STRNCPY_S,
                                      args, 4U, &strings_ok);
        strings_ok = strings_ok && result == 0U &&
                     strcmp(output, "abcd") == 0;

        args[1] = 4U;
        args[3] = UINT32_MAX;       /* _TRUNCATE */
        result = call_host_cdecl_args(RVA_IAT_STRNCPY_S,
                                      args, 4U, &strings_ok);
        strings_ok = strings_ok && result == 80U &&
                     strcmp(output, "abc") == 0;

        args[1] = 3U;
        args[3] = 6U;
        result = call_host_cdecl_args(RVA_IAT_STRNCPY_S,
                                      args, 4U, &strings_ok);
        strings_ok = strings_ok && result == 34U && output[0] == '\0';

        memset(output, 0xCC, sizeof output);
        args[0] = (uint32_t)(uintptr_t)output;
        args[1] = (uint32_t)sizeof output;
        args[2] = (uint32_t)(uintptr_t)source;
        result = call_host_cdecl_args(RVA_IAT_STRCPY_S,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 0U &&
                     strcmp(output, source) == 0;

        args[1] = 4U;
        result = call_host_cdecl_args(RVA_IAT_STRCPY_S,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 34U && output[0] == '\0';

        output[0] = 'x';
        args[1] = (uint32_t)sizeof output;
        args[2] = 0U;
        result = call_host_cdecl_args(RVA_IAT_STRCPY_S,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 22U && output[0] == '\0';

        memset(output, 0xA5, sizeof output);
        memcpy(zero_capacity_sentinel, output, sizeof output);
        args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)source;
        result = call_host_cdecl_args(RVA_IAT_STRCPY_S,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 34U &&
                     memcmp(output, zero_capacity_sentinel,
                            sizeof output) == 0;

        memcpy(output, "base", 5U);
        args[0] = (uint32_t)(uintptr_t)output;
        args[1] = (uint32_t)sizeof output;
        args[2] = (uint32_t)(uintptr_t)suffix;
        result = call_host_cdecl_args(RVA_IAT_STRCAT_S,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 0U &&
                     strcmp(output, "base/xy") == 0;

        memcpy(output, "base", 5U);
        args[1] = 6U;
        result = call_host_cdecl_args(RVA_IAT_STRCAT_S,
                                      args, 3U, &strings_ok);
        strings_ok = strings_ok && result == 34U && output[0] == '\0';
        printf("guest string boundaries     : %s, %u import calls\n",
               strings_ok ? "copy/pad/truncate/range/append exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!strings_ok || g_host_import_calls - calls_before != 21U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t args[2], first, nested;
        int com_ok = 1;
        args[0] = 0U;
        first = call_host_stdcall_args(RVA_IAT_COINIT,
                                       args, 1U, &com_ok);
        args[0] = 0U;
        args[1] = 0U;
        nested = call_host_stdcall_args(RVA_IAT_COINIT_EX,
                                        args, 2U, &com_ok);
        (void)call_host_stdcall_args(RVA_IAT_COUNINIT,
                                     NULL, 0U, &com_ok);
        (void)call_host_stdcall_args(RVA_IAT_COUNINIT,
                                     NULL, 0U, &com_ok);
        com_ok = com_ok && first == 0U && nested == 1U;
        printf("target COM lifecycle        : %s, %u import calls\n",
               com_ok ? "S_OK/S_FALSE/balanced stdcall exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!com_ok || g_host_import_calls - calls_before != 4U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t caps[2] = {0U, 0U};
        uint32_t args[2], previous, begin, now, end;
        int timer_ok = 1;
        args[0] = 0x80000002U;  /* ES_CONTINUOUS | ES_DISPLAY_REQUIRED */
        previous = call_host_stdcall_args(RVA_IAT_EXEC_STATE,
                                          args, 1U, &timer_ok);
        args[0] = (uint32_t)(uintptr_t)caps;
        args[1] = (uint32_t)sizeof caps;
        (void)call_host_stdcall_args(RVA_IAT_TIME_CAPS,
                                     args, 2U, &timer_ok);
        args[0] = caps[0];
        begin = call_host_stdcall_args(RVA_IAT_TIME_BEGIN,
                                       args, 1U, &timer_ok);
        now = call_host_stdcall_args(RVA_IAT_TIME_GET,
                                     NULL, 0U, &timer_ok);
        args[0] = caps[0];
        end = call_host_stdcall_args(RVA_IAT_TIME_END,
                                     args, 1U, &timer_ok);
        timer_ok = timer_ok && previous == 0x80000000U &&
                   caps[0] == 1U && caps[1] == 1000U &&
                   begin == 0U && end == 0U && now != UINT32_MAX;
        printf("target power/timer policy   : %s, %u import calls\n",
               timer_ok ? "state/caps/begin/clock/end exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!timer_ok || g_host_import_calls - calls_before != 5U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t saved = 0U, observed = UINT32_MAX;
        uint32_t args[4];
        int foreground_ok = 1;

        args[0] = 0x2000U;      /* SPI_GETFOREGROUNDLOCKTIMEOUT */
        args[1] = 0U;
        args[2] = (uint32_t)(uintptr_t)&saved;
        args[3] = 0U;
        foreground_ok = foreground_ok &&
            call_host_stdcall_args(RVA_IAT_SYSTEM_PARAMETERS,
                                   args, 4U, &foreground_ok) == 1U &&
            saved == 200000U;

        args[0] = 0x2001U;      /* SPI_SETFOREGROUNDLOCKTIMEOUT */
        args[2] = 0U;
        args[3] = 2U;           /* SPIF_SENDCHANGE, target-side no-op */
        foreground_ok = foreground_ok &&
            call_host_stdcall_args(RVA_IAT_SYSTEM_PARAMETERS,
                                   args, 4U, &foreground_ok) == 1U;

        args[0] = 0x2000U;
        args[2] = (uint32_t)(uintptr_t)&observed;
        args[3] = 0U;
        foreground_ok = foreground_ok &&
            call_host_stdcall_args(RVA_IAT_SYSTEM_PARAMETERS,
                                   args, 4U, &foreground_ok) == 1U &&
            observed == 0U;

        args[0] = 0x2001U;
        args[2] = saved;
        args[3] = 2U;
        foreground_ok = foreground_ok &&
            call_host_stdcall_args(RVA_IAT_SYSTEM_PARAMETERS,
                                   args, 4U, &foreground_ok) == 1U;

        observed = 0U;
        args[0] = 0x2000U;
        args[2] = (uint32_t)(uintptr_t)&observed;
        args[3] = 0U;
        foreground_ok = foreground_ok &&
            call_host_stdcall_args(RVA_IAT_SYSTEM_PARAMETERS,
                                   args, 4U, &foreground_ok) == 1U &&
            observed == saved;

        printf("target foreground policy    : %s, %u import calls\n",
               foreground_ok ? "get/clear/restore local state exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!foreground_ok || g_host_import_calls - calls_before != 5U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t args[3];
        uint32_t module, icon, icon_again, result;
        int window_cleanup_ok = 1;

        args[0] = 0U;
        module = call_host_stdcall_args(RVA_IAT_MODULE_HANDLE_A,
                                        args, 1U, &window_cleanup_ok);
        args[0] = module;
        args[1] = 101U;                 /* MAKEINTRESOURCEA(101) */
        icon = call_host_stdcall_args(RVA_IAT_LOAD_ICON_A,
                                      args, 2U, &window_cleanup_ok);
        icon_again = call_host_stdcall_args(RVA_IAT_LOAD_ICON_A,
                                            args, 2U, &window_cleanup_ok);

        args[0] = 0U;                   /* failed GLFW path has HWND=NULL */
        args[1] = UINT32_C(0xFFFFFFF2); /* GCL_HICON = -14 */
        args[2] = icon;
        result = call_host_stdcall_args(RVA_IAT_SET_CLASS_LONG_A,
                                        args, 3U, &window_cleanup_ok);
        window_cleanup_ok = window_cleanup_ok && result == 0U;

        args[1] = UINT32_C(0xFFFFFFF0); /* GWL_STYLE = -16 */
        result = call_host_stdcall_args(RVA_IAT_GET_WINDOW_LONG_A,
                                        args, 2U, &window_cleanup_ok);
        window_cleanup_ok = window_cleanup_ok && result == 0U;
        args[2] = 0U;
        result = call_host_stdcall_args(RVA_IAT_SET_WINDOW_LONG_A,
                                        args, 3U, &window_cleanup_ok);
        window_cleanup_ok = window_cleanup_ok && result == 0U &&
                            module == GUEST_IMAGE_BASE && icon != 0U &&
                            icon_again == icon;

        printf("failed-window decoration     : %s, %u import calls\n",
               window_cleanup_ok
                   ? "module/icon stable; NULL HWND remains invalid"
                   : "FAILED",
               g_host_import_calls - calls_before);
        if (!window_cleanup_ok || g_host_import_calls - calls_before != 6U)
            fail++;
    }
    {
        unsigned calls_before = g_host_import_calls;
        uint32_t args[1], in_stream, out_stream, err_stream;
        int stdio_ok = 1;
        args[0] = 0U;
        in_stream = call_host_cdecl_args(RVA_IAT_ACRT_IOB,
                                         args, 1U, &stdio_ok);
        args[0] = 1U;
        out_stream = call_host_cdecl_args(RVA_IAT_ACRT_IOB,
                                          args, 1U, &stdio_ok);
        args[0] = 2U;
        err_stream = call_host_cdecl_args(RVA_IAT_ACRT_IOB,
                                          args, 1U, &stdio_ok);
        stdio_ok = stdio_ok &&
            in_stream == (uint32_t)(uintptr_t)stdin &&
            out_stream == (uint32_t)(uintptr_t)stdout &&
            err_stream == (uint32_t)(uintptr_t)stderr;
        printf("native standard streams     : %s, %u import calls\n",
               stdio_ok ? "opaque stdin/stdout/stderr exact" : "FAILED",
               g_host_import_calls - calls_before);
        if (!stdio_ok || g_host_import_calls - calls_before != 3U)
            fail++;
    }

    {
        static const char module_name[] = "OPENGL32";
        static const char get_string_name[] = "glGetString";
        static const char unknown_name[] = "glDefinitelyMissing";
        unsigned imports_before = g_host_import_calls;
        unsigned dynamics_before = g_host_dynamic_calls;
        uint32_t args[2], module, token, expected_token, missing;
        uint32_t version, shading, freed;
        uint32_t saved = cpu.esp;
        size_t surface_count = 0U;
        const guest_gl_symbol *surface;
        guest_gl_backend preserved_backend;
        int dynamic_ok = 1;

        args[0] = (uint32_t)(uintptr_t)module_name;
        module = call_host_stdcall_args(RVA_IAT_LOAD_LIBRARY_A,
                                        args, 1U, &dynamic_ok);
        args[0] = module;
        args[1] = (uint32_t)(uintptr_t)get_string_name;
        token = call_host_stdcall_args(RVA_IAT_GET_PROC_ADDRESS,
                                       args, 2U, &dynamic_ok);
        args[1] = (uint32_t)(uintptr_t)unknown_name;
        missing = call_host_stdcall_args(RVA_IAT_GET_PROC_ADDRESS,
                                         args, 2U, &dynamic_ok);

        expected_token = guest_gl_resolve(get_string_name);
        surface = guest_gl_symbols(&surface_count);
        memset(&preserved_backend, 0, sizeof preserved_backend);
        preserved_backend.glGetString = preserved_glGetString;
        guest_gl_install_backend(&preserved_backend);
        g_guest_gl_inventory_mode = 1;
        gpush(&cpu, 0x1F02U);
        gpush(&cpu, 0xABC0D001U);
        guest_call(&cpu, token);
        version = cpu.eax;
        dynamic_ok = dynamic_ok && cpu.esp == saved && version != 0U &&
                     strstr((const char *)(uintptr_t)version, "VitaGL") != NULL;
        cpu.esp = saved;
        gpush(&cpu, 0x8B8CU);
        gpush(&cpu, 0xABC0D002U);
        guest_call(&cpu, token);
        shading = cpu.eax;
        dynamic_ok = dynamic_ok && cpu.esp == saved && shading != 0U &&
                     strstr((const char *)(uintptr_t)shading, "1.00") != NULL;
        cpu.esp = saved;
        g_guest_gl_inventory_mode = 0;
        gpush(&cpu, 0x1F02U);
        gpush(&cpu, 0xABC0D003U);
        guest_call(&cpu, token);
        dynamic_ok = dynamic_ok && cpu.esp == saved && cpu.eax != 0U &&
                     strstr((const char *)(uintptr_t)cpu.eax,
                            "preserved installed backend") != NULL;
        cpu.esp = saved;
        guest_gl_install_backend(NULL);

        args[0] = module;
        freed = call_host_stdcall_args(RVA_IAT_FREE_LIBRARY,
                                       args, 1U, &dynamic_ok);
        dynamic_ok = dynamic_ok && freed == 1U &&
                     token == expected_token && token != 0U && missing == 0U &&
                     surface && surface_count == GUEST_GL_SURFACE_COUNT &&
                     guest_import_name(token) == NULL &&
                     guest_lookup(token) == NULL;

        printf("dynamic GL token boundary  : %s, %u symbols, %u imports / %u calls\n",
               dynamic_ok ? "typed registry; scoped inventory; backend preserved"
                           : "FAILED",
               (unsigned)surface_count,
               g_host_import_calls - imports_before,
               g_host_dynamic_calls - dynamics_before);
        if (!dynamic_ok || g_host_import_calls - imports_before != 4U ||
            g_host_dynamic_calls - dynamics_before != 3U)
            fail++;
    }

    {
        static const char module_name[] = "OPENGL32";
        static const char resolver_name[] = "wglGetProcAddress";
        static const char get_string_name[] = "glGetString";
        static const char unknown_name[] = "glDefinitelyMissing";
        static const char import_name[] =
            "OPENGL32.dll!wglGetProcAddress";
        unsigned imports_before = g_host_import_calls;
        unsigned dynamics_before = g_host_dynamic_calls;
        uint32_t args[2], token, missing, null_result, ordinal_result;
        uint32_t module, dynamic_resolver, dynamic_token, freed;
        uint32_t expected_token = guest_gl_resolve(get_string_name);
        uint32_t saved = cpu.esp;
        uint32_t import_token =
            ld32(GUEST_IMAGE_BASE + RVA_IAT_WGL_GET_PROC_ADDRESS);
        int wgl_ok = 1;

        args[0] = (uint32_t)(uintptr_t)get_string_name;
        token = call_host_stdcall_args(RVA_IAT_WGL_GET_PROC_ADDRESS,
                                       args, 1U, &wgl_ok);
        args[0] = (uint32_t)(uintptr_t)unknown_name;
        missing = call_host_stdcall_args(RVA_IAT_WGL_GET_PROC_ADDRESS,
                                         args, 1U, &wgl_ok);
        args[0] = 0U;
        null_result = call_host_stdcall_args(RVA_IAT_WGL_GET_PROC_ADDRESS,
                                             args, 1U, &wgl_ok);
        args[0] = 7U;                 /* ordinal-shaped MAKEINTRESOURCE */
        ordinal_result = call_host_stdcall_args(
            RVA_IAT_WGL_GET_PROC_ADDRESS, args, 1U, &wgl_ok);

        args[0] = (uint32_t)(uintptr_t)module_name;
        module = call_host_stdcall_args(
            RVA_IAT_LOAD_LIBRARY_A, args, 1U, &wgl_ok);
        args[0] = module;
        args[1] = (uint32_t)(uintptr_t)resolver_name;
        dynamic_resolver = call_host_stdcall_args(
            RVA_IAT_GET_PROC_ADDRESS, args, 2U, &wgl_ok);
        cpu.esp = saved;
        gpush(&cpu, (uint32_t)(uintptr_t)get_string_name);
        gpush(&cpu, 0xABC062FCU);
        guest_call(&cpu, dynamic_resolver);
        dynamic_token = cpu.eax;
        args[0] = module;
        freed = call_host_stdcall_args(
            RVA_IAT_FREE_LIBRARY, args, 1U, &wgl_ok);
        wgl_ok = wgl_ok && cpu.esp == saved && token == expected_token &&
                 token != 0U && missing == 0U && null_result == 0U &&
                 ordinal_result == 0U && module != 0U &&
                 dynamic_resolver != 0U && dynamic_token == expected_token &&
                 freed == 1U && guest_import_name(dynamic_resolver) == NULL &&
                 guest_lookup(dynamic_resolver) == NULL &&
                 guest_import_name(import_token) != NULL &&
                 strcmp(guest_import_name(import_token), import_name) == 0;

        printf("WGL guest token resolver   : %s, %u imports / %u calls\n",
               wgl_ok ? "direct + dynamic exact; invalid rejected; ESP exact"
                      : "FAILED",
               g_host_import_calls - imports_before,
               g_host_dynamic_calls - dynamics_before);
        if (!wgl_ok || g_host_import_calls - imports_before != 7U ||
            g_host_dynamic_calls - dynamics_before != 1U)
            fail++;
    }

    {
        static const char module_name[] = "DINPUT8.dll";
        static const char proc_name[] = "DirectInput8Create";
        static const uint32_t iid_direct_input_8a[4] = {
            0xBF798030U, 0x4DA2483AU, 0x645D99AAU, 0x009736EDU
        };
        unsigned imports_before = g_host_import_calls;
        unsigned dynamics_before = g_host_dynamic_calls;
        uint32_t args[2], module, token, output = 0xCCCCCCCCU, result, freed;
        uint32_t saved = cpu.esp;
        int dinput_ok = 1;

        args[0] = (uint32_t)(uintptr_t)module_name;
        module = call_host_stdcall_args(RVA_IAT_LOAD_LIBRARY_A,
                                        args, 1U, &dinput_ok);
        args[0] = module;
        args[1] = (uint32_t)(uintptr_t)proc_name;
        token = call_host_stdcall_args(RVA_IAT_GET_PROC_ADDRESS,
                                       args, 2U, &dinput_ok);

        cpu.esp = saved;
        gpush(&cpu, 0U); /* pUnkOuter */
        gpush(&cpu, (uint32_t)(uintptr_t)&output);
        gpush(&cpu, (uint32_t)(uintptr_t)iid_direct_input_8a);
        gpush(&cpu, 0x0800U);
        gpush(&cpu, GUEST_IMAGE_BASE);
        gpush(&cpu, 0xABC0D108U);
        guest_call(&cpu, token);
        result = cpu.eax;

        args[0] = module;
        freed = call_host_stdcall_args(RVA_IAT_FREE_LIBRARY,
                                       args, 1U, &dinput_ok);
        dinput_ok = dinput_ok && cpu.esp == saved && module != 0U &&
                    token != 0U && result == 0x80004005U && output == 0U &&
                    freed == 1U && guest_import_name(token) == NULL &&
                    guest_lookup(token) == NULL;
        printf("optional DirectInput boundary: %s, %u imports / %u calls\n",
               dinput_ok ? "typed E_FAIL + NULL interface; ESP exact"
                         : "FAILED",
               g_host_import_calls - imports_before,
               g_host_dynamic_calls - dynamics_before);
        if (!dinput_ok || g_host_import_calls - imports_before != 3U ||
            g_host_dynamic_calls - dynamics_before != 1U)
            fail++;
    }

    /* ---- the dispatch table resolves a guest address to real code ------- */
    {
        static const uint32_t probe[] = {
            RVA_NEXT, RVA_RANDINT, RVA_RANDFLOAT,
            RVA_PICK, RVA_GREED, RVA_CHAOS, RVA_I64_TO_DBL, RVA_U64_TO_DBL,
            RVA_F32_TO_U32, RVA_F64_TO_U32
        };
        int i, rva_ok = 0, va_ok = 0, same = 0;
        for (i = 0; i < (int)(sizeof probe / sizeof probe[0]); i++) {
            guest_fn by_rva = guest_lookup(probe[i]);
            guest_fn by_va = guest_lookup(GUEST_IMAGE_BASE + probe[i]);
            if (by_rva) rva_ok++;
            else printf("   RVA lookup FAILED for %08x\n", probe[i]);
            if (by_va) va_ok++;
            else printf("   VA lookup FAILED for %08x\n",
                        GUEST_IMAGE_BASE + probe[i]);
            if (by_rva && by_rva == by_va) same++;
        }
        printf("dispatch RVA/VA lookups     : %d/%d RVA, %d/%d VA, %d equal\n",
               rva_ok, (int)(sizeof probe / sizeof probe[0]),
               va_ok, (int)(sizeof probe / sizeof probe[0]), same);
        if (rva_ok != (int)(sizeof probe / sizeof probe[0]) ||
            va_ok != (int)(sizeof probe / sizeof probe[0]) ||
            same != (int)(sizeof probe / sizeof probe[0])) fail++;

        /* Exercise a pointer that the loader actually read and relocated,
         * rather than constructing another VA in the test. */
        {
            uint32_t slot_va = GUEST_IMAGE_BASE + RVA_RELOCATED_FN_PTR;
            uint32_t relocated = ld32(slot_va);
            guest_fn by_slot = guest_lookup(relocated);
            guest_fn expected = guest_lookup(RVA_RELOCATED_FN);
            printf("relocated .rdata pointer    : [%08x] = %08x, %s\n",
                   slot_va, relocated,
                   relocated == GUEST_IMAGE_BASE + RVA_RELOCATED_FN &&
                   by_slot && by_slot == expected ? "resolved" : "FAILED");
            if (relocated != GUEST_IMAGE_BASE + RVA_RELOCATED_FN ||
                !by_slot || by_slot != expected) fail++;
        }

        /* These real KAGE::Filesys::BufferedStream vtable entries exposed a
         * hole in function discovery: neither callable entry has CC padding
         * or a direct CALL.  Verify the loader relocation and the final
         * dispatch table together, so a stale metadata-only fix cannot pass. */
        {
            static const uint32_t slots[] = {
                RVA_BUFFEREDSTREAM_SLOT0,
                RVA_BUFFEREDSTREAM_SLOT1,
                RVA_BUFFEREDSTREAM_SLOT2,
            };
            static const uint32_t targets[] = {
                RVA_BUFFEREDSTREAM_ZERO,
                RVA_BUFFEREDSTREAM_ZERO,
                RVA_BUFFEREDSTREAM_DTOR,
            };
            int i, resolved = 0;
            for (i = 0; i < 3; ++i) {
                uint32_t relocated = ld32(GUEST_IMAGE_BASE + slots[i]);
                guest_fn by_slot = guest_lookup(relocated);
                guest_fn expected = guest_lookup(targets[i]);
                if (relocated == GUEST_IMAGE_BASE + targets[i] &&
                    by_slot && by_slot == expected)
                    resolved++;
            }
            printf("relocated vtable-only roots : %d/3 exact dispatch entries\n",
                   resolved);
            if (resolved != 3) fail++;
        }

        /* An address that is NOT a function start must resolve to nothing,
         * or the table is answering questions it was not asked. */
        if (guest_lookup(RVA_NEXT + 3) ||
            guest_lookup(GUEST_IMAGE_BASE + RVA_NEXT + 3)) {
            printf("   lookup of a mid-function RVA/VA returned a function"
                   " -- dispatch is too permissive\n");
            fail++;
        } else {
            printf("negative RVA/VA lookup      : mid-function addresses"
                   " correctly unresolved\n");
        }
    }

    /* ---- rung 1 through the full build --------------------------------- */
    for (idx = 0; idx < 81; idx++) {
        RNG got, ref;
        got.seed = ref.seed = 1u;
        got.s1 = ref.s1 = shifts[idx * 3 + 0];
        got.s2 = ref.s2 = shifts[idx * 3 + 1];
        got.s3 = ref.s3 = shifts[idx * 3 + 2];
        for (k = 0; k < 8; k++) {
            uint32_t r;
            call_this(sub_003a7e20, (uint32_t)(uintptr_t)&got, NULL, 0);
            r = ref_next(&ref);
            checks++;
            if (got.seed != r) {
                if (rng_mismatches < 5)
                    printf("   MISMATCH idx=%d k=%d  got %08X want %08X\n",
                           idx, k, got.seed, r);
                rng_mismatches++;
                fail++;
            }
            if (cpu.fault) {
                printf("   FAULT at %08x: %s\n", cpu.fault_addr, cpu.fault);
                fail++;
                cpu.fault = 0;
                break;
            }
        }
    }
    printf("rung 1 RNG::Next            : %d checks, %d mismatches\n",
           checks, rng_mismatches);

    /* ---- a 200,000-draw stream, hashed --------------------------------- */
    {
        RNG got, ref;
        got.seed = ref.seed = 1u;
        got.s1 = ref.s1 = shifts[0];
        got.s2 = ref.s2 = shifts[1];
        got.s3 = ref.s3 = shifts[2];
        h_ref = h_got = 2166136261u;
        for (k = 0; k < 200000; k++) {
            call_this(sub_003a7e20, (uint32_t)(uintptr_t)&got, NULL, 0);
            ref_next(&ref);
            h_got = fnv(h_got, got.seed);
            h_ref = fnv(h_ref, ref.seed);
        }
        checks += 200000;
        printf("200,000-draw stream         : ref %08X  recompiled %08X  %s\n",
               h_ref, h_got, h_ref == h_got ? "equal" : "DIFFERENT");
        if (h_ref != h_got) fail++;
    }

    if (cpu.fault) {
        printf("\nFAULT still set at %08x: %s\n", cpu.fault_addr, cpu.fault);
        fail++;
    }

    {
        uint32_t fs = guest_fs_base(&cpu);
        uint32_t tls_array = fs ? ld32(fs + 0x2CU) : 0U;
        uint32_t tls_index = ld32(VA_TLS_INDEX);
        guest_image_free();
        printf("static TLS cleanup          : %s\n",
               tls_array && tls_index < 64U &&
               ld32(tls_array + tls_index * 4U) == 0U
               ? "TIB slot cleared" : "FAILED");
        if (!tls_array || tls_index >= 64U ||
            ld32(tls_array + tls_index * 4U) != 0U)
            fail++;
    }

    printf("\nTOTAL                       : %d checks, %d failures\n",
           checks, fail);
    printf("VERDICT                     : %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
