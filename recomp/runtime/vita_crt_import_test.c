/* Compile/link/static softfp ARM oracle for host_vita_crt.c.
 *
 * It executes the production handlers, not copies: exact measured order and
 * evidence hash, cdecl/stdcall cleanup, writable host-owned CRT storage,
 * initializer short-circuit/fault propagation, table bounds, and the
 * mutation-free unknown-name handoff are encoded here; execution acceptance
 * belongs to Vita3K or hardware, not arm-vita-eabi-run. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "host_vita_console.h"
#include "host_vita_crt.h"
#include "host_vita_fls.h"
#include "host_vita_gl.h"
#include "host_vita_memory.h"
#include "host_vita_startup.h"
#include "host_vita_sync.h"
#include "platform.h"
#include "vita_boot_imports.h"
#include "vita_host_services.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita CRT import oracle requires a 32-bit identity-mapped host
#endif

enum {
    CALLBACK_ZERO = 0x10001000U,
    CALLBACK_NONZERO = 0x10001004U,
    CALLBACK_MUST_NOT_RUN = 0x10001008U,
    CALLBACK_NESTED_FAULT = 0x1000100cU,
    CALLBACK_BAD_STACK = 0x10001010U,
    CALLBACK_BOOT_CHAIN = 0x10001014U,
    CALLBACK_EXIT_A = 0x10001018U,
    CALLBACK_EXIT_B = 0x1000101cU,
    CALLBACK_EXIT_TLS = 0x10001020U
};

typedef struct import_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t call_rva;
    uint32_t return_rva;
} import_evidence;

static const import_evidence s_evidence[] = {
    { ISAAC_VITA_CRT_INITTERM_E_NAME,
      ISAAC_VITA_CRT_INITTERM_E_IAT_RVA,
      ISAAC_VITA_CRT_INITTERM_E_CALL_RVA,
      ISAAC_VITA_CRT_INITTERM_E_RETURN_RVA },
    { ISAAC_VITA_CRT_SET_APP_TYPE_NAME,
      ISAAC_VITA_CRT_SET_APP_TYPE_IAT_RVA,
      ISAAC_VITA_CRT_SET_APP_TYPE_CALL_RVA,
      ISAAC_VITA_CRT_SET_APP_TYPE_RETURN_RVA },
    { ISAAC_VITA_CRT_SET_FMODE_NAME,
      ISAAC_VITA_CRT_SET_FMODE_IAT_RVA,
      ISAAC_VITA_CRT_SET_FMODE_CALL_RVA,
      ISAAC_VITA_CRT_SET_FMODE_RETURN_RVA },
    { ISAAC_VITA_CRT_P_COMMODE_NAME,
      ISAAC_VITA_CRT_P_COMMODE_IAT_RVA,
      ISAAC_VITA_CRT_P_COMMODE_CALL_RVA,
      ISAAC_VITA_CRT_P_COMMODE_RETURN_RVA },
    { ISAAC_VITA_CRT_ATEXIT_NAME,
      ISAAC_VITA_CRT_ATEXIT_IAT_RVA,
      ISAAC_VITA_CRT_ATEXIT_CALL_RVA,
      ISAAC_VITA_CRT_ATEXIT_RETURN_RVA },
    { ISAAC_VITA_CRT_CONFIGURE_ARGV_NAME,
      ISAAC_VITA_CRT_CONFIGURE_ARGV_IAT_RVA,
      ISAAC_VITA_CRT_CONFIGURE_ARGV_CALL_RVA,
      ISAAC_VITA_CRT_CONFIGURE_ARGV_RETURN_RVA },
    { ISAAC_VITA_CRT_INITIALIZE_SLIST_NAME,
      ISAAC_VITA_CRT_INITIALIZE_SLIST_IAT_RVA,
      ISAAC_VITA_CRT_INITIALIZE_SLIST_CALL_RVA,
      ISAAC_VITA_CRT_INITIALIZE_SLIST_RETURN_RVA },
    { ISAAC_VITA_CRT_CONTROLFP_S_NAME,
      ISAAC_VITA_CRT_CONTROLFP_S_IAT_RVA,
      ISAAC_VITA_CRT_CONTROLFP_S_CALL_RVA,
      ISAAC_VITA_CRT_CONTROLFP_S_RETURN_RVA },
    { ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME,
      ISAAC_VITA_CRT_CONFIGTHREADLOCALE_IAT_RVA,
      ISAAC_VITA_CRT_CONFIGTHREADLOCALE_CALL_RVA,
      ISAAC_VITA_CRT_CONFIGTHREADLOCALE_RETURN_RVA },
    { ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_NAME,
      ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_IAT_RVA,
      ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_CALL_RVA,
      ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_RETURN_RVA },
    { ISAAC_VITA_CRT_INITTERM_NAME,
      ISAAC_VITA_CRT_INITTERM_IAT_RVA,
      ISAAC_VITA_CRT_INITTERM_CALL_RVA,
      ISAAC_VITA_CRT_INITTERM_RETURN_RVA },
    { ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_NAME,
      ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_IAT_RVA,
      ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_CALL_RVA,
      ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_RETURN_RVA },
    { ISAAC_VITA_CRT_P_ARGV_NAME,
      ISAAC_VITA_CRT_P_ARGV_IAT_RVA,
      ISAAC_VITA_CRT_P_ARGV_CALL_RVA,
      ISAAC_VITA_CRT_P_ARGV_RETURN_RVA },
    { ISAAC_VITA_CRT_P_ARGC_NAME,
      ISAAC_VITA_CRT_P_ARGC_IAT_RVA,
      ISAAC_VITA_CRT_P_ARGC_CALL_RVA,
      ISAAC_VITA_CRT_P_ARGC_RETURN_RVA },
    { ISAAC_VITA_CRT_GETENV_NAME,
      ISAAC_VITA_CRT_GETENV_IAT_RVA,
      ISAAC_VITA_CRT_GETENV_USERPROFILE_CALL_RVA,
      ISAAC_VITA_CRT_GETENV_USERPROFILE_RETURN_RVA },
    { ISAAC_VITA_CRT_VSPRINTF_NAME,
      ISAAC_VITA_CRT_VSPRINTF_IAT_RVA,
      ISAAC_VITA_CRT_VSPRINTF_IMPORT_CALL_RVA,
      ISAAC_VITA_CRT_VSPRINTF_IMPORT_RETURN_RVA },
    { ISAAC_VITA_CRT_FOPEN_NAME,
      ISAAC_VITA_CRT_FOPEN_IAT_RVA,
      ISAAC_VITA_CRT_FOPEN_SAVEPATH_CALL_RVA,
      ISAAC_VITA_CRT_FOPEN_SAVEPATH_RETURN_RVA },
    { ISAAC_VITA_CRT_VFPRINTF_NAME,
      ISAAC_VITA_CRT_VFPRINTF_IAT_RVA,
      ISAAC_VITA_CRT_VFPRINTF_IMPORT_CALL_RVA,
      ISAAC_VITA_CRT_VFPRINTF_IMPORT_RETURN_RVA },
    { ISAAC_VITA_CRT_FCLOSE_NAME,
      ISAAC_VITA_CRT_FCLOSE_IAT_RVA,
      ISAAC_VITA_CRT_FCLOSE_CALL_RVA,
      ISAAC_VITA_CRT_FCLOSE_RETURN_RVA },
    { ISAAC_VITA_CRT_STRNCPY_NAME,
      ISAAC_VITA_CRT_STRNCPY_IAT_RVA,
      ISAAC_VITA_CRT_STRNCPY_CALL_RVA,
      ISAAC_VITA_CRT_STRNCPY_RETURN_RVA },
    { ISAAC_VITA_CRT_STRNCPY_S_NAME,
      ISAAC_VITA_CRT_STRNCPY_S_IAT_RVA,
      ISAAC_VITA_CRT_STRNCPY_S_CALL_RVA,
      ISAAC_VITA_CRT_STRNCPY_S_RETURN_RVA },
    { ISAAC_VITA_CRT_STRDUP_NAME,
      ISAAC_VITA_CRT_STRDUP_IAT_RVA,
      ISAAC_VITA_CRT_STRDUP_CALL_RVA,
      ISAAC_VITA_CRT_STRDUP_RETURN_RVA },
    { ISAAC_VITA_CRT_MBSTOWCS_S_NAME,
      ISAAC_VITA_CRT_MBSTOWCS_S_IAT_RVA,
      ISAAC_VITA_CRT_MBSTOWCS_S_CALL_RVA,
      ISAAC_VITA_CRT_MBSTOWCS_S_RETURN_RVA },
    { ISAAC_VITA_CRT_WCSTOMBS_S_NAME,
      ISAAC_VITA_CRT_WCSTOMBS_S_IAT_RVA,
      ISAAC_VITA_CRT_WCSTOMBS_S_CALL_RVA,
      ISAAC_VITA_CRT_WCSTOMBS_S_RETURN_RVA },
    { ISAAC_VITA_CRT_VSPRINTF_S_NAME,
      ISAAC_VITA_CRT_VSPRINTF_S_IAT_RVA,
      ISAAC_VITA_CRT_VSPRINTF_S_IMPORT_CALL_RVA,
      ISAAC_VITA_CRT_VSPRINTF_S_IMPORT_RETURN_RVA },
    { ISAAC_VITA_CRT_STRNCMP_NAME,
      ISAAC_VITA_CRT_STRNCMP_IAT_RVA,
      ISAAC_VITA_CRT_STRNCMP_CALL_RVA,
      ISAAC_VITA_CRT_STRNCMP_RETURN_RVA },
    { ISAAC_VITA_CRT_STRPBRK_NAME,
      ISAAC_VITA_CRT_STRPBRK_IAT_RVA,
      ISAAC_VITA_CRT_STRPBRK_CALL_RVA,
      ISAAC_VITA_CRT_STRPBRK_RETURN_RVA },
    { ISAAC_VITA_CRT_ISPUNCT_NAME,
      ISAAC_VITA_CRT_ISPUNCT_IAT_RVA,
      ISAAC_VITA_CRT_ISPUNCT_CALL_RVA,
      ISAAC_VITA_CRT_ISPUNCT_RETURN_RVA },
    { ISAAC_VITA_CRT_TOLOWER_NAME,
      ISAAC_VITA_CRT_TOLOWER_IAT_RVA,
      ISAAC_VITA_CRT_TOLOWER_CALL_RVA,
      ISAAC_VITA_CRT_TOLOWER_RETURN_RVA },
    { ISAAC_VITA_CRT_STRNICMP_NAME,
      ISAAC_VITA_CRT_STRNICMP_IAT_RVA,
      ISAAC_VITA_CRT_STRNICMP_CALL_RVA,
      ISAAC_VITA_CRT_STRNICMP_RETURN_RVA },
    { ISAAC_VITA_CRT_ISDIGIT_NAME,
      ISAAC_VITA_CRT_ISDIGIT_IAT_RVA,
      ISAAC_VITA_CRT_ISDIGIT_CALL_RVA,
      ISAAC_VITA_CRT_ISDIGIT_RETURN_RVA },
    { ISAAC_VITA_CRT_ISWSPACE_NAME,
      ISAAC_VITA_CRT_ISWSPACE_IAT_RVA,
      ISAAC_VITA_CRT_ISWSPACE_CALL_RVA,
      ISAAC_VITA_CRT_ISWSPACE_RETURN_RVA },
    { ISAAC_VITA_CRT_TOUPPER_NAME,
      ISAAC_VITA_CRT_TOUPPER_IAT_RVA,
      ISAAC_VITA_CRT_TOUPPER_CALL_RVA,
      ISAAC_VITA_CRT_TOUPPER_RETURN_RVA },
    { ISAAC_VITA_CRT_STRCAT_S_NAME,
      ISAAC_VITA_CRT_STRCAT_S_IAT_RVA,
      ISAAC_VITA_CRT_STRCAT_S_CALL_RVA,
      ISAAC_VITA_CRT_STRCAT_S_RETURN_RVA },
    { ISAAC_VITA_CRT_STRCPY_S_NAME,
      ISAAC_VITA_CRT_STRCPY_S_IAT_RVA,
      ISAAC_VITA_CRT_STRCPY_S_CALL_RVA,
      ISAAC_VITA_CRT_STRCPY_S_RETURN_RVA },
    { ISAAC_VITA_CRT_ISSPACE_NAME,
      ISAAC_VITA_CRT_ISSPACE_IAT_RVA,
      ISAAC_VITA_CRT_ISSPACE_CALL_RVA,
      ISAAC_VITA_CRT_ISSPACE_RETURN_RVA },
    { ISAAC_VITA_CRT_VSSCANF_NAME,
      ISAAC_VITA_CRT_VSSCANF_IAT_RVA,
      ISAAC_VITA_CRT_VSSCANF_IMPORT_CALL_RVA,
      ISAAC_VITA_CRT_VSSCANF_IMPORT_RETURN_RVA },
    { ISAAC_VITA_CRT_ERRNO_NAME,
      ISAAC_VITA_CRT_ERRNO_IAT_RVA,
      ISAAC_VITA_CRT_ERRNO_CALL1_RVA,
      ISAAC_VITA_CRT_ERRNO_RETURN1_RVA },
    { ISAAC_VITA_CRT_SET_ERRNO_NAME,
      ISAAC_VITA_CRT_SET_ERRNO_IAT_RVA,
      ISAAC_VITA_CRT_SET_ERRNO_CALL1_RVA,
      ISAAC_VITA_CRT_SET_ERRNO_RETURN1_RVA },
    { ISAAC_VITA_CRT_STRFTIME_NAME,
      ISAAC_VITA_CRT_STRFTIME_IAT_RVA,
      ISAAC_VITA_CRT_STRFTIME_CALL_RVA,
      ISAAC_VITA_CRT_STRFTIME_RETURN_RVA },
    { ISAAC_VITA_CRT_GMTIME64_NAME,
      ISAAC_VITA_CRT_GMTIME64_IAT_RVA,
      ISAAC_VITA_CRT_GMTIME64_CALL1_RVA,
      ISAAC_VITA_CRT_GMTIME64_RETURN1_RVA },
    { ISAAC_VITA_CRT_TIME64_NAME,
      ISAAC_VITA_CRT_TIME64_IAT_RVA,
      ISAAC_VITA_CRT_TIME64_FRONTIER_CALL_RVA,
      ISAAC_VITA_CRT_TIME64_FRONTIER_RETURN_RVA },
    { ISAAC_VITA_CRT_MKGMTIME64_NAME,
      ISAAC_VITA_CRT_MKGMTIME64_IAT_RVA,
      ISAAC_VITA_CRT_MKGMTIME64_CALL1_RVA,
      ISAAC_VITA_CRT_MKGMTIME64_RETURN1_RVA },
    { ISAAC_VITA_CRT_LOCALTIME64_NAME,
      ISAAC_VITA_CRT_LOCALTIME64_IAT_RVA,
      ISAAC_VITA_CRT_LOCALTIME64_CALL_RVA,
      ISAAC_VITA_CRT_LOCALTIME64_RETURN_RVA },
    { ISAAC_VITA_CRT_FILENO_NAME,
      ISAAC_VITA_CRT_FILENO_IAT_RVA,
      ISAAC_VITA_CRT_FILENO_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_FILENO_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_FREAD_NAME,
      ISAAC_VITA_CRT_FREAD_IAT_RVA,
      ISAAC_VITA_CRT_FREAD_CALL_RVA,
      ISAAC_VITA_CRT_FREAD_RETURN_RVA },
    { ISAAC_VITA_CRT_FWRITE_NAME,
      ISAAC_VITA_CRT_FWRITE_IAT_RVA,
      ISAAC_VITA_CRT_FWRITE_CALL_RVA,
      ISAAC_VITA_CRT_FWRITE_RETURN_RVA },
    { ISAAC_VITA_CRT_FSEEK_NAME,
      ISAAC_VITA_CRT_FSEEK_IAT_RVA,
      ISAAC_VITA_CRT_FSEEK_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_FSEEK_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,
      ISAAC_VITA_CRT_GET_OSFHANDLE_IAT_RVA,
      ISAAC_VITA_CRT_GET_OSFHANDLE_CALL_RVA,
      ISAAC_VITA_CRT_GET_OSFHANDLE_RETURN_RVA },
    { ISAAC_VITA_CRT_FTELL_NAME,
      ISAAC_VITA_CRT_FTELL_IAT_RVA,
      ISAAC_VITA_CRT_FTELL_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_FTELL_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_FFLUSH_NAME,
      ISAAC_VITA_CRT_FFLUSH_IAT_RVA,
      ISAAC_VITA_CRT_FFLUSH_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_ACRT_IOB_NAME,
      ISAAC_VITA_CRT_ACRT_IOB_IAT_RVA,
      ISAAC_VITA_CRT_ACRT_IOB_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_ACRT_IOB_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_STRTOULL_NAME,
      ISAAC_VITA_CRT_STRTOULL_IAT_RVA,
      ISAAC_VITA_CRT_STRTOULL_CALL1_RVA,
      ISAAC_VITA_CRT_STRTOULL_RETURN1_RVA },
    { ISAAC_VITA_CRT_STRTOL_NAME,
      ISAAC_VITA_CRT_STRTOL_IAT_RVA,
      ISAAC_VITA_CRT_STRTOL_CALL_RVA,
      ISAAC_VITA_CRT_STRTOL_RETURN_RVA },
    { ISAAC_VITA_CRT_ATOI_NAME,
      ISAAC_VITA_CRT_ATOI_IAT_RVA,
      ISAAC_VITA_CRT_ATOI_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_ATOI_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_ATOF_NAME,
      ISAAC_VITA_CRT_ATOF_IAT_RVA,
      ISAAC_VITA_CRT_ATOF_FIRST_CALL_RVA,
      ISAAC_VITA_CRT_ATOF_FIRST_RETURN_RVA },
    { ISAAC_VITA_CRT_QSORT_NAME,
      ISAAC_VITA_CRT_QSORT_IAT_RVA,
      ISAAC_VITA_CRT_QSORT_LIVE_CALL_RVA,
      ISAAC_VITA_CRT_QSORT_LIVE_RETURN_RVA },
    { ISAAC_VITA_CRT_TLS_EXIT_REGISTER_NAME,
      ISAAC_VITA_CRT_TLS_EXIT_REGISTER_IAT_RVA,
      ISAAC_VITA_CRT_TLS_EXIT_REGISTER_CALL_RVA,
      ISAAC_VITA_CRT_TLS_EXIT_REGISTER_RETURN_RVA },
    { ISAAC_VITA_CRT__EXIT_NAME,
      ISAAC_VITA_CRT__EXIT_IAT_RVA,
      ISAAC_VITA_CRT__EXIT_CALL_RVA,
      ISAAC_VITA_CRT__EXIT_RETURN_RVA },
    { ISAAC_VITA_CRT_EXIT_NAME,
      ISAAC_VITA_CRT_EXIT_IAT_RVA,
      ISAAC_VITA_CRT_EXIT_CALL_RVA,
      ISAAC_VITA_CRT_EXIT_RETURN_RVA }
};

typedef struct import_call_site {
    uint32_t call_rva;
    uint32_t return_rva;
} import_call_site;

#define ISAAC_CALL_SITE(call_rva, return_rva) { call_rva, return_rva },
static const import_call_site s_fileno_sites[] = {
    ISAAC_VITA_CRT_FILENO_DIRECT_CALL_SITES(ISAAC_CALL_SITE)
    ISAAC_VITA_CRT_FILENO_REGISTER_CALL_SITES(ISAAC_CALL_SITE)
};
static const import_call_site s_fread_sites[] = {
    { ISAAC_VITA_CRT_FREAD_CALL_RVA, ISAAC_VITA_CRT_FREAD_RETURN_RVA }
};
static const import_call_site s_fwrite_sites[] = {
    { ISAAC_VITA_CRT_FWRITE_CALL_RVA, ISAAC_VITA_CRT_FWRITE_RETURN_RVA }
};
static const import_call_site s_fseek_sites[] = {
    ISAAC_VITA_CRT_FSEEK_REGISTER_CALL_SITES(ISAAC_CALL_SITE)
    ISAAC_VITA_CRT_FSEEK_DIRECT_CALL_SITES(ISAAC_CALL_SITE)
};
static const import_call_site s_get_osfhandle_sites[] = {
    { ISAAC_VITA_CRT_GET_OSFHANDLE_CALL_RVA,
      ISAAC_VITA_CRT_GET_OSFHANDLE_RETURN_RVA }
};
static const import_call_site s_ftell_sites[] = {
    ISAAC_VITA_CRT_FTELL_REGISTER_CALL_SITES(ISAAC_CALL_SITE)
    ISAAC_VITA_CRT_FTELL_DIRECT_CALL_SITES(ISAAC_CALL_SITE)
};
static const import_call_site s_fflush_sites[] = {
    ISAAC_VITA_CRT_FFLUSH_CALL_SITES(ISAAC_CALL_SITE)
};
static const import_call_site s_acrt_iob_sites[] = {
    ISAAC_VITA_CRT_ACRT_IOB_DIRECT_CALL_SITES(ISAAC_CALL_SITE)
    ISAAC_VITA_CRT_ACRT_IOB_REGISTER_CALL_SITES(ISAAC_CALL_SITE)
};
static const import_call_site s_exit_sites[] = {
    ISAAC_VITA_CRT_EXIT_CALL_SITES(ISAAC_CALL_SITE)
};

typedef struct qsort_call_site {
    uint32_t owner_rva;
    uint32_t call_rva;
    uint32_t return_rva;
    uint32_t comparator_push_rva;
    uint32_t comparator_rva;
    uint32_t width;
} qsort_call_site;

#define ISAAC_QSORT_SITE(owner, call, ret, push, comparator, width) \
    { owner, call, ret, push, comparator, width },
static const qsort_call_site s_qsort_sites[] = {
    ISAAC_VITA_CRT_QSORT_CALL_SITES(ISAAC_QSORT_SITE)
};
#undef ISAAC_QSORT_SITE
#undef ISAAC_CALL_SITE

#define ISAAC_LOAD_SITE(load_rva) load_rva,
static const uint32_t s_fileno_load_sites[] = {
    ISAAC_VITA_CRT_FILENO_REGISTER_LOAD_SITES(ISAAC_LOAD_SITE)
};
static const uint32_t s_fseek_load_sites[] = {
    ISAAC_VITA_CRT_FSEEK_REGISTER_LOAD_SITES(ISAAC_LOAD_SITE)
};
static const uint32_t s_ftell_load_sites[] = {
    ISAAC_VITA_CRT_FTELL_REGISTER_LOAD_SITES(ISAAC_LOAD_SITE)
};
static const uint32_t s_acrt_iob_load_sites[] = {
    ISAAC_VITA_CRT_ACRT_IOB_REGISTER_LOAD_SITES(ISAAC_LOAD_SITE)
};
#undef ISAAC_LOAD_SITE

_Static_assert(sizeof s_evidence / sizeof s_evidence[0] ==
               ISAAC_VITA_CRT_IMPORT_COUNT,
               "CRT evidence order lost an import");
_Static_assert(sizeof s_exit_sites / sizeof s_exit_sites[0] ==
                   ISAAC_VITA_CRT_EXIT_PHYSICAL_CALL_COUNT,
               "frozen PE exit physical-call census drifted");
_Static_assert(sizeof s_qsort_sites / sizeof s_qsort_sites[0] ==
                   ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT &&
               ISAAC_VITA_CRT_QSORT_DIRECT_CALL_COUNT ==
                   ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT &&
               ISAAC_VITA_CRT_QSORT_REGISTER_LOAD_COUNT == 0U &&
               ISAAC_VITA_CRT_QSORT_REGISTER_CALL_COUNT == 0U &&
               ISAAC_VITA_CRT_QSORT_COMPARATOR_COUNT == 6U,
               "frozen PE qsort physical-call census drifted");
_Static_assert(sizeof s_fileno_sites / sizeof s_fileno_sites[0] ==
                   ISAAC_VITA_CRT_FILENO_CALL_COUNT &&
               sizeof s_fread_sites / sizeof s_fread_sites[0] ==
                   ISAAC_VITA_CRT_FREAD_CALL_COUNT &&
               sizeof s_fwrite_sites / sizeof s_fwrite_sites[0] ==
                   ISAAC_VITA_CRT_FWRITE_CALL_COUNT &&
               sizeof s_fseek_sites / sizeof s_fseek_sites[0] ==
                   ISAAC_VITA_CRT_FSEEK_CALL_COUNT &&
               sizeof s_get_osfhandle_sites /
                   sizeof s_get_osfhandle_sites[0] ==
                   ISAAC_VITA_CRT_GET_OSFHANDLE_CALL_COUNT &&
               sizeof s_ftell_sites / sizeof s_ftell_sites[0] ==
                   ISAAC_VITA_CRT_FTELL_CALL_COUNT &&
               sizeof s_fflush_sites / sizeof s_fflush_sites[0] ==
                   ISAAC_VITA_CRT_FFLUSH_CALL_COUNT &&
               sizeof s_acrt_iob_sites / sizeof s_acrt_iob_sites[0] ==
                   ISAAC_VITA_CRT_ACRT_IOB_CALL_COUNT,
               "frozen PE stdio physical call census drifted");
_Static_assert(sizeof s_fileno_load_sites /
                   sizeof s_fileno_load_sites[0] == 4U &&
               sizeof s_fseek_load_sites / sizeof s_fseek_load_sites[0] ==
                   1U &&
               sizeof s_ftell_load_sites / sizeof s_ftell_load_sites[0] ==
                   1U &&
               sizeof s_acrt_iob_load_sites /
                   sizeof s_acrt_iob_load_sites[0] == 10U,
               "frozen PE stdio register-load census drifted");
_Static_assert(ISAAC_VITA_CRT_STRING_IMPORT_COUNT == 14U &&
               ISAAC_VITA_CRT_STRPBRK_IAT_RVA ==
                   ISAAC_VITA_CRT_STRDUP_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_ISPUNCT_IAT_RVA ==
                   ISAAC_VITA_CRT_STRPBRK_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_TOLOWER_IAT_RVA ==
                   ISAAC_VITA_CRT_ISPUNCT_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_STRNICMP_IAT_RVA ==
                   ISAAC_VITA_CRT_TOLOWER_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_STRNCPY_IAT_RVA ==
                   ISAAC_VITA_CRT_STRNICMP_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_ISDIGIT_IAT_RVA ==
                   ISAAC_VITA_CRT_STRNCPY_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_STRNCMP_IAT_RVA ==
                   ISAAC_VITA_CRT_ISDIGIT_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_ISWSPACE_IAT_RVA ==
                   ISAAC_VITA_CRT_STRNCMP_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_STRNCPY_S_IAT_RVA ==
                   ISAAC_VITA_CRT_ISWSPACE_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_TOUPPER_IAT_RVA ==
                   ISAAC_VITA_CRT_STRNCPY_S_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_STRCAT_S_IAT_RVA ==
                   ISAAC_VITA_CRT_TOUPPER_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_STRCPY_S_IAT_RVA ==
                   ISAAC_VITA_CRT_STRCAT_S_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_ISSPACE_IAT_RVA ==
                   ISAAC_VITA_CRT_STRCPY_S_IAT_RVA + 4U,
               "frozen PE narrow-string IAT family is not contiguous");
_Static_assert(ISAAC_VITA_CRT_SCAN_IMPORT_COUNT == 1U &&
               ISAAC_VITA_CRT_VSSCANF_STATIC_CALL_COUNT == 4U &&
               ISAAC_VITA_CRT_VSSCANF_STATIC_FORMAT_COUNT == 3U &&
               ISAAC_VITA_CRT_VSSCANF_IAT_RVA == 0x00606610U &&
               ISAAC_VITA_CRT_VSSCANF_IAT_VA == 0x98606610U &&
               ISAAC_VITA_CRT_VSSCANF_IMPORT_CALL_RVA == 0x003f0f09U &&
               ISAAC_VITA_CRT_VSSCANF_IMPORT_RETURN_RVA == 0x003f0f0fU,
               "frozen PE scanf-family evidence drifted");
_Static_assert(ISAAC_VITA_CRT_ERRNO_FAMILY_IMPORT_COUNT == 2U &&
               ISAAC_VITA_CRT_DOSERRNO_IMPORT_COUNT == 0U &&
               ISAAC_VITA_CRT_ERRNO_FAMILY_DIRECT_CALL_COUNT == 7U &&
               EINVAL == ISAAC_VITA_CRT_ERRNO_CALL1_STORE_VALUE &&
               ENOMEM == ISAAC_VITA_CRT_ERRNO_CALL2_STORE_VALUE &&
               ENOENT == ISAAC_VITA_CRT_SET_ERRNO_ENOENT_VALUE &&
               EBADF == ISAAC_VITA_CRT_SET_ERRNO_EBADF_VALUE,
               "frozen PE errno/doserrno import census drifted");
_Static_assert(ISAAC_VITA_CRT_TIME_IMPORT_COUNT == 5U &&
               ISAAC_VITA_CRT_TIME_CALL_COUNT == 22U &&
               ISAAC_VITA_CRT_TM_DWORD_COUNT == 9U &&
               ISAAC_VITA_CRT_STRFTIME_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_GMTIME64_IAT_RVA &&
               ISAAC_VITA_CRT_GMTIME64_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_TIME64_IAT_RVA &&
               ISAAC_VITA_CRT_TIME64_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_MKGMTIME64_IAT_RVA &&
               ISAAC_VITA_CRT_MKGMTIME64_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_LOCALTIME64_IAT_RVA &&
               ISAAC_VITA_CRT_TIME64_NULL_CALL_COUNT == 3U &&
               ISAAC_VITA_CRT_TIME64_FRONTIER_BEFORE_COUNT == 2774U &&
               ISAAC_VITA_CRT_TIME64_FRONTIER_ORDINAL == 2775U,
               "frozen PE time-family evidence drifted");
_Static_assert(ISAAC_VITA_CRT_STDIO_IMPORT_COUNT == 16U &&
               ISAAC_VITA_CRT_FILE_IO_IMPORT_COUNT == 8U &&
               ISAAC_VITA_CRT_FILE_IO_CALL_COUNT == 92U &&
               ISAAC_VITA_CRT_FILENO_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FREAD_IAT_RVA &&
               ISAAC_VITA_CRT_FREAD_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FWRITE_IAT_RVA &&
               ISAAC_VITA_CRT_FWRITE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FSEEK_IAT_RVA &&
               ISAAC_VITA_CRT_FSEEK_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_GET_OSFHANDLE_IAT_RVA &&
               ISAAC_VITA_CRT_GET_OSFHANDLE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FTELL_IAT_RVA &&
               ISAAC_VITA_CRT_FTELL_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_P_COMMODE_IAT_RVA &&
               ISAAC_VITA_CRT_P_COMMODE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VFPRINTF_IAT_RVA &&
               ISAAC_VITA_CRT_VFPRINTF_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FFLUSH_IAT_RVA &&
               ISAAC_VITA_CRT_FFLUSH_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_ACRT_IOB_IAT_RVA &&
               ISAAC_VITA_CRT_ACRT_IOB_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FOPEN_IAT_RVA &&
               ISAAC_VITA_CRT_FOPEN_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_SET_FMODE_IAT_RVA &&
               ISAAC_VITA_CRT_SET_FMODE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VSSCANF_IAT_RVA &&
               ISAAC_VITA_CRT_VSSCANF_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VSPRINTF_IAT_RVA &&
               ISAAC_VITA_CRT_VSPRINTF_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VSPRINTF_S_IAT_RVA &&
               ISAAC_VITA_CRT_VSPRINTF_S_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FCLOSE_IAT_RVA,
               "frozen PE complete stdio IAT run drifted");
_Static_assert(ISAAC_VITA_CRT_CONVERT_IMPORT_COUNT == 6U &&
               ISAAC_VITA_CRT_NUMERIC_CONVERT_IMPORT_COUNT == 4U &&
               ISAAC_VITA_CRT_NUMERIC_CONVERT_CALL_COUNT == 456U &&
               ISAAC_VITA_CRT_STRTOULL_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_MBSTOWCS_S_IAT_RVA &&
               ISAAC_VITA_CRT_MBSTOWCS_S_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_WCSTOMBS_S_IAT_RVA &&
               ISAAC_VITA_CRT_WCSTOMBS_S_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_STRTOL_IAT_RVA &&
               ISAAC_VITA_CRT_STRTOL_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_ATOI_IAT_RVA &&
               ISAAC_VITA_CRT_ATOI_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_ATOF_IAT_RVA &&
               ISAAC_VITA_CRT_STRTOULL_CALL_FNV64 ==
                   UINT64_C(0x0035ba6d7f48d3dc) &&
               ISAAC_VITA_CRT_STRTOL_CALL_FNV64 ==
                   UINT64_C(0xde15b345976c94b8) &&
               ISAAC_VITA_CRT_ATOI_DIRECT_CALL_COUNT == 191U &&
               ISAAC_VITA_CRT_ATOI_DIRECT_CALL_FNV64 ==
                   UINT64_C(0x12df3796e35d8810) &&
               ISAAC_VITA_CRT_ATOI_REGISTER_LOAD_COUNT == 46U &&
               ISAAC_VITA_CRT_ATOI_REGISTER_LOAD_FNV64 ==
                   UINT64_C(0x511eadab81df57e7) &&
               ISAAC_VITA_CRT_ATOI_REGISTER_CALL_COUNT == 102U &&
               ISAAC_VITA_CRT_ATOI_REGISTER_CALL_FNV64 ==
                   UINT64_C(0x1326d7c20835a7fd) &&
               ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_COUNT == 293U &&
               ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_FNV64 ==
                   UINT64_C(0x6781eec9a7526398) &&
               ISAAC_VITA_CRT_ATOI_DIRECT_CALL_COUNT +
                   ISAAC_VITA_CRT_ATOI_REGISTER_CALL_COUNT ==
                   ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_COUNT &&
               ISAAC_VITA_CRT_ATOF_CALL_FNV64 ==
                   UINT64_C(0xdbaef5daea0a3908),
               "frozen PE complete convert-DLL census drifted");

_Alignas(8) static uint32_t s_frame[16];
_Alignas(8) static uint32_t s_initializer_table[4];
_Alignas(8) static uint32_t s_slist[6];
_Alignas(8) static uint32_t s_output[4];
_Alignas(8) static uint32_t s_boot_frame[8];
_Alignas(8) static uint16_t s_wide_source[32];
_Alignas(8) static uint8_t s_narrow_output[32];
_Alignas(8) static uint8_t s_secure_output[32];
_Alignas(8) static uint32_t s_secure_varargs[2];
static char s_secure_format[] = ISAAC_VITA_CRT_VSPRINTF_S_FORMAT;
static char s_secure_left[] = ISAAC_VITA_CRT_VSPRINTF_S_FIRST_LEFT;
static char s_secure_right[] = ISAAC_VITA_CRT_VSPRINTF_S_FIRST_RIGHT;
_Alignas(8) static unsigned char s_string_left[32];
_Alignas(8) static unsigned char s_string_right[32];
_Alignas(8) static unsigned char s_string_output[32];
static char s_scan_version_input[] = "2.0 VitaGL inventory";
static char s_scan_version_format[] = "%i.%i";
static char s_scan_shader_input[] = "ISAACNG_GSR4294967295";
static char s_scan_shader_format[] = "ISAACNG_GSR%u";
static char s_scan_triple_input[] = "-3.12.0 NVIDIA";
static char s_scan_triple_format[] = "%d.%d.%d";
static char s_scan_partial_input[] = "7.x";
static char s_scan_unknown_format[] = "%f";
_Alignas(8) static uint32_t s_scan_outputs[3];
_Alignas(8) static uint32_t s_scan_varargs[3];
_Alignas(8) static uint32_t s_time_known[2] = { 951827696U, 0U };
_Alignas(8) static uint32_t s_time_written[4];
_Alignas(8) static uint32_t s_time_tm_guarded[11];
static char s_time_format[] = ISAAC_VITA_CRT_STRFTIME_FORMAT;
static char s_time_formatted[ISAAC_VITA_CRT_STRFTIME_CAPACITY];
static uint64_t s_test_filetime = UINT64_C(0x01bf82b162519800);
static int s_test_filetime_result;
static char s_file_oracle_path[] =
    "ux0:data/crt-stdio-oracle.tmp";
static char s_file_mode_wb[] = ISAAC_VITA_CRT_FOPEN_BINARY_WRITE_MODE;
static char s_file_mode_rb[] = ISAAC_VITA_CRT_FOPEN_READ_MODE;
static char s_file_mode_w[] = ISAAC_VITA_CRT_FOPEN_WRITE_MODE;
static char s_file_mode_r[] = ISAAC_VITA_CRT_FOPEN_TEXT_READ_MODE;
static char s_file_mode_invalid[] = "rx";
static char s_file_bad_format[] = "%q";
static char s_file_good_format[] = "V";
_Alignas(8) static uint32_t s_file_varargs[1];
_Alignas(8) static unsigned char s_file_payload[32];
_Alignas(8) static unsigned char s_file_readback[32];
_Alignas(8) static uint32_t s_archive_read_guard[4];
static unsigned s_native_fread_calls;
_Alignas(8) static char s_convert_text[64];
_Alignas(8) static uint32_t s_convert_endptr;
static uint32_t s_initializer_order[4];
static unsigned s_initializer_calls;
static unsigned s_must_not_run;
static unsigned s_boot_chain_status;
static unsigned s_sync_delegate_calls;
static unsigned s_memory_delegate_calls;
static unsigned s_console_delegate_calls;
static uint32_t s_exit_order[3];
static uint32_t s_exit_tls_args[3];
static unsigned s_exit_callback_count;
static unsigned s_guest_exit_calls;
static int32_t s_guest_exit_code;
static const char *s_guest_exit_api;

int isaac_vita_sync_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    s_sync_delegate_calls++;
    return 0;
}

int isaac_vita_memory_import_counted(CPU *__restrict c, const char *name,
                                     unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    s_memory_delegate_calls++;
    return 0;
}

int isaac_vita_console_import_counted(CPU *__restrict c, const char *name,
                                      unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    s_console_delegate_calls++;
    return 0;
}

#define ISAAC_REJECTING_DELEGATE(function_name) \
    int function_name(CPU *__restrict c, const char *name, \
                      unsigned *call_count) \
    { \
        (void)c; (void)name; (void)call_count; \
        return 0; \
    }

ISAAC_REJECTING_DELEGATE(isaac_vita_startup_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_fls_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_exception_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_find_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_file_lock_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_filesystem_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_math_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_rtti_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_heap_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_steam_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_com_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_post_com_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_gl_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_user32_import_counted)

int isaac_vita_gl_dynamic_counted(CPU *__restrict c, uint32_t token,
                                  unsigned *call_count)
{
    (void)c;
    (void)token;
    (void)call_count;
    return 0;
}

int isaac_vita_startup_map_path(CPU *__restrict c, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    const char *path = (const char *)(uintptr_t)guest_path;
    size_t length = strlen(path);
    (void)c;
    if (length >= capacity)
        return 0;
    memcpy(native_path, path, length + 1U);
    return 1;
}

static int s_guest_heap_terminal;
static int s_guest_heap_malloc_fail;
static unsigned s_guest_heap_malloc_calls;
static unsigned s_guest_heap_free_calls;

void *isaac_vita_guest_malloc(size_t size)
{
    ++s_guest_heap_malloc_calls;
    if (s_guest_heap_malloc_fail)
        return NULL;
    return malloc(size);
}

int isaac_vita_guest_free(void *pointer)
{
    ++s_guest_heap_free_calls;
    free(pointer);
    return 1;
}

int isaac_vita_guest_heap_terminal(void)
{
    return s_guest_heap_terminal;
}

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    /* 2000-02-29 12:34:56 UTC, matching the independent PC oracle. */
    if (s_test_filetime_result)
        return s_test_filetime_result;
    *filetime = s_test_filetime;
    return s_test_filetime_result;
}

