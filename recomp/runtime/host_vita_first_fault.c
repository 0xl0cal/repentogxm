/* Minimal Vita host-API surface for the measured boot-import frontier.
 *
 * guest.c owns the exact PE import table.  Every name absent below remains a
 * loud generated DLL!symbol fault; no Win32 behavior is guessed or silently
 * stubbed. */
#include <string.h>

#include "guest.h"
#include "host_vita_com.h"
#include "host_vita_console.h"
#include "host_vita_crt.h"
#include "host_vita_exception.h"
#include "host_vita_file_lock.h"
#include "host_vita_filesystem.h"
#include "host_vita_find.h"
#include "host_vita_fls.h"
#include "host_vita_gl.h"
#include "host_vita_heap.h"
#include "host_vita_import_id.h"
#include "host_vita_math.h"
#include "host_vita_memory.h"
#include "host_vita_post_com.h"
#include "host_vita_rtti.h"
#include "host_vita_startup.h"
#include "host_vita_steam.h"
#include "host_vita_sync.h"
#include "host_vita_user32.h"
#include "kage_vita_io_profile.h"
#include "kage_vita_loading.h"
#include "kage_vita_stall_probe.h"
#include "platform.h"
#include "vita_boot_imports.h"
#include "vita_host_services.h"

#ifndef ISAAC_VITA_AUDIO
#define ISAAC_VITA_AUDIO 0
#endif

#ifndef ISAAC_VITA_XINPUT
#define ISAAC_VITA_XINPUT 0
#endif

#if ISAAC_VITA_AUDIO
#include "host_vita_audio.h"
#endif

#if ISAAC_VITA_XINPUT
#include "host_vita_xinput.h"
#endif

unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
int g_guest_gl_inventory_mode;

#define ISAAC_VITA_LOADING_FREAD_GRANULARITY 512U
#define ISAAC_VITA_BASELINE_IMPORT_COUNT 5U

static uint32_t s_archive_fread_calls;

_Static_assert(
    (ISAAC_VITA_LOADING_FREAD_GRANULARITY &
     (ISAAC_VITA_LOADING_FREAD_GRANULARITY - 1U)) == 0U,
    "loading fread cadence mask requires a power-of-two granularity");

/* The non-KAGE frontier and standalone import oracles need no graphics
 * dependency.  The KAGE target links kage_vita_loading.c's strong definition;
 * noinline keeps this interposable weak fallback from being folded into the
 * hot dispatcher in this translation unit. */
#if defined(__GNUC__)
__attribute__((weak, noinline))
#endif
void kage_vita_loading_note_fread(uint32_t completed_calls)
{
    (void)completed_calls;
}

void guest_cpuid(CPU *__restrict c)
{
    uint32_t leaf = c->eax;

    /* Copy of the working PC virtual-CPU contract.  This describes the x86
     * code paths the translator supports, not the native ARM host. */
    c->eax = c->ebx = c->ecx = c->edx = 0U;
    if (leaf == 0U) {
        c->eax = 1U;
        c->ebx = 0x756e6547U;     /* "Genu" */
        c->edx = 0x49656e69U;     /* "ineI" */
        c->ecx = 0x6c65746eU;     /* "ntel" */
    } else if (leaf == 1U) {
        c->eax = 0x00000663U;
        c->edx = (1U << 0)  |     /* x87 */
                 (1U << 8)  |     /* CMPXCHG8B */
                 (1U << 15) |     /* CMOV */
                 (1U << 24) |     /* FXSAVE/FXRSTOR */
                 (1U << 25) |     /* SSE */
                 (1U << 26);      /* SSE2 */
    } else if (leaf == 0x80000000U) {
        c->eax = 0x80000000U;
    }
}

void guest_xgetbv(CPU *__restrict c)
{
    c->eax = c->ecx == 0U ? 3U : 0U;
    c->edx = 0U;
}

