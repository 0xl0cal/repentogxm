/* Executable ARM oracle for the first measured Vita boot-import batch.
 *
 * This links the production handler with deterministic platform providers.
 * It checks guest writes, x86 stdcall cleanup, return registers, call counts,
 * the loud next frontier, and the RTC failure path on emitted ARM code. */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

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

#if GUEST_IMAGE_BASE != 0x98000000u
#error Vita boot-import oracle requires the production guest base
#endif

static uint64_t s_filetime;
static uint64_t s_process_time;
static uint32_t s_thread_id;
static uint32_t s_process_id;
static int s_filetime_status;
static unsigned s_filetime_calls;
static unsigned s_thread_id_calls;
static unsigned s_process_id_calls;
static unsigned s_process_time_calls;
static unsigned s_log_calls;
static unsigned s_crt_delegate_calls;
static unsigned s_crt_delegate_name_matches;
static unsigned s_sync_delegate_calls;
static unsigned s_memory_delegate_calls;
static unsigned s_memory_delegate_name_matches;
static unsigned s_console_delegate_calls;
static int s_crt_delegate_handle;
static int s_memory_delegate_handle;

int isaac_vita_crt_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    (void)c;
    s_crt_delegate_calls++;
    if (strcmp(name, ISAAC_VITA_CRT_INITTERM_E_NAME) == 0)
        s_crt_delegate_name_matches++;
    if (!s_crt_delegate_handle)
        return 0;
    ++*call_count;
    return 1;
}

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
    s_memory_delegate_calls++;
    if (strcmp(name, ISAAC_VITA_MEMORY_MEMSET_NAME) == 0)
        s_memory_delegate_name_matches++;
    if (!s_memory_delegate_handle)
        return 0;
    ++*call_count;
    return 1;
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
ISAAC_REJECTING_DELEGATE(isaac_vita_file_lock_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_filesystem_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_math_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_rtti_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_find_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_com_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_post_com_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_heap_import_counted)
ISAAC_REJECTING_DELEGATE(isaac_vita_steam_import_counted)
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

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    s_filetime_calls++;
    if (s_filetime_status < 0)
        return s_filetime_status;
    *filetime = s_filetime;
    return 0;
}

uint32_t isaac_vita_get_thread_id(void)
{
    s_thread_id_calls++;
    return s_thread_id;
}

uint32_t isaac_vita_get_process_id(void)
{
    s_process_id_calls++;
    return s_process_id;
}

