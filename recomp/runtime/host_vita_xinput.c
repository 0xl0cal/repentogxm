/* Synthetic XInput 1.4/1.3 boundary for Isaac's original libstem gamepad
 * path.  It consumes the producer-owned Vita pad cache; this file contains no
 * sceCtrl call and therefore cannot create a second sample within one tick. */
#include "host_vita_xinput.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ISAAC_VITA_XINPUT_ORACLE)
# include "kage_vita_input_test_ctrl.h"
#else
# include <psp2/ctrl.h>
#endif

_Static_assert(KAGE_VITA_INPUT_XINPUT_BACK == ISAAC_VITA_XINPUT_BACK,
               "Vita Start+R XInput Back token drifted");
_Static_assert(KAGE_VITA_INPUT_XINPUT_RIGHT_SHOULDER ==
                   ISAAC_VITA_XINPUT_RIGHT_SHOULDER,
               "Vita Start+L XInput right-shoulder token drifted");

static uint32_t vita_xinput_arg(CPU *__restrict c, uint32_t index)
{
    /* While an x86 stdcall is active, ESP still points at its return address. */
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_xinput_stdcall_return(CPU *__restrict c,
                                       uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static int vita_xinput_copy_ascii(
    uint32_t address,
    char output[ISAAC_VITA_XINPUT_PROCEDURE_NAME_MAX + 1U])
{
    uint32_t i;

    if (!address || (address >> 16U) == 0U)
        return 0;
    for (i = 0U; i <= ISAAC_VITA_XINPUT_PROCEDURE_NAME_MAX; ++i) {
        uint8_t value;
        if (address > UINT32_MAX - i)
            return 0;
        value = ld8(address + i);
        if (!value) {
            output[i] = '\0';
            return 1;
        }
        if (value > 0x7fU || i == ISAAC_VITA_XINPUT_PROCEDURE_NAME_MAX)
            return 0;
        output[i] = (char)value;
    }
    return 0;
}

static unsigned char vita_xinput_ascii_fold(unsigned char value)
{
    if (value >= 'A' && value <= 'Z')
        return (unsigned char)(value + ('a' - 'A'));
    return value;
}

static int vita_xinput_ascii_case_equal(const char *left, const char *right)
{
    if (!left || !right)
        return 0;
    for (;;) {
        unsigned char a = vita_xinput_ascii_fold((unsigned char)*left++);
        unsigned char b = vita_xinput_ascii_fold((unsigned char)*right++);
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
}

uint32_t isaac_vita_xinput_module_for_name(const char *name)
{
    if (vita_xinput_ascii_case_equal(name, ISAAC_VITA_XINPUT_14_NAME) ||
        vita_xinput_ascii_case_equal(name, ISAAC_VITA_XINPUT_13_NAME) ||
        vita_xinput_ascii_case_equal(name, ISAAC_VITA_XINPUT_13_BIN_NAME))
        return ISAAC_VITA_XINPUT_MODULE_TOKEN;
    return 0U;
}

uint32_t isaac_vita_xinput_token_for_name(const char *name)
{
    if (!name)
        return 0U;
    if (strcmp(name, ISAAC_VITA_XINPUT_GET_STATE_NAME) == 0)
        return ISAAC_VITA_XINPUT_GET_STATE_TOKEN;
    if (strcmp(name, ISAAC_VITA_XINPUT_GET_CAPS_NAME) == 0)
        return ISAAC_VITA_XINPUT_GET_CAPS_TOKEN;
    return 0U;
}

uint32_t isaac_vita_xinput_token_for_ordinal(uint32_t ordinal)
{
    return ordinal == ISAAC_VITA_XINPUT_GET_STATE_EX_ORDINAL
        ? ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN : 0U;
}

int16_t isaac_vita_xinput_axis(uint8_t value, int inverted)
{
    int32_t result;

    /* Vita's documented neutral sample is 128.  Piecewise scaling keeps that
     * exact while still reaching both signed XInput endpoints. */
    if (!inverted) {
        if (value < 128U)
            result = -32768 + (int32_t)value * 256;
        else
            result = ((int32_t)value - 128) * 32767 / 127;
    } else {
        /* Vita Y grows downward; XInput Y grows upward. */
        if (value < 128U)
            result = (128 - (int32_t)value) * 32767 / 128;
        else
            result = -((int32_t)value - 128) * 32768 / 127;
    }
    return (int16_t)result;
}

void isaac_vita_xinput_build_state(
    const kage_vita_gamepad_snapshot *snapshot,
    isaac_vita_xinput_state *state)
{
    uint32_t buttons;

    if (!snapshot || !state)
        return;
    memset(state, 0, sizeof *state);
    buttons = snapshot->buttons;
    state->dwPacketNumber = snapshot->packet_number;

    if (buttons & SCE_CTRL_UP)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_DPAD_UP;
    if (buttons & SCE_CTRL_DOWN)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_DPAD_DOWN;
    if (buttons & SCE_CTRL_LEFT)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_DPAD_LEFT;
    if (buttons & SCE_CTRL_RIGHT)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_DPAD_RIGHT;
    if (buttons & SCE_CTRL_START)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_START;
    if (buttons & SCE_CTRL_LTRIGGER)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_LEFT_SHOULDER;
    if (buttons & SCE_CTRL_CROSS)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_A;
    if (buttons & SCE_CTRL_CIRCLE)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_B;
    if (buttons & SCE_CTRL_SQUARE)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_X;
    if (buttons & SCE_CTRL_TRIANGLE)
        state->Gamepad.wButtons |= ISAAC_VITA_XINPUT_Y;

    /* Frozen guest defaults: LT is ACTION_ACTIVE, RB is ACTION_PILL_CARD,
     * BACK is ACTION_MAP and RT is ACTION_DROP.  Physical Vita R directly
     * publishes LT.  The cached producer supplies synthetic RB/BACK for its
     * Start-prefixed controls; Select remains a level RT signal for the
     * guest's own drop timing. */
    state->Gamepad.wButtons |= snapshot->xinput_buttons;
    state->Gamepad.bLeftTrigger = snapshot->xinput_left_trigger;
    if (buttons & SCE_CTRL_RTRIGGER)
        state->Gamepad.bLeftTrigger =
            KAGE_VITA_INPUT_XINPUT_TRIGGER_FULL;
    if (buttons & SCE_CTRL_SELECT)
        state->Gamepad.bRightTrigger = 255U;

    state->Gamepad.sThumbLX = isaac_vita_xinput_axis(snapshot->lx, 0);
    state->Gamepad.sThumbLY = isaac_vita_xinput_axis(snapshot->ly, 1);
    state->Gamepad.sThumbRX = isaac_vita_xinput_axis(snapshot->rx, 0);
    state->Gamepad.sThumbRY = isaac_vita_xinput_axis(snapshot->ry, 1);
}

void isaac_vita_xinput_build_capabilities(
    isaac_vita_xinput_capabilities *capabilities)
{
    if (!capabilities)
        return;
    memset(capabilities, 0, sizeof *capabilities);
    capabilities->Type = ISAAC_VITA_XINPUT_DEVTYPE_GAMEPAD;
    capabilities->SubType = ISAAC_VITA_XINPUT_DEVSUBTYPE_GAMEPAD;
    capabilities->Gamepad.wButtons =
        ISAAC_VITA_XINPUT_DPAD_UP |
        ISAAC_VITA_XINPUT_DPAD_DOWN |
        ISAAC_VITA_XINPUT_DPAD_LEFT |
        ISAAC_VITA_XINPUT_DPAD_RIGHT |
        ISAAC_VITA_XINPUT_BACK |
        ISAAC_VITA_XINPUT_START |
        ISAAC_VITA_XINPUT_LEFT_SHOULDER |
        ISAAC_VITA_XINPUT_RIGHT_SHOULDER |
        ISAAC_VITA_XINPUT_A |
        ISAAC_VITA_XINPUT_B |
        ISAAC_VITA_XINPUT_X |
        ISAAC_VITA_XINPUT_Y;
    capabilities->Gamepad.bLeftTrigger = 255U;
    capabilities->Gamepad.bRightTrigger = 255U;
    /* All mapped stick endpoints and intermediate output bits are live. */
    capabilities->Gamepad.sThumbLX = -1;
    capabilities->Gamepad.sThumbLY = -1;
    capabilities->Gamepad.sThumbRX = -1;
    capabilities->Gamepad.sThumbRY = -1;
}

static void vita_xinput_store_gamepad(
    uint32_t output, const isaac_vita_xinput_gamepad *gamepad)
{
    st16(output + 0U, gamepad->wButtons);
    st8(output + 2U, gamepad->bLeftTrigger);
    st8(output + 3U, gamepad->bRightTrigger);
    st16(output + 4U, (uint16_t)gamepad->sThumbLX);
    st16(output + 6U, (uint16_t)gamepad->sThumbLY);
    st16(output + 8U, (uint16_t)gamepad->sThumbRX);
    st16(output + 10U, (uint16_t)gamepad->sThumbRY);
}

static void vita_xinput_store_state(
    uint32_t output, const isaac_vita_xinput_state *state, int extended)
{
    st32(output + 0U, state->dwPacketNumber);
    vita_xinput_store_gamepad(output + 4U, &state->Gamepad);
    if (extended)
        st32(output + 16U, 0U);
}

static void vita_xinput_store_capabilities(
    uint32_t output, const isaac_vita_xinput_capabilities *capabilities)
{
    st8(output + 0U, capabilities->Type);
    st8(output + 1U, capabilities->SubType);
    st16(output + 2U, capabilities->Flags);
    vita_xinput_store_gamepad(output + 4U, &capabilities->Gamepad);
    st16(output + 16U, capabilities->Vibration.wLeftMotorSpeed);
    st16(output + 18U, capabilities->Vibration.wRightMotorSpeed);
}

static int vita_xinput_import_dispatch(CPU *__restrict c, const char *name,
                                       unsigned *call_count)
{
    uint32_t argument;
    uint32_t token;
    char value[ISAAC_VITA_XINPUT_PROCEDURE_NAME_MAX + 1U];

    if (!c || !name)
        return 0;

    if (strcmp(name, ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME) == 0) {
        argument = vita_xinput_arg(c, 0U);
        if (!vita_xinput_copy_ascii(argument, value) ||
            !isaac_vita_xinput_module_for_name(value))
            return 0;
        if (call_count)
            ++*call_count;
        kage_vita_input_set_xinput_active(1);
        c->eax = ISAAC_VITA_XINPUT_MODULE_TOKEN;
        vita_xinput_stdcall_return(c, 4U);
        return 1;
    }

    if (strcmp(name, ISAAC_VITA_XINPUT_GET_PROC_NAME) == 0) {
        if (vita_xinput_arg(c, 0U) != ISAAC_VITA_XINPUT_MODULE_TOKEN)
            return 0;
        argument = vita_xinput_arg(c, 1U);
        if ((argument >> 16U) == 0U) {
            token = isaac_vita_xinput_token_for_ordinal(argument);
        } else {
            token = vita_xinput_copy_ascii(argument, value)
                ? isaac_vita_xinput_token_for_name(value) : 0U;
        }
        if (call_count)
            ++*call_count;
        c->eax = token;
        vita_xinput_stdcall_return(c, 8U);
        return 1;
    }

    if (strcmp(name, ISAAC_VITA_XINPUT_FREE_LIBRARY_NAME) == 0) {
        if (vita_xinput_arg(c, 0U) != ISAAC_VITA_XINPUT_MODULE_TOKEN)
            return 0;
        if (call_count)
            ++*call_count;
        kage_vita_input_set_xinput_active(0);
        c->eax = 1U;
        vita_xinput_stdcall_return(c, 4U);
        return 1;
    }
    return 0;
}

int isaac_vita_xinput_import(CPU *__restrict c, const char *name)
{
    return vita_xinput_import_dispatch(c, name, NULL);
}

int isaac_vita_xinput_import_counted(CPU *__restrict c, const char *name,
                                     unsigned *call_count)
{
    return vita_xinput_import_dispatch(c, name, call_count);
}

static void vita_xinput_get_state(CPU *__restrict c, uint32_t token,
                                  int extended)
{
    uint32_t user_index = vita_xinput_arg(c, 0U);
    uint32_t output = vita_xinput_arg(c, 1U);
    kage_vita_gamepad_snapshot snapshot;
    isaac_vita_xinput_state state;

    if (user_index > 3U) {
        guest_fault(c, token, "XInputGetState user index outside frozen ABI");
        return;
    }
    if (!output) {
        guest_fault(c, token, "XInputGetState received a null output");
        return;
    }
    if (user_index != 0U ||
        !kage_vita_input_gamepad_snapshot(&snapshot)) {
        c->eax = ISAAC_VITA_XINPUT_ERROR_DEVICE_NOT_CONNECTED;
        vita_xinput_stdcall_return(c, 8U);
        return;
    }
    isaac_vita_xinput_build_state(&snapshot, &state);
    vita_xinput_store_state(output, &state, extended);
    c->eax = ISAAC_VITA_XINPUT_ERROR_SUCCESS;
    vita_xinput_stdcall_return(c, 8U);
}

static void vita_xinput_get_capabilities(CPU *__restrict c, uint32_t token)
{
    uint32_t user_index = vita_xinput_arg(c, 0U);
    uint32_t flags = vita_xinput_arg(c, 1U);
    uint32_t output = vita_xinput_arg(c, 2U);
    kage_vita_gamepad_snapshot snapshot;
    isaac_vita_xinput_capabilities capabilities;

    if (user_index > 3U) {
        guest_fault(c, token,
                    "XInputGetCapabilities user index outside frozen ABI");
        return;
    }
    if (flags != 0U && flags != ISAAC_VITA_XINPUT_FLAG_GAMEPAD) {
        guest_fault(c, token,
                    "XInputGetCapabilities flags outside frozen ABI");
        return;
    }
    if (!output) {
        guest_fault(c, token,
                    "XInputGetCapabilities received a null output");
        return;
    }
    if (user_index != 0U ||
        !kage_vita_input_gamepad_snapshot(&snapshot)) {
        c->eax = ISAAC_VITA_XINPUT_ERROR_DEVICE_NOT_CONNECTED;
        vita_xinput_stdcall_return(c, 12U);
        return;
    }
    isaac_vita_xinput_build_capabilities(&capabilities);
    vita_xinput_store_capabilities(output, &capabilities);
    c->eax = ISAAC_VITA_XINPUT_ERROR_SUCCESS;
    vita_xinput_stdcall_return(c, 12U);
}

static int vita_xinput_dynamic_dispatch(CPU *__restrict c, uint32_t token,
                                        unsigned *call_count)
{
    if (!c)
        return 0;
    if (token != ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN &&
        token != ISAAC_VITA_XINPUT_GET_STATE_TOKEN &&
        token != ISAAC_VITA_XINPUT_GET_CAPS_TOKEN)
        return 0;
    if (call_count)
        ++*call_count;

    if (token == ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN)
        vita_xinput_get_state(c, token, 1);
    else if (token == ISAAC_VITA_XINPUT_GET_STATE_TOKEN)
        vita_xinput_get_state(c, token, 0);
    else
        vita_xinput_get_capabilities(c, token);
    return 1;
}

int isaac_vita_xinput_dynamic(CPU *__restrict c, uint32_t token)
{
    return vita_xinput_dynamic_dispatch(c, token, NULL);
}

int isaac_vita_xinput_dynamic_counted(CPU *__restrict c, uint32_t token,
                                      unsigned *call_count)
{
    return vita_xinput_dynamic_dispatch(c, token, call_count);
}

_Static_assert((ISAAC_VITA_XINPUT_MODULE_TOKEN & UINT32_C(0xff000000)) ==
               UINT32_C(0x7f000000),
               "XInput module token left the synthetic module family");
_Static_assert((ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN &
                UINT32_C(0xffff0000)) == UINT32_C(0x7d200000) &&
               (ISAAC_VITA_XINPUT_GET_STATE_TOKEN &
                UINT32_C(0xffff0000)) == UINT32_C(0x7d200000) &&
               (ISAAC_VITA_XINPUT_GET_CAPS_TOKEN &
                UINT32_C(0xffff0000)) == UINT32_C(0x7d200000),
               "XInput calls left their reserved dynamic-token family");
