/* Executable softfp ARM oracle for the Vita synchronization frontier. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/thread.h>

#include "guest.h"
#include "host_vita_console.h"
#include "host_vita_crt.h"
#include "host_vita_fls.h"
#include "host_vita_memory.h"
#include "host_vita_startup.h"
#include "host_vita_sync.h"
#include "platform.h"
#include "vita_boot_imports.h"
#include "vita_host_services.h"
#include "vita_sync_services.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita synchronization oracle requires a 32-bit identity-mapped host
#endif

typedef struct sync_call_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t call_rva;
    uint32_t return_rva;
} sync_call_evidence;

typedef struct sync_physical_call_site {
    uint32_t call_rva;
    uint32_t return_rva;
} sync_physical_call_site;

enum { CALLBACK_BOOT23 = 0x10002300U };

static const sync_call_evidence s_evidence[] = {
    { ISAAC_VITA_SYNC_INIT_CS_NAME,
      ISAAC_VITA_SYNC_INIT_CS_IAT_RVA,
      ISAAC_VITA_SYNC_INIT_CS_CALL_RVA,
      ISAAC_VITA_SYNC_INIT_CS_RETURN_RVA },
    { ISAAC_VITA_SYNC_GET_MODULE_NAME,
      ISAAC_VITA_SYNC_GET_MODULE_IAT_RVA,
      ISAAC_VITA_SYNC_GET_APISET_CALL_RVA,
      ISAAC_VITA_SYNC_GET_APISET_RETURN_RVA },
    { ISAAC_VITA_SYNC_GET_MODULE_NAME,
      ISAAC_VITA_SYNC_GET_MODULE_IAT_RVA,
      ISAAC_VITA_SYNC_GET_KERNEL32_CALL_RVA,
      ISAAC_VITA_SYNC_GET_KERNEL32_RETURN_RVA },
    { ISAAC_VITA_SYNC_GET_PROC_NAME,
      ISAAC_VITA_SYNC_GET_PROC_IAT_RVA,
      ISAAC_VITA_SYNC_GET_SLEEP_CALL_RVA,
      ISAAC_VITA_SYNC_GET_SLEEP_RETURN_RVA },
    { ISAAC_VITA_SYNC_GET_PROC_NAME,
      ISAAC_VITA_SYNC_GET_PROC_IAT_RVA,
      ISAAC_VITA_SYNC_GET_WAKE_ALL_CALL_RVA,
      ISAAC_VITA_SYNC_GET_WAKE_ALL_RETURN_RVA },
    { ISAAC_VITA_SYNC_CREATE_EVENT_NAME,
      ISAAC_VITA_SYNC_CREATE_EVENT_IAT_RVA,
      ISAAC_VITA_SYNC_CREATE_EVENT_CALL_RVA,
      ISAAC_VITA_SYNC_CREATE_EVENT_RETURN_RVA }
};

static const sync_call_evidence s_plain_cs_sites[] = {
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE0_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE0_RETURN_RVA },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE1_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE1_RETURN_RVA },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE2_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE2_RETURN_RVA },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE3_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE3_RETURN_RVA },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE4_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE4_RETURN_RVA },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE5_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE5_RETURN_RVA },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME,
      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE6_CALL_RVA,
      ISAAC_VITA_SYNC_PLAIN_CS_SITE6_RETURN_RVA }
};

static const sync_call_evidence s_userenv_getproc_evidence[] = {
    { ISAAC_VITA_SYNC_GET_PROC_NAME,
      ISAAC_VITA_SYNC_GET_PROC_IAT_RVA,
      ISAAC_VITA_SYNC_GET_USER_PROFILE_CALL_RVA,
      ISAAC_VITA_SYNC_GET_USER_PROFILE_RETURN_RVA }
};

static const uint32_t s_set_event_references[] = {
    0x005eb2e3U,
};
static const sync_physical_call_site s_set_event_calls[] = {
    { 0x005eb2e3U, 0x005eb2e9U },
};

static const uint32_t s_reset_event_references[] = {
    0x005eb2efU,
};
static const sync_physical_call_site s_reset_event_calls[] = {
    { 0x005eb2efU, 0x005eb2f5U },
};

/* Ten direct FF/15 calls plus two register-load/call pairs.  Only the final
 * 0x5eb1fa site owns the CreateEventW event; every other handle kind remains
 * a loud rejection at the current narrow implementation frontier. */
static const uint32_t s_close_handle_references[] = {
    0x0048bb8eU, 0x0050a8edU, 0x00598a08U, 0x00598c12U,
    0x005a5854U, 0x005a586dU, 0x005a5883U, 0x005ac7ffU,
    0x005aca9fU, 0x005ad640U, 0x005e7953U, 0x005eb1faU,
};
static const uint32_t s_close_handle_direct_calls[] = {
    0x0048bb8eU, 0x0050a8edU, 0x00598a08U, 0x00598c12U,
    0x005a5883U, 0x005ac7ffU, 0x005aca9fU, 0x005ad640U,
    0x005e7953U, 0x005eb1faU,
};
static const uint32_t s_close_handle_loads[] = {
    0x005a5854U, 0x005a586dU,
};
static const uint32_t s_close_handle_register_calls[] = {
    0x005a5859U, 0x005a5872U,
};
static const sync_physical_call_site s_close_handle_calls[] = {
    { 0x0048bb8eU, 0x0048bb94U },
    { 0x0050a8edU, 0x0050a8f3U },
    { 0x00598a08U, 0x00598a0eU },
    { 0x00598c12U, 0x00598c18U },
    { 0x005a5859U, 0x005a585bU },
    { 0x005a5872U, 0x005a5874U },
    { 0x005a5883U, 0x005a5889U },
    { 0x005ac7ffU, 0x005ac805U },
    { 0x005aca9fU, 0x005acaa5U },
    { 0x005ad640U, 0x005ad646U },
    { 0x005e7953U, 0x005e7959U },
    { 0x005eb1faU, 0x005eb200U },
};

/* Five direct FF/15 references and three register loads.  The ESI loaded at
 * 0x59818b is invoked twice, so the physical-call list has nine entries. */
static const uint32_t s_get_module_a_references[] = {
    0x004a4b90U, 0x004a4bccU, 0x00561967U, 0x005699aeU,
    0x00598072U, 0x0059818bU, 0x0059ca4cU, 0x005a761cU,
};
static const uint32_t s_get_module_a_direct_calls[] = {
    0x00561967U, 0x005699aeU, 0x00598072U, 0x0059ca4cU,
    0x005a761cU,
};
static const uint32_t s_get_module_a_loads[] = {
    0x004a4b90U, 0x004a4bccU, 0x0059818bU,
};
static const uint32_t s_get_module_a_register_calls[] = {
    0x004a4b9dU, 0x004a4bd9U, 0x005981afU, 0x005981e0U,
};
static const sync_physical_call_site s_get_module_a_calls[] = {
    { 0x004a4b9dU, 0x004a4b9fU },
    { 0x004a4bd9U, 0x004a4bdbU },
    { 0x00561967U, 0x0056196dU },
    { 0x005699aeU, 0x005699b4U },
    { 0x00598072U, 0x00598078U },
    { 0x005981afU, 0x005981b1U },
    { 0x005981e0U, 0x005981e2U },
    { 0x0059ca4cU, 0x0059ca52U },
    { 0x005a761cU, 0x005a7622U },
};

/* Five direct FF/15 calls plus eight register-load/call pairs. */
static const uint32_t s_sleep_references[] = {
    0x00476c08U, 0x0048bea9U, 0x0055e3dfU, 0x00562e35U,
    0x00562e98U, 0x00569e01U, 0x0056e73eU, 0x00598b15U,
    0x005a565fU, 0x005a7720U, 0x005a7795U, 0x005ad302U,
    0x005c2e07U,
};
static const uint32_t s_sleep_direct_calls[] = {
    0x00476c08U, 0x0048bea9U, 0x00562e98U, 0x00598b15U,
    0x005ad302U,
};
static const uint32_t s_sleep_loads[] = {
    0x0055e3dfU, 0x00562e35U, 0x00569e01U, 0x0056e73eU,
    0x005a565fU, 0x005a7720U, 0x005a7795U, 0x005c2e07U,
};
static const uint32_t s_sleep_register_calls[] = {
    0x0055e3eaU, 0x00562e45U, 0x00569e50U, 0x0056e753U,
    0x005a5667U, 0x005a772bU, 0x005a77a5U, 0x005c2ea1U,
};
static const sync_physical_call_site s_sleep_calls[] = {
    { 0x00476c08U, 0x00476c0eU },
    { 0x0048bea9U, 0x0048beafU },
    { 0x0055e3eaU, 0x0055e3ecU },
    { 0x00562e45U, 0x00562e47U },
    { 0x00562e98U, 0x00562e9eU },
    { 0x00569e50U, 0x00569e52U },
    { 0x0056e753U, 0x0056e755U },
    { 0x00598b15U, 0x00598b1bU },
    { 0x005a5667U, 0x005a5669U },
    { 0x005a772bU, 0x005a772dU },
    { 0x005a77a5U, 0x005a77a7U },
    { 0x005ad302U, 0x005ad308U },
    { 0x005c2ea1U, 0x005c2ea3U },
};

_Static_assert(sizeof s_evidence / sizeof s_evidence[0] ==
               ISAAC_VITA_SYNC_CALL_COUNT,
               "sync evidence lost a measured call");
_Static_assert(sizeof s_plain_cs_sites / sizeof s_plain_cs_sites[0] ==
               ISAAC_VITA_SYNC_PLAIN_CS_SITE_COUNT,
               "plain critical-section physical-site evidence drifted");
_Static_assert(sizeof s_userenv_getproc_evidence /
                   sizeof s_userenv_getproc_evidence[0] ==
               ISAAC_VITA_SYNC_LATE_GET_PROC_CALL_COUNT,
               "USERENV GetProcAddress evidence drifted");
_Static_assert(sizeof s_set_event_references /
                   sizeof s_set_event_references[0] ==
               ISAAC_VITA_SYNC_SET_EVENT_REFERENCE_COUNT,
               "SetEvent reference census drifted");
_Static_assert(sizeof s_set_event_calls / sizeof s_set_event_calls[0] ==
               ISAAC_VITA_SYNC_SET_EVENT_CALL_COUNT,
               "SetEvent physical-call census drifted");
_Static_assert(sizeof s_reset_event_references /
                   sizeof s_reset_event_references[0] ==
               ISAAC_VITA_SYNC_RESET_EVENT_REFERENCE_COUNT,
               "ResetEvent reference census drifted");
_Static_assert(sizeof s_reset_event_calls / sizeof s_reset_event_calls[0] ==
               ISAAC_VITA_SYNC_RESET_EVENT_CALL_COUNT,
               "ResetEvent physical-call census drifted");
_Static_assert(sizeof s_close_handle_references /
                   sizeof s_close_handle_references[0] ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_REFERENCE_COUNT,
               "CloseHandle reference census drifted");
_Static_assert(sizeof s_close_handle_direct_calls /
                   sizeof s_close_handle_direct_calls[0] ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_DIRECT_COUNT,
               "CloseHandle direct-call census drifted");
_Static_assert(sizeof s_close_handle_loads /
                   sizeof s_close_handle_loads[0] ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_LOAD_COUNT,
               "CloseHandle load census drifted");
_Static_assert(sizeof s_close_handle_register_calls /
                   sizeof s_close_handle_register_calls[0] ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_REGISTER_CALL_COUNT,
               "CloseHandle register-call census drifted");
_Static_assert(sizeof s_close_handle_calls /
                   sizeof s_close_handle_calls[0] ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_COUNT,
               "CloseHandle physical-call census drifted");
_Static_assert(sizeof s_get_module_a_references /
                   sizeof s_get_module_a_references[0] ==
               ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_COUNT,
               "GetModuleHandleA reference census drifted");
_Static_assert(sizeof s_get_module_a_calls /
                   sizeof s_get_module_a_calls[0] ==
               ISAAC_VITA_SYNC_GET_MODULE_A_CALL_COUNT,
               "GetModuleHandleA physical-call census drifted");
_Static_assert(sizeof s_get_module_a_direct_calls /
                   sizeof s_get_module_a_direct_calls[0] ==
               ISAAC_VITA_SYNC_GET_MODULE_A_DIRECT_COUNT,
               "GetModuleHandleA direct-call census drifted");
_Static_assert(sizeof s_get_module_a_loads /
                   sizeof s_get_module_a_loads[0] ==
               ISAAC_VITA_SYNC_GET_MODULE_A_LOAD_COUNT,
               "GetModuleHandleA load census drifted");
_Static_assert(sizeof s_get_module_a_register_calls /
                   sizeof s_get_module_a_register_calls[0] ==
               ISAAC_VITA_SYNC_GET_MODULE_A_REGISTER_CALL_COUNT,
               "GetModuleHandleA register-call census drifted");
