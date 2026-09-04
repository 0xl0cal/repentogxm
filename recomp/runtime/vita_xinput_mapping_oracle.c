#define _GNU_SOURCE 1

/* Executable host oracle for the synthetic Vita -> XInput policy.  A private
 * fixed low mapping makes the identity-mapped x86 stack/pointers executable
 * even in a 64-bit host process, so resolver, stdcall, stores, errors and
 * delegation are tested here as behavior.  ARM softfp remains a separate
 * compile/link ABI gate. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "host_vita_xinput.h"
#include "kage_vita_input_test_ctrl.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita XInput oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static kage_vita_gamepad_snapshot s_dispatch_snapshot;
static int s_dispatch_available;
static int s_dispatch_xinput_active;
static unsigned s_dispatch_xinput_set_calls;

int kage_vita_input_gamepad_snapshot(kage_vita_gamepad_snapshot *snapshot)
{
    if (!snapshot)
        return 0;
    *snapshot = s_dispatch_snapshot;
    return s_dispatch_available;
}

void kage_vita_input_set_xinput_active(int active)
{
    s_dispatch_xinput_active = active != 0;
    ++s_dispatch_xinput_set_calls;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

#define ORACLE_GUEST_BASE UINT32_C(0x18000000)
#define ORACLE_GUEST_SIZE UINT32_C(0x00010000)
#define ORACLE_STACK_TOP  (ORACLE_GUEST_BASE + UINT32_C(0x0000f000))

static int oracle_guest_mapping_open(void)
{
    void *requested = (void *)(uintptr_t)ORACLE_GUEST_BASE;
    void *mapping = mmap(requested, ORACLE_GUEST_SIZE,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1, 0);

    return mapping != MAP_FAILED && mapping == requested;
}

static void oracle_guest_mapping_close(void)
{
    (void)munmap((void *)(uintptr_t)ORACLE_GUEST_BASE,
                 ORACLE_GUEST_SIZE);
}

static void oracle_guest_copy(uint32_t address, const void *source,
                              size_t size)
{
    memcpy((void *)(uintptr_t)address, source, size);
}

static void oracle_push_frame(CPU *cpu, const uint32_t *arguments,
                              uint32_t argument_count,
                              uint32_t return_address)
{
    uint32_t index;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, return_address);
}

static int oracle_import_call(CPU *cpu, const char *name,
                              const uint32_t *arguments,
                              uint32_t argument_count, unsigned *calls,
                              uint32_t *result)
{
    uint32_t original_esp = cpu->esp;

    cpu->fault = NULL;
    cpu->fault_addr = 0U;
    oracle_push_frame(cpu, arguments, argument_count,
                      UINT32_C(0xaabbccdd));
    if (!isaac_vita_xinput_import_counted(cpu, name, calls) ||
        cpu->esp != original_esp || cpu->fault) {
        cpu->esp = original_esp;
        return 0;
    }
    *result = cpu->eax;
    return 1;
}

static int oracle_delegated_import(CPU *cpu, const char *name,
                                   const uint32_t *arguments,
                                   uint32_t argument_count,
                                   unsigned *calls)
{
    CPU original = *cpu;
    CPU entry;
    unsigned before = *calls;
    int delegated;

    oracle_push_frame(cpu, arguments, argument_count,
                      UINT32_C(0x12345678));
    entry = *cpu;
    delegated =
        isaac_vita_xinput_import_counted(cpu, name, calls) == 0 &&
        *calls == before && memcmp(cpu, &entry, sizeof entry) == 0;
    *cpu = original;
    return delegated;
}

static int oracle_dynamic_call(CPU *cpu, uint32_t token,
                               const uint32_t *arguments,
                               uint32_t argument_count, unsigned *calls,
                               uint32_t *result)
{
    uint32_t original_esp = cpu->esp;

    cpu->fault = NULL;
    cpu->fault_addr = 0U;
    oracle_push_frame(cpu, arguments, argument_count,
                      UINT32_C(0xddccbbaa));
    if (!isaac_vita_xinput_dynamic_counted(cpu, token, calls) ||
        cpu->esp != original_esp || cpu->fault) {
        cpu->esp = original_esp;
        return 0;
    }
    *result = cpu->eax;
    return 1;
}

static int oracle_faulting_dynamic_call(CPU *cpu, uint32_t token,
                                        const uint32_t *arguments,
                                        uint32_t argument_count,
                                        unsigned *calls,
                                        const char *fault_substring)
{
    CPU original = *cpu;
    uint32_t entry_esp;
    unsigned before = *calls;
    int correct;

    cpu->fault = NULL;
    cpu->fault_addr = 0U;
    oracle_push_frame(cpu, arguments, argument_count,
                      UINT32_C(0xdeadbeef));
    entry_esp = cpu->esp;
    correct = isaac_vita_xinput_dynamic_counted(cpu, token, calls) == 1 &&
              *calls == before + 1U && cpu->esp == entry_esp &&
              cpu->fault_addr == token && cpu->fault &&
              strstr(cpu->fault, fault_substring) != NULL;
    *cpu = original;
    return correct;
}

static int oracle_bytes_are(uint32_t address, size_t size, uint8_t value)
{
    const uint8_t *bytes = (const uint8_t *)(uintptr_t)address;
    size_t index;

    for (index = 0U; index < size; ++index)
        if (bytes[index] != value)
            return 0;
    return 1;
}

static int oracle_dispatch(void)
{
    static const char module[] = ISAAC_VITA_XINPUT_14_NAME;
    static const char rejected_module[] = "DINPUT8.dll";
    static const char state_name[] = ISAAC_VITA_XINPUT_GET_STATE_NAME;
    static const char caps_name[] = ISAAC_VITA_XINPUT_GET_CAPS_NAME;
    static const char missing_name[] = "XInputSetState";
    const uint32_t module_address = ORACLE_GUEST_BASE + 0x0100U;
    const uint32_t rejected_address = ORACLE_GUEST_BASE + 0x0140U;
    const uint32_t state_name_address = ORACLE_GUEST_BASE + 0x0180U;
    const uint32_t caps_name_address = ORACLE_GUEST_BASE + 0x01c0U;
    const uint32_t missing_name_address = ORACLE_GUEST_BASE + 0x0200U;
    const uint32_t output = ORACLE_GUEST_BASE + 0x1000U;
    uint32_t arguments[3];
    uint32_t result;
    uint32_t module_token;
    uint32_t state_ex_token;
    uint32_t state_token;
    uint32_t caps_token;
    unsigned imports = 0U;
    unsigned dynamics = 0U;
    CPU cpu;
    CPU untouched;
    const isaac_vita_xinput_state *state;
    const isaac_vita_xinput_state_ex *state_ex;
    const isaac_vita_xinput_capabilities *capabilities;
    int ok = 1;

    if (!oracle_guest_mapping_open())
        return 0;
    memset(&cpu, 0, sizeof cpu);
    cpu.esp = ORACLE_STACK_TOP;
    oracle_guest_copy(module_address, module, sizeof module);
    oracle_guest_copy(rejected_address, rejected_module,
                      sizeof rejected_module);
    oracle_guest_copy(state_name_address, state_name, sizeof state_name);
    oracle_guest_copy(caps_name_address, caps_name, sizeof caps_name);
    oracle_guest_copy(missing_name_address, missing_name,
                      sizeof missing_name);

    arguments[0] = rejected_address;
    ok = ok && oracle_delegated_import(
        &cpu, ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME,
        arguments, 1U, &imports);
    arguments[0] = module_address;
    ok = ok && oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME,
        arguments, 1U, &imports, &module_token);
    ok = ok && module_token == ISAAC_VITA_XINPUT_MODULE_TOKEN &&
         s_dispatch_xinput_active && s_dispatch_xinput_set_calls == 1U;

    arguments[0] = UINT32_C(0x7f000099);
    arguments[1] = state_name_address;
    ok = ok && oracle_delegated_import(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports);

    arguments[0] = module_token;
    arguments[1] = ISAAC_VITA_XINPUT_GET_STATE_EX_ORDINAL;
    ok = ok && oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &state_ex_token);
    arguments[1] = state_name_address;
    ok = ok && oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &state_token);
    arguments[1] = caps_name_address;
    ok = ok && oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &caps_token);
    arguments[1] = missing_name_address;
    ok = ok && oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &result) && result == 0U;
    ok = ok && state_ex_token == ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN &&
         state_token == ISAAC_VITA_XINPUT_GET_STATE_TOKEN &&
         caps_token == ISAAC_VITA_XINPUT_GET_CAPS_TOKEN;

    memset(&s_dispatch_snapshot, 0, sizeof s_dispatch_snapshot);
    s_dispatch_snapshot.packet_number = UINT32_C(0x12345678);
    s_dispatch_snapshot.buttons = SCE_CTRL_CROSS | SCE_CTRL_SELECT;
    s_dispatch_snapshot.xinput_buttons = ISAAC_VITA_XINPUT_BACK;
    s_dispatch_snapshot.xinput_left_trigger = 255U;
    s_dispatch_snapshot.lx = 0U;
    s_dispatch_snapshot.ly = 0U;
    s_dispatch_snapshot.rx = 255U;
    s_dispatch_snapshot.ry = 255U;
    s_dispatch_available = 1;

    memset((void *)(uintptr_t)output, 0xcc, 32U);
    arguments[0] = 0U;
    arguments[1] = 0U;
    arguments[2] = output;
    ok = ok && oracle_dynamic_call(
        &cpu, caps_token, arguments, 3U, &dynamics, &result) &&
         result == ISAAC_VITA_XINPUT_ERROR_SUCCESS;
    capabilities = (const isaac_vita_xinput_capabilities *)(uintptr_t)output;
    ok = ok && capabilities->Type == ISAAC_VITA_XINPUT_DEVTYPE_GAMEPAD &&
         capabilities->SubType == ISAAC_VITA_XINPUT_DEVSUBTYPE_GAMEPAD &&
         (capabilities->Gamepad.wButtons & ISAAC_VITA_XINPUT_BACK) != 0U &&
         capabilities->Gamepad.bLeftTrigger == 255U &&
         capabilities->Gamepad.bRightTrigger == 255U &&
         oracle_bytes_are(output + sizeof *capabilities,
                          32U - sizeof *capabilities, 0xccU);

    memset((void *)(uintptr_t)output, 0xcc, 32U);
    arguments[0] = 0U;
    arguments[1] = output;
    ok = ok && oracle_dynamic_call(
        &cpu, state_ex_token, arguments, 2U, &dynamics, &result) &&
         result == ISAAC_VITA_XINPUT_ERROR_SUCCESS;
    state_ex = (const isaac_vita_xinput_state_ex *)(uintptr_t)output;
    ok = ok && state_ex->dwPacketNumber == UINT32_C(0x12345678) &&
         state_ex->Gamepad.wButtons ==
             (ISAAC_VITA_XINPUT_A | ISAAC_VITA_XINPUT_BACK) &&
         state_ex->Gamepad.bLeftTrigger == 255U &&
         state_ex->Gamepad.bRightTrigger == 255U &&
         state_ex->Gamepad.sThumbLX == -32768 &&
         state_ex->Gamepad.sThumbLY == 32767 &&
         state_ex->Gamepad.sThumbRX == 32767 &&
         state_ex->Gamepad.sThumbRY == -32768 &&
         state_ex->dwPaddingReserved == 0U &&
         oracle_bytes_are(output + sizeof *state_ex,
                          32U - sizeof *state_ex, 0xccU);

    memset((void *)(uintptr_t)output, 0xcc, 32U);
    ok = ok && oracle_dynamic_call(
        &cpu, state_token, arguments, 2U, &dynamics, &result) &&
         result == ISAAC_VITA_XINPUT_ERROR_SUCCESS;
    state = (const isaac_vita_xinput_state *)(uintptr_t)output;
    ok = ok && state->dwPacketNumber == UINT32_C(0x12345678) &&
         state->Gamepad.wButtons ==
             (ISAAC_VITA_XINPUT_A | ISAAC_VITA_XINPUT_BACK) &&
         state->Gamepad.bLeftTrigger == 255U &&
         state->Gamepad.bRightTrigger == 255U &&
         oracle_bytes_are(output + sizeof *state,
                          32U - sizeof *state, 0xccU);

    memset((void *)(uintptr_t)output, 0xcc, 32U);
    arguments[0] = 1U;
    arguments[1] = output;
    ok = ok && oracle_dynamic_call(
        &cpu, state_token, arguments, 2U, &dynamics, &result) &&
         result == ISAAC_VITA_XINPUT_ERROR_DEVICE_NOT_CONNECTED &&
         oracle_bytes_are(output, 32U, 0xccU);

    arguments[0] = 0U;
    arguments[1] = 0U;
    ok = ok && oracle_faulting_dynamic_call(
        &cpu, state_token, arguments, 2U, &dynamics,
        "null output");

    untouched = cpu;
    ok = ok && isaac_vita_xinput_dynamic_counted(
        &cpu, UINT32_C(0x12345678), &dynamics) == 0 &&
         dynamics == 5U && memcmp(&cpu, &untouched, sizeof cpu) == 0;

    arguments[0] = module_token;
    ok = ok && oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_FREE_LIBRARY_NAME,
        arguments, 1U, &imports, &result) && result == 1U &&
         !s_dispatch_xinput_active && s_dispatch_xinput_set_calls == 2U;
    ok = ok && imports == 6U && dynamics == 5U;
    oracle_guest_mapping_close();
    return ok;
}

static isaac_vita_xinput_state map(uint32_t buttons,
                                   uint8_t lx, uint8_t ly,
                                   uint8_t rx, uint8_t ry)
{
    kage_vita_gamepad_snapshot snapshot;
    isaac_vita_xinput_state state;

    memset(&snapshot, 0, sizeof snapshot);
    memset(&state, 0xcc, sizeof state);
    snapshot.packet_number = UINT32_C(0x12345678);
    snapshot.buttons = buttons;
    snapshot.lx = lx;
    snapshot.ly = ly;
    snapshot.rx = rx;
    snapshot.ry = ry;
    isaac_vita_xinput_build_state(&snapshot, &state);
    return state;
}

static isaac_vita_xinput_state map_synthetic(uint16_t buttons,
                                              uint8_t left_trigger)
{
    kage_vita_gamepad_snapshot snapshot;
    isaac_vita_xinput_state state;

    memset(&snapshot, 0, sizeof snapshot);
    memset(&state, 0xcc, sizeof state);
    snapshot.packet_number = UINT32_C(0x87654321);
    snapshot.xinput_buttons = buttons;
    snapshot.xinput_left_trigger = left_trigger;
    snapshot.lx = snapshot.ly = snapshot.rx = snapshot.ry = 128U;
    isaac_vita_xinput_build_state(&snapshot, &state);
    return state;
}

int main(void)
{
    static const char *const aliases[ISAAC_VITA_XINPUT_MODULE_ALIAS_COUNT] = {
        "xinput1_4.DLL", "XiNpUt1_3.dLl", "BIN\\xInPuT1_3.DlL"
    };
    static const char *const rejected[] = {
        "XInput9_1_0.dll", "XInput1_4", "XInput1_3.dll.1",
        "\\bin\\XInput1_3.dll", "DINPUT8.dll", "kernel32.dll"
    };
    isaac_vita_xinput_capabilities capabilities;
    isaac_vita_xinput_state state;
    uint16_t physical_buttons;
    size_t index;
    int previous;

    for (index = 0U; index < ISAAC_VITA_XINPUT_MODULE_ALIAS_COUNT; ++index)
        CHECK(isaac_vita_xinput_module_for_name(aliases[index]) ==
              ISAAC_VITA_XINPUT_MODULE_TOKEN);
    for (index = 0U; index < sizeof rejected / sizeof rejected[0]; ++index)
        CHECK(isaac_vita_xinput_module_for_name(rejected[index]) == 0U);
    CHECK(isaac_vita_xinput_module_for_name(NULL) == 0U);

    CHECK(isaac_vita_xinput_token_for_ordinal(100U) ==
          ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN);
    CHECK(isaac_vita_xinput_token_for_ordinal(99U) == 0U);
    CHECK(isaac_vita_xinput_token_for_name("XInputGetState") ==
          ISAAC_VITA_XINPUT_GET_STATE_TOKEN);
    CHECK(isaac_vita_xinput_token_for_name("XInputGetCapabilities") ==
          ISAAC_VITA_XINPUT_GET_CAPS_TOKEN);
    CHECK(isaac_vita_xinput_token_for_name("xinputgetstate") == 0U);
    CHECK(isaac_vita_xinput_token_for_name("XInputSetState") == 0U);
    CHECK(isaac_vita_xinput_token_for_name(NULL) == 0U);

    CHECK(isaac_vita_xinput_axis(0U, 0) == -32768);
    CHECK(isaac_vita_xinput_axis(127U, 0) < 0);
    CHECK(isaac_vita_xinput_axis(128U, 0) == 0);
    CHECK(isaac_vita_xinput_axis(129U, 0) > 0);
    CHECK(isaac_vita_xinput_axis(255U, 0) == 32767);
    CHECK(isaac_vita_xinput_axis(0U, 1) == 32767);
    CHECK(isaac_vita_xinput_axis(127U, 1) > 0);
    CHECK(isaac_vita_xinput_axis(128U, 1) == 0);
    CHECK(isaac_vita_xinput_axis(129U, 1) < 0);
    CHECK(isaac_vita_xinput_axis(255U, 1) == -32768);
    previous = isaac_vita_xinput_axis(0U, 0);
    for (index = 1U; index < 256U; ++index) {
        int current = isaac_vita_xinput_axis((uint8_t)index, 0);
        CHECK(current > previous);
        previous = current;
    }
    previous = isaac_vita_xinput_axis(0U, 1);
    for (index = 1U; index < 256U; ++index) {
        int current = isaac_vita_xinput_axis((uint8_t)index, 1);
        CHECK(current < previous);
        previous = current;
    }

    state = map(SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT |
                SCE_CTRL_RIGHT | SCE_CTRL_START | SCE_CTRL_LTRIGGER |
                SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE |
                SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE | SCE_CTRL_SELECT,
                0U, 0U, 255U, 255U);
    physical_buttons = ISAAC_VITA_XINPUT_DPAD_UP |
                     ISAAC_VITA_XINPUT_DPAD_DOWN |
                     ISAAC_VITA_XINPUT_DPAD_LEFT |
                     ISAAC_VITA_XINPUT_DPAD_RIGHT |
                     ISAAC_VITA_XINPUT_START |
                     ISAAC_VITA_XINPUT_LEFT_SHOULDER |
                     ISAAC_VITA_XINPUT_A |
                     ISAAC_VITA_XINPUT_B |
                     ISAAC_VITA_XINPUT_X |
                     ISAAC_VITA_XINPUT_Y;
    CHECK(state.dwPacketNumber == UINT32_C(0x12345678));
    CHECK(state.Gamepad.wButtons == physical_buttons);
    CHECK(state.Gamepad.bLeftTrigger == 255U);
    CHECK(state.Gamepad.bRightTrigger == 255U);
    CHECK((state.Gamepad.wButtons & ISAAC_VITA_XINPUT_BACK) == 0U);
    CHECK(state.Gamepad.sThumbLX == -32768 &&
          state.Gamepad.sThumbLY == 32767 &&
          state.Gamepad.sThumbRX == 32767 &&
          state.Gamepad.sThumbRY == -32768);

    state = map(SCE_CTRL_CROSS, 128U, 128U, 128U, 128U);
    CHECK(state.Gamepad.wButtons == ISAAC_VITA_XINPUT_A);
    state = map(SCE_CTRL_CIRCLE, 128U, 128U, 128U, 128U);
    CHECK(state.Gamepad.wButtons == ISAAC_VITA_XINPUT_B);
    state = map(SCE_CTRL_SQUARE, 128U, 128U, 128U, 128U);
    CHECK(state.Gamepad.wButtons == ISAAC_VITA_XINPUT_X);
    state = map(SCE_CTRL_TRIANGLE, 128U, 128U, 128U, 128U);
    CHECK(state.Gamepad.wButtons == ISAAC_VITA_XINPUT_Y);
    CHECK(state.Gamepad.sThumbLX == 0 && state.Gamepad.sThumbLY == 0 &&
          state.Gamepad.sThumbRX == 0 && state.Gamepad.sThumbRY == 0);

    state = map(SCE_CTRL_RTRIGGER, 128U, 128U, 128U, 128U);
    CHECK(state.Gamepad.wButtons == 0U);
    CHECK(state.Gamepad.bLeftTrigger == 255U);
    CHECK(state.Gamepad.bRightTrigger == 0U);

    state = map_synthetic(ISAAC_VITA_XINPUT_RIGHT_SHOULDER, 0U);
    CHECK(state.Gamepad.wButtons == ISAAC_VITA_XINPUT_RIGHT_SHOULDER);
    CHECK(state.Gamepad.bLeftTrigger == 0U);
    CHECK(state.Gamepad.bRightTrigger == 0U);

    state = map_synthetic(ISAAC_VITA_XINPUT_BACK, 255U);
    CHECK(state.dwPacketNumber == UINT32_C(0x87654321));
    CHECK(state.Gamepad.wButtons == ISAAC_VITA_XINPUT_BACK);
    CHECK(state.Gamepad.bLeftTrigger == 255U);
    CHECK(state.Gamepad.bRightTrigger == 0U);

    memset(&capabilities, 0xcc, sizeof capabilities);
    isaac_vita_xinput_build_capabilities(&capabilities);
    CHECK(capabilities.Type == ISAAC_VITA_XINPUT_DEVTYPE_GAMEPAD);
    CHECK(capabilities.SubType == ISAAC_VITA_XINPUT_DEVSUBTYPE_GAMEPAD);
    CHECK(capabilities.Flags == 0U);
    CHECK(capabilities.Gamepad.wButtons ==
          (uint16_t)(physical_buttons | ISAAC_VITA_XINPUT_BACK |
                     ISAAC_VITA_XINPUT_RIGHT_SHOULDER));
    CHECK(capabilities.Gamepad.bLeftTrigger == 255U);
    CHECK(capabilities.Gamepad.bRightTrigger == 255U);
    CHECK(capabilities.Gamepad.sThumbLX == -1 &&
          capabilities.Gamepad.sThumbLY == -1 &&
          capabilities.Gamepad.sThumbRX == -1 &&
          capabilities.Gamepad.sThumbRY == -1);
    CHECK(capabilities.Vibration.wLeftMotorSpeed == 0U &&
          capabilities.Vibration.wRightMotorSpeed == 0U);

    CHECK(ISAAC_VITA_XINPUT_ACTION_BOMB == 8U &&
          ISAAC_VITA_XINPUT_BINDING_LEFT_SHOULDER == 8U);
    CHECK(ISAAC_VITA_XINPUT_ACTION_ACTIVE == 9U &&
          ISAAC_VITA_XINPUT_BINDING_LEFT_TRIGGER == 9U);
    CHECK(ISAAC_VITA_XINPUT_ACTION_PILL_CARD == 10U &&
          ISAAC_VITA_XINPUT_BINDING_RIGHT_SHOULDER == 11U);
    CHECK(ISAAC_VITA_XINPUT_ACTION_DROP == 11U &&
          ISAAC_VITA_XINPUT_BINDING_RIGHT_TRIGGER == 12U);
    CHECK(ISAAC_VITA_XINPUT_ACTION_PAUSE == 12U &&
          ISAAC_VITA_XINPUT_BINDING_START == 15U);
    CHECK(ISAAC_VITA_XINPUT_ACTION_MAP == 13U &&
          ISAAC_VITA_XINPUT_BINDING_BACK == 14U);

    CHECK(oracle_dispatch());

    puts("Vita synthetic XInput resolver/mapping/dispatch oracle: PASS");
    return 0;
}
