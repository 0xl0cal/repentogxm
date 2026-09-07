/* PC-first implementations of guest imports reached during startup.
 *
 * This is an ABI boundary, not a bag of convenient host calls.  Every shim
 * reads arguments from the emulated x86 stack, writes through guest pointers,
 * sets guest return registers, and performs the original callee cleanup.  A
 * name absent from this table remains a loud DLL!symbol fault in guest.c.
 */
#include <errno.h>
#include <io.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "gl_bridge.h"
#include "guest.h"
#include "host_frontier.h"
#include "kage_pc_backend.h"

typedef void (*host_import_fn)(CPU *__restrict);

typedef struct host_import_entry {
    const char    *name;
    host_import_fn fn;
} host_import_entry;

unsigned g_host_import_calls;

void guest_cpuid(CPU *__restrict c)
{
    uint32_t leaf = c->eax;

    /* This is the capability profile of the translated guest CPU, not of the
     * machine running the PC harness.  Passing the native host's AVX/AVX-512
     * bits through made MSVC's __isa_available_init select VEX/EVEX helper
     * paths that the ARMv7-oriented translator deliberately does not expose.
     * Report a stable SSE2-era Intel profile: all generated scalar/SSE2 paths
     * remain available, while newer optional paths take their own fallback. */
    c->eax = c->ebx = c->ecx = c->edx = 0U;
    if (leaf == 0U) {
        c->eax = 1U;              /* highest supported standard leaf */
        c->ebx = 0x756e6547U;     /* "Genu" */
        c->edx = 0x49656e69U;     /* "ineI" */
        c->ecx = 0x6c65746eU;     /* "ntel" */
    } else if (leaf == 1U) {
        c->eax = 0x00000663U;     /* family 6, model 6, stepping 3 */
        c->edx = (1U << 0)  |     /* x87 */
                 (1U << 8)  |     /* CMPXCHG8B */
                 (1U << 15) |     /* CMOV */
                 (1U << 24) |     /* FXSAVE/FXRSTOR */
                 (1U << 25) |     /* SSE */
                 (1U << 26);      /* SSE2 */
    } else if (leaf == 0x80000000U) {
        c->eax = 0x80000000U;     /* no extended feature leaves */
    }
}

void guest_xgetbv(CPU *__restrict c)
{
    /* CPUID above does not advertise OSXSAVE, so conforming guest code never
     * reaches XGETBV.  Keep a deterministic XCR0 value for a direct probe. */
    c->eax = c->ecx == 0U ? 3U : 0U; /* x87 + SSE state */
    c->edx = 0U;
}

static uint32_t host_arg(CPU *__restrict c, uint32_t index)
{
    /* ESP points at the guest return address while an imported function runs. */
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void host_stdcall_return(CPU *__restrict c, uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void host_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);             /* caller removes arguments */
}

static int call_guest_initializer(CPU *__restrict c, uint32_t target)
{
    uint32_t saved = c->esp;
    /* A translated RET must have a guest return address to pop even though
     * control returns to this native shim through the C call stack. */
    gpush(c, 0xFFF1A11EU);
    guest_call(c, target);
    if (c->fault) return 0;
    if (c->esp != saved) {
        guest_fault(c, target, "CRT callback did not restore its cdecl stack");
        return 0;
    }
    return 1;
}

static int call_guest_stdcall1(CPU *__restrict c, uint32_t target,
                               uint32_t argument)
{
    uint32_t saved = c->esp;
    gpush(c, argument);
    gpush(c, 0xFFF15A11U);
    guest_call(c, target);
    if (c->fault) return 0;
    if (c->esp != saved) {
        guest_fault(c, target,
                    "FLS callback did not clean its stdcall argument");
        return 0;
    }
    return 1;
}

static int call_guest_stdcall3(CPU *__restrict c, uint32_t target,
                               uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t saved = c->esp;
    gpush(c, a2);
    gpush(c, a1);
    gpush(c, a0);
    gpush(c, 0xFFF17311U);
    guest_call(c, target);
    if (c->fault) return 0;
    if (c->esp != saved) {
        guest_fault(c, target,
                    "TLS exit callback did not clean its stdcall arguments");
        return 0;
    }
    return 1;
}

static void host_GetSystemTimeAsFileTime(CPU *__restrict c)
{
    FILETIME value;
    uint32_t out = host_arg(c, 0);
    GetSystemTimeAsFileTime(&value);
    st32(out, value.dwLowDateTime);
    st32(out + 4U, value.dwHighDateTime);
    host_stdcall_return(c, 4U);
}

/* `DWORD WINAPI GetFileAttributesA(LPCSTR)`.  Filesystem paths are an actual
 * platform boundary, unlike guest CPU/CRT policy.  The PC harness therefore
 * asks Win32 about its controlled working directory; the Vita backend will
 * translate this same seam to its save/data roots. */
static void host_GetFileAttributesA(CPU *__restrict c)
{
    c->eax = GetFileAttributesA(
        (LPCSTR)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

/* `BOOL WINAPI CreateDirectoryA(LPCSTR, LPSECURITY_ATTRIBUTES)`.  The game
 * builds its save tree one component at a time and deliberately ignores an
 * ALREADY_EXISTS result before verifying the final path with
 * GetFileAttributesA. */
static void host_CreateDirectoryA(CPU *__restrict c)
{
    c->eax = (uint32_t)CreateDirectoryA(
        (LPCSTR)(uintptr_t)host_arg(c, 0),
        (LPSECURITY_ATTRIBUTES)(uintptr_t)host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

/* `DWORD WINAPI GetCurrentDirectoryA(DWORD, LPSTR)`.  run_entry.py fixes the
 * harness cwd before launch, so this is an explicit PC platform input rather
 * than whatever directory happened to invoke the build script. */
static void host_GetCurrentDirectoryA(CPU *__restrict c)
{
    c->eax = GetCurrentDirectoryA(
        host_arg(c, 0), (LPSTR)(uintptr_t)host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

/* `DWORD WINAPI GetFullPathNameW(LPCWSTR,DWORD,LPWSTR,LPWSTR*)`, copied from
 * the installed SDK's fileapi.h.  The PC oracle delegates path syntax to
 * Win32, but marshals the optional pointer-to-pointer explicitly so this
 * remains a real guest ABI seam.  The Vita backend will normalize against its
 * mounted data/save roots and still return a 16-bit Windows-style path. */
static void host_GetFullPathNameW(CPU *__restrict c)
{
    uint32_t file_name = host_arg(c, 0);
    uint32_t capacity = host_arg(c, 1);
    uint32_t buffer = host_arg(c, 2);
    uint32_t file_part_out = host_arg(c, 3);
    LPWSTR file_part = NULL;
    DWORD result = GetFullPathNameW(
        (LPCWSTR)(uintptr_t)file_name, capacity,
        (LPWSTR)(uintptr_t)buffer, file_part_out ? &file_part : NULL);
    if (file_part_out)
        st32(file_part_out, (uint32_t)(uintptr_t)file_part);
    c->eax = result;
    host_stdcall_return(c, 16U);
}

/* Win32 search handles are an opaque family: FindFirstFileW creates them,
 * FindNextFileW advances them, and FindClose (not CloseHandle) destroys them.
 * The 32-bit PC oracle can retain native handles and the exact x86
 * WIN32_FIND_DATAW layout.  Vita will replace the handle with an indexed
 * directory iterator while keeping these guest-visible contracts. */
static void host_FindFirstFileW(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)FindFirstFileW(
        (LPCWSTR)(uintptr_t)host_arg(c, 0),
        (LPWIN32_FIND_DATAW)(uintptr_t)host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

static void host_FindNextFileW(CPU *__restrict c)
{
    c->eax = (uint32_t)FindNextFileW(
        (HANDLE)(uintptr_t)host_arg(c, 0),
        (LPWIN32_FIND_DATAW)(uintptr_t)host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

static void host_FindClose(CPU *__restrict c)
{
    c->eax = (uint32_t)FindClose((HANDLE)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

/* These two imports consume the native HANDLE obtained from the same CRT
 * boundary as `_fileno`/`_get_osfhandle` below.  The PC oracle is deliberately
 * x86: its OVERLAPPED is the guest's five-dword (20-byte) layout, including
 * the split 64-bit file offset.  Keep the guest buffer in place so Win32 sees
 * the exact Internal/InternalHigh/Offset/OffsetHigh/hEvent fields. */
typedef char host_overlapped_must_match_x86[
    sizeof(OVERLAPPED) == 20U ? 1 : -1];

static void host_LockFileEx(CPU *__restrict c)
{
    c->eax = (uint32_t)LockFileEx(
        (HANDLE)(uintptr_t)host_arg(c, 0),
        host_arg(c, 1), host_arg(c, 2), host_arg(c, 3), host_arg(c, 4),
        (LPOVERLAPPED)(uintptr_t)host_arg(c, 5));
    host_stdcall_return(c, 24U);
}

static void host_UnlockFileEx(CPU *__restrict c)
{
    c->eax = (uint32_t)UnlockFileEx(
        (HANDLE)(uintptr_t)host_arg(c, 0),
        host_arg(c, 1), host_arg(c, 2), host_arg(c, 3),
        (LPOVERLAPPED)(uintptr_t)host_arg(c, 4));
    host_stdcall_return(c, 20U);
}

static void host_GetCurrentThreadId(CPU *__restrict c)
{
    c->eax = (uint32_t)GetCurrentThreadId();
    host_stdcall_return(c, 0U);
}

static void host_GetCurrentProcessId(CPU *__restrict c)
{
    c->eax = (uint32_t)GetCurrentProcessId();
    host_stdcall_return(c, 0U);
}

/* Power/display inhibition is meaningful on desktop but the Vita frontend
 * owns suspend policy.  Preserve Win32's observable previous-state return
 * while making the call itself a target-side no-op. */
static uint32_t s_execution_state = 0x80000000U; /* ES_CONTINUOUS */

static void host_SetThreadExecutionState(CPU *__restrict c)
{
    uint32_t requested = host_arg(c, 0);
    uint32_t previous = s_execution_state;
    if (requested & 0x80000000U) s_execution_state = requested;
    c->eax = previous;
    host_stdcall_return(c, 4U);
}

/* WinMM timer-resolution lifecycle.  Repentance queries the two-dword
 * TIMECAPS, requests wPeriodMin, and logs it.  A 1 ms reported minimum keeps
 * its timing math unchanged; the PC/Vita runtimes already own their clocks,
 * so begin/end are balanced no-ops rather than global host mutations. */
static void host_timeGetDevCaps(CPU *__restrict c)
{
    uint32_t caps = host_arg(c, 0);
    uint32_t size = host_arg(c, 1);
    if (!caps || size < 8U) {
        c->eax = 97U;            /* TIMERR_NOCANDO */
    } else {
        st32(caps, 1U);
        st32(caps + 4U, 1000U);
        c->eax = 0U;             /* TIMERR_NOERROR */
    }
    host_stdcall_return(c, 8U);
}

static void host_timeBeginPeriod(CPU *__restrict c)
{
    uint32_t period = host_arg(c, 0);
    c->eax = period >= 1U && period <= 1000U ? 0U : 97U;
    host_stdcall_return(c, 4U);
}

static void host_timeEndPeriod(CPU *__restrict c)
{
    uint32_t period = host_arg(c, 0);
    c->eax = period >= 1U && period <= 1000U ? 0U : 97U;
    host_stdcall_return(c, 4U);
}

static void host_timeGetTime(CPU *__restrict c)
{
    c->eax = (uint32_t)GetTickCount();
    host_stdcall_return(c, 0U);
}

/* Repentance temporarily clears Win32's foreground-lock timeout while its
 * window is active, then restores the value saved by a preceding GET.  Calling
 * the native SET action from a test harness would mutate the developer's
 * system-wide desktop policy.  Model the two actions locally instead: the
 * game observes the documented GET/SET round trip and Vita needs no desktop
 * foreground arbitration at all.  Other SPI actions remain loud until their
 * target policy is defined deliberately.
 *
 * BOOL WINAPI SystemParametersInfoA(UINT,UINT,PVOID,UINT), stdcall.  For
 * SPI_GETFOREGROUNDLOCKTIMEOUT pvParam points to a DWORD; for SET it carries
 * the timeout value directly, despite its pointer type in the generic API. */
static uint32_t s_foreground_lock_timeout = 200000U;

typedef int (WINAPI *host_get_system_metrics_fn)(int);
static HMODULE s_metrics_user32;
static host_get_system_metrics_fn s_native_get_system_metrics;

static int host_query_system_metric(CPU *__restrict c, int index, int *value)
{
    if (!s_native_get_system_metrics) {
        FARPROC address;
        s_metrics_user32 = LoadLibraryA("user32.dll");
        address = s_metrics_user32
            ? GetProcAddress(s_metrics_user32, "GetSystemMetrics") : NULL;
        if (!address) {
            guest_fault(c, GetLastError(),
                        "could not resolve native GetSystemMetrics");
            return 0;
        }
        s_native_get_system_metrics = (host_get_system_metrics_fn)address;
    }
    *value = s_native_get_system_metrics(index);
    return 1;
}

static void host_GetSystemMetrics(CPU *__restrict c)
{
    int index = (int32_t)host_arg(c, 0);
    int value;
    if (index != SM_CXSCREEN && index != SM_CYSCREEN &&
        index != SM_CXICON && index != SM_CYICON &&
        index != SM_CXSMICON && index != SM_CYSMICON) {
        guest_fault(c, (uint32_t)index,
                    "GetSystemMetrics outside the measured selectors");
        return;
    }
    if (!host_query_system_metric(c, index, &value))
        return;
    c->eax = (uint32_t)value;
    host_stdcall_return(c, 4U);
}

/* GLFW's Win32 clipboard helpers are used by the vanilla debug console's
 * Ctrl+V path.  In ordinary runs this remains a genuinely native PC boundary:
 * the 32-bit harness shares one address space with USER32/KERNEL32, so HGLOBAL
 * values and the pointer returned by GlobalLock fit guest registers.  Explicit
 * console automation instead gives the untouched getter one process-private
 * HGLOBAL supplied over WM_COPYDATA; only that armed transaction is intercepted
 * and the real system clipboard is never opened.  Keep the complete eight-
 * function family together and preserve the original x86 stdcall cleanup. */
static void host_GlobalAlloc(CPU *__restrict c)
{
    HGLOBAL result = GlobalAlloc(host_arg(c, 0), (SIZE_T)host_arg(c, 1));
    c->eax = (uint32_t)(uintptr_t)result;
    host_stdcall_return(c, 8U);
}

static void host_GlobalLock(CPU *__restrict c)
{
    uintptr_t handle = (uintptr_t)host_arg(c, 0);
    uintptr_t result;
    if (!kage_pc_backend_virtual_global_lock(handle, &result))
        result = (uintptr_t)GlobalLock((HGLOBAL)handle);
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 4U);
}

static void host_GlobalUnlock(CPU *__restrict c)
{
    uintptr_t handle = (uintptr_t)host_arg(c, 0);
    uint32_t result;
    if (!kage_pc_backend_virtual_global_unlock(handle, &result))
        result = (uint32_t)GlobalUnlock((HGLOBAL)handle);
    c->eax = result;
    host_stdcall_return(c, 4U);
}

static void host_OpenClipboard(CPU *__restrict c)
{
    uintptr_t owner = (uintptr_t)host_arg(c, 0);
    uint32_t result;
    if (!kage_pc_backend_virtual_clipboard_open(owner, &result))
        result = (uint32_t)OpenClipboard((HWND)owner);
    c->eax = result;
    host_stdcall_return(c, 4U);
}

static void host_CloseClipboard(CPU *__restrict c)
{
    uint32_t result;
    if (!kage_pc_backend_virtual_clipboard_close(&result))
        result = (uint32_t)CloseClipboard();
    c->eax = result;
    host_stdcall_return(c, 0U);
}

static void host_EmptyClipboard(CPU *__restrict c)
{
    c->eax = (uint32_t)EmptyClipboard();
    host_stdcall_return(c, 0U);
}

static void host_GetClipboardData(CPU *__restrict c)
{
    uint32_t format = host_arg(c, 0);
    uintptr_t result;
    if (!kage_pc_backend_virtual_clipboard_get_data(format, &result))
        result = (uintptr_t)GetClipboardData(format);
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 4U);
}

static void host_SetClipboardData(CPU *__restrict c)
{
    HANDLE result = SetClipboardData(
        host_arg(c, 0), (HANDLE)(uintptr_t)host_arg(c, 1));
    c->eax = (uint32_t)(uintptr_t)result;
    host_stdcall_return(c, 8U);
}

static void host_SystemParametersInfoA(CPU *__restrict c)
{
    uint32_t action = host_arg(c, 0);
    uint32_t ui_param = host_arg(c, 1);
    uint32_t parameter = host_arg(c, 2);
    uint32_t update_flags = host_arg(c, 3);

    (void)ui_param;             /* documented zero for these two actions */
    (void)update_flags;         /* no host profile write or broadcast */
    if (action == SPI_GETFOREGROUNDLOCKTIMEOUT) {
        if (parameter) {
            st32(parameter, s_foreground_lock_timeout);
            c->eax = 1U;
        } else {
            c->eax = 0U;
        }
    } else if (action == SPI_SETFOREGROUNDLOCKTIMEOUT) {
        s_foreground_lock_timeout = parameter;
        c->eax = 1U;
    } else {
        guest_fault(c, action, "unsupported SystemParametersInfoA action");
        return;
    }
    host_stdcall_return(c, 16U);
}

/* The PE imports only COM apartment lifecycle (CoInitialize/Ex/Uninitialize),
 * no object creation or marshaling API.  Model that lifecycle locally so the
 * same code works on Vita, where COM does not exist.  S_OK on the first
 * balanced initialize and S_FALSE on nested calls match Win32's observable
 * contract for this single-thread runtime. */
static uint32_t s_com_init_count;

static uint32_t host_com_initialize_result(void)
{
    uint32_t result = s_com_init_count ? 1U : 0U; /* S_FALSE : S_OK */
    ++s_com_init_count;
    return result;
}

static void host_CoInitialize(CPU *__restrict c)
{
    (void)host_arg(c, 0);
    c->eax = host_com_initialize_result();
    host_stdcall_return(c, 4U);
}

static void host_CoInitializeEx(CPU *__restrict c)
{
    (void)host_arg(c, 0);
    (void)host_arg(c, 1);       /* apartment flags have no target-side effect */
    c->eax = host_com_initialize_result();
    host_stdcall_return(c, 8U);
}

static void host_CoUninitialize(CPU *__restrict c)
{
    if (s_com_init_count) --s_com_init_count;
    host_stdcall_return(c, 0U);
}

static void host_QueryPerformanceCounter(CPU *__restrict c)
{
    LARGE_INTEGER value;
    uint32_t out = host_arg(c, 0);
    BOOL result = QueryPerformanceCounter(&value);
    st32(out, (uint32_t)value.LowPart);
    st32(out + 4U, (uint32_t)value.HighPart);
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 4U);
}

static void host_IsProcessorFeaturePresent(CPU *__restrict c)
{
    uint32_t feature = host_arg(c, 0);
    /* Same virtual capability boundary as guest_cpuid.  A native-host query
     * would make startup choose different code on different development PCs. */
    c->eax = feature == PF_XMMI_INSTRUCTIONS_AVAILABLE ||
             feature == PF_XMMI64_INSTRUCTIONS_AVAILABLE;
    host_stdcall_return(c, 4U);
}

/* `BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *)` -- the companion
 * to QueryPerformanceCounter, copied from the installed SDK's profileapi.h.
 * Both APIs must describe the same native clock on the PC backend. */
static void host_QueryPerformanceFrequency(CPU *__restrict c)
{
    LARGE_INTEGER value;
    uint32_t out = host_arg(c, 0);
    BOOL result = QueryPerformanceFrequency(&value);
    st32(out, (uint32_t)value.LowPart);
    st32(out + 4U, (uint32_t)value.HighPart);
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 4U);
}

static void host_initterm_e(CPU *__restrict c)
{
    uint32_t current = host_arg(c, 0);
    uint32_t end = host_arg(c, 1);
    uint32_t result = 0;
    if (current > end || ((end - current) & 3U)) {
        guest_fault(c, current, "_initterm_e invalid guest range");
        return;
    }
    while (current < end) {
        uint32_t callback = ld32(current);
        current += 4U;
        if (!callback) continue;
        if (!call_guest_initializer(c, callback)) return;
        result = c->eax;
        if (result) break;
    }
    c->eax = result;
    host_cdecl_return(c);
}

static void host_initterm(CPU *__restrict c)
{
    uint32_t current = host_arg(c, 0);
    uint32_t end = host_arg(c, 1);
    if (current > end || ((end - current) & 3U)) {
        guest_fault(c, current, "_initterm invalid guest range");
        return;
    }
    while (current < end) {
        uint32_t callback = ld32(current);
        current += 4U;
        if (callback && !call_guest_initializer(c, callback)) return;
    }
    host_cdecl_return(c);
}

/* `void __cdecl _set_app_type(int)` -- an internal UCRT startup call that
 * records whether this is a console or a GUI process (`_crt_app_type`:
 * 0 unknown, 1 console, 2 GUI).  It only affects how the CRT later reports
 * fatal errors, so recording the value is a complete emulation for us; it is
 * kept observable rather than discarded because "the game asked for console"
 * is a fact worth being able to check later.
 *
 * The ABI was taken from the call site, not from the header.  MSVC merges
 * cdecl cleanup here:
 *
 *     005eb5f8  push 1          ; this argument
 *     005eb5fa  call 0x5ec1be   ; -> jmp [0x30606588] _set_app_type
 *     005eb604  push eax        ; second call's argument
 *     005eb616  push 1          ; third call's argument
 *     005eb61f  add  esp, 0xc   ; ONE cleanup for all three
 *
 * So the callee must not touch the argument.  A stdcall-style `ret 4` here
 * would make that single `add esp, 0xc` over-pop by four bytes and corrupt
 * the frame far away from the cause. */
unsigned g_guest_app_type;

static void host_set_app_type(CPU *__restrict c)
{
    g_guest_app_type = host_arg(c, 0);
    host_cdecl_return(c);
}

/* `errno_t __cdecl _set_fmode(int)` -- sets the default translation mode for
 * files opened without an explicit one.  The call site feeds it the constant
 * 0x4000 (`_O_TEXT`) produced by an already-translated helper:
 *
 *     005eb5ff  call 0x5ec07a   ; mov eax, 0x4000 ; ret
 *     005eb604  push eax
 *     005eb605  call 0x5ec1e2   ; -> _set_fmode
 *     005eb60a                  ; observed return address
 *
 * Recorded, not acted on: our file layer is not the UCRT one, and on the Vita
 * there is no text/binary distinction to honour.  Returns 0 (success) because
 * the UCRT contract is errno_t, even though this caller ignores it. */
unsigned g_guest_fmode;

static void host_set_fmode(CPU *__restrict c)
{
    g_guest_fmode = host_arg(c, 0);
    c->eax = 0U;
    host_cdecl_return(c);
}

/* `int __cdecl _set_new_mode(int)` -- declaration copied from the installed
 * UCRT new.h; return semantics verified against Microsoft Learn.  The mode
 * is CRT-owned state: 1 makes malloc consult the C++ new-handler on failure,
 * 0 is the default, and the function returns the previous value.  Our current
 * allocator layer has no guest new-handler yet, but preserving/querying the
 * state is still required by CRT startup.  An invalid value is kept loud
 * until the guest invalid-parameter/errno layer exists. */
static int s_guest_new_mode;

static void host_set_new_mode(CPU *__restrict c)
{
    uint32_t mode = host_arg(c, 0);
    int previous;
    if (mode > 1U) {
        guest_fault(c, mode, "_set_new_mode requires 0 or 1");
        return;
    }
    previous = s_guest_new_mode;
    s_guest_new_mode = (int)mode;
    c->eax = (uint32_t)previous;
    host_cdecl_return(c);
}

/* Core UCRT heap family.  These four functions must stay on one allocator:
 * memory returned by malloc/calloc/realloc is later passed to free, so mixing
 * a Win32 heap, mimalloc internals and the host CRT would be a silent ABI bug.
 * The PC harness is itself 32-bit MSVC/UCRT, hence its heap pointers fit the
 * guest registers and are directly dereferenceable in the identity-mapped
 * memory model.  Vita will replace this boundary with its bounded allocator,
 * while keeping the same cdecl guest stack contract. */
static void host_malloc(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)malloc((size_t)host_arg(c, 0));
    host_cdecl_return(c);
}

static void host_calloc(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)calloc((size_t)host_arg(c, 0),
                                        (size_t)host_arg(c, 1));
    host_cdecl_return(c);
}

static void host_realloc(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)realloc(
        (void *)(uintptr_t)host_arg(c, 0), (size_t)host_arg(c, 1));
    host_cdecl_return(c);
}

static void host_free(CPU *__restrict c)
{
    free((void *)(uintptr_t)host_arg(c, 0));
    host_cdecl_return(c);
}

/* A narrow CRT string is a guest-pointer boundary, even on the PC harness
 * where guest and host addresses currently have the same numeric value.
 * Copy through committed readable pages before entering the UCRT: a null,
 * unreadable, or unterminated guest value must become a controlled guest fault
 * rather than a native invalid-parameter abort/access violation.  The current
 * numeric inputs and filesystem paths are bounded well below 4096 bytes; a
 * longer value stays loud until a target path policy deliberately admits it. */
#define HOST_GUEST_C_STRING_CAP 4096U
static uint32_t s_guest_errno;

static int host_copy_guest_c_string(CPU *__restrict c, uint32_t source,
                                    char text[HOST_GUEST_C_STRING_CAP],
                                    const char *null_fault,
                                    const char *range_fault)
{
    uintptr_t readable_end = 0U;
    uint32_t i;

    if (!source) {
        guest_fault(c, source, null_fault);
        return 0;
    }
    for (i = 0U; i < HOST_GUEST_C_STRING_CAP; ++i) {
        uint32_t address;
        unsigned char byte;
        if (i > UINT32_MAX - source) {
            guest_fault(c, source, range_fault);
            return 0;
        }
        address = source + i;
        if ((uintptr_t)address >= readable_end) {
            MEMORY_BASIC_INFORMATION info;
            uintptr_t region_start, region_end;
            if (!VirtualQuery((const void *)(uintptr_t)address, &info,
                              sizeof info) || info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                guest_fault(c, address, range_fault);
                return 0;
            }
            region_start = (uintptr_t)info.BaseAddress;
            region_end = region_start + info.RegionSize;
            if ((uintptr_t)address < region_start || region_end <= address) {
                guest_fault(c, address, range_fault);
                return 0;
            }
            readable_end = region_end;
        }
        byte = *(const unsigned char *)(uintptr_t)address;
        text[i] = (char)byte;
        if (!byte) return 1;
    }
    guest_fault(c, source, range_fault);
    return 0;
}

/* `int __cdecl remove(const char *)` and
 * `int __cdecl _access(const char *, int)` are the PE's complete narrow UCRT
 * filesystem import pair.  Never pass the raw guest pointer to native CRT:
 * copy and validate it first so this remains a real guest/host boundary.
 * Native errno is private to the harness.  Preserve the guest errno cell on
 * success and mirror the UCRT error only for its documented -1 return. */
static void host_remove(CPU *__restrict c)
{
    char path[HOST_GUEST_C_STRING_CAP];
    int saved_errno, call_errno, result;

    if (!host_copy_guest_c_string(
            c, host_arg(c, 0), path,
            "remove received a null guest path",
            "remove path is unreadable or exceeds 4095 bytes"))
        return;
    saved_errno = errno;
    errno = 0;
    result = remove(path);
    call_errno = errno;
    errno = saved_errno;
    if (result == -1)
        s_guest_errno = (uint32_t)call_errno;
    c->eax = (uint32_t)result;
    host_cdecl_return(c);       /* pop RET only; caller owns the path argument */
}

static void host_access(CPU *__restrict c)
{
    char path[HOST_GUEST_C_STRING_CAP];
    int saved_errno, call_errno, result;

    if (!host_copy_guest_c_string(
            c, host_arg(c, 0), path,
            "_access received a null guest path",
            "_access path is unreadable or exceeds 4095 bytes"))
        return;
    saved_errno = errno;
    errno = 0;
    result = _access(path, (int)host_arg(c, 1));
    call_errno = errno;
    errno = saved_errno;
    if (result == -1)
        s_guest_errno = (uint32_t)call_errno;
    c->eax = (uint32_t)result;
    host_cdecl_return(c);       /* pop RET only; caller owns both arguments */
}

/* Installed-UCRT measurements in its C locale pin these policies:
 *
 *   atoi("2147483648")  -> INT_MAX, guest errno = ERANGE
 *   atoi("-2147483649") -> INT_MIN, guest errno = ERANGE
 *   atof("1e309")       -> +Inf, errno unchanged
 *
 * `_strtol_l` supplies the first contract without locale drift; `_atof_l`
 * is used rather than `_strtod_l` because UCRT atof deliberately preserves
 * errno on overflow/underflow.  Both receive a validated private copy, never
 * a raw guest pointer.  Their x86 declarations are cdecl: this shim pops only
 * the guest return address and leaves the one argument for the caller. */
static void host_atoi(CPU *__restrict c)
{
    char text[HOST_GUEST_C_STRING_CAP];
    _locale_t c_locale;
    long value;
    int saved_errno, conversion_errno;

    if (!host_copy_guest_c_string(
            c, host_arg(c, 0), text,
            "atoi received a null guest pointer",
            "atoi input is unreadable or exceeds 4095 bytes"))
        return;
    c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale) {
        guest_fault(c, 0U, "atoi could not create the C numeric locale");
        return;
    }
    saved_errno = errno;
    errno = 0;
    value = _strtol_l(text, NULL, 10, c_locale);
    conversion_errno = errno;
    errno = saved_errno;
    _free_locale(c_locale);
    if (conversion_errno == ERANGE) {
        s_guest_errno = ERANGE;
    } else if (conversion_errno != 0) {
        guest_fault(c, (uint32_t)conversion_errno,
                    "atoi produced an unexpected UCRT conversion error");
        return;
    }
    c->eax = (uint32_t)(int32_t)value;
    host_cdecl_return(c);
}

static void host_atof(CPU *__restrict c)
{
    char text[HOST_GUEST_C_STRING_CAP];
    _locale_t c_locale;
    double value;
    int saved_errno;

    if (!host_copy_guest_c_string(
            c, host_arg(c, 0), text,
            "atof received a null guest pointer",
            "atof input is unreadable or exceeds 4095 bytes"))
        return;
    c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale) {
        guest_fault(c, 0U, "atof could not create the C numeric locale");
        return;
    }
    saved_errno = errno;
    value = _atof_l(text, c_locale);
    errno = saved_errno;
    _free_locale(c_locale);
    fpush(c, value);             /* x86 double return channel is x87 ST(0) */
    host_cdecl_return(c);
}

/* UCRT `_strdup` is part of the same allocator boundary: its result is later
 * released through `free`.  Walk/copy guest bytes explicitly instead of
 * handing an ARM guest address to the target libc. */
static void host_strdup(CPU *__restrict c)
{
    uint32_t source = host_arg(c, 0);
    uint32_t length = 0U;
    char *copy;

    if (!source) {
        guest_fault(c, source, "_strdup received a null guest pointer");
        return;
    }
    while (ld8(source + length)) {
        if (length == UINT32_MAX - 1U) {
            guest_fault(c, source, "_strdup source is not terminated");
            return;
        }
        ++length;
    }
    copy = (char *)malloc((size_t)length + 1U);
    if (copy) {
        uint32_t i;
        for (i = 0U; i <= length; ++i)
            copy[i] = (char)ld8(source + i);
    }
    c->eax = (uint32_t)(uintptr_t)copy;
    host_cdecl_return(c);
}

/* The x86 UCRT math ABI passes a binary64 on the cdecl stack and returns its
 * binary64 result in x87 ST(0).  Do not put the result in EDX:EAX: all 478
 * physical callers consume ST(0), and several deliberately postpone the
 * caller-side `add esp,8` while they continue using that result. */
static void host_ceil(CPU *__restrict c)
{
    fpush(c, ceil(ldd(guest_stack_address(c, c->esp + 4U, 8U, 0U))));
    host_cdecl_return(c);
}

static void host_floor(CPU *__restrict c)
{
    fpush(c, floor(ldd(guest_stack_address(c, c->esp + 4U, 8U, 0U))));
    host_cdecl_return(c);
}

/* `corecrt_math.h` declares `_fdclass` as `short __cdecl _fdclass(float)`.
 * Its result is the C99 fpclassify code, not one of float.h's `_FPCLASS_*`
 * bit flags: subnormal=-2, normal=-1, zero=0, infinity=1 and NaN=2.
 *
 * Classify the binary32 representation as an integer.  In particular, do not
 * load a signaling NaN into the host FPU merely to inspect it, and do not let
 * the host CRT's exception/errno state leak into the guest.  All eight
 * physical callers consume the short result from AX.  Materialise the same
 * value sign-extended through EAX as well, so no stale high half can escape
 * into a generated caller that snapshots the complete return register. */
static void host_fdclass(CPU *__restrict c)
{
    uint32_t bits = host_arg(c, 0);
    uint32_t magnitude = bits & 0x7fffffffU;
    int16_t classification;

    if (magnitude > 0x7f800000U)
        classification = 2;       /* FP_NAN */
    else if (magnitude == 0x7f800000U)
        classification = 1;       /* FP_INFINITE */
    else if (magnitude >= 0x00800000U)
        classification = -1;      /* FP_NORMAL */
    else if (magnitude != 0U)
        classification = -2;      /* FP_SUBNORMAL */
    else
        classification = 0;       /* FP_ZERO */

    c->eax = (uint32_t)(int32_t)classification;
    host_cdecl_return(c);          /* pop RET only; caller removes float */
}

typedef double (__cdecl *host_binary_double_fn)(double, double);

/* MSVC's x86 `_CI*` binary helpers use the compiler's private x87 ABI, not
 * ordinary cdecl stack arguments: entry ST(0) is x, entry ST(1) is y, and
 * the result replaces both at ST(0).  The measured wrappers load y first and
 * x second, so the native C operation must receive (y, x). */
static void host_ci_binary_x87(CPU *__restrict c,
                               host_binary_double_fn operation)
{
    int saved_errno = errno;
    int math_errno;
    double x = fpop(c);
    double y = fpop(c);
    double result;

    errno = 0;
    result = operation(y, x);
    math_errno = errno;
    errno = saved_errno;
    if (math_errno != 0)
        s_guest_errno = (uint32_t)math_errno;
    fpush(c, result);
    host_cdecl_return(c);       /* x87 register ABI: pop RET, no arguments */
}

static void host_CIatan2(CPU *__restrict c)
{
    host_ci_binary_x87(c, atan2);
}

static void host_CIfmod(CPU *__restrict c)
{
    host_ci_binary_x87(c, fmod);
}

/* MSVC's x86 `/fp:precise` helpers are not ordinary cdecl libm calls.  Unary
 * helpers receive one binary64 value in XMM0; pow receives its base in XMM0
 * and exponent in XMM1.  All return through the low binary64 lane of XMM0
 * with no argument on the guest stack.  Exact wrappers and raw call censuses
 * below pin that register ABI against this input PE.
 *
 * Use the native C libm only as the semantic backend.  Explicitly bridge its
 * host ABI to the guest XMM lane, keep native errno private, and update guest
 * errno only when UCRT reports a domain/range error.  Writing d[0] alone also
 * leaves the undefined upper return lane and the other guest XMM registers
 * undisturbed, which is deterministic and no stronger than the caller-save
 * contract requires. */
typedef double (__cdecl *host_unary_double_fn)(double);

static void host_libm_sse2_unary_precise(CPU *__restrict c,
                                         host_unary_double_fn operation)
{
    int saved_errno = errno;
    int math_errno;
    double result;

    errno = 0;
    result = operation(c->x[0].d[0]);
    math_errno = errno;
    errno = saved_errno;
    c->x[0].d[0] = result;
    if (math_errno != 0)
        s_guest_errno = (uint32_t)math_errno;
    host_cdecl_return(c);       /* register ABI: pop RET, no stack arguments */
}

static void host_libm_sse2_acos_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, acos);
}

