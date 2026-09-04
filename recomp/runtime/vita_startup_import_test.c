/* Compile/link/static softfp ARM oracle for the Vita startup-platform batch. */
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "guest_stack_legacy_oracle_stub.h"
#include "host_vita_startup.h"
#include "platform.h"

typedef struct startup_call_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t call_rva;
    uint32_t return_rva;
    uint32_t argument_evidence;
    uint32_t boot_ordinal;
} startup_call_evidence;

static const startup_call_evidence s_evidence[] = {
    { ISAAC_VITA_STARTUP_ENV_NAME,
      ISAAC_VITA_STARTUP_ENV_IAT_RVA,
      ISAAC_VITA_STARTUP_ENV_CALL_RVA,
      ISAAC_VITA_STARTUP_ENV_RETURN_RVA,
      ISAAC_VITA_STARTUP_ENV_BUFFER_SIZE,
      ISAAC_VITA_STARTUP_ENV_FIRST_ORDINAL },
    { ISAAC_VITA_STARTUP_ENV_NAME,
      ISAAC_VITA_STARTUP_ENV_IAT_RVA,
      ISAAC_VITA_STARTUP_ENV_FALLBACK_CALL_RVA,
      ISAAC_VITA_STARTUP_ENV_FALLBACK_RETURN_RVA,
      ISAAC_VITA_STARTUP_ENV_BUFFER_SIZE,
      ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_ORDINAL },
    { ISAAC_VITA_STARTUP_ENV_NAME,
      ISAAC_VITA_STARTUP_ENV_IAT_RVA,
      ISAAC_VITA_STARTUP_ENV_FALLBACK_CALL_RVA,
      ISAAC_VITA_STARTUP_ENV_FALLBACK_RETURN_RVA,
      ISAAC_VITA_STARTUP_ENV_BUFFER_SIZE,
      ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_ORDINAL },
    { ISAAC_VITA_STARTUP_SYSTEM_INFO_NAME,
      ISAAC_VITA_STARTUP_SYSTEM_INFO_IAT_RVA,
      ISAAC_VITA_STARTUP_SYSTEM_INFO_CALL_RVA,
      ISAAC_VITA_STARTUP_SYSTEM_INFO_RETURN_RVA,
      ISAAC_VITA_STARTUP_SYSTEM_INFO_SIZE,
      ISAAC_VITA_STARTUP_SYSTEM_INFO_ORDINAL },
    { ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
      ISAAC_VITA_STARTUP_LOAD_LIBRARY_IAT_RVA,
      ISAAC_VITA_STARTUP_LOAD_KERNELBASE_CALL_RVA,
      ISAAC_VITA_STARTUP_LOAD_KERNELBASE_RETURN_RVA,
      ISAAC_VITA_STARTUP_LOAD_KERNELBASE_NAME_VA,
      ISAAC_VITA_STARTUP_LOAD_KERNELBASE_ORDINAL },
    { ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
      ISAAC_VITA_STARTUP_LOAD_LIBRARY_IAT_RVA,
      ISAAC_VITA_STARTUP_LOAD_NTDLL_CALL_RVA,
      ISAAC_VITA_STARTUP_LOAD_NTDLL_RETURN_RVA,
      ISAAC_VITA_STARTUP_LOAD_NTDLL_NAME_VA,
      ISAAC_VITA_STARTUP_LOAD_NTDLL_ORDINAL },
    { ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
      ISAAC_VITA_STARTUP_LOAD_LIBRARY_IAT_RVA,
      ISAAC_VITA_STARTUP_LOAD_KERNEL32_CALL_RVA,
      ISAAC_VITA_STARTUP_LOAD_KERNEL32_RETURN_RVA,
      ISAAC_VITA_STARTUP_LOAD_KERNEL32_NAME_VA,
      ISAAC_VITA_STARTUP_LOAD_KERNEL32_ORDINAL },
    { ISAAC_VITA_STARTUP_QPF_NAME,
      ISAAC_VITA_STARTUP_QPF_IAT_RVA,
      ISAAC_VITA_STARTUP_QPF_CALL_RVA,
      ISAAC_VITA_STARTUP_QPF_RETURN_RVA,
      ISAAC_VITA_STARTUP_QPF_FREQUENCY,
      ISAAC_VITA_STARTUP_QPF_ORDINAL },
    { ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
      ISAAC_VITA_STARTUP_LOAD_LIBRARY_IAT_RVA,
      ISAAC_VITA_STARTUP_LOAD_USERENV_CALL_RVA,
      ISAAC_VITA_STARTUP_LOAD_USERENV_RETURN_RVA,
      ISAAC_VITA_STARTUP_LOAD_USERENV_NAME_VA,
      ISAAC_VITA_STARTUP_LOAD_USERENV_ORDINAL },
    { ISAAC_VITA_STARTUP_FREE_LIBRARY_NAME,
      ISAAC_VITA_STARTUP_FREE_LIBRARY_IAT_RVA,
      ISAAC_VITA_STARTUP_FREE_USERENV_CALL_RVA,
      ISAAC_VITA_STARTUP_FREE_USERENV_RETURN_RVA,
      ISAAC_VITA_STARTUP_FREE_USERENV_MODULE,
      ISAAC_VITA_STARTUP_FREE_USERENV_ORDINAL },
    { ISAAC_VITA_STARTUP_FILE_ATTRIBUTES_NAME,
      ISAAC_VITA_STARTUP_FILE_ATTRIBUTES_IAT_RVA,
      ISAAC_VITA_STARTUP_FILE_ATTRIBUTES_ROOT_CALL_RVA,
      ISAAC_VITA_STARTUP_FILE_ATTRIBUTES_ROOT_RETURN_RVA,
      ISAAC_VITA_STARTUP_FILE_ATTRIBUTE_DIRECTORY,
      ISAAC_VITA_STARTUP_FILE_ATTRIBUTES_ROOT_ORDINAL },
    { ISAAC_VITA_STARTUP_CREATE_DIRECTORY_NAME,
      ISAAC_VITA_STARTUP_CREATE_DIRECTORY_IAT_RVA,
      ISAAC_VITA_STARTUP_CREATE_SAVE_CALL_RVA,
      ISAAC_VITA_STARTUP_CREATE_SAVE_RETURN_RVA,
      0U,
      ISAAC_VITA_STARTUP_CREATE_SAVE_FIRST_ORDINAL },
    { ISAAC_VITA_STARTUP_CURRENT_DIRECTORY_NAME,
      ISAAC_VITA_STARTUP_CURRENT_DIRECTORY_IAT_RVA,
      ISAAC_VITA_STARTUP_CURRENT_DIRECTORY_CALL_RVA,
      ISAAC_VITA_STARTUP_CURRENT_DIRECTORY_RETURN_RVA,
      ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH,
      ISAAC_VITA_STARTUP_CURRENT_DIRECTORY_ORDINAL },
    { ISAAC_VITA_STARTUP_FULL_PATH_NAME,
      ISAAC_VITA_STARTUP_FULL_PATH_IAT_RVA,
      ISAAC_VITA_STARTUP_FULL_PATH_FILL_CALL_RVA,
      ISAAC_VITA_STARTUP_FULL_PATH_FILL_RETURN_RVA,
      ISAAC_VITA_STARTUP_FULL_PATH_REQUIRED,
      ISAAC_VITA_STARTUP_FULL_PATH_FILL_ORDINAL },
    { ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME,
      ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_IAT_RVA,
      ISAAC_VITA_STARTUP_SPI_GET_CALL_RVA,
      ISAAC_VITA_STARTUP_SPI_GET_RETURN_RVA,
      ISAAC_VITA_STARTUP_SPI_SAVED_TIMEOUT_VA,
      ISAAC_VITA_STARTUP_SPI_GET_ORDINAL },
    { ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME,
      ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_IAT_RVA,
      ISAAC_VITA_STARTUP_SPI_SET_ZERO_CALL_RVA,
      ISAAC_VITA_STARTUP_SPI_SET_ZERO_RETURN_RVA,
      ISAAC_VITA_STARTUP_SPI_SET_ZERO_VALUE,
      ISAAC_VITA_STARTUP_SPI_SET_ZERO_ORDINAL },
    { ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
      ISAAC_VITA_STARTUP_LOAD_LIBRARY_IAT_RVA,
      ISAAC_VITA_STARTUP_LOAD_WINMM_CALL_RVA,
      ISAAC_VITA_STARTUP_LOAD_WINMM_RETURN_RVA,
      ISAAC_VITA_STARTUP_LOAD_WINMM_NAME_VA,
      ISAAC_VITA_STARTUP_LOAD_WINMM_ORDINAL },
    { ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME,
      ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_IAT_RVA,
      ISAAC_VITA_STARTUP_SPI_RESTORE_CALL_RVA,
      ISAAC_VITA_STARTUP_SPI_RESTORE_RETURN_RVA,
      ISAAC_VITA_STARTUP_SPI_SAVED_TIMEOUT_VA,
      ISAAC_VITA_STARTUP_SPI_RESTORE_ORDINAL },
    { ISAAC_VITA_STARTUP_GET_LAST_ERROR_NAME,
      ISAAC_VITA_STARTUP_GET_LAST_ERROR_IAT_RVA,
      ISAAC_VITA_STARTUP_GET_LAST_ERROR_CALL_RVA,
      ISAAC_VITA_STARTUP_GET_LAST_ERROR_RETURN_RVA,
      ISAAC_VITA_STARTUP_ERROR_ALREADY_EXISTS,
      ISAAC_VITA_STARTUP_GET_LAST_ERROR_FIRST_ORDINAL }
};

