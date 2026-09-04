/* Portable x86 ABI layer for the first Vita synchronization frontier. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_sync.h"
#include "host_vita_import_id.h"
#include "vita_sync_services.h"
#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
#include "host_vita_sync_fastpath.h"

/* Armed by the first bound CPU; see host_vita_sync_fastpath.h. */
uint32_t g_isaac_vita_sync_inline_owner = ISAAC_VITA_SYNC_INLINE_OWNER_NONE;
#endif

typedef void (*vita_sync_import_fn)(CPU *__restrict);

typedef struct vita_sync_import_entry {
    const char *name;
    vita_sync_import_fn fn;
} vita_sync_import_entry;

/* CreateEventW has one measured caller and produces the one event consumed by
 * the CRT exit callback.  Keep ownership separate from raw Vita UIDs so a
 * thread/mutex/mapping handle can never be deleted as an event by accident. */
static int32_t s_owned_event;

void isaac_vita_sync_bind_current_thread(CPU *__restrict c)
{
    int32_t thread_id;

    if (!c || c->vita_sync_thread_id)
        return;
    thread_id = isaac_vita_sync_current_thread_id();
    if (thread_id > 0) {
        c->vita_sync_thread_id = (uint32_t)thread_id;
#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
        isaac_vita_sync_inline_note_bound((uint32_t)thread_id);
#endif
    }
}

static int32_t vita_sync_bound_thread_id(const CPU *__restrict c)
{
    int32_t thread_id = (int32_t)c->vita_sync_thread_id;

    return thread_id > 0 ? thread_id : 0;
}

