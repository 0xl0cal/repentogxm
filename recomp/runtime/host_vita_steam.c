/* Disabled-Steam callback listener state for Vita.
 *
 * Registering is still guest-visible state even though no native Steam client
 * exists.  Keep a small bounded registry so a later UnregisterCallback or
 * Shutdown can observe the real lifecycle, but never invoke guest callbacks
 * or manufacture a successful Steam session. */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_steam.h"
#include "host_vita_import_id.h"

typedef struct vita_steam_callback_entry {
    uint32_t object;
    int32_t callback_id;
} vita_steam_callback_entry;

static vita_steam_callback_entry
    s_callbacks[ISAAC_VITA_STEAM_CALLBACK_CAPACITY];
static size_t s_callback_count;
static atomic_flag s_callback_lock = ATOMIC_FLAG_INIT;

static void vita_steam_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
               &s_callback_lock, memory_order_acquire)) {
        /* Registration happens during single-threaded startup.  Keeping the
         * state lock self-contained also makes later lifecycle calls safe. */
    }
}

static void vita_steam_unlock(void)
{
    atomic_flag_clear_explicit(&s_callback_lock, memory_order_release);
}

static uint32_t vita_steam_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_steam_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

#if defined(ISAAC_VITA_STEAM_CONTEXT_TEST_READ32)
/* The standalone oracle has no PE mapped at 0x98000000.  It injects only the
 * descriptor reads; the production object still compiles the direct checked
 * guest-memory path below. */
uint32_t isaac_vita_steam_context_test_read32(uint32_t address);

static uint32_t vita_steam_context_read32(uint32_t address)
{
    return isaac_vita_steam_context_test_read32(address);
}
#else
static uint32_t vita_steam_context_read32(uint32_t address)
{
    return ld32(address);
}
#endif

static int vita_steam_store_callback(uint32_t object, int32_t callback_id)
{
    size_t index;

    vita_steam_lock();
    for (index = 0U; index < s_callback_count; ++index) {
        if (s_callbacks[index].object == object) {
            /* CCallbackBase owns one callback id.  Re-registration changes
             * that object's observable registration rather than inventing a
             * second listener for the same guest object. */
            s_callbacks[index].callback_id = callback_id;
            vita_steam_unlock();
            return 1;
        }
    }
    if (s_callback_count == ISAAC_VITA_STEAM_CALLBACK_CAPACITY) {
        vita_steam_unlock();
        return 0;
    }
    s_callbacks[s_callback_count].object = object;
    s_callbacks[s_callback_count].callback_id = callback_id;
    ++s_callback_count;
    vita_steam_unlock();
    return 1;
}

static void vita_steam_RegisterCallback(CPU *__restrict c)
{
    /* Complete both checked guest reads before mutating process state. */
    uint32_t object = vita_steam_arg(c, 0U);
    int32_t callback_id = (int32_t)vita_steam_arg(c, 1U);

    if (!object) {
        guest_fault(c, object,
                    "SteamAPI_RegisterCallback received a NULL object");
        return;
    }
    if (!vita_steam_store_callback(object, callback_id)) {
        guest_fault(c, object,
                    "Steam callback registry is full");
        return;
    }
    /* Void cdecl: preserve EAX and leave both arguments for the caller. */
    vita_steam_cdecl_return(c);
}

/* SteamInternal_ContextInit is not an interface-success stub.  Steam's x86
 * implementation always returns descriptor+8.  With SteamAPI_Init=false its
 * callback leaves this PE's complete 21-dword CSteamAPIContext zeroed.  Pin
 * the one measured descriptor so a future real interface use faults here. */
static void vita_steam_ContextInit(CPU *__restrict c)
{
    uint32_t descriptor = vita_steam_arg(c, 0U);
    uint32_t index;

    if (descriptor != ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA ||
        vita_steam_context_read32(descriptor) !=
            ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA ||
        vita_steam_context_read32(descriptor + 4U) !=
            ISAAC_VITA_STEAM_CONTEXT_GENERATION) {
        guest_fault(c, descriptor,
                    "SteamInternal_ContextInit unknown descriptor state");
        return;
    }
    for (index = 0U; index < ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT; ++index) {
        uint32_t slot = descriptor + 8U + index * 4U;

        if (vita_steam_context_read32(slot) != 0U) {
            guest_fault(c, slot,
                        "Steam-disabled context unexpectedly initialized");
            return;
        }
    }
    c->eax = descriptor + 8U;
    vita_steam_cdecl_return(c);
}

/* SteamAPICall_t is uint64 on x86, so both helpers have three guest dwords:
 * CCallbackBase*, handle low, handle high.  A disabled backend cannot deliver
 * these call results; consume the measured cdecl ABI without retaining a
 * registration that promises delivery. */
static void vita_steam_RegisterCallResult(CPU *__restrict c)
{
    uint32_t object = vita_steam_arg(c, 0U);
    uint32_t handle_low = vita_steam_arg(c, 1U);
    uint32_t handle_high = vita_steam_arg(c, 2U);

    (void)object;
    (void)handle_low;
    (void)handle_high;
    vita_steam_cdecl_return(c);
}