uint32_t isaac_vita_get_thread_id(void)
{
    return 0U;
}

uint32_t isaac_vita_get_process_id(void)
{
    return 0U;
}

uint64_t isaac_vita_get_process_time(void)
{
    return 0U;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

size_t isaac_vita_crt_oracle_fread(void *buffer, size_t size,
                                   size_t count, FILE *stream)
{
    ++s_native_fread_calls;
    return fread(buffer, size, count, stream);
}

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint64_t oracle_double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static double oracle_double_from_bits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t prepare_call(CPU *c)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[4]);
    s_frame[4] = 0x0badc0deU;
    c->esp = esp;
    return esp;
}

static int call_crt_cdecl(CPU *c, const char *name,
                          const uint32_t *arguments, uint32_t count,
                          uint32_t *result)
{
    uint32_t esp = prepare_call(c);
    uint32_t i;

    for (i = 0U; i < count; ++i)
        s_frame[5U + i] = arguments[i];
    if (!isaac_vita_crt_import(c, name) || c->fault ||
        c->esp != esp + 4U)
        return 0;
    if (result)
        *result = c->eax;
    return 1;
}

static uint32_t prepare_archive_fread_call(CPU *c, uint32_t parent_return)
{
    uint32_t esp = prepare_call(c);

    s_frame[4] = ISAAC_VITA_CRT_FREAD_RETURN_RVA;
    c->ebp = pointer32(&s_frame[10]);
    s_frame[11] = parent_return; /* production adapter reads [EBP+4] */
    return esp;
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_IMPORT_COUNT; ++i) {
        const unsigned char *p =
            (const unsigned char *)s_evidence[i].name;
        uint32_t values[3];
        uint32_t j;

        do {
            hash = hash_byte(hash, *p);
        } while (*p++ != 0U);
        values[0] = s_evidence[i].iat_rva;
        values[1] = s_evidence[i].call_rva;
        values[2] = s_evidence[i].return_rva;
        for (j = 0U; j < 3U; ++j) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash, (uint8_t)(values[j] >> shift));
        }
    }
    return hash;
}

static int call_sites_valid(const import_call_site *sites, uint32_t count)
{
    uint32_t i;

    for (i = 0U; i < count; ++i) {
        uint32_t width = sites[i].return_rva - sites[i].call_rva;
        if (sites[i].call_rva == 0U ||
            (width != 2U && width != 6U))
            return 0;
    }
    return 1;
}

