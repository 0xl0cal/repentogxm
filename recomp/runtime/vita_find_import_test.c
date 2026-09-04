/* Compile/link/static softfp ARM oracle for the Vita FindFile subsystem. */
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_find.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita FindFile oracle requires a 32-bit identity-mapped host
#endif

typedef struct find_call_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t call_rva;
    uint32_t return_rva;
} find_call_evidence;

static const find_call_evidence s_evidence[] = {
    { ISAAC_VITA_FIND_FIRST_NAME,
      ISAAC_VITA_FIND_FIRST_IAT_RVA,
      ISAAC_VITA_FIND_FIRST_CALL_RVA,
      ISAAC_VITA_FIND_FIRST_RETURN_RVA },
    { ISAAC_VITA_FIND_NEXT_NAME,
      ISAAC_VITA_FIND_NEXT_IAT_RVA,
      ISAAC_VITA_FIND_NEXT_CALL_RVA,
      ISAAC_VITA_FIND_NEXT_RETURN_RVA },
    { ISAAC_VITA_FIND_CLOSE_NAME,
      ISAAC_VITA_FIND_CLOSE_IAT_RVA,
      ISAAC_VITA_FIND_CLOSE_CALL_RVA,
      ISAAC_VITA_FIND_CLOSE_RETURN_RVA }
};

_Static_assert(sizeof s_evidence / sizeof s_evidence[0] ==
               ISAAC_VITA_FIND_IMPORT_COUNT,
               "FindFile oracle lost exact call-site evidence");
_Static_assert(ISAAC_VITA_FIND_DATA_SIZE == 592U,
               "x86 WIN32_FIND_DATAW size changed");
_Static_assert(ISAAC_VITA_FIND_HAS_LAST_ERROR == 1U,
               "FindFile LastError channel must stay enabled");
_Static_assert(ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND == 0x80010002U,
               "sceIoDopen missing-path result changed");
_Static_assert(ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY == 0x80010014U,
               "sceIoDopen non-directory-path result changed");
_Static_assert(ISAAC_VITA_FIND_ERROR_FILE_NOT_FOUND == 2U &&
               ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND == 3U &&
               ISAAC_VITA_FIND_ERROR_INVALID_HANDLE == 6U &&
               ISAAC_VITA_FIND_ERROR_NO_MORE_FILES == 18U,
               "FindFile Win32 ERROR_* values changed");
_Static_assert(ISAAC_VITA_FIND_DREAD_NEGATIVE_IS_EOF == 0U &&
               ISAAC_VITA_FIND_NATIVE_ERROR_IS_LOUD == 1U &&
               ISAAC_VITA_FIND_STALE_TOKEN_IS_LOUD == 0U &&
               ISAAC_VITA_FIND_REGISTRY_IS_LOCKED == 1U,
               "FindFile failure policy changed");
_Static_assert(ISAAC_VITA_FIND_SYNTHETIC_DOT_ENTRIES == 0U &&
               ISAAC_VITA_FIND_EXACT_PATH_USES_STAT == 1U,
               "FindFile path-enumeration policy changed");

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    unsigned calls = 0U;
    uint32_t i;

    if (ISAAC_VITA_FIND_FIRST_ATTEMPT_ORDINAL != 346U ||
        strcmp(ISAAC_VITA_FIND_FIRST_PATTERN,
               "ux0:data/isaacr001\\*") != 0 ||
        ISAAC_VITA_FIND_DATA_NAME_OFFSET != 44U ||
        ISAAC_VITA_FIND_DATA_ALT_NAME_OFFSET != 564U ||
        ISAAC_VITA_FIND_HANDLE_CAPACITY != 16U ||
        ISAAC_VITA_FIND_HAS_LAST_ERROR != 1U ||
        ISAAC_VITA_FIND_DREAD_NEGATIVE_IS_EOF != 0U ||
        ISAAC_VITA_FIND_NATIVE_ERROR_IS_LOUD != 1U ||
        ISAAC_VITA_FIND_STALE_TOKEN_IS_LOUD != 0U ||
        ISAAC_VITA_FIND_REGISTRY_IS_LOCKED != 1U ||
        ISAAC_VITA_FIND_SYNTHETIC_DOT_ENTRIES != 0U ||
        ISAAC_VITA_FIND_EXACT_PATH_USES_STAT != 1U)
        return 1;
    for (i = 0U; i < ISAAC_VITA_FIND_IMPORT_COUNT; ++i) {
        if (!s_evidence[i].name || !s_evidence[i].iat_rva ||
            !s_evidence[i].call_rva || !s_evidence[i].return_rva)
            return 2;
    }

    /* Unknown-name handoff is the only execution-independent dispatch path:
     * it must not touch CPU state or count a call. */
    memset(&cpu, 0xa5, sizeof cpu);
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_find_import_counted(
            &cpu, "KERNEL32.dll!not_a_find_import", &calls) != 0 ||
        calls != 0U || memcmp(&cpu, &snapshot, sizeof cpu) != 0)
        return 3;

    /* The real directory calls are deliberately not executed here:
     * arm-vita-eabi-run is not a Vita runtime.  Linking these handlers against
     * SceIofilemgr and inspecting the ELF is the scope of this oracle. */
    return 0;
}
