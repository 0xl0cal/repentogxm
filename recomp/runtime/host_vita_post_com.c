/* Target-local policy for the four imports immediately following COM setup. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_post_com.h"
#include "host_vita_import_id.h"
#include "vita_host_services.h"

typedef void (*vita_post_com_handler)(CPU *__restrict c);

typedef struct vita_post_com_import_entry {
    const char *name;
    vita_post_com_handler handler;
} vita_post_com_import_entry;

static uint32_t s_execution_state =
    ISAAC_VITA_POST_COM_EXECUTION_STATE_INITIAL;

static uint32_t vita_post_com_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_post_com_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

static void vita_post_com_stdcall_return(CPU *__restrict c,
                                         uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static void vita_post_com_SteamAPI_Init(CPU *__restrict c)
{
    /* No Steam client exists on Vita.  False selects the game's established
     * offline path without manufacturing interfaces or callbacks. */
    c->eax = 0U;
    vita_post_com_cdecl_return(c);
}

static void vita_post_com_SetThreadExecutionState(CPU *__restrict c)
{
    uint32_t requested = vita_post_com_arg(c, 0U);
    uint32_t previous;

    /* Suspend/display policy belongs to the Vita frontend.  Preserve only
     * Win32's observable previous-state result, exactly as host_win32 does. */
    previous = s_execution_state;
    if (requested & 0x80000000U)
        s_execution_state = requested;
    c->eax = previous;
    vita_post_com_stdcall_return(c, 4U);
}

static void vita_post_com_timeGetDevCaps(CPU *__restrict c)
{
    uint32_t caps = vita_post_com_arg(c, 0U);
    uint32_t size = vita_post_com_arg(c, 1U);

    if (!caps || size < ISAAC_VITA_POST_COM_CAPS_SIZE) {
        c->eax = ISAAC_VITA_POST_COM_TIMERR_NOCANDO;
    } else {
        st32(caps, ISAAC_VITA_POST_COM_PERIOD_MIN);
        st32(caps + 4U, ISAAC_VITA_POST_COM_PERIOD_MAX);
        c->eax = ISAAC_VITA_POST_COM_TIMERR_NOERROR;
    }
    vita_post_com_stdcall_return(c, 8U);
}

static void vita_post_com_timeBeginPeriod(CPU *__restrict c)
{
    uint32_t period = vita_post_com_arg(c, 0U);

    c->eax = period >= ISAAC_VITA_POST_COM_PERIOD_MIN &&
             period <= ISAAC_VITA_POST_COM_PERIOD_MAX
                 ? ISAAC_VITA_POST_COM_TIMERR_NOERROR
                 : ISAAC_VITA_POST_COM_TIMERR_NOCANDO;
    vita_post_com_stdcall_return(c, 4U);
}

static void vita_post_com_timeEndPeriod(CPU *__restrict c)
{
    uint32_t period = vita_post_com_arg(c, 0U);

    c->eax = period >= ISAAC_VITA_POST_COM_PERIOD_MIN &&
             period <= ISAAC_VITA_POST_COM_PERIOD_MAX
                 ? ISAAC_VITA_POST_COM_TIMERR_NOERROR
                 : ISAAC_VITA_POST_COM_TIMERR_NOCANDO;
    vita_post_com_stdcall_return(c, 4U);
}

#if defined(ISAAC_VITA_PHASE_PROFILE)
/* ph120.c tgt: every guest timeGetTime import call.  The KAGE clear/present
 * hook (sub_00564f50) loops over its pending-release list calling it once per
 * element per pass, a volume no other record captured. */
uint32_t g_isaac_vita_post_com_time_get_time_calls;
#endif

static void vita_post_com_timeGetTime(CPU *__restrict c)
{
#if defined(ISAAC_VITA_PHASE_PROFILE)
    ++g_isaac_vita_post_com_time_get_time_calls;
#endif
    /* timeGetTime is the low 32 bits of a monotonic millisecond counter.
     * Vita process time is monotonic microseconds and has the same wrap
     * behavior after this narrowing conversion. */
    c->eax = (uint32_t)(isaac_vita_get_process_time() / UINT64_C(1000));
    vita_post_com_stdcall_return(c, 0U);
}

static const vita_post_com_import_entry s_vita_post_com_imports[] = {
    { ISAAC_VITA_POST_COM_EXECUTION_STATE_NAME,
      vita_post_com_SetThreadExecutionState },
    { ISAAC_VITA_POST_COM_GET_CAPS_NAME, vita_post_com_timeGetDevCaps },
    { ISAAC_VITA_POST_COM_BEGIN_PERIOD_NAME, vita_post_com_timeBeginPeriod },
    { ISAAC_VITA_POST_COM_END_PERIOD_NAME, vita_post_com_timeEndPeriod },
    { ISAAC_VITA_POST_COM_GET_TIME_NAME, vita_post_com_timeGetTime },
    { ISAAC_VITA_POST_COM_STEAM_INIT_NAME, vita_post_com_SteamAPI_Init },
};

_Static_assert(sizeof s_vita_post_com_imports /
                   sizeof s_vita_post_com_imports[0] ==
                   ISAAC_VITA_POST_COM_IMPORT_COUNT,
               "Vita post-COM import inventory drifted");
_Static_assert(ISAAC_VITA_POST_COM_STEAM_INIT_ORDINAL +
                   ISAAC_VITA_POST_COM_BOOT_CALL_COUNT ==
                   ISAAC_VITA_POST_COM_NEXT_ORDINAL,
               "Vita post-COM boot denominator drifted");

const char *isaac_vita_post_com_import_name(uint32_t index)
{
    return index < ISAAC_VITA_POST_COM_IMPORT_COUNT
        ? s_vita_post_com_imports[index].name : NULL;
}

int isaac_vita_post_com_import_indexed(CPU *__restrict c, uint32_t index,
                                       unsigned *call_count)
{
    if (index >= ISAAC_VITA_POST_COM_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_post_com_imports[index].handler(c);
    return 1;
}

static int vita_post_com_dispatch(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_POST_COM_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_post_com_imports[index].name) == 0)
            return isaac_vita_post_com_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_post_com_import(CPU *__restrict c, const char *name)
{
    return vita_post_com_dispatch(c, name, NULL);
}

int isaac_vita_post_com_import_counted(CPU *__restrict c, const char *name,
                                       unsigned *call_count)
{
    return vita_post_com_dispatch(c, name, call_count);
}