_Static_assert(sizeof s_sleep_references / sizeof s_sleep_references[0] ==
               ISAAC_VITA_SYNC_SLEEP_REFERENCE_COUNT,
               "Sleep reference census drifted");
_Static_assert(sizeof s_sleep_calls / sizeof s_sleep_calls[0] ==
               ISAAC_VITA_SYNC_SLEEP_CALL_COUNT,
               "Sleep physical-call census drifted");
_Static_assert(sizeof s_sleep_direct_calls /
                   sizeof s_sleep_direct_calls[0] ==
               ISAAC_VITA_SYNC_SLEEP_DIRECT_COUNT,
               "Sleep direct-call census drifted");
_Static_assert(sizeof s_sleep_loads / sizeof s_sleep_loads[0] ==
               ISAAC_VITA_SYNC_SLEEP_LOAD_COUNT,
               "Sleep load census drifted");
_Static_assert(sizeof s_sleep_register_calls /
                   sizeof s_sleep_register_calls[0] ==
               ISAAC_VITA_SYNC_SLEEP_REGISTER_CALL_COUNT,
               "Sleep register-call census drifted");
_Static_assert(ISAAC_VITA_SYNC_CS_SIZE == 6U * sizeof(uint32_t),
               "critical section guest layout is not exactly 24 bytes");

_Alignas(8) static uint32_t s_frame[16];
_Alignas(8) static uint32_t s_critical[8];
_Alignas(8) static uint32_t
    s_plain_critical[ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT][8];
_Alignas(8) static uint32_t
    s_plain_snapshot[ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT][8];
_Alignas(8) static uint32_t s_plain_failure[8];
_Alignas(8) static uint32_t s_lw_fallback_critical[3][8];
_Alignas(8) static uint32_t s_cached_thread_critical[8];
_Alignas(8) static uint32_t s_embedded_stress[4097][8];
_Alignas(8) static uint16_t s_module_api[64];
_Alignas(8) static uint16_t s_module_kernel[64];
_Alignas(8) static uint16_t s_module_upper[64];
_Alignas(8) static uint16_t s_module_other[64];
_Alignas(8) static uint32_t s_boot_frame[10];
_Alignas(8) static uint32_t s_initializer_table[1];
_Alignas(8) static uint32_t s_slist[4];
_Alignas(8) static uint32_t s_boot_memory[32];
_Alignas(8) static uint8_t s_environment_output[72];
_Alignas(8) static uint8_t s_system_info[40];
_Alignas(8) static uint32_t s_late_memory[8];
_Alignas(8) static uint32_t s_performance[8];
static char s_environment_name[] = "mimalloc_verbose";
static char s_environment_fallback_abandoned[] =
    ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_NAME;
static char s_environment_fallback_reset[] =
    ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_NAME;
static char s_kernelbase[] = "kernelbase.dll";
static char s_ntdll[] = "ntdll.dll";
static char s_kernel32[] = "kernel32.dll";
static char s_module_a_upper[] = "KERNEL32.DLL";
static char s_module_a_other[] = "user32.dll";
static char s_sleep_name[64];
static char s_wake_name[64];
static char s_user_profile_name[64];
static char s_wrong_proc[64];

static unsigned s_bad_mock_call;
static int32_t s_thread_id = (int32_t)UINT32_C(0x40010003);
static unsigned s_thread_id_calls;

static unsigned s_event_create_calls;
static unsigned s_event_set_calls;
static unsigned s_event_clear_calls;
static unsigned s_event_wait_calls;
static unsigned s_event_delete_calls;
static int s_event_force_create;
static int32_t s_event_create_result;
static int32_t s_event_next_uid;
static int32_t s_event_uid;
static int s_event_active;
static uint32_t s_event_bits;
static int s_event_force_set;
static int32_t s_event_set_result;
static int s_event_force_clear;
static int32_t s_event_clear_result;
static int s_event_force_delete;
static int32_t s_event_delete_result;

static unsigned s_delay_calls;
static uint32_t s_delay_values[8];
static int s_delay_force;
static int32_t s_delay_result;

static unsigned s_log_calls;
static char s_log_operation[48];
static uint32_t s_log_result;
static unsigned s_boot23_status;
static unsigned s_boot23_initializer_calls;
static uint32_t s_boot_fls_index;

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    *filetime = 0U;
    return 0;
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

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
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

static void clear_fault(CPU *c)
{
    c->fault = NULL;
    c->fault_addr = 0U;
}

static void make_utf16(uint16_t output[64], const char *input)
{
    uint32_t i;
    memset(output, 0, 64U * sizeof output[0]);
    for (i = 0U; input[i] && i < 63U; ++i)
        output[i] = (uint16_t)(unsigned char)input[i];
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t rva_sequence_hash(const uint32_t *values, uint32_t count)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < count; ++i) {
        uint32_t shift;
        for (shift = 0U; shift < 32U; shift += 8U)
            hash = hash_byte(hash, (uint8_t)(values[i] >> shift));
    }
    return hash;
}

static uint64_t physical_call_hash(const sync_physical_call_site *sites,
                                   uint32_t count, int include_returns)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < count; ++i) {
        uint32_t values[2];
        uint32_t value_count = include_returns ? 2U : 1U;
        uint32_t j;
        values[0] = sites[i].call_rva;
        values[1] = sites[i].return_rva;
        for (j = 0U; j < value_count; ++j) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash, (uint8_t)(values[j] >> shift));
        }
    }
    return hash;
}

static int pc_hit_evidence_valid(void)
{
    return rva_sequence_hash(
               s_set_event_references,
               ISAAC_VITA_SYNC_SET_EVENT_REFERENCE_COUNT) ==
               ISAAC_VITA_SYNC_SET_EVENT_REFERENCE_FNV64 &&
           physical_call_hash(
               s_set_event_calls, ISAAC_VITA_SYNC_SET_EVENT_CALL_COUNT, 0) ==
               ISAAC_VITA_SYNC_SET_EVENT_CALL_FNV64 &&
           physical_call_hash(
               s_set_event_calls, ISAAC_VITA_SYNC_SET_EVENT_CALL_COUNT, 1) ==
               ISAAC_VITA_SYNC_SET_EVENT_CALL_RETURN_FNV64 &&
           rva_sequence_hash(
               s_reset_event_references,
               ISAAC_VITA_SYNC_RESET_EVENT_REFERENCE_COUNT) ==
               ISAAC_VITA_SYNC_RESET_EVENT_REFERENCE_FNV64 &&
           physical_call_hash(
               s_reset_event_calls,
               ISAAC_VITA_SYNC_RESET_EVENT_CALL_COUNT, 0) ==
               ISAAC_VITA_SYNC_RESET_EVENT_CALL_FNV64 &&
           physical_call_hash(
               s_reset_event_calls,
               ISAAC_VITA_SYNC_RESET_EVENT_CALL_COUNT, 1) ==
               ISAAC_VITA_SYNC_RESET_EVENT_CALL_RETURN_FNV64 &&
           rva_sequence_hash(
               s_get_module_a_references,
               ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_COUNT) ==
               ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_FNV64 &&
           rva_sequence_hash(
               s_get_module_a_direct_calls,
               ISAAC_VITA_SYNC_GET_MODULE_A_DIRECT_COUNT) ==
               ISAAC_VITA_SYNC_GET_MODULE_A_DIRECT_FNV64 &&
           rva_sequence_hash(
               s_get_module_a_loads,
               ISAAC_VITA_SYNC_GET_MODULE_A_LOAD_COUNT) ==
               ISAAC_VITA_SYNC_GET_MODULE_A_LOAD_FNV64 &&
           rva_sequence_hash(
               s_get_module_a_register_calls,
               ISAAC_VITA_SYNC_GET_MODULE_A_REGISTER_CALL_COUNT) ==
               ISAAC_VITA_SYNC_GET_MODULE_A_REGISTER_CALL_FNV64 &&
           physical_call_hash(
               s_get_module_a_calls,
               ISAAC_VITA_SYNC_GET_MODULE_A_CALL_COUNT, 0) ==
               ISAAC_VITA_SYNC_GET_MODULE_A_CALL_FNV64 &&
           physical_call_hash(
               s_get_module_a_calls,
               ISAAC_VITA_SYNC_GET_MODULE_A_CALL_COUNT, 1) ==
               ISAAC_VITA_SYNC_GET_MODULE_A_CALL_RETURN_FNV64 &&
           rva_sequence_hash(
               s_sleep_references,
               ISAAC_VITA_SYNC_SLEEP_REFERENCE_COUNT) ==
               ISAAC_VITA_SYNC_SLEEP_REFERENCE_FNV64 &&
           rva_sequence_hash(
               s_sleep_direct_calls,
               ISAAC_VITA_SYNC_SLEEP_DIRECT_COUNT) ==
               ISAAC_VITA_SYNC_SLEEP_DIRECT_FNV64 &&
           rva_sequence_hash(
               s_sleep_loads,
               ISAAC_VITA_SYNC_SLEEP_LOAD_COUNT) ==
               ISAAC_VITA_SYNC_SLEEP_LOAD_FNV64 &&
           rva_sequence_hash(
               s_sleep_register_calls,
               ISAAC_VITA_SYNC_SLEEP_REGISTER_CALL_COUNT) ==
               ISAAC_VITA_SYNC_SLEEP_REGISTER_CALL_FNV64 &&
           physical_call_hash(
               s_sleep_calls, ISAAC_VITA_SYNC_SLEEP_CALL_COUNT, 0) ==
               ISAAC_VITA_SYNC_SLEEP_CALL_FNV64 &&
           physical_call_hash(
               s_sleep_calls, ISAAC_VITA_SYNC_SLEEP_CALL_COUNT, 1) ==
               ISAAC_VITA_SYNC_SLEEP_CALL_RETURN_FNV64;
}

static int close_handle_evidence_valid(void)
{
    return rva_sequence_hash(
               s_close_handle_references,
               ISAAC_VITA_SYNC_CLOSE_HANDLE_REFERENCE_COUNT) ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_REFERENCE_FNV64 &&
           rva_sequence_hash(
               s_close_handle_direct_calls,
               ISAAC_VITA_SYNC_CLOSE_HANDLE_DIRECT_COUNT) ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_DIRECT_FNV64 &&
           rva_sequence_hash(
               s_close_handle_loads,
               ISAAC_VITA_SYNC_CLOSE_HANDLE_LOAD_COUNT) ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_LOAD_FNV64 &&
           rva_sequence_hash(
               s_close_handle_register_calls,
               ISAAC_VITA_SYNC_CLOSE_HANDLE_REGISTER_CALL_COUNT) ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_REGISTER_CALL_FNV64 &&
           physical_call_hash(
               s_close_handle_calls,
               ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_COUNT, 0) ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_FNV64 &&
           physical_call_hash(
               s_close_handle_calls,
               ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_COUNT, 1) ==
               ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_RETURN_FNV64;
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_SYNC_CALL_COUNT; ++i) {
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
    for (i = 0U; i < ISAAC_VITA_SYNC_PLAIN_CS_SITE_COUNT; ++i) {
        const unsigned char *p =
            (const unsigned char *)s_plain_cs_sites[i].name;
        uint32_t values[3];
        uint32_t j;
        do {
            hash = hash_byte(hash, *p);
        } while (*p++ != 0U);
        values[0] = s_plain_cs_sites[i].iat_rva;
        values[1] = s_plain_cs_sites[i].call_rva;
        values[2] = s_plain_cs_sites[i].return_rva;
        for (j = 0U; j < 3U; ++j) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash, (uint8_t)(values[j] >> shift));
        }
    }
    for (i = 0U; i < ISAAC_VITA_SYNC_LATE_GET_PROC_CALL_COUNT; ++i) {
        const unsigned char *p =
            (const unsigned char *)s_userenv_getproc_evidence[i].name;
        uint32_t values[3];
        uint32_t j;
        do {
            hash = hash_byte(hash, *p);
        } while (*p++ != 0U);
        values[0] = s_userenv_getproc_evidence[i].iat_rva;
        values[1] = s_userenv_getproc_evidence[i].call_rva;
        values[2] = s_userenv_getproc_evidence[i].return_rva;
        for (j = 0U; j < 3U; ++j) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash, (uint8_t)(values[j] >> shift));
        }
    }
    return hash;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

void guest_exit(CPU *__restrict c, int32_t code, const char *api)
{
    (void)code;
    (void)api;
    guest_fault(c, 0U, "sync oracle unexpectedly entered guest_exit");
}

/* Keep this sync oracle's frozen integration prefix independent from later
 * production batches.  Their probes are redirected; the new plain-CS
 * handler is still the real one. */
int isaac_vita_sync_test_skip_exception(CPU *__restrict c, const char *name,
                                         unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    return 0;
}

int isaac_vita_sync_test_skip_file_lock(CPU *__restrict c, const char *name,
                                         unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    return 0;
}

int isaac_vita_sync_test_skip_later_import(CPU *__restrict c,
                                            const char *name,
                                            unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    return 0;
}