static void host_libm_sse2_asin_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, asin);
}

static void host_libm_sse2_atan_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, atan);
}

static void host_libm_sse2_cos_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, cos);
}

static void host_libm_sse2_exp_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, exp);
}

static void host_libm_sse2_log10_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, log10);
}

static void host_libm_sse2_log_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, log);
}

static void host_libm_sse2_sin_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, sin);
}

static void host_libm_sse2_sqrt_precise(CPU *__restrict c)
{
    host_libm_sse2_unary_precise(c, sqrt);
}

static void host_libm_sse2_binary_precise(CPU *__restrict c,
                                          host_binary_double_fn operation)
{
    int saved_errno = errno;
    int math_errno;
    double result;

    errno = 0;
    result = operation(c->x[0].d[0], c->x[1].d[0]);
    math_errno = errno;
    errno = saved_errno;
    c->x[0].d[0] = result;
    if (math_errno != 0)
        s_guest_errno = (uint32_t)math_errno;
    host_cdecl_return(c);       /* register ABI: pop RET, no stack arguments */
}

static void host_libm_sse2_pow_precise(CPU *__restrict c)
{
    host_libm_sse2_binary_precise(c, pow);
}

/* The only measured nextafterf call passes two binary32 values on the cdecl
 * stack and consumes a binary32 result from x87 ST(0).  UCRT reports both
 * underflow into/out of the subnormal range and overflow through errno; keep
 * native errno private and mirror only that documented guest-visible error. */
static void host_nextafterf(CPU *__restrict c)
{
    int saved_errno = errno;
    int math_errno;
    float result;

    errno = 0;
    result = nextafterf(
        ldf(guest_stack_address(c, c->esp + 4U, 4U, 0U)),
        ldf(guest_stack_address(c, c->esp + 8U, 4U, 0U)));
    math_errno = errno;
    errno = saved_errno;
    if (math_errno == ERANGE) {
        s_guest_errno = ERANGE;
    } else if (math_errno != 0) {
        guest_fault(c, (uint32_t)math_errno,
                    "nextafterf produced an unexpected UCRT math error");
        return;
    }
    fpush(c, (double)result);
    host_cdecl_return(c);
}

/* Steam-disabled lifecycle.  The Vita has no Steam client, and the PC
 * bring-up must take the same explicit path rather than accidentally binding
 * a developer machine's steam_api.dll.  Register/Unregister are cdecl by the
 * measured constructor call at 00001de0 (its later add esp,0xc cleans both
 * callback arguments plus the following CRT argument).  Init returns false;
 * callbacks and shutdown are inert.  Interface creation exports remain loud:
 * reaching one after a failed Init would be a real missing platform policy,
 * not something these lifecycle no-ops should conceal.
 *
 * SteamInternal_ContextInit is different: Steam's own x86 implementation
 * always returns descriptor+8, even when the cached generation is current and
 * the interface-populating callback is not called.  This EXE has one exact
 * descriptor at 307aa3c8: callback 300189a0, generation zero, followed by a
 * 21-dword CSteamAPIContext.  Its callback clears the first 20 slots and its
 * success path also owns the final slot at +50 before returning.  It then
 * only populates them after obtaining a Steam pipe.  With SteamAPI_Init=false
 * the faithful target result is therefore the existing all-zero context.
 * Keep the descriptor and every slot pinned so a future real Steam use faults
 * here instead of becoming a later NULL-vtable mystery. */
#define GUEST_STEAM_CONTEXT_DESCRIPTOR 0x307aa3c8U
#define GUEST_STEAM_CONTEXT_CALLBACK   0x300189a0U
#define GUEST_STEAM_CONTEXT_DWORDS     21U

static void host_SteamInternal_ContextInit(CPU *__restrict c)
{
    uint32_t descriptor = host_arg(c, 0);
    uint32_t i;

    if (descriptor != GUEST_STEAM_CONTEXT_DESCRIPTOR ||
        ld32(descriptor) != GUEST_STEAM_CONTEXT_CALLBACK ||
        ld32(descriptor + 4U) != 0U) {
        guest_fault(c, descriptor,
                    "SteamInternal_ContextInit unknown descriptor state");
        return;
    }
    for (i = 0; i < GUEST_STEAM_CONTEXT_DWORDS; ++i) {
        uint32_t slot = descriptor + 8U + i * 4U;
        if (ld32(slot) != 0U) {
            guest_fault(c, slot,
                        "Steam-disabled context unexpectedly initialized");
            return;
        }
    }
    c->eax = descriptor + 8U;
    host_cdecl_return(c);
}
static void host_SteamAPI_RegisterCallback(CPU *__restrict c)
{
    (void)host_arg(c, 0);
    (void)host_arg(c, 1);
    host_cdecl_return(c);
}

static void host_SteamAPI_UnregisterCallback(CPU *__restrict c)
{
    (void)host_arg(c, 0);
    host_cdecl_return(c);
}

