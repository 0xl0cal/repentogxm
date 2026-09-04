/* Exact early Win32 console pair, implemented without exposing native Vita
 * handles to the identity-mapped x86 guest. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_console.h"
#include "host_vita_import_id.h"
#include "platform.h"

typedef void (*vita_console_import_fn)(CPU *__restrict);

typedef struct vita_console_import_entry {
    const char *name;
    vita_console_import_fn fn;
} vita_console_import_entry;

static uint32_t vita_console_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_console_stdcall_return(CPU *__restrict c,
                                        uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void vita_console_GetStdHandle(CPU *__restrict c)
{
    uint32_t requested = vita_console_arg(c, 0U);

    if (requested != ISAAC_VITA_CONSOLE_STD_ERROR_ARGUMENT) {
        guest_fault(c,
                    GUEST_IMAGE_BASE +
                        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_IAT_RVA,
                    "GetStdHandle requested an unexpected stream");
        return;
    }
    c->eax = ISAAC_VITA_CONSOLE_STDERR_TOKEN;
    vita_console_stdcall_return(c, 4U);
}

static void vita_console_WriteConsoleA(CPU *__restrict c)
{
    uint32_t handle = vita_console_arg(c, 0U);
    uint32_t buffer = vita_console_arg(c, 1U);
    uint32_t count = vita_console_arg(c, 2U);
    uint32_t written = vita_console_arg(c, 3U);
    uint32_t reserved = vita_console_arg(c, 4U);
    uint32_t offset = 0U;
    char chunk[ISAAC_VITA_CONSOLE_LOG_CHUNK + 1U];

    if (handle != ISAAC_VITA_CONSOLE_STDERR_TOKEN || reserved != 0U) {
        guest_fault(c, GUEST_IMAGE_BASE + ISAAC_VITA_CONSOLE_WRITE_IAT_RVA,
                    "WriteConsoleA received an unexpected call shape");
        return;
    }
    if (!written || written > UINT32_MAX - 3U ||
        count > ISAAC_VITA_CONSOLE_MAX_WRITE ||
        (count && (!buffer || buffer > UINT32_MAX - (count - 1U)))) {
        guest_fault(c, (!written || written > UINT32_MAX - 3U) ?
                           written : buffer,
                    "WriteConsoleA received an invalid guest range");
        return;
    }

    while (offset < count) {
        uint32_t length = count - offset;
        uint32_t i;
        if (length > ISAAC_VITA_CONSOLE_LOG_CHUNK)
            length = ISAAC_VITA_CONSOLE_LOG_CHUNK;
        for (i = 0U; i < length; ++i)
            chunk[i] = (char)ld8(buffer + offset + i);
        chunk[length] = '\0';
        /* The guest bytes are data, never the logger's format string. */
        isaac_vita_log("%s", chunk);
        offset += length;
    }
    st32(written, count);
    c->eax = 1U;
    vita_console_stdcall_return(c, 20U);
}

static const vita_console_import_entry s_vita_console_imports[] = {
    { ISAAC_VITA_CONSOLE_GET_STD_HANDLE_NAME, vita_console_GetStdHandle },
    { ISAAC_VITA_CONSOLE_WRITE_NAME,          vita_console_WriteConsoleA }
};

_Static_assert(sizeof s_vita_console_imports /
                   sizeof s_vita_console_imports[0] ==
               ISAAC_VITA_CONSOLE_IMPORT_COUNT,
               "Vita console import count drifted from exact pair");

const char *isaac_vita_console_import_name(uint32_t index)
{
    return index < ISAAC_VITA_CONSOLE_IMPORT_COUNT
        ? s_vita_console_imports[index].name : NULL;
}

int isaac_vita_console_import_indexed(CPU *__restrict c, uint32_t index,
                                      unsigned *call_count)
{
    if (index >= ISAAC_VITA_CONSOLE_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_console_imports[index].fn(c);
    return 1;
}

static int vita_console_dispatch(CPU *__restrict c, const char *name,
                                 unsigned *call_count)
{
    uint32_t i;
    for (i = 0U; i < ISAAC_VITA_CONSOLE_IMPORT_COUNT; ++i) {
        if (strcmp(name, s_vita_console_imports[i].name) == 0)
            return isaac_vita_console_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_console_import(CPU *__restrict c, const char *name)
{
    return vita_console_dispatch(c, name, NULL);
}

int isaac_vita_console_import_counted(CPU *__restrict c, const char *name,
                                      unsigned *call_count)
{
    return vita_console_dispatch(c, name, call_count);
}