static uint32_t host_arg(CPU *__restrict c, uint32_t index)
{
    /* ESP points at the x86 return address while an import is active. */
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void host_stdcall_return(CPU *__restrict c, uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void host_GetSystemTimeAsFileTime(CPU *__restrict c)
{
    uint64_t value;
    uint32_t output = host_arg(c, 0U);
    int result = isaac_vita_get_win32_filetime(&value);

    if (result < 0) {
        isaac_vita_log(
            "GetSystemTimeAsFileTime host clock FAILED: 0x%08x",
            (unsigned)result);
        guest_fault(c,
                    GUEST_IMAGE_BASE + ISAAC_VITA_SYSTEM_TIME_IMPORT_RVA,
                    "Vita RTC failed for GetSystemTimeAsFileTime");
        return;
    }

    st32(output, (uint32_t)value);
    st32(output + 4U, (uint32_t)(value >> 32U));
    host_stdcall_return(c, 4U);
}

static void host_GetCurrentThreadId(CPU *__restrict c)
{
    c->eax = isaac_vita_get_thread_id();
    host_stdcall_return(c, 0U);
}

static void host_GetCurrentProcessId(CPU *__restrict c)
{
    c->eax = isaac_vita_get_process_id();
    host_stdcall_return(c, 0U);
}

static void host_QueryPerformanceCounter(CPU *__restrict c)
{
    uint64_t value = isaac_vita_get_process_time();
    uint32_t output = host_arg(c, 0U);

    st32(output, (uint32_t)value);
    st32(output + 4U, (uint32_t)(value >> 32U));
    c->eax = 1U;                 /* Win32 BOOL success */
    host_stdcall_return(c, 4U);
}

static void host_IsProcessorFeaturePresent(CPU *__restrict c)
{
    uint32_t feature = host_arg(c, 0U);

    /* Keep the target independent from the native ARM host: this is the same
     * virtual x86 SSE/SSE2 policy exposed by guest_cpuid and host_win32.c. */
    c->eax = feature == ISAAC_VITA_PROCESSOR_FEATURE_SSE ||
             feature == ISAAC_VITA_PROCESSOR_FEATURE_SSE2;
    host_stdcall_return(c, 4U);
}

const char *isaac_vita_baseline_import_name(uint32_t index)
{
    static const char *const names[ISAAC_VITA_BASELINE_IMPORT_COUNT] = {
        ISAAC_VITA_SYSTEM_TIME_IMPORT_NAME,
        ISAAC_VITA_THREAD_ID_IMPORT_NAME,
        ISAAC_VITA_PROCESS_ID_IMPORT_NAME,
        ISAAC_VITA_QPC_IMPORT_NAME,
        ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME
    };

    return index < ISAAC_VITA_BASELINE_IMPORT_COUNT ? names[index] : NULL;
}

int isaac_vita_baseline_import_indexed(CPU *__restrict c, uint32_t index,
                                       unsigned *call_count)
{
    static void (*const handlers[ISAAC_VITA_BASELINE_IMPORT_COUNT])(
        CPU *__restrict) = {
        host_GetSystemTimeAsFileTime,
        host_GetCurrentThreadId,
        host_GetCurrentProcessId,
        host_QueryPerformanceCounter,
        host_IsProcessorFeaturePresent
    };

    if (index >= ISAAC_VITA_BASELINE_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    handlers[index](c);
    return 1;
}

void isaac_vita_reset_archive_fread_epoch(void)
{
    /* Backend initialization runs before the one guest-dispatch owner and
     * before the stall reader thread.  No atomic RMW is needed on this edge. */
    s_archive_fread_calls = 0U;
}

void isaac_vita_note_archive_fread(void)
{
    uint32_t completed = ++s_archive_fread_calls;
    int loading_checkpoint =
        (completed & (ISAAC_VITA_LOADING_FREAD_GRANULARITY - 1U)) == 0U;

    if (loading_checkpoint) {
        /* Publish before either durable profiling or the loading presenter.
         * If either owner path itself stalls, the independent watchdog still
         * retains the exact completed 512-read boundary which reached it. */
        KAGE_VITA_STALL_NOTE_LOGICAL_FREAD(completed);
    }

#if defined(ISAAC_VITA_IO_PROFILE)
    /* One first marker plus 512-read polls.  The profiler itself writes only
     * at >=1024 power-of-two boundaries or after thirty seconds, so this is
     * neither a per-fread clock nor a per-fread durable logger. */
    if (completed == 1U || loading_checkpoint)
        kage_vita_io_profile_progress(completed);
#endif

    if (loading_checkpoint) {
        KAGE_VITA_STALL_TRACE_SYNC(
            KAGE_VITA_STALL_SYNC_FREAD_POWER, completed);
    }

    /* One cooperative clock poll per 512 dispatches, not one swap per
     * fread.  At the recorded 34.9K fread/s this gives the owner-thread
     * loading module enough samples for its 233854 us (about 4.276 Hz)
     * cadence; it still rejects every non-vitaGL-owner caller. */
    if (loading_checkpoint) {
        kage_vita_loading_note_fread(completed);
        KAGE_VITA_STALL_TRACE_SYNC(
            KAGE_VITA_STALL_SYNC_LOADING_NOTE_RETURN, completed);
    }
}

int isaac_vita_shared_loader_import_indexed(CPU *__restrict c,
                                            uint32_t shared_index)
{
    static const char *const names[] = {
        ISAAC_VITA_GL_LOAD_LIBRARY_NAME,
        ISAAC_VITA_GL_GET_PROC_NAME,
        ISAAC_VITA_GL_FREE_LIBRARY_NAME
    };

    if (shared_index >= sizeof names / sizeof names[0])
        return 0;
    /* These three IAT identities are selected by module token/name arguments,
     * not by import ID alone.  Re-enter the exact established XInput -> GL ->
     * sync/startup name chain instead of guessing a final handler. */
    return guest_host_import(c, names[shared_index]);
}

int guest_host_import(CPU *__restrict c, const char *name)
{
    /* This boundary, its CPU/gmem state and g_host_import_calls are all owned
     * by one guest-dispatch thread.  Keep the matching archive counter plain:
     * making only it atomic would add 2.07M hot-path RMWs without making the
     * surrounding dispatcher safe for concurrent entry. */
    /* The frozen PC coverage archive records 2,070,715 fread dispatches.
     * Route that exact immutable import name before the five baseline checks;
     * CRT still owns lookup, counting and the FILE registry.  The one local
     * counter feeds a coarse, process-lifetime loading estimate. */
    if (strcmp(name, ISAAC_VITA_CRT_FREAD_NAME) == 0) {
        int handled = isaac_vita_crt_import_counted(
            c, name, &g_host_import_calls);
        isaac_vita_note_archive_fread();
        return handled;
    }
    if (strcmp(name, ISAAC_VITA_SYSTEM_TIME_IMPORT_NAME) == 0) {
        return isaac_vita_baseline_import_indexed(
            c, 0U, &g_host_import_calls);
    }
    if (strcmp(name, ISAAC_VITA_THREAD_ID_IMPORT_NAME) == 0) {
        return isaac_vita_baseline_import_indexed(
            c, 1U, &g_host_import_calls);
    }
    if (strcmp(name, ISAAC_VITA_PROCESS_ID_IMPORT_NAME) == 0) {
        return isaac_vita_baseline_import_indexed(
            c, 2U, &g_host_import_calls);
    }
    if (strcmp(name, ISAAC_VITA_QPC_IMPORT_NAME) == 0) {
        return isaac_vita_baseline_import_indexed(
            c, 3U, &g_host_import_calls);
    }
    if (strcmp(name, ISAAC_VITA_PROCESSOR_FEATURE_IMPORT_NAME) == 0) {
        return isaac_vita_baseline_import_indexed(
            c, 4U, &g_host_import_calls);
    }
    if (isaac_vita_crt_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_math_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_rtti_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_exception_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_file_lock_import_counted(c, name,
                                             &g_host_import_calls))
        return 1;
    if (isaac_vita_filesystem_import_counted(c, name,
                                              &g_host_import_calls))
        return 1;
    if (isaac_vita_find_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_com_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_post_com_import_counted(c, name,
                                           &g_host_import_calls))
        return 1;
    if (isaac_vita_heap_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_steam_import_counted(c, name, &g_host_import_calls))
        return 1;
#if ISAAC_VITA_AUDIO
    /* Frozen OpenAL32 names are disjoint from every Win32 delegate.  Keep the
     * complete 24-entry cdecl block behind the same A/B switch as its archive
     * and Sound::Manager implementation. */
    if (isaac_vita_audio_import_counted(c, name, &g_host_import_calls))
        return 1;
#endif
    /* These delegates precede sync/startup because all four share KERNEL32
     * loader names.  Each claims only its own synthetic module/name argument
     * shapes and leaves every foreign frame byte-for-byte untouched. */
#if ISAAC_VITA_XINPUT
    if (isaac_vita_xinput_import_counted(c, name, &g_host_import_calls))
        return 1;
#endif
    if (isaac_vita_gl_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_sync_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_memory_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_console_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_user32_import_counted(c, name, &g_host_import_calls))
        return 1;
    if (isaac_vita_startup_import_counted(c, name, &g_host_import_calls))
        return 1;
    return isaac_vita_fls_import_counted(c, name, &g_host_import_calls);
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    uint32_t family = token & UINT32_C(0xff000000);

    /* Synthetic callable tokens are reserved to WGL/XInput (0x7d) and
     * generated typed GL (0x7e).  Ordinary guest RVAs/VAs go to guest_lookup
     * without entering either native adapter. */
    if (family != UINT32_C(0x7d000000) &&
        family != UINT32_C(0x7e000000))
        return 0;
#if ISAAC_VITA_XINPUT
    if (isaac_vita_xinput_dynamic_counted(c, token,
                                          &g_host_dynamic_calls))
        return 1;
#endif
    return isaac_vita_gl_dynamic_counted(c, token,
                                         &g_host_dynamic_calls);
}