static uint32_t vita_sync_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_sync_stdcall_return(CPU *__restrict c,
                                     uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void vita_sync_fault(CPU *__restrict c, uint32_t address,
                            const char *what)
{
    guest_fault(c, address, what);
}

static int vita_sync_cs_boundary(CPU *__restrict c, uint32_t address)
{
    if (!address || (address & 3U) ||
        address > UINT32_MAX - (ISAAC_VITA_SYNC_CS_SIZE - 1U)) {
        vita_sync_fault(c, address, "critical section address is invalid");
        return 0;
    }
    G_CHK(address, ISAAC_VITA_SYNC_CS_SIZE, 1);
    return 1;
}

static int vita_sync_cs_validate(CPU *__restrict c, uint32_t address)
{
    int32_t value;

#if defined(ISAAC_VITA_SYNC_INLINE_FASTPATH)
    /* Every owner/depth operation (Enter/Leave/TryEnter/Delete) validates
     * first, so a second CPU identity is noticed before it can touch words. */
    isaac_vita_sync_inline_note_endpoint(c);
#endif
    if (!vita_sync_cs_boundary(c, address))
        return 0;
    if (ld32(address) != ISAAC_VITA_SYNC_CS_MAGIC) {
        vita_sync_fault(c, address, "critical section is not initialized");
        return 0;
    }
    value = (int32_t)ld32(address + 4U);
    if (!isaac_vita_sync_is_userspace_mutex(value) ||
        ld32(address + 12U) != ISAAC_VITA_SYNC_CS_VERSION) {
        vita_sync_fault(c, address, "critical section state is corrupt");
        return 0;
    }
    return 1;
}

static int vita_sync_cs_initialize_at(CPU *__restrict c, uint32_t address,
                                      uint32_t spin_count,
                                      uint32_t failure_iat_rva)
{
    int32_t uid;

    if (!vita_sync_cs_boundary(c, address))
        return 0;
    if (ld32(address) == ISAAC_VITA_SYNC_CS_MAGIC) {
        vita_sync_fault(c, address, "critical section initialized twice");
        return 0;
    }

    /* Obtain the fixed embedded-backend marker first.  Until that succeeds,
     * arbitrary pre-init guest bytes remain byte-for-byte unchanged and
     * cannot resemble valid state. */
    uid = isaac_vita_sync_create_recursive_mutex();
    if (uid <= 0) {
        isaac_vita_sync_log_failure("VitaEmbeddedMutexCreate", uid);
        vita_sync_fault(c, GUEST_IMAGE_BASE + failure_iat_rva,
                        "Vita recursive mutex creation failed");
        return 0;
    }

    st32(address + 4U, (uint32_t)uid);
    st32(address + 8U, spin_count);
    st32(address + 12U, ISAAC_VITA_SYNC_CS_VERSION);
    st32(address + 16U, 0U);
    st32(address + 20U, 0U);
    st32(address, ISAAC_VITA_SYNC_CS_MAGIC); /* validity marker written last */
    return 1;
}

int isaac_vita_sync_cs_initialize(CPU *__restrict c, uint32_t address,
                                  uint32_t spin_count)
{
    return vita_sync_cs_initialize_at(c, address, spin_count,
                                      ISAAC_VITA_SYNC_INIT_CS_IAT_RVA);
}

int isaac_vita_sync_cs_initialize_plain(CPU *__restrict c,
                                        uint32_t address)
{
    return vita_sync_cs_initialize_at(c, address, 0U,
                                      ISAAC_VITA_SYNC_PLAIN_CS_IAT_RVA);
}

int isaac_vita_sync_cs_enter(CPU *__restrict c, uint32_t address)
{
    int32_t thread_id;
    int result;

    if (!vita_sync_cs_validate(c, address))
        return 0;
    thread_id = vita_sync_bound_thread_id(c);
    if (thread_id > 0) {
        result = isaac_vita_sync_lock_userspace_mutex_for_thread(
            (uint32_t *)(uintptr_t)(address + 16U),
            (uint32_t *)(uintptr_t)(address + 20U), thread_id);
    } else {
        result = isaac_vita_sync_lock_userspace_mutex(
            (uint32_t *)(uintptr_t)(address + 16U),
            (uint32_t *)(uintptr_t)(address + 20U));
    }
    if (result < 0) {
        isaac_vita_sync_log_failure("VitaEmbeddedMutexLock", result);
        vita_sync_fault(c, address, "Vita recursive mutex lock failed");
        return 0;
    }
    return 1;
}

int isaac_vita_sync_cs_leave(CPU *__restrict c, uint32_t address)
{
    int32_t thread_id;
    int result;

    if (!vita_sync_cs_validate(c, address))
        return 0;
    thread_id = vita_sync_bound_thread_id(c);
    if (thread_id > 0) {
        result = isaac_vita_sync_unlock_userspace_mutex_for_thread(
            (uint32_t *)(uintptr_t)(address + 16U),
            (uint32_t *)(uintptr_t)(address + 20U), thread_id);
    } else {
        result = isaac_vita_sync_unlock_userspace_mutex(
            (uint32_t *)(uintptr_t)(address + 16U),
            (uint32_t *)(uintptr_t)(address + 20U));
    }
    if (result < 0) {
        isaac_vita_sync_log_failure("VitaEmbeddedMutexUnlock", result);
        vita_sync_fault(c, address, "Vita recursive mutex unlock failed");
        return 0;
    }
    return 1;
}

int isaac_vita_sync_cs_delete(CPU *__restrict c, uint32_t address)
{
    int result;
    uint32_t offset;

    if (!vita_sync_cs_validate(c, address))
        return 0;
    result = isaac_vita_sync_delete_userspace_mutex(
        (uint32_t *)(uintptr_t)(address + 16U),
        (uint32_t *)(uintptr_t)(address + 20U));
    if (result < 0) {
        isaac_vita_sync_log_failure("VitaEmbeddedMutexDelete", result);
        vita_sync_fault(c, address, "Vita recursive mutex deletion failed");
        return 0;
    }

    /* Invalidate first.  No observer can treat the remaining state as live. */
    st32(address, 0U);
    for (offset = 4U; offset < ISAAC_VITA_SYNC_CS_SIZE; offset += 4U)
        st32(address + offset, 0U);
    return 1;
}

static int vita_sync_ascii_case_equal(const char *left, const char *right)
{
    for (;;) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
}

static int vita_sync_copy_utf16_ascii(CPU *__restrict c, uint32_t address,
                                      char output[64])
{
    uint32_t i;

    if (!address) {
        vita_sync_fault(c, address,
                        "GetModuleHandleW received a null module name");
        return 0;
    }
    for (i = 0U; i < 63U; ++i) {
        uint32_t byte_offset = i * 2U;
        uint16_t value;
        if (address > UINT32_MAX - byte_offset) {
            vita_sync_fault(c, address,
                            "GetModuleHandleW module name wrapped address space");
            return 0;
        }
        value = ld16(address + byte_offset);
        if (!value) {
            output[i] = '\0';
            return 1;
        }
        if (value > 0x7fU) {
            vita_sync_fault(c, address,
                            "GetModuleHandleW module name is not ASCII UTF-16");
            return 0;
        }
        output[i] = (char)value;
    }
    output[63] = '\0';
    vita_sync_fault(c, address,
                    "GetModuleHandleW module name exceeds 63 characters");
    return 0;
}

static int vita_sync_copy_ascii(CPU *__restrict c, uint32_t address,
                                char output[64])
{
    uint32_t i;

    if (!address || (address >> 16U) == 0U) {
        vita_sync_fault(c, address,
                        "GetProcAddress received null or ordinal name");
        return 0;
    }
    for (i = 0U; i < 63U; ++i) {
        uint8_t value;
        if (address > UINT32_MAX - i) {
            vita_sync_fault(c, address,
                            "GetProcAddress name wrapped address space");
            return 0;
        }
        value = ld8(address + i);
        if (!value) {
            output[i] = '\0';
            return 1;
        }
        if (value > 0x7fU) {
            vita_sync_fault(c, address,
                            "GetProcAddress name is not ASCII");
            return 0;
        }
        output[i] = (char)value;
    }
    output[63] = '\0';
    vita_sync_fault(c, address,
                    "GetProcAddress name exceeds 63 characters");
    return 0;
}

static int vita_sync_copy_module_ascii(CPU *__restrict c, uint32_t address,
                                       char output[64])
{
    uint32_t i;

    for (i = 0U; i < 63U; ++i) {
        uint8_t value;
        if (address > UINT32_MAX - i) {
            vita_sync_fault(c, address,
                            "GetModuleHandleA module name wrapped address space");
            return 0;
        }
        value = ld8(address + i);
        if (!value) {
            output[i] = '\0';
            return 1;
        }
        if (value > 0x7fU) {
            vita_sync_fault(c, address,
                            "GetModuleHandleA module name is not ASCII");
            return 0;
        }
        output[i] = (char)value;
    }
    output[63] = '\0';
    vita_sync_fault(c, address,
                    "GetModuleHandleA module name exceeds 63 characters");
    return 0;
}

static void vita_sync_InitializeCriticalSectionAndSpinCount(
    CPU *__restrict c)
{
    uint32_t address = vita_sync_arg(c, 0U);
    uint32_t spin_count = vita_sync_arg(c, 1U);

    if (!isaac_vita_sync_cs_initialize(c, address, spin_count))
        return;
    c->eax = 1U;                 /* Win32 BOOL success */
    vita_sync_stdcall_return(c, 8U);
}

static void vita_sync_InitializeCriticalSection(CPU *__restrict c)
{
    uint32_t address = vita_sync_arg(c, 0U);

    if (!isaac_vita_sync_cs_initialize_plain(c, address))
        return;
    /* Win32 declares this import void: preserve the incoming EAX value. */
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_EnterCriticalSection(CPU *__restrict c)
{
    uint32_t address = vita_sync_arg(c, 0U);

    if (!isaac_vita_sync_cs_enter(c, address))
        return;
    /* Win32 declares this import void: preserve the incoming EAX value. */
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_TryEnterCriticalSection(CPU *__restrict c)
{
    uint32_t address = vita_sync_arg(c, 0U);
    int32_t thread_id;
    int result;

    if (!vita_sync_cs_validate(c, address))
        return;
    thread_id = vita_sync_bound_thread_id(c);
    if (thread_id > 0) {
        result = isaac_vita_sync_try_lock_userspace_mutex_for_thread(
            (uint32_t *)(uintptr_t)(address + 16U),
            (uint32_t *)(uintptr_t)(address + 20U), thread_id);
    } else {
        result = isaac_vita_sync_try_lock_userspace_mutex(
            (uint32_t *)(uintptr_t)(address + 16U),
            (uint32_t *)(uintptr_t)(address + 20U));
    }
    if (result < 0) {
        isaac_vita_sync_log_failure(
            "VitaEmbeddedTryEnterCriticalSection", result);
        vita_sync_fault(
            c, GUEST_IMAGE_BASE + ISAAC_VITA_SYNC_TRY_ENTER_CS_IAT_RVA,
            "Vita recursive mutex try-lock failed");
        return;
    }
    c->eax = result ? 1U : 0U;
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_LeaveCriticalSection(CPU *__restrict c)
{
    uint32_t address = vita_sync_arg(c, 0U);

    if (!isaac_vita_sync_cs_leave(c, address))
        return;
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_DeleteCriticalSection(CPU *__restrict c)
{
    uint32_t address = vita_sync_arg(c, 0U);

    if (!isaac_vita_sync_cs_delete(c, address))
        return;
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_GetModuleHandleW(CPU *__restrict c)
{
    uint32_t name = vita_sync_arg(c, 0U);
    char module[64];

    if (!vita_sync_copy_utf16_ascii(c, name, module))
        return;
    if (vita_sync_ascii_case_equal(
            module, "api-ms-win-core-synch-l1-2-0.dll")) {
        c->eax = 0U;             /* explicitly absent: use the CRT fallback */
    } else if (vita_sync_ascii_case_equal(module, "kernel32.dll")) {
        c->eax = ISAAC_VITA_SYNC_KERNEL32_TOKEN;
    } else {
        vita_sync_fault(c, name,
                        "GetModuleHandleW for an unexpected module");
        return;
    }
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_GetProcAddress(CPU *__restrict c)
{
    uint32_t module = vita_sync_arg(c, 0U);
    uint32_t name = vita_sync_arg(c, 1U);
    char procedure[64];

    if (module != ISAAC_VITA_SYNC_KERNEL32_TOKEN &&
        module != ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE) {
        vita_sync_fault(c, module,
                        "GetProcAddress received an unexpected module token");
        return;
    }
    if (!vita_sync_copy_ascii(c, name, procedure))
        return;

    if (module == ISAAC_VITA_SYNC_KERNEL32_TOKEN) {
        if (strcmp(procedure, "SleepConditionVariableCS") != 0 &&
            strcmp(procedure, "WakeAllConditionVariable") != 0) {
            vita_sync_fault(c, name,
                            "GetProcAddress requested an unexpected procedure");
            return;
        }
    } else if (module == ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE) {
        if (strcmp(procedure,
                   ISAAC_VITA_SYNC_GET_USER_PROFILE_PROCEDURE) != 0) {
            vita_sync_fault(
                c, name,
                "GetProcAddress requested an unexpected absent-module procedure");
            return;
        }
    }

    if (module == ISAAC_VITA_SYNC_GET_USER_PROFILE_MODULE) {
        /* USERENV itself was absent, so its optional procedure is absent too.
         * The caller immediately falls back to getenv("USERPROFILE"). */
        c->eax = 0U;
        vita_sync_stdcall_return(c, 8U);
        return;
    }

    c->eax = 0U;                 /* both optional native entry points absent */
    vita_sync_stdcall_return(c, 8U);
}

static void vita_sync_CreateEventW(CPU *__restrict c)
{
    uint32_t security = vita_sync_arg(c, 0U);
    uint32_t manual_reset = vita_sync_arg(c, 1U);
    uint32_t initial_state = vita_sync_arg(c, 2U);
    uint32_t name = vita_sync_arg(c, 3U);
    int32_t uid;

    if (security != 0U || manual_reset != 1U ||
        initial_state != 0U || name != 0U) {
        vita_sync_fault(c,
                        GUEST_IMAGE_BASE + ISAAC_VITA_SYNC_CREATE_EVENT_IAT_RVA,
                        "CreateEventW outside the measured manual-reset shape");
        return;
    }
    if (s_owned_event) {
        vita_sync_fault(c, (uint32_t)s_owned_event,
                        "CreateEventW attempted to replace its owned event");
        return;
    }
    uid = isaac_vita_sync_create_manual_reset_event(0);
    if (uid <= 0) {
        /* CreateEventW reports failure with a NULL handle.  The translated
         * caller already has a fatal branch for it, so preserve that control
         * flow instead of manufacturing a guest fault here. */
        isaac_vita_sync_log_failure("sceKernelCreateEventFlag", uid);
        c->eax = 0U;
        vita_sync_stdcall_return(c, 16U);
        return;
    }
    s_owned_event = uid;
    c->eax = (uint32_t)uid;
    vita_sync_stdcall_return(c, 16U);
}

static void vita_sync_CloseHandle(CPU *__restrict c)
{
    uint32_t handle = vita_sync_arg(c, 0U);
    int result;

    if (!handle || (int32_t)handle != s_owned_event) {
        vita_sync_fault(
            c, handle,
            "CloseHandle received an unknown, stale, or non-event handle");
        return;
    }
    result = isaac_vita_sync_delete_event((int32_t)handle);
    if (result < 0) {
        isaac_vita_sync_log_failure("sceKernelDeleteEventFlag", result);
        c->last_error = ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE;
        c->eax = 0U;
        vita_sync_stdcall_return(c, 4U);
        return;
    }
    s_owned_event = 0;
    c->eax = 1U;
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_SetEvent(CPU *__restrict c)
{
    int result = isaac_vita_sync_set_event(
        (int32_t)vita_sync_arg(c, 0U));

    if (result < 0) {
        isaac_vita_sync_log_failure("sceKernelSetEventFlag", result);
        c->last_error = ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE;
        c->eax = 0U;
    } else {
        c->eax = 1U;
    }
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_ResetEvent(CPU *__restrict c)
{
    int result = isaac_vita_sync_reset_event(
        (int32_t)vita_sync_arg(c, 0U));

    if (result < 0) {
        isaac_vita_sync_log_failure("sceKernelClearEventFlag", result);
        c->last_error = ISAAC_VITA_SYNC_ERROR_INVALID_HANDLE;
        c->eax = 0U;
    } else {
        c->eax = 1U;
    }
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_Sleep(CPU *__restrict c)
{
    int result = isaac_vita_sync_sleep_milliseconds(
        vita_sync_arg(c, 0U));

    if (result < 0) {
        isaac_vita_sync_log_failure("sceKernelDelayThread", result);
        vita_sync_fault(c,
                        GUEST_IMAGE_BASE + ISAAC_VITA_SYNC_SLEEP_IAT_RVA,
                        "Vita thread delay failed");
        return;
    }
    /* Win32 declares Sleep void: preserve the incoming EAX value. */
    vita_sync_stdcall_return(c, 4U);
}

static void vita_sync_GetModuleHandleA(CPU *__restrict c)
{
    uint32_t name = vita_sync_arg(c, 0U);
    char module[64];

    if (!name) {
        c->eax = GUEST_IMAGE_BASE;
    } else {
        if (!vita_sync_copy_module_ascii(c, name, module))
            return;
        if (vita_sync_ascii_case_equal(module, "kernel32.dll")) {
            c->eax = ISAAC_VITA_SYNC_KERNEL32_TOKEN;
        } else {
            vita_sync_fault(c, name,
                            "GetModuleHandleA for an unexpected module");
            return;
        }
    }
    vita_sync_stdcall_return(c, 4U);
}

/* Unique imports in first-observed order.  The evidence header retains both
 * the initial six-call batch and the later 32-call plain-CS batch. */
static const vita_sync_import_entry s_vita_sync_imports[] = {
    { ISAAC_VITA_SYNC_INIT_CS_NAME,
      vita_sync_InitializeCriticalSectionAndSpinCount },
    { ISAAC_VITA_SYNC_GET_MODULE_NAME, vita_sync_GetModuleHandleW },
    { ISAAC_VITA_SYNC_GET_PROC_NAME, vita_sync_GetProcAddress },
    { ISAAC_VITA_SYNC_CREATE_EVENT_NAME, vita_sync_CreateEventW },
    { ISAAC_VITA_SYNC_PLAIN_CS_NAME, vita_sync_InitializeCriticalSection },
    { ISAAC_VITA_SYNC_ENTER_CS_NAME, vita_sync_EnterCriticalSection },
    { ISAAC_VITA_SYNC_LEAVE_CS_NAME, vita_sync_LeaveCriticalSection },
    { ISAAC_VITA_SYNC_DELETE_CS_NAME, vita_sync_DeleteCriticalSection },
    { ISAAC_VITA_SYNC_SET_EVENT_NAME, vita_sync_SetEvent },
    { ISAAC_VITA_SYNC_RESET_EVENT_NAME, vita_sync_ResetEvent },
    { ISAAC_VITA_SYNC_SLEEP_NAME, vita_sync_Sleep },
    { ISAAC_VITA_SYNC_GET_MODULE_A_NAME, vita_sync_GetModuleHandleA },
    { ISAAC_VITA_SYNC_CLOSE_HANDLE_NAME, vita_sync_CloseHandle },
    { ISAAC_VITA_SYNC_TRY_ENTER_CS_NAME,
      vita_sync_TryEnterCriticalSection }
};

_Static_assert(sizeof s_vita_sync_imports / sizeof s_vita_sync_imports[0] ==
               ISAAC_VITA_SYNC_IMPORT_COUNT,
               "Vita sync import count drifted from measured batch");

const char *isaac_vita_sync_import_name(uint32_t index)
{
    return index < ISAAC_VITA_SYNC_IMPORT_COUNT
        ? s_vita_sync_imports[index].name : NULL;
}

int isaac_vita_sync_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    if (index >= ISAAC_VITA_SYNC_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count; /* guest_fault may not return */
    s_vita_sync_imports[index].fn(c);
    return 1;
}

static int vita_sync_dispatch(CPU *__restrict c, const char *name,
                              unsigned *call_count)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_SYNC_IMPORT_COUNT; ++i) {
        if (strcmp(s_vita_sync_imports[i].name, name) == 0)
            return isaac_vita_sync_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_sync_import(CPU *__restrict c, const char *name)
{
    return vita_sync_dispatch(c, name, NULL);
}

int isaac_vita_sync_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    return vita_sync_dispatch(c, name, call_count);
}