int isaac_vita_sync_test_skip_gl_dynamic(CPU *__restrict c, uint32_t token,
                                          unsigned *call_count)
{
    (void)c;
    (void)token;
    (void)call_count;
    return 0;
}

void *isaac_vita_guest_malloc(size_t size)
{
    (void)size;
    return NULL;
}

int isaac_vita_guest_free(void *pointer)
{
    (void)pointer;
    return 1;
}

int isaac_vita_guest_heap_terminal(void)
{
    return 0;
}

int isaac_vita_sync_test_skip_heap(CPU *__restrict c, const char *name,
                                    unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    return 0;
}

int isaac_vita_sync_test_skip_steam(CPU *__restrict c, const char *name,
                                    unsigned *call_count)
{
    (void)c;
    (void)name;
    (void)call_count;
    return 0;
}

static int boot23_import(CPU *__restrict c, const char *name,
                         uint32_t argument0, uint32_t argument1,
                         uint32_t argument2, uint32_t argument3,
                         uint32_t esp_advance, unsigned expected_count)
{
    uint32_t esp;

    memset(s_boot_frame, 0xcc, sizeof s_boot_frame);
    esp = pointer32(&s_boot_frame[2]);
    s_boot_frame[2] = 0xb020c0deU;
    s_boot_frame[3] = argument0;
    s_boot_frame[4] = argument1;
    s_boot_frame[5] = argument2;
    s_boot_frame[6] = argument3;
    c->esp = esp;
    if (!guest_host_import(c, name))
        return 0;
    return !c->fault && c->esp == esp + esp_advance &&
           g_host_import_calls == expected_count;
}

static void run_boot23_chain(CPU *__restrict c)
{
    uint32_t callback_esp = c->esp;
    uint32_t i;

    if (!boot23_import(c, ISAAC_VITA_CRT_SET_APP_TYPE_NAME,
                       1U, 0U, 0U, 0U, 4U, 7U)) {
        s_boot23_status = 7U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_SET_FMODE_NAME,
                       0x4000U, 0U, 0U, 0U, 4U, 8U)) {
        s_boot23_status = 8U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_P_COMMODE_NAME,
                       0U, 0U, 0U, 0U, 4U, 9U)) {
        s_boot23_status = 9U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_ATEXIT_NAME,
                       ISAAC_VITA_CRT_ATEXIT_FIRST_CALLBACK_VA,
                       0U, 0U, 0U, 4U, 10U)) {
        s_boot23_status = 10U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_CONFIGURE_ARGV_NAME,
                       1U, 0U, 0U, 0U, 4U, 11U)) {
        s_boot23_status = 11U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_INITIALIZE_SLIST_NAME,
                       pointer32(&s_slist[1]), 0U, 0U, 0U, 8U, 12U)) {
        s_boot23_status = 12U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_CONTROLFP_S_NAME,
                       0U, 0x00010000U, 0x00030000U, 0U, 4U, 13U)) {
        s_boot23_status = 13U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME,
                       0U, 0U, 0U, 0U, 4U, 14U)) {
        s_boot23_status = 14U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_NAME,
                       0U, 0U, 0U, 0U, 4U, 15U)) {
        s_boot23_status = 15U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_SYNC_INIT_CS_NAME,
                       pointer32(&s_critical[1]),
                       ISAAC_VITA_SYNC_BOOT_CS_SPIN,
                       0U, 0U, 12U, 16U)) {
        s_boot23_status = 16U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_SYNC_GET_MODULE_NAME,
                       pointer32(s_module_api), 0U, 0U, 0U, 8U, 17U)) {
        s_boot23_status = 17U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_SYNC_GET_MODULE_NAME,
                       pointer32(s_module_kernel), 0U, 0U, 0U, 8U, 18U)) {
        s_boot23_status = 18U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_SYNC_GET_PROC_NAME,
                       ISAAC_VITA_SYNC_KERNEL32_TOKEN,
                       pointer32(s_sleep_name), 0U, 0U, 12U, 19U)) {
        s_boot23_status = 19U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_SYNC_GET_PROC_NAME,
                       ISAAC_VITA_SYNC_KERNEL32_TOKEN,
                       pointer32(s_wake_name), 0U, 0U, 12U, 20U)) {
        s_boot23_status = 20U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_SYNC_CREATE_EVENT_NAME,
                       0U, 1U, 0U, 0U, 20U, 21U)) {
        s_boot23_status = 21U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_ATEXIT_NAME,
                       ISAAC_VITA_CRT_ATEXIT_PRE_MEMORY_CALLBACK_VA,
                       0U, 0U, 0U, 4U, 22U)) {
        s_boot23_status = 22U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_QPC_IMPORT_NAME,
                       pointer32(&s_performance[0]), 0U, 0U, 0U,
                       8U, 23U) || c->eax != 1U) {
        s_boot23_status = 23U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_MEMORY_MEMSET_NAME,
                       pointer32(&s_boot_memory[1]),
                       ISAAC_VITA_MEMORY_MEMSET_LIVE_VALUE,
                       ISAAC_VITA_MEMORY_MEMSET_LIVE_SIZE,
                       0U, 4U, 24U)) {
        s_boot23_status = 24U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CRT_ATEXIT_NAME,
                       ISAAC_VITA_CRT_ATEXIT_POST_MEMORY_CALLBACK_VA,
                       0U, 0U, 0U, 4U, 25U)) {
        s_boot23_status = 25U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_CONSOLE_GET_STD_HANDLE_NAME,
                       ISAAC_VITA_CONSOLE_STD_ERROR_ARGUMENT,
                       0U, 0U, 0U, 8U, 26U)) {
        s_boot23_status = 26U;
        return;
    }

    for (i = 0U; i < ISAAC_VITA_STARTUP_ENV_BOOT_CALL_COUNT; ++i) {
        const char *environment_name = s_environment_name;
        if (i == ISAAC_VITA_STARTUP_ENV_FALLBACK_ABANDONED_ORDINAL -
                     ISAAC_VITA_STARTUP_ENV_FIRST_ORDINAL)
            environment_name = s_environment_fallback_abandoned;
        else if (i == ISAAC_VITA_STARTUP_ENV_FALLBACK_RESET_ORDINAL -
                          ISAAC_VITA_STARTUP_ENV_FIRST_ORDINAL)
            environment_name = s_environment_fallback_reset;
        if (!boot23_import(c, ISAAC_VITA_STARTUP_ENV_NAME,
                           pointer32(environment_name),
                           pointer32(s_environment_output),
                           ISAAC_VITA_STARTUP_ENV_BUFFER_SIZE,
                           0U, 16U, 27U + i) || c->eax != 0U) {
            s_boot23_status = 27U + i;
            return;
        }
    }
    if (!boot23_import(c, ISAAC_VITA_FLS_ALLOC_NAME,
                       ISAAC_VITA_FLS_BOOT_CALLBACK_VA,
                       0U, 0U, 0U, 8U, 54U) ||
        c->eax == ISAAC_VITA_FLS_OUT_OF_INDEXES) {
        s_boot23_status = 54U;
        return;
    }
    s_boot_fls_index = c->eax;
    if (!boot23_import(c, ISAAC_VITA_FLS_SETVALUE_NAME,
                       s_boot_fls_index, ISAAC_VITA_FLS_BOOT_FIRST_VALUE,
                       0U, 0U, 12U, 55U) || c->eax != 1U) {
        s_boot23_status = 55U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_STARTUP_SYSTEM_INFO_NAME,
                       pointer32(s_system_info), 0U, 0U, 0U, 8U, 56U)) {
        s_boot23_status = 56U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
                       pointer32(s_kernelbase), 0U, 0U, 0U, 8U, 57U) ||
        c->eax != 0U ||
        !boot23_import(c, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
                       pointer32(s_ntdll), 0U, 0U, 0U, 8U, 58U) ||
        c->eax != 0U ||
        !boot23_import(c, ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME,
                       pointer32(s_kernel32), 0U, 0U, 0U, 8U, 59U) ||
        c->eax != 0U) {
        s_boot23_status = g_host_import_calls;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_FLS_SETVALUE_NAME,
                       s_boot_fls_index, 0U, 0U, 0U, 12U, 60U) ||
        c->eax != 1U) {
        s_boot23_status = 60U;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_MEMORY_MEMSET_NAME,
                       pointer32(&s_late_memory[1]), 0U, 8U, 0U,
                       4U, 61U) ||
        !boot23_import(c, ISAAC_VITA_MEMORY_MEMSET_NAME,
                       pointer32(&s_late_memory[4]), 0U, 8U, 0U,
                       4U, 62U)) {
        s_boot23_status = g_host_import_calls;
        return;
    }
    if (!boot23_import(c, ISAAC_VITA_QPC_IMPORT_NAME,
                       pointer32(&s_performance[0]), 0U, 0U, 0U,
                       8U, 63U) || c->eax != 1U ||
        !boot23_import(c, ISAAC_VITA_STARTUP_QPF_NAME,
                       pointer32(&s_performance[2]), 0U, 0U, 0U,
                       8U, 64U) || c->eax != 1U ||
        !boot23_import(c, ISAAC_VITA_QPC_IMPORT_NAME,
                       pointer32(&s_performance[4]), 0U, 0U, 0U,
                       8U, 65U) || c->eax != 1U ||
        !boot23_import(c, ISAAC_VITA_QPC_IMPORT_NAME,
                       pointer32(&s_performance[6]), 0U, 0U, 0U,
                       8U, 66U) || c->eax != 1U) {
        s_boot23_status = g_host_import_calls;
        return;
    }

    /* Return from the real `_initterm_e` guest callback. */
    c->esp = callback_esp;
    (void)gpop(c);
}

void guest_call(CPU *__restrict c, uint32_t target)
{
    if (target != CALLBACK_BOOT23 || ld32(c->esp) != 0xFFF1A11EU) {
        guest_fault(c, target, "boot23 oracle received unexpected callback");
        return;
    }
    ++s_boot23_initializer_calls;
    run_boot23_chain(c);
}

int sceKernelGetThreadId(void)
{
    ++s_thread_id_calls;
    return s_thread_id;
}

SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *option)
{
    int32_t uid;
    ++s_event_create_calls;
    if (strcmp(name, "isaac_crt_event") != 0 ||
        attr != SCE_EVENT_WAITMULTIPLE ||
        (bits != 0 && bits != 1) || option != NULL)
        ++s_bad_mock_call;
    if (s_event_force_create)
        return (SceUID)s_event_create_result;
    uid = s_event_next_uid++;
    s_event_uid = uid;
    s_event_active = 1;
    s_event_bits = (uint32_t)bits;
    return (SceUID)uid;
}

int sceKernelSetEventFlag(SceUID uid, unsigned int bits)
{
    ++s_event_set_calls;
    if (s_event_force_set)
        return s_event_set_result;
    if (!s_event_active || uid != s_event_uid || bits != 1U) {
        ++s_bad_mock_call;
        return -0x3201;
    }
    s_event_bits |= bits;
    return 0;
}

int sceKernelClearEventFlag(SceUID uid, unsigned int bits)
{
    ++s_event_clear_calls;
    if (s_event_force_clear)
        return s_event_clear_result;
    if (!s_event_active || uid != s_event_uid || bits != ~1U) {
        ++s_bad_mock_call;
        return -0x3202;
    }
    s_event_bits &= bits;
    return 0;
}

int sceKernelWaitEventFlag(int uid, unsigned int bits, unsigned int wait,
                           unsigned int *out_bits, SceUInt *timeout)
{
    ++s_event_wait_calls;
    if (!s_event_active || uid != s_event_uid || bits != 1U ||
        wait != SCE_EVENT_WAITOR || out_bits != NULL || timeout != NULL) {
        ++s_bad_mock_call;
        return -0x3203;
    }
    return (s_event_bits & 1U) ? 0 : -0x3204;
}

int sceKernelDeleteEventFlag(int uid)
{
    ++s_event_delete_calls;
    if (s_event_force_delete)
        return s_event_delete_result;
    if (!s_event_active || uid != s_event_uid) {
        ++s_bad_mock_call;
        return -0x3205;
    }
    s_event_active = 0;
    return 0;
}

int sceKernelDelayThread(SceUInt delay)
{
    if (s_delay_calls < sizeof s_delay_values / sizeof s_delay_values[0])
        s_delay_values[s_delay_calls] = (uint32_t)delay;
    else
        ++s_bad_mock_call;
    ++s_delay_calls;
    if (s_delay_force)
        return s_delay_result;
    return 0;
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    const char *operation;
    unsigned result;

    ++s_log_calls;
    if (strcmp(format, "[isaac-sync] %s failed: 0x%08x\n") != 0)
        ++s_bad_mock_call;
    va_start(arguments, format);
    operation = va_arg(arguments, const char *);
    result = va_arg(arguments, unsigned);
    va_end(arguments);
    strncpy(s_log_operation, operation, sizeof s_log_operation - 1U);
    s_log_operation[sizeof s_log_operation - 1U] = '\0';
    s_log_result = result;
    return 0;
}