/* SteamAPICall_t is uint64 on x86 too, so both call-result helpers receive
 * three guest dwords: CCallbackBase*, handle low, handle high.  Steam is
 * deliberately disabled above (`SteamAPI_Init` returns false), therefore
 * retaining these registrations would promise callbacks that can never be
 * delivered.  Consume the exact cdecl ABI and keep the operation inert; do
 * not manufacture a successful Steam context or interface. */
static void host_SteamAPI_RegisterCallResult(CPU *__restrict c)
{
    (void)host_arg(c, 0);
    (void)host_arg(c, 1);
    (void)host_arg(c, 2);
    host_cdecl_return(c);
}

static void host_SteamAPI_UnregisterCallResult(CPU *__restrict c)
{
    (void)host_arg(c, 0);
    (void)host_arg(c, 1);
    (void)host_arg(c, 2);
    host_cdecl_return(c);
}

static void host_SteamAPI_Init(CPU *__restrict c)
{
    c->eax = 0U;
    host_cdecl_return(c);
}

static void host_SteamAPI_RunCallbacks(CPU *__restrict c)
{
    host_cdecl_return(c);
}

static void host_SteamAPI_Shutdown(CPU *__restrict c)
{
    host_cdecl_return(c);
}

/* ---- CRT globals the guest image does not own ---------------------------
 * `__p__commode` and friends return a POINTER into the UCRT's own data, and
 * the caller writes through it:
 *
 *     005eb60a  call 0x542a20      ; guest code, result in eax
 *     005eb60f  mov  esi, eax
 *     005eb611  call 0x5ec20c      ; -> __p__commode
 *     005eb616                     ; observed return address
 *     005eb618  mov  dword ptr [eax], esi
 *
 * So a host-side value is not enough; we must hand back an address the guest
 * can store to.  These variables are not in the PE -- they belong to the CRT
 * DLL, which we are replacing -- so host storage is the correct home for
 * them, not a carved-out corner of the guest image.
 *
 * This works because `guest.h` defines guest address == host address and the
 * process is 32-bit, so the address of a static object is itself a valid
 * guest pointer.  That equality is the one assumption here; if the Vita
 * backend ever moves to an offset-based memory model (an earlier design
 * used `m_memory + address`) these need to move into guest space and
 * this comment is the reason why. */
static uint32_t s_crt_commode;
static uint32_t s_guest_tm[9];

static uint32_t host_guest_ptr(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    return (uint32_t)a;
}

static void host_p_commode(CPU *__restrict c)
{
    c->eax = host_guest_ptr(&s_crt_commode);
    host_cdecl_return(c);
}

/* `_errno` returns a writable pointer, while `_set_errno` updates the same
 * guest-visible cell.  The PC bring-up currently has one guest thread, so one
 * stable cell matches every reached caller; move it into per-thread guest CRT
 * state when translated guest threads become runnable. */
static void host_errno(CPU *__restrict c)
{
    c->eax = host_guest_ptr(&s_guest_errno);
    host_cdecl_return(c);
}

static void host_set_errno(CPU *__restrict c)
{
    s_guest_errno = host_arg(c, 0);
    c->eax = 0U;
    host_cdecl_return(c);
}

/* Microsoft x86 `struct tm` is nine consecutive 32-bit ints in this order.
 * Decode it field-by-field: handing a guest layout to the target libc would
 * silently become wrong on a libc that appends timezone fields. */
static void host_load_tm(uint32_t address, struct tm *value)
{
    memset(value, 0, sizeof *value);
    value->tm_sec   = (int32_t)ld32(address + 0U);
    value->tm_min   = (int32_t)ld32(address + 4U);
    value->tm_hour  = (int32_t)ld32(address + 8U);
    value->tm_mday  = (int32_t)ld32(address + 12U);
    value->tm_mon   = (int32_t)ld32(address + 16U);
    value->tm_year  = (int32_t)ld32(address + 20U);
    value->tm_wday  = (int32_t)ld32(address + 24U);
    value->tm_yday  = (int32_t)ld32(address + 28U);
    value->tm_isdst = (int32_t)ld32(address + 32U);
}

static void host_store_tm(uint32_t address, const struct tm *value)
{
    st32(address + 0U,  (uint32_t)value->tm_sec);
    st32(address + 4U,  (uint32_t)value->tm_min);
    st32(address + 8U,  (uint32_t)value->tm_hour);
    st32(address + 12U, (uint32_t)value->tm_mday);
    st32(address + 16U, (uint32_t)value->tm_mon);
    st32(address + 20U, (uint32_t)value->tm_year);
    st32(address + 24U, (uint32_t)value->tm_wday);
    st32(address + 28U, (uint32_t)value->tm_yday);
    st32(address + 32U, (uint32_t)value->tm_isdst);
}

static __time64_t host_load_time64(uint32_t address)
{
    uint64_t bits = (uint64_t)ld32(address) |
                    ((uint64_t)ld32(address + 4U) << 32U);
    return (__time64_t)(int64_t)bits;
}

static void host_return_time64(CPU *__restrict c, __time64_t value)
{
    uint64_t bits = (uint64_t)(int64_t)value;
    c->eax = (uint32_t)bits;
    c->edx = (uint32_t)(bits >> 32U);
}

/* `__time64_t __cdecl _time64(__time64_t *)`: x86 returns the 64-bit value
 * in EDX:EAX and optionally writes the identical eight bytes through the
 * guest pointer. */
static void host_time64(CPU *__restrict c)
{
    uint32_t output = host_arg(c, 0);
    __time64_t value;

    errno = 0;
    value = _time64(NULL);
    if (value == (__time64_t)-1)
        s_guest_errno = errno ? (uint32_t)errno : EINVAL;
    if (output) {
        uint64_t bits = (uint64_t)(int64_t)value;
        st32(output, (uint32_t)bits);
        st32(output + 4U, (uint32_t)(bits >> 32U));
    }
    host_return_time64(c, value);
    host_cdecl_return(c);
}

/* `_gmtime64` and `_localtime64` return a pointer to shared CRT-owned `tm`
 * storage.  Mirror that contract with one stable guest-visible nine-int cell;
 * callers in the image copy all 36 bytes before making another time call. */
static void host_gmtime64(CPU *__restrict c)
{
    uint32_t source = host_arg(c, 0);
    struct tm value;
    errno_t error;

    if (!source) {
        s_guest_errno = EINVAL;
        c->eax = 0U;
        host_cdecl_return(c);
        return;
    }
    {
        __time64_t time_value = host_load_time64(source);
        error = _gmtime64_s(&value, &time_value);
    }
    if (error) {
        s_guest_errno = (uint32_t)error;
        c->eax = 0U;
    } else {
        host_store_tm(host_guest_ptr(s_guest_tm), &value);
        c->eax = host_guest_ptr(s_guest_tm);
    }
    host_cdecl_return(c);
}

static void host_localtime64(CPU *__restrict c)
{
    uint32_t source = host_arg(c, 0);
    struct tm value;
    errno_t error;

    if (!source) {
        s_guest_errno = EINVAL;
        c->eax = 0U;
        host_cdecl_return(c);
        return;
    }
    {
        __time64_t time_value = host_load_time64(source);
        error = _localtime64_s(&value, &time_value);
    }
    if (error) {
        s_guest_errno = (uint32_t)error;
        c->eax = 0U;
    } else {
        host_store_tm(host_guest_ptr(s_guest_tm), &value);
        c->eax = host_guest_ptr(s_guest_tm);
    }
    host_cdecl_return(c);
}

/* `_mkgmtime64` interprets the nine guest fields as UTC, normalises them in
 * place, and returns seconds in EDX:EAX. */
static void host_mkgmtime64(CPU *__restrict c)
{
    uint32_t source = host_arg(c, 0);
    __time64_t result = (__time64_t)-1;

    if (!source) {
        s_guest_errno = EINVAL;
    } else {
        struct tm value;
        host_load_tm(source, &value);
        errno = 0;
        result = _mkgmtime64(&value);
        if (result == (__time64_t)-1)
            s_guest_errno = errno ? (uint32_t)errno : EINVAL;
        host_store_tm(source, &value);
    }
    host_return_time64(c, result);
    host_cdecl_return(c);
}

/* The guest never changes its C locale.  Use an explicit C `LC_TIME` locale
 * so month/day names cannot depend on the PC harness user's locale. */
static void host_strftime(CPU *__restrict c)
{
    uint32_t destination = host_arg(c, 0);
    uint32_t capacity = host_arg(c, 1);
    uint32_t format = host_arg(c, 2);
    uint32_t time_value = host_arg(c, 3);
    struct tm value;
    _locale_t c_locale;
    size_t result;

    if (!destination || !capacity || !format || !time_value) {
        if (!destination || !format || !time_value)
            s_guest_errno = EINVAL;
        c->eax = 0U;
        host_cdecl_return(c);
        return;
    }
    host_load_tm(time_value, &value);
    c_locale = _create_locale(LC_TIME, "C");
    if (!c_locale) {
        s_guest_errno = ENOMEM;
        c->eax = 0U;
        host_cdecl_return(c);
        return;
    }
    result = _strftime_l((char *)(uintptr_t)destination, (size_t)capacity,
                         (const char *)(uintptr_t)format, &value, c_locale);
    _free_locale(c_locale);
    c->eax = (uint32_t)result;
    host_cdecl_return(c);
}

/* `int __cdecl _crt_atexit(_PVFV)` -- registers a guest callback to run at
 * process exit; returns 0 on success. The table is consumed in LIFO order by
 * host_exit below. It is bounded, and overflowing it faults loudly rather than
 * wrapping, because "some handlers vanished" is the kind of failure that
 * surfaces somewhere else entirely. */
#define HOST_ATEXIT_MAX 256
static uint32_t s_atexit[HOST_ATEXIT_MAX];
static uint32_t s_tls_atexit;
static int      s_exit_running;
unsigned g_guest_atexit_count;

static void host_crt_atexit(CPU *__restrict c)
{
    uint32_t fn = host_arg(c, 0);
    if (g_guest_atexit_count >= HOST_ATEXIT_MAX) {
        guest_fault(c, fn, "_crt_atexit table full");
        return;
    }
    s_atexit[g_guest_atexit_count++] = fn;
    c->eax = 0U;
    host_cdecl_return(c);
}

/* UCRT `exit`, copied from the installed 10.0.26100 exit.cpp rather than
 * inferred from the current failure path: dynamic TLS destructors run first,
 * then `_crt_atexit` callbacks in LIFO order, then process termination.  The
 * host harness must survive that last step, so guest_exit performs a distinct
 * per-CPU guarded stop instead of calling native ExitProcess/exit.
 *
 * Mark an atexit slot consumed before calling it.  A callback may register a
 * new callback; the while loop then observes and runs that new tail exactly as
 * the UCRT table walker does after its begin/end pointers change. */
static int host_run_exit_cleanup(CPU *__restrict c)
{
    uint32_t fn;
    if (s_exit_running) {
        guest_fault(c, 0U, "recursive guest exit cleanup");
        return 0;
    }
    s_exit_running = 1;

    fn = s_tls_atexit;
    s_tls_atexit = 0U;
    if (fn && !call_guest_stdcall3(c, fn, 0U, 0U, 0U)) {
        s_exit_running = 0;
        return 0;
    }

    while (g_guest_atexit_count) {
        fn = s_atexit[--g_guest_atexit_count];
        s_atexit[g_guest_atexit_count] = 0U;
        if (fn && !call_guest_initializer(c, fn)) {
            s_exit_running = 0;
            return 0;
        }
    }
    s_exit_running = 0;
    return 1;
}

static void host_exit(CPU *__restrict c)
{
    int32_t code = (int32_t)host_arg(c, 0);
    if (!host_run_exit_cleanup(c)) return;
    guest_exit(c, code, "exit");
}

static void host__exit(CPU *__restrict c)
{
    guest_exit(c, (int32_t)host_arg(c, 0), "_exit");
}

/* `errno_t __cdecl _configure_narrow_argv(_crt_argv_mode)` -- returns 0 on
 * success.  We report success without building anything: argv is handed over
 * separately through `__p___argv` / `__p___argc`, and the Vita has no command
 * line to expand.  Mode recorded so the choice stays checkable.
 *
 * The mode values are copied from
 * `VC\Tools\MSVC\14.44.35207\include\vcruntime_startup.h`, not recalled:
 *
 *     _crt_argv_no_arguments        = 0
 *     _crt_argv_unexpanded_arguments = 1
 *     _crt_argv_expanded_arguments   = 2
 *
 * and `argv_mode.cpp` in the same toolchain returns `unexpanded` by default,
 * which is what this binary passes.  An earlier version of this comment had 1
 * and 2 swapped; a second review caught it.
 *
 * It is cdecl, but do NOT read the two `pop ecx` after the call as this
 * function's cleanup: only the first belongs to it, the second removes the
 * argument of the earlier `call 0x5eb079`.  MSVC defers cleanup, so pairing
 * pops with calls by adjacency is wrong here. */
unsigned g_guest_argv_mode;

static void host_configure_narrow_argv(CPU *__restrict c)
{
    g_guest_argv_mode = host_arg(c, 0);
    c->eax = 0U;
    host_cdecl_return(c);
}

/* `VOID WINAPI InitializeSListHead(PSLIST_HEADER)` -- note WINAPI: this one is
 * **stdcall**, so the callee removes the argument, unlike every CRT entry
 * above it.  Getting that backwards costs four bytes of guest stack per call
 * and the damage surfaces far from here.
 *
 * The real API is called rather than reimplemented: the guest pointer is a
 * host pointer in this backend, and an SLIST_HEADER is 8 bytes on x86 with an
 * 8-byte alignment requirement that a hand-written memset would have to honour
 * anyway.  The Vita backend has no interlocked singly-linked lists and will
 * need its own zeroing version. */
static void host_InitializeSListHead(CPU *__restrict c)
{
    InitializeSListHead((PSLIST_HEADER)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

/* `errno_t __cdecl _controlfp_s(unsigned *cur, unsigned new, unsigned mask)`
 * -- the FP control word.  The one call site asks for exactly one thing:
 *
 *     005ec080  push 0x30000   ; mask = _MCW_PC   (precision control)
 *     005ec085  push 0x10000   ; new  = _PC_53    (53-bit mantissa)
 *     005ec08a  push 0         ; cur  = NULL
 *     005ec08c  call 0x5ec212
 *     005ec091  add  esp, 0xc  ; cdecl, three arguments
 *     005ec096  jne  0x5ec099  ; a non-zero return goes to an abort path
 *
 * The request is already satisfied and cannot be honoured any other way: our
 * x87 is not an x87.  The emitter implements the FPU stack as C `double`, so
 * arithmetic runs on SSE2 with a 53-bit mantissa by definition, and the host
 * x87 precision field controls nothing we use.  Calling the real
 * `_controlfp_s` would mutate host FPU state for no effect on the guest.
 *
 * The same answer holds on the Vita for the same reason, so this is not a PC
 * shortcut that will need revisiting -- unlike `guest_cpuid`, which really
 * does report the host today.
 *
 * The word is still tracked so a later query returns what was set, and `cur`
 * is honoured even though this caller passes NULL. */
static uint32_t s_crt_fp_control = 0x00010000u;   /* _PC_53 */

static void host_controlfp_s(CPU *__restrict c)
{
    uint32_t out = host_arg(c, 0);
    uint32_t value = host_arg(c, 1);
    uint32_t mask = host_arg(c, 2);
    s_crt_fp_control = (s_crt_fp_control & ~mask) | (value & mask);
    if (out) st32(out, s_crt_fp_control);
    c->eax = 0U;                                  /* errno_t success */
    host_cdecl_return(c);
}

/* ---- the rest of the startup worklist -----------------------------------
 * `recomp/diag_startup.py` resolves every `jmp dword ptr [IAT]` thunk the CRT
 * startup neighbourhood can reach, so these were written from a measured list
 * rather than one build per name.  The run is still the arbiter: a static walk
 * cannot know which branches are taken.
 *
 * ABI reminder, because the two conventions are mixed here and getting it
 * wrong corrupts the frame far from the call: everything from KERNEL32 is
 * WINAPI/stdcall and pops its own arguments; everything from the CRT and
 * VCRUNTIME is cdecl and must not. */

/* argc/argv/environ live in host storage for the same reason as _commode:
 * they belong to the CRT we are replacing, not to the PE.  Guest address ==
 * host address in this backend, so the address of a static is a usable guest
 * pointer. */
static char     s_argv0[] = "isaac-ng.exe";
static uint32_t s_argv_vec[2];
static uint32_t s_argv_ptr;
static uint32_t s_env_vec[1];
static uint32_t s_env_ptr;
static int32_t  s_argc = 1;
static int      s_startup_ready;

static void host_startup_init(void)
{
    if (s_startup_ready) return;
    s_startup_ready = 1;
    s_argv_vec[0] = host_guest_ptr(s_argv0);
    s_argv_vec[1] = 0U;
    s_argv_ptr = host_guest_ptr(s_argv_vec);
    s_env_vec[0] = 0U;
    s_env_ptr = host_guest_ptr(s_env_vec);
}

/* `int __cdecl _configthreadlocale(int)` -- 0 disables per-thread locale, 1
 * enables it, -1 queries.  Returns the PREVIOUS setting.  We stay on the
 * process-wide locale: there is one locale on the target and no reason to
 * pretend otherwise. */
static int32_t s_thread_locale;

static void host_configthreadlocale(CPU *__restrict c)
{
    int32_t want = (int32_t)host_arg(c, 0);
    int32_t previous = s_thread_locale;
    if (want == 0 || want == 1) s_thread_locale = want;
    c->eax = (uint32_t)previous;
    host_cdecl_return(c);
}

/* `void *__cdecl memset(void *, int, size_t)` -- the guest pointer is a host
 * pointer here, so the real one is correct and fast.  Returns its first
 * argument. */
static void host_memset(CPU *__restrict c)
{
    uint32_t dst = host_arg(c, 0);
    int value = (int)host_arg(c, 1);
    uint32_t n = host_arg(c, 2);
    if (n) memset((void *)(uintptr_t)dst, value, (size_t)n);
    c->eax = dst;
    host_cdecl_return(c);
}

/* `memcpy` and `memmove` share the VCRUNTIME cdecl boundary but not the
 * overlap contract.  Keep the real operations distinct; replacing memmove
 * with memcpy happens to pass non-overlap startup and corrupts containers
 * later.  Both return the original destination. */
static void host_memcpy(CPU *__restrict c)
{
    uint32_t dst = host_arg(c, 0);
    uint32_t src = host_arg(c, 1);
    uint32_t n = host_arg(c, 2);
    if (n) memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src,
                  (size_t)n);
    c->eax = dst;
    host_cdecl_return(c);
}

static void host_memmove(CPU *__restrict c)
{
    uint32_t dst = host_arg(c, 0);
    uint32_t src = host_arg(c, 1);
    uint32_t n = host_arg(c, 2);
    if (n) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src,
                   (size_t)n);
    c->eax = dst;
    host_cdecl_return(c);
}

/* MSVC x86 `void *__cdecl __RTDynamicCast(void *inptr, long vf_delta,
 * TypeDescriptor *source, TypeDescriptor *target, int is_reference)`.
 *
 * The PE's RTTI is ordinary absolute 32-bit metadata after relocation.  The
 * the PE has 662 physical call sites; the currently emitted CFG reaches 585
 * unique sites (25 source/target pairs),
 * and every target hierarchy is single-inheritance.  Implement that complete
 * measured surface here instead of forwarding to the host VCRUNTIME, which
 * would make the boundary unusable on Vita.  Multiple/virtual inheritance is
 * deliberately loud until a reached call proves that larger algorithm is
 * needed. */
enum {
    GUEST_RTTI_MAX_BASES = 512U,
    GUEST_RTTI_MAX_NAME = 4096U,
    GUEST_RTTI_CHD_MULTINH = 0x01U,
    GUEST_RTTI_CHD_VIRTINH = 0x02U,
    GUEST_RTTI_CHD_AMBIGUOUS = 0x04U,
    GUEST_RTTI_BCD_PRIVORPROTBASE = 0x04U,
    GUEST_RTTI_BCD_KNOWN = 0x7fU
};

typedef struct guest_rtti_bcd {
    uint32_t type_descriptor;
    uint32_t contained_bases;
    int32_t mdisp;
    int32_t pdisp;
    int32_t vdisp;
    uint32_t attributes;
} guest_rtti_bcd;

static int guest_rtti_image_range(CPU *__restrict c, uint32_t address,
                                  uint32_t size, const char *what)
{
    if (!guest_image_contains(address, size)) {
        guest_fault(c, address, what);
        return 0;
    }
    return 1;
}