static int exit_sites_valid(void)
{
    static const import_call_site expected[] = {
        { 0x005990edU, 0x005990f3U },
        { 0x005991b7U, 0x005991bdU },
        { 0x005d3f75U, 0x005d3f7bU },
        { 0x005eb830U, 0x005eb835U }
    };
    uint32_t i;

    if (sizeof expected / sizeof expected[0] !=
        ISAAC_VITA_CRT_EXIT_PHYSICAL_CALL_COUNT)
        return 0;
    for (i = 0U; i < ISAAC_VITA_CRT_EXIT_PHYSICAL_CALL_COUNT; ++i) {
        if (s_exit_sites[i].call_rva != expected[i].call_rva ||
            s_exit_sites[i].return_rva != expected[i].return_rva)
            return 0;
    }
    return 1;
}

static int stdio_census_valid(void)
{
    const uint32_t *load_groups[] = {
        s_fileno_load_sites, s_fseek_load_sites,
        s_ftell_load_sites, s_acrt_iob_load_sites
    };
    const uint32_t load_counts[] = {
        (uint32_t)(sizeof s_fileno_load_sites /
                   sizeof s_fileno_load_sites[0]),
        (uint32_t)(sizeof s_fseek_load_sites /
                   sizeof s_fseek_load_sites[0]),
        (uint32_t)(sizeof s_ftell_load_sites /
                   sizeof s_ftell_load_sites[0]),
        (uint32_t)(sizeof s_acrt_iob_load_sites /
                   sizeof s_acrt_iob_load_sites[0])
    };
    uint32_t group;
    uint32_t i;

    if (!call_sites_valid(s_fileno_sites,
                          ISAAC_VITA_CRT_FILENO_CALL_COUNT) ||
        !call_sites_valid(s_fread_sites,
                          ISAAC_VITA_CRT_FREAD_CALL_COUNT) ||
        !call_sites_valid(s_fwrite_sites,
                          ISAAC_VITA_CRT_FWRITE_CALL_COUNT) ||
        !call_sites_valid(s_fseek_sites,
                          ISAAC_VITA_CRT_FSEEK_CALL_COUNT) ||
        !call_sites_valid(s_get_osfhandle_sites,
                          ISAAC_VITA_CRT_GET_OSFHANDLE_CALL_COUNT) ||
        !call_sites_valid(s_ftell_sites,
                          ISAAC_VITA_CRT_FTELL_CALL_COUNT) ||
        !call_sites_valid(s_fflush_sites,
                          ISAAC_VITA_CRT_FFLUSH_CALL_COUNT) ||
        !call_sites_valid(s_acrt_iob_sites,
                          ISAAC_VITA_CRT_ACRT_IOB_CALL_COUNT))
        return 0;
    for (group = 0U; group < sizeof load_groups / sizeof load_groups[0];
         ++group)
        for (i = 0U; i < load_counts[group]; ++i)
            if (!load_groups[group][i])
                return 0;
    return 1;
}

static int qsort_census_valid(void)
{
    uint32_t unique_comparators = 0U;
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT; ++i) {
        uint32_t j;
        int first = 1;

        if (!s_qsort_sites[i].owner_rva ||
            !s_qsort_sites[i].comparator_push_rva ||
            s_qsort_sites[i].return_rva - s_qsort_sites[i].call_rva != 6U ||
            !s_qsort_sites[i].comparator_rva || !s_qsort_sites[i].width)
            return 0;
        for (j = 0U; j < i; ++j) {
            if (s_qsort_sites[j].call_rva == s_qsort_sites[i].call_rva)
                return 0;
            if (s_qsort_sites[j].comparator_rva ==
                s_qsort_sites[i].comparator_rva)
                first = 0;
        }
        if (first)
            ++unique_comparators;
    }
    return unique_comparators == ISAAC_VITA_CRT_QSORT_COMPARATOR_COUNT &&
           s_qsort_sites[1].owner_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_OWNER_RVA &&
           s_qsort_sites[1].call_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_CALL_RVA &&
           s_qsort_sites[1].return_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_RETURN_RVA &&
           s_qsort_sites[1].comparator_push_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_PUSH_RVA &&
           s_qsort_sites[1].comparator_rva ==
               ISAAC_VITA_CRT_QSORT_LIVE_COMPARATOR_RVA &&
           s_qsort_sites[1].width == ISAAC_VITA_CRT_QSORT_LIVE_WIDTH;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    /* Production unwinds through guest_run_until_stop.  Returning here keeps
     * the exact fault snapshot observable to this focused oracle. */
    c->fault_addr = address;
    c->fault = what;
}

void guest_exit(CPU *__restrict c, int32_t code, const char *api)
{
    ++s_guest_exit_calls;
    s_guest_exit_code = code;
    s_guest_exit_api = api;
    c->fault = NULL;
    c->fault_addr = 0U;
    c->exit_code = code;
    c->exit_api = api;
    c->stop_kind = GUEST_RUN_EXIT;
}

static int boot_chain_import(CPU *__restrict c, const char *name,
                             uint32_t argument0, uint32_t argument1,
                             uint32_t argument2, uint32_t esp_advance,
                             unsigned expected_count)
{
    uint32_t esp;

    memset(s_boot_frame, 0xcc, sizeof s_boot_frame);
    esp = pointer32(&s_boot_frame[2]);
    s_boot_frame[2] = 0xb007c0deU;
    s_boot_frame[3] = argument0;
    s_boot_frame[4] = argument1;
    s_boot_frame[5] = argument2;
    c->esp = esp;
    if (!guest_host_import(c, name))
        return 0;
    return !c->fault && c->esp == esp + esp_advance &&
           g_host_import_calls == expected_count;
}

static void run_boot_chain(CPU *__restrict c)
{
    uint32_t next_esp;

    if (!boot_chain_import(c, ISAAC_VITA_CRT_SET_APP_TYPE_NAME,
                           1U, 0U, 0U, 4U, 6U)) {
        s_boot_chain_status = 6U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_SET_FMODE_NAME,
                           0x4000U, 0U, 0U, 4U, 7U)) {
        s_boot_chain_status = 7U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_P_COMMODE_NAME,
                           0U, 0U, 0U, 4U, 8U)) {
        s_boot_chain_status = 8U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_ATEXIT_NAME,
                           0x985eb04cU, 0U, 0U, 4U, 9U)) {
        s_boot_chain_status = 9U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_CONFIGURE_ARGV_NAME,
                           1U, 0U, 0U, 4U, 10U)) {
        s_boot_chain_status = 10U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_INITIALIZE_SLIST_NAME,
                           pointer32(&s_slist[2]), 0U, 0U, 8U, 11U)) {
        s_boot_chain_status = 11U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_CONTROLFP_S_NAME,
                           0U, 0x00010000U, 0x00030000U, 4U, 12U)) {
        s_boot_chain_status = 12U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME,
                           0U, 0U, 0U, 4U, 13U)) {
        s_boot_chain_status = 13U;
        return;
    }
    if (!boot_chain_import(c, ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_NAME,
                           0U, 0U, 0U, 4U, 14U)) {
        s_boot_chain_status = 14U;
        return;
    }

    memset(s_boot_frame, 0xcc, sizeof s_boot_frame);
    next_esp = pointer32(&s_boot_frame[2]);
    s_boot_frame[2] = ISAAC_VITA_CRT_NEXT_RETURN_RVA;
    c->esp = next_esp;
    if (guest_host_import(c, ISAAC_VITA_CRT_NEXT_NAME) != 0 ||
        g_host_import_calls != 14U) {
        s_boot_chain_status = 15U;
        return;
    }
    guest_fault(c, GUEST_IMAGE_BASE + ISAAC_VITA_CRT_NEXT_IAT_RVA,
                ISAAC_VITA_CRT_NEXT_NAME);
}

void guest_call(CPU *__restrict c, uint32_t target)
{
    if (target == CALLBACK_EXIT_TLS) {
        if (ld32(c->esp) != 0xFFF17311U) {
            guest_fault(c, target,
                        "oracle TLS callback missing return sentinel");
            return;
        }
        s_exit_tls_args[0] = ld32(c->esp + 4U);
        s_exit_tls_args[1] = ld32(c->esp + 8U);
        s_exit_tls_args[2] = ld32(c->esp + 12U);
        c->esp += 16U;
        s_exit_order[s_exit_callback_count++] = 3U;
        return;
    }
    if (ld32(c->esp) != 0xFFF1A11EU) {
        guest_fault(c, target, "oracle initializer missing return sentinel");
        return;
    }
    if (s_initializer_calls < 4U)
        s_initializer_order[s_initializer_calls] = target;
    ++s_initializer_calls;

    switch (target) {
    case CALLBACK_ZERO:
        (void)gpop(c);
        c->eax = 0U;
        break;
    case CALLBACK_NONZERO:
        (void)gpop(c);
        c->eax = 0x37U;
        break;
    case CALLBACK_MUST_NOT_RUN:
        ++s_must_not_run;
        (void)gpop(c);
        c->eax = 0x5a5aa5a5U;
        break;
    case CALLBACK_NESTED_FAULT:
        guest_fault(c, target, "oracle nested initializer fault");
        break;
    case CALLBACK_BAD_STACK:
        /* Deliberately leave the synthetic return on the guest stack. */
        c->eax = 0U;
        break;
    case CALLBACK_BOOT_CHAIN:
        run_boot_chain(c);
        break;
    case CALLBACK_EXIT_A:
        (void)gpop(c);
        s_exit_order[s_exit_callback_count++] = 1U;
        break;
    case CALLBACK_EXIT_B:
        (void)gpop(c);
        s_exit_order[s_exit_callback_count++] = 2U;
        break;
    default:
        guest_fault(c, target, "oracle received unknown initializer");
        break;
    }
}

