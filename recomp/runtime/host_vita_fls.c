/* Minimal single-guest-thread FLS model for Vita bring-up.
 *
 * Windows FLS is per fiber.  The current Vita runtime has one guest thread and
 * no guest fiber scheduler, so one process-static value per slot is the exact
 * useful subset.  Slot indices and callback behavior match the already-
 * oracled PC portable implementation; no native Vita TLS API is involved.
 */
#include <stdint.h>
#include <string.h>

#include "host_vita_fls.h"
#include "host_vita_import_id.h"

typedef struct vita_fls_slot {
    uint32_t callback;
    uint32_t value;
    uint8_t allocated;
} vita_fls_slot;

typedef void (*vita_fls_import_fn)(CPU *__restrict);

typedef struct vita_fls_import_entry {
    const char *name;
    vita_fls_import_fn fn;
} vita_fls_import_entry;

static vita_fls_slot s_vita_fls_slots[ISAAC_VITA_FLS_SLOT_COUNT];

static uint32_t vita_fls_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_fls_stdcall_return(CPU *__restrict c, uint32_t argument_count)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_count * 4U, 0U);
}

/* Copied from the already-oracled host_win32 helper.  The synthetic return and
 * one argument must both be removed by the translated stdcall callback. */
static int vita_fls_call_guest_stdcall1(CPU *__restrict c, uint32_t target,
                                        uint32_t argument)
{
    uint32_t saved_esp = c->esp;

    gpush(c, argument);
    gpush(c, ISAAC_VITA_FLS_CALLBACK_SENTINEL);
    guest_call(c, target);
    if (c->fault)
        return 0;
    if (c->esp != saved_esp) {
        guest_fault(c, target,
                    "FLS callback did not clean its stdcall argument");
        return 0;
    }
    return 1;
}

static int vita_fls_slot_is_valid(uint32_t index)
{
    return index < ISAAC_VITA_FLS_SLOT_COUNT &&
           s_vita_fls_slots[index].allocated;
}

static void vita_fls_free(CPU *__restrict c)
{
    uint32_t index = vita_fls_arg(c, 0U);
    uint32_t callback;
    uint32_t value;

    if (!vita_fls_slot_is_valid(index)) {
        c->eax = 0U;
        vita_fls_stdcall_return(c, 1U);
        return;
    }

    callback = s_vita_fls_slots[index].callback;
    value = s_vita_fls_slots[index].value;
    if (callback && value &&
        !vita_fls_call_guest_stdcall1(c, callback, value)) {
        /* A nested fault must not free or otherwise mutate the slot. */
        return;
    }

    memset(&s_vita_fls_slots[index], 0, sizeof s_vita_fls_slots[index]);
    c->eax = 1U;
    vita_fls_stdcall_return(c, 1U);
}

static void vita_fls_set_value(CPU *__restrict c)
{
    uint32_t index = vita_fls_arg(c, 0U);
    uint32_t value = vita_fls_arg(c, 1U);

    if (!vita_fls_slot_is_valid(index)) {
        c->eax = 0U;
        vita_fls_stdcall_return(c, 2U);
        return;
    }

    s_vita_fls_slots[index].value = value;
    c->eax = 1U;
    vita_fls_stdcall_return(c, 2U);
}

static void vita_fls_alloc(CPU *__restrict c)
{
    uint32_t callback = vita_fls_arg(c, 0U);
    uint32_t index;

    c->eax = ISAAC_VITA_FLS_OUT_OF_INDEXES;
    for (index = 0U; index < ISAAC_VITA_FLS_SLOT_COUNT; ++index) {
        if (!s_vita_fls_slots[index].allocated) {
            s_vita_fls_slots[index].allocated = 1U;
            s_vita_fls_slots[index].callback = callback;
            s_vita_fls_slots[index].value = 0U;
            c->eax = index;
            break;
        }
    }
    vita_fls_stdcall_return(c, 1U);
}

/* Selected-PE import-table order. */
static const vita_fls_import_entry s_vita_fls_imports[] = {
    { ISAAC_VITA_FLS_FREE_NAME,     vita_fls_free },
    { ISAAC_VITA_FLS_SETVALUE_NAME, vita_fls_set_value },
    { ISAAC_VITA_FLS_ALLOC_NAME,    vita_fls_alloc }
};

_Static_assert(sizeof s_vita_fls_imports / sizeof s_vita_fls_imports[0] ==
               ISAAC_VITA_FLS_IMPORT_COUNT,
               "Vita FLS import count drifted from exact PE batch");

const char *isaac_vita_fls_import_name(uint32_t index)
{
    return index < ISAAC_VITA_FLS_IMPORT_COUNT
        ? s_vita_fls_imports[index].name : NULL;
}

int isaac_vita_fls_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count)
{
    if (index >= ISAAC_VITA_FLS_IMPORT_COUNT)
        return 0;
    /* Must precede the handler: the callback can fault/longjmp. */
    if (call_count)
        ++*call_count;
    s_vita_fls_imports[index].fn(c);
    return 1;
}

static int vita_fls_dispatch(CPU *__restrict c, const char *name,
                             unsigned *call_count)
{
    uint32_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_FLS_IMPORT_COUNT; ++index) {
        if (strcmp(s_vita_fls_imports[index].name, name) == 0)
            return isaac_vita_fls_import_indexed(c, index, call_count);
    }
    return 0;
}

int isaac_vita_fls_import(CPU *__restrict c, const char *name)
{
    return vita_fls_dispatch(c, name, NULL);
}

int isaac_vita_fls_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    return vita_fls_dispatch(c, name, call_count);
}
