/* Evidence-bounded USER32 compatibility for Vita's windowless KAGE backend. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_user32.h"
#include "host_vita_import_id.h"

typedef void (*vita_user32_handler)(CPU *__restrict c);

typedef struct vita_user32_import_entry {
    const char *name;
    vita_user32_handler handler;
} vita_user32_import_entry;

static uint32_t vita_user32_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_user32_stdcall_return(CPU *__restrict c,
                                       uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static int vita_user32_is_return_site(CPU *__restrict c, uint32_t rva)
{
    uint32_t address = guest_stack_address(c, c->esp, 4U, 0U);
    uint32_t return_word;

    if (!address)
        return 0;
    return_word = ld32(address);
    return return_word == rva ||
           return_word == (uint32_t)GUEST_IMAGE_BASE + rva;
}

static void vita_user32_GetSystemMetrics(CPU *__restrict c)
{
    uint32_t index = vita_user32_arg(c, 0U);
    uint32_t value;

    switch (index) {
    case ISAAC_VITA_USER32_SM_CXSCREEN:
        value = ISAAC_VITA_USER32_SCREEN_WIDTH;
        break;
    case ISAAC_VITA_USER32_SM_CYSCREEN:
        value = ISAAC_VITA_USER32_SCREEN_HEIGHT;
        break;
    case ISAAC_VITA_USER32_SM_CXICON:
        value = ISAAC_VITA_USER32_ICON_WIDTH;
        break;
    case ISAAC_VITA_USER32_SM_CYICON:
        value = ISAAC_VITA_USER32_ICON_HEIGHT;
        break;
    case ISAAC_VITA_USER32_SM_CXSMICON:
        value = ISAAC_VITA_USER32_SMALL_ICON_WIDTH;
        break;
    case ISAAC_VITA_USER32_SM_CYSMICON:
        value = ISAAC_VITA_USER32_SMALL_ICON_HEIGHT;
        break;
    default:
        guest_fault(c, index,
                    "GetSystemMetrics outside the measured selectors");
        return;
    }
    c->eax = value;
    vita_user32_stdcall_return(c, 4U);
}

static int vita_user32_is_icon(uint32_t token, uint32_t icon_kind)
{
    uint32_t bias = icon_kind == ISAAC_VITA_USER32_ICON_SMALL
        ? ISAAC_VITA_USER32_HICON_SMALL_BIAS : 0U;
    return token == ISAAC_VITA_USER32_HICON_ISAAC + bias ||
           token == ISAAC_VITA_USER32_HICON_ISAAC_ALT + bias;
}

static void vita_user32_LoadImageA(CPU *__restrict c)
{
    uint32_t instance = vita_user32_arg(c, 0U);
    uint32_t resource = vita_user32_arg(c, 1U);
    uint32_t type = vita_user32_arg(c, 2U);
    uint32_t width = vita_user32_arg(c, 3U);
    uint32_t height = vita_user32_arg(c, 4U);
    uint32_t flags = vita_user32_arg(c, 5U);
    int large = width == ISAAC_VITA_USER32_ICON_WIDTH &&
                height == ISAAC_VITA_USER32_ICON_HEIGHT;
    int small = width == ISAAC_VITA_USER32_SMALL_ICON_WIDTH &&
                height == ISAAC_VITA_USER32_SMALL_ICON_HEIGHT;
    uint32_t token;

    if (instance != GUEST_IMAGE_BASE ||
        (resource != 101U && resource != 104U) ||
        type != ISAAC_VITA_USER32_IMAGE_ICON || flags != 0U ||
        (!large && !small)) {
        guest_fault(c, resource,
                    "LoadImageA outside the measured Isaac icons");
        return;
    }
    token = resource == 101U ? ISAAC_VITA_USER32_HICON_ISAAC
                             : ISAAC_VITA_USER32_HICON_ISAAC_ALT;
    if (small)
        token += ISAAC_VITA_USER32_HICON_SMALL_BIAS;
    c->eax = token;
    vita_user32_stdcall_return(c, 24U);
}

static void vita_user32_SendMessageA(CPU *__restrict c)
{
    uint32_t window = vita_user32_arg(c, 0U);
    uint32_t message = vita_user32_arg(c, 1U);
    uint32_t icon_kind = vita_user32_arg(c, 2U);
    uint32_t icon = vita_user32_arg(c, 3U);

    if (window != 0U || message != ISAAC_VITA_USER32_WM_SETICON ||
        icon_kind > ISAAC_VITA_USER32_ICON_BIG ||
        !vita_user32_is_icon(icon, icon_kind)) {
        guest_fault(c, window,
                    "SendMessageA needs the measured NULL-HWND icon request");
        return;
    }
    c->eax = 0U;
    vita_user32_stdcall_return(c, 16U);
}

static void vita_user32_SetWindowPos(CPU *__restrict c)
{
    uint32_t window;
    uint32_t insert_after;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;

    if (!vita_user32_is_return_site(
            c, ISAAC_VITA_USER32_FULLSCREEN_RETURN_RVA)) {
        if (!c->fault)
            guest_fault(c, c->esp,
                        "SetWindowPos outside OptionsConfig::SetFullScreen");
        return;
    }
    window = vita_user32_arg(c, 0U);
    insert_after = vita_user32_arg(c, 1U);
    x = vita_user32_arg(c, 2U);
    y = vita_user32_arg(c, 3U);
    width = vita_user32_arg(c, 4U);
    height = vita_user32_arg(c, 5U);
    flags = vita_user32_arg(c, 6U);

    /* The fixed Vita display has no HWND to resize.  Accept only the exact
     * desktop-sized frame-change emitted by the frozen SetFullScreen body,
     * leave the 960x544 surface untouched, and report Win32 TRUE. */
    if (window != 0U || insert_after != 0U || x != 0U || y != 0U ||
        width != ISAAC_VITA_USER32_FULLSCREEN_WIDTH ||
        height != ISAAC_VITA_USER32_FULLSCREEN_HEIGHT ||
        flags != ISAAC_VITA_USER32_SWP_FRAMECHANGED) {
        guest_fault(c, width,
                    "SetWindowPos outside the measured fullscreen request");
        return;
    }
    c->eax = 1U;
    vita_user32_stdcall_return(c, 28U);
}