uint64_t isaac_vita_get_process_time(void)
{
    s_process_time_calls++;
    return s_process_time;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
    s_log_calls++;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    /* The production function unwinds through guest_run_until_stop.  Returning
     * here lets this focused oracle inspect the exact pre-unwind snapshot. */
    c->fault_addr = address;
    c->fault = what;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static void reset_cpu(CPU *c)
{
    memset(c, 0, sizeof *c);
}

int main(void)
{
    static uint32_t frame[6];
    static uint32_t output[4];
    CPU cpu;
    uint32_t frame_esp;
    uint32_t output_address;
    unsigned calls_before;

    if (ISAAC_VITA_SYSTEM_TIME_IMPORT_RVA != 0x00606084U ||
        ISAAC_VITA_SYSTEM_TIME_IMPORT_RETURN != 0x005ebffaU ||
        strcmp(ISAAC_VITA_SYSTEM_TIME_IMPORT_NAME,
               "KERNEL32.dll!GetSystemTimeAsFileTime") != 0 ||
        ISAAC_VITA_THREAD_ID_IMPORT_RVA != 0x0060612cU ||
        ISAAC_VITA_THREAD_ID_IMPORT_RETURN != 0x005ec009U ||
        strcmp(ISAAC_VITA_THREAD_ID_IMPORT_NAME,
               "KERNEL32.dll!GetCurrentThreadId") != 0 ||
        ISAAC_VITA_PROCESS_ID_IMPORT_RVA != 0x00606114U ||
        ISAAC_VITA_PROCESS_ID_IMPORT_RETURN != 0x005ec012U ||
        strcmp(ISAAC_VITA_PROCESS_ID_IMPORT_NAME,
               "KERNEL32.dll!GetCurrentProcessId") != 0 ||
        ISAAC_VITA_QPC_IMPORT_RVA != 0x006060c4U ||
        ISAAC_VITA_QPC_IMPORT_RETURN != 0x005ec01fU ||
        strcmp(ISAAC_VITA_QPC_IMPORT_NAME,
               "KERNEL32.dll!QueryPerformanceCounter") != 0 ||
        ISAAC_VITA_FIRST4_NEXT_IMPORT_RVA != 0x00606030U ||
        ISAAC_VITA_FIRST4_NEXT_IMPORT_RETURN != 0x005ebaf7U ||
        strcmp(ISAAC_VITA_FIRST4_NEXT_IMPORT_NAME,
               "KERNEL32.dll!IsProcessorFeaturePresent") != 0 ||
        ISAAC_VITA_PROCESSOR_FEATURE_CALL_RVA != 0x005ebaf1U ||
        ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT != 10U ||
        ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_COUNT != 1U ||
        ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT != 4U)
        return 14;

    if (!pointer_fits(&frame[0]) || !pointer_fits(&output[0]))
        return 1;
    frame_esp = (uint32_t)(uintptr_t)&frame[1];
    output_address = (uint32_t)(uintptr_t)&output[1];

    /* VOID WINAPI GetSystemTimeAsFileTime(LPFILETIME): preserve EAX, write
     * low/high dwords, pop return + four argument bytes. */
    reset_cpu(&cpu);
    frame[0] = 0x0f0e0d0cU;
    frame[1] = 0x11112222U;
    frame[2] = output_address;
    frame[3] = 0x33334444U;
    output[0] = 0x55667788U;
    output[1] = 0xccccccccU;
    output[2] = 0xccccccccU;
    output[3] = 0x99aabbccU;
    cpu.esp = frame_esp;
    cpu.eax = 0x13579bdfU;
    s_filetime = UINT64_C(0x01dc123489abcdef);
    s_filetime_status = 0;
    if (!guest_host_import(&cpu, ISAAC_VITA_SYSTEM_TIME_IMPORT_NAME))
        return 2;
    if (cpu.esp != frame_esp + 8U || cpu.eax != 0x13579bdfU ||
        output[0] != 0x55667788U || output[1] != 0x89abcdefU ||
        output[2] != 0x01dc1234U || output[3] != 0x99aabbccU ||
        frame[0] != 0x0f0e0d0cU || frame[1] != 0x11112222U ||
        frame[2] != output_address || frame[3] != 0x33334444U ||
        cpu.fault != NULL || s_filetime_calls != 1U)
        return 3;

    /* DWORD WINAPI GetCurrentThreadId(void): one return-address pop. */
    reset_cpu(&cpu);
    frame[1] = 0x22223333U;
    frame[2] = 0x44445555U;
    cpu.esp = frame_esp;
    s_thread_id = 0xf1020304U;
    if (!guest_host_import(&cpu, ISAAC_VITA_THREAD_ID_IMPORT_NAME))
        return 4;
    if (cpu.esp != frame_esp + 4U || cpu.eax != s_thread_id ||
        frame[1] != 0x22223333U || frame[2] != 0x44445555U ||
        s_thread_id_calls != 1U)
        return 5;

    /* DWORD WINAPI GetCurrentProcessId(void): same exact no-arg stdcall. */
    reset_cpu(&cpu);
    frame[1] = 0x33334444U;
    frame[2] = 0x55556666U;
    cpu.esp = frame_esp;
    s_process_id = 0xa5060708U;
    if (!guest_host_import(&cpu, ISAAC_VITA_PROCESS_ID_IMPORT_NAME))
        return 6;
    if (cpu.esp != frame_esp + 4U || cpu.eax != s_process_id ||
        frame[1] != 0x33334444U || frame[2] != 0x55556666U ||
        s_process_id_calls != 1U)
        return 7;

    /* BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *): process time is
     * the Vita monotonic microsecond tick; BOOL success is exactly one. */
    reset_cpu(&cpu);
    frame[1] = 0x44445555U;
    frame[2] = output_address;
    frame[3] = 0x66667777U;
    output[0] = 0x10203040U;
    output[1] = 0xccccccccU;
    output[2] = 0xccccccccU;
    output[3] = 0x50607080U;
    cpu.esp = frame_esp;
    s_process_time = UINT64_C(0x0123456789abcdef);
    if (!guest_host_import(&cpu, ISAAC_VITA_QPC_IMPORT_NAME))
        return 8;
    if (cpu.esp != frame_esp + 8U || cpu.eax != 1U ||
        output[0] != 0x10203040U || output[1] != 0x89abcdefU ||
        output[2] != 0x01234567U || output[3] != 0x50607080U ||
        frame[1] != 0x44445555U || frame[2] != output_address ||
        frame[3] != 0x66667777U || s_process_time_calls != 1U)
        return 9;

    if (g_host_import_calls != ISAAC_VITA_IMPLEMENTED_IMPORT_COUNT)
        return 10;

    /* Exact live call #5: BOOL WINAPI IsProcessorFeaturePresent(10). */
    reset_cpu(&cpu);
    frame[1] = 0x55556666U;
    frame[2] = ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT;
    frame[3] = 0x77778888U;
    cpu.esp = frame_esp;
    calls_before = g_host_import_calls;
    if (!guest_host_import(&cpu,
                           ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME) ||
        cpu.esp != frame_esp + 8U || cpu.eax != 1U ||
        frame[1] != 0x55556666U ||
        frame[2] != ISAAC_VITA_PROCESSOR_FEATURE_BOOT_ARGUMENT ||
        frame[3] != 0x77778888U ||
        g_host_import_calls != calls_before + 1U ||
        s_crt_delegate_calls != 0U || s_sync_delegate_calls != 0U)
        return 16;

    /* A rejecting CRT delegate leaves `_initterm_e` and the count untouched. */
    reset_cpu(&cpu);
    cpu.esp = frame_esp;
    cpu.eax = 0xdeadbeefU;
    calls_before = g_host_import_calls;
    s_crt_delegate_handle = 0;
    if (guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME) != 0 ||
        g_host_import_calls != calls_before || cpu.esp != frame_esp ||
        cpu.eax != 0xdeadbeefU || cpu.fault != NULL ||
        s_crt_delegate_calls != 1U || s_crt_delegate_name_matches != 1U ||
        s_sync_delegate_calls != 1U || s_memory_delegate_calls != 1U ||
        s_console_delegate_calls != 1U)
        return 11;

    /* A matched delegate owns its count before entering a handler.  The real
     * CRT oracle below exercises that contract through a nested guest fault. */
    reset_cpu(&cpu);
    cpu.esp = frame_esp;
    cpu.eax = 0x76543210U;
    calls_before = g_host_import_calls;
    s_crt_delegate_handle = 1;
    if (!guest_host_import(&cpu, ISAAC_VITA_CRT_INITTERM_E_NAME) ||
        g_host_import_calls != calls_before + 1U ||
        cpu.esp != frame_esp || cpu.eax != 0x76543210U ||
        cpu.fault != NULL || s_crt_delegate_calls != 2U ||
        s_crt_delegate_name_matches != 2U || s_sync_delegate_calls != 1U ||
        s_memory_delegate_calls != 1U || s_console_delegate_calls != 1U)
        return 15;

    /* A target clock failure is loud and leaves the x86 frame/output intact;
     * production guest_fault unwinds immediately from this same snapshot. */
    reset_cpu(&cpu);
    frame[1] = 0x77778888U;
    frame[2] = output_address;
    output[1] = 0xa1a2a3a4U;
    output[2] = 0xb1b2b3b4U;
    cpu.esp = frame_esp;
    cpu.eax = 0xc1c2c3c4U;
    s_filetime_status = -0x1234;
    calls_before = g_host_import_calls;
    if (!guest_host_import(&cpu, ISAAC_VITA_SYSTEM_TIME_IMPORT_NAME))
        return 12;
    if (g_host_import_calls != calls_before + 1U ||
        cpu.esp != frame_esp || cpu.eax != 0xc1c2c3c4U ||
        output[1] != 0xa1a2a3a4U || output[2] != 0xb1b2b3b4U ||
        cpu.fault_addr != GUEST_IMAGE_BASE +
                          ISAAC_VITA_SYSTEM_TIME_IMPORT_RVA ||
        !cpu.fault || strcmp(cpu.fault,
            "Vita RTC failed for GetSystemTimeAsFileTime") != 0 ||
        s_filetime_calls != 2U || s_log_calls != 1U)
        return 13;

    /* The sibling SSE feature remains true; unrelated feature IDs are false.
     * Both use the same one-argument stdcall cleanup and bypass delegates. */
    reset_cpu(&cpu);
    frame[1] = 0x88889999U;
    frame[2] = ISAAC_VITA_PROCESSOR_FEATURE_SSE;
    cpu.esp = frame_esp;
    calls_before = g_host_import_calls;
    if (!guest_host_import(&cpu,
                           ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME) ||
        cpu.esp != frame_esp + 8U || cpu.eax != 1U ||
        frame[2] != ISAAC_VITA_PROCESSOR_FEATURE_SSE ||
        g_host_import_calls != calls_before + 1U)
        return 17;
    reset_cpu(&cpu);
    frame[1] = 0x9999aaaaU;
    frame[2] = 7U;
    cpu.esp = frame_esp;
    calls_before = g_host_import_calls;
    if (!guest_host_import(&cpu,
                           ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME) ||
        cpu.esp != frame_esp + 8U || cpu.eax != 0U || frame[2] != 7U ||
        g_host_import_calls != calls_before + 1U ||
        s_crt_delegate_calls != 2U || s_sync_delegate_calls != 1U ||
        s_memory_delegate_calls != 1U || s_console_delegate_calls != 1U)
        return 18;

    /* The outer dispatcher reaches memory only after CRT and sync reject the
     * exact name, and a match owns its count before entering the handler. */
    reset_cpu(&cpu);
    cpu.esp = frame_esp;
    cpu.eax = 0xabcdef01U;
    calls_before = g_host_import_calls;
    s_memory_delegate_handle = 0;
    if (guest_host_import(&cpu, ISAAC_VITA_MEMORY_MEMSET_NAME) != 0 ||
        g_host_import_calls != calls_before || cpu.esp != frame_esp ||
        cpu.eax != 0xabcdef01U || s_crt_delegate_calls != 3U ||
        s_sync_delegate_calls != 2U || s_memory_delegate_calls != 2U ||
        s_memory_delegate_name_matches != 1U ||
        s_console_delegate_calls != 2U)
        return 19;
    s_memory_delegate_handle = 1;
    if (!guest_host_import(&cpu, ISAAC_VITA_MEMORY_MEMSET_NAME) ||
        g_host_import_calls != calls_before + 1U || cpu.esp != frame_esp ||
        cpu.eax != 0xabcdef01U || s_crt_delegate_calls != 4U ||
        s_sync_delegate_calls != 3U || s_memory_delegate_calls != 3U ||
        s_memory_delegate_name_matches != 2U ||
        s_console_delegate_calls != 2U)
        return 20;

    return 0;
}