static int run_oracle(void)
{
    CPU cpu;
    CPU snapshot;
    isaac_vita_crt_state state_snapshot;
    uint32_t esp;
    uint32_t commode_pointer;
    uint32_t environment_pointer;
    uint32_t argv_variable_pointer;
    uint32_t argv_pointer;
    uint32_t argv0_pointer;
    uint32_t argc_pointer;
    uint32_t i;
    int32_t guest_errno_snapshot;

    if (!pointer_fits(s_frame) || !pointer_fits(s_initializer_table) ||
        !pointer_fits(s_slist) || !pointer_fits(s_output) ||
        !pointer_fits(s_boot_frame) ||
        !pointer_fits(s_secure_output) ||
        !pointer_fits(s_secure_varargs) ||
        !pointer_fits(s_secure_format) ||
        !pointer_fits(s_secure_left) || !pointer_fits(s_secure_right) ||
        !pointer_fits(s_string_left) || !pointer_fits(s_string_right) ||
        !pointer_fits(s_string_output) ||
        !pointer_fits(s_scan_version_input) ||
        !pointer_fits(s_scan_version_format) ||
        !pointer_fits(s_scan_shader_input) ||
        !pointer_fits(s_scan_shader_format) ||
        !pointer_fits(s_scan_triple_input) ||
        !pointer_fits(s_scan_triple_format) ||
        !pointer_fits(s_scan_partial_input) ||
        !pointer_fits(s_scan_unknown_format) ||
        !pointer_fits(s_scan_outputs) || !pointer_fits(s_scan_varargs) ||
        !pointer_fits(s_time_known) || !pointer_fits(s_time_written) ||
        !pointer_fits(s_time_tm_guarded) ||
        !pointer_fits(s_time_format) || !pointer_fits(s_time_formatted) ||
        !pointer_fits(s_file_oracle_path) ||
        !pointer_fits(s_file_mode_wb) || !pointer_fits(s_file_mode_rb) ||
        !pointer_fits(s_file_mode_w) || !pointer_fits(s_file_mode_r) ||
        !pointer_fits(s_file_mode_invalid) ||
        !pointer_fits(s_file_bad_format) ||
        !pointer_fits(s_file_good_format) ||
        !pointer_fits(s_file_varargs) ||
        !pointer_fits(s_file_payload) || !pointer_fits(s_file_readback) ||
        !pointer_fits(s_archive_read_guard) ||
        !pointer_fits(s_convert_text) || !pointer_fits(&s_convert_endptr) ||
        !pointer_fits(&g_isaac_vita_crt) ||
        !pointer_fits(&g_isaac_vita_crt_errno))
        return 1;
    if (evidence_hash() != UINT64_C(0x14f4c13ffce02b11))
        return 2;
    if (!stdio_census_valid())
        return 119;
    if (!qsort_census_valid())
        return 179;
    if (!exit_sites_valid())
        return 180;
    if (strcmp(ISAAC_VITA_CRT_NEXT_NAME,
               "KERNEL32.dll!InitializeCriticalSectionAndSpinCount") != 0 ||
        ISAAC_VITA_CRT_NEXT_IAT_RVA != 0x0060606cU ||
        ISAAC_VITA_CRT_NEXT_CALL_RVA != 0x005eb16fU ||
        ISAAC_VITA_CRT_NEXT_RETURN_RVA != 0x005eb175U ||
        strcmp(ISAAC_VITA_CRT_INITTERM_NAME,
               "api-ms-win-crt-runtime-l1-1-0.dll!_initterm") != 0 ||
        ISAAC_VITA_CRT_INITTERM_IAT_RVA != 0x00606580U ||
        ISAAC_VITA_CRT_INITTERM_IAT_VA != 0x98606580U ||
        ISAAC_VITA_CRT_INITTERM_CALL_RVA != 0x005eb733U ||
        ISAAC_VITA_CRT_INITTERM_RETURN_RVA != 0x005eb738U ||
        ISAAC_VITA_CRT_INITTERM_TABLE_START != 0x986066e0U ||
        ISAAC_VITA_CRT_INITTERM_TABLE_END != 0x98606890U ||
        (ISAAC_VITA_CRT_INITTERM_TABLE_END -
         ISAAC_VITA_CRT_INITTERM_TABLE_START) / 4U != 108U ||
        ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT +
            ISAAC_VITA_CRT_IMPORT_COUNT != 64U ||
        ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_BOOT_ORDINAL != 263U ||
        ISAAC_VITA_CRT_P_ARGV_BOOT_ORDINAL != 264U ||
        ISAAC_VITA_CRT_P_ARGC_BOOT_ORDINAL != 265U ||
        strcmp(ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_NAME,
               "api-ms-win-crt-runtime-l1-1-0.dll!_get_initial_narrow_environment") != 0 ||
        ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_IAT_RVA != 0x00606574U ||
        ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_CALL_RVA != 0x005eb79aU ||
        ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_RETURN_RVA != 0x005eb79fU ||
        strcmp(ISAAC_VITA_CRT_P_ARGV_NAME,
               "api-ms-win-crt-runtime-l1-1-0.dll!__p___argv") != 0 ||
        ISAAC_VITA_CRT_P_ARGV_IAT_RVA != 0x006065d4U ||
        ISAAC_VITA_CRT_P_ARGV_CALL_RVA != 0x005eb7a1U ||
        ISAAC_VITA_CRT_P_ARGV_RETURN_RVA != 0x005eb7a6U ||
        strcmp(ISAAC_VITA_CRT_P_ARGC_NAME,
               "api-ms-win-crt-runtime-l1-1-0.dll!__p___argc") != 0 ||
        ISAAC_VITA_CRT_P_ARGC_IAT_RVA != 0x006065b8U ||
        ISAAC_VITA_CRT_P_ARGC_CALL_RVA != 0x005eb7a8U ||
        ISAAC_VITA_CRT_P_ARGC_RETURN_RVA != 0x005eb7adU ||
        strcmp(ISAAC_VITA_CRT_WCSTOMBS_S_NAME,
               "api-ms-win-crt-convert-l1-1-0.dll!wcstombs_s") != 0 ||
        ISAAC_VITA_CRT_WCSTOMBS_S_IAT_RVA != 0x006064d0U ||
        ISAAC_VITA_CRT_WCSTOMBS_S_CALL_RVA != 0x00563303U ||
        ISAAC_VITA_CRT_WCSTOMBS_S_RETURN_RVA != 0x00563305U ||
        ISAAC_VITA_CRT_WCSTOMBS_S_ALT_CALL_RVA != 0x0056332cU ||
        ISAAC_VITA_CRT_WCSTOMBS_S_ALT_RETURN_RVA != 0x0056332fU ||
        ISAAC_VITA_CRT_WCSTOMBS_S_FIRST_ORDINAL != 347U ||
        ISAAC_VITA_CRT_WCSTOMBS_S_MEASURED_CALL_COUNT != 7U ||
        strcmp(ISAAC_VITA_CRT_VSPRINTF_S_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vsprintf_s") != 0 ||
        ISAAC_VITA_CRT_VSPRINTF_S_IAT_RVA != 0x00606618U ||
        ISAAC_VITA_CRT_VSPRINTF_S_IMPORT_CALL_RVA != 0x00011fcaU ||
        ISAAC_VITA_CRT_VSPRINTF_S_IMPORT_RETURN_RVA != 0x00011fd0U ||
        ISAAC_VITA_CRT_VSPRINTF_S_ADAPTER_RVA != 0x00011fb0U ||
        ISAAC_VITA_CRT_VSPRINTF_S_WRAPPER_RVA != 0x001ef060U ||
        ISAAC_VITA_CRT_VSPRINTF_S_WRAPPER_CALL_RVA != 0x001ef072U ||
        ISAAC_VITA_CRT_VSPRINTF_S_WRAPPER_RETURN_RVA != 0x001ef077U ||
        ISAAC_VITA_CRT_VSPRINTF_S_ORIGIN_CALL_RVA != 0x005987ceU ||
        ISAAC_VITA_CRT_VSPRINTF_S_ORIGIN_RETURN_RVA != 0x005987d3U ||
        ISAAC_VITA_CRT_VSPRINTF_S_FORMAT_VA != 0x9876492cU ||
        strcmp(ISAAC_VITA_CRT_VSPRINTF_S_FORMAT, "%s/%s") != 0 ||
        ISAAC_VITA_CRT_VSPRINTF_S_FIRST_CAPACITY != 0x13U ||
        ISAAC_VITA_CRT_VSPRINTF_S_FIRST_ORDINAL != 445U ||
        strcmp(ISAAC_VITA_CRT_VSSCANF_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vsscanf") != 0 ||
        ISAAC_VITA_CRT_VSSCANF_IAT_RVA != 0x00606610U ||
        ISAAC_VITA_CRT_VSSCANF_IAT_VA != 0x98606610U ||
        ISAAC_VITA_CRT_VSSCANF_ADAPTER_RVA != 0x003f0ef0U ||
        ISAAC_VITA_CRT_VSSCANF_IMPORT_CALL_RVA != 0x003f0f09U ||
        ISAAC_VITA_CRT_VSSCANF_IMPORT_RETURN_RVA != 0x003f0f0fU ||
        ISAAC_VITA_CRT_VSSCANF_FIRST_BEFORE_COUNT != 563U ||
        ISAAC_VITA_CRT_VSSCANF_FIRST_ORDINAL != 564U ||
        ISAAC_VITA_CRT_VSSCANF_SHADER_CALL1_RVA != 0x00524399U ||
        ISAAC_VITA_CRT_VSSCANF_SHADER_RETURN1_RVA != 0x0052439eU ||
        ISAAC_VITA_CRT_VSSCANF_SHADER_CALL2_RVA != 0x0052e40fU ||
        ISAAC_VITA_CRT_VSSCANF_SHADER_RETURN2_RVA != 0x0052e414U ||
        ISAAC_VITA_CRT_VSSCANF_VERSION_CALL_RVA != 0x00599198U ||
        ISAAC_VITA_CRT_VSSCANF_VERSION_RETURN_RVA != 0x0059919dU ||
        ISAAC_VITA_CRT_VSSCANF_TRIPLE_CALL_RVA != 0x0059aa43U ||
        ISAAC_VITA_CRT_VSSCANF_TRIPLE_RETURN_RVA != 0x0059aa48U ||
        ISAAC_VITA_CRT_VSSCANF_SHADER_FORMAT_VA != 0x9875b470U ||
        ISAAC_VITA_CRT_VSSCANF_VERSION_FORMAT_VA != 0x987649e4U ||
        ISAAC_VITA_CRT_VSSCANF_TRIPLE_FORMAT_VA != 0x98747dacU ||
        ISAAC_VITA_CRT_VSSCANF_STATIC_CALL_COUNT != 4U ||
        ISAAC_VITA_CRT_VSSCANF_STATIC_FORMAT_COUNT != 3U ||
        strcmp(ISAAC_VITA_CRT_ERRNO_NAME,
               "api-ms-win-crt-runtime-l1-1-0.dll!_errno") != 0 ||
        ISAAC_VITA_CRT_ERRNO_IAT_RVA != 0x006065a4U ||
        ISAAC_VITA_CRT_ERRNO_IAT_VA != 0x986065a4U ||
        ISAAC_VITA_CRT_ERRNO_CALL1_OWNER_RVA != 0x005965d0U ||
        ISAAC_VITA_CRT_ERRNO_CALL1_RVA != 0x0059662eU ||
        ISAAC_VITA_CRT_ERRNO_RETURN1_RVA != 0x00596634U ||
        ISAAC_VITA_CRT_ERRNO_CALL1_STORE_VALUE != 22U ||
        ISAAC_VITA_CRT_ERRNO_CALL2_OWNER_RVA != 0x005e1e90U ||
        ISAAC_VITA_CRT_ERRNO_CALL2_RVA != 0x005e1f5cU ||
        ISAAC_VITA_CRT_ERRNO_RETURN2_RVA != 0x005e1f62U ||
        ISAAC_VITA_CRT_ERRNO_CALL2_STORE_VALUE != 12U ||
        ISAAC_VITA_CRT_ERRNO_DIRECT_CALL_COUNT != 2U ||
        strcmp(ISAAC_VITA_CRT_SET_ERRNO_NAME,
               "api-ms-win-crt-runtime-l1-1-0.dll!_set_errno") != 0 ||
        ISAAC_VITA_CRT_SET_ERRNO_IAT_RVA != 0x006065d8U ||
        ISAAC_VITA_CRT_SET_ERRNO_IAT_VA != 0x986065d8U ||
        ISAAC_VITA_CRT_SET_ERRNO_OWNER1_RVA != 0x00562ef0U ||
        ISAAC_VITA_CRT_SET_ERRNO_CALL1_RVA != 0x0056305cU ||
        ISAAC_VITA_CRT_SET_ERRNO_RETURN1_RVA != 0x00563062U ||
        ISAAC_VITA_CRT_SET_ERRNO_CALL2_RVA != 0x005630a8U ||
        ISAAC_VITA_CRT_SET_ERRNO_RETURN2_RVA != 0x005630aeU ||
        ISAAC_VITA_CRT_SET_ERRNO_CALL3_RVA != 0x005630bdU ||
        ISAAC_VITA_CRT_SET_ERRNO_RETURN3_RVA != 0x005630c3U ||
        ISAAC_VITA_CRT_SET_ERRNO_CALL4_RVA != 0x005630efU ||
        ISAAC_VITA_CRT_SET_ERRNO_RETURN4_RVA != 0x005630f5U ||
        ISAAC_VITA_CRT_SET_ERRNO_OWNER2_RVA != 0x00563200U ||
        ISAAC_VITA_CRT_SET_ERRNO_CALL5_RVA != 0x005634e5U ||
        ISAAC_VITA_CRT_SET_ERRNO_RETURN5_RVA != 0x005634ebU ||
        ISAAC_VITA_CRT_SET_ERRNO_ENOENT_VALUE != 2U ||
        ISAAC_VITA_CRT_SET_ERRNO_ENOENT_CALL_COUNT != 4U ||
        ISAAC_VITA_CRT_SET_ERRNO_EBADF_VALUE != 9U ||
        ISAAC_VITA_CRT_SET_ERRNO_EBADF_CALL_COUNT != 1U ||
        ISAAC_VITA_CRT_SET_ERRNO_DIRECT_CALL_COUNT != 5U ||
        ISAAC_VITA_CRT_ERRNO_FAMILY_IMPORT_COUNT != 2U ||
        ISAAC_VITA_CRT_DOSERRNO_IMPORT_COUNT != 0U ||
        ISAAC_VITA_CRT_ERRNO_FAMILY_DIRECT_CALL_COUNT != 7U)
        return 41;
    if (strcmp(ISAAC_VITA_CRT_STRFTIME_NAME,
               "api-ms-win-crt-time-l1-1-0.dll!strftime") != 0 ||
        ISAAC_VITA_CRT_STRFTIME_IAT_RVA != 0x00606660U ||
        ISAAC_VITA_CRT_STRFTIME_IAT_VA != 0x98606660U ||
        ISAAC_VITA_CRT_STRFTIME_OWNER_RVA != 0x003f65d0U ||
        ISAAC_VITA_CRT_STRFTIME_CALL_RVA != 0x003f71d6U ||
        ISAAC_VITA_CRT_STRFTIME_RETURN_RVA != 0x003f71dcU ||
        ISAAC_VITA_CRT_STRFTIME_FORMAT_VA != 0x9874f08cU ||
        strcmp(ISAAC_VITA_CRT_STRFTIME_FORMAT, "%a, %m/%d/%Y") != 0 ||
        ISAAC_VITA_CRT_STRFTIME_CAPACITY != 0x48U ||
        ISAAC_VITA_CRT_STRFTIME_CALL_COUNT != 1U ||
        strcmp(ISAAC_VITA_CRT_GMTIME64_NAME,
               "api-ms-win-crt-time-l1-1-0.dll!_gmtime64") != 0 ||
        ISAAC_VITA_CRT_GMTIME64_IAT_RVA != 0x00606664U ||
        ISAAC_VITA_CRT_GMTIME64_IAT_VA != 0x98606664U ||
        ISAAC_VITA_CRT_GMTIME64_OWNER1_RVA != 0x0001ee50U ||
        ISAAC_VITA_CRT_GMTIME64_LOAD1_RVA != 0x0001eea9U ||
        ISAAC_VITA_CRT_GMTIME64_CALL1_RVA != 0x0001eebaU ||
        ISAAC_VITA_CRT_GMTIME64_RETURN1_RVA != 0x0001eebcU ||
        ISAAC_VITA_CRT_GMTIME64_OWNER2_RVA != 0x0001fb20U ||
        ISAAC_VITA_CRT_GMTIME64_CALL2_RVA != 0x0001fcdaU ||
        ISAAC_VITA_CRT_GMTIME64_RETURN2_RVA != 0x0001fce0U ||
        ISAAC_VITA_CRT_GMTIME64_OWNER3_RVA != 0x003f5090U ||
        ISAAC_VITA_CRT_GMTIME64_LOAD3_RVA != 0x003f50dcU ||
        ISAAC_VITA_CRT_GMTIME64_CALL3_RVA != 0x003f50e8U ||
        ISAAC_VITA_CRT_GMTIME64_RETURN3_RVA != 0x003f50eaU ||
        ISAAC_VITA_CRT_GMTIME64_OWNER4_RVA != 0x003f5240U ||
        ISAAC_VITA_CRT_GMTIME64_LOAD4_RVA != 0x003f528cU ||
        ISAAC_VITA_CRT_GMTIME64_CALL4_RVA != 0x003f5298U ||
        ISAAC_VITA_CRT_GMTIME64_RETURN4_RVA != 0x003f529aU ||
        ISAAC_VITA_CRT_GMTIME64_OWNER5_RVA != 0x003f65d0U ||
        ISAAC_VITA_CRT_GMTIME64_CALL5_RVA != 0x003f718cU ||
        ISAAC_VITA_CRT_GMTIME64_RETURN5_RVA != 0x003f7192U ||
        ISAAC_VITA_CRT_GMTIME64_OWNER6_RVA != 0x003f85d0U ||
        ISAAC_VITA_CRT_GMTIME64_CALL6_RVA != 0x003f85f8U ||
        ISAAC_VITA_CRT_GMTIME64_RETURN6_RVA != 0x003f85feU ||
        ISAAC_VITA_CRT_GMTIME64_OWNER7_RVA != 0x00466270U ||
        ISAAC_VITA_CRT_GMTIME64_CALL7_RVA != 0x00466af8U ||
        ISAAC_VITA_CRT_GMTIME64_RETURN7_RVA != 0x00466afeU ||
        ISAAC_VITA_CRT_GMTIME64_OWNER8_RVA != 0x004b3590U ||
        ISAAC_VITA_CRT_GMTIME64_CALL8_RVA != 0x004b35d6U ||
        ISAAC_VITA_CRT_GMTIME64_RETURN8_RVA != 0x004b35dcU ||
        ISAAC_VITA_CRT_GMTIME64_OWNER9_RVA != 0x00566e40U ||
        ISAAC_VITA_CRT_GMTIME64_CALL9_RVA != 0x00566e8dU ||
        ISAAC_VITA_CRT_GMTIME64_RETURN9_RVA != 0x00566e93U ||
        ISAAC_VITA_CRT_GMTIME64_CALL10_RVA != 0x00566eb8U ||
        ISAAC_VITA_CRT_GMTIME64_RETURN10_RVA != 0x00566ebeU ||
        ISAAC_VITA_CRT_GMTIME64_CALL_COUNT != 10U ||
        strcmp(ISAAC_VITA_CRT_TIME64_NAME,
               "api-ms-win-crt-time-l1-1-0.dll!_time64") != 0 ||
        ISAAC_VITA_CRT_TIME64_IAT_RVA != 0x00606668U ||
        ISAAC_VITA_CRT_TIME64_IAT_VA != 0x98606668U ||
        ISAAC_VITA_CRT_TIME64_OWNER1_RVA != 0x00484660U ||
        ISAAC_VITA_CRT_TIME64_CALL1_RVA != 0x00484681U ||
        ISAAC_VITA_CRT_TIME64_RETURN1_RVA != 0x00484687U ||
        ISAAC_VITA_CRT_TIME64_OWNER2_RVA != 0x0050b240U ||
        ISAAC_VITA_CRT_TIME64_CALL2_RVA != 0x0050b7f9U ||
        ISAAC_VITA_CRT_TIME64_RETURN2_RVA != 0x0050b7ffU ||
        ISAAC_VITA_CRT_TIME64_OWNER3_RVA != 0x00566e40U ||
        ISAAC_VITA_CRT_TIME64_CALL3_RVA != 0x00566e74U ||
        ISAAC_VITA_CRT_TIME64_RETURN3_RVA != 0x00566e7aU ||
        ISAAC_VITA_CRT_TIME64_NULL_CALL_COUNT != 3U ||
        ISAAC_VITA_CRT_TIME64_FRONTIER_CALL_RVA != 0x0050b7f9U ||
        ISAAC_VITA_CRT_TIME64_FRONTIER_RETURN_RVA != 0x0050b7ffU ||
        ISAAC_VITA_CRT_TIME64_FRONTIER_BEFORE_COUNT != 2774U ||
        ISAAC_VITA_CRT_TIME64_FRONTIER_ORDINAL != 2775U ||
        strcmp(ISAAC_VITA_CRT_MKGMTIME64_NAME,
               "api-ms-win-crt-time-l1-1-0.dll!_mkgmtime64") != 0 ||
        ISAAC_VITA_CRT_MKGMTIME64_IAT_RVA != 0x0060666cU ||
        ISAAC_VITA_CRT_MKGMTIME64_IAT_VA != 0x9860666cU ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER1_RVA != 0x0001ee50U ||
        ISAAC_VITA_CRT_MKGMTIME64_LOAD1_RVA != 0x0001ee81U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL1_RVA != 0x0001ee92U ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN1_RVA != 0x0001ee94U ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER2_RVA != 0x0001fb20U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL2_RVA != 0x0001fcbfU ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN2_RVA != 0x0001fcc5U ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER3_RVA != 0x003f5090U ||
        ISAAC_VITA_CRT_MKGMTIME64_LOAD3_RVA != 0x003f50c1U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL3_RVA != 0x003f50cdU ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN3_RVA != 0x003f50cfU ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER4_RVA != 0x003f5240U ||
        ISAAC_VITA_CRT_MKGMTIME64_LOAD4_RVA != 0x003f5271U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL4_RVA != 0x003f527dU ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN4_RVA != 0x003f527fU ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER5_RVA != 0x003f65d0U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL5_RVA != 0x003f7166U ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN5_RVA != 0x003f716cU ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER6_RVA != 0x003f85d0U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL6_RVA != 0x003f85deU ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN6_RVA != 0x003f85e4U ||
        ISAAC_VITA_CRT_MKGMTIME64_OWNER7_RVA != 0x00466270U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL7_RVA != 0x00466addU ||
        ISAAC_VITA_CRT_MKGMTIME64_RETURN7_RVA != 0x00466ae3U ||
        ISAAC_VITA_CRT_MKGMTIME64_CALL_COUNT != 7U ||
        strcmp(ISAAC_VITA_CRT_LOCALTIME64_NAME,
               "api-ms-win-crt-time-l1-1-0.dll!_localtime64") != 0 ||
        ISAAC_VITA_CRT_LOCALTIME64_IAT_RVA != 0x00606670U ||
        ISAAC_VITA_CRT_LOCALTIME64_IAT_VA != 0x98606670U ||
        ISAAC_VITA_CRT_LOCALTIME64_OWNER_RVA != 0x00484660U ||
        ISAAC_VITA_CRT_LOCALTIME64_CALL_RVA != 0x00484697U ||
        ISAAC_VITA_CRT_LOCALTIME64_RETURN_RVA != 0x0048469dU ||
        ISAAC_VITA_CRT_LOCALTIME64_CALL_COUNT != 1U ||
        ISAAC_VITA_CRT_TIME_IMPORT_COUNT != 5U ||
        ISAAC_VITA_CRT_TIME_CALL_COUNT != 22U ||
        ISAAC_VITA_CRT_TM_DWORD_COUNT != 9U ||
        ISAAC_VITA_CRT_FILETIME_UNIX_EPOCH !=
            UINT64_C(116444736000000000) ||
        ISAAC_VITA_CRT_FILETIME_TICKS_PER_SECOND != UINT64_C(10000000) ||
        ISAAC_VITA_CRT_TIME64_MAX != INT64_C(32535215999))
        return 100;
    if (strcmp(ISAAC_VITA_CRT_FILENO_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!_fileno") != 0 ||
        ISAAC_VITA_CRT_FILENO_IAT_RVA != 0x006065e0U ||
        ISAAC_VITA_CRT_FILENO_FIRST_CALL_RVA != 0x0040dd88U ||
        ISAAC_VITA_CRT_FILENO_FIRST_RETURN_RVA != 0x0040dd8eU ||
        ISAAC_VITA_CRT_FILENO_CALL_COUNT != 24U ||
        strcmp(ISAAC_VITA_CRT_FREAD_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!fread") != 0 ||
        ISAAC_VITA_CRT_FREAD_IAT_RVA != 0x006065e4U ||
        ISAAC_VITA_CRT_FREAD_ADAPTER_RVA != 0x00596560U ||
        ISAAC_VITA_CRT_FREAD_CALL_RVA != 0x0059657bU ||
        ISAAC_VITA_CRT_FREAD_RETURN_RVA != 0x00596581U ||
        ISAAC_VITA_CRT_FREAD_FIRST_ORIGIN_CALL_RVA != 0x00563a3aU ||
        ISAAC_VITA_CRT_FREAD_FIRST_ORIGIN_RETURN_RVA != 0x00563a3dU ||
        ISAAC_VITA_CRT_FREAD_FIRST_SIZE != 1U ||
        ISAAC_VITA_CRT_FREAD_FIRST_COUNT != 7U ||
        ISAAC_VITA_CRT_FREAD_PREDICTED_BEFORE_COUNT != 2792U ||
        ISAAC_VITA_CRT_FREAD_PREDICTED_ORDINAL != 2793U ||
        ISAAC_VITA_CRT_FREAD_PREDICTED_DYNAMIC_COUNT != 79U ||
        strcmp(ISAAC_VITA_CRT_FWRITE_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!fwrite") != 0 ||
        ISAAC_VITA_CRT_FWRITE_IAT_RVA != 0x006065e8U ||
        ISAAC_VITA_CRT_FWRITE_CALL_RVA != 0x005965abU ||
        ISAAC_VITA_CRT_FWRITE_RETURN_RVA != 0x005965b1U ||
        ISAAC_VITA_CRT_FWRITE_FIRST_ORIGIN_CALL_RVA != 0x0055e4e3U ||
        ISAAC_VITA_CRT_FWRITE_FIRST_ORIGIN_RETURN_RVA != 0x0055e4e6U ||
        ISAAC_VITA_CRT_FWRITE_FIRST_BEFORE_COUNT != 2771U ||
        ISAAC_VITA_CRT_FWRITE_FIRST_ORDINAL != 2772U ||
        strcmp(ISAAC_VITA_CRT_FSEEK_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!fseek") != 0 ||
        ISAAC_VITA_CRT_FSEEK_IAT_RVA != 0x006065ecU ||
        ISAAC_VITA_CRT_FSEEK_CALL_COUNT != 5U ||
        strcmp(ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!_get_osfhandle") != 0 ||
        ISAAC_VITA_CRT_GET_OSFHANDLE_IAT_RVA != 0x006065f0U ||
        ISAAC_VITA_CRT_GET_OSFHANDLE_CALL_RVA != 0x005965dbU ||
        ISAAC_VITA_CRT_GET_OSFHANDLE_RETURN_RVA != 0x005965e1U ||
        strcmp(ISAAC_VITA_CRT_FTELL_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!ftell") != 0 ||
        ISAAC_VITA_CRT_FTELL_IAT_RVA != 0x006065f4U ||
        ISAAC_VITA_CRT_FTELL_CALL_COUNT != 3U ||
        strcmp(ISAAC_VITA_CRT_FFLUSH_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!fflush") != 0 ||
        ISAAC_VITA_CRT_FFLUSH_IAT_RVA != 0x00606600U ||
        ISAAC_VITA_CRT_FFLUSH_FIRST_CALL_RVA != 0x005965c3U ||
        ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA != 0x005965c9U ||
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_CALL_RVA != 0x0055e4f7U ||
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA != 0x0055e4faU ||
        ISAAC_VITA_CRT_FFLUSH_FIRST_BEFORE_COUNT != 2772U ||
        ISAAC_VITA_CRT_FFLUSH_FIRST_ORDINAL != 2773U ||
        strcmp(ISAAC_VITA_CRT_ACRT_IOB_NAME,
               "api-ms-win-crt-stdio-l1-1-0.dll!__acrt_iob_func") != 0 ||
        ISAAC_VITA_CRT_ACRT_IOB_IAT_RVA != 0x00606604U ||
        ISAAC_VITA_CRT_ACRT_IOB_CALL_COUNT != 55U ||
        ISAAC_VITA_CRT_FILE_IO_IMPORT_COUNT != 8U ||
        ISAAC_VITA_CRT_FILE_IO_CALL_COUNT != 92U ||
        ISAAC_VITA_CRT_STDIO_IMPORT_COUNT != 16U ||
        ISAAC_VITA_CRT_FILE_TOKEN_COUNT != 16U ||
        ISAAC_VITA_CRT_STANDARD_STREAM_COUNT != 3U ||
        ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE != 16U * 1024U ||
        strcmp(ISAAC_VITA_CRT_FOPEN_BINARY_WRITE_MODE, "wb") != 0 ||
        ISAAC_VITA_CRT_FOPEN_BINARY_WRITE_MODE_VA != 0x9876488cU ||
        strcmp(ISAAC_VITA_CRT_FOPEN_TEXT_READ_MODE, "r") != 0 ||
        ISAAC_VITA_CRT_FOPEN_TEXT_READ_MODE_VA != 0x98747e04U)
        return 120;
    if (strcmp(ISAAC_VITA_CRT_STRTOULL_NAME,
               "api-ms-win-crt-convert-l1-1-0.dll!strtoull") != 0 ||
        ISAAC_VITA_CRT_STRTOULL_IAT_RVA != 0x006064c8U ||
        ISAAC_VITA_CRT_STRTOULL_CALL1_RVA != 0x0025e6f1U ||
        ISAAC_VITA_CRT_STRTOULL_RETURN1_RVA != 0x0025e6f7U ||
        ISAAC_VITA_CRT_STRTOULL_CALL2_RVA != 0x00265770U ||
        ISAAC_VITA_CRT_STRTOULL_RETURN2_RVA != 0x00265776U ||
        ISAAC_VITA_CRT_STRTOULL_DIRECT_CALL_COUNT != 2U ||
        ISAAC_VITA_CRT_STRTOULL_CALL_FNV64 !=
            UINT64_C(0x0035ba6d7f48d3dc) ||
        strcmp(ISAAC_VITA_CRT_STRTOL_NAME,
               "api-ms-win-crt-convert-l1-1-0.dll!strtol") != 0 ||
        ISAAC_VITA_CRT_STRTOL_IAT_RVA != 0x006064d4U ||
        ISAAC_VITA_CRT_STRTOL_CALL_RVA != 0x005e39bcU ||
        ISAAC_VITA_CRT_STRTOL_RETURN_RVA != 0x005e39c2U ||
        ISAAC_VITA_CRT_STRTOL_DIRECT_CALL_COUNT != 1U ||
        ISAAC_VITA_CRT_STRTOL_CALL_FNV64 !=
            UINT64_C(0xde15b345976c94b8) ||
        strcmp(ISAAC_VITA_CRT_ATOI_NAME,
               "api-ms-win-crt-convert-l1-1-0.dll!atoi") != 0 ||
        ISAAC_VITA_CRT_ATOI_IAT_RVA != 0x006064d8U ||
        ISAAC_VITA_CRT_ATOI_FIRST_CALL_RVA != 0x0000a057U ||
        ISAAC_VITA_CRT_ATOI_FIRST_RETURN_RVA != 0x0000a05dU ||
        ISAAC_VITA_CRT_ATOI_DIRECT_CALL_COUNT != 191U ||
        ISAAC_VITA_CRT_ATOI_DIRECT_CALL_FNV64 !=
            UINT64_C(0x12df3796e35d8810) ||
        ISAAC_VITA_CRT_ATOI_REGISTER_LOAD_COUNT != 46U ||
        ISAAC_VITA_CRT_ATOI_REGISTER_LOAD_FNV64 !=
            UINT64_C(0x511eadab81df57e7) ||
        ISAAC_VITA_CRT_ATOI_REGISTER_CALL_COUNT != 102U ||
        ISAAC_VITA_CRT_ATOI_REGISTER_CALL_FNV64 !=
            UINT64_C(0x1326d7c20835a7fd) ||
        ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_COUNT != 293U ||
        ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_FNV64 !=
            UINT64_C(0x6781eec9a7526398) ||
        strcmp(ISAAC_VITA_CRT_ATOF_NAME,
               "api-ms-win-crt-convert-l1-1-0.dll!atof") != 0 ||
        ISAAC_VITA_CRT_ATOF_IAT_RVA != 0x006064dcU ||
        ISAAC_VITA_CRT_ATOF_FIRST_CALL_RVA != 0x004b279dU ||
        ISAAC_VITA_CRT_ATOF_FIRST_RETURN_RVA != 0x004b27a3U ||
        ISAAC_VITA_CRT_ATOF_DIRECT_CALL_COUNT != 160U ||
        ISAAC_VITA_CRT_ATOF_CALL_FNV64 !=
            UINT64_C(0xdbaef5daea0a3908) ||
        ISAAC_VITA_CRT_CONVERT_TEXT_MAX != 4095U ||
        ISAAC_VITA_CRT_CONVERT_IMPORT_COUNT != 6U ||
        ISAAC_VITA_CRT_NUMERIC_CONVERT_IMPORT_COUNT != 4U ||
        ISAAC_VITA_CRT_NUMERIC_CONVERT_CALL_COUNT != 456U)
        return 168;
    if (g_isaac_vita_crt.app_type != 0U ||
        g_isaac_vita_crt.fmode != 0U ||
        g_isaac_vita_crt.commode != 0U ||
        g_isaac_vita_crt.argv_mode != 0U ||
        g_isaac_vita_crt.fp_control != 0x00010000U ||
        g_isaac_vita_crt.thread_locale != 0 ||
        g_isaac_vita_crt_errno != 0 ||
        g_isaac_vita_crt.atexit_count != 0U)
        return 3;

    /* #5: _initterm_e skips NULL entries and stops at the first nonzero
     * initializer result.  Its two arguments remain for cdecl cleanup. */
    s_initializer_table[0] = CALLBACK_ZERO;
    s_initializer_table[1] = 0U;
    s_initializer_table[2] = CALLBACK_NONZERO;
    s_initializer_table[3] = CALLBACK_MUST_NOT_RUN;
    s_initializer_calls = 0U;
    s_must_not_run = 0U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 4);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME))
        return 4;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x37U ||
        s_initializer_calls != 2U || s_must_not_run != 0U ||
        s_initializer_order[0] != CALLBACK_ZERO ||
        s_initializer_order[1] != CALLBACK_NONZERO ||
        s_frame[5] != pointer32(s_initializer_table) ||
        s_frame[6] != pointer32(s_initializer_table + 4))
        return 5;

    /* `_initterm` skips NULL, does not stop on a nonzero EAX, excludes END,
     * and does not manufacture an EAX result of its own. */
    s_initializer_table[0] = 0U;
    s_initializer_table[1] = CALLBACK_NONZERO;
    s_initializer_table[2] = CALLBACK_MUST_NOT_RUN;
    s_initializer_table[3] = CALLBACK_ZERO; /* end-exclusive sentinel */
    s_initializer_calls = 0U;
    s_must_not_run = 0U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 3);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME))
        return 44;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x5a5aa5a5U ||
        s_initializer_calls != 2U || s_must_not_run != 1U ||
        s_initializer_order[0] != CALLBACK_NONZERO ||
        s_initializer_order[1] != CALLBACK_MUST_NOT_RUN)
        return 45;

    /* Empty is a valid range and a void call preserves the incoming EAX. */
    s_initializer_calls = 0U;
    esp = prepare_call(&cpu);
    cpu.eax = 0x13572468U;
    s_frame[5] = pointer32(s_initializer_table + 2);
    s_frame[6] = pointer32(s_initializer_table + 2);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13572468U ||
        s_initializer_calls != 0U)
        return 46;

    /* #6: _set_app_type is cdecl void; EAX is not a return channel. */
    esp = prepare_call(&cpu);
    cpu.eax = 0x13579bdfU;
    s_frame[5] = 1U;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_SET_APP_TYPE_NAME))
        return 6;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        g_isaac_vita_crt.app_type != 1U || s_frame[5] != 1U)
        return 7;

    /* #7: the binary requests _O_TEXT (0x4000); Vita records it and reports
     * errno_t success without inventing a text-mode filesystem transform. */
    esp = prepare_call(&cpu);
    cpu.eax = 0xffffffffU;
    s_frame[5] = 0x4000U;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_SET_FMODE_NAME))
        return 8;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
        g_isaac_vita_crt.fmode != 0x4000U)
        return 9;

    /* #8: __p__commode returns the stable, writable address of host-owned CRT
     * state.  On 32-bit Vita that host address is honestly a guest address. */
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_COMMODE_NAME))
        return 10;
    commode_pointer = pointer32(&g_isaac_vita_crt.commode);
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != commode_pointer ||
        ld32(commode_pointer) != 0U)
        return 11;
    st32(commode_pointer, 0x2468ace0U);
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_COMMODE_NAME) ||
        cpu.esp != esp + 4U || cpu.eax != commode_pointer ||
        ld32(cpu.eax) != 0x2468ace0U)
        return 12;

    /* #9: one guest callback is registered; cdecl leaves its argument. */
    esp = prepare_call(&cpu);
    s_frame[5] = 0x985eb04cU;
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATEXIT_NAME))
        return 13;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
        g_isaac_vita_crt.atexit_count != 1U ||
        g_isaac_vita_crt_atexit[0] != 0x985eb04cU ||
        s_frame[5] != 0x985eb04cU)
        return 14;

    /* #10: measured _crt_argv_unexpanded_arguments is exactly 1. */
    esp = prepare_call(&cpu);
    s_frame[5] = 1U;
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_CONFIGURE_ARGV_NAME))
        return 15;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
        g_isaac_vita_crt.argv_mode != 1U)
        return 16;

    /* #11: the only stdcall in this batch zeroes exactly the eight-byte x86
     * SLIST_HEADER and removes its own four-byte argument. */
    s_slist[1] = 0x11223344U;
    s_slist[2] = 0xffffffffU;
    s_slist[3] = 0xffffffffU;
    s_slist[4] = 0x55667788U;
    esp = prepare_call(&cpu);
    cpu.eax = 0x89abcdefU;
    s_frame[5] = pointer32(&s_slist[2]);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITIALIZE_SLIST_NAME))
        return 17;
    if (cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0x89abcdefU ||
        s_slist[1] != 0x11223344U || s_slist[2] != 0U ||
        s_slist[3] != 0U || s_slist[4] != 0x55667788U)
        return 18;

    /* #12: the real boot request selects 53-bit precision.  The translated
     * x87 already uses C double, so only the guest-visible word changes. */
    esp = prepare_call(&cpu);
    s_frame[5] = 0U;
    s_frame[6] = 0x00010000U;
    s_frame[7] = 0x00030000U;
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_CONTROLFP_S_NAME))
        return 19;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
        g_isaac_vita_crt.fp_control != 0x00010000U)
        return 20;

    /* #13: the measured call disables per-thread locale with argument 0. */
    esp = prepare_call(&cpu);
    s_frame[5] = 0U;
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_crt_import(&cpu,
                               ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME))
        return 21;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
        g_isaac_vita_crt.thread_locale != 0)
        return 22;

    /* The getter must also be safe if reached before the explicit
     * environment initializer.  Its result is a stable char ** whose first
     * entry is the terminating NULL. */
    esp = prepare_call(&cpu);
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_crt_import(
            &cpu, ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_NAME))
        return 51;
    environment_pointer = cpu.eax;
    if (cpu.fault || cpu.esp != esp + 4U || !environment_pointer ||
        ld32(environment_pointer) != 0U)
        return 52;

    /* Match the established PC host contract: __p___argv returns char ***
     * whose stable outer slot points at { "isaac-ng.exe", NULL };
     * __p___argc returns the writable address of the matching count one. */
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_ARGV_NAME))
        return 53;
    argv_variable_pointer = cpu.eax;
    argv_pointer = ld32(argv_variable_pointer);
    argv0_pointer = ld32(argv_pointer);
    if (cpu.fault || cpu.esp != esp + 4U || !argv_variable_pointer ||
        !argv_pointer || !argv0_pointer || ld32(argv_pointer + 4U) != 0U ||
        strcmp((const char *)(uintptr_t)argv0_pointer, "isaac-ng.exe") != 0)
        return 54;
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_ARGC_NAME))
        return 55;
    argc_pointer = cpu.eax;
    if (cpu.fault || cpu.esp != esp + 4U || !argc_pointer ||
        ld32(argc_pointer) != 1U)
        return 56;

    /* These are CRT variables, not freshly manufactured values.  Guest
     * writes remain visible through the same returned addresses. */
    st32(argv_variable_pointer, environment_pointer);
    st32(argc_pointer, 7U);
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_ARGV_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != argv_variable_pointer ||
        ld32(argv_variable_pointer) != environment_pointer)
        return 57;
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_ARGC_NAME) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != argc_pointer ||
        ld32(argc_pointer) != 7U)
        return 58;
    st32(argv_variable_pointer, argv_pointer);
    st32(argc_pointer, 1U);

    /* #14: explicit initialization is idempotent and preserves all three
     * startup-storage addresses prepared by the lazy getter path. */
    esp = prepare_call(&cpu);
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_crt_import(&cpu,
                               ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_NAME))
        return 23;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U)
        return 24;
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(
            &cpu, ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != environment_pointer || ld32(cpu.eax) != 0U)
        return 59;
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_ARGV_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != argv_variable_pointer || ld32(cpu.eax) != argv_pointer ||
        ld32(argv_pointer) != argv0_pointer ||
        ld32(argv_pointer + 4U) != 0U)
        return 60;
    esp = prepare_call(&cpu);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_P_ARGC_NAME) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != argc_pointer ||
        ld32(cpu.eax) != 1U)
        return 61;

    /* Unknown names are not swallowed: return zero without changing CPU or
     * CRT state so guest.c can issue the exact DLL!symbol fault. */
    esp = prepare_call(&cpu);
    cpu.eax = 0xa1b2c3d4U;
    cpu.ecx = 0x10203040U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    memcpy(&state_snapshot, &g_isaac_vita_crt, sizeof state_snapshot);
    guest_errno_snapshot = g_isaac_vita_crt_errno;
    if (isaac_vita_crt_import(
            &cpu, "api-ms-win-crt-runtime-l1-1-0.dll!not_a_real_import") != 0)
        return 25;
    if (memcmp(&cpu, &snapshot, sizeof cpu) != 0 ||
        memcmp(&g_isaac_vita_crt, &state_snapshot, sizeof state_snapshot) != 0 ||
        g_isaac_vita_crt_errno != guest_errno_snapshot ||
        cpu.esp != esp)
        return 26;

    /* Querying _controlfp_s through a non-NULL guest pointer returns the
     * tracked word and leaves guard dwords untouched. */
    s_output[0] = 0x01020304U;
    s_output[1] = 0xccccccccU;
    s_output[2] = 0x50607080U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(&s_output[1]);
    s_frame[6] = 0U;
    s_frame[7] = 0U;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_CONTROLFP_S_NAME))
        return 27;
    if (cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
        s_output[0] != 0x01020304U || s_output[1] != 0x00010000U ||
        s_output[2] != 0x50607080U)
        return 28;

    /* Enable, query (-1), and disable preserve the documented previous-value
     * return channel used by current host_win32.c. */
    esp = prepare_call(&cpu);
    s_frame[5] = 1U;
    if (!isaac_vita_crt_import(&cpu,
                               ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME) ||
        cpu.esp != esp + 4U || cpu.eax != 0U ||
        g_isaac_vita_crt.thread_locale != 1)
        return 29;
    esp = prepare_call(&cpu);
    s_frame[5] = UINT32_MAX;
    if (!isaac_vita_crt_import(&cpu,
                               ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME) ||
        cpu.esp != esp + 4U || cpu.eax != 1U ||
        g_isaac_vita_crt.thread_locale != 1)
        return 30;
    esp = prepare_call(&cpu);
    s_frame[5] = 0U;
    if (!isaac_vita_crt_import(&cpu,
                               ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME) ||
        cpu.esp != esp + 4U || cpu.eax != 1U ||
        g_isaac_vita_crt.thread_locale != 0)
        return 31;

    /* Invalid ranges fault before touching the table or import frame. */
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table + 1);
    s_frame[6] = pointer32(s_initializer_table);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME))
        return 32;
    if (cpu.esp != esp ||
        cpu.fault_addr != pointer32(s_initializer_table + 1) ||
        !cpu.fault || strcmp(cpu.fault, "_initterm_e invalid guest range") != 0)
        return 33;

    /* A nested guest fault survives unchanged.  In this returning oracle the
     * synthetic RET remains on ESP; production unwinds from that snapshot. */
    s_initializer_table[0] = CALLBACK_NESTED_FAULT;
    s_initializer_table[1] = CALLBACK_MUST_NOT_RUN;
    s_initializer_calls = 0U;
    s_must_not_run = 0U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 2);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME))
        return 34;
    if (cpu.esp != esp - 4U || cpu.fault_addr != CALLBACK_NESTED_FAULT ||
        !cpu.fault || strcmp(cpu.fault, "oracle nested initializer fault") != 0 ||
        s_initializer_calls != 1U || s_must_not_run != 0U)
        return 35;

    /* A callback that returns without restoring its cdecl stack becomes an
     * immediate, attributed fault rather than latent stack corruption. */
    s_initializer_table[0] = CALLBACK_BAD_STACK;
    s_initializer_calls = 0U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 1);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME))
        return 36;
    if (cpu.esp != esp - 4U || cpu.fault_addr != CALLBACK_BAD_STACK ||
        !cpu.fault ||
        strcmp(cpu.fault, "CRT callback did not restore its cdecl stack") != 0)
        return 37;

    /* Fill the exact bounded atexit table, then prove the 257th registration
     * is loud and does not pop or overwrite the caller's frame. */
    for (i = 1U; i < ISAAC_VITA_CRT_ATEXIT_MAX; ++i) {
        esp = prepare_call(&cpu);
        s_frame[5] = 0x20000000U + i * 4U;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATEXIT_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
            g_isaac_vita_crt.atexit_count != i + 1U ||
            g_isaac_vita_crt_atexit[i] != 0x20000000U + i * 4U)
            return 38;
    }
    esp = prepare_call(&cpu);
    s_frame[5] = 0x2fffffffU;
    cpu.eax = 0xfeedfaceU;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATEXIT_NAME))
        return 39;
    if (cpu.esp != esp || cpu.eax != 0xfeedfaceU ||
        cpu.fault_addr != 0x2fffffffU || !cpu.fault ||
        strcmp(cpu.fault, "_crt_atexit table full") != 0 ||
        g_isaac_vita_crt.atexit_count != ISAAC_VITA_CRT_ATEXIT_MAX ||
        g_isaac_vita_crt_atexit[ISAAC_VITA_CRT_ATEXIT_MAX - 1U] !=
             0x20000000U + (ISAAC_VITA_CRT_ATEXIT_MAX - 1U) * 4U)
        return 40;

    /* Reproduce the real nesting shape: _initterm_e is still active while
     * imports #6..#14 execute in its guest initializer.  The exact next
     * missing import faults before _initterm_e can return, so its count must
     * already be visible. */
    g_isaac_vita_crt = (isaac_vita_crt_state) {
        0U, 0U, 0U, 0U, 0x00010000U, 0, 0U
    };
    g_isaac_vita_crt_errno = 0;
    memset(g_isaac_vita_crt_atexit, 0, sizeof g_isaac_vita_crt_atexit);
    memset(s_slist, 0xcc, sizeof s_slist);
    s_initializer_table[0] = CALLBACK_BOOT_CHAIN;
    s_initializer_calls = 0U;
    s_boot_chain_status = 0U;
    g_host_import_calls = ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT;
    g_host_dynamic_calls = 0U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 1);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME))
        return 42;
    if (s_boot_chain_status != 0U ||
        g_host_import_calls != ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT +
                               ISAAC_VITA_CRT_PRE_MEMORY_CALL_COUNT ||
        g_host_dynamic_calls != 0U || s_initializer_calls != 1U ||
        s_initializer_order[0] != CALLBACK_BOOT_CHAIN ||
        cpu.fault_addr != GUEST_IMAGE_BASE + ISAAC_VITA_CRT_NEXT_IAT_RVA ||
        !cpu.fault || strcmp(cpu.fault, ISAAC_VITA_CRT_NEXT_NAME) != 0 ||
        cpu.esp != pointer32(&s_boot_frame[2]) ||
        ld32(cpu.esp) != ISAAC_VITA_CRT_NEXT_RETURN_RVA ||
        g_isaac_vita_crt.atexit_count != 1U ||
        s_sync_delegate_calls != 1U || s_memory_delegate_calls != 1U ||
        s_console_delegate_calls != 1U)
        return 43;

    /* The counted production edge owns `_initterm` before a nested callback
     * fault prevents its native activation from returning. */
    s_initializer_table[0] = CALLBACK_ZERO;
    s_initializer_table[1] = CALLBACK_NESTED_FAULT;
    s_initializer_table[2] = CALLBACK_MUST_NOT_RUN;
    s_initializer_calls = 0U;
    s_must_not_run = 0U;
    g_host_import_calls = 70U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 3);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME))
        return 47;
    if (g_host_import_calls != 71U ||
        cpu.esp != esp - 4U || cpu.fault_addr != CALLBACK_NESTED_FAULT ||
        !cpu.fault || strcmp(cpu.fault,
                            "oracle nested initializer fault") != 0 ||
        s_initializer_calls != 2U || s_must_not_run != 0U ||
        s_initializer_order[0] != CALLBACK_ZERO ||
        s_initializer_order[1] != CALLBACK_NESTED_FAULT)
        return 48;

    /* Reversed and misaligned `_initterm` ranges fail before table access. */
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table + 1);
    s_frame[6] = pointer32(s_initializer_table);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME) ||
        cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault, "_initterm invalid guest range") != 0)
        return 49;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table) + 3U;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME) ||
        cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault, "_initterm invalid guest range") != 0)
        return 50;

    /* The live chain reaches these after Steam callback #255 and seven
     * `_crt_atexit` calls #256..#262.  Count at the production dispatch edge
     * before each cdecl handler and prove the exact getter -> argv -> argc
     * order that leads into main. */
    g_host_import_calls = 262U;
    esp = prepare_call(&cpu);
    if (!guest_host_import(
            &cpu, ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        g_host_import_calls !=
            ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_BOOT_ORDINAL ||
        cpu.eax != environment_pointer || ld32(cpu.eax) != 0U)
        return 62;
    esp = prepare_call(&cpu);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_P_ARGV_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        g_host_import_calls != ISAAC_VITA_CRT_P_ARGV_BOOT_ORDINAL ||
        cpu.eax != argv_variable_pointer || ld32(cpu.eax) != argv_pointer ||
        ld32(argv_pointer) != argv0_pointer ||
        ld32(argv_pointer + 4U) != 0U ||
        strcmp((const char *)(uintptr_t)argv0_pointer, "isaac-ng.exe") != 0)
        return 63;
    esp = prepare_call(&cpu);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_P_ARGC_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        g_host_import_calls != ISAAC_VITA_CRT_P_ARGC_BOOT_ORDINAL ||
        cpu.eax != argc_pointer || ld32(cpu.eax) != 1U)
        return 64;

    /* UCRT's C locale preserves every byte numerically through U+00xx.  Pin
     * the non-ASCII edge explicitly so neither side can drift to an ASCII or
     * host-ACP policy. */
    {
        static const uint8_t identity_bytes[] = {
            0x80U, 0x81U, 0x9fU, 0xc0U, 0xffU, 0U
        };
        static const uint16_t identity_wide[] = {
            0x0080U, 0x0081U, 0x009fU, 0x00c0U, 0x00ffU, 0U
        };
        static const char measured[] = "first-arm-fault.log";
        uint32_t converted_pointer = pointer32(&s_output[1]);
        uint32_t source_pointer = pointer32(s_wide_source);
        uint32_t dest_pointer = pointer32(s_narrow_output);
        uint32_t length = (uint32_t)strlen(measured);

        memcpy(s_narrow_output, identity_bytes, sizeof identity_bytes);
        memset(s_wide_source, 0xcc, sizeof s_wide_source);
        s_output[1] = 0xccccccccU;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = source_pointer;
        s_frame[7] = 32U;
        s_frame[8] = dest_pointer;
        s_frame[9] = 32U;
        if (!isaac_vita_crt_import(&cpu,
                                   ISAAC_VITA_CRT_MBSTOWCS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
            s_output[1] != sizeof identity_bytes ||
            memcmp(s_wide_source, identity_wide,
                   sizeof identity_wide) != 0)
            return 187;

        memset(s_narrow_output, 0xcc, sizeof s_narrow_output);
        s_output[1] = 0xccccccccU;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = dest_pointer;
        s_frame[7] = 32U;
        s_frame[8] = source_pointer;
        s_frame[9] = 32U;
        if (!isaac_vita_crt_import(&cpu,
                                   ISAAC_VITA_CRT_WCSTOMBS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
            s_output[1] != sizeof identity_bytes ||
            memcmp(s_narrow_output, identity_bytes,
                   sizeof identity_bytes) != 0)
            return 188;

        /* #347 and every yielded FindFile record share wcstombs_s.  Pin the
         * measured ASCII result plus UCRT query, ERANGE, EILSEQ, and
         * _TRUNCATE behavior without introducing a second implementation. */
        for (i = 0U; i <= length; ++i)
            s_wide_source[i] = (uint16_t)(uint8_t)measured[i];
        memset(s_narrow_output, 0xcc, sizeof s_narrow_output);
        s_output[1] = 0xccccccccU;
        g_host_import_calls = 346U;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = dest_pointer;
        s_frame[7] = ISAAC_VITA_CRT_WCSTOMBS_S_CAPACITY;
        s_frame[8] = source_pointer;
        s_frame[9] = ISAAC_VITA_CRT_WCSTOMBS_S_CAPACITY;
        if (!guest_host_import(&cpu, ISAAC_VITA_CRT_WCSTOMBS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
            g_host_import_calls !=
                ISAAC_VITA_CRT_WCSTOMBS_S_FIRST_ORDINAL ||
            s_output[1] != length + 1U ||
            strcmp((const char *)s_narrow_output, measured) != 0)
            return 65;

        s_output[1] = 0xccccccccU;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = 0U;
        s_frame[7] = 0U;
        s_frame[8] = source_pointer;
        s_frame[9] = 0U; /* UCRT query mode ignores count. */
        if (!isaac_vita_crt_import(&cpu,
                                   ISAAC_VITA_CRT_WCSTOMBS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U ||
            s_output[1] != length + 1U)
            return 66;

        memset(s_narrow_output, 0xcc, sizeof s_narrow_output);
        s_output[1] = 0xccccccccU;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = dest_pointer;
        s_frame[7] = 4U;
        s_frame[8] = source_pointer;
        s_frame[9] = length;
        if (!isaac_vita_crt_import(&cpu,
                                   ISAAC_VITA_CRT_WCSTOMBS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != ERANGE ||
            s_output[1] != 0U || s_narrow_output[0] != 0U ||
            s_narrow_output[1] != 0xccU)
            return 67;

        s_wide_source[0] = (uint16_t)'A';
        s_wide_source[1] = 0x0100U;
        s_wide_source[2] = 0U;
        memset(s_narrow_output, 0xcc, sizeof s_narrow_output);
        s_output[1] = 0xccccccccU;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = dest_pointer;
        s_frame[7] = 8U;
        s_frame[8] = source_pointer;
        s_frame[9] = 7U;
        if (!isaac_vita_crt_import(&cpu,
                                   ISAAC_VITA_CRT_WCSTOMBS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != EILSEQ ||
            s_output[1] != 0U || s_narrow_output[0] != 0U)
            return 68;

        for (i = 0U; i <= 9U; ++i)
            s_wide_source[i] = (uint16_t)(uint8_t)"Documents"[i];
        memset(s_narrow_output, 0xcc, sizeof s_narrow_output);
        s_output[1] = 0xccccccccU;
        esp = prepare_call(&cpu);
        s_frame[5] = converted_pointer;
        s_frame[6] = dest_pointer;
        s_frame[7] = 5U;
        s_frame[8] = source_pointer;
        s_frame[9] = UINT32_MAX;
        if (!isaac_vita_crt_import(&cpu,
                                   ISAAC_VITA_CRT_WCSTOMBS_S_NAME) ||
            cpu.fault || cpu.esp != esp + 4U ||
            cpu.eax != ISAAC_VITA_CRT_WCSTOMBS_S_STRUNCATE ||
            s_output[1] != 5U ||
            memcmp(s_narrow_output, "Docu", 5U) != 0 ||
            s_narrow_output[5] != 0xccU)
            return 69;
    }

    /* Attempt #445 is the first secure formatter call reached after the
     * recursive FindFile walk.  Reproduce its exact seven-dword UCRT ABI and
     * measured path join, then pin the secure overflow/invalid reset rule. */
    s_secure_varargs[0] = pointer32(s_secure_left);
    s_secure_varargs[1] = pointer32(s_secure_right);
    memset(s_secure_output, 0xcc, sizeof s_secure_output);
    g_host_import_calls = ISAAC_VITA_CRT_VSPRINTF_S_FIRST_ORDINAL - 1U;
    esp = prepare_call(&cpu);
    s_frame[5] = 2U; /* exact local UCRT secure-format option word */
    s_frame[6] = 0U;
    s_frame[7] = pointer32(s_secure_output);
    s_frame[8] = ISAAC_VITA_CRT_VSPRINTF_S_FIRST_CAPACITY;
    s_frame[9] = pointer32(s_secure_format);
    s_frame[10] = 0U;
    s_frame[11] = pointer32(s_secure_varargs);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_VSPRINTF_S_NAME) ||
        cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != ISAAC_VITA_CRT_VSPRINTF_S_FIRST_CAPACITY - 1U ||
        g_host_import_calls != ISAAC_VITA_CRT_VSPRINTF_S_FIRST_ORDINAL ||
        strcmp((const char *)s_secure_output,
               ISAAC_VITA_CRT_VSPRINTF_S_FIRST_RESULT) != 0 ||
        s_secure_output[ISAAC_VITA_CRT_VSPRINTF_S_FIRST_CAPACITY] != 0xccU)
        return 70;

    memset(s_secure_output, 0xcc, sizeof s_secure_output);
    esp = prepare_call(&cpu);
    s_frame[5] = 2U;
    s_frame[6] = 0U;
    s_frame[7] = pointer32(s_secure_output);
    s_frame[8] = ISAAC_VITA_CRT_VSPRINTF_S_FIRST_CAPACITY - 1U;
    s_frame[9] = pointer32(s_secure_format);
    s_frame[10] = 0U;
    s_frame[11] = pointer32(s_secure_varargs);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_VSPRINTF_S_NAME) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != UINT32_MAX ||
        s_secure_output[0] != 0U)
        return 71;

    memset(s_secure_output, 0xcc, sizeof s_secure_output);
    esp = prepare_call(&cpu);
    s_frame[5] = 2U;
    s_frame[6] = 0U;
    s_frame[7] = pointer32(s_secure_output);
    s_frame[8] = ISAAC_VITA_CRT_VSPRINTF_S_FIRST_CAPACITY;
    s_frame[9] = 0U;
    s_frame[10] = 0U;
    s_frame[11] = pointer32(s_secure_varargs);
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_VSPRINTF_S_NAME) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != UINT32_MAX ||
        s_secure_output[0] != 0U || s_secure_output[1] != 0xccU)
        return 72;

    /* The full 14-slot narrow-string family is one guest-pointer/cdecl
     * boundary.  Exercise the eleven newly connected handlers through the
     * production dispatch, including unsigned-byte comparisons, C-locale
     * classification bits, exact returned guest pointers and secure reset
     * behavior. */
    {
        uint32_t arguments[4];
        uint32_t result;
        unsigned counted;

        s_string_left[0] = 0xffU;
        s_string_left[1] = 0U;
        s_string_right[0] = 0x7fU;
        s_string_right[1] = 0U;
        arguments[0] = pointer32(s_string_left);
        arguments[1] = pointer32(s_string_right);
        arguments[2] = 1U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRNCMP_NAME,
                            arguments, 3U, &result) || result != 0x80U)
            return 73;

        memcpy(s_string_left, "A@", 3U);
        memcpy(s_string_right, "a[", 3U);
        arguments[2] = 2U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRNICMP_NAME,
                            arguments, 3U, &result) ||
            result != (uint32_t)(int32_t)-27)
            return 74;

        memcpy(s_string_left, "abc/def", 8U);
        memcpy(s_string_right, "/x", 3U);
        arguments[0] = pointer32(s_string_left);
        arguments[1] = pointer32(s_string_right);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRPBRK_NAME,
                            arguments, 2U, &result) ||
            result != pointer32(s_string_left + 3U))
            return 75;

        arguments[0] = (uint32_t)'9';
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_ISDIGIT_NAME,
                            arguments, 1U, &result) || result != 0x04U)
            return 76;
        arguments[0] = (uint32_t)'/';
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_ISPUNCT_NAME,
                            arguments, 1U, &result) || result != 0x10U)
            return 77;
        arguments[0] = (uint32_t)'\n';
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_ISSPACE_NAME,
                            arguments, 1U, &result) || result != 0x08U)
            return 78;
        arguments[0] = 0x2007U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_ISWSPACE_NAME,
                            arguments, 1U, &result) || result != 0x08U)
            return 79;
        arguments[0] = (uint32_t)'A';
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_TOLOWER_NAME,
                            arguments, 1U, &result) ||
            result != (uint32_t)'a')
            return 80;
        arguments[0] = (uint32_t)'z';
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_TOUPPER_NAME,
                            arguments, 1U, &result) ||
            result != (uint32_t)'Z')
            return 81;

        memset(s_string_output, 0xcc, sizeof s_string_output);
        memcpy(s_string_left, "copy", 5U);
        arguments[0] = pointer32(s_string_output);
        arguments[1] = sizeof s_string_output;
        arguments[2] = pointer32(s_string_left);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRCPY_S_NAME,
                            arguments, 3U, &result) || result != 0U ||
            strcmp((const char *)s_string_output, "copy") != 0 ||
            s_string_output[5] != 0xccU)
            return 82;

        memcpy(s_string_output, "ab", 3U);
        memcpy(s_string_left, "cd", 3U);
        arguments[0] = pointer32(s_string_output);
        arguments[1] = sizeof s_string_output;
        arguments[2] = pointer32(s_string_left);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRCAT_S_NAME,
                            arguments, 3U, &result) || result != 0U ||
            strcmp((const char *)s_string_output, "abcd") != 0)
            return 83;

        /* A handler fault cannot hide its dynamic occurrence: dispatch owns
         * the count immediately after exact-name match, before dereference. */
        counted = 549U;
        esp = prepare_call(&cpu);
        s_frame[5] = 0U;
        s_frame[6] = pointer32(s_string_right);
        s_frame[7] = 1U;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_STRNCMP_NAME, &counted) ||
            counted != 550U || cpu.esp != esp || cpu.fault_addr != 0U ||
            !cpu.fault || strcmp(cpu.fault,
                                 "strncmp received a null guest pointer") != 0)
            return 84;

        arguments[0] = 0U;
        arguments[1] = 0U;
        arguments[2] = 0U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRNCMP_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 85;
    }

    /* The frozen PE has one scanf-family import, not one format.  Exercise
     * every statically measured format through the production seven-dword
     * UCRT boundary.  Destination pointers live in a consecutive x86 guest
     * va_list; no native ARM va_list participates. */
    {
        uint32_t arguments[7];
        uint32_t result;
        uint32_t esp;
        unsigned counted = ISAAC_VITA_CRT_VSSCANF_FIRST_BEFORE_COUNT;

        arguments[0] = 2U; /* _CRT_INTERNAL_SCANF_LEGACY_WIDE_SPECIFIERS */
        arguments[1] = 0U;
        arguments[2] = pointer32(s_scan_version_input);
        arguments[3] = UINT32_MAX;
        arguments[4] = pointer32(s_scan_version_format);
        arguments[5] = 0U;
        arguments[6] = pointer32(s_scan_varargs);
        s_scan_outputs[0] = UINT32_MAX;
        s_scan_outputs[1] = UINT32_MAX;
        s_scan_varargs[0] = pointer32(&s_scan_outputs[0]);
        s_scan_varargs[1] = pointer32(&s_scan_outputs[1]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_VSSCANF_NAME,
                            arguments, 7U, &result) || result != 2U ||
            s_scan_outputs[0] != 2U || s_scan_outputs[1] != 0U ||
            s_scan_varargs[0] != pointer32(&s_scan_outputs[0]) ||
            s_scan_varargs[1] != pointer32(&s_scan_outputs[1]))
            return 86;

        arguments[0] = 0U;
        arguments[2] = pointer32(s_scan_shader_input);
        arguments[4] = pointer32(s_scan_shader_format);
        s_scan_outputs[0] = 0U;
        s_scan_varargs[0] = pointer32(&s_scan_outputs[0]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_VSSCANF_NAME,
                            arguments, 7U, &result) || result != 1U ||
            s_scan_outputs[0] != UINT32_MAX)
            return 87;

        arguments[2] = pointer32(s_scan_triple_input);
        arguments[4] = pointer32(s_scan_triple_format);
        s_scan_outputs[0] = UINT32_MAX;
        s_scan_outputs[1] = UINT32_MAX;
        s_scan_outputs[2] = UINT32_MAX;
        s_scan_varargs[0] = pointer32(&s_scan_outputs[0]);
        s_scan_varargs[1] = pointer32(&s_scan_outputs[1]);
        s_scan_varargs[2] = pointer32(&s_scan_outputs[2]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_VSSCANF_NAME,
                            arguments, 7U, &result) || result != 3U ||
            s_scan_outputs[0] != (uint32_t)(int32_t)-3 ||
            s_scan_outputs[1] != 12U || s_scan_outputs[2] != 0U)
            return 88;

        arguments[2] = pointer32(s_scan_partial_input);
        arguments[4] = pointer32(s_scan_version_format);
        s_scan_outputs[0] = UINT32_MAX;
        s_scan_outputs[1] = UINT32_MAX;
        s_scan_varargs[0] = pointer32(&s_scan_outputs[0]);
        s_scan_varargs[1] = pointer32(&s_scan_outputs[1]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_VSSCANF_NAME,
                            arguments, 7U, &result) || result != 1U ||
            s_scan_outputs[0] != 7U || s_scan_outputs[1] != UINT32_MAX)
            return 89;

        /* Count-before-handler is the live contract: the previously unknown
         * frontier left the total at 563; an exact-name handler increments it
         * to occurrence 564 before an unsupported format faults. */
        esp = prepare_call(&cpu);
        s_frame[5] = 0U;
        s_frame[6] = 0U;
        s_frame[7] = pointer32(s_scan_version_input);
        s_frame[8] = UINT32_MAX;
        s_frame[9] = pointer32(s_scan_unknown_format);
        s_frame[10] = 0U;
        s_frame[11] = pointer32(s_scan_varargs);
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_VSSCANF_NAME, &counted) ||
            counted != ISAAC_VITA_CRT_VSSCANF_FIRST_ORDINAL ||
            !cpu.fault ||
            strcmp(cpu.fault, "guest stdio unsupported scan format") != 0 ||
            cpu.fault_addr != pointer32(s_scan_unknown_format) ||
            cpu.esp != esp)
            return 90;

        /* A parsed value still cannot turn a null guest destination into a
         * native pointer.  The fault identifies the exact va_list dword and
         * leaves the cdecl frame intact. */
        esp = prepare_call(&cpu);
        s_scan_varargs[0] = 0U;
        s_frame[5] = 0U;
        s_frame[6] = 0U;
        s_frame[7] = pointer32(s_scan_shader_input);
        s_frame[8] = UINT32_MAX;
        s_frame[9] = pointer32(s_scan_shader_format);
        s_frame[10] = 0U;
        s_frame[11] = pointer32(s_scan_varargs);
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_VSSCANF_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "guest stdio scan has a null destination") != 0 ||
            cpu.fault_addr != pointer32(s_scan_varargs) || cpu.esp != esp)
            return 91;
    }

    /* The frozen PE imports the two-member errno family, not merely the
     * setter first encountered on the live path.  `_errno` returns stable,
     * writable guest-visible storage; `_set_errno` updates that exact cell,
     * returns errno_t success, and leaves its one cdecl argument for the
     * translated caller.  Native newlib errno stays private throughout. */
    {
        static const char *const absent_doserrno_names[] = {
            "api-ms-win-crt-runtime-l1-1-0.dll!__doserrno",
            "api-ms-win-crt-runtime-l1-1-0.dll!_get_doserrno",
            "api-ms-win-crt-runtime-l1-1-0.dll!_set_doserrno"
        };
        uint32_t arguments[1];
        uint32_t errno_pointer;
        uint32_t result;
        unsigned counted = 700U;
        int saved_host_errno = errno;

        errno = EACCES;
        esp = prepare_call(&cpu);
        cpu.eax = 0xfeedfaceU;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_ERRNO_NAME, &counted) ||
            counted != 701U || cpu.fault || cpu.esp != esp + 4U ||
            cpu.eax != pointer32(&g_isaac_vita_crt_errno) ||
            ld32(cpu.eax) != 0U || errno != EACCES)
            return 92;
        errno_pointer = cpu.eax;

        st32(errno_pointer, ISAAC_VITA_CRT_ERRNO_CALL1_STORE_VALUE);
        if (g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 93;
        esp = prepare_call(&cpu);
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ERRNO_NAME) ||
            cpu.fault || cpu.esp != esp + 4U ||
            cpu.eax != errno_pointer ||
            ld32(cpu.eax) != ISAAC_VITA_CRT_ERRNO_CALL1_STORE_VALUE ||
            errno != EACCES)
            return 94;

        arguments[0] = ISAAC_VITA_CRT_SET_ERRNO_ENOENT_VALUE;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_SET_ERRNO_NAME,
                            arguments, 1U, &result) || result != 0U ||
            s_frame[5] != ISAAC_VITA_CRT_SET_ERRNO_ENOENT_VALUE ||
            g_isaac_vita_crt_errno != ENOENT ||
            ld32(errno_pointer) != ISAAC_VITA_CRT_SET_ERRNO_ENOENT_VALUE ||
            errno != EACCES)
            return 95;

        esp = prepare_call(&cpu);
        s_frame[5] = ISAAC_VITA_CRT_SET_ERRNO_EBADF_VALUE;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_SET_ERRNO_NAME, &counted) ||
            counted != 702U || cpu.fault || cpu.esp != esp + 4U ||
            cpu.eax != 0U ||
            s_frame[5] != ISAAC_VITA_CRT_SET_ERRNO_EBADF_VALUE ||
            g_isaac_vita_crt_errno != EBADF ||
            ld32(errno_pointer) != ISAAC_VITA_CRT_SET_ERRNO_EBADF_VALUE ||
            errno != EACCES)
            return 96;

        st32(errno_pointer, ISAAC_VITA_CRT_ERRNO_CALL2_STORE_VALUE);
        if (g_isaac_vita_crt_errno != ENOMEM || errno != EACCES)
            return 97;

        /* `_set_errno` accepts an int, not a curated POSIX subset.  Preserve
         * the complete x86 value rather than translating through newlib. */
        arguments[0] = 0x87654321U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_SET_ERRNO_NAME,
                            arguments, 1U, &result) || result != 0U ||
            (uint32_t)g_isaac_vita_crt_errno != arguments[0] ||
            ld32(errno_pointer) != arguments[0] || errno != EACCES)
            return 98;

        /* The expensive negative was checked against all 413 PE slots:
         * there is no doserrno import.  Keep those names as mutation-free
         * handoffs instead of inventing an unused second state cell. */
        for (i = 0U; i < sizeof absent_doserrno_names /
                         sizeof absent_doserrno_names[0]; ++i) {
            unsigned absent_count = 702U;

            esp = prepare_call(&cpu);
            cpu.eax = 0xa1b2c3d4U;
            cpu.ecx = 0x10203040U;
            memcpy(&snapshot, &cpu, sizeof snapshot);
            memcpy(&state_snapshot, &g_isaac_vita_crt,
                   sizeof state_snapshot);
            guest_errno_snapshot = g_isaac_vita_crt_errno;
            if (isaac_vita_crt_import_counted(
                    &cpu, absent_doserrno_names[i], &absent_count) != 0 ||
                absent_count != 702U ||
                memcmp(&cpu, &snapshot, sizeof cpu) != 0 ||
                memcmp(&g_isaac_vita_crt, &state_snapshot,
                       sizeof state_snapshot) != 0 ||
                g_isaac_vita_crt_errno != guest_errno_snapshot ||
                cpu.esp != esp)
                return 99;
        }
        errno = saved_host_errno;
    }

    /* Complete five-import time family.  The input is the leap-day instant
     * 2000-02-29 12:34:56 UTC used by the independent PC oracle.  Exercise
     * the x86 EDX:EAX return, optional eight-byte write, shared tm9 cell,
     * normalisation, console-local conversion, C locale, errors, and native
     * errno isolation through the production handlers. */
    {
        static const uint32_t expected_tm[ISAAC_VITA_CRT_TM_DWORD_COUNT] = {
            56U, 34U, 12U, 29U, 1U, 100U, 2U, 59U, 0U
        };
        static const char formatted_expected[] = "Tue, 02/29/2000";
        uint32_t arguments[4];
        uint32_t result;
        uint32_t gmtime_pointer;
        uint32_t localtime_pointer;
        uint64_t returned;
        uint64_t written;
        unsigned counted = ISAAC_VITA_CRT_TIME64_FRONTIER_BEFORE_COUNT;
        int saved_host_errno = errno;

        s_test_filetime = UINT64_C(0x01bf82b162519800);
        s_test_filetime_result = 0;
        errno = EACCES;

        /* All three PE callers pass NULL.  Pin the live second site and the
         * count-before-handler rule: actual=2774 becomes occurrence #2775. */
        esp = prepare_call(&cpu);
        s_frame[5] = 0U;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_TIME64_NAME, &counted) ||
            counted != ISAAC_VITA_CRT_TIME64_FRONTIER_ORDINAL ||
            cpu.fault || cpu.esp != esp + 4U ||
            cpu.eax != 951827696U || cpu.edx != 0U ||
            s_frame[5] != 0U || errno != EACCES)
            return 101;

        s_time_written[0] = 0x13579bdfU;
        s_time_written[1] = 0xccccccccU;
        s_time_written[2] = 0xccccccccU;
        s_time_written[3] = 0x2468ace0U;
        arguments[0] = pointer32(&s_time_written[1]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_TIME64_NAME,
                            arguments, 1U, &result))
            return 102;
        returned = (uint64_t)result | ((uint64_t)cpu.edx << 32U);
        written = (uint64_t)s_time_written[1] |
                  ((uint64_t)s_time_written[2] << 32U);
        if (returned != UINT64_C(951827696) || written != returned ||
            s_time_written[0] != 0x13579bdfU ||
            s_time_written[3] != 0x2468ace0U || errno != EACCES)
            return 103;

        /* Provider failure maps to signed -1 in both result channels and to
         * guest EINVAL, while native errno and surrounding dwords survive. */
        s_test_filetime_result = -1;
        g_isaac_vita_crt_errno = ENOMEM;
        s_time_written[1] = 0U;
        s_time_written[2] = 0U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_TIME64_NAME,
                            arguments, 1U, &result) ||
            result != UINT32_MAX || cpu.edx != UINT32_MAX ||
            s_time_written[1] != UINT32_MAX ||
            s_time_written[2] != UINT32_MAX ||
            s_time_written[0] != 0x13579bdfU ||
            s_time_written[3] != 0x2468ace0U ||
            g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 104;
        s_test_filetime_result = 0;

        arguments[0] = pointer32(s_time_known);
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GMTIME64_NAME,
                            arguments, 1U, &gmtime_pointer) ||
            !gmtime_pointer || g_isaac_vita_crt_errno != ENOMEM ||
            errno != EACCES)
            return 105;
        for (i = 0U; i < ISAAC_VITA_CRT_TM_DWORD_COUNT; ++i)
            if (ld32(gmtime_pointer + i * 4U) != expected_tm[i])
                return 106;

        /* March day zero plus 33:116 is the same leap-day instant.  UCRT
         * rewrites all nine fields to their canonical values on success. */
        s_time_tm_guarded[0] = 0x10203040U;
        s_time_tm_guarded[1] = 116U;
        s_time_tm_guarded[2] = 33U;
        s_time_tm_guarded[3] = 12U;
        s_time_tm_guarded[4] = 0U;
        s_time_tm_guarded[5] = 2U;
        s_time_tm_guarded[6] = 100U;
        s_time_tm_guarded[7] = 0xccccccccU;
        s_time_tm_guarded[8] = 0xddddddddU;
        s_time_tm_guarded[9] = 0xeeeeeeeeU;
        s_time_tm_guarded[10] = 0x50607080U;
        arguments[0] = pointer32(&s_time_tm_guarded[1]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_MKGMTIME64_NAME,
                            arguments, 1U, &result))
            return 107;
        returned = (uint64_t)result | ((uint64_t)cpu.edx << 32U);
        if (returned != UINT64_C(951827696) ||
            s_time_tm_guarded[0] != 0x10203040U ||
            s_time_tm_guarded[10] != 0x50607080U || errno != EACCES)
            return 108;
        for (i = 0U; i < ISAAC_VITA_CRT_TM_DWORD_COUNT; ++i)
            if (s_time_tm_guarded[i + 1U] != expected_tm[i])
                return 109;

        /* The actual local fields depend on the console setting.  The ABI
         * facts do not: success, a valid tm, and the same shared pointer. */
        arguments[0] = pointer32(s_time_known);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_LOCALTIME64_NAME,
                            arguments, 1U, &localtime_pointer) ||
            localtime_pointer != gmtime_pointer || !localtime_pointer ||
            (int32_t)ld32(localtime_pointer + 0U) < 0 ||
            ld32(localtime_pointer + 0U) > 60U ||
            ld32(localtime_pointer + 4U) > 59U ||
            ld32(localtime_pointer + 8U) > 23U ||
            ld32(localtime_pointer + 12U) < 1U ||
            ld32(localtime_pointer + 12U) > 31U ||
            ld32(localtime_pointer + 16U) > 11U ||
            ld32(localtime_pointer + 24U) > 6U ||
            ld32(localtime_pointer + 28U) > 365U ||
            ld32(localtime_pointer + 32U) > 1U || errno != EACCES)
            return 110;

        /* Restore deterministic UTC data in the shared cell, then exercise
         * the exact sole PE format and capacity in the initial C locale. */
        arguments[0] = pointer32(s_time_known);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GMTIME64_NAME,
                            arguments, 1U, &result) ||
            result != gmtime_pointer)
            return 111;
        memset(s_time_formatted, 0xcc, sizeof s_time_formatted);
        arguments[0] = pointer32(s_time_formatted);
        arguments[1] = ISAAC_VITA_CRT_STRFTIME_CAPACITY;
        arguments[2] = pointer32(s_time_format);
        arguments[3] = gmtime_pointer;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRFTIME_NAME,
                            arguments, 4U, &result) ||
            result != sizeof formatted_expected - 1U ||
            strcmp(s_time_formatted, formatted_expected) != 0 ||
            errno != EACCES)
            return 112;

        /* Insufficient capacity is the ordinary zero return, while null
         * required pointers and invalid epoch values set guest EINVAL. */
        arguments[1] = 4U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRFTIME_NAME,
                            arguments, 4U, &result) || result != 0U ||
            errno != EACCES)
            return 113;
        arguments[0] = 0U;
        arguments[1] = ISAAC_VITA_CRT_STRFTIME_CAPACITY;
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRFTIME_NAME,
                            arguments, 4U, &result) || result != 0U ||
            g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 114;

        arguments[0] = 0U;
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GMTIME64_NAME,
                            arguments, 1U, &result) || result != 0U ||
            g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 115;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_LOCALTIME64_NAME,
                            arguments, 1U, &result) || result != 0U ||
            g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 116;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_MKGMTIME64_NAME,
                            arguments, 1U, &result) ||
            result != UINT32_MAX || cpu.edx != UINT32_MAX ||
            g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 117;

        /* A normalised instant below the documented lower bound fails and
         * leaves the caller's nine dwords and both guards unchanged. */
        s_time_tm_guarded[0] = 0x10203040U;
        s_time_tm_guarded[1] = 59U;
        s_time_tm_guarded[2] = 59U;
        s_time_tm_guarded[3] = 23U;
        s_time_tm_guarded[4] = 31U;
        s_time_tm_guarded[5] = 11U;
        s_time_tm_guarded[6] = 69U;
        s_time_tm_guarded[7] = 3U;
        s_time_tm_guarded[8] = 364U;
        s_time_tm_guarded[9] = 0U;
        s_time_tm_guarded[10] = 0x50607080U;
        arguments[0] = pointer32(&s_time_tm_guarded[1]);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_MKGMTIME64_NAME,
                            arguments, 1U, &result) ||
            result != UINT32_MAX || cpu.edx != UINT32_MAX ||
            s_time_tm_guarded[0] != 0x10203040U ||
            s_time_tm_guarded[1] != 59U ||
            s_time_tm_guarded[2] != 59U ||
            s_time_tm_guarded[3] != 23U ||
            s_time_tm_guarded[4] != 31U ||
            s_time_tm_guarded[5] != 11U ||
            s_time_tm_guarded[6] != 69U ||
            s_time_tm_guarded[7] != 3U ||
            s_time_tm_guarded[8] != 364U ||
            s_time_tm_guarded[9] != 0U ||
            s_time_tm_guarded[10] != 0x50607080U ||
            g_isaac_vita_crt_errno != EINVAL || errno != EACCES)
            return 118;

        errno = saved_host_errno;
    }

    /* Complete contiguous stdio/file-I/O family.  Every FILE token remains
     * in the production newlib registry: standard streams live outside the
     * bounded dynamic ledger, while fopen/fread/fwrite/seek/tell/flush and
     * descriptor identity all cross the same locked ownership boundary. */
    {
        enum { FILE_PAYLOAD_LENGTH = 17U, FILE_FIRST_READ = 7U };
        uint32_t arguments[6];
        uint32_t standard_tokens[ISAAC_VITA_CRT_STANDARD_STREAM_COUNT];
        uint32_t standard_descriptors[ISAAC_VITA_CRT_STANDARD_STREAM_COUNT];
        uint32_t capacity_tokens[ISAAC_VITA_CRT_FILE_TOKEN_COUNT];
        uint32_t token;
        uint32_t descriptor;
        uint32_t result;
        unsigned counted;
        unsigned fread_before;
        unsigned archive_parent_index;
        int saved_host_errno = errno;
        int cleanup_errno;

        cleanup_errno = errno;
        (void)remove(s_file_oracle_path);
        errno = cleanup_errno;
        errno = EACCES;
        g_isaac_vita_crt_errno = ENOMEM;

        /* __acrt_iob_func exposes stable tokens without consuming any of the
         * dynamic slots.  _fileno and _get_osfhandle preserve one identity
         * newlib descriptor family, and success clears neither errno cell. */
        for (i = 0U; i < ISAAC_VITA_CRT_STANDARD_STREAM_COUNT; ++i) {
            arguments[0] = i;
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_ACRT_IOB_NAME,
                                arguments, 1U, &standard_tokens[i]) ||
                !standard_tokens[i] || errno != EACCES ||
                g_isaac_vita_crt_errno != ENOMEM)
                return 121;
            arguments[0] = standard_tokens[i];
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FILENO_NAME,
                                arguments, 1U, &standard_descriptors[i]) ||
                (int32_t)standard_descriptors[i] < 0 ||
                !isaac_vita_crt_osfhandle_is_owned(
                    standard_descriptors[i]) ||
                errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
                return 122;
            arguments[0] = standard_descriptors[i];
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,
                                arguments, 1U, &result) ||
                result != standard_descriptors[i] || errno != EACCES ||
                g_isaac_vita_crt_errno != ENOMEM)
                return 123;
        }
        if (standard_tokens[0] == standard_tokens[1] ||
            standard_tokens[0] == standard_tokens[2] ||
            standard_tokens[1] == standard_tokens[2])
            return 124;

        arguments[0] = 0U;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FFLUSH_NAME,
                            arguments, 1U, &result) || result != 0U ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 125;

        esp = prepare_call(&cpu);
        s_frame[5] = ISAAC_VITA_CRT_STANDARD_STREAM_COUNT;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ACRT_IOB_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "__acrt_iob_func index outside 0..2") != 0 ||
            cpu.fault_addr != ISAAC_VITA_CRT_STANDARD_STREAM_COUNT ||
            cpu.esp != esp || errno != EACCES ||
            g_isaac_vita_crt_errno != ENOMEM)
            return 126;

        /* fclose must reject standard tokens and release the lock before its
         * controlled guest fault.  The immediately following fileno would
         * hang forever in the source oracle if that path leaked the lock. */
        esp = prepare_call(&cpu);
        s_frame[5] = standard_tokens[1];
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FCLOSE_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault,
                   "fclose received a standard FILE token") != 0 ||
            cpu.fault_addr != standard_tokens[1] || cpu.esp != esp)
            return 127;
        arguments[0] = standard_tokens[1];
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FILENO_NAME,
                            arguments, 1U, &result) ||
            result != standard_descriptors[1])
            return 128;

        arguments[0] = UINT32_MAX;
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,
                            arguments, 1U, &result) ||
            result != UINT32_MAX ||
            isaac_vita_crt_osfhandle_is_owned(UINT32_MAX) ||
            g_isaac_vita_crt_errno != EBADF || errno != EACCES)
            return 129;

        arguments[0] = pointer32(s_file_oracle_path);
        arguments[1] = pointer32(s_file_mode_wb);
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FOPEN_NAME,
                            arguments, 2U, &token) || !token ||
            g_isaac_vita_crt_errno != ENOMEM || errno != EACCES)
            return 130;

        /* Unsupported formatting can unwind nonlocally in production.  It
         * happens outside s_file_lock; a same-registry ftell immediately
         * proves the controlled fault did not poison later I/O. */
        s_file_varargs[0] = 0U;
        esp = prepare_call(&cpu);
        s_frame[5] = 0U;
        s_frame[6] = 0U;
        s_frame[7] = token;
        s_frame[8] = pointer32(s_file_bad_format);
        s_frame[9] = 0U;
        s_frame[10] = pointer32(s_file_varargs);
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_VFPRINTF_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "guest stdio unsupported format") != 0 ||
            cpu.fault_addr != pointer32(s_file_bad_format) ||
            cpu.esp != esp || errno != EACCES ||
            g_isaac_vita_crt_errno != ENOMEM)
            return 131;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FTELL_NAME,
                            arguments, 1U, &result) || result != 0U)
            return 132;

        /* Invalid-token validation precedes all guest format reads, mutates
         * no registry state, and likewise leaves no held lock. */
        esp = prepare_call(&cpu);
        s_frame[5] = 0U;
        s_frame[6] = 0U;
        s_frame[7] = 0xdecafbadU;
        s_frame[8] = pointer32(s_file_bad_format);
        s_frame[9] = 0U;
        s_frame[10] = pointer32(s_file_varargs);
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_VFPRINTF_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault,
                   "vfprintf received an unknown FILE token") != 0 ||
            cpu.fault_addr != 0xdecafbadU || cpu.esp != esp)
            return 133;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FTELL_NAME,
                            arguments, 1U, &result) || result != 0U)
            return 134;

        /* Exercise the host-private vfprintf buffer and its revalidation;
         * rewind before the pinned logger fwrite so the payload stays exact. */
        arguments[0] = 0U;
        arguments[1] = 0U;
        arguments[2] = token;
        arguments[3] = pointer32(s_file_good_format);
        arguments[4] = 0U;
        arguments[5] = pointer32(s_file_varargs);
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_VFPRINTF_NAME,
                            arguments, 6U, &result) || result != 1U ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 135;
        arguments[0] = token;
        arguments[1] = 0U;
        arguments[2] = (uint32_t)SEEK_SET;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 136;

        arguments[0] = 0U;
        arguments[1] = 0U;
        arguments[2] = 7U;
        arguments[3] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FWRITE_NAME,
                            arguments, 4U, &result) || result != 0U ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 166;

        for (i = 0U; i < FILE_PAYLOAD_LENGTH; ++i)
            s_file_payload[i] = (unsigned char)(0x31U + i);
        /* Valid MiniZ final-block header, little-endian: length four. */
        s_file_payload[0] = 0x04U;
        s_file_payload[1] = 0x00U;
        s_file_payload[2] = 0x00U;
        s_file_payload[3] = 0x80U;
        memset(s_file_readback, 0xcc, sizeof s_file_readback);
        counted = ISAAC_VITA_CRT_FWRITE_FIRST_BEFORE_COUNT;
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_file_payload);
        s_frame[6] = 1U;
        s_frame[7] = FILE_PAYLOAD_LENGTH;
        s_frame[8] = token;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_FWRITE_NAME, &counted) ||
            counted != ISAAC_VITA_CRT_FWRITE_FIRST_ORDINAL || cpu.fault ||
            cpu.esp != esp + 4U || cpu.eax != FILE_PAYLOAD_LENGTH ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 137;

        esp = prepare_call(&cpu);
        s_frame[5] = token;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_FFLUSH_NAME, &counted) ||
            counted != ISAAC_VITA_CRT_FFLUSH_FIRST_ORDINAL || cpu.fault ||
            cpu.esp != esp + 4U || cpu.eax != 0U || errno != EACCES ||
            g_isaac_vita_crt_errno != ENOMEM)
            return 138;

        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FTELL_NAME,
                            arguments, 1U, &result) ||
            result != FILE_PAYLOAD_LENGTH)
            return 139;
        arguments[0] = token;
        arguments[1] = 0U;
        arguments[2] = (uint32_t)SEEK_SET;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 140;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FILENO_NAME,
                            arguments, 1U, &descriptor) ||
            (int32_t)descriptor < 0 ||
            !isaac_vita_crt_osfhandle_is_owned(descriptor))
            return 141;
        arguments[0] = descriptor;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,
                            arguments, 1U, &result) || result != descriptor)
            return 142;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FCLOSE_NAME,
                            arguments, 1U, &result) || result != 0U)
            return 143;
        for (i = 0U; i < ISAAC_VITA_CRT_STANDARD_STREAM_COUNT; ++i)
            if (descriptor == standard_descriptors[i])
                return 144;
        if (isaac_vita_crt_osfhandle_is_owned(descriptor))
            return 145;
        arguments[0] = descriptor;
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,
                            arguments, 1U, &result) ||
            result != UINT32_MAX || g_isaac_vita_crt_errno != EBADF ||
            errno != EACCES)
            return 146;

        arguments[0] = pointer32(s_file_oracle_path);
        arguments[1] = pointer32(s_file_mode_rb);
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FOPEN_NAME,
                            arguments, 2U, &token) || !token ||
            g_isaac_vita_crt_errno != ENOMEM || errno != EACCES)
            return 147;

        /* Pin the measured fread adapter frame: the import return is at ESP,
         * while ArchivedFile's exact header/payload caller is [EBP+4]. */
        s_archive_read_guard[0] = 0x11223344U;
        s_archive_read_guard[1] = 0x10203040U;
        s_archive_read_guard[2] = 0x55667788U;
        s_archive_read_guard[3] = 0x99aabbccU;
        fread_before = s_native_fread_calls;
        esp = prepare_archive_fread_call(
            &cpu, ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA);
        s_frame[5] = pointer32(&s_archive_read_guard[1]);
        s_frame[6] = 1U;
        s_frame[7] = 1U;
        s_frame[8] = token;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault,
                   "ArchivedFile block header read shape is invalid") != 0 ||
            cpu.fault_addr != pointer32(&s_archive_read_guard[1]) ||
            cpu.esp != esp || s_native_fread_calls != fread_before ||
            s_archive_read_guard[0] != 0x11223344U ||
            s_archive_read_guard[1] != 0x10203040U ||
            s_archive_read_guard[2] != 0x55667788U ||
            s_archive_read_guard[3] != 0x99aabbccU)
            return 189;

        s_archive_read_guard[1] = 0U;
        fread_before = s_native_fread_calls;
        esp = prepare_archive_fread_call(
            &cpu, ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA);
        s_frame[5] = pointer32(&s_archive_read_guard[1]);
        s_frame[6] = 4U;
        s_frame[7] = 1U;
        s_frame[8] = token;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME) ||
            cpu.fault || cpu.esp != esp + 4U || cpu.eax != 1U ||
            s_native_fread_calls != fread_before + 1U ||
            s_archive_read_guard[0] != 0x11223344U ||
            s_archive_read_guard[1] != 0x80000004U ||
            s_archive_read_guard[2] != 0x55667788U ||
            s_archive_read_guard[3] != 0x99aabbccU)
            return 168;

        arguments[0] = token;
        arguments[1] = 4U;
        arguments[2] = (uint32_t)SEEK_SET;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 169;
        s_archive_read_guard[1] = 0U;
        fread_before = s_native_fread_calls;
        esp = prepare_archive_fread_call(
            &cpu, ISAAC_VITA_CRT_ARCHIVE_TYPE1_HEADER_RETURN_RVA);
        s_frame[5] = pointer32(&s_archive_read_guard[1]);
        s_frame[6] = 4U;
        s_frame[7] = 1U;
        s_frame[8] = token;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "ArchivedFile block header is invalid") != 0 ||
            cpu.fault_addr != pointer32(&s_archive_read_guard[1]) ||
            cpu.esp != esp || s_native_fread_calls != fread_before + 1U ||
            s_archive_read_guard[0] != 0x11223344U ||
            s_archive_read_guard[2] != 0x55667788U ||
            s_archive_read_guard[3] != 0x99aabbccU)
            return 170;

        arguments[0] = token;
        arguments[1] = 0U;
        arguments[2] = (uint32_t)SEEK_SET;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 171; /* also proves the fatal path released s_file_lock */

        for (archive_parent_index = 0U;
             archive_parent_index < 2U; ++archive_parent_index) {
            uint32_t parent = archive_parent_index ?
                ISAAC_VITA_CRT_ARCHIVE_TYPE2_PAYLOAD_RETURN_RVA :
                ISAAC_VITA_CRT_ARCHIVE_TYPE1_PAYLOAD_RETURN_RVA;

            s_archive_read_guard[0] = 0x11223344U;
            s_archive_read_guard[1] = 0x10203040U;
            s_archive_read_guard[2] = 0x55667788U;
            s_archive_read_guard[3] = 0x99aabbccU;
            fread_before = s_native_fread_calls;
            esp = prepare_archive_fread_call(&cpu, parent);
            s_frame[5] = pointer32(&s_archive_read_guard[1]);
            s_frame[6] = 1U;
            s_frame[7] = ISAAC_VITA_CRT_ARCHIVE_BLOCK_MAX_BYTES + 1U;
            s_frame[8] = token;
            if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME) ||
                !cpu.fault ||
                strcmp(cpu.fault,
                       "ArchivedFile payload exceeds 0x800-byte block") != 0 ||
                cpu.fault_addr != pointer32(&s_archive_read_guard[1]) ||
                cpu.esp != esp || s_native_fread_calls != fread_before ||
                s_archive_read_guard[0] != 0x11223344U ||
                s_archive_read_guard[1] != 0x10203040U ||
                s_archive_read_guard[2] != 0x55667788U ||
                s_archive_read_guard[3] != 0x99aabbccU)
                return 172 + (int)archive_parent_index;
        }

        arguments[0] = token;
        arguments[1] = 0U;
        arguments[2] = (uint32_t)SEEK_SET;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 174; /* both pre-read faults released the registry lock */

        arguments[0] = 0U;
        arguments[1] = 1U;
        arguments[2] = 0U;
        arguments[3] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FREAD_NAME,
                            arguments, 4U, &result) || result != 0U ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 167;

        counted = ISAAC_VITA_CRT_FREAD_PREDICTED_BEFORE_COUNT;
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_file_readback);
        s_frame[6] = ISAAC_VITA_CRT_FREAD_FIRST_SIZE;
        s_frame[7] = ISAAC_VITA_CRT_FREAD_FIRST_COUNT;
        s_frame[8] = token;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_FREAD_NAME, &counted) ||
            counted != ISAAC_VITA_CRT_FREAD_PREDICTED_ORDINAL || cpu.fault ||
            cpu.esp != esp + 4U || cpu.eax != FILE_FIRST_READ ||
            memcmp(s_file_readback, s_file_payload, FILE_FIRST_READ) != 0 ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 148;
        arguments[0] = token;
        arguments[1] = (uint32_t)(int32_t)-4;
        arguments[2] = (uint32_t)SEEK_END;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result != 0U)
            return 149;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FTELL_NAME,
                            arguments, 1U, &result) ||
            result != FILE_PAYLOAD_LENGTH - 4U)
            return 150;

        /* Both multiplication overflow and end-address wrap fault at the
         * guest boundary before native libc sees an invalid pointer. */
        counted = 4000U;
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_file_readback);
        s_frame[6] = UINT32_MAX;
        s_frame[7] = 2U;
        s_frame[8] = token;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_FREAD_NAME, &counted) ||
            counted != 4001U || !cpu.fault ||
            strcmp(cpu.fault, "fread guest range overflow") != 0 ||
            cpu.fault_addr != pointer32(s_file_readback) || cpu.esp != esp ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 151;
        counted = 5000U;
        esp = prepare_call(&cpu);
        s_frame[5] = UINT32_MAX - 1U;
        s_frame[6] = 1U;
        s_frame[7] = 4U;
        s_frame[8] = token;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_FWRITE_NAME, &counted) ||
            counted != 5001U || !cpu.fault ||
            strcmp(cpu.fault, "fwrite guest range overflow") != 0 ||
            cpu.fault_addr != UINT32_MAX - 1U || cpu.esp != esp ||
            errno != EACCES || g_isaac_vita_crt_errno != ENOMEM)
            return 152;

        /* Unknown-token lookup faults only after releasing the lock. */
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_file_readback);
        s_frame[6] = 1U;
        s_frame[7] = 1U;
        s_frame[8] = 0xdecafbadU;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FREAD_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "fread received an unknown FILE token") != 0 ||
            cpu.fault_addr != 0xdecafbadU || cpu.esp != esp)
            return 153;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FTELL_NAME,
                            arguments, 1U, &result) ||
            result != FILE_PAYLOAD_LENGTH - 4U)
            return 154;

        /* Native failures mirror newlib errno (or deterministic EIO fallback)
         * into the guest cell while restoring the host's prior errno. */
        arguments[0] = token;
        arguments[1] = 0U;
        arguments[2] = 0x7fffffffU;
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FSEEK_NAME,
                            arguments, 3U, &result) || result == 0U ||
            g_isaac_vita_crt_errno == ENOMEM ||
            g_isaac_vita_crt_errno == 0 || errno != EACCES)
            return 155;
        arguments[0] = pointer32(s_file_payload);
        arguments[1] = 1U;
        arguments[2] = 1U;
        arguments[3] = token;
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FWRITE_NAME,
                            arguments, 4U, &result) || result != 0U ||
            g_isaac_vita_crt_errno == ENOMEM ||
            g_isaac_vita_crt_errno == 0 || errno != EACCES)
            return 156;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FCLOSE_NAME,
                            arguments, 1U, &result) || result != 0U)
            return 157;

        /* The measured text-read mode is accepted too. */
        arguments[0] = pointer32(s_file_oracle_path);
        arguments[1] = pointer32(s_file_mode_r);
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FOPEN_NAME,
                            arguments, 2U, &token) || !token ||
            g_isaac_vita_crt_errno != ENOMEM || errno != EACCES)
            return 158;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FCLOSE_NAME,
                            arguments, 1U, &result) || result != 0U)
            return 159;

        /* Fill every dynamic slot after resolving all three standard tokens.
         * One more fopen must fault cleanly and close its native stream; this
         * pins the proven-peak-plus-headroom bound and standard separation. */
        for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i) {
            arguments[0] = pointer32(s_file_oracle_path);
            arguments[1] = pointer32(s_file_mode_rb);
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FOPEN_NAME,
                                arguments, 2U, &capacity_tokens[i]) ||
                !capacity_tokens[i])
                return 160;
        }
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_file_oracle_path);
        s_frame[6] = pointer32(s_file_mode_rb);
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FOPEN_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "Vita CRT FILE token table full") != 0 ||
            !cpu.fault_addr || cpu.esp != esp)
            return 161;
        for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i) {
            arguments[0] = capacity_tokens[i];
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FCLOSE_NAME,
                                arguments, 1U, &result) || result != 0U)
                return 162;
        }

        /* Existing text-write mode remains valid, while an unknown modifier
         * faults before fopen and cannot create or register a native stream. */
        arguments[0] = pointer32(s_file_oracle_path);
        arguments[1] = pointer32(s_file_mode_w);
        g_isaac_vita_crt_errno = ENOMEM;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FOPEN_NAME,
                            arguments, 2U, &token) || !token ||
            g_isaac_vita_crt_errno != ENOMEM || errno != EACCES)
            return 163;
        arguments[0] = token;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_FCLOSE_NAME,
                            arguments, 1U, &result) || result != 0U)
            return 164;
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_file_oracle_path);
        s_frame[6] = pointer32(s_file_mode_invalid);
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_FOPEN_NAME) ||
            !cpu.fault || strcmp(cpu.fault,
                                 "fopen received an invalid mode") != 0 ||
            cpu.fault_addr != pointer32(s_file_mode_invalid) ||
            cpu.esp != esp || errno != EACCES)
            return 165;

        cleanup_errno = errno;
        (void)remove(s_file_oracle_path);
        errno = cleanup_errno;
        errno = saved_host_errno;
    }

    /* Complete six-slot convert DLL.  The four numeric handlers use private
     * copies in the process C locale, map native end offsets back to guest
     * addresses, preserve native errno, and use the x86 EDX:EAX/x87 return
     * channels measured by the independent installed-UCRT oracle. */
    {
        static const struct {
            const char *text;
            int base;
            uint64_t value;
            uint32_t end_offset;
            int32_t initial_errno;
            int32_t final_errno;
        } strtoull_cases[] = {
            { "", 10, UINT64_C(0), 0U, 71, 71 },
            { "  +0x10z", 0, UINT64_C(0x10), 7U, 72, 72 },
            { "  +42tail", 10, UINT64_C(42), 5U, 73, 73 },
            { "-1", 10, UINT64_MAX, 2U, 74, 74 },
            { "-17x", 10, UINT64_C(0xffffffffffffffef), 3U, 75, 75 },
            { "18446744073709551615!", 10, UINT64_MAX, 20U, 76, 76 },
            { "18446744073709551616!", 10, UINT64_MAX, 20U, 77, ERANGE }
        };
        static const struct {
            const char *text;
            int base;
            int32_t value;
            uint32_t end_offset;
            int32_t initial_errno;
            int32_t final_errno;
        } strtol_cases[] = {
            { "", 10, 0, 0U, 81, 81 },
            { "  -0x10z", 0, -16, 7U, 82, 82 },
            { "2147483647!", 10, INT32_MAX, 10U, 83, 83 },
            { "2147483648!", 10, INT32_MAX, 10U, 84, ERANGE },
            { "-2147483648!", 10, INT32_MIN, 11U, 85, 85 },
            { "-2147483649!", 10, INT32_MIN, 11U, 86, ERANGE }
        };
        static const struct {
            const char *text;
            int32_t value;
            int32_t initial_errno;
            int32_t final_errno;
        } atoi_cases[] = {
            { "", 0, 91, 91 },
            { "\t\r\n +42xyz", 42, 92, 92 },
            { "-17tail", -17, 93, 93 },
            { "+-3", 0, 94, 94 },
            { "2147483647", INT32_MAX, 95, 95 },
            { "2147483648", INT32_MAX, 96, ERANGE },
            { "-2147483648", INT32_MIN, 97, 97 },
            { "-2147483649", INT32_MIN, 98, ERANGE }
        };
        static const struct {
            const char *text;
            uint64_t result_bits;
        } atof_cases[] = {
            { "\t -12.5e2tail", UINT64_C(0xc093880000000000) },
            { "+.25E+2x", UINT64_C(0x4039000000000000) },
            { "1e", UINT64_C(0x3ff0000000000000) },
            { "-0", UINT64_C(0x8000000000000000) },
            { "3.141592653589793", UINT64_C(0x400921fb54442d18) },
            { "1e309", UINT64_C(0x7ff0000000000000) }
        };
        static const uint64_t x87_sentinel =
            UINT64_C(0x4005bf0a8b145769);
        uint32_t arguments[3];
        uint32_t result;
        uint64_t wide_result;
        unsigned counted;
        int saved_host_errno = errno;

        errno = EACCES;
        for (i = 0U; i < sizeof strtoull_cases /
                         sizeof strtoull_cases[0]; ++i) {
            strcpy(s_convert_text, strtoull_cases[i].text);
            s_convert_endptr = 0xccccccccU;
            g_isaac_vita_crt_errno = strtoull_cases[i].initial_errno;
            arguments[0] = pointer32(s_convert_text);
            arguments[1] = pointer32(&s_convert_endptr);
            arguments[2] = (uint32_t)strtoull_cases[i].base;
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRTOULL_NAME,
                                arguments, 3U, &result))
                return 169;
            wide_result = (uint64_t)result | ((uint64_t)cpu.edx << 32U);
            if (wide_result != strtoull_cases[i].value ||
                s_convert_endptr != pointer32(s_convert_text) +
                    strtoull_cases[i].end_offset ||
                g_isaac_vita_crt_errno != strtoull_cases[i].final_errno ||
                errno != EACCES)
                return 170;
        }

        /* Both frozen callers use base ten and a null endptr. */
        strcpy(s_convert_text, "-1");
        arguments[0] = pointer32(s_convert_text);
        arguments[1] = 0U;
        arguments[2] = 10U;
        g_isaac_vita_crt_errno = 79;
        if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRTOULL_NAME,
                            arguments, 3U, &result) ||
            result != UINT32_MAX || cpu.edx != UINT32_MAX ||
            g_isaac_vita_crt_errno != 79 || errno != EACCES)
            return 171;

        counted = 8000U;
        s_convert_endptr = 0x13579bdfU;
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_convert_text);
        s_frame[6] = pointer32(&s_convert_endptr);
        s_frame[7] = 1U;
        if (!isaac_vita_crt_import_counted(
                &cpu, ISAAC_VITA_CRT_STRTOULL_NAME, &counted) ||
            counted != 8001U || !cpu.fault ||
            strcmp(cpu.fault, "strtoull received an invalid base") != 0 ||
            cpu.fault_addr != 1U || cpu.esp != esp ||
            s_convert_endptr != 0x13579bdfU || errno != EACCES)
            return 172;

        for (i = 0U; i < sizeof strtol_cases /
                         sizeof strtol_cases[0]; ++i) {
            strcpy(s_convert_text, strtol_cases[i].text);
            s_convert_endptr = 0xccccccccU;
            g_isaac_vita_crt_errno = strtol_cases[i].initial_errno;
            arguments[0] = pointer32(s_convert_text);
            arguments[1] = pointer32(&s_convert_endptr);
            arguments[2] = (uint32_t)strtol_cases[i].base;
            if (!call_crt_cdecl(&cpu, ISAAC_VITA_CRT_STRTOL_NAME,
                                arguments, 3U, &result) ||
                (int32_t)result != strtol_cases[i].value ||
                s_convert_endptr != pointer32(s_convert_text) +
                    strtol_cases[i].end_offset ||
                g_isaac_vita_crt_errno != strtol_cases[i].final_errno ||
                errno != EACCES)
                return 173;
        }

        /* Reject an unwritable guest endptr before native conversion. */
        strcpy(s_convert_text, "42");
        esp = prepare_call(&cpu);
        s_frame[5] = pointer32(s_convert_text);
        s_frame[6] = UINT32_MAX - 1U;
        s_frame[7] = 10U;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_STRTOL_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "strtol end pointer range overflow") != 0 ||
            cpu.fault_addr != UINT32_MAX - 1U || cpu.esp != esp ||
            errno != EACCES)
            return 174;

        for (i = 0U; i < sizeof atoi_cases / sizeof atoi_cases[0]; ++i) {
            strcpy(s_convert_text, atoi_cases[i].text);
            g_isaac_vita_crt_errno = atoi_cases[i].initial_errno;
            esp = prepare_call(&cpu);
            cpu.st_top = 3U;
            *fst(&cpu, 0) = oracle_double_from_bits(x87_sentinel);
            s_frame[5] = pointer32(s_convert_text);
            if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATOI_NAME) ||
                cpu.fault || cpu.esp != esp + 4U ||
                (int32_t)cpu.eax != atoi_cases[i].value ||
                cpu.st_top != 3U ||
                oracle_double_bits(*fst(&cpu, 0)) != x87_sentinel ||
                g_isaac_vita_crt_errno != atoi_cases[i].final_errno ||
                errno != EACCES)
                return 175;
        }

        esp = prepare_call(&cpu);
        s_frame[5] = 0U;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATOI_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "atoi received a null guest pointer") != 0 ||
            cpu.fault_addr != 0U || cpu.esp != esp || errno != EACCES)
            return 176;

        for (i = 0U; i < sizeof atof_cases / sizeof atof_cases[0]; ++i) {
            strcpy(s_convert_text, atof_cases[i].text);
            g_isaac_vita_crt_errno = 101;
            esp = prepare_call(&cpu);
            cpu.st_top = 5U;
            *fst(&cpu, 0) = oracle_double_from_bits(x87_sentinel);
            s_frame[5] = pointer32(s_convert_text);
            if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATOF_NAME) ||
                cpu.fault || cpu.esp != esp + 4U || cpu.st_top != 4U ||
                oracle_double_bits(*fst(&cpu, 0)) !=
                    atof_cases[i].result_bits ||
                oracle_double_bits(*fst(&cpu, 1)) != x87_sentinel ||
                g_isaac_vita_crt_errno != 101 || errno != EACCES)
                return 177;
        }

        esp = prepare_call(&cpu);
        cpu.st_top = 2U;
        *fst(&cpu, 0) = oracle_double_from_bits(x87_sentinel);
        s_frame[5] = 0U;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATOF_NAME) ||
            !cpu.fault ||
            strcmp(cpu.fault, "atof received a null guest pointer") != 0 ||
            cpu.fault_addr != 0U || cpu.esp != esp || cpu.st_top != 2U ||
            oracle_double_bits(*fst(&cpu, 0)) != x87_sentinel ||
            errno != EACCES)
            return 178;
        errno = saved_host_errno;
    }

    /* A durable heap terminal must never be translated into `_strdup`'s
     * ordinary NULL result.  Cover both a masked native success (which must be
     * cleaned up) and a direct allocation failure, without consuming RET. */
    {
        unsigned malloc_calls = s_guest_heap_malloc_calls;
        unsigned free_calls = s_guest_heap_free_calls;
        char *source;

        esp = prepare_call(&cpu);
        source = (char *)&s_frame[12];
        source[0] = 'x';
        source[1] = '\0';
        s_frame[5] = pointer32(source);
        s_guest_heap_terminal = 1;
        s_guest_heap_malloc_fail = 0;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_STRDUP_NAME) ||
            !cpu.fault || cpu.esp != esp ||
            s_guest_heap_malloc_calls != malloc_calls + 1U ||
            s_guest_heap_free_calls != free_calls + 1U)
            return 179;

        malloc_calls = s_guest_heap_malloc_calls;
        free_calls = s_guest_heap_free_calls;
        esp = prepare_call(&cpu);
        source = (char *)&s_frame[12];
        source[0] = 'y';
        source[1] = '\0';
        s_frame[5] = pointer32(source);
        s_guest_heap_malloc_fail = 1;
        if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_STRDUP_NAME) ||
            !cpu.fault || cpu.esp != esp ||
            s_guest_heap_malloc_calls != malloc_calls + 1U ||
            s_guest_heap_free_calls != free_calls)
            return 180;
        s_guest_heap_terminal = 0;
        s_guest_heap_malloc_fail = 0;
    }

    /* The frozen process-exit family mirrors the already exercised PC
     * backend: `_exit` skips cleanup, while `exit` runs the one TLS callback
     * and the ordinary callbacks in LIFO order before the controlled guest
     * noreturn handoff.  This oracle's guest_exit records instead of
     * longjmping so the ARM build can retain and inspect the terminal state. */
    g_isaac_vita_crt.atexit_count = 0U;
    memset(g_isaac_vita_crt_atexit, 0, sizeof g_isaac_vita_crt_atexit);
    memset(s_exit_order, 0, sizeof s_exit_order);
    memset(s_exit_tls_args, 0xff, sizeof s_exit_tls_args);
    s_exit_callback_count = 0U;
    s_guest_exit_calls = 0U;
    s_guest_exit_code = 0;
    s_guest_exit_api = NULL;

    esp = prepare_call(&cpu);
    s_frame[5] = CALLBACK_EXIT_A;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATEXIT_NAME) ||
        cpu.fault || cpu.esp != esp + 4U)
        return 181;
    esp = prepare_call(&cpu);
    s_frame[5] = CALLBACK_EXIT_B;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_ATEXIT_NAME) ||
        cpu.fault || cpu.esp != esp + 4U)
        return 182;
    esp = prepare_call(&cpu);
    s_frame[5] = CALLBACK_EXIT_TLS;
    if (!isaac_vita_crt_import(
            &cpu, ISAAC_VITA_CRT_TLS_EXIT_REGISTER_NAME) ||
        cpu.fault || cpu.esp != esp + 4U)
        return 183;

    /* Registration is one-shot.  A duplicate remains on the caller frame and
     * faults instead of replacing the callback that `exit` must later run. */
    esp = prepare_call(&cpu);
    s_frame[5] = CALLBACK_EXIT_TLS;
    if (!isaac_vita_crt_import(
            &cpu, ISAAC_VITA_CRT_TLS_EXIT_REGISTER_NAME) ||
        cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault, "TLS exit callback registered twice") != 0 ||
        cpu.fault_addr != CALLBACK_EXIT_TLS)
        return 184;

    esp = prepare_call(&cpu);
    s_frame[5] = 9U;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT__EXIT_NAME) ||
        cpu.fault || cpu.esp != esp || s_guest_exit_calls != 1U ||
        s_guest_exit_code != 9 || !s_guest_exit_api ||
        strcmp(s_guest_exit_api, "_exit") != 0 ||
        s_exit_callback_count != 0U ||
        g_isaac_vita_crt.atexit_count != 2U)
        return 185;

    esp = prepare_call(&cpu);
    s_frame[5] = 1U;
    if (!isaac_vita_crt_import(&cpu, ISAAC_VITA_CRT_EXIT_NAME) ||
        cpu.fault || cpu.esp != esp || s_guest_exit_calls != 2U ||
        s_guest_exit_code != 1 || !s_guest_exit_api ||
        strcmp(s_guest_exit_api, "exit") != 0 ||
        s_exit_callback_count != 3U ||
        s_exit_order[0] != 3U || s_exit_order[1] != 2U ||
        s_exit_order[2] != 1U ||
        s_exit_tls_args[0] != 0U || s_exit_tls_args[1] != 0U ||
        s_exit_tls_args[2] != 0U ||
        g_isaac_vita_crt.atexit_count != 0U)
        return 186;

    return 0;
}

int main(void)
{
    return run_oracle();
}