static void vita_steam_UnregisterCallResult(CPU *__restrict c)
{
    uint32_t object = vita_steam_arg(c, 0U);
    uint32_t handle_low = vita_steam_arg(c, 1U);
    uint32_t handle_high = vita_steam_arg(c, 2U);

    (void)object;
    (void)handle_low;
    (void)handle_high;
    vita_steam_cdecl_return(c);
}

static void vita_steam_UnregisterCallback(CPU *__restrict c)
{
    uint32_t object = vita_steam_arg(c, 0U);

    (void)isaac_vita_steam_callback_unregister(object);
    vita_steam_cdecl_return(c);
}

static void vita_steam_RunCallbacks(CPU *__restrict c)
{
    vita_steam_cdecl_return(c);
}

static void vita_steam_Shutdown(CPU *__restrict c)
{
    isaac_vita_steam_callback_clear();
    vita_steam_cdecl_return(c);
}

const char *isaac_vita_steam_import_name(uint32_t index)
{
    static const char *const names[ISAAC_VITA_STEAM_IMPORT_COUNT] = {
        ISAAC_VITA_STEAM_CONTEXT_INIT_NAME,
        ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME,
        ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_NAME,
        ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_NAME,
        ISAAC_VITA_STEAM_RUN_CALLBACKS_NAME,
        ISAAC_VITA_STEAM_SHUTDOWN_NAME,
        ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_NAME
    };

    return index < ISAAC_VITA_STEAM_IMPORT_COUNT ? names[index] : NULL;
}

int isaac_vita_steam_import_indexed(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count)
{
    static void (*const handlers[ISAAC_VITA_STEAM_IMPORT_COUNT])(
        CPU *__restrict) = {
        vita_steam_ContextInit,
        vita_steam_RegisterCallback,
        vita_steam_RegisterCallResult,
        vita_steam_UnregisterCallResult,
        vita_steam_RunCallbacks,
        vita_steam_Shutdown,
        vita_steam_UnregisterCallback
    };

    if (index >= ISAAC_VITA_STEAM_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count; /* checked stack reads and capacity faults may unwind */
    handlers[index](c);
    return 1;
}

static int vita_steam_dispatch(CPU *__restrict c, const char *name,
                               unsigned *call_count)
{
    uint32_t index;

    if (!name)
        return 0;
    if (strcmp(name, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME) == 0)
        index = 0U;
    else if (strcmp(name, ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME) == 0)
        index = 1U;
    else if (strcmp(name, ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_NAME) == 0)
        index = 2U;
    else if (strcmp(name, ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_NAME) == 0)
        index = 3U;
    else if (strcmp(name, ISAAC_VITA_STEAM_RUN_CALLBACKS_NAME) == 0)
        index = 4U;
    else if (strcmp(name, ISAAC_VITA_STEAM_SHUTDOWN_NAME) == 0)
        index = 5U;
    else if (strcmp(name, ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_NAME) == 0)
        index = 6U;
    else
        return 0; /* Init has another owner; three interface slots stay loud. */
    return isaac_vita_steam_import_indexed(c, index, call_count);
}

int isaac_vita_steam_import(CPU *__restrict c, const char *name)
{
    return vita_steam_dispatch(c, name, NULL);
}

int isaac_vita_steam_import_counted(CPU *__restrict c, const char *name,
                                    unsigned *call_count)
{
    return vita_steam_dispatch(c, name, call_count);
}

size_t isaac_vita_steam_callback_count(void)
{
    size_t count;

    vita_steam_lock();
    count = s_callback_count;
    vita_steam_unlock();
    return count;
}

int isaac_vita_steam_callback_lookup(uint32_t object, int32_t *callback_id)
{
    size_t index;
    int found = 0;

    vita_steam_lock();
    for (index = 0U; index < s_callback_count; ++index) {
        if (s_callbacks[index].object == object) {
            if (callback_id)
                *callback_id = s_callbacks[index].callback_id;
            found = 1;
            break;
        }
    }
    vita_steam_unlock();
    return found;
}

int isaac_vita_steam_callback_unregister(uint32_t object)
{
    size_t index;

    vita_steam_lock();
    for (index = 0U; index < s_callback_count; ++index) {
        if (s_callbacks[index].object == object) {
            --s_callback_count;
            s_callbacks[index] = s_callbacks[s_callback_count];
            s_callbacks[s_callback_count].object = 0U;
            s_callbacks[s_callback_count].callback_id = 0;
            vita_steam_unlock();
            return 1;
        }
    }
    vita_steam_unlock();
    return 0;
}

void isaac_vita_steam_callback_clear(void)
{
    vita_steam_lock();
    memset(s_callbacks, 0, sizeof s_callbacks);
    s_callback_count = 0U;
    vita_steam_unlock();
}