static int guest_rtti_read_bcd(CPU *__restrict c, uint32_t address,
                               uint32_t hierarchy_count,
                               guest_rtti_bcd *out)
{
    if (!guest_rtti_image_range(c, address, 24U,
                                "__RTDynamicCast BCD leaves guest image"))
        return 0;
    out->type_descriptor = ld32(address);
    out->contained_bases = ld32(address + 4U);
    out->mdisp = (int32_t)ld32(address + 8U);
    out->pdisp = (int32_t)ld32(address + 12U);
    out->vdisp = (int32_t)ld32(address + 16U);
    out->attributes = ld32(address + 20U);
    if (!guest_image_contains(out->type_descriptor, 9U) ||
        out->contained_bases >= hierarchy_count ||
        out->mdisp < 0 || out->pdisp != -1 || out->vdisp < 0 ||
        (out->attributes & ~GUEST_RTTI_BCD_KNOWN) != 0U) {
        guest_fault(c, address, "__RTDynamicCast invalid SI base descriptor");
        return 0;
    }
    return 1;
}

static int guest_rtti_names_equal(CPU *__restrict c, uint32_t left,
                                  uint32_t right, int *ok)
{
    uint32_t index;
    for (index = 0U; index < GUEST_RTTI_MAX_NAME; ++index) {
        uint32_t left_at = left + 8U + index;
        uint32_t right_at = right + 8U + index;
        uint8_t a, b;
        if (!guest_image_contains(left_at, 1U) ||
            !guest_image_contains(right_at, 1U)) {
            guest_fault(c, !guest_image_contains(left_at, 1U)
                             ? left_at : right_at,
                        "__RTDynamicCast type name leaves guest image");
            *ok = 0;
            return 0;
        }
        a = ld8(left_at);
        b = ld8(right_at);
        if (a != b) return 0;
        if (!a) return 1;
    }
    guest_fault(c, left, "__RTDynamicCast unterminated type name");
    *ok = 0;
    return 0;
}

static void host_RTDynamicCast(CPU *__restrict c)
{
    uint32_t input = host_arg(c, 0);
    uint32_t vf_delta = host_arg(c, 1);
    uint32_t source_type = host_arg(c, 2);
    uint32_t target_type = host_arg(c, 3);
    uint32_t is_reference = host_arg(c, 4);
    uint32_t vfptr, locator, hierarchy, base_array, base_count;
    uint32_t locator_offset, construction_offset, complete;
    uint32_t pass, i, j, target_address = 0U;
    int64_t complete_calc;
    guest_rtti_bcd target_bcd;
    int match_ok = 1;

    (void)vf_delta; /* MSVC's SI path does not use the vfptr displacement. */
    if (!input) {
        c->eax = 0U;
        host_cdecl_return(c);
        return;
    }

    /* The object pointer itself may belong to the guest heap.  Once its
     * compiler-written vfptr is loaded, every RTTI pointer must stay inside
     * the mapped PE image. */
    vfptr = ld32(input);
    if (vfptr < 4U ||
        !guest_rtti_image_range(c, vfptr - 4U, 4U,
                                "__RTDynamicCast vfptr leaves guest image"))
        return;
    locator = ld32(vfptr - 4U);
    if (!guest_rtti_image_range(c, locator, 20U,
                                "__RTDynamicCast COL leaves guest image"))
        return;
    locator_offset = ld32(locator + 4U);
    construction_offset = ld32(locator + 8U);
    if (ld32(locator) != 0U || locator_offset > 0x100000U ||
        construction_offset > 0x100000U || (locator_offset & 3U) != 0U ||
        (construction_offset & 3U) != 0U) {
        guest_fault(c, locator, "__RTDynamicCast invalid x86 COL");
        return;
    }
    complete_calc = (int64_t)(uint64_t)input - (int64_t)locator_offset;
    if (construction_offset) {
        if (input < construction_offset) {
            guest_fault(c, input, "__RTDynamicCast construction offset wraps");
            return;
        }
        complete_calc -= (int32_t)ld32(input - construction_offset);
    }
    if (complete_calc < 0 || complete_calc > UINT32_MAX) {
        guest_fault(c, input, "__RTDynamicCast complete object wraps");
        return;
    }
    complete = (uint32_t)complete_calc;

    hierarchy = ld32(locator + 16U);
    if (!guest_rtti_image_range(c, hierarchy, 16U,
                                "__RTDynamicCast CHD leaves guest image"))
        return;
    if (ld32(hierarchy) != 0U ||
        (ld32(hierarchy + 4U) &
         ~(GUEST_RTTI_CHD_MULTINH | GUEST_RTTI_CHD_VIRTINH |
           GUEST_RTTI_CHD_AMBIGUOUS)) != 0U) {
        guest_fault(c, hierarchy, "__RTDynamicCast invalid class hierarchy");
        return;
    }
    if (ld32(hierarchy + 4U) != 0U) {
        guest_fault(c, hierarchy,
                    "__RTDynamicCast MI/VI hierarchy is not implemented");
        return;
    }
    base_count = ld32(hierarchy + 8U);
    base_array = ld32(hierarchy + 12U);
    if (!base_count || base_count > GUEST_RTTI_MAX_BASES ||
        !guest_rtti_image_range(c, base_array, base_count * 4U,
                                "__RTDynamicCast base array leaves guest image"))
        return;
    if (!guest_rtti_image_range(c, source_type, 9U,
                                "__RTDynamicCast source type leaves guest image") ||
        !guest_rtti_image_range(c, target_type, 9U,
                                "__RTDynamicCast target type leaves guest image"))
        return;

    /* Match pointer identity first across the whole hierarchy.  Only if no
     * target pointer exists do we repeat by decorated name, matching MSVC's
     * cross-image fallback ordering. */
    for (pass = 0U; pass < 2U; ++pass) {
        for (i = 0U; i < base_count; ++i) {
            uint32_t address = ld32(base_array + i * 4U);
            guest_rtti_bcd candidate;
            int target_match;
            if (!guest_rtti_read_bcd(c, address, base_count, &candidate)) return;
            target_match = pass == 0U
                ? candidate.type_descriptor == target_type
                : guest_rtti_names_equal(c, candidate.type_descriptor,
                                         target_type, &match_ok);
            if (!match_ok) return;
            if (!target_match) continue;

            target_address = address;
            target_bcd = candidate;
            for (j = i + 1U; j < base_count; ++j) {
                uint32_t source_address = ld32(base_array + j * 4U);
                guest_rtti_bcd source_bcd;
                int source_match;
                if (!guest_rtti_read_bcd(c, source_address, base_count,
                                         &source_bcd)) return;
                if (source_bcd.attributes & GUEST_RTTI_BCD_PRIVORPROTBASE)
                    goto cast_failed;
                source_match = pass == 0U
                    ? source_bcd.type_descriptor == source_type
                    : guest_rtti_names_equal(c, source_bcd.type_descriptor,
                                             source_type, &match_ok);
                if (!match_ok) return;
                if (source_match) goto cast_succeeded;
            }
            goto cast_failed;
        }
    }

cast_failed:
    c->eax = 0U;
    if (is_reference) {
        guest_fault(c, target_type,
                    "__RTDynamicCast reference failure needs guest bad_cast");
        return;
    }
    host_cdecl_return(c);
    return;

cast_succeeded:
    (void)target_address;
    complete_calc = (int64_t)(uint64_t)complete + target_bcd.mdisp;
    if (complete_calc < 0 || complete_calc > UINT32_MAX) {
        guest_fault(c, complete, "__RTDynamicCast result pointer wraps");
        return;
    }
    c->eax = (uint32_t)complete_calc;
    host_cdecl_return(c);
}

/* `void *__cdecl memchr(const void *, int, size_t)` returns an address inside
 * the guest range, not a copied host pointer.  Scan through guest loads so
 * this remains correct for a future offset-backed Vita memory model. */
static void host_memchr(CPU *__restrict c)
{
    uint32_t source = host_arg(c, 0);
    uint8_t value = (uint8_t)host_arg(c, 1);
    uint32_t count = host_arg(c, 2);
    uint32_t index;

    if (count && (!source || source > UINT32_MAX - (count - 1U))) {
        guest_fault(c, source, "memchr received an invalid guest range");
        return;
    }
    c->eax = 0U;
    for (index = 0U; index < count; ++index) {
        if (ld8(source + index) == value) {
            c->eax = source + index;
            break;
        }
    }
    host_cdecl_return(c);
}

/* `char *__cdecl strchr(const char *, int)` searches through the terminating
 * NUL and returns the exact guest address.  Keep all reads in guest space so
 * the same boundary remains valid for an offset-backed target memory model. */
static void host_strchr(CPU *__restrict c)
{
    uint32_t cursor = host_arg(c, 0);
    uint8_t needle = (uint8_t)host_arg(c, 1);

    if (!cursor) {
        guest_fault(c, cursor, "strchr received a null guest pointer");
        return;
    }
    for (;;) {
        uint8_t value;
        if (cursor == UINT32_MAX) {
            guest_fault(c, cursor, "strchr guest string wraps address space");
            return;
        }
        value = ld8(cursor);
        if (value == needle) {
            c->eax = cursor;
            break;
        }
        if (!value) {
            c->eax = 0U;
            break;
        }
        ++cursor;
    }
    host_cdecl_return(c);
}

/* `char *__cdecl strstr(const char *, const char *)` returns an address in
 * the original guest haystack.  Keep every read in guest space instead of
 * handing a future offset-backed Vita address to a target libc.  The empty
 * needle returns the haystack before it is dereferenced, as the C contract
 * requires. */
static void host_strstr(CPU *__restrict c)
{
    uint32_t haystack = host_arg(c, 0);
    uint32_t needle = host_arg(c, 1);
    uint32_t needle_length = 0U;
    uint32_t cursor;

    if (!haystack || !needle) {
        guest_fault(c, !haystack ? haystack : needle,
                    "strstr received a null guest pointer");
        return;
    }
    for (;;) {
        if (needle > UINT32_MAX - needle_length) {
            guest_fault(c, needle, "strstr needle wraps address space");
            return;
        }
        if (!ld8(needle + needle_length)) break;
        ++needle_length;
    }
    if (!needle_length) {
        c->eax = haystack;
        host_cdecl_return(c);
        return;
    }

    cursor = haystack;
    for (;;) {
        uint32_t index;
        if (!ld8(cursor)) {
            c->eax = 0U;
            break;
        }
        for (index = 0U; index < needle_length; ++index) {
            uint8_t haystack_byte;
            if (cursor > UINT32_MAX - index) {
                guest_fault(c, cursor,
                            "strstr haystack wraps address space");
                return;
            }
            haystack_byte = ld8(cursor + index);
            if (!haystack_byte || haystack_byte != ld8(needle + index))
                break;
        }
        if (index == needle_length) {
            c->eax = cursor;
            break;
        }
        if (cursor == UINT32_MAX) {
            guest_fault(c, cursor, "strstr haystack wraps address space");
            return;
        }
        ++cursor;
    }
    host_cdecl_return(c);
}

/* `void __cdecl longjmp(jmp_buf, int)` is a genuine nonlocal transfer.  Its
 * native destination was established inline in the generated `_setjmp3`
 * owner; returning from this import would continue an already-unwound guest
 * path, so this handler never performs cdecl cleanup and never returns. */
static void host_longjmp(CPU *__restrict c)
{
    guest_longjmp(c, host_arg(c, 0), (int32_t)host_arg(c, 1));
}

/* `void __cdecl qsort(void *, size_t, size_t, int (__cdecl *)(...))`.
 *
 * A native qsort cannot call a translated comparator: its function pointer
 * is a guest address and its arguments must be placed on the emulated x86
 * stack.  Use an in-place heapsort instead.  Its comparison sequence is
 * deterministic, it needs no host allocation, and all element traffic stays
 * in guest bytes so this seam remains usable by an offset-based Vita backend.
 */
static int host_qsort_compare(CPU *__restrict c, uint32_t comparator,
                              uint32_t left, uint32_t right, int32_t *order)
{
    uint32_t saved = c->esp;

    gpush(c, right);                  /* cdecl: rightmost argument first */
    gpush(c, left);
    gpush(c, 0xFFF25047U);            /* synthetic native-shim return */
    guest_call(c, comparator);
    if (c->fault) return 0;
    if (c->esp != saved - 8U) {
        (void)guest_stack_set(c, saved, comparator);
        guest_fault(c, comparator,
                    "qsort comparator did not preserve its cdecl stack");
        return 0;
    }
    if (!guest_stack_adjust(c, 8U, comparator))
        return 0;                     /* qsort is the comparator's caller */
    *order = (int32_t)c->eax;
    return 1;
}

static void host_qsort_swap(uint32_t left, uint32_t right, uint32_t width)
{
    uint32_t i;

    if (left == right) return;
    for (i = 0U; i < width; ++i) {
        uint8_t temporary = ld8(left + i);
        st8(left + i, ld8(right + i));
        st8(right + i, temporary);
    }
}

static int host_qsort_sift_down(CPU *__restrict c, uint32_t base,
                                uint32_t width, uint32_t comparator,
                                uint32_t root, uint32_t end)
{
    for (;;) {
        uint32_t child;
        uint32_t root_address, child_address;
        int32_t order;

        /* Check before doubling: a valid width-1 array can contain nearly
         * UINT32_MAX elements, and a leaf index must not wrap into a child. */
        if (end < 2U || root > (end - 2U) / 2U) return 1;
        child = root * 2U + 1U;
        if (child + 1U < end) {
            uint32_t left = base + child * width;
            uint32_t right = left + width;
            if (!host_qsort_compare(c, comparator, left, right, &order))
                return 0;
            if (order < 0) ++child;   /* choose the strictly larger child */
        }
        root_address = base + root * width;
        child_address = base + child * width;
        if (!host_qsort_compare(c, comparator, root_address, child_address,
                                &order))
            return 0;
        if (order >= 0) return 1;
        host_qsort_swap(root_address, child_address, width);
        root = child;
    }
}

static void host_qsort(CPU *__restrict c)
{
    uint32_t base = host_arg(c, 0);
    uint32_t count = host_arg(c, 1);
    uint32_t width = host_arg(c, 2);
    uint32_t comparator = host_arg(c, 3);
    uint64_t span;
    uint32_t start, end;

    /* No element is compared or touched in these cases.  In particular, a
     * zero-length container may use the conventional null data pointer. */
    if (count < 2U) {
        host_cdecl_return(c);
        return;
    }
    if (!base) {
        guest_fault(c, base, "qsort received a null base");
        return;
    }
    if (!width) {
        guest_fault(c, width, "qsort received a zero element width");
        return;
    }
    if (!comparator) {
        guest_fault(c, comparator, "qsort received a null comparator");
        return;
    }
    span = (uint64_t)count * (uint64_t)width;
    if (span > (uint64_t)UINT32_MAX - (uint64_t)base + 1U) {
        guest_fault(c, base,
                    "qsort guest range overflows 32-bit address space");
        return;
    }

    for (start = count / 2U; start != 0U; --start) {
        if (!host_qsort_sift_down(c, base, width, comparator,
                                  start - 1U, count))
            return;
    }
    for (end = count; end > 1U; --end) {
        host_qsort_swap(base, base + (end - 1U) * width, width);
        if (!host_qsort_sift_down(c, base, width, comparator, 0U, end - 1U))
            return;
    }
    host_cdecl_return(c);
}

/* `char *__cdecl getenv(const char *)`.  The target has no process
 * environment, and inheriting the PC harness's USERPROFILE/HOMEDRIVE would
 * make the guest's save path depend on who launched the test.  Report every
 * key as absent.  This exact main() first probes USERPROFILE, then
 * HOMEDRIVE+HOMEPATH, and finally uses its own portable `./` fallback after
 * checking that the directory is writable. */
static void host_getenv(CPU *__restrict c)
{
    (void)host_arg(c, 0);       /* name is intentionally policy-independent */
    c->eax = 0U;
    host_cdecl_return(c);
}

/* `int __cdecl isdigit(int)`.  The UCRT returns the `_DIGIT` classification
 * bit (0x04), not merely a made-up Boolean.  Test the unsigned distance from
 * ASCII '0' so negative values and values above UCHAR_MAX stay well-defined;
 * this is the exact boundary reached while libepoxy parses GL_VERSION. */
static void host_isdigit(CPU *__restrict c)
{
    uint32_t value = host_arg(c, 0);
    c->eax = value - (uint32_t)'0' <= 9U ? 0x04U : 0U;
    host_cdecl_return(c);
}

static int host_ascii_space(uint32_t value)
{
    return value == 0x20U || value - 0x09U <= 4U;
}

static int host_ascii_punct(uint32_t value)
{
    return (value - 0x21U <= 0x0EU) ||
           (value - 0x3AU <= 0x06U) ||
           (value - 0x5BU <= 0x05U) ||
           (value - 0x7BU <= 0x03U);
}

/* MSVC's narrow ctype functions return their classification BIT rather than
 * a made-up Boolean.  The guest remains in the C locale, so make the ASCII
 * policy explicit and leave negative sign-extended bytes unclassified. */
static void host_isspace(CPU *__restrict c)
{
    c->eax = host_ascii_space(host_arg(c, 0)) ? 0x08U : 0U;
    host_cdecl_return(c);
}

static void host_ispunct(CPU *__restrict c)
{
    c->eax = host_ascii_punct(host_arg(c, 0)) ? 0x10U : 0U;
    host_cdecl_return(c);
}

static void host_tolower(CPU *__restrict c)
{
    uint32_t value = host_arg(c, 0);
    c->eax = value - (uint32_t)'A' <= 25U ? value + 0x20U : value;
    host_cdecl_return(c);
}

static void host_toupper(CPU *__restrict c)
{
    uint32_t value = host_arg(c, 0);
    c->eax = value - (uint32_t)'a' <= 25U ? value - 0x20U : value;
    host_cdecl_return(c);
}

/* Measured against the installed UCRT in its default C locale.  Unlike the
 * narrow classifier, iswspace recognises the Windows Unicode whitespace set;
 * preserve that useful behaviour without depending on target wchar_t width
 * or locale tables. */
static void host_iswspace(CPU *__restrict c)
{
    uint32_t value = host_arg(c, 0);
    int yes = host_ascii_space(value) || value == 0x85U || value == 0xA0U ||
              value == 0x1680U || value == 0x180EU ||
              value - 0x2000U <= 0x0AU ||
              value - 0x2028U <= 1U || value == 0x202FU ||
              value == 0x205FU || value == 0x3000U;
    c->eax = yes ? 0x08U : 0U;
    host_cdecl_return(c);
}

/*
 * int __cdecl __stdio_common_vsprintf(
 *     uint64_t options, char *buffer, size_t count, const char *format,
 *     _locale_t locale, va_list arguments);
 *
 * The signature is copied from the installed UCRT `stdio.h`.  Its va_list is
 * an x86 guest pointer, so passing it to the host CRT would work accidentally
 * in this 32-bit harness and fail as soon as the runtime is built for ARM.
 * Decode the measured portable subset instead.  In addition to the startup
 * path forms, the game's statically named logger call sites prove the narrow
 * integer, bounded-string, double and uint64 forms handled below.  Anything
 * else stays loud until a real call proves the next format we need.
 *
 * Option bit 0 selects legacy `_vsnprintf` truncation: exact-fit output returns
 * `count` without a terminator; longer output writes all `count` bytes and
 * returns -1.  Bit 1 is standard snprintf: reserve a terminator and return the
 * required length.  UCRT renders a null `%s` argument as `(null)`, which is how
 * the game's unchecked getenv(NULL-result) probes safely fall through. */
typedef struct host_format_output {
    uint32_t buffer;
    uint32_t limit;
    uint32_t written;
    FILE    *stream;
    int      failed;
} host_format_output;

static void host_stdio_put(host_format_output *out, uint8_t value)
{
    if (out->stream) {
        if (fputc((int)value, out->stream) == EOF)
            out->failed = 1;
    } else if (out->written < out->limit) {
        st8(out->buffer + out->written, value);
    }
    ++out->written;
}

static void host_stdio_put_string_limited(host_format_output *out,
                                          uint32_t source, uint32_t limit)
{
    static const char null_string[] = "(null)";
    if (!source) {
        const char *p = null_string;
        while (*p && limit) {
            host_stdio_put(out, (uint8_t)*p++);
            --limit;
        }
        return;
    }
    while (limit) {
        uint8_t value = ld8(source++);
        if (!value) return;
        host_stdio_put(out, value);
        --limit;
    }
}

static void host_stdio_put_string(host_format_output *out, uint32_t source)
{
    host_stdio_put_string_limited(out, source, UINT32_MAX);
}

