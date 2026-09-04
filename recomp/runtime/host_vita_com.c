/* Target-local COM apartment lifecycle for Vita.  The game imports no COM
 * object API, so this deliberately creates no native or synthetic objects. */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_com.h"
#include "host_vita_import_id.h"

typedef void (*vita_com_handler)(CPU *__restrict c);

typedef struct vita_com_import_entry {
    const char *name;
    vita_com_handler handler;
} vita_com_import_entry;

static atomic_uint s_com_init_count;

static uint32_t vita_com_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_com_stdcall_return(CPU *__restrict c,
                                    uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static uint32_t vita_com_initialize_result(void)
{
    unsigned previous = atomic_fetch_add_explicit(
        &s_com_init_count, 1U, memory_order_relaxed);
    return previous ? 1U : 0U; /* S_FALSE : S_OK */
}

static void vita_com_CoInitialize(CPU *__restrict c)
{
    (void)vita_com_arg(c, 0U);
    c->eax = vita_com_initialize_result();
    vita_com_stdcall_return(c, 4U);
}

static void vita_com_CoInitializeEx(CPU *__restrict c)
{
    (void)vita_com_arg(c, 0U);
    (void)vita_com_arg(c, 1U);
    c->eax = vita_com_initialize_result();
    vita_com_stdcall_return(c, 8U);
}

static void vita_com_CoUninitialize(CPU *__restrict c)
{
    unsigned current = atomic_load_explicit(&s_com_init_count,
                                            memory_order_relaxed);

    while (current && !atomic_compare_exchange_weak_explicit(
               &s_com_init_count, &current, current - 1U,
               memory_order_relaxed, memory_order_relaxed)) {
    }
    vita_com_stdcall_return(c, 0U);
}

static const vita_com_import_entry s_vita_com_imports[] = {
    { ISAAC_VITA_COM_INITIALIZE_NAME, vita_com_CoInitialize },
    { ISAAC_VITA_COM_INITIALIZE_EX_NAME, vita_com_CoInitializeEx },
    { ISAAC_VITA_COM_UNINITIALIZE_NAME, vita_com_CoUninitialize },
};

_Static_assert(sizeof s_vita_com_imports / sizeof s_vita_com_imports[0] ==
                   ISAAC_VITA_COM_IMPORT_COUNT,
               "Vita COM import inventory drifted");

const char *isaac_vita_com_import_name(uint32_t index)
{
    return index < ISAAC_VITA_COM_IMPORT_COUNT
        ? s_vita_com_imports[index].name : NULL;
}

int isaac_vita_com_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count)
{
    if (index >= ISAAC_VITA_COM_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_com_imports[index].handler(c);
    return 1;
}

static int vita_com_dispatch(CPU *__restrict c, const char *name,
                             unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_COM_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_com_imports[index].name) == 0)
            return isaac_vita_com_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_com_import(CPU *__restrict c, const char *name)
{
    return vita_com_dispatch(c, name, NULL);
}

int isaac_vita_com_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    return vita_com_dispatch(c, name, call_count);
}