_Static_assert(sizeof s_evidence / sizeof s_evidence[0] == 19U,
               "startup oracle lost an exact call-site record");

_Alignas(8) static uint32_t s_frame[16];
_Alignas(8) static uint8_t s_environment_output[72];
_Alignas(8) static uint8_t s_system_info[44];
_Alignas(8) static uint32_t s_frequency[4];
_Alignas(8) static uint32_t s_foreground_timeout[3];
_Alignas(8) static uint16_t s_full_path_input[] = {
    '.', '/', 0x00e9U, '.', 'd', 'a', 't', 0U
};
_Alignas(8) static uint16_t s_full_path_output[64];
_Alignas(8) static uint32_t s_full_path_file_part[3];
static const uint16_t s_full_path_suffix[] = {
    0x00e9U, '.', 'd', 'a', 't', 0U
};
static char s_environment_name[] = "mimalloc_verbose";
static char s_long_environment_name[ISAAC_VITA_STARTUP_ENV_NAME_MAX + 1U];
static char s_kernelbase[] = "kernelbase.dll";
static char s_ntdll[] = "ntdll.dll";
static char s_kernel32[] = "kernel32.dll";
static char s_userenv[] = ISAAC_VITA_STARTUP_LOAD_USERENV_MODULE;
static char s_winmm[] = ISAAC_VITA_STARTUP_LOAD_WINMM_MODULE;
static char s_unexpected_module[] = "opengl32.dll";
static CPU s_jump_cpu;
static unsigned s_jump_count;
static jmp_buf s_fault_jump;
static int s_fault_must_jump;

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint32_t prepare_call(CPU *c, uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t last_error = c->last_error;
    uint32_t esp;

    memset(c, 0, sizeof *c);
    c->last_error = last_error;
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[3]);
    s_frame[3] = 0x0badc0deU;
    s_frame[4] = a0;
    s_frame[5] = a1;
    s_frame[6] = a2;
    c->esp = esp;
    return esp;
}