static void vita_user32_GetWindowLongA(CPU *__restrict c)
{
    uint32_t window = vita_user32_arg(c, 0U);
    uint32_t index = vita_user32_arg(c, 1U);

    if (window != 0U || index != ISAAC_VITA_USER32_GWL_STYLE) {
        guest_fault(c, window,
                    "GetWindowLongA needs the measured NULL HWND/style index");
        return;
    }
    c->eax = 0U;
    vita_user32_stdcall_return(c, 8U);
}

static void vita_user32_SetWindowLongA(CPU *__restrict c)
{
    uint32_t window = vita_user32_arg(c, 0U);
    uint32_t index = vita_user32_arg(c, 1U);

    /* KAGE's Vita backend owns one immutable fullscreen surface.  Mirror the
     * existing Win32 host policy: accept the game's NULL-HWND style update,
     * leave the physical display alone, and report a zero previous style.
     * Other SetWindowLong uses remain loud instead of becoming a broad stub. */
    if (window != 0U || index != ISAAC_VITA_USER32_GWL_STYLE) {
        guest_fault(c, window,
                    "SetWindowLongA needs the measured NULL HWND/style index");
        return;
    }
    c->eax = 0U;
    vita_user32_stdcall_return(c, 12U);
}

static const vita_user32_import_entry s_vita_user32_imports[] = {
    { ISAAC_VITA_USER32_GET_WINDOW_LONG_NAME,
      vita_user32_GetWindowLongA },
    { ISAAC_VITA_USER32_LOAD_IMAGE_NAME, vita_user32_LoadImageA },
    { ISAAC_VITA_USER32_SEND_MESSAGE_NAME, vita_user32_SendMessageA },
    { ISAAC_VITA_USER32_SET_WINDOW_POS_NAME,
      vita_user32_SetWindowPos },
    { ISAAC_VITA_USER32_GET_SYSTEM_METRICS_NAME,
      vita_user32_GetSystemMetrics },
    { ISAAC_VITA_USER32_SET_WINDOW_LONG_NAME,
      vita_user32_SetWindowLongA },
};

_Static_assert(sizeof s_vita_user32_imports /
                   sizeof s_vita_user32_imports[0] ==
                   ISAAC_VITA_USER32_IMPORT_COUNT,
               "Vita USER32 import inventory drifted");
_Static_assert(ISAAC_VITA_USER32_GET_WINDOW_LONG_CALL_COUNT +
                   ISAAC_VITA_USER32_LOAD_IMAGE_CALL_COUNT +
                   ISAAC_VITA_USER32_SEND_MESSAGE_CALL_COUNT +
                   ISAAC_VITA_USER32_SET_WINDOW_POS_CALL_COUNT +
                   ISAAC_VITA_USER32_GET_SYSTEM_METRICS_CALL_COUNT +
                   ISAAC_VITA_USER32_SET_WINDOW_LONG_CALL_COUNT ==
                   ISAAC_VITA_USER32_PHYSICAL_CALL_COUNT,
               "Vita USER32 physical-call census drifted");

const char *isaac_vita_user32_import_name(uint32_t index)
{
    return index < ISAAC_VITA_USER32_IMPORT_COUNT
        ? s_vita_user32_imports[index].name : NULL;
}

int isaac_vita_user32_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count)
{
    if (index >= ISAAC_VITA_USER32_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_user32_imports[index].handler(c);
    return 1;
}

static int vita_user32_dispatch(CPU *__restrict c, const char *name,
                                unsigned *call_count)
{
    size_t i;

    if (!name)
        return 0;
    for (i = 0U; i < ISAAC_VITA_USER32_IMPORT_COUNT; ++i) {
        if (strcmp(name, s_vita_user32_imports[i].name) == 0)
            return isaac_vita_user32_import_indexed(
                c, (uint32_t)i, call_count);
    }
    return 0;
}

int isaac_vita_user32_import(CPU *__restrict c, const char *name)
{
    return vita_user32_dispatch(c, name, NULL);
}

int isaac_vita_user32_import_counted(CPU *__restrict c, const char *name,
                                     unsigned *call_count)
{
    return vita_user32_dispatch(c, name, call_count);
}
