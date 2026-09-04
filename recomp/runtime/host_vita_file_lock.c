/* Win32 byte-range lock boundary for CRT-owned Vita FILE descriptors. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_crt.h"
#include "host_vita_file_lock.h"
#include "host_vita_import_id.h"

typedef void (*vita_file_lock_handler)(CPU *__restrict c);

typedef struct vita_file_lock_import_entry {
    const char *name;
    vita_file_lock_handler handler;
} vita_file_lock_import_entry;

static uint32_t vita_file_lock_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_file_lock_stdcall_return(CPU *__restrict c,
                                          uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void vita_file_lock_fail(CPU *__restrict c, uint32_t error,
                                uint32_t argument_bytes)
{
    c->last_error = error;
    c->eax = 0U;
    vita_file_lock_stdcall_return(c, argument_bytes);
}

static void vita_file_lock_LockFileEx(CPU *__restrict c)
{
    uint32_t handle = vita_file_lock_arg(c, 0U);
    uint32_t flags = vita_file_lock_arg(c, 1U);
    uint32_t reserved = vita_file_lock_arg(c, 2U);
    uint32_t overlapped = vita_file_lock_arg(c, 5U);

    if (!isaac_vita_crt_osfhandle_is_owned(handle)) {
        vita_file_lock_fail(
            c, ISAAC_VITA_FILE_LOCK_ERROR_INVALID_HANDLE, 24U);
        return;
    }
    if ((flags & ~ISAAC_VITA_FILE_LOCK_ALLOWED_FLAGS) != 0U ||
        reserved != 0U || overlapped == 0U) {
        vita_file_lock_fail(
            c, ISAAC_VITA_FILE_LOCK_ERROR_INVALID_PARAMETER, 24U);
        return;
    }

    /* Win32 byte-range locking arbitrates with other processes and handles.
     * Vita runs one title process and has no foreign Win32 lock participant,
     * so every valid owned range is immediately available.  Do not pass the
     * guest-visible value to sceIo/fcntl: it is only an ownership-checked CRT
     * descriptor token.  Length and OVERLAPPED offset remain opaque because
     * no native I/O boundary consumes either guest value here. */
    (void)vita_file_lock_arg(c, 3U);
    (void)vita_file_lock_arg(c, 4U);
    c->eax = 1U;
    vita_file_lock_stdcall_return(c, 24U);
}

static void vita_file_lock_UnlockFileEx(CPU *__restrict c)
{
    uint32_t handle = vita_file_lock_arg(c, 0U);
    uint32_t reserved = vita_file_lock_arg(c, 1U);
    uint32_t overlapped = vita_file_lock_arg(c, 4U);

    if (!isaac_vita_crt_osfhandle_is_owned(handle)) {
        vita_file_lock_fail(
            c, ISAAC_VITA_FILE_LOCK_ERROR_INVALID_HANDLE, 20U);
        return;
    }
    if (reserved != 0U || overlapped == 0U) {
        vita_file_lock_fail(
            c, ISAAC_VITA_FILE_LOCK_ERROR_INVALID_PARAMETER, 20U);
        return;
    }

    (void)vita_file_lock_arg(c, 2U);
    (void)vita_file_lock_arg(c, 3U);
    c->eax = 1U;
    vita_file_lock_stdcall_return(c, 20U);
}

static const vita_file_lock_import_entry s_vita_file_lock_imports[] = {
    { ISAAC_VITA_FILE_LOCK_LOCK_NAME, vita_file_lock_LockFileEx },
    { ISAAC_VITA_FILE_LOCK_UNLOCK_NAME, vita_file_lock_UnlockFileEx },
};

_Static_assert(sizeof s_vita_file_lock_imports /
                   sizeof s_vita_file_lock_imports[0] ==
                   ISAAC_VITA_FILE_LOCK_IMPORT_COUNT,
               "Vita file-lock import inventory drifted");

const char *isaac_vita_file_lock_import_name(uint32_t index)
{
    return index < ISAAC_VITA_FILE_LOCK_IMPORT_COUNT
        ? s_vita_file_lock_imports[index].name : NULL;
}

int isaac_vita_file_lock_import_indexed(CPU *__restrict c, uint32_t index,
                                        unsigned *call_count)
{
    if (index >= ISAAC_VITA_FILE_LOCK_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_file_lock_imports[index].handler(c);
    return 1;
}

static int vita_file_lock_dispatch(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_FILE_LOCK_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_file_lock_imports[index].name) == 0)
            return isaac_vita_file_lock_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_file_lock_import(CPU *__restrict c, const char *name)
{
    return vita_file_lock_dispatch(c, name, NULL);
}

int isaac_vita_file_lock_import_counted(CPU *__restrict c, const char *name,
                                        unsigned *call_count)
{
    return vita_file_lock_dispatch(c, name, call_count);
}