static void host_stdio_put_repeat(host_format_output *out, uint8_t value,
                                  unsigned count)
{
    while (count--)
        host_stdio_put(out, value);
}

static void host_stdio_put_i32_width(host_format_output *out, uint32_t raw,
                                     unsigned width, uint8_t padding,
                                     int space_sign)
{
    char digits[10];
    uint32_t magnitude = raw;
    unsigned used = 0U;
    unsigned prefix = 0U;
    uint8_t sign = 0U;

    if ((int32_t)raw < 0) {
        sign = '-';
        magnitude = 0U - raw;       /* also defined for INT32_MIN */
    } else if (space_sign) {
        sign = ' ';
    }
    do {
        digits[used++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    prefix = sign != 0U;
    if (padding != '0' && width > used + prefix)
        host_stdio_put_repeat(out, ' ', width - used - prefix);
    if (sign) host_stdio_put(out, sign);
    if (padding == '0' && width > used + prefix)
        host_stdio_put_repeat(out, '0', width - used - prefix);
    while (used)
        host_stdio_put(out, (uint8_t)digits[--used]);
}

static void host_stdio_put_i32(host_format_output *out, uint32_t raw)
{
    host_stdio_put_i32_width(out, raw, 0U, ' ', 0);
}

static void host_stdio_put_u64_hex_width(host_format_output *out, uint64_t raw,
                                         int uppercase, unsigned width)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *alphabet = uppercase ? upper : lower;
    char digits[16];
    unsigned used = 0U;

    do {
        digits[used++] = alphabet[(unsigned)(raw & 15U)];
        raw >>= 4;
    } while (raw);
    while (used < width)
        digits[used++] = '0';
    while (used)
        host_stdio_put(out, (uint8_t)digits[--used]);
}

static void host_stdio_put_u32_width(host_format_output *out, uint32_t raw,
                                     unsigned width, uint8_t padding)
{
    char digits[10];
    unsigned used = 0U;

    do {
        digits[used++] = (char)('0' + raw % 10U);
        raw /= 10U;
    } while (raw);
    if (width > used)
        host_stdio_put_repeat(out, padding, width - used);
    while (used)
        host_stdio_put(out, (uint8_t)digits[--used]);
}

static void host_stdio_put_u32(host_format_output *out, uint32_t raw)
{
    host_stdio_put_u32_width(out, raw, 0U, ' ');
}

static void host_stdio_put_u64(host_format_output *out, uint64_t raw)
{
    char digits[20];
    unsigned used = 0U;

    do {
        digits[used++] = (char)('0' + raw % 10U);
        raw /= 10U;
    } while (raw);
    while (used)
        host_stdio_put(out, (uint8_t)digits[--used]);
}

static int host_stdio_put_double(CPU *__restrict c, host_format_output *out,
                                 uint64_t raw, const char *native_format,
                                 uint32_t guest_format)
{
    char text[384];
    double value;
    _locale_t locale;
    int length;
    int i;

    memcpy(&value, &raw, sizeof value);
    locale = _create_locale(LC_NUMERIC, "C");
    if (!locale) {
        guest_fault(c, guest_format, "guest stdio could not create C locale");
        return 0;
    }
    length = _snprintf_l(text, sizeof text, native_format, locale, value);
    _free_locale(locale);
    if (length < 0 || (unsigned)length >= sizeof text) {
        guest_fault(c, guest_format, "guest stdio double conversion overflow");
        return 0;
    }
    for (i = 0; i < length; ++i)
        host_stdio_put(out, (uint8_t)text[i]);
    return 1;
}

static int host_stdio_format(CPU *__restrict c, uint32_t format,
                             uint32_t arguments, host_format_output *out)
{
    for (;;) {
        uint8_t value = ld8(format++);
        if (!value) return 1;
        if (value != '%') {
            host_stdio_put(out, value);
            continue;
        }
        {
        uint32_t guest_format = format - 1U;
        value = ld8(format++);
        if (value == '%') {
            host_stdio_put(out, '%');
        } else if (value == 's') {
            uint32_t source = ld32(arguments);
            arguments += 4U;
            host_stdio_put_string(out, source);
        } else if (value == 'd') {
            uint32_t integer = ld32(arguments);
            arguments += 4U;
            host_stdio_put_i32(out, integer);
        } else if (value == 'X' || value == 'x') {
            uint32_t integer = ld32(arguments);
            arguments += 4U;
            host_stdio_put_u64_hex_width(out, integer, value == 'X', 0U);
        } else if (value == 'u') {
            uint32_t integer = ld32(arguments);
            arguments += 4U;
            host_stdio_put_u32(out, integer);
        } else if (value == 'l' && ld8(format) == 'd') {
            uint32_t integer = ld32(arguments);
            ++format;
            arguments += 4U;        /* x86 long is 32 bits */
            host_stdio_put_i32(out, integer);
        } else if (value == '0' && ld8(format) == '8' &&
                   ld8(format + 1U) == 'x') {
            uint32_t integer = ld32(arguments);
            format += 2U;
            arguments += 4U;
            host_stdio_put_u64_hex_width(out, integer, 0, 8U);
        } else if (value == '0' &&
                   (ld8(format) == '2' || ld8(format) == '3' ||
                    ld8(format) == '4') &&
                   ld8(format + 1U) == 'u') {
            uint32_t integer = ld32(arguments);
            unsigned width = (unsigned)(ld8(format) - '0');
            format += 2U;
            arguments += 4U;
            host_stdio_put_u32_width(out, integer, width, '0');
        } else if ((value == '2' || value == '4') && ld8(format) == 'd') {
            uint32_t integer = ld32(arguments);
            unsigned width = (unsigned)(value - '0');
            ++format;
            arguments += 4U;
            host_stdio_put_i32_width(out, integer, width, ' ', 0);
        } else if (value == '0' &&
                   (ld8(format) == '2' || ld8(format) == '3') &&
                   ld8(format + 1U) == 'd') {
            uint32_t integer = ld32(arguments);
            unsigned width = (unsigned)(ld8(format) - '0');
            format += 2U;
            arguments += 4U;
            host_stdio_put_i32_width(out, integer, width, '0', 0);
        } else if (value == '.' && ld8(format) == '1' &&
                   ld8(format + 1U) == '6' && ld8(format + 2U) == 's') {
            uint32_t source = ld32(arguments);
            format += 3U;
            arguments += 4U;
            host_stdio_put_string_limited(out, source, 16U);
        } else if (value == ' ' && ld8(format) == 'd') {
            uint32_t integer = ld32(arguments);
            ++format;
            arguments += 4U;
            host_stdio_put_i32_width(out, integer, 0U, ' ', 1);
        } else if (value == 'f' || value == 'g' || value == '.') {
            uint64_t raw;
            const char *native_format = NULL;
            if (value == 'f') {
                native_format = "%f";
            } else if (value == 'g') {
                native_format = "%g";
            } else if (ld8(format) == 'f') {
                ++format;
                native_format = "%.0f";
            } else if (ld8(format) == '1' && ld8(format + 1U) == 'f') {
                format += 2U;
                native_format = "%.1f";
            } else if (ld8(format) == '2' && ld8(format + 1U) == 'f') {
                format += 2U;
                native_format = "%.2f";
            } else if (ld8(format) == '4' && ld8(format + 1U) == 'f') {
                format += 2U;
                native_format = "%.4f";
            }
            if (!native_format) {
                guest_fault(c, guest_format, "guest stdio unsupported format");
                return 0;
            }
            raw = (uint64_t)ld32(arguments) |
                  ((uint64_t)ld32(arguments + 4U) << 32);
            arguments += 8U;
            if (!host_stdio_put_double(c, out, raw, native_format,
                                       guest_format)) return 0;
        } else if (value == 'l' && ld8(format) == 'l' &&
                   ld8(format + 1U) == 'u') {
            uint64_t integer = (uint64_t)ld32(arguments) |
                               ((uint64_t)ld32(arguments + 4U) << 32);
            format += 2U;
            arguments += 8U;
            host_stdio_put_u64(out, integer);
        } else if (value == '0' && ld8(format) == '1' &&
                   ld8(format + 1U) == '6' && ld8(format + 2U) == 'l' &&
                   ld8(format + 3U) == 'l' && ld8(format + 4U) == 'x') {
            uint64_t integer = (uint64_t)ld32(arguments) |
                               ((uint64_t)ld32(arguments + 4U) << 32);
            format += 5U;
            arguments += 8U;
            host_stdio_put_u64_hex_width(out, integer, 0, 16U);
        } else if (value == '0' && ld8(format) == '8' &&
                   ld8(format + 1U) == 'l' && ld8(format + 2U) == 'l' &&
                   ld8(format + 3U) == 'x') {
            uint64_t integer = (uint64_t)ld32(arguments) |
                               ((uint64_t)ld32(arguments + 4U) << 32);
            format += 4U;
            arguments += 8U;
            host_stdio_put_u64_hex_width(out, integer, 0, 8U);
        } else {
            guest_fault(c, guest_format, "guest stdio unsupported format");
            return 0;
        }
        }
    }
}

static void host_stdio_common_vsprintf(CPU *__restrict c)
{
    uint32_t buffer = host_arg(c, 2);
    uint32_t count = host_arg(c, 3);
    uint32_t format = host_arg(c, 4);
    uint32_t arguments = host_arg(c, 6);
    uint32_t options = host_arg(c, 0);
    int standard = (options & 2U) != 0;
    uint32_t limit = standard ? (count ? count - 1U : 0U) : count;
    host_format_output out;

    (void)host_arg(c, 1);       /* options high and locale do not affect */
    (void)host_arg(c, 5);
    if (!format || (!buffer && count)) {
        c->eax = UINT32_MAX;
        host_cdecl_return(c);
        return;
    }

    out.buffer = buffer;
    out.limit = limit;
    out.written = 0U;
    out.stream = NULL;
    out.failed = 0;
    if (!host_stdio_format(c, format, arguments, &out)) return;

    if (!buffer) {
        /* UCRT's count-only form is valid for either option mode. */
        c->eax = out.written;
    } else if (standard) {
        if (count)
            st8(buffer + (out.written < limit ? out.written : limit), 0U);
        c->eax = out.written;
    } else {
        if (out.written < count)
            st8(buffer + out.written, 0U);
        c->eax = out.written <= count ? out.written : UINT32_MAX;
    }
    host_cdecl_return(c);
}

/* Secure sibling of the formatter above.  It has the same seven-dword x86
 * ABI and guest va_list, but requires a real destination and must leave an
 * empty string rather than a partial result when the formatted output does
 * not fit. */
static void host_stdio_common_vsprintf_s(CPU *__restrict c)
{
    uint32_t buffer = host_arg(c, 2);
    uint32_t count = host_arg(c, 3);
    uint32_t format = host_arg(c, 4);
    uint32_t arguments = host_arg(c, 6);
    host_format_output out;

    (void)host_arg(c, 0);
    (void)host_arg(c, 1);
    (void)host_arg(c, 5);
    if (!buffer || !count || !format) {
        if (buffer && count) st8(buffer, 0U);
        c->eax = UINT32_MAX;
        host_cdecl_return(c);
        return;
    }
    out.buffer = buffer;
    out.limit = count - 1U;
    out.written = 0U;
    out.stream = NULL;
    out.failed = 0;
    if (!host_stdio_format(c, format, arguments, &out)) return;
    if (out.written >= count) {
        st8(buffer, 0U);
        c->eax = UINT32_MAX;
    } else {
        st8(buffer + out.written, 0U);
        c->eax = out.written;
    }
    host_cdecl_return(c);
}

/*
 * int __cdecl __stdio_common_vsscanf(
 *     uint64_t options, const char *buffer, size_t buffer_count,
 *     const char *format, _locale_t locale, va_list arguments);
 *
 * The signature is copied from the installed UCRT `stdio.h`.  The PE has one
 * IAT call site, in its ordinary `sscanf` wrapper at RVA 0x003f0ef0.  That
 * wrapper supplies `(size_t)-1`, a null locale, and an x86 guest va_list (a
 * pointer to consecutive destination pointers).  CRT startup ORs option bit
 * 1 (`_CRT_INTERNAL_SCANF_LEGACY_WIDE_SPECIFIERS`, numeric value 2) into the
 * local option word; it does not affect the admitted narrow integer formats.
 * Four
 * statically measured callers use exactly these formats:
 *
 *     ISAACNG_GSR%u       two shader-register call sites
 *     %i.%i               OpenGL version parsing
 *     %d.%d.%d            GLFW/WGL version parsing
 *
 * Never pass the guest va_list to a native CRT: it is only accidentally ABI
 * compatible in the 32-bit PC harness and will not be compatible on ARM.
 * Keep the admitted surface explicit; a new format or ABI shape is a useful
 * frontier and must fault instead of being guessed or silently ignored. */
static int host_scan_guest_string_equals(uint32_t guest, const char *text)
{
    if (!guest || !text) return 0;
    for (;;) {
        uint8_t actual = ld8(guest++);
        uint8_t expected = (uint8_t)*text++;
        if (actual != expected) return 0;
        if (!actual) return 1;
    }
}

static int host_scan_space(uint8_t value)
{
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\v' || value == '\f';
}

static int host_scan_digit(uint8_t value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F') return (int)(value - 'A') + 10;
    return -1;
}

/* Return 1 for a conversion, 0 for a matching failure, and -1 after a loud
 * range fault.  The three admitted conversions all store one 32-bit value. */
static int host_scan_integer(CPU *__restrict c, uint32_t *cursor,
                             int requested_base, int signed_result,
                             uint32_t *result)
{
    uint32_t p = *cursor;
    uint32_t value = 0U;
    uint32_t limit;
    unsigned digits = 0U;
    int negative = 0;
    int base = requested_base;

    while (host_scan_space(ld8(p))) ++p;
    if (ld8(p) == '+' || ld8(p) == '-') {
        negative = ld8(p) == '-';
        ++p;
    }
    if (base == 0) {
        if (ld8(p) == '0') {
            int after_prefix = host_scan_digit(ld8(p + 2U));
            if ((ld8(p + 1U) == 'x' || ld8(p + 1U) == 'X') &&
                after_prefix >= 0 && after_prefix < 16) {
                base = 16;
                p += 2U;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    }
    limit = signed_result
        ? (negative ? UINT32_C(0x80000000) : UINT32_C(0x7fffffff))
        : UINT32_MAX;
    for (;;) {
        int digit = host_scan_digit(ld8(p));
        if (digit < 0 || digit >= base) break;
        if (value > (limit - (uint32_t)digit) / (uint32_t)base) {
            guest_fault(c, p, "guest stdio scanned integer outside 32-bit range");
            return -1;
        }
        value = value * (uint32_t)base + (uint32_t)digit;
        ++p;
        ++digits;
    }
    if (!digits) return 0;
    *cursor = p;
    *result = negative ? 0U - value : value;
    return 1;
}

static int host_scan_store_integer(CPU *__restrict c, uint32_t *cursor,
                                   uint32_t *arguments, int base,
                                   int signed_result)
{
    uint32_t value, destination;
    int converted = host_scan_integer(c, cursor, base, signed_result, &value);
    if (converted <= 0) return converted;
    destination = ld32(*arguments);
    if (!destination) {
        guest_fault(c, *arguments, "guest stdio scan has a null destination");
        return -1;
    }
    *arguments += 4U;
    st32(destination, value);
    return 1;
}

static int host_scan_literal(uint32_t *cursor, const char *literal)
{
    uint32_t p = *cursor;
    while (*literal) {
        if (ld8(p) != (uint8_t)*literal++) return 0;
        ++p;
    }
    *cursor = p;
    return 1;
}

static void host_stdio_common_vsscanf(CPU *__restrict c)
{
    const uint32_t legacy_wide_specifiers = 2U;
    uint32_t options_low = host_arg(c, 0);
    uint32_t options_high = host_arg(c, 1);
    uint32_t cursor = host_arg(c, 2);
    uint32_t buffer_count = host_arg(c, 3);
    uint32_t format = host_arg(c, 4);
    uint32_t locale = host_arg(c, 5);
    uint32_t arguments = host_arg(c, 6);
    uint32_t assignments = 0U;
    int converted;

    if ((options_low & ~legacy_wide_specifiers) || options_high) {
        guest_fault(c, options_low ? options_low : options_high,
                    "guest stdio unsupported vsscanf option bits");
        return;
    }
    if (buffer_count != UINT32_MAX) {
        guest_fault(c, buffer_count,
                    "guest stdio unsupported finite scan count");
        return;
    }
    if (locale) {
        guest_fault(c, locale, "guest stdio unsupported scan locale");
        return;
    }
    if (!cursor || !format || !arguments) {
        guest_fault(c, format, "guest stdio vsscanf received a null pointer");
        return;
    }

    if (host_scan_guest_string_equals(format, "ISAACNG_GSR%u")) {
        if (host_scan_literal(&cursor, "ISAACNG_GSR")) {
            converted = host_scan_store_integer(c, &cursor, &arguments,
                                                10, 0);
            if (converted < 0) return;
            assignments += (uint32_t)converted;
        }
    } else if (host_scan_guest_string_equals(format, "%i.%i")) {
        converted = host_scan_store_integer(c, &cursor, &arguments, 0, 1);
        if (converted < 0) return;
        assignments += (uint32_t)converted;
        if (converted && host_scan_literal(&cursor, ".")) {
            converted = host_scan_store_integer(c, &cursor, &arguments, 0, 1);
            if (converted < 0) return;
            assignments += (uint32_t)converted;
        }
    } else if (host_scan_guest_string_equals(format, "%d.%d.%d")) {
        converted = host_scan_store_integer(c, &cursor, &arguments, 10, 1);
        if (converted < 0) return;
        assignments += (uint32_t)converted;
        if (converted && host_scan_literal(&cursor, ".")) {
            converted = host_scan_store_integer(c, &cursor, &arguments, 10, 1);
            if (converted < 0) return;
            assignments += (uint32_t)converted;
            if (converted && host_scan_literal(&cursor, ".")) {
                converted = host_scan_store_integer(c, &cursor, &arguments,
                                                    10, 1);
                if (converted < 0) return;
                assignments += (uint32_t)converted;
            }
        }
    } else {
        guest_fault(c, format, "guest stdio unsupported scan format");
        return;
    }

    c->eax = assignments;
    host_cdecl_return(c);
}

/* FILE* is an opaque native platform handle.  Both current targets are
 * 32-bit, so it fits in the guest return register; every FILE operation must
 * stay on this one libc boundary rather than mixing guest and host FILE
 * layouts. */
static void host_fopen(CPU *__restrict c)
{
    FILE *stream = fopen((const char *)(uintptr_t)host_arg(c, 0),
                         (const char *)(uintptr_t)host_arg(c, 1));
    c->eax = host_guest_ptr(stream);
    host_cdecl_return(c);
}

static void host_fclose(CPU *__restrict c)
{
    FILE *stream = (FILE *)(uintptr_t)host_arg(c, 0);
    c->eax = stream ? (uint32_t)fclose(stream) : UINT32_MAX;
    host_cdecl_return(c);
}

static void host_fileno(CPU *__restrict c)
{
    FILE *stream = (FILE *)(uintptr_t)host_arg(c, 0);
    c->eax = (uint32_t)_fileno(stream);
    host_cdecl_return(c);
}

static void host_fread(CPU *__restrict c)
{
    c->eax = (uint32_t)fread(
        (void *)(uintptr_t)host_arg(c, 0),
        (size_t)host_arg(c, 1), (size_t)host_arg(c, 2),
        (FILE *)(uintptr_t)host_arg(c, 3));
    host_cdecl_return(c);
}

static void host_fwrite(CPU *__restrict c)
{
    c->eax = (uint32_t)fwrite(
        (const void *)(uintptr_t)host_arg(c, 0),
        (size_t)host_arg(c, 1), (size_t)host_arg(c, 2),
        (FILE *)(uintptr_t)host_arg(c, 3));
    host_cdecl_return(c);
}

static void host_fseek(CPU *__restrict c)
{
    c->eax = (uint32_t)fseek(
        (FILE *)(uintptr_t)host_arg(c, 0),
        (long)(int32_t)host_arg(c, 1), (int)host_arg(c, 2));
    host_cdecl_return(c);
}

static void host_get_osfhandle(CPU *__restrict c)
{
    c->eax = (uint32_t)(intptr_t)_get_osfhandle((int)host_arg(c, 0));
    host_cdecl_return(c);
}

static void host_ftell(CPU *__restrict c)
{
    c->eax = (uint32_t)(int32_t)ftell(
        (FILE *)(uintptr_t)host_arg(c, 0));
    host_cdecl_return(c);
}

static void host_fflush(CPU *__restrict c)
{
    c->eax = (uint32_t)fflush(
        (FILE *)(uintptr_t)host_arg(c, 0));
    host_cdecl_return(c);
}

/* `FILE *__cdecl __acrt_iob_func(unsigned)` -- the UCRT's 0/1/2 standard
 * streams.  Keep FILE opaque and on the same native-libc boundary as
 * __stdio_common_vfprintf below; guest code only passes this token back to
 * that shim.  The current missing-OpenGL report requests index 2 (stderr). */
static void host_acrt_iob_func(CPU *__restrict c)
{
    uint32_t index = host_arg(c, 0);
    FILE *stream;
    if (index == 0U) stream = stdin;
    else if (index == 1U) stream = stdout;
    else if (index == 2U) stream = stderr;
    else {
        guest_fault(c, index, "__acrt_iob_func index outside 0..2");
        return;
    }
    c->eax = host_guest_ptr(stream);
    host_cdecl_return(c);
}

/* Bounds-checked UCRT strings are decoded at the guest boundary instead of
 * calling the host secure CRT (whose invalid-parameter handler is process
 * global and whose implementation will not exist on Vita). */
#define HOST_STRUNCATE 80U

/* `mbstowcs_s` from the PE's UCRT ABI converts into 16-bit Windows wchar_t,
 * irrespective of the target libc's native wchar_t width.  The game remains
 * in the C locale, whose UCRT conversion maps each unsigned byte directly to
 * the same U+00xx code point without consulting the PC's active code page. */
static void host_mbstowcs_s(CPU *__restrict c)
{
    uint32_t converted = host_arg(c, 0);
    uint32_t dest = host_arg(c, 1);
    uint32_t capacity = host_arg(c, 2);
    uint32_t source = host_arg(c, 3);
    uint32_t max_count = host_arg(c, 4);
    uint32_t limit, length = 0U, i;
    int truncated = 0;

    if (converted) st32(converted, 0U);
    if (!source || (!dest && capacity)) {
        if (dest && capacity) st16(dest, 0U);
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    limit = max_count == UINT32_MAX
        ? (capacity ? capacity - 1U : 0U) : max_count;
    while (length < limit) {
        uint8_t value = ld8(source + length);
        if (!value) break;
        ++length;
    }
    if (max_count == UINT32_MAX && ld8(source + length) != 0U)
        truncated = 1;
    if (length + 1U > capacity && dest) {
        if (capacity) st16(dest, 0U);
        c->eax = ERANGE;
        host_cdecl_return(c);
        return;
    }
    if (dest) {
        for (i = 0U; i < length; ++i)
            st16(dest + i * 2U, (uint16_t)ld8(source + i));
        st16(dest + length * 2U, 0U);
    }
    if (converted) st32(converted, length + 1U);
    c->eax = truncated ? HOST_STRUNCATE : 0U;
    host_cdecl_return(c);
}

/* Reverse side of the same guest path boundary.  Read 16-bit PE wchar_t
 * explicitly; newlib's wchar_t is wider on Vita.  UCRT's C locale maps every
 * U+00xx unit to the same byte and reports larger units as EILSEQ. */
static void host_wcstombs_s(CPU *__restrict c)
{
    uint32_t converted = host_arg(c, 0);
    uint32_t dest = host_arg(c, 1);
    uint32_t capacity = host_arg(c, 2);
    uint32_t source = host_arg(c, 3);
    uint32_t max_count = host_arg(c, 4);
    uint32_t limit, length = 0U, i;
    int truncated = 0;

    if (converted) st32(converted, 0U);
    if (!source || (!dest && capacity)) {
        if (dest && capacity) st8(dest, 0U);
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    limit = max_count == UINT32_MAX
        ? (capacity ? capacity - 1U : 0U) : max_count;
    while (length < limit) {
        uint16_t value = ld16(source + length * 2U);
        if (!value) break;
        if (value > 0xffU) {
            if (dest && capacity) st8(dest, 0U);
            c->eax = EILSEQ;
            host_cdecl_return(c);
            return;
        }
        ++length;
    }
    if (max_count == UINT32_MAX && ld16(source + length * 2U) != 0U)
        truncated = 1;
    if (length + 1U > capacity && dest) {
        if (capacity) st8(dest, 0U);
        c->eax = ERANGE;
        host_cdecl_return(c);
        return;
    }
    if (dest) {
        for (i = 0U; i < length; ++i)
            st8(dest + i, (uint8_t)ld16(source + i * 2U));
        st8(dest + length, 0U);
    }
    if (converted) st32(converted, length + 1U);
    c->eax = truncated ? HOST_STRUNCATE : 0U;
    host_cdecl_return(c);
}

/* ISO C `strpbrk` returns a pointer into its first guest string, never a copy.
 * The current VFS parser repeatedly calls it with "/" to split a path, so
 * preserving the exact guest address is part of the contract. */
static void host_strpbrk(CPU *__restrict c)
{
    uint32_t source = host_arg(c, 0);
    uint32_t accept = host_arg(c, 1);
    uint32_t cursor;

    if (!source || !accept) {
        guest_fault(c, !source ? source : accept,
                    "strpbrk received a null guest pointer");
        return;
    }
    for (cursor = source; ld8(cursor); ++cursor) {
        uint32_t candidate;
        uint8_t value = ld8(cursor);
        for (candidate = accept; ld8(candidate); ++candidate) {
            if (value == ld8(candidate)) {
                c->eax = cursor;
                host_cdecl_return(c);
                return;
            }
        }
    }
    c->eax = 0U;
    host_cdecl_return(c);
}

static uint8_t host_ascii_lower_byte(uint8_t value)
{
    return value - (uint8_t)'A' <= 25U ? (uint8_t)(value + 0x20U) : value;
}

/* UCRT `_strnicmp` in the C locale folds ASCII and otherwise compares bytes
 * as unsigned.  Return the first folded-byte difference; callers only require
 * the documented sign/zero contract, and this also matches measured UCRT
 * results such as "A@" versus "a[" -> -27. */
static void host_strnicmp(CPU *__restrict c)
{
    uint32_t left = host_arg(c, 0);
    uint32_t right = host_arg(c, 1);
    uint32_t count = host_arg(c, 2);
    uint32_t i;
    int result = 0;

    if (count && (!left || !right)) {
        guest_fault(c, !left ? left : right,
                    "_strnicmp received a null guest pointer");
        return;
    }
    for (i = 0U; i < count; ++i) {
        uint8_t original = ld8(left + i);
        uint8_t a = host_ascii_lower_byte(original);
        uint8_t b = host_ascii_lower_byte(ld8(right + i));
        if (a != b) {
            result = (int)a - (int)b;
            break;
        }
        if (!original) break;
    }
    c->eax = (uint32_t)result;
    host_cdecl_return(c);
}

/* ISO C `strncmp` compares bytes as unsigned char and never reads beyond the
 * requested count.  Return the first byte difference, which preserves the
 * standard's sign contract and matches the UCRT implementation. */
static void host_strncmp(CPU *__restrict c)
{
    uint32_t left = host_arg(c, 0);
    uint32_t right = host_arg(c, 1);
    uint32_t count = host_arg(c, 2);
    uint32_t i;
    int result = 0;

    if (count && (!left || !right)) {
        guest_fault(c, !left ? left : right,
                    "strncmp received a null guest pointer");
        return;
    }
    for (i = 0U; i < count; ++i) {
        uint8_t a = ld8(left + i);
        uint8_t b = ld8(right + i);
        if (a != b) {
            result = (int)a - (int)b;
            break;
        }
        if (!a) break;
    }
    c->eax = (uint32_t)result;
    host_cdecl_return(c);
}

/* ISO C `strncpy(char *, const char *, size_t)` -- cdecl.  Keep the copy in
 * guest-address terms so the same implementation survives the ARM backend:
 * copy at most count bytes, pad the remainder after the first NUL, and do not
 * append an extra terminator when the source fills the whole count. */
static void host_strncpy(CPU *__restrict c)
{
    uint32_t dest = host_arg(c, 0);
    uint32_t source = host_arg(c, 1);
    uint32_t count = host_arg(c, 2);
    uint32_t i;
    int padding = 0;

    if (count && (!dest || !source)) {
        guest_fault(c, source, "strncpy received a null guest pointer");
        return;
    }
    for (i = 0U; i < count; ++i) {
        uint8_t value = padding ? 0U : ld8(source + i);
        st8(dest + i, value);
        if (!value) padding = 1;
    }
    c->eax = dest;
    host_cdecl_return(c);
}

/* UCRT `strcpy_s(char *, rsize_t, const char *)` -- cdecl.  Keep the
 * destination in a deterministic empty-string state on constraint failures
 * for which a positive writable capacity was supplied. */
static void host_strcpy_s(CPU *__restrict c)
{
    uint32_t dest = host_arg(c, 0);
    uint32_t capacity = host_arg(c, 1);
    uint32_t source = host_arg(c, 2);
    uint32_t i;

    if (!dest) {
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    if (!capacity) {
        c->eax = ERANGE;
        host_cdecl_return(c);
        return;
    }
    if (!source) {
        st8(dest, 0U);
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    for (i = 0U; i < capacity; ++i) {
        uint8_t value = ld8(source + i);
        st8(dest + i, value);
        if (!value) {
            c->eax = 0U;
            host_cdecl_return(c);
            return;
        }
    }
    st8(dest, 0U);
    c->eax = ERANGE;
    host_cdecl_return(c);
}

static void host_strncpy_s(CPU *__restrict c)
{
    uint32_t dest = host_arg(c, 0);
    uint32_t capacity = host_arg(c, 1);
    uint32_t source = host_arg(c, 2);
    uint32_t count = host_arg(c, 3);
    uint32_t i;

    if (!dest || !capacity) {
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    if (!source) {
        st8(dest, 0U);
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    if (!count) {
        st8(dest, 0U);
        c->eax = 0U;
        host_cdecl_return(c);
        return;
    }
    for (i = 0U; count == UINT32_MAX || i < count; ++i) {
        uint8_t value = ld8(source + i);
        if (!value) {
            st8(dest + i, 0U);
            c->eax = 0U;
            host_cdecl_return(c);
            return;
        }
        if (i + 1U >= capacity) {
            if (count == UINT32_MAX) {
                st8(dest + capacity - 1U, 0U);
                c->eax = HOST_STRUNCATE;
            } else {
                st8(dest, 0U);
                c->eax = ERANGE;
            }
            host_cdecl_return(c);
            return;
        }
        st8(dest + i, value);
    }
    st8(dest + i, 0U);
    c->eax = 0U;
    host_cdecl_return(c);
}

static void host_strcat_s(CPU *__restrict c)
{
    uint32_t dest = host_arg(c, 0);
    uint32_t capacity = host_arg(c, 1);
    uint32_t source = host_arg(c, 2);
    uint32_t used = 0U, i = 0U;

    if (!dest || !capacity) {
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    if (!source) {
        st8(dest, 0U);
        c->eax = EINVAL;
        host_cdecl_return(c);
        return;
    }
    while (used < capacity && ld8(dest + used)) ++used;
    if (used == capacity) {
        st8(dest, 0U);
        c->eax = ERANGE;
        host_cdecl_return(c);
        return;
    }
    for (;;) {
        uint8_t value = ld8(source + i++);
        if (used >= capacity - 1U && value) {
            st8(dest, 0U);
            c->eax = ERANGE;
            host_cdecl_return(c);
            return;
        }
        st8(dest + used++, value);
        if (!value) break;
    }
    c->eax = 0U;
    host_cdecl_return(c);
}

/* UCRT signature: options (u64), FILE*, format, locale, guest va_list. */
static void host_stdio_common_vfprintf(CPU *__restrict c)
{
    FILE *stream = (FILE *)(uintptr_t)host_arg(c, 2);
    uint32_t format = host_arg(c, 3);
    uint32_t arguments = host_arg(c, 5);
    host_format_output out;
    (void)host_arg(c, 0);
    (void)host_arg(c, 1);
    (void)host_arg(c, 4);
    if (!stream || !format) {
        c->eax = UINT32_MAX;
        host_cdecl_return(c);
        return;
    }
    out.buffer = 0U;
    out.limit = 0U;
    out.written = 0U;
    out.stream = stream;
    out.failed = 0;
    if (!host_stdio_format(c, format, arguments, &out)) return;
    c->eax = out.failed ? UINT32_MAX : out.written;
    host_cdecl_return(c);
}

/* `BOOL WINAPI IsDebuggerPresent(void)` -- stdcall, no arguments.  Answer no:
 * saying yes routes the CRT into debugger-only reporting paths that we have
 * not built and that do not exist on the target. */
static void host_IsDebuggerPresent(CPU *__restrict c)
{
    c->eax = 0U;
    host_stdcall_return(c, 0U);
}

/* `LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(...)` --
 * stdcall, one argument.  Recorded and not installed: we have no structured
 * exception machinery yet, and quietly accepting the registration is right --
 * the guest only stores the previous value. */
static uint32_t s_unhandled_filter;

static void host_SetUnhandledExceptionFilter(CPU *__restrict c)
{
    uint32_t previous = s_unhandled_filter;
    s_unhandled_filter = host_arg(c, 0);
    c->eax = previous;
    host_stdcall_return(c, 4U);
}

/* `HMODULE WINAPI GetModuleHandleW(LPCWSTR)` -- stdcall, one argument.  A
 * NULL name means "this process's image", and the honest answer is the base
 * we relocated the guest to.  Any other name faults loudly rather than
 * returning NULL, because a silent NULL here becomes a null-dereference
 * hundreds of instructions later. */
/* Synthetic module handles.  Distinct values so GetProcAddress can tell which
 * module is being asked about; deliberately outside the guest image so they
 * can never be mistaken for a code address by guest_lookup. */
#define HOST_HMODULE_KERNEL32  0x7F000001u
#define HOST_HMODULE_OPENGL32  0x7F000002u
#define HOST_HMODULE_DINPUT8   0x7F000003u
#define HOST_HICON_ISAAC      0x7F100065u
#define HOST_HICON_ISAAC_ALT  0x7F100068u
#define HOST_HICON_SMALL_BIAS 0x00010000u

static void host_copy_ascii(uint32_t addr, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; ++i) {
        uint8_t ch = ld8(addr + (uint32_t)i);
        if (!ch) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

/* Copy a guest UTF-16 string into an ASCII buffer.  Module names are ASCII;
 * anything else is truncated rather than guessed at, and the caller compares
 * exactly. */
static void host_wide_to_ascii(uint32_t addr, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; ++i) {
        uint16_t w = ld16(addr + (uint32_t)(i * 2));
        if (!w) break;
        out[i] = (w < 0x80) ? (char)w : '?';
    }
    out[i] = '\0';
}

/* `HMODULE WINAPI GetModuleHandleW(LPCWSTR)` -- stdcall.
 *
 * NULL means "this process's image", so the honest answer is our relocated
 * base.  The interesting case is measured, not assumed: the CRT probes
 *
 *     GetModuleHandleW(L"api-ms-win-core-synch-l1-2-0.dll")   // WaitOnAddress
 *     GetModuleHandleW(L"kernel32.dll")                        // fallback
 *
 * (both strings sit next to each other at RVA 0x608000).  For the api-ms-win-*
 * sets **NULL is the correct answer, not a stub**: we are emulating a system
 * where that API set is absent, and the CRT then takes its own fallback path.
 * Returning a handle there would promise functions we do not have.
 *
 * Anything outside those two cases still faults by name, because a silent NULL
 * for a module the guest genuinely needs turns into a null dereference much
 * later. */
static void host_GetModuleHandleW(CPU *__restrict c)
{
    uint32_t name = host_arg(c, 0);
    char buf[64];

    if (!name) {
        c->eax = (uint32_t)GUEST_IMAGE_BASE;   /* guest.h owns this constant */
        host_stdcall_return(c, 4U);
        return;
    }

    host_wide_to_ascii(name, buf, sizeof buf);
    if (_stricmp(buf, "kernel32.dll") == 0) {
        c->eax = HOST_HMODULE_KERNEL32;
    } else if (strncmp(buf, "api-ms-win-", 11) == 0) {
        c->eax = 0U;                            /* absent, and that is correct */
    } else {
        guest_fault(c, name, "GetModuleHandleW for an unexpected module");
        return;
    }
    host_stdcall_return(c, 4U);
}

/* Narrow sibling of the policy above.  The measured GLFW cleanup path asks
 * only for GetModuleHandleA(NULL), which is the relocated guest image.  A
 * kernel32 name is supported for parity with the CRT's W call; any other
 * module remains loud until a real caller gives it semantics. */
static void host_GetModuleHandleA(CPU *__restrict c)
{
    uint32_t name = host_arg(c, 0);
    char buf[64];

    if (!name) {
        c->eax = (uint32_t)GUEST_IMAGE_BASE;
    } else {
        host_copy_ascii(name, buf, sizeof buf);
        if (_stricmp(buf, "kernel32.dll") == 0)
            c->eax = HOST_HMODULE_KERNEL32;
        else {
            guest_fault(c, name, "GetModuleHandleA for an unexpected module");
            return;
        }
    }
    host_stdcall_return(c, 4U);
}

/* Failed GLFW initialisation still executes its Win32 decoration cleanup:
 * it loads resource icon 101 and then calls the three Long APIs with HWND=0.
 * Do not pass these through to USER32 or pretend NULL is a target window.
 * Return a stable opaque icon token for the exact resource request, while the
 * invalid-window operations reproduce Win32's zero result and no state.
 * A future CreateWindowExA shim must introduce a real virtual HWND and keyed
 * style/class state before non-NULL handles are accepted here. */
static void host_LoadIconA(CPU *__restrict c)
{
    uint32_t instance = host_arg(c, 0);
    uint32_t name = host_arg(c, 1);
    if (instance != GUEST_IMAGE_BASE || name != 101U) {
        guest_fault(c, name, "LoadIconA outside the measured Isaac resource");
        return;
    }
    c->eax = HOST_HICON_ISAAC;
    host_stdcall_return(c, 8U);
}

/* The menu constructor decorates the GLFW window with RT_GROUP_ICON 101 or
 * 104 via LoadImageA, then sends WM_SETICON twice.  The module handle is the
 * relocated guest image, not a native HMODULE, and PC KAGE deliberately has
 * no guest GLFW HWND.  Keep this cosmetic boundary target-owned: validate the
 * exact icon requests and return stable opaque tokens rather than handing
 * guest handles to USER32. */
static int host_is_isaac_icon_token(uint32_t token, uint32_t icon_kind)
{
    uint32_t bias = icon_kind == ICON_SMALL ? HOST_HICON_SMALL_BIAS : 0U;
    return token == HOST_HICON_ISAAC + bias ||
           token == HOST_HICON_ISAAC_ALT + bias;
}

static void host_LoadImageA(CPU *__restrict c)
{
    uint32_t instance = host_arg(c, 0);
    uint32_t name = host_arg(c, 1);
    uint32_t type = host_arg(c, 2);
    int width = (int32_t)host_arg(c, 3);
    int height = (int32_t)host_arg(c, 4);
    uint32_t flags = host_arg(c, 5);
    int large_width, large_height, small_width, small_height;
    int is_large, is_small;
    uint32_t token;

    if (!host_query_system_metric(c, SM_CXICON, &large_width) ||
        !host_query_system_metric(c, SM_CYICON, &large_height) ||
        !host_query_system_metric(c, SM_CXSMICON, &small_width) ||
        !host_query_system_metric(c, SM_CYSMICON, &small_height))
        return;
    is_large = width == large_width && height == large_height;
    is_small = width == small_width && height == small_height;

    if (instance != GUEST_IMAGE_BASE || (name != 101U && name != 104U) ||
        type != IMAGE_ICON || flags != 0U || (!is_large && !is_small)) {
        guest_fault(c, name, "LoadImageA outside the measured Isaac icons");
        return;
    }
    token = name == 101U ? HOST_HICON_ISAAC : HOST_HICON_ISAAC_ALT;
    if (!is_large && is_small)
        token += HOST_HICON_SMALL_BIAS;
    c->eax = token;
    host_stdcall_return(c, 24U);
}

static void host_SendMessageA(CPU *__restrict c)
{
    uint32_t window = host_arg(c, 0);
    uint32_t message = host_arg(c, 1);
    uint32_t icon_kind = host_arg(c, 2);
    uint32_t icon = host_arg(c, 3);

    if (window != 0U || message != WM_SETICON || icon_kind > ICON_BIG ||
        !host_is_isaac_icon_token(icon, icon_kind)) {
        guest_fault(c, window, "SendMessageA needs the measured NULL-HWND icon request");
        return;
    }
    c->eax = 0U;
    host_stdcall_return(c, 16U);
}

static void host_SetClassLongA(CPU *__restrict c)
{
    uint32_t window = host_arg(c, 0);
    int32_t index = (int32_t)host_arg(c, 1);
    (void)host_arg(c, 2);
    if (window || index != -14) {              /* GCL_HICON */
        guest_fault(c, window, "SetClassLongA needs a virtual HWND");
        return;
    }
    c->eax = 0U;
    host_stdcall_return(c, 12U);
}

static void host_GetWindowLongA(CPU *__restrict c)
{
    uint32_t window = host_arg(c, 0);
    int32_t index = (int32_t)host_arg(c, 1);
    if (window || index != -16) {              /* GWL_STYLE */
        guest_fault(c, window, "GetWindowLongA needs a virtual HWND");
        return;
    }
    c->eax = 0U;
    host_stdcall_return(c, 8U);
}

static void host_SetWindowLongA(CPU *__restrict c)
{
    uint32_t window = host_arg(c, 0);
    int32_t index = (int32_t)host_arg(c, 1);
    (void)host_arg(c, 2);
    if (window || index != -16) {              /* GWL_STYLE */
        guest_fault(c, window, "SetWindowLongA needs a virtual HWND");
        return;
    }
    c->eax = 0U;
    host_stdcall_return(c, 12U);
}

/* The UCRT accepts this stdcall TLS-dtor dispatcher once.  A second
 * registration calls terminate in the reference implementation; keep it loud
 * here without pretending that overwriting the first callback is harmless. */
static void host_register_tls_atexit(CPU *__restrict c)
{
    uint32_t fn = host_arg(c, 0);
    if (s_tls_atexit) {
        guest_fault(c, fn, "TLS exit callback registered twice");
        return;
    }
    s_tls_atexit = fn;
    host_cdecl_return(c);
}

/* `char **__cdecl _get_initial_narrow_environment(void)` -- an empty, properly
 * terminated environment.  The Vita has no environment block, so empty is not
 * a placeholder, it is the final answer. */
static void host_get_initial_narrow_environment(CPU *__restrict c)
{
    host_startup_init();
    c->eax = s_env_ptr;
    host_cdecl_return(c);
}

/* `char ***__cdecl __p___argv(void)` and `int *__cdecl __p___argc(void)` --
 * these hand back the ADDRESS of the CRT's argv/argc variables, so the guest
 * can read (and in principle write) them. */
static void host_p_argv(CPU *__restrict c)
{
    host_startup_init();
    c->eax = host_guest_ptr(&s_argv_ptr);
    host_cdecl_return(c);
}

static void host_p_argc(CPU *__restrict c)
{
    host_startup_init();
    c->eax = host_guest_ptr(&s_argc);
    host_cdecl_return(c);
}

/* `int __CRTDECL _initialize_narrow_environment(void)` -- signature copied
 * from `Windows Kits\10\Include\10.0.26100.0\ucrt\corecrt_startup.h`, which
 * notes it is deliberately not `_ACRTIMP` because a link option can disable
 * environment setup entirely.  Returns 0 on success.
 *
 * `pre_c_initialization` calls this behind `_should_initialize_environment()`
 * and treats a failure as fatal, so success is the only safe answer.  The
 * environment itself stays empty; `_get_initial_narrow_environment` above
 * hands out the terminated empty block. */
static void host_initialize_narrow_environment(CPU *__restrict c)
{
    host_startup_init();
    c->eax = 0U;
    host_cdecl_return(c);
}

/* ---- critical sections --------------------------------------------------
 * All stdcall.  A CRITICAL_SECTION lives in guest memory, and since a guest
 * address is a host address in this backend, the real API can initialise and
 * use it directly -- no shadow table, no handle mapping.
 *
 * This is the first shim family that is genuinely PC-only.  The Vita has no
 * CRITICAL_SECTION API; its backend implements the same recursive owner/depth
 * state directly in the caller-provided 24 guest bytes, without a side table
 * or one Vita kernel UID per object.
 *
 * Only `InitializeCriticalSectionAndSpinCount` was reached by measurement.
 * The rest of the family is added with it deliberately: they take the same
 * object, they always appear together, and each is a one-line passthrough. */
static void host_InitializeCriticalSectionAndSpinCount(CPU *__restrict c)
{
    c->eax = (uint32_t)InitializeCriticalSectionAndSpinCount(
        (LPCRITICAL_SECTION)(uintptr_t)host_arg(c, 0), host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

static void host_InitializeCriticalSection(CPU *__restrict c)
{
    InitializeCriticalSection((LPCRITICAL_SECTION)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_EnterCriticalSection(CPU *__restrict c)
{
    EnterCriticalSection((LPCRITICAL_SECTION)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_TryEnterCriticalSection(CPU *__restrict c)
{
    c->eax = (uint32_t)TryEnterCriticalSection(
        (LPCRITICAL_SECTION)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_LeaveCriticalSection(CPU *__restrict c)
{
    LeaveCriticalSection((LPCRITICAL_SECTION)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_DeleteCriticalSection(CPU *__restrict c)
{
    DeleteCriticalSection((LPCRITICAL_SECTION)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

/* Dynamic platform entry points cannot be returned as native FARPROC values.
 * The generated bridge owns exact x86/APIENTRY decoding for the measured,
 * typed game surface and returns stable name-derived guest tokens. */
unsigned g_host_dynamic_calls;
int g_guest_gl_inventory_mode;

/* Distinct from generated 0x7e...... GL tokens and 0x7f...... module handles.
 * This token represents the typed WGL resolver itself, not an arbitrary
 * native FARPROC. */
#define HOST_DYNAMIC_WGL_GET_PROC_ADDRESS UINT32_C(0x7d100001)
#define HOST_DYNAMIC_DIRECTINPUT8CREATE    UINT32_C(0x7d100002)

static void host_wglGetProcAddress(CPU *__restrict c);
static void host_DirectInput8Create(CPU *__restrict c);

static guest_gl_addr host_inventory_glGetString(guest_gl_enum name)
{
    static const char version[] = "OpenGL ES 2.0 VitaGL inventory";
    static const char shading[] = "OpenGL ES GLSL ES 1.00";
    static const char vendor[] = "repentogxm";
    static const char renderer[] = "headless resolver inventory";
    static const char extensions[] = "";
    const char *result = NULL;

    /* With no current GLFW/WGL context, real glGetString returns NULL.  The
     * explicit inventory mode deliberately supplies Vita-shaped capability
     * strings only to discover the next dynamically required symbol; it is not
     * a rendering backend and is never enabled by the normal entry probe. */
    if (g_guest_gl_inventory_mode) {
        if (name == 0x1F02U) result = version;       /* GL_VERSION */
        else if (name == 0x8B8CU) result = shading;  /* GL_SHADING_LANGUAGE_VERSION */
        else if (name == 0x1F00U) result = vendor;   /* GL_VENDOR */
        else if (name == 0x1F01U) result = renderer; /* GL_RENDERER */
        else if (name == 0x1F03U) result = extensions;
    }
    return host_guest_ptr(result);
}

static uint32_t host_dynamic_address(const char *module, const char *name)
{
    if (!name)
        return 0U;
    if (strcmp(module, "OPENGL32") == 0) {
        if (strcmp(name, "wglGetProcAddress") == 0)
            return HOST_DYNAMIC_WGL_GET_PROC_ADDRESS;
        return guest_gl_resolve(name);
    }
    if (strcmp(module, "DINPUT8") == 0 &&
        strcmp(name, "DirectInput8Create") == 0)
        return HOST_DYNAMIC_DIRECTINPUT8CREATE;
    return 0U;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    /* Inventory owns only this one dispatch while the explicit mode flag is
     * set.  It deliberately does not call guest_gl_install_backend(): doing so
     * would overwrite a real PC/vitaGL table, and the bridge has no reason to
     * make that process-global change for a diagnostic query.  Turning the
     * mode off therefore needs no restore step and exposes the exact backend
     * that was installed before inventory began. */
    if (g_guest_gl_inventory_mode &&
        token == guest_gl_resolve("glGetString")) {
        c->eax = host_inventory_glGetString(host_arg(c, 0));
        host_stdcall_return(c, 4U);
        ++g_host_dynamic_calls;
        return 1;
    }
    if (token == HOST_DYNAMIC_WGL_GET_PROC_ADDRESS) {
        host_wglGetProcAddress(c);
        ++g_host_dynamic_calls;
        return 1;
    }
    if (token == HOST_DYNAMIC_DIRECTINPUT8CREATE) {
        host_DirectInput8Create(c);
        ++g_host_dynamic_calls;
        return 1;
    }
    if (guest_gl_dispatch(c, token)) {
        ++g_host_dynamic_calls;
        return 1;
    }
    return 0;
}

/* `PROC WINAPI wglGetProcAddress(LPCSTR)` -- x86 stdcall, one argument.
 *
 * The imported WGL resolver is a second route to exactly the same dynamic GL
 * surface as KERNEL32!GetProcAddress.  It must never return a native FARPROC:
 * generated guest code would treat that host address as an x86 guest target.
 * Resolve a real guest string to the generated stable token; NULL, ordinal-
 * shaped values and names outside the frozen surface are rejected. */
static void host_wglGetProcAddress(CPU *__restrict c)
{
    uint32_t name = host_arg(c, 0);
    char requested[64] = { 0 };

    if (name && (name >> 16) != 0U)
        host_copy_ascii(name, requested, sizeof requested);
    c->eax = requested[0] ? guest_gl_resolve(requested) : 0U;
    host_stdcall_return(c, 4U);
}

/* `HRESULT WINAPI DirectInput8Create(HINSTANCE,DWORD,REFIID,void**,IUnknown*)`
 * -- x86 stdcall.  Isaac loads this entry dynamically but does not check the
 * resolver result before calling it.  Returning a native FARPROC would be
 * unsound, and the game's own failure branch is complete, so model an absent
 * DirectInput backend explicitly: validate the measured ABI/IID, clear the
 * output interface, and return E_FAIL.  The high-level input seam can replace
 * this discovery fallback later without exposing COM objects to guest code. */
static void host_DirectInput8Create(CPU *__restrict c)
{
    uint32_t instance = host_arg(c, 0);
    uint32_t version = host_arg(c, 1);
    uint32_t iid = host_arg(c, 2);
    uint32_t output = host_arg(c, 3);
    uint32_t outer = host_arg(c, 4);
    int exact = instance == GUEST_IMAGE_BASE && version == 0x0800U && iid &&
                output && !outer &&
                ld32(iid + 0U) == 0xBF798030U &&
                ld32(iid + 4U) == 0x4DA2483AU &&
                ld32(iid + 8U) == 0x645D99AAU &&
                ld32(iid + 12U) == 0x009736EDU;

    if (!exact) {
        guest_fault(c, HOST_DYNAMIC_DIRECTINPUT8CREATE,
                    "DirectInput8Create outside measured KAGE startup ABI");
        return;
    }
    st32(output, 0U);
    c->eax = 0x80004005U;             /* E_FAIL / DIERR_GENERIC */
    host_stdcall_return(c, 20U);
}

/* `FARPROC WINAPI GetProcAddress(HMODULE, LPCSTR)` -- stdcall.
 *
 * Returning NULL is the correct answer for the optional startup probes
 * measured so far (`WaitOnAddress`, `VirtualAlloc2*`, NUMA helpers and their
 * siblings): each call site has an older fallback.  It is not a universal
 * GetProcAddress implementation; a later dynamically required symbol must
 * get an explicit guest-aware shim rather than a native FARPROC.
 *
 * But "correct" and "silent" are different things, and a refusal that leaves
 * no trace is how a null dereference three thousand instructions later gets
 * blamed on the wrong code.  Every request is recorded by name and the list
 * is printed by the entry probe, so a NULL that turns out to matter is
 * visible in the same run that produced it. */
static char     s_proc_log[HOST_PROC_LOG_MAX][48];
unsigned        g_guest_getproc_calls;
unsigned        g_guest_getproc_logged;

const char *guest_host_getproc_name(unsigned i)
{
    return (i < g_guest_getproc_logged) ? s_proc_log[i] : NULL;
}

static void host_GetProcAddress(CPU *__restrict c)
{
    uint32_t module = host_arg(c, 0);
    uint32_t name = host_arg(c, 1);
    char requested[64] = { 0 };
    ++g_guest_getproc_calls;
    if (name && (name >> 16) != 0U)
        host_copy_ascii(name, requested, sizeof requested);
    if (name && g_guest_getproc_logged < HOST_PROC_LOG_MAX) {
        char *slot = s_proc_log[g_guest_getproc_logged++];
        if ((name >> 16) == 0U) {
            (void)snprintf(slot, sizeof s_proc_log[0], "%08x!#%u",
                           module, name);
        } else {
            (void)snprintf(slot, sizeof s_proc_log[0], "%08x!%s",
                           module, requested);
        }
    }
    c->eax = module == HOST_HMODULE_OPENGL32
        ? host_dynamic_address("OPENGL32", requested)
        : module == HOST_HMODULE_DINPUT8
        ? host_dynamic_address("DINPUT8", requested) : 0U;
    host_stdcall_return(c, 8U);
}

/* `HMODULE WINAPI LoadLibraryA(LPCSTR)` and
 * `BOOL WINAPI FreeLibrary(HMODULE)` -- signatures copied together from the
 * installed SDK's `um/libloaderapi.h`.  The measured allocator startup loads
 * `kernelbase.dll`, `ntdll.dll` and `kernel32.dll` only to probe optional
 * entry points through GetProcAddress.  On the 32-bit PC backend a real HMODULE fits the guest
 * register and is safe to pass back to FreeLibrary.  GetProcAddress above
 * still refuses the optional native function pointer: returning one would
 * make generated code dispatch a host address as if it were a guest target.
 * The Vita backend will replace these module handles with synthetic tokens. */
static void host_LoadLibraryA(CPU *__restrict c)
{
    uint32_t name = host_arg(c, 0);
    char buf[64];
    host_copy_ascii(name, buf, sizeof buf);
    if (_stricmp(buf, "OPENGL32") == 0 ||
        _stricmp(buf, "OPENGL32.dll") == 0) {
        c->eax = HOST_HMODULE_OPENGL32;
    } else if (_stricmp(buf, "DINPUT8") == 0 ||
               _stricmp(buf, "DINPUT8.dll") == 0) {
        c->eax = HOST_HMODULE_DINPUT8;
    } else {
        c->eax = (uint32_t)(uintptr_t)LoadLibraryA(
            (LPCSTR)(uintptr_t)name);
    }
    host_stdcall_return(c, 4U);
}

static void host_FreeLibrary(CPU *__restrict c)
{
    uint32_t module = host_arg(c, 0);
    c->eax = (module == HOST_HMODULE_OPENGL32 ||
              module == HOST_HMODULE_DINPUT8) ? 1U :
        (uint32_t)FreeLibrary((HMODULE)(uintptr_t)module);
    host_stdcall_return(c, 4U);
}

/* `HANDLE WINAPI GetStdHandle(DWORD)` -- signature copied from the installed
 * Windows SDK's `um/processenv.h`.  Startup asks for STD_ERROR_HANDLE (-12)
 * and stores the result for a following WriteFile.  On the PC backend the
 * real handle is the correct answer and fits in the 32-bit guest register;
 * the Vita backend will map this pseudo-handle to its log sink instead. */
static void host_GetStdHandle(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)GetStdHandle(host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

/* `VOID WINAPI GetSystemInfo(LPSYSTEM_INFO)` -- signature copied from the
 * installed SDK's `um/sysinfoapi.h`.  Mimalloc consumes the page size,
 * allocation granularity and processor count during startup.  The guest
 * pointer is the API boundary and is directly usable on this PC backend;
 * Vita will fill the same 32-bit SYSTEM_INFO layout from target constants. */
static void host_GetSystemInfo(CPU *__restrict c)
{
    GetSystemInfo((LPSYSTEM_INFO)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

/* Native virtual-memory family used by the statically linked mimalloc OS
 * layer.  The PC harness is a 32-bit process and its current memory contract
 * is guest address == host address, so Win32 itself is the exact allocation
 * ledger: fixed-address commits, MEM_RESET/MEM_DECOMMIT, and an interior
 * VirtualQuery all retain their real semantics.  In particular, mimalloc
 * deliberately tries VirtualFree on an aligned interior pointer, observes
 * ERROR_INVALID_ADDRESS, queries AllocationBase, and releases that base.
 * Replacing any one member with malloc/free would break that sequence.
 *
 * Signatures are copied from memoryapi.h/sysinfoapi.h.  On x86 the native
 * MEMORY_BASIC_INFORMATION is the guest's seven-word (28-byte) structure;
 * every wrapper below owns the original stdcall cleanup.  GetLastError must
 * read the native thread value before doing any other host work. */
static void host_GetLastError(CPU *__restrict c)
{
    DWORD error = GetLastError();
    c->eax = (uint32_t)error;
    host_stdcall_return(c, 0U);
}

static void host_GetLargePageMinimum(CPU *__restrict c)
{
    c->eax = (uint32_t)GetLargePageMinimum();
    host_stdcall_return(c, 0U);
}

static void host_VirtualAlloc(CPU *__restrict c)
{
    LPVOID result = VirtualAlloc(
        (LPVOID)(uintptr_t)host_arg(c, 0),
        (SIZE_T)host_arg(c, 1), host_arg(c, 2), host_arg(c, 3));
    c->eax = (uint32_t)(uintptr_t)result;
    host_stdcall_return(c, 16U);
}

static void host_VirtualFree(CPU *__restrict c)
{
    BOOL result = VirtualFree(
        (LPVOID)(uintptr_t)host_arg(c, 0),
        (SIZE_T)host_arg(c, 1), host_arg(c, 2));
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 12U);
}

static void host_VirtualQuery(CPU *__restrict c)
{
    SIZE_T result = VirtualQuery(
        (LPCVOID)(uintptr_t)host_arg(c, 0),
        (PMEMORY_BASIC_INFORMATION)(uintptr_t)host_arg(c, 1),
        (SIZE_T)host_arg(c, 2));
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 12U);
}

static void host_VirtualUnlock(CPU *__restrict c)
{
    BOOL result = VirtualUnlock(
        (LPVOID)(uintptr_t)host_arg(c, 0), (SIZE_T)host_arg(c, 1));
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 8U);
}

/* `DWORD WINAPI GetEnvironmentVariableA(LPCSTR, LPSTR, DWORD)` -- signature
 * copied from the installed SDK's `um/processenv.h`.  The measured startup
 * calls build names such as `mimalloc_verbose` in a guest stack buffer; the
 * native game sees the process environment too, so the real PC API is the
 * faithful bring-up answer.  Vita will normally return "not present" unless
 * a port-owned configuration source deliberately provides a value. */
static void host_GetEnvironmentVariableA(CPU *__restrict c)
{
    c->eax = GetEnvironmentVariableA(
        (LPCSTR)(uintptr_t)host_arg(c, 0),
        (LPSTR)(uintptr_t)host_arg(c, 1),
        host_arg(c, 2));
    host_stdcall_return(c, 12U);
}

/* Fiber-local storage, signatures copied as one family from the installed
 * SDK's `um/fibersapi.h`.  The native callback pointer cannot be the guest
 * function address: Windows would jump into translated x86 bytes as native
 * code on teardown.  Allocate real per-thread FLS with a NULL native callback
 * and retain the guest callback by index; FlsFree invokes it through the guest
 * dispatcher for the current value before releasing the slot.
 *
 * This is enough for the measured single-thread startup and preserves values
 * for the current native fiber.  It does NOT yet reproduce callback delivery
 * for every fiber at thread/fiber teardown; that belongs in the later guest
 * thread runtime.  The Vita backend can keep the same guest callback table
 * over its own TLS slots. */
static uint32_t s_fls_callbacks[FLS_MAXIMUM_AVAILABLE];

static void host_FlsAlloc(CPU *__restrict c)
{
    uint32_t callback = host_arg(c, 0);
    DWORD index = FlsAlloc(NULL);
    if (index != FLS_OUT_OF_INDEXES) {
        if (index >= FLS_MAXIMUM_AVAILABLE) {
            guest_fault(c, index, "FlsAlloc returned an out-of-range index");
            return;
        }
        s_fls_callbacks[index] = callback;
    }
    c->eax = index;
    host_stdcall_return(c, 4U);
}

static void host_FlsGetValue(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)FlsGetValue(host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_FlsSetValue(CPU *__restrict c)
{
    c->eax = (uint32_t)FlsSetValue(
        host_arg(c, 0), (PVOID)(uintptr_t)host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

static void host_FlsFree(CPU *__restrict c)
{
    DWORD index = host_arg(c, 0);
    uint32_t callback = index < FLS_MAXIMUM_AVAILABLE
        ? s_fls_callbacks[index] : 0U;
    uint32_t value = (uint32_t)(uintptr_t)FlsGetValue(index);
    BOOL result;
    if (callback && value && !call_guest_stdcall1(c, callback, value))
        return;
    result = FlsFree(index);
    if (result && index < FLS_MAXIMUM_AVAILABLE)
        s_fls_callbacks[index] = 0U;
    c->eax = (uint32_t)result;
    host_stdcall_return(c, 4U);
}

/* ---- events, waits and TLS ----------------------------------------------
 * Reached because `GetProcAddress` refused `WaitOnAddress`: the CRT then
 * builds its condition variables out of events, which is the older path and
 * exactly what we asked for by reporting the api-ms-win-core-synch set absent.
 *
 * All stdcall, all straight passthrough on this backend -- a HANDLE fits in
 * 32 bits in a 32-bit process, and a guest pointer is a host pointer.  Added
 * as a family for the same reason as the critical sections: they arrive
 * together and each is one line.
 *
 * None of this survives the port.  On the Vita these become `sceKernelCreate
 * EventFlag` / `sceKernelWaitEventFlag` and the TLS slots become a small
 * fixed array, since there is one guest thread in the current design.  The
 * argument byte counts below are the part that must stay right either way. */
static void host_CreateEventW(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)CreateEventW(
        (LPSECURITY_ATTRIBUTES)(uintptr_t)host_arg(c, 0),
        (BOOL)host_arg(c, 1), (BOOL)host_arg(c, 2),
        (LPCWSTR)(uintptr_t)host_arg(c, 3));
    host_stdcall_return(c, 16U);
}

static void host_CloseHandle(CPU *__restrict c)
{
    c->eax = (uint32_t)CloseHandle((HANDLE)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_SetEvent(CPU *__restrict c)
{
    c->eax = (uint32_t)SetEvent((HANDLE)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_ResetEvent(CPU *__restrict c)
{
    c->eax = (uint32_t)ResetEvent((HANDLE)(uintptr_t)host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_WaitForSingleObject(CPU *__restrict c)
{
    c->eax = WaitForSingleObject((HANDLE)(uintptr_t)host_arg(c, 0),
                                 host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

static void host_WaitForSingleObjectEx(CPU *__restrict c)
{
    c->eax = WaitForSingleObjectEx((HANDLE)(uintptr_t)host_arg(c, 0),
                                   host_arg(c, 1), (BOOL)host_arg(c, 2));
    host_stdcall_return(c, 12U);
}

/* Scheduling pair reached by the main-loop/thread support code.  `Sleep` is
 * the Win32 `VOID WINAPI Sleep(DWORD)` boundary and owns its four-byte
 * stdcall argument.  MSVCP's `_Thrd_yield` is `void __cdecl(void)`; the PC
 * oracle implements that operation with SwitchToThread.  Both are void, so
 * deliberately do not manufacture a value in the guest EAX register. */
static void host_Sleep(CPU *__restrict c)
{
    Sleep(host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_Thrd_yield(CPU *__restrict c)
{
    (void)SwitchToThread();
    host_cdecl_return(c);
}

static void host_TlsAlloc(CPU *__restrict c)
{
    c->eax = TlsAlloc();
    host_stdcall_return(c, 0U);
}

static void host_TlsFree(CPU *__restrict c)
{
    c->eax = (uint32_t)TlsFree(host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_TlsGetValue(CPU *__restrict c)
{
    c->eax = (uint32_t)(uintptr_t)TlsGetValue(host_arg(c, 0));
    host_stdcall_return(c, 4U);
}

static void host_TlsSetValue(CPU *__restrict c)
{
    c->eax = (uint32_t)TlsSetValue(host_arg(c, 0),
                                   (LPVOID)(uintptr_t)host_arg(c, 1));
    host_stdcall_return(c, 8U);
}

/* Keep this table sorted: dispatch is a binary search and startup depends on
 * exact generated DLL!symbol spelling. */
static const host_import_entry s_host_imports[] = {
    { "KERNEL32.dll!CloseHandle",
      host_CloseHandle },
    { "KERNEL32.dll!CreateDirectoryA",
      host_CreateDirectoryA },
    { "KERNEL32.dll!CreateEventW",
      host_CreateEventW },
    { "KERNEL32.dll!DeleteCriticalSection",
      host_DeleteCriticalSection },
    { "KERNEL32.dll!EnterCriticalSection",
      host_EnterCriticalSection },
    { "KERNEL32.dll!FindClose",
      host_FindClose },
    { "KERNEL32.dll!FindFirstFileW",
      host_FindFirstFileW },
    { "KERNEL32.dll!FindNextFileW",
      host_FindNextFileW },
    { "KERNEL32.dll!FlsAlloc",
      host_FlsAlloc },
    { "KERNEL32.dll!FlsFree",
      host_FlsFree },
    { "KERNEL32.dll!FlsGetValue",
      host_FlsGetValue },
    { "KERNEL32.dll!FlsSetValue",
      host_FlsSetValue },
    { "KERNEL32.dll!FreeLibrary",
      host_FreeLibrary },
    { "KERNEL32.dll!GetCurrentDirectoryA",
      host_GetCurrentDirectoryA },
    { "KERNEL32.dll!GetCurrentProcessId",
      host_GetCurrentProcessId },
    { "KERNEL32.dll!GetCurrentThreadId",
      host_GetCurrentThreadId },
    { "KERNEL32.dll!GetEnvironmentVariableA",
      host_GetEnvironmentVariableA },
    { "KERNEL32.dll!GetFileAttributesA",
      host_GetFileAttributesA },
    { "KERNEL32.dll!GetFullPathNameW",
      host_GetFullPathNameW },
    { "KERNEL32.dll!GetLargePageMinimum",
      host_GetLargePageMinimum },
    { "KERNEL32.dll!GetLastError",
      host_GetLastError },
    { "KERNEL32.dll!GetModuleHandleA",
      host_GetModuleHandleA },
    { "KERNEL32.dll!GetModuleHandleW",
      host_GetModuleHandleW },
    { "KERNEL32.dll!GetProcAddress",
      host_GetProcAddress },
    { "KERNEL32.dll!GetStdHandle",
      host_GetStdHandle },
    { "KERNEL32.dll!GetSystemInfo",
      host_GetSystemInfo },
    { "KERNEL32.dll!GetSystemTimeAsFileTime",
      host_GetSystemTimeAsFileTime },
    { "KERNEL32.dll!GlobalAlloc",
      host_GlobalAlloc },
    { "KERNEL32.dll!GlobalLock",
      host_GlobalLock },
    { "KERNEL32.dll!GlobalUnlock",
      host_GlobalUnlock },
    { "KERNEL32.dll!InitializeCriticalSection",
      host_InitializeCriticalSection },
    { "KERNEL32.dll!InitializeCriticalSectionAndSpinCount",
      host_InitializeCriticalSectionAndSpinCount },
    { "KERNEL32.dll!InitializeSListHead",
      host_InitializeSListHead },
    { "KERNEL32.dll!IsDebuggerPresent",
      host_IsDebuggerPresent },
    { "KERNEL32.dll!IsProcessorFeaturePresent",
      host_IsProcessorFeaturePresent },
    { "KERNEL32.dll!LeaveCriticalSection",
      host_LeaveCriticalSection },
    { "KERNEL32.dll!LoadLibraryA",
      host_LoadLibraryA },
    { "KERNEL32.dll!LockFileEx",
      host_LockFileEx },
    { "KERNEL32.dll!QueryPerformanceCounter",
      host_QueryPerformanceCounter },
    { "KERNEL32.dll!QueryPerformanceFrequency",
      host_QueryPerformanceFrequency },
    { "KERNEL32.dll!ResetEvent",
      host_ResetEvent },
    { "KERNEL32.dll!SetEvent",
      host_SetEvent },
    { "KERNEL32.dll!SetThreadExecutionState",
      host_SetThreadExecutionState },
    { "KERNEL32.dll!SetUnhandledExceptionFilter",
      host_SetUnhandledExceptionFilter },
    { "KERNEL32.dll!Sleep",
      host_Sleep },
    { "KERNEL32.dll!TlsAlloc",
      host_TlsAlloc },
    { "KERNEL32.dll!TlsFree",
      host_TlsFree },
    { "KERNEL32.dll!TlsGetValue",
      host_TlsGetValue },
    { "KERNEL32.dll!TlsSetValue",
      host_TlsSetValue },
    { "KERNEL32.dll!TryEnterCriticalSection",
      host_TryEnterCriticalSection },
    { "KERNEL32.dll!UnlockFileEx",
      host_UnlockFileEx },
    { "KERNEL32.dll!VirtualAlloc",
      host_VirtualAlloc },
    { "KERNEL32.dll!VirtualFree",
      host_VirtualFree },
    { "KERNEL32.dll!VirtualQuery",
      host_VirtualQuery },
    { "KERNEL32.dll!VirtualUnlock",
      host_VirtualUnlock },
    { "KERNEL32.dll!WaitForSingleObject",
      host_WaitForSingleObject },
    { "KERNEL32.dll!WaitForSingleObjectEx",
      host_WaitForSingleObjectEx },
    { "MSVCP140.dll!_Thrd_yield",
      host_Thrd_yield },
    { "OPENGL32.dll!wglGetProcAddress",
      host_wglGetProcAddress },
    { "USER32.dll!CloseClipboard",
      host_CloseClipboard },
    { "USER32.dll!EmptyClipboard",
      host_EmptyClipboard },
    { "USER32.dll!GetClipboardData",
      host_GetClipboardData },
    { "USER32.dll!GetSystemMetrics",
      host_GetSystemMetrics },
    { "USER32.dll!GetWindowLongA",
      host_GetWindowLongA },
    { "USER32.dll!LoadIconA",
      host_LoadIconA },
    { "USER32.dll!LoadImageA",
      host_LoadImageA },
    { "USER32.dll!OpenClipboard",
      host_OpenClipboard },
    { "USER32.dll!SendMessageA",
      host_SendMessageA },
    { "USER32.dll!SetClassLongA",
      host_SetClassLongA },
    { "USER32.dll!SetClipboardData",
      host_SetClipboardData },
    { "USER32.dll!SetWindowLongA",
      host_SetWindowLongA },
    { "USER32.dll!SystemParametersInfoA",
      host_SystemParametersInfoA },
    { "VCRUNTIME140.dll!__RTDynamicCast",
      host_RTDynamicCast },
    { "VCRUNTIME140.dll!longjmp",
      host_longjmp },
    { "VCRUNTIME140.dll!memchr",
      host_memchr },
    { "VCRUNTIME140.dll!memcpy",
      host_memcpy },
    { "VCRUNTIME140.dll!memmove",
      host_memmove },
    { "VCRUNTIME140.dll!memset",
      host_memset },
    { "VCRUNTIME140.dll!strchr",
      host_strchr },
    { "VCRUNTIME140.dll!strstr",
      host_strstr },
    { "WINMM.dll!timeBeginPeriod",
      host_timeBeginPeriod },
    { "WINMM.dll!timeEndPeriod",
      host_timeEndPeriod },
    { "WINMM.dll!timeGetDevCaps",
      host_timeGetDevCaps },
    { "WINMM.dll!timeGetTime",
      host_timeGetTime },
    { "api-ms-win-crt-convert-l1-1-0.dll!atof",
      host_atof },
    { "api-ms-win-crt-convert-l1-1-0.dll!atoi",
      host_atoi },
    { "api-ms-win-crt-convert-l1-1-0.dll!mbstowcs_s",
      host_mbstowcs_s },
    { "api-ms-win-crt-convert-l1-1-0.dll!wcstombs_s",
      host_wcstombs_s },
    { "api-ms-win-crt-environment-l1-1-0.dll!getenv",
      host_getenv },
    { "api-ms-win-crt-filesystem-l1-1-0.dll!_access",
      host_access },
    { "api-ms-win-crt-filesystem-l1-1-0.dll!remove",
      host_remove },
    { "api-ms-win-crt-heap-l1-1-0.dll!_set_new_mode",
      host_set_new_mode },
    { "api-ms-win-crt-heap-l1-1-0.dll!calloc",
      host_calloc },
    { "api-ms-win-crt-heap-l1-1-0.dll!free",
      host_free },
    { "api-ms-win-crt-heap-l1-1-0.dll!malloc",
      host_malloc },
    { "api-ms-win-crt-heap-l1-1-0.dll!realloc",
      host_realloc },
    { "api-ms-win-crt-locale-l1-1-0.dll!_configthreadlocale",
      host_configthreadlocale },
    { "api-ms-win-crt-math-l1-1-0.dll!_CIatan2",
      host_CIatan2 },
    { "api-ms-win-crt-math-l1-1-0.dll!_CIfmod",
      host_CIfmod },
    { "api-ms-win-crt-math-l1-1-0.dll!_fdclass",
      host_fdclass },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_acos_precise",
      host_libm_sse2_acos_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_asin_precise",
      host_libm_sse2_asin_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_atan_precise",
      host_libm_sse2_atan_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_cos_precise",
      host_libm_sse2_cos_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_exp_precise",
      host_libm_sse2_exp_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_log10_precise",
      host_libm_sse2_log10_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_log_precise",
      host_libm_sse2_log_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_pow_precise",
      host_libm_sse2_pow_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_sin_precise",
      host_libm_sse2_sin_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_sqrt_precise",
      host_libm_sse2_sqrt_precise },
    { "api-ms-win-crt-math-l1-1-0.dll!ceil",
      host_ceil },
    { "api-ms-win-crt-math-l1-1-0.dll!floor",
      host_floor },
    { "api-ms-win-crt-math-l1-1-0.dll!nextafterf",
      host_nextafterf },
    { "api-ms-win-crt-runtime-l1-1-0.dll!__p___argc",
      host_p_argc },
    { "api-ms-win-crt-runtime-l1-1-0.dll!__p___argv",
      host_p_argv },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_configure_narrow_argv",
      host_configure_narrow_argv },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_controlfp_s",
      host_controlfp_s },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_crt_atexit",
      host_crt_atexit },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_errno",
      host_errno },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_exit",
      host__exit },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_get_initial_narrow_environment",
      host_get_initial_narrow_environment },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_initialize_narrow_environment",
      host_initialize_narrow_environment },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_initterm",
      host_initterm },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_initterm_e",
      host_initterm_e },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_register_thread_local_exe_atexit_callback",
      host_register_tls_atexit },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_set_app_type",
      host_set_app_type },
    { "api-ms-win-crt-runtime-l1-1-0.dll!_set_errno",
      host_set_errno },
    { "api-ms-win-crt-runtime-l1-1-0.dll!exit",
      host_exit },
    { "api-ms-win-crt-stdio-l1-1-0.dll!__acrt_iob_func",
      host_acrt_iob_func },
    { "api-ms-win-crt-stdio-l1-1-0.dll!__p__commode",
      host_p_commode },
    { "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vfprintf",
      host_stdio_common_vfprintf },
    { "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vsprintf",
      host_stdio_common_vsprintf },
    { "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vsprintf_s",
      host_stdio_common_vsprintf_s },
    { "api-ms-win-crt-stdio-l1-1-0.dll!__stdio_common_vsscanf",
      host_stdio_common_vsscanf },
    { "api-ms-win-crt-stdio-l1-1-0.dll!_fileno",
      host_fileno },
    { "api-ms-win-crt-stdio-l1-1-0.dll!_get_osfhandle",
      host_get_osfhandle },
    { "api-ms-win-crt-stdio-l1-1-0.dll!_set_fmode",
      host_set_fmode },
    { "api-ms-win-crt-stdio-l1-1-0.dll!fclose",
      host_fclose },
    { "api-ms-win-crt-stdio-l1-1-0.dll!fflush",
      host_fflush },
    { "api-ms-win-crt-stdio-l1-1-0.dll!fopen",
      host_fopen },
    { "api-ms-win-crt-stdio-l1-1-0.dll!fread",
      host_fread },
    { "api-ms-win-crt-stdio-l1-1-0.dll!fseek",
      host_fseek },
    { "api-ms-win-crt-stdio-l1-1-0.dll!ftell",
      host_ftell },
    { "api-ms-win-crt-stdio-l1-1-0.dll!fwrite",
      host_fwrite },
    { "api-ms-win-crt-string-l1-1-0.dll!_strdup",
      host_strdup },
    { "api-ms-win-crt-string-l1-1-0.dll!_strnicmp",
      host_strnicmp },
    { "api-ms-win-crt-string-l1-1-0.dll!isdigit",
      host_isdigit },
    { "api-ms-win-crt-string-l1-1-0.dll!ispunct",
      host_ispunct },
    { "api-ms-win-crt-string-l1-1-0.dll!isspace",
      host_isspace },
    { "api-ms-win-crt-string-l1-1-0.dll!iswspace",
      host_iswspace },
    { "api-ms-win-crt-string-l1-1-0.dll!strcat_s",
      host_strcat_s },
    { "api-ms-win-crt-string-l1-1-0.dll!strcpy_s",
      host_strcpy_s },
    { "api-ms-win-crt-string-l1-1-0.dll!strncmp",
      host_strncmp },
    { "api-ms-win-crt-string-l1-1-0.dll!strncpy",
      host_strncpy },
    { "api-ms-win-crt-string-l1-1-0.dll!strncpy_s",
      host_strncpy_s },
    { "api-ms-win-crt-string-l1-1-0.dll!strpbrk",
      host_strpbrk },
    { "api-ms-win-crt-string-l1-1-0.dll!tolower",
      host_tolower },
    { "api-ms-win-crt-string-l1-1-0.dll!toupper",
      host_toupper },
    { "api-ms-win-crt-time-l1-1-0.dll!_gmtime64",
      host_gmtime64 },
    { "api-ms-win-crt-time-l1-1-0.dll!_localtime64",
      host_localtime64 },
    { "api-ms-win-crt-time-l1-1-0.dll!_mkgmtime64",
      host_mkgmtime64 },
    { "api-ms-win-crt-time-l1-1-0.dll!_time64",
      host_time64 },
    { "api-ms-win-crt-time-l1-1-0.dll!strftime",
      host_strftime },
    { "api-ms-win-crt-utility-l1-1-0.dll!qsort",
      host_qsort },
    { "ole32.dll!CoInitialize",
      host_CoInitialize },
    { "ole32.dll!CoInitializeEx",
      host_CoInitializeEx },
    { "ole32.dll!CoUninitialize",
      host_CoUninitialize },
    { "steam_api.dll!SteamAPI_Init",
      host_SteamAPI_Init },
    { "steam_api.dll!SteamAPI_RegisterCallResult",
      host_SteamAPI_RegisterCallResult },
    { "steam_api.dll!SteamAPI_RegisterCallback",
      host_SteamAPI_RegisterCallback },
    { "steam_api.dll!SteamAPI_RunCallbacks",
      host_SteamAPI_RunCallbacks },
    { "steam_api.dll!SteamAPI_Shutdown",
      host_SteamAPI_Shutdown },
    { "steam_api.dll!SteamAPI_UnregisterCallResult",
      host_SteamAPI_UnregisterCallResult },
    { "steam_api.dll!SteamAPI_UnregisterCallback",
      host_SteamAPI_UnregisterCallback },
    { "steam_api.dll!SteamInternal_ContextInit",
      host_SteamInternal_ContextInit },
};

int guest_host_import(CPU *__restrict c, const char *name)
{
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)(sizeof s_host_imports / sizeof s_host_imports[0]);

    /* The comment above asks for a sorted table; this checks it.  A table that
     * drifts out of order makes the binary search miss an entry that IS
     * implemented, and the symptom is a named import fault -- which reads as
     * "not written yet" and sends the next person to write it twice. */
    static int order_checked;
    if (!order_checked) {
        uint32_t i;
        for (i = 1; i < hi; ++i) {
            if (strcmp(s_host_imports[i - 1].name, s_host_imports[i].name) >= 0) {
                guest_fault(c, i, "host import table is not sorted");
                return 1;   /* leave order_checked clear: a check that latches
                             * on its own failure makes every later call blind,
                             * which is the opposite of what a guard is for */
            }
        }
        order_checked = 1;
    }
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2U;
        int order = strcmp(s_host_imports[mid].name, name);
        if (!order) {
            g_host_import_calls++;
            s_host_imports[mid].fn(c);
            return 1;
        }
        if (order < 0) lo = mid + 1U; else hi = mid;
    }
    return 0;
}