/* ------------------------------------------------------------------------ */

static int cs_words_equal(const uint32_t expected[6])
{
    return memcmp(&s_critical[1], expected,
                  ISAAC_VITA_SYNC_CS_SIZE) == 0;
}

int main(void)
{
    CPU cpu;
    CPU cached_cpu;
    CPU cpu_snapshot;
    isaac_vita_sync_mutex_pool_snapshot pool_snapshot;
    isaac_vita_sync_mutex_pool_snapshot pool_before;
    uint32_t cs_snapshot[6];
    uint32_t expected[6];
    uint32_t esp;
    uint32_t cs_address;
    uint32_t event_uid;
    unsigned event_calls;
    unsigned delete_calls;
    unsigned import_calls = 0U;
    unsigned late_import_calls = 0U;
    unsigned lifecycle_calls = 0U;
    unsigned try_calls = 0U;
    unsigned pc_hit_calls = 0U;
    unsigned logs;
    unsigned delay_calls;
    unsigned thread_id_calls;
    unsigned cached_try_calls = 0U;
    uint32_t i;

    if (!pointer_fits(s_frame) || !pointer_fits(s_critical) ||
        !pointer_fits(s_plain_critical) || !pointer_fits(s_plain_snapshot) ||
        !pointer_fits(s_plain_failure) ||
        !pointer_fits(s_lw_fallback_critical) ||
        !pointer_fits(s_cached_thread_critical) ||
        !pointer_fits(s_embedded_stress) ||
        !pointer_fits(s_module_api) || !pointer_fits(s_module_kernel) ||
        !pointer_fits(s_module_upper) || !pointer_fits(s_module_other) ||
        !pointer_fits(s_sleep_name) || !pointer_fits(s_wake_name) ||
        !pointer_fits(s_user_profile_name) || !pointer_fits(s_wrong_proc) ||
        !pointer_fits(s_boot_frame) ||
        !pointer_fits(s_initializer_table) || !pointer_fits(s_slist) ||
        !pointer_fits(s_boot_memory) ||
        !pointer_fits(s_environment_name) ||
        !pointer_fits(s_environment_fallback_abandoned) ||
        !pointer_fits(s_environment_fallback_reset) ||
        !pointer_fits(s_environment_output) ||
        !pointer_fits(s_system_info) || !pointer_fits(s_kernelbase) ||
        !pointer_fits(s_ntdll) || !pointer_fits(s_kernel32) ||
        !pointer_fits(s_module_a_upper) ||
        !pointer_fits(s_module_a_other) ||
        !pointer_fits(s_late_memory) || !pointer_fits(s_performance) ||
        !pointer_fits(&g_isaac_vita_crt))
        return 1;
    if (evidence_hash() != UINT64_C(0x65a7f55274455c61))
        return 2;
    if (!pc_hit_evidence_valid())
        return 65;
    if (!close_handle_evidence_valid())
        return 78;
    if (ISAAC_VITA_SYNC_BOOT_CS_RVA != 0x007fd2ccU ||
        ISAAC_VITA_SYNC_BOOT_CS_SPIN != 4000U ||
        ISAAC_VITA_SYNC_APISET_STRING_RVA != 0x006080c8U ||
        ISAAC_VITA_SYNC_KERNEL32_STRING_RVA != 0x0060810cU ||
        ISAAC_VITA_SYNC_SLEEP_STRING_RVA != 0x00608128U ||
        ISAAC_VITA_SYNC_WAKE_ALL_STRING_RVA != 0x00608144U ||
         strcmp(ISAAC_VITA_SYNC_GET_USER_PROFILE_PROCEDURE,
                "GetUserProfileDirectoryA") != 0 ||
         ISAAC_VITA_SYNC_GET_USER_PROFILE_CALL_RVA != 0x0050a886U ||
         ISAAC_VITA_SYNC_GET_USER_PROFILE_RETURN_RVA != 0x0050a88cU ||
         ISAAC_VITA_SYNC_GET_USER_PROFILE_STRING_RVA != 0x0075a988U ||
         ISAAC_VITA_SYNC_GET_USER_PROFILE_STRING_VA != 0x9875a988U ||
         ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE != 0U ||
         ISAAC_VITA_SYNC_GET_USER_PROFILE_ORDINAL != 270U ||
         ISAAC_VITA_SYNC_LATE_GET_PROC_CALL_COUNT != 1U ||
         ISAAC_VITA_SYNC_EVENT_STORAGE_RVA != 0x007fd2c8U ||
         strcmp(ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME,
                "KERNEL32.dll!CloseHandle") != 0 ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_IAT_RVA != 0x00606120U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_IAT_VA != 0x98606120U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_EXIT_CALLBACK_RVA != 0x005eb1e5U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_EXIT_CALL_RVA != 0x005eb1faU ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_EXIT_RETURN_RVA != 0x005eb200U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_REFERENCE_COUNT != 12U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_DIRECT_COUNT != 10U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_LOAD_COUNT != 2U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_REGISTER_CALL_COUNT != 2U ||
         ISAAC_VITA_SYNC_CLOSE_HANDLE_CALL_COUNT != 12U ||
         strcmp(ISAAC_VITA_SYNC_PLAIN_CS_NAME,
                "KERNEL32.dll!InitializeCriticalSection") != 0 ||
         ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA != 0x006060f4U ||
         ISAAC_VITA_SYNC_PLAIN_CS_IAT_VA != 0x986060f4U ||
         ISAAC_VITA_SYNC_PLAIN_CS_FIRST_CALL_RVA != 0x00562db2U ||
         ISAAC_VITA_SYNC_PLAIN_CS_FIRST_RETURN_RVA != 0x00562db8U ||
         ISAAC_VITA_SYNC_PLAIN_CS_FIRST_ORDINAL != 105U ||
         ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT != 32U ||
         strcmp(ISAAC_VITA_SYNC_ENTER_CS_NAME,
                "KERNEL32.dll!EnterCriticalSection") != 0 ||
         ISAAC_VITA_SYNC_ENTER_CS_IAT_RVA != 0x006060fcU ||
         ISAAC_VITA_SYNC_ENTER_CS_CALL_RVA != 0x0055e3d3U ||
         ISAAC_VITA_SYNC_ENTER_CS_RETURN_RVA != 0x0055e3d9U ||
         ISAAC_VITA_SYNC_ENTER_CS_FIRST_ORDINAL != 315U ||
         strcmp(ISAAC_VITA_SYNC_LEAVE_CS_NAME,
                "KERNEL32.dll!LeaveCriticalSection") != 0 ||
         ISAAC_VITA_SYNC_LEAVE_CS_IAT_RVA != 0x006060f8U ||
         ISAAC_VITA_SYNC_LEAVE_CS_CALL_RVA != 0x0055e53fU ||
         ISAAC_VITA_SYNC_LEAVE_CS_RETURN_RVA != 0x0055e545U ||
         ISAAC_VITA_SYNC_LEAVE_CS_FIRST_ORDINAL != 322U ||
         strcmp(ISAAC_VITA_SYNC_DELETE_CS_NAME,
                 "KERNEL32.dll!DeleteCriticalSection") != 0 ||
         ISAAC_VITA_SYNC_DELETE_CS_IAT_RVA != 0x006060f0U ||
         strcmp(ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME,
                "KERNEL32.dll!TryEnterCriticalSection") != 0 ||
         ISAAC_VITA_SYNC_TRY_ENTER_CS_IAT_RVA != 0x00606100U ||
         ISAAC_VITA_SYNC_TRY_ENTER_CS_CALL_RVA != 0x00562e74U ||
         ISAAC_VITA_SYNC_TRY_ENTER_CS_RETURN_RVA != 0x00562e7aU ||
         ISAAC_VITA_SYNC_TRY_ENTER_CS_REFERENCE_COUNT != 1U ||
         ISAAC_VITA_SYNC_IMPORT_COUNT != 14U ||
         ISAAC_VITA_SYNC_PC_HIT_GAP_IMPORT_COUNT != 4U ||
         ISAAC_VITA_SYNC_PC_HIT_REFERENCE_COUNT != 23U ||
         ISAAC_VITA_SYNC_PC_HIT_CALL_COUNT != 24U ||
         strcmp(ISAAC_VITA_SYNC_SET_EVENT_NAME,
                "KERNEL32.dll!SetEvent") != 0 ||
         ISAAC_VITA_SYNC_SET_EVENT_IAT_RVA != 0x006060a8U ||
         ISAAC_VITA_SYNC_SET_EVENT_CALL_RVA != 0x005eb2e3U ||
         ISAAC_VITA_SYNC_SET_EVENT_RETURN_RVA != 0x005eb2e9U ||
         strcmp(ISAAC_VITA_SYNC_RESET_EVENT_NAME,
                "KERNEL32.dll!ResetEvent") != 0 ||
         ISAAC_VITA_SYNC_RESET_EVENT_IAT_RVA != 0x006060a0U ||
         ISAAC_VITA_SYNC_RESET_EVENT_CALL_RVA != 0x005eb2efU ||
         ISAAC_VITA_SYNC_RESET_EVENT_RETURN_RVA != 0x005eb2f5U ||
         strcmp(ISAAC_VITA_SYNC_GET_MODULE_A_NAME,
                "KERNEL32.dll!GetModuleHandleA") != 0 ||
         ISAAC_VITA_SYNC_GET_MODULE_A_IAT_RVA != 0x00606110U ||
         ISAAC_VITA_SYNC_GET_MODULE_A_REFERENCE_COUNT != 8U ||
         ISAAC_VITA_SYNC_GET_MODULE_A_DIRECT_COUNT != 5U ||
         ISAAC_VITA_SYNC_GET_MODULE_A_LOAD_COUNT != 3U ||
         ISAAC_VITA_SYNC_GET_MODULE_A_REGISTER_CALL_COUNT != 4U ||
         ISAAC_VITA_SYNC_GET_MODULE_A_CALL_COUNT != 9U ||
         ISAAC_VITA_SYNC_GET_MODULE_A_NULL_CALL_COUNT != 9U ||
         strcmp(ISAAC_VITA_SYNC_SLEEP_NAME,
                "KERNEL32.dll!Sleep") != 0 ||
         ISAAC_VITA_SYNC_SLEEP_IAT_RVA != 0x00606138U ||
         ISAAC_VITA_SYNC_SLEEP_DIRECT_COUNT != 5U ||
         ISAAC_VITA_SYNC_SLEEP_LOAD_COUNT != 8U ||
         ISAAC_VITA_SYNC_SLEEP_REGISTER_CALL_COUNT != 8U ||
         ISAAC_VITA_SYNC_SLEEP_REFERENCE_COUNT != 13U ||
         ISAAC_VITA_SYNC_SLEEP_CALL_COUNT != 13U ||
         ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS != 4294967U ||
         ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE != 6U ||
         strcmp(ISAAC_VITA_SYNC_LATE_NEXT_NAME,
                "steam_api.dll!SteamAPI_RegisterCallback") != 0 ||
         ISAAC_VITA_SYNC_LATE_NEXT_IAT_RVA != 0x006066bcU ||
         ISAAC_VITA_SYNC_LATE_NEXT_CALL_RVA != 0x00001deaU ||
         ISAAC_VITA_SYNC_LATE_NEXT_RETURN_RVA != 0x00001df0U ||
         ISAAC_VITA_SYNC_LATE_NEXT_OBJECT_VA != 0x987e815cU ||
         ISAAC_VITA_SYNC_LATE_NEXT_CALLBACK_ID != 0x44dU ||
         ISAAC_VITA_SYNC_LATE_NEXT_ORDINAL != 255U ||
        strcmp(ISAAC_VITA_SYNC_NEXT_NAME, "KERNEL32.dll!GetStdHandle") != 0 ||
        ISAAC_VITA_SYNC_NEXT_IAT_RVA != 0x00606098U ||
        ISAAC_VITA_SYNC_NEXT_CALL_RVA != 0x005e348bU ||
        ISAAC_VITA_SYNC_NEXT_RETURN_RVA != 0x005e3491U ||
        strcmp(ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME,
               "KERNEL32.dll!IsProcessorFeaturePresent") != 0 ||
        ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_RVA != 0x00606030U ||
        ISAAC_VITA_PROCESSOR_FEATURE_CALL_RVA != 0x005ebaf1U ||
        ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_RETURN != 0x005ebaf7U ||
        ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT != 10U ||
        strcmp(ISAAC_VITA_MEMORY_MEMSET_NAME,
               "VCRUNTIME140.dll!memset") != 0 ||
        ISAAC_VITA_MEMORY_MEMSET_IAT_RVA != 0x00606484U ||
        ISAAC_VITA_MEMORY_MEMSET_IAT_VA != 0x98606484U ||
        ISAAC_VITA_MEMORY_MEMSET_CALL_RVA != 0x005e8fafU ||
        ISAAC_VITA_MEMORY_MEMSET_RETURN_RVA != 0x005e8fb4U ||
        ISAAC_VITA_MEMORY_MEMSET_LIVE_DST != 0x987a9e34U ||
        ISAAC_VITA_MEMORY_MEMSET_LIVE_VALUE != 0U ||
        ISAAC_VITA_MEMORY_MEMSET_LIVE_SIZE != 0x78U ||
        ISAAC_VITA_QPC_SEED_CALL_RVA != 0x005e8b7aU ||
        ISAAC_VITA_QPC_SEED_RETURN_RVA != 0x005e8b80U ||
        ISAAC_VITA_QPC_SEED_BOOT_ORDINAL != 23U ||
        strcmp(ISAAC_VITA_CONSOLE_GET_STD_HANDLE_NAME,
               "KERNEL32.dll!GetStdHandle") != 0 ||
        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_IAT_RVA != 0x00606098U ||
        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_CALL_RVA != 0x005e348bU ||
        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_RETURN_RVA != 0x005e3491U ||
        ISAAC_VITA_CONSOLE_STD_ERROR_ARGUMENT != UINT32_C(0xfffffff4) ||
        strcmp(ISAAC_VITA_CONSOLE_NEXT_NAME,
               "KERNEL32.dll!GetEnvironmentVariableA") != 0 ||
        ISAAC_VITA_CONSOLE_NEXT_IAT_RVA != 0x00606094U ||
        ISAAC_VITA_CONSOLE_NEXT_CALL_RVA != 0x005e3849U ||
        ISAAC_VITA_CONSOLE_NEXT_RETURN_RVA != 0x005e384fU ||
        ISAAC_VITA_STARTUP_ENV_BOOT_CALL_COUNT != 27U ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_CALL_RVA != 0x005e38d9U ||
        ISAAC_VITA_STARTUP_ENV_FALLBACK_RETURN_RVA != 0x005e38dfU ||
        ISAAC_VITA_FLS_BOOT_CALL_COUNT != 3U ||
        ISAAC_VITA_STARTUP_SYSTEM_INFO_ORDINAL != 56U ||
        ISAAC_VITA_STARTUP_QPF_ORDINAL != 64U ||
        ISAAC_VITA_STARTUP_NEXT_INITTERM_ORDINAL != 67U ||
        strcmp(ISAAC_VITA_STARTUP_NEXT_NAME,
               "KERNEL32.dll!SetUnhandledExceptionFilter") != 0 ||
        ISAAC_VITA_STARTUP_NEXT_IAT_RVA != 0x0060603cU ||
        ISAAC_VITA_STARTUP_NEXT_CALL_RVA != 0x005ebf7dU ||
        ISAAC_VITA_STARTUP_NEXT_RETURN_RVA != 0x005ebf83U ||
        ISAAC_VITA_STARTUP_NEXT_CALLBACK_VA != 0x985ebf84U ||
        ISAAC_VITA_BOOT_BASE_CALL_COUNT +
            ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_COUNT +
            ISAAC_VITA_CRT_BOOT_CALL_COUNT +
            ISAAC_VITA_SYNC_CALL_COUNT +
            ISAAC_VITA_MEMORY_BOOT_CALL_COUNT +
            ISAAC_VITA_CONSOLE_BOOT_CALL_COUNT +
            ISAAC_VITA_STARTUP_BOOT_CALL_COUNT +
            ISAAC_VITA_FLS_BOOT_CALL_COUNT != 67U)
        return 3;

    make_utf16(s_module_api, "api-ms-win-core-synch-l1-2-0.dll");
    make_utf16(s_module_kernel, "kernel32.dll");
    make_utf16(s_module_upper, "KERNEL32.DLL");
    make_utf16(s_module_other, "user32.dll");
    strcpy(s_sleep_name, "SleepConditionVariableCS");
    strcpy(s_wake_name, "WakeAllConditionVariable");
    strcpy(s_user_profile_name,
           ISAAC_VITA_SYNC_GET_USER_PROFILE_PROCEDURE);
    strcpy(s_wrong_proc, "sleepConditionVariableCS");

    s_event_next_uid = 0x2345;
    cs_address = pointer32(&s_critical[1]);

    /* Call 1: arbitrary pre-init bytes are replaced inside exactly 24 bytes;
     * owner/depth live in the guest object and magic becomes valid last. */
    s_critical[0] = 0x11223344U;
    s_critical[1] = 0xa1a2a3a4U;
    s_critical[2] = 0xb1b2b3b4U;
    s_critical[3] = 0xc1c2c3c4U;
    s_critical[4] = 0xd1d2d3d4U;
    s_critical[5] = 0xe1e2e3e4U;
    s_critical[6] = 0xf1f2f3f4U;
    s_critical[7] = 0x55667788U;
    esp = prepare_call(&cpu);
    cpu.eax = 0xfeedfaceU;
    s_frame[5] = cs_address;
    s_frame[6] = ISAAC_VITA_SYNC_BOOT_CS_SPIN;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_INIT_CS_NAME, &import_calls))
        return 4;
    expected[0] = ISAAC_VITA_SYNC_CS_MAGIC;
    expected[1] = (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE;
    expected[2] = ISAAC_VITA_SYNC_BOOT_CS_SPIN;
    expected[3] = ISAAC_VITA_SYNC_CS_VERSION;
    expected[4] = 0U;
    expected[5] = 0U;
    if (cpu.fault || cpu.esp != esp + 12U || cpu.eax != 1U ||
        !cs_words_equal(expected) ||
        s_critical[0] != 0x11223344U || s_critical[7] != 0x55667788U ||
        s_bad_mock_call != 0U)
        return 5;

    /* Calls 2-3: actual UTF-16 is decoded with ld16.  The api-set is absent;
     * the case-insensitive kernel32 fallback gets one synthetic token. */
    esp = prepare_call(&cpu);
    cpu.eax = 0xffffffffU;
    s_frame[5] = pointer32(s_module_api);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_MODULE_NAME, &import_calls))
        return 6;
    if (cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U)
        return 7;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_module_kernel);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_MODULE_NAME, &import_calls))
        return 8;
    if (cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != ISAAC_VITA_SYNC_KERNEL32_TOKEN)
        return 9;

    /* Calls 4-5: exact, case-sensitive names are deliberately unavailable so
     * the CRT selects its event-based condition-variable fallback. */
    esp = prepare_call(&cpu);
    s_frame[5] = ISAAC_VITA_SYNC_KERNEL32_TOKEN;
    s_frame[6] = pointer32(s_sleep_name);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_PROC_NAME, &import_calls))
        return 10;
    if (cpu.fault || cpu.esp != esp + 12U || cpu.eax != 0U)
        return 11;
    esp = prepare_call(&cpu);
    s_frame[5] = ISAAC_VITA_SYNC_KERNEL32_TOKEN;
    s_frame[6] = pointer32(s_wake_name);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_PROC_NAME, &import_calls))
        return 12;
    if (cpu.fault || cpu.esp != esp + 12U || cpu.eax != 0U)
        return 13;

    /* Late call #270: USERENV was reported absent at #269, so resolving its
     * one measured optional procedure against NULL also returns NULL. */
    esp = prepare_call(&cpu);
    cpu.eax = 0xffffffffU;
    s_frame[5] = ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE;
    s_frame[6] = pointer32(s_user_profile_name);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_PROC_NAME, &late_import_calls) ||
        late_import_calls != 1U || cpu.fault || cpu.eax != 0U ||
        cpu.esp != esp + 12U)
        return 60;

    /* The NULL-module exception is exact by procedure name.  A different
     * request faults after the dispatcher has already counted it. */
    esp = prepare_call(&cpu);
    cpu.eax = 0x13579bdfU;
    s_frame[5] = ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE;
    s_frame[6] = pointer32(s_wrong_proc);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_PROC_NAME, &late_import_calls) ||
        late_import_calls != 2U || cpu.esp != esp ||
        cpu.eax != 0x13579bdfU || !cpu.fault ||
        strcmp(cpu.fault,
            "GetProcAddress requested an unexpected absent-module procedure") != 0)
        return 61;

    /* Call 6: NULL,TRUE,FALSE,NULL creates an unsignalled manual-reset event;
     * its positive Vita UID is the opaque 32-bit guest HANDLE. */
    esp = prepare_call(&cpu);
    s_frame[5] = 0U;
    s_frame[6] = 1U;
    s_frame[7] = 0U;
    s_frame[8] = 0U;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_CREATE_EVENT_NAME, &import_calls))
        return 14;
    event_uid = (uint32_t)s_event_uid;
    if (cpu.fault || cpu.esp != esp + 20U || cpu.eax != event_uid ||
        event_uid != 0x2345U || import_calls != ISAAC_VITA_SYNC_CALL_COUNT ||
        s_event_create_calls != 1U ||
        s_event_active != 1 || s_event_bits != 0U || s_bad_mock_call != 0U)
        return 15;

    /* Four later PC-hit gaps: BOOL event transitions, scheduler-backed void
     * Sleep, and the exact narrow module policy.  Count-before remains
     * observable on both normal Win32 failures and the two loud rejects. */
    esp = prepare_call(&cpu);
    cpu.last_error = 0x11112222U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_SET_EVENT_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 1U ||
        cpu.last_error != 0x11112222U || s_event_bits != 1U ||
        s_event_set_calls != 1U || pc_hit_calls != 1U)
        return 66;

    esp = prepare_call(&cpu);
    cpu.last_error = 0x33334444U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_RESET_EVENT_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 1U ||
        cpu.last_error != 0x33334444U || s_event_bits != 0U ||
        s_event_clear_calls != 1U || pc_hit_calls != 2U)
        return 67;

    s_event_force_set = 1;
    s_event_set_result = -0x4301;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.last_error = 0xa5a5a5a5U;
    cpu.eax = 0x55556666U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_SET_EVENT_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE ||
        s_event_bits != 0U || s_log_calls != logs + 1U ||
        strcmp(s_log_operation, "sceKernelSetEventFlag") != 0 ||
        s_log_result != (uint32_t)-0x4301 || pc_hit_calls != 3U)
        return 68;
    s_event_force_set = 0;

    s_event_force_clear = 1;
    s_event_clear_result = -0x4302;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.last_error = 0xa5a5a5a5U;
    cpu.eax = 0x77778888U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_RESET_EVENT_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE ||
        s_event_bits != 0U || s_log_calls != logs + 1U ||
        strcmp(s_log_operation, "sceKernelClearEventFlag") != 0 ||
        s_log_result != (uint32_t)-0x4302 || pc_hit_calls != 4U)
        return 69;
    s_event_force_clear = 0;

    esp = prepare_call(&cpu);
    cpu.last_error = 0x9999aaaaU;
    cpu.eax = 0x12345678U;
    s_frame[5] = 0U;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_SLEEP_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0x12345678U ||
        cpu.last_error != 0x9999aaaaU || s_delay_calls != 1U ||
        s_delay_values[0] != 1U || pc_hit_calls != 5U)
        return 70;

    esp = prepare_call(&cpu);
    cpu.eax = 0x89abcdefU;
    s_frame[5] = 17U;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_SLEEP_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0x89abcdefU ||
        s_delay_calls != 2U || s_delay_values[1] != 17000U ||
        pc_hit_calls != 6U)
        return 71;
    if (isaac_vita_sync_sleep_milliseconds(
            ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS + 1U) != 0 ||
        s_delay_calls != 4U ||
        s_delay_values[2] !=
            ISAAC_VITA_SYNC_SLEEP_CHUNK_MAX_MS * 1000U ||
        s_delay_values[3] != 1000U)
        return 72;

    s_delay_force = 1;
    s_delay_result = -0x4401;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0xcafebabeU;
    s_frame[5] = 99U;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_SLEEP_NAME, &pc_hit_calls) ||
        !cpu.fault || strcmp(cpu.fault, "Vita thread delay failed") != 0 ||
        cpu.fault_addr !=
            GUEST_IMAGE_BASE + ISAAC_VITA_SYNC_SLEEP_IAT_RVA ||
        cpu.esp != esp || cpu.eax != 0xcafebabeU ||
        s_delay_calls != 5U || s_delay_values[4] != 99000U ||
        s_log_calls != logs + 1U ||
        strcmp(s_log_operation, "sceKernelDelayThread") != 0 ||
        s_log_result != (uint32_t)-0x4401 || pc_hit_calls != 7U)
        return 73;
    s_delay_force = 0;

    esp = prepare_call(&cpu);
    cpu.last_error = 0xbbbbccccU;
    s_frame[5] = 0U;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_MODULE_A_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != GUEST_IMAGE_BASE || cpu.last_error != 0xbbbbccccU ||
        pc_hit_calls != 8U)
        return 74;

    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_module_a_upper);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_MODULE_A_NAME, &pc_hit_calls) ||
        cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != ISAAC_VITA_SYNC_KERNEL32_TOKEN || pc_hit_calls != 9U)
        return 75;

    esp = prepare_call(&cpu);
    cpu.eax = 0x0f1e2d3cU;
    s_frame[5] = pointer32(s_module_a_other);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_GET_MODULE_A_NAME, &pc_hit_calls) ||
        !cpu.fault ||
        strcmp(cpu.fault,
               "GetModuleHandleA for an unexpected module") != 0 ||
        cpu.fault_addr != pointer32(s_module_a_other) || cpu.esp != esp ||
        cpu.eax != 0x0f1e2d3cU || pc_hit_calls != 10U || s_bad_mock_call != 0U)
        return 76;

    /* The exact next frontier stays a mutation-free unknown handoff. */
    esp = prepare_call(&cpu);
    cpu.eax = 0x89abcdefU;
    cpu.ecx = 0x10203040U;
    memcpy(&cpu_snapshot, &cpu, sizeof cpu_snapshot);
    memcpy(cs_snapshot, &s_critical[1], sizeof cs_snapshot);
    event_calls = s_event_create_calls;
    if (isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_NEXT_NAME, &import_calls) != 0)
        return 16;
    if (memcmp(&cpu, &cpu_snapshot, sizeof cpu) != 0 ||
        memcmp(&s_critical[1], cs_snapshot, sizeof cs_snapshot) != 0 ||
        cpu.esp != esp ||
        s_event_create_calls != event_calls ||
        import_calls != ISAAC_VITA_SYNC_CALL_COUNT)
        return 17;

    /* Embedded lifecycle: recursive enter twice, refuse owned deletion
     * without changing guest state, leave twice, delete, and reinitialize. */
    memset(&cpu, 0, sizeof cpu);
    if (!isaac_vita_sync_cs_enter(&cpu, cs_address) ||
        !isaac_vita_sync_cs_enter(&cpu, cs_address) ||
        cpu.fault || s_critical[5] != (uint32_t)s_thread_id ||
        s_critical[6] != 2U)
        return 18;
    memcpy(cs_snapshot, &s_critical[1], sizeof cs_snapshot);
    logs = s_log_calls;
    if (isaac_vita_sync_cs_delete(&cpu, cs_address) != 0 ||
        !cpu.fault ||
        strcmp(cpu.fault, "Vita recursive mutex deletion failed") != 0 ||
        cpu.fault_addr != cs_address ||
        memcmp(&s_critical[1], cs_snapshot, sizeof cs_snapshot) != 0 ||
        s_critical[5] != (uint32_t)s_thread_id ||
        s_critical[6] != 2U ||
        s_log_calls != logs + 1U ||
        strcmp(s_log_operation, "VitaEmbeddedMutexDelete") != 0 ||
        s_log_result !=
            (uint32_t)SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN)
        return 19;
    clear_fault(&cpu);
    if (!isaac_vita_sync_cs_leave(&cpu, cs_address) ||
        !isaac_vita_sync_cs_leave(&cpu, cs_address) ||
        cpu.fault || s_critical[5] != 0U || s_critical[6] != 0U)
        return 20;
    if (!isaac_vita_sync_cs_delete(&cpu, cs_address) || cpu.fault)
        return 21;
    expected[0] = expected[1] = expected[2] = 0U;
    expected[3] = expected[4] = expected[5] = 0U;
    if (!cs_words_equal(expected) ||
        s_critical[0] != 0x11223344U || s_critical[7] != 0x55667788U)
        return 22;
    if (!isaac_vita_sync_cs_initialize(
            &cpu, cs_address, ISAAC_VITA_SYNC_BOOT_CS_SPIN) || cpu.fault ||
        ld32(cs_address) != ISAAC_VITA_SYNC_CS_MAGIC ||
        ld32(cs_address + 4U) !=
            (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE)
        return 23;

    /* The lifecycle helpers must also be reachable through the production
     * name dispatcher.  All three are void stdcall(1): preserve EAX, pop the
     * return and one argument, and count before any possible nested fault. */
    esp = prepare_call(&cpu);
    cpu.eax = 0x31531531U;
    s_frame[5] = cs_address;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_ENTER_CS_NAME, &lifecycle_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0x31531531U ||
        lifecycle_calls != 1U || s_critical[5] != (uint32_t)s_thread_id ||
        s_critical[6] != 1U)
        return 62;
    esp = prepare_call(&cpu);
    cpu.eax = 0x32232232U;
    s_frame[5] = cs_address;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_LEAVE_CS_NAME, &lifecycle_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0x32232232U ||
        lifecycle_calls != 2U || s_critical[5] != 0U ||
        s_critical[6] != 0U)
        return 63;

    memcpy(cs_snapshot, &s_critical[1], sizeof cs_snapshot);
    if (isaac_vita_sync_cs_initialize(&cpu, cs_address, 77U) != 0 ||
        !cpu.fault || strcmp(cpu.fault,
                            "critical section initialized twice") != 0 ||
        memcmp(&s_critical[1], cs_snapshot, sizeof cs_snapshot) != 0)
        return 24;
    clear_fault(&cpu);
    esp = prepare_call(&cpu);
    cpu.eax = 0x0de1e7e0U;
    s_frame[5] = cs_address;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_DELETE_CS_NAME, &lifecycle_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0x0de1e7e0U ||
        lifecycle_calls != 3U)
        return 64;

    /* Even a forced kernel-mutex failure is irrelevant: Vista-era Win32
     * critical sections use their caller-provided 24 bytes and always init. */
    s_critical[1] = 0x01020304U;
    s_critical[2] = 0x11121314U;
    s_critical[3] = 0x21222324U;
    s_critical[4] = 0x31323334U;
    s_critical[5] = 0x41424344U;
    s_critical[6] = 0x51525354U;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0xaabbccddU;
    s_frame[5] = cs_address;
    s_frame[6] = ISAAC_VITA_SYNC_BOOT_CS_SPIN;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_INIT_CS_NAME, &import_calls))
        return 26;
    if (cpu.fault || cpu.esp != esp + 12U || cpu.eax != 1U ||
        s_critical[1] != ISAAC_VITA_SYNC_CS_MAGIC ||
        s_critical[2] !=
            (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
        s_critical[3] != ISAAC_VITA_SYNC_BOOT_CS_SPIN ||
        s_critical[4] != ISAAC_VITA_SYNC_CS_VERSION ||
        s_critical[5] != 0U || s_critical[6] != 0U ||
        s_critical[0] != 0x11223344U || s_critical[7] != 0x55667788U ||
        s_log_calls != logs ||
        import_calls != ISAAC_VITA_SYNC_CALL_COUNT + 1U)
        return 27;
    if (!isaac_vita_sync_cs_delete(&cpu, cs_address) || cpu.fault)
        return 114;

    /* Event services preserve manual-reset behavior: a set survives repeated
     * waits and reset clears it. */
    if (isaac_vita_sync_wait_event((int32_t)event_uid) != -0x3204 ||
        isaac_vita_sync_set_event((int32_t)event_uid) != 0 ||
        isaac_vita_sync_wait_event((int32_t)event_uid) != 0 ||
        isaac_vita_sync_wait_event((int32_t)event_uid) != 0 ||
        s_event_bits != 1U ||
        isaac_vita_sync_reset_event((int32_t)event_uid) != 0 ||
        s_event_bits != 0U ||
        isaac_vita_sync_wait_event((int32_t)event_uid) != -0x3204 ||
        s_event_active != 1 || s_event_set_calls != 3U ||
        s_event_clear_calls != 3U || s_event_wait_calls != 4U ||
        s_event_delete_calls != 0U || s_bad_mock_call != 0U)
        return 28;

    /* CloseHandle owns only the exact CreateEventW result.  A synthetic
     * thread/foreign handle faults before touching the native event. */
    delete_calls = s_event_delete_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0x13579bdfU;
    s_frame[5] = event_uid + 1U;
    if (!isaac_vita_sync_import(
            &cpu, ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME) ||
        cpu.esp != esp || cpu.eax != 0x13579bdfU || !cpu.fault ||
        cpu.fault_addr != event_uid + 1U ||
        strcmp(cpu.fault,
               "CloseHandle received an unknown, stale, or non-event handle") != 0 ||
        s_event_active != 1 || s_event_delete_calls != delete_calls)
        return 79;

    /* A native close failure is ordinary Win32 FALSE: retain ownership for a
     * retry, set ERROR_INVALID_HANDLE, and keep stdcall cleanup exact. */
    s_event_force_delete = 1;
    s_event_delete_result = -0x4303;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.last_error = 0xa5a5a5a5U;
    cpu.eax = 0x2468ace0U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import(
            &cpu, ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE ||
        s_event_active != 1 ||
        s_event_delete_calls != delete_calls + 1U ||
        s_log_calls != logs + 1U ||
        strcmp(s_log_operation, "sceKernelDeleteEventFlag") != 0 ||
        s_log_result != (uint32_t)-0x4303)
        return 80;
    s_event_force_delete = 0;

    /* Success deletes and retires the owner, returns BOOL TRUE, and preserves
     * LastError.  The same handle is then stale and must fault loudly. */
    esp = prepare_call(&cpu);
    cpu.last_error = 0x11223344U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import(
            &cpu, ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 1U ||
        cpu.last_error != 0x11223344U || s_event_active != 0 ||
        s_event_delete_calls != delete_calls + 2U || s_bad_mock_call != 0U)
        return 81;

    esp = prepare_call(&cpu);
    cpu.eax = 0xabcdef01U;
    s_frame[5] = event_uid;
    if (!isaac_vita_sync_import(
            &cpu, ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME) ||
        cpu.esp != esp || cpu.eax != 0xabcdef01U || !cpu.fault ||
        cpu.fault_addr != event_uid ||
        strcmp(cpu.fault,
               "CloseHandle received an unknown, stale, or non-event handle") != 0 ||
        s_event_delete_calls != delete_calls + 2U)
        return 82;

    /* CreateEvent failure is a normal NULL return with stdcall cleanup; the
     * caller's existing branch owns the fatal decision. */
    s_event_force_create = 1;
    s_event_create_result = -0x4201;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0xffffffffU;
    s_frame[5] = 0U;
    s_frame[6] = 1U;
    s_frame[7] = 0U;
    s_frame[8] = 0U;
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_CREATE_EVENT_NAME, &import_calls))
        return 29;
    if (cpu.fault || cpu.esp != esp + 20U || cpu.eax != 0U ||
        s_log_calls != logs + 1U ||
        strcmp(s_log_operation, "sceKernelCreateEventFlag") != 0 ||
        s_log_result != (uint32_t)-0x4201 ||
        import_calls != ISAAC_VITA_SYNC_CALL_COUNT + 2U)
        return 30;
    s_event_force_create = 0;

    /* Case policy and loud rejects: module names ignore ASCII case; proc names
     * do not.  Ordinals, wrong tokens, and wrong event shapes do not clean the
     * x86 frame or invoke a target service. */
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_module_upper);
    if (!isaac_vita_sync_import(&cpu, ISAAC_VITA_SYNC_GET_MODULE_NAME) ||
        cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != ISAAC_VITA_SYNC_KERNEL32_TOKEN)
        return 31;
    esp = prepare_call(&cpu);
    cpu.eax = 0x01010101U;
    s_frame[5] = pointer32(s_module_other);
    if (!isaac_vita_sync_import(&cpu, ISAAC_VITA_SYNC_GET_MODULE_NAME) ||
        cpu.esp != esp || cpu.eax != 0x01010101U || !cpu.fault ||
        strcmp(cpu.fault,
               "GetModuleHandleW for an unexpected module") != 0)
        return 32;
    esp = prepare_call(&cpu);
    s_frame[5] = ISAAC_VITA_SYNC_KERNEL32_TOKEN;
    s_frame[6] = pointer32(s_wrong_proc);
    if (!isaac_vita_sync_import(&cpu, ISAAC_VITA_SYNC_GET_PROC_NAME) ||
        cpu.esp != esp || !cpu.fault || strcmp(cpu.fault,
            "GetProcAddress requested an unexpected procedure") != 0)
        return 33;
    esp = prepare_call(&cpu);
    s_frame[5] = ISAAC_VITA_SYNC_KERNEL32_TOKEN;
    s_frame[6] = 7U;
    if (!isaac_vita_sync_import(&cpu, ISAAC_VITA_SYNC_GET_PROC_NAME) ||
        cpu.esp != esp || !cpu.fault || strcmp(cpu.fault,
            "GetProcAddress received null or ordinal name") != 0)
        return 34;
    esp = prepare_call(&cpu);
    s_frame[5] = ISAAC_VITA_SYNC_KERNEL32_TOKEN + 1U;
    s_frame[6] = pointer32(s_sleep_name);
    if (!isaac_vita_sync_import(&cpu, ISAAC_VITA_SYNC_GET_PROC_NAME) ||
        cpu.esp != esp || !cpu.fault || strcmp(cpu.fault,
            "GetProcAddress received an unexpected module token") != 0)
        return 35;
    event_calls = s_event_create_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0x02020202U;
    s_frame[5] = 0U;
    s_frame[6] = 0U;             /* wrong: auto-reset */
    s_frame[7] = 0U;
    s_frame[8] = 0U;
    if (!isaac_vita_sync_import(&cpu, ISAAC_VITA_SYNC_CREATE_EVENT_NAME) ||
        cpu.esp != esp || cpu.eax != 0x02020202U || !cpu.fault ||
        cpu.fault_addr !=
            GUEST_IMAGE_BASE + ISAAC_VITA_SYNC_CREATE_EVENT_IAT_RVA ||
        strcmp(cpu.fault,
               "CreateEventW outside the measured manual-reset shape") != 0 ||
        s_event_create_calls != event_calls)
        return 36;

    if (s_bad_mock_call || s_event_active)
        return 37;

    /* Separate smoke for the production `_initterm` dispatcher and cdecl
     * empty range; the complete run below counts its real boot occurrence. */
    g_host_import_calls = 0U;
    esp = prepare_call(&cpu);
    cpu.eax = 0x24681357U;
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME) || cpu.fault ||
        cpu.esp != esp + 4U || cpu.eax != 0x24681357U ||
        g_host_import_calls != 1U)
        return 41;

    /* Execute the complete production order through call #67.  `_initterm_e`
     * remains active through #66; `_initterm` follows after its callback
     * returns, then SetUnhandledExceptionFilter is the exact loud frontier. */
    g_isaac_vita_crt = (isaac_vita_crt_state) {
        0U, 0U, 0U, 0U, 0x00010000U, 0, 0U
    };
    memset(g_isaac_vita_crt_atexit, 0, sizeof g_isaac_vita_crt_atexit);
    memset(s_slist, 0xcc, sizeof s_slist);
    memset(s_critical, 0xcc, sizeof s_critical);
    memset(s_environment_output, 0xa5, sizeof s_environment_output);
    memset(s_system_info, 0xcc, sizeof s_system_info);
    memset(s_late_memory, 0xcc, sizeof s_late_memory);
    memset(s_performance, 0xcc, sizeof s_performance);
    for (i = 0U; i < 32U; ++i)
        s_boot_memory[i] = 0xccccccccU;
    s_boot_memory[0] = 0x11223344U;
    s_boot_memory[31] = 0x55667788U;
    s_initializer_table[0] = CALLBACK_BOOT23;
    s_boot23_status = 0U;
    s_boot23_initializer_calls = 0U;
    event_calls = s_event_create_calls;
    g_host_import_calls = ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT;
    g_host_dynamic_calls = 0U;
    if (!boot23_import(&cpu, ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME,
                       ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT,
                       0U, 0U, 0U, 8U, 5U) || cpu.eax != 1U ||
        s_boot_frame[3] != ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT)
        return 38;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table + 1);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME))
        return 39;
    if (s_boot23_status != 0U || s_boot23_initializer_calls != 1U ||
        g_host_import_calls != 66U || g_host_dynamic_calls != 0U ||
        cpu.fault || cpu.esp != esp + 4U)
        return 40;

    s_initializer_table[0] = 0U;
    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(s_initializer_table);
    s_frame[6] = pointer32(s_initializer_table);
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_NAME) || cpu.fault ||
        cpu.esp != esp + 4U || g_host_import_calls != 67U)
        return 44;

    memset(s_boot_frame, 0xcc, sizeof s_boot_frame);
    s_boot_frame[2] = ISAAC_VITA_STARTUP_NEXT_RETURN_RVA;
    s_boot_frame[3] = ISAAC_VITA_STARTUP_NEXT_CALLBACK_VA;
    cpu.esp = pointer32(&s_boot_frame[2]);
    if (guest_host_import(&cpu, ISAAC_VITA_STARTUP_NEXT_NAME) != 0 ||
        g_host_import_calls != 67U)
        return 45;
    guest_fault(&cpu, GUEST_IMAGE_BASE + ISAAC_VITA_STARTUP_NEXT_IAT_RVA,
                ISAAC_VITA_STARTUP_NEXT_NAME);

    if (g_host_import_calls != ISAAC_VITA_BOOT_BASE_CALL_COUNT +
                               ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_COUNT +
                               ISAAC_VITA_CRT_BOOT_CALL_COUNT +
                               ISAAC_VITA_SYNC_CALL_COUNT +
                               ISAAC_VITA_MEMORY_BOOT_CALL_COUNT +
                               ISAAC_VITA_CONSOLE_BOOT_CALL_COUNT +
                               ISAAC_VITA_STARTUP_BOOT_CALL_COUNT +
                               ISAAC_VITA_FLS_BOOT_CALL_COUNT ||
        cpu.fault_addr != GUEST_IMAGE_BASE + ISAAC_VITA_STARTUP_NEXT_IAT_RVA ||
        !cpu.fault || strcmp(cpu.fault, ISAAC_VITA_STARTUP_NEXT_NAME) != 0 ||
        cpu.esp != pointer32(&s_boot_frame[2]) ||
        ld32(cpu.esp) != ISAAC_VITA_STARTUP_NEXT_RETURN_RVA ||
        ld32(cpu.esp + 4U) != ISAAC_VITA_STARTUP_NEXT_CALLBACK_VA ||
        g_isaac_vita_crt.atexit_count != 3U ||
        g_isaac_vita_crt_atexit[0] !=
            ISAAC_VITA_CRT_ATEXIT_FIRST_CALLBACK_VA ||
        g_isaac_vita_crt_atexit[1] !=
            ISAAC_VITA_CRT_ATEXIT_PRE_MEMORY_CALLBACK_VA ||
        g_isaac_vita_crt_atexit[2] !=
            ISAAC_VITA_CRT_ATEXIT_POST_MEMORY_CALLBACK_VA ||
         ld32(pointer32(&s_critical[1])) != ISAAC_VITA_SYNC_CS_MAGIC ||
         ld32(pointer32(&s_critical[2])) !=
             (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
        s_event_create_calls != event_calls + 1U || !s_event_active ||
        s_event_bits != 0U || s_bad_mock_call != 0U)
        return 46;
    if (s_boot_memory[0] != 0x11223344U ||
        s_boot_memory[31] != 0x55667788U)
        return 42;
    for (i = 1U; i < 31U; ++i) {
        if (s_boot_memory[i] != 0U)
            return 43;
    }
    if (s_environment_output[0] != 0xa5U ||
        s_environment_output[sizeof s_environment_output - 1U] != 0xa5U ||
        ld32(pointer32(s_system_info) + 20U) !=
            ISAAC_VITA_STARTUP_PROCESSOR_COUNT ||
        s_boot_fls_index == ISAAC_VITA_FLS_OUT_OF_INDEXES ||
        s_late_memory[0] != 0xccccccccU || s_late_memory[1] != 0U ||
        s_late_memory[2] != 0U || s_late_memory[3] != 0xccccccccU ||
        s_late_memory[4] != 0U || s_late_memory[5] != 0U ||
        s_late_memory[6] != 0xccccccccU ||
        s_performance[0] != 0U || s_performance[1] != 0U ||
        s_performance[2] != ISAAC_VITA_STARTUP_QPF_FREQUENCY ||
        s_performance[3] != 0U || s_performance[4] != 0U ||
        s_performance[5] != 0U || s_performance[6] != 0U ||
        s_performance[7] != 0U)
        return 47;

    /* The normal initializer table reaches this void stdcall 32 times at
     * 0x00562db2.  The seven records above are the separate whole-PE direct-
     * site census.  Every object gets independent embedded state, stores spin
     * zero in exactly 24 bytes, preserves EAX, and cleans only its argument. */
    import_calls = 0U;
    memset(s_plain_critical, 0xcc, sizeof s_plain_critical);
    for (i = 0U; i < ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT; ++i) {
        uint32_t *plain = s_plain_critical[i];
        uint32_t eax = 0x60000000U + i;

        plain[0] = 0x11000000U + i;
        plain[7] = 0x77000000U + i;
        esp = prepare_call(&cpu);
        cpu.eax = eax;
        s_frame[5] = pointer32(&plain[1]);
        if (!isaac_vita_sync_import_counted(
                &cpu, ISAAC_VITA_SYNC_PLAIN_CS_NAME, &import_calls))
            return 48;
        if (cpu.fault || cpu.esp != esp + 8U || cpu.eax != eax ||
            plain[0] != 0x11000000U + i ||
            plain[1] != ISAAC_VITA_SYNC_CS_MAGIC ||
            plain[2] !=
                (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
            plain[3] != 0U ||
            plain[4] != ISAAC_VITA_SYNC_CS_VERSION ||
            plain[5] != 0U || plain[6] != 0U ||
            plain[7] != 0x77000000U + i || s_bad_mock_call != 0U)
            return 49;
    }
    if (import_calls != ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT)
        return 50;

    /* The measured Steam callback registration is the exact next unknown
     * handoff.  Rejecting it cannot mutate CPU, CS storage, or the counter. */
    memcpy(s_plain_snapshot, s_plain_critical, sizeof s_plain_snapshot);
    esp = prepare_call(&cpu);
    cpu.eax = 0x89abcdefU;
    cpu.ecx = 0x10203040U;
    s_frame[5] = ISAAC_VITA_SYNC_LATE_NEXT_OBJECT_VA;
    s_frame[6] = ISAAC_VITA_SYNC_LATE_NEXT_CALLBACK_ID;
    memcpy(&cpu_snapshot, &cpu, sizeof cpu_snapshot);
    if (isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_LATE_NEXT_NAME, &import_calls) != 0 ||
        memcmp(&cpu, &cpu_snapshot, sizeof cpu) != 0 ||
        memcmp(s_plain_critical, s_plain_snapshot,
               sizeof s_plain_critical) != 0 ||
        cpu.esp != esp ||
        import_calls != ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT)
        return 51;

    /* Plain initialization never allocates a Vita mutex UID.  Arbitrary old
     * bytes are replaced in exactly the six guest dwords, while canaries and
     * the void stdcall result survive. */
    for (i = 0U; i < 8U; ++i)
        s_plain_failure[i] = 0x81000000U + i;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0xaabbccddU;
    s_frame[5] = pointer32(&s_plain_failure[1]);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_PLAIN_CS_NAME, &import_calls))
        return 52;
    if (cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0xaabbccddU ||
        s_plain_failure[0] != 0x81000000U ||
        s_plain_failure[7] != 0x81000007U ||
        s_plain_failure[1] != ISAAC_VITA_SYNC_CS_MAGIC ||
        s_plain_failure[2] !=
            (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
        s_plain_failure[3] != 0U ||
        s_plain_failure[4] != ISAAC_VITA_SYNC_CS_VERSION ||
        s_plain_failure[5] != 0U || s_plain_failure[6] != 0U ||
        s_log_calls != logs ||
        import_calls != ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT + 1U)
        return 53;

    if (!isaac_vita_sync_cs_delete(
            &cpu, pointer32(&s_plain_failure[1])) || cpu.fault ||
        !isaac_vita_sync_cs_initialize_plain(
            &cpu, pointer32(&s_plain_failure[1])) || cpu.fault ||
        !isaac_vita_sync_cs_delete(
            &cpu, pointer32(&s_plain_failure[1])) || cpu.fault)
        return 54;

    /* Every object has the same backend marker but independent owner/depth
     * words.  Exercise blocking recursion plus the complete stdcall ABI for
     * TryEnterCriticalSection: recursive/free succeed, foreign returns FALSE
     * immediately without a scheduler delay or mutation. */
    memset(s_lw_fallback_critical, 0xcc, sizeof s_lw_fallback_critical);
    for (i = 0U; i < 3U; ++i) {
        uint32_t *critical = s_lw_fallback_critical[i];

        if (!isaac_vita_sync_cs_initialize_plain(
                &cpu, pointer32(&critical[1])) || cpu.fault ||
            critical[0] != 0xccccccccU ||
            critical[1] != ISAAC_VITA_SYNC_CS_MAGIC ||
            critical[2] !=
                (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
            critical[3] != 0U ||
            critical[4] != ISAAC_VITA_SYNC_CS_VERSION ||
            critical[5] != 0U || critical[6] != 0U ||
            critical[7] != 0xccccccccU)
            return 97;
    }

    if (!isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) ||
        !isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) ||
        !isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_lw_fallback_critical[1][1])) ||
        cpu.fault || s_lw_fallback_critical[0][5] !=
            (uint32_t)s_thread_id ||
        s_lw_fallback_critical[0][6] != 2U ||
        s_lw_fallback_critical[1][5] != (uint32_t)s_thread_id ||
        s_lw_fallback_critical[1][6] != 1U)
        return 101;

    esp = prepare_call(&cpu);
    cpu.eax = 0xababababU;
    s_frame[5] = pointer32(&s_lw_fallback_critical[0][1]);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME, &try_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 1U ||
        try_calls != 1U || s_lw_fallback_critical[0][6] != 3U)
        return 98;

    delay_calls = s_delay_calls;
    s_thread_id = (int32_t)UINT32_C(0x40010004);
    esp = prepare_call(&cpu);
    cpu.eax = 0xcdcdcdcdU;
    s_frame[5] = pointer32(&s_lw_fallback_critical[0][1]);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME, &try_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U ||
        try_calls != 2U || s_delay_calls != delay_calls ||
        s_lw_fallback_critical[0][5] != UINT32_C(0x40010003) ||
        s_lw_fallback_critical[0][6] != 3U)
        return 109;

    esp = prepare_call(&cpu);
    s_frame[5] = pointer32(&s_lw_fallback_critical[2][1]);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME, &try_calls) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 1U ||
        try_calls != 3U ||
        s_lw_fallback_critical[2][5] != (uint32_t)s_thread_id ||
        s_lw_fallback_critical[2][6] != 1U ||
        !isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_lw_fallback_critical[2][1])) || cpu.fault)
        return 99;

    /* A different owner cannot leave, and blocking contention reports an
     * exact scheduler failure without changing the original lock. */
    logs = s_log_calls;
    if (isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) != 0 ||
        !cpu.fault ||
        strcmp(cpu.fault, "Vita recursive mutex unlock failed") != 0 ||
        strcmp(s_log_operation, "VitaEmbeddedMutexUnlock") != 0 ||
        s_log_calls != logs + 1U ||
        s_lw_fallback_critical[0][5] != UINT32_C(0x40010003) ||
        s_lw_fallback_critical[0][6] != 3U)
        return 100;
    clear_fault(&cpu);
    s_delay_force = 1;
    s_delay_result = -0x4501;
    logs = s_log_calls;
    if (isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) != 0 ||
        !cpu.fault ||
        strcmp(cpu.fault, "Vita recursive mutex lock failed") != 0 ||
        strcmp(s_log_operation, "VitaEmbeddedMutexLock") != 0 ||
        s_log_result != (uint32_t)-0x4501 ||
        s_log_calls != logs + 1U || s_delay_calls != delay_calls + 1U ||
        s_lw_fallback_critical[0][5] != UINT32_C(0x40010003) ||
        s_lw_fallback_critical[0][6] != 3U)
        return 110;
    s_delay_force = 0;
    clear_fault(&cpu);
    s_thread_id = (int32_t)UINT32_C(0x40010003);
    if (!isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) ||
        !isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) ||
        !isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_lw_fallback_critical[0][1])) ||
        !isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_lw_fallback_critical[1][1])) ||
        cpu.fault)
        return 102;

    /* Overflow is an attributed TryEnter failure: no stdcall cleanup and no
     * wrap.  Restore the synthetic state only after observing the failure. */
    s_lw_fallback_critical[2][5] = (uint32_t)s_thread_id;
    s_lw_fallback_critical[2][6] = UINT32_MAX;
    logs = s_log_calls;
    esp = prepare_call(&cpu);
    cpu.eax = 0x13579bdfU;
    s_frame[5] = pointer32(&s_lw_fallback_critical[2][1]);
    if (!isaac_vita_sync_import_counted(
            &cpu, ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME, &try_calls) ||
        !cpu.fault || cpu.esp != esp || cpu.eax != 0x13579bdfU ||
        cpu.fault_addr !=
            GUEST_IMAGE_BASE + ISAAC_VITA_SYNC_TRY_ENTER_CS_IAT_RVA ||
        strcmp(cpu.fault, "Vita recursive mutex try-lock failed") != 0 ||
        strcmp(s_log_operation,
               "VitaEmbeddedTryEnterCriticalSection") != 0 ||
        s_log_calls != logs + 1U || try_calls != 4U ||
        s_lw_fallback_critical[2][6] != UINT32_MAX)
        return 115;
    s_lw_fallback_critical[2][5] = 0U;
    s_lw_fallback_critical[2][6] = 0U;
    clear_fault(&cpu);

    s_lw_fallback_critical[2][6] = 1U;
    if (isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_lw_fallback_critical[2][1])) != 0 ||
        !cpu.fault ||
        strcmp(cpu.fault, "Vita recursive mutex lock failed") != 0 ||
        s_lw_fallback_critical[2][5] != 0U ||
        s_lw_fallback_critical[2][6] != 1U)
        return 123;
    s_lw_fallback_critical[2][6] = 0U;
    clear_fault(&cpu);

    for (i = 0U; i < 3U; ++i) {
        uint32_t *critical = s_lw_fallback_critical[i];
        unsigned word;

        if (!isaac_vita_sync_cs_delete(
                &cpu, pointer32(&critical[1])) || cpu.fault)
            return 103;
        for (word = 1U; word < 7U; ++word) {
            if (critical[word] != 0U)
                return 104;
        }
        if (critical[0] != 0xccccccccU ||
            critical[7] != 0xccccccccU)
            return 105;
    }

    /* The hardware reached 4096 simultaneous reference-count locks.  Keep
     * 4097 real guest critical sections live and prove there is no backend
     * capacity edge, then exercise more than the deleted 255-thread table. */
    isaac_vita_sync_get_mutex_pool_snapshot(&pool_before);
    memset(s_embedded_stress, 0xcc, sizeof s_embedded_stress);
    for (i = 0U; i < 4097U; ++i) {
        uint32_t *critical = s_embedded_stress[i];

        critical[0] = 0x92000000U + i;
        critical[7] = 0x93000000U + i;
        if (!isaac_vita_sync_cs_initialize_plain(
                &cpu, pointer32(&critical[1])) || cpu.fault ||
            critical[0] != 0x92000000U + i ||
            critical[1] != ISAAC_VITA_SYNC_CS_MAGIC ||
            critical[2] !=
                (uint32_t)ISAAC_VITA_SYNC_USER_EMBEDDED_HANDLE ||
            critical[3] != 0U ||
            critical[4] != ISAAC_VITA_SYNC_CS_VERSION ||
            critical[5] != 0U || critical[6] != 0U ||
            critical[7] != 0x93000000U + i)
            return 111;
    }
    isaac_vita_sync_get_mutex_pool_snapshot(&pool_snapshot);
    if (!pool_snapshot.enabled ||
        pool_snapshot.live != pool_before.live + 4097U ||
        pool_snapshot.creates != pool_before.creates + 4097U ||
        pool_snapshot.deletes != pool_before.deletes ||
        pool_snapshot.peak < pool_snapshot.live)
        return 107;

    s_thread_id = (int32_t)UINT32_C(0x40010003);
    if (!isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_embedded_stress[0][1])) || cpu.fault)
        return 108;
    s_thread_id = (int32_t)UINT32_C(0x40010004);
    if (!isaac_vita_sync_cs_enter(
            &cpu, pointer32(&s_embedded_stress[4096][1])) || cpu.fault ||
        s_embedded_stress[0][5] != UINT32_C(0x40010003) ||
        s_embedded_stress[4096][5] != UINT32_C(0x40010004) ||
        !isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_embedded_stress[4096][1])) || cpu.fault)
        return 116;
    s_thread_id = (int32_t)UINT32_C(0x40010003);
    if (!isaac_vita_sync_cs_leave(
            &cpu, pointer32(&s_embedded_stress[0][1])) || cpu.fault)
        return 117;
    for (i = 0U; i < 512U; ++i) {
        s_thread_id = (int32_t)(UINT32_C(0x40011000) + i);
        if (!isaac_vita_sync_cs_enter(
                &cpu, pointer32(&s_embedded_stress[0][1])) || cpu.fault ||
            !isaac_vita_sync_cs_leave(
                &cpu, pointer32(&s_embedded_stress[0][1])) || cpu.fault)
            return 118;
    }
    s_thread_id = (int32_t)UINT32_C(0x40010003);
    for (i = 0U; i < 4097U; ++i) {
        if (!isaac_vita_sync_cs_delete(
                &cpu, pointer32(&s_embedded_stress[i][1])) || cpu.fault)
            return 112;
    }
    isaac_vita_sync_get_mutex_pool_snapshot(&pool_snapshot);
    if (pool_snapshot.live != pool_before.live ||
        pool_snapshot.creates != pool_before.creates + 4097U ||
        pool_snapshot.deletes != pool_before.deletes + 4097U ||
        pool_snapshot.waits < 1U || pool_snapshot.wait_failures < 1U ||
        pool_snapshot.owner_failures < 1U ||
        pool_snapshot.state_failures < 1U)
        return 106;

    /* Reject bad guest boundaries before any direct atomic pointer exists. */
    for (i = 0U; i < 8U; ++i)
        s_plain_failure[i] = 0xa1000000U + i;
    memcpy(cs_snapshot, &s_plain_failure[1], sizeof cs_snapshot);
    if (isaac_vita_sync_cs_initialize_plain(
            &cpu, pointer32(&s_plain_failure[1]) + 1U) != 0 ||
        !cpu.fault ||
        strcmp(cpu.fault, "critical section address is invalid") != 0 ||
        memcmp(&s_plain_failure[1], cs_snapshot, sizeof cs_snapshot) != 0)
        return 107;
    clear_fault(&cpu);

    /* Retire the 32 boot-table objects and the one complete boot object.  A
     * deleted address is stale, but reinitializing that same storage works. */
    for (i = 0U; i < ISAAC_VITA_SYNC_PLAIN_CS_BOOT_CALL_COUNT; ++i) {
        if (!isaac_vita_sync_cs_delete(
                &cpu, pointer32(&s_plain_critical[i][1])) || cpu.fault)
            return 119;
    }
    if (!isaac_vita_sync_cs_delete(&cpu, pointer32(&s_critical[1])) ||
        cpu.fault)
        return 120;
    if (isaac_vita_sync_cs_enter(&cpu, pointer32(&s_critical[1])) != 0 ||
        !cpu.fault ||
        strcmp(cpu.fault, "critical section is not initialized") != 0)
        return 121;
    clear_fault(&cpu);
    if (!isaac_vita_sync_cs_initialize_plain(
            &cpu, pointer32(&s_critical[1])) || cpu.fault ||
        !isaac_vita_sync_cs_delete(&cpu, pointer32(&s_critical[1])) ||
        cpu.fault)
        return 122;

    /* The production runner binds its one stack-owned CPU once.  Freeze the
     * fast path across direct and imported recursive operations, then prove
     * clearing the binding retains the old per-operation lookup fallback. */
    memset(&cached_cpu, 0, sizeof cached_cpu);
    memset(s_cached_thread_critical, 0xcc,
           sizeof s_cached_thread_critical);
    if (!isaac_vita_sync_cs_initialize_plain(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        cached_cpu.fault)
        return 124;
    thread_id_calls = s_thread_id_calls;
    isaac_vita_sync_bind_current_thread(&cached_cpu);
    if (cached_cpu.vita_sync_thread_id != (uint32_t)s_thread_id ||
        s_thread_id_calls != thread_id_calls + 1U)
        return 125;
    thread_id_calls = s_thread_id_calls;
    if (!isaac_vita_sync_cs_enter(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        !isaac_vita_sync_cs_enter(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        cached_cpu.fault || s_cached_thread_critical[5] !=
            (uint32_t)s_thread_id || s_cached_thread_critical[6] != 2U)
        return 126;
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[4]);
    s_frame[4] = 0x0badc0deU;
    s_frame[5] = pointer32(&s_cached_thread_critical[1]);
    cached_cpu.esp = esp;
    cached_cpu.eax = 0xababababU;
    if (!isaac_vita_sync_import_counted(
            &cached_cpu, ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME,
            &cached_try_calls) || cached_cpu.fault ||
        cached_cpu.esp != esp + 8U || cached_cpu.eax != 1U ||
        cached_try_calls != 1U || s_cached_thread_critical[6] != 3U ||
        s_thread_id_calls != thread_id_calls)
        return 127;
    if (!isaac_vita_sync_cs_leave(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        !isaac_vita_sync_cs_leave(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        !isaac_vita_sync_cs_leave(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        cached_cpu.fault || s_cached_thread_critical[5] != 0U ||
        s_cached_thread_critical[6] != 0U ||
        s_thread_id_calls != thread_id_calls)
        return 128;
    cached_cpu.vita_sync_thread_id = 0U;
    if (!isaac_vita_sync_cs_enter(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        !isaac_vita_sync_cs_leave(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        cached_cpu.fault || s_thread_id_calls != thread_id_calls + 2U)
        return 129;
    if (!isaac_vita_sync_cs_delete(
            &cached_cpu, pointer32(&s_cached_thread_critical[1])) ||
        cached_cpu.fault || s_cached_thread_critical[0] != 0xccccccccU ||
        s_cached_thread_critical[7] != 0xccccccccU)
        return 130;
    isaac_vita_sync_get_mutex_pool_snapshot(&pool_snapshot);
    if (!pool_snapshot.enabled || pool_snapshot.live != 0U ||
        pool_snapshot.creates != pool_snapshot.deletes ||
        try_calls != 4U || s_bad_mock_call != 0U)
        return 113;

    return 0;
}