static uint32_t prepare_call4(CPU *c, uint32_t a0, uint32_t a1,
                              uint32_t a2, uint32_t a3)
{
    uint32_t esp = prepare_call(c, a0, a1, a2);
    s_frame[7] = a3;
    return esp;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
    if (s_fault_must_jump)
        longjmp(s_fault_jump, 1);
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < sizeof s_evidence / sizeof s_evidence[0]; ++i) {
        const unsigned char *text =
            (const unsigned char *)s_evidence[i].name;
        const uint32_t values[] = {
            s_evidence[i].iat_rva,
            s_evidence[i].call_rva,
            s_evidence[i].return_rva,
            s_evidence[i].argument_evidence,
            s_evidence[i].boot_ordinal
        };
        uint32_t j;
        do {
            hash = hash_byte(hash, *text);
        } while (*text++ != 0U);
        for (j = 0U; j < sizeof values / sizeof values[0]; ++j) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash, (uint8_t)(values[j] >> shift));
        }
    }
    return hash;
}

static int unchanged_bytes(const uint8_t *bytes, uint32_t size,
                           uint8_t expected)
{
    uint32_t i;
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != expected)
            return 0;
    }
    return 1;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    unsigned calls = 0U;
    unsigned before;
    uint32_t esp;
    uint32_t info;
    uint32_t i;

    memset(&cpu, 0, sizeof cpu);

    if (!pointer_fits(s_frame) || !pointer_fits(s_environment_output) ||
        !pointer_fits(s_system_info) || !pointer_fits(s_frequency) ||
        !pointer_fits(s_foreground_timeout) ||
        !pointer_fits(s_full_path_input) ||
        !pointer_fits(s_full_path_output) ||
        !pointer_fits(s_full_path_file_part) ||
        !pointer_fits(s_environment_name) ||
        !pointer_fits(s_long_environment_name) ||
        !pointer_fits(s_kernelbase) || !pointer_fits(s_ntdll) ||
        !pointer_fits(s_kernel32) || !pointer_fits(s_userenv) ||
        !pointer_fits(s_winmm) ||
        !pointer_fits(s_unexpected_module))
        return 1;
    if (evidence_hash() != UINT64_C(0x1cb907cd6cad0597))
        return 2;
    if (ISAAC_VITA_STARTUP_ENV_IAT_VA != 0x98606094U ||
        ISAAC_VITA_STARTUP_ENV_BOOT_CALL_COUNT != 27U ||
        ISAAC_VITA_STARTUP_ENV_FIRST_ORDINAL != 27U ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_ORDINAL != 40U ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_ORDINAL != 44U ||
        ISAAC_VITA_STARTUP_ENV_LAST_ORDINAL != 53U ||
        strcmp(ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_NAME,
               "mimalloc_abandoned_page_reset") != 0 ||
        strcmp(ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_NAME,
               "mimalloc_reset_delay") != 0 ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_OPTION_RVA !=
            0x007a9120U ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_NAME_RVA !=
            0x006073ccU ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_OPTION_RVA != 0x007a915cU ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_NAME_RVA != 0x00607424U ||
        (ISAAC_VITA_STARTUP_ENV_OPTION_LAST_RVA -
         ISAAC_VITA_STARTUP_ENV_OPTION_FIRST_RVA) /
            ISAAC_VITA_STARTUP_ENV_OPTION_STRIDE + 1U != 25U ||
        ISAAC_VITA_STARTUP_SYSTEM_INFO_ORDINAL != 56U ||
        ISAAC_VITA_STARTUP_LOAD_KERNELBASE_ORDINAL != 57U ||
        ISAAC_VITA_STARTUP_LOAD_KERNEL32_ORDINAL != 59U ||
        ISAAC_VITA_STARTUP_LOAD_USERENV_CALL_RVA != 0x0050a878U ||
        ISAAC_VITA_STARTUP_LOAD_USERENV_RETURN_RVA != 0x0050a87eU ||
        ISAAC_VITA_STARTUP_LOAD_USERENV_NAME_RVA != 0x0075a89cU ||
        ISAAC_VITA_STARTUP_LOAD_USERENV_NAME_VA != 0x9875a89cU ||
        ISAAC_VITA_STARTUP_LOAD_USERENV_ORDINAL != 269U ||
        strcmp(ISAAC_VITA_STARTUP_LOAD_USERENV_MODULE, "userenv") != 0 ||
        strcmp(ISAAC_VITA_STARTUP_FREE_LIBRARY_NAME,
               "KERNEL32.dll!FreeLibrary") != 0 ||
        ISAAC_VITA_STARTUP_FREE_LIBRARY_IAT_RVA != 0x00606104U ||
        ISAAC_VITA_STARTUP_FREE_LIBRARY_IAT_VA != 0x98606104U ||
        ISAAC_VITA_STARTUP_FREE_USERENV_ARGUMENT_RVA != 0x0050a911U ||
        ISAAC_VITA_STARTUP_FREE_USERENV_CALL_RVA != 0x0050a912U ||
        ISAAC_VITA_STARTUP_FREE_USERENV_RETURN_RVA != 0x0050a918U ||
        ISAAC_VITA_STARTUP_FREE_USERENV_MODULE != 0U ||
        ISAAC_VITA_STARTUP_FREE_USERENV_ORDINAL != 271U ||
        ISAAC_VITA_STARTUP_USERENV_OWNED_CALL_COUNT != 2U ||
        strcmp(ISAAC_VITA_STARTUP_USERENV_NEXT_NAME,
               "api-ms-win-crt-environment-l1-1-0.dll!getenv") != 0 ||
        ISAAC_VITA_STARTUP_USERENV_NEXT_IAT_RVA != 0x006064e4U ||
        ISAAC_VITA_STARTUP_USERENV_NEXT_CALL_RVA != 0x0050aa0bU ||
        ISAAC_VITA_STARTUP_USERENV_NEXT_RETURN_RVA != 0x0050aa0dU ||
        ISAAC_VITA_STARTUP_USERENV_NEXT_ARGUMENT_VA != 0x9875a9a8U ||
        strcmp(ISAAC_VITA_STARTUP_USERENV_NEXT_ARGUMENT,
               "USERPROFILE") != 0 ||
        ISAAC_VITA_STARTUP_USERENV_NEXT_ATTEMPT_ORDINAL != 274U ||
        strcmp(ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME,
               "USER32.dll!SystemParametersInfoA") != 0 ||
        ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_IAT_RVA != 0x006063fcU ||
        ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_IAT_VA != 0x986063fcU ||
        ISAAC_VITA_STARTUP_SPI_GET_CALL_RVA != 0x005a745fU ||
        ISAAC_VITA_STARTUP_SPI_GET_RETURN_RVA != 0x005a7461U ||
        ISAAC_VITA_STARTUP_SPI_GET_ORDINAL != 524U ||
        ISAAC_VITA_STARTUP_SPI_SET_ZERO_CALL_RVA != 0x005a746cU ||
        ISAAC_VITA_STARTUP_SPI_SET_ZERO_RETURN_RVA != 0x005a746eU ||
        ISAAC_VITA_STARTUP_SPI_SET_ZERO_ORDINAL != 525U ||
        ISAAC_VITA_STARTUP_SPI_RESTORE_CALL_RVA != 0x005a7647U ||
        ISAAC_VITA_STARTUP_SPI_RESTORE_RETURN_RVA != 0x005a764dU ||
        ISAAC_VITA_STARTUP_SPI_RESTORE_ORDINAL != 527U ||
        ISAAC_VITA_STARTUP_SPI_SAVED_TIMEOUT_RVA != 0x007fe364U ||
        ISAAC_VITA_STARTUP_SPI_SAVED_TIMEOUT_VA != 0x987fe364U ||
        ISAAC_VITA_STARTUP_SPI_MEASURED_CALL_COUNT != 3U ||
        strcmp(ISAAC_VITA_STARTUP_LOAD_WINMM_MODULE, "winmm.dll") != 0 ||
        ISAAC_VITA_STARTUP_LOAD_WINMM_NAME_RVA != 0x0076707cU ||
        ISAAC_VITA_STARTUP_LOAD_WINMM_NAME_VA != 0x9876707cU ||
        ISAAC_VITA_STARTUP_LOAD_WINMM_CALL_RVA != 0x005a7479U ||
        ISAAC_VITA_STARTUP_LOAD_WINMM_RETURN_RVA != 0x005a747bU ||
        ISAAC_VITA_STARTUP_LOAD_WINMM_ORDINAL != 526U ||
        ISAAC_VITA_STARTUP_LATE_DESKTOP_CALL_COUNT != 4U ||
        ISAAC_VITA_STARTUP_QPF_ORDINAL != 64U ||
        ISAAC_VITA_STARTUP_NEXT_INITTERM_ORDINAL != 67U ||
        ISAAC_VITA_STARTUP_BOOT_CALL_COUNT != 32U)
        return 3;

    /* Unknown names are a mutation-free handoff to the shared dispatcher. */
    esp = prepare_call(&cpu, 1U, 2U, 3U);
    cpu.eax = 0x12345678U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_startup_import_counted(
            &cpu, "KERNEL32.dll!not_a_startup_import", &calls) != 0 ||
        calls != 0U || memcmp(&cpu, &snapshot, sizeof cpu) != 0 ||
        cpu.esp != esp)
        return 4;

    /* The name is the only memory contract needed by Vita's all-absent
     * environment policy.  Validate it before considering output arguments. */
    esp = prepare_call(&cpu, 0U, 0xffffffe0U, 256U);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_ENV_NAME, &calls) ||
        calls != 1U || cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault,
               "GetEnvironmentVariableA received a null name") != 0)
        return 5;
    memset(s_long_environment_name, 'x', sizeof s_long_environment_name);
    esp = prepare_call(&cpu, pointer32(s_long_environment_name), 0U, 256U);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_ENV_NAME, &calls) ||
        calls != 2U || cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault,
               "GetEnvironmentVariableA name exceeds measured bound") != 0)
        return 6;

    /* Vita reports every bounded name absent.  The exact frozen calls use
     * 65 bytes; neither that buffer nor a normal 256-byte caller is touched. */
    memset(s_environment_output, 0xa5, sizeof s_environment_output);
    esp = prepare_call(&cpu, pointer32(s_environment_name),
                       pointer32(&s_environment_output[3]),
                       ISAAC_VITA_STARTUP_ENV_BUFFER_SIZE);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_ENV_NAME, &calls) ||
        calls != 3U || cpu.fault || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_STARTUP_ERROR_ENVVAR_NOT_FOUND ||
        cpu.esp != esp + 16U ||
        s_frame[4] != pointer32(s_environment_name) ||
        s_frame[5] != pointer32(&s_environment_output[3]) ||
        s_frame[6] != ISAAC_VITA_STARTUP_ENV_BUFFER_SIZE ||
        !unchanged_bytes(s_environment_output,
                         sizeof s_environment_output, 0xa5U))
        return 7;

    memset(s_environment_output, 0x5a, sizeof s_environment_output);
    cpu.last_error = 0x11223344U;
    esp = prepare_call(&cpu, pointer32(s_environment_name),
                       pointer32(&s_environment_output[3]), 256U);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_ENV_NAME, &calls) ||
        calls != 4U || cpu.fault || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_STARTUP_ERROR_ENVVAR_NOT_FOUND ||
        cpu.esp != esp + 16U ||
        !unchanged_bytes(s_environment_output,
                         sizeof s_environment_output, 0x5aU))
        return 8;

    /* lpBuffer is optional.  Even a nonzero normal capacity with NULL has no
     * output contract when the variable is absent. */
    cpu.last_error = 0x55667788U;
    esp = prepare_call(&cpu, pointer32(s_environment_name), 0U, 256U);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_ENV_NAME, &calls) ||
        calls != 5U || cpu.fault || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_STARTUP_ERROR_ENVVAR_NOT_FOUND ||
        cpu.esp != esp + 16U)
        return 9;

    /* The all-absent policy must not turn a foreign output range into a guest
     * fault either; this call is deliberately outside the progress counter. */
    cpu.last_error = 0xaabbccddU;
    esp = prepare_call(&cpu, pointer32(s_environment_name),
                       0xffffffe0U, 256U);
    if (!isaac_vita_startup_import(
            &cpu, ISAAC_VITA_STARTUP_ENV_NAME) ||
        calls != 5U || cpu.fault || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_STARTUP_ERROR_ENVVAR_NOT_FOUND ||
        cpu.esp != esp + 16U)
        return 26;

    /* GetSystemInfo writes exactly the 36-byte x86 structure and is void. */
    memset(s_system_info, 0xa5, sizeof s_system_info);
    info = pointer32(&s_system_info[4]);
    esp = prepare_call(&cpu, info, 0U, 0U);
    cpu.eax = 0x89abcdefU;
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_SYSTEM_INFO_NAME, &calls) ||
        calls != 6U || cpu.fault || cpu.eax != 0x89abcdefU ||
        cpu.esp != esp + 8U ||
        ld16(info + 0U) != ISAAC_VITA_STARTUP_PROCESSOR_ARCHITECTURE ||
        ld16(info + 2U) != 0U ||
        ld32(info + 4U) != ISAAC_VITA_STARTUP_PAGE_SIZE ||
        ld32(info + 8U) != ISAAC_VITA_STARTUP_MIN_APP_ADDRESS ||
        ld32(info + 12U) != ISAAC_VITA_STARTUP_MAX_APP_ADDRESS ||
        ld32(info + 16U) != ISAAC_VITA_STARTUP_ACTIVE_PROCESSOR_MASK ||
        ld32(info + 20U) != ISAAC_VITA_STARTUP_PROCESSOR_COUNT ||
        ld32(info + 24U) != ISAAC_VITA_STARTUP_PROCESSOR_TYPE ||
        ld32(info + 28U) != ISAAC_VITA_STARTUP_ALLOCATION_GRANULARITY ||
        ld16(info + 32U) != ISAAC_VITA_STARTUP_PROCESSOR_LEVEL ||
        ld16(info + 34U) != ISAAC_VITA_STARTUP_PROCESSOR_REVISION)
        return 10;
    for (i = 0U; i < 4U; ++i) {
        if (s_system_info[i] != 0xa5U ||
            s_system_info[40U + i] != 0xa5U)
            return 11;
    }
    esp = prepare_call(&cpu, 0U, 0U, 0U);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_SYSTEM_INFO_NAME, &calls) ||
        calls != 7U || cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault,
               "GetSystemInfo received an invalid output range") != 0)
        return 12;

    /* The three early allocator modules and late USERENV probe are the only
     * recognized loads.  Each is explicitly absent on Vita. */
    {
        char *modules[4] = { s_kernelbase, s_ntdll, s_kernel32, s_userenv };
        for (i = 0U; i < 4U; ++i) {
            esp = prepare_call(&cpu, pointer32(modules[i]), 0U, 0U);
            cpu.eax = 0xffffffffU;
            if (!isaac_vita_startup_import_counted(
                    &cpu, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME, &calls) ||
                calls != 8U + i || cpu.fault || cpu.eax != 0U ||
                cpu.esp != esp + 8U ||
                s_frame[4] != pointer32(modules[i]))
                return 13;
        }
    }
    esp = prepare_call(&cpu, pointer32(s_unexpected_module), 0U, 0U);
    before = calls;
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault,
               "LoadLibraryA received an unexpected module") != 0)
        return 14;

    /* QPF is exactly the microsecond frequency used by the existing QPC. */
    s_frequency[0] = 0x11223344U;
    s_frequency[1] = 0xccccccccU;
    s_frequency[2] = 0xccccccccU;
    s_frequency[3] = 0x55667788U;
    esp = prepare_call(&cpu, pointer32(&s_frequency[1]), 0U, 0U);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_QPF_NAME, &calls) ||
        calls != 13U || cpu.fault || cpu.eax != 1U ||
        cpu.esp != esp + 8U ||
        s_frequency[0] != 0x11223344U ||
        s_frequency[1] != ISAAC_VITA_STARTUP_QPF_FREQUENCY ||
        s_frequency[2] != 0U || s_frequency[3] != 0x55667788U)
        return 15;

    /* FreeLibrary(NULL) is the exact unconditional cleanup after the absent
     * procedure.  It returns FALSE and cleans its one stdcall argument. */
    esp = prepare_call(&cpu, ISAAC_VITA_STARTUP_FREE_USERENV_MODULE, 0U, 0U);
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_FREE_LIBRARY_NAME, &calls) ||
        calls != 14U || cpu.fault || cpu.eax != 0U ||
        cpu.esp != esp + 8U ||
        s_frame[4] != ISAAC_VITA_STARTUP_FREE_USERENV_MODULE)
        return 16;

    /* Count-before-handler survives the same nonlocal control transfer used
     * by production guest_fault.  A non-NULL token was never issued and
     * therefore remains loud. */
    esp = prepare_call(&s_jump_cpu, 0x7f0000ffU, 0U, 0U);
    s_jump_cpu.eax = 0x2468ace0U;
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_startup_import_counted(
            &s_jump_cpu, ISAAC_VITA_STARTUP_FREE_LIBRARY_NAME,
            &s_jump_count);
        return 17;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != 15U || s_jump_cpu.esp != esp ||
        s_jump_cpu.eax != 0x2468ace0U ||
        s_jump_cpu.fault_addr != 0x7f0000ffU ||
        !s_jump_cpu.fault || strcmp(s_jump_cpu.fault,
            "FreeLibrary received an unexpected module token") != 0)
        return 18;

    /* The optional desktop module is absent on Vita, exactly like the other
     * measured optional modules: NULL takes GLFW's normal failure path. */
    esp = prepare_call(&cpu, pointer32(s_winmm), 0U, 0U);
    cpu.eax = 0xffffffffU;
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME, &calls) ||
        calls != 15U || cpu.fault || cpu.eax != 0U ||
        cpu.esp != esp + 8U || s_frame[4] != pointer32(s_winmm))
        return 19;

    /* The desktop timeout is private guest-visible state.  GET writes the
     * saved DWORD, SET consumes pvParam as the value, and neither reaches the
     * Vita or build-host desktop. */
    s_foreground_timeout[0] = 0x11223344U;
    s_foreground_timeout[1] = 0xa5a5a5a5U;
    s_foreground_timeout[2] = 0x55667788U;
    esp = prepare_call4(
        &cpu, ISAAC_VITA_STARTUP_SPI_GET_FOREGROUND_LOCK_TIMEOUT, 0U,
        pointer32(&s_foreground_timeout[1]),
        ISAAC_VITA_STARTUP_SPI_UPDATE_FLAGS_NONE);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME, &calls) ||
        calls != 16U || cpu.fault || cpu.eax != 1U ||
        cpu.esp != esp + 20U ||
        s_foreground_timeout[0] != 0x11223344U ||
        s_foreground_timeout[1] != 0U ||
        s_foreground_timeout[2] != 0x55667788U)
        return 20;
    esp = prepare_call4(
        &cpu, ISAAC_VITA_STARTUP_SPI_SET_FOREGROUND_LOCK_TIMEOUT, 0U,
        ISAAC_VITA_STARTUP_SPI_SET_ZERO_VALUE,
        ISAAC_VITA_STARTUP_SPI_UPDATE_FLAGS_SEND_CHANGE);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME, &calls) ||
        calls != 17U || cpu.fault || cpu.eax != 1U ||
        cpu.esp != esp + 20U)
        return 21;
    s_foreground_timeout[1] = 0xa5a5a5a5U;
    esp = prepare_call4(
        &cpu, ISAAC_VITA_STARTUP_SPI_GET_FOREGROUND_LOCK_TIMEOUT, 0U,
        pointer32(&s_foreground_timeout[1]),
        ISAAC_VITA_STARTUP_SPI_UPDATE_FLAGS_NONE);
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME, &calls) ||
        calls != 18U || cpu.fault || cpu.eax != 1U ||
        cpu.esp != esp + 20U || s_foreground_timeout[1] != 0U)
        return 22;

    /* Count-before remains observable when an unsupported action faults via
     * the production nonlocal-control path. */
    esp = prepare_call4(&s_jump_cpu, 0xdead2000U, 0U, 0U, 0U);
    s_jump_cpu.eax = 0x13579bdfU;
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_startup_import_counted(
            &s_jump_cpu, ISAAC_VITA_STARTUP_SYSTEM_PARAMETERS_NAME,
            &s_jump_count);
        return 23;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != 19U || s_jump_cpu.esp != esp ||
        s_jump_cpu.eax != 0x13579bdfU ||
        s_jump_cpu.fault_addr != 0xdead2000U ||
        !s_jump_cpu.fault || strcmp(s_jump_cpu.fault,
            "SystemParametersInfoA received unsupported action") != 0)
        return 24;

    /* The optional winmm probe most recently set ERROR_MOD_NOT_FOUND.  GET
     * is stdcall with no arguments and must not clear the compatibility cell. */
    esp = prepare_call(&cpu, 0U, 0U, 0U);
    cpu.eax = 0xccccccccU;
    if (!isaac_vita_startup_import_counted(
            &cpu, ISAAC_VITA_STARTUP_GET_LAST_ERROR_NAME, &calls) ||
        calls != 19U || cpu.fault ||
        cpu.eax != ISAAC_VITA_STARTUP_ERROR_MOD_NOT_FOUND ||
        cpu.esp != esp + 4U)
        return 25;

    /* The PE reaches GetFullPathNameW only through C-locale mbstowcs_s, so
     * U+00xx is its complete non-ASCII input domain.  Query and fill must
     * preserve U+00e9 and point lpFilePart at that first suffix unit. */
    before = calls;
    esp = prepare_call4(&cpu, pointer32(s_full_path_input), 0U, 0U, 0U);
    if (!isaac_vita_startup_import(
            &cpu, ISAAC_VITA_STARTUP_FULL_PATH_NAME) ||
        calls != before || cpu.fault ||
        cpu.eax != ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH + 7U ||
        cpu.esp != esp + 20U)
        return 28;

    memset(s_full_path_output, 0xcc, sizeof s_full_path_output);
    s_full_path_file_part[0] = 0x11223344U;
    s_full_path_file_part[1] = 0xccccccccU;
    s_full_path_file_part[2] = 0x55667788U;
    esp = prepare_call4(
        &cpu, pointer32(s_full_path_input),
        (uint32_t)(sizeof s_full_path_output / sizeof s_full_path_output[0]),
        pointer32(s_full_path_output), pointer32(&s_full_path_file_part[1]));
    if (!isaac_vita_startup_import(
            &cpu, ISAAC_VITA_STARTUP_FULL_PATH_NAME) ||
        calls != before || cpu.fault ||
        cpu.eax != ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH + 6U ||
        cpu.esp != esp + 20U ||
        s_full_path_file_part[0] != 0x11223344U ||
        s_full_path_file_part[1] !=
            pointer32(&s_full_path_output[
                ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH + 1U]) ||
        s_full_path_file_part[2] != 0x55667788U)
        return 29;
    for (i = 0U; i < ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH; ++i) {
        if (s_full_path_output[i] != (uint8_t)ISAAC_VITA_DATA_ROOT[i])
            return 30;
    }
    if (s_full_path_output[ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH] != '/' ||
        memcmp(&s_full_path_output[
                   ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH + 1U],
               s_full_path_suffix, sizeof s_full_path_suffix) != 0 ||
        s_full_path_output[
            ISAAC_VITA_STARTUP_FULL_PATH_ROOT_LENGTH + 7U] != 0xccccU)
        return 31;
    if (guest_stack_legacy_oracle_violation_calls() != 0U)
        return 27;

    return 0;
}
