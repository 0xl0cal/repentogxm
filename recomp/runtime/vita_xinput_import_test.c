/* Compile/link/static softfp ARM oracle for the synthetic XInput boundary.
 * The ELF is intentionally not executed by the host test script. */
#include <stdint.h>
#include <string.h>

#include "host_vita_xinput.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita XInput import oracle requires a 32-bit identity-mapped host
#endif

_Static_assert(ISAAC_VITA_XINPUT_PE_SIZE == UINT32_C(8650240),
               "frozen XInput PE size drifted");
_Static_assert(ISAAC_VITA_XINPUT_MODULE_ALIAS_COUNT == 3U &&
               ISAAC_VITA_XINPUT_RESOLVED_PROC_COUNT == 3U,
               "frozen XInput loader census changed");
_Static_assert(ISAAC_VITA_XINPUT_INIT_ROOT_RVA == 0x00569830U &&
               ISAAC_VITA_XINPUT_INIT_BYPASS_SITE_RVA == 0x005698d3U &&
               ISAAC_VITA_XINPUT_INIT_BYPASS_TARGET_RVA == 0x00569a03U,
               "Gamepad_init/bypass evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_INITIALIZED_RVA == 0x00803040U &&
               ISAAC_VITA_XINPUT_AVAILABLE_RVA == 0x008007dfU &&
               ISAAC_VITA_DINPUT_CHANGED_RVA == 0x008007deU,
               "libstem state-byte evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_LOAD_LIBRARY_IAT_RVA == 0x00606124U &&
               ISAAC_VITA_XINPUT_GET_PROC_IAT_RVA == 0x00606118U &&
               ISAAC_VITA_XINPUT_FREE_LIBRARY_IAT_RVA == 0x00606104U,
               "KERNEL32 loader IAT evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_14_NAME_RVA == 0x00765334U &&
               ISAAC_VITA_XINPUT_14_CALL_RVA == 0x005698dfU &&
               ISAAC_VITA_XINPUT_14_RETURN_RVA == 0x005698e6U,
               "XInput 1.4 load evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_13_NAME_RVA == 0x00765268U &&
               ISAAC_VITA_XINPUT_13_CALL_RVA == 0x005698ecU &&
               ISAAC_VITA_XINPUT_13_RETURN_RVA == 0x005698f3U,
               "XInput 1.3 load evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_13_BIN_NAME_RVA == 0x00765278U &&
               ISAAC_VITA_XINPUT_13_BIN_CALL_RVA == 0x005698f9U &&
               ISAAC_VITA_XINPUT_13_BIN_RETURN_RVA == 0x00569900U,
               "bundled XInput 1.3 load evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_GET_STATE_EX_ORDINAL == 100U &&
               ISAAC_VITA_XINPUT_GET_STATE_EX_CALL_RVA == 0x00569934U &&
               ISAAC_VITA_XINPUT_GET_STATE_EX_RETURN_RVA == 0x0056993aU &&
               ISAAC_VITA_XINPUT_GET_STATE_EX_CACHE_RVA == 0x00803028U,
               "XInputGetStateEx resolver evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_GET_STATE_NAME_RVA == 0x007652e8U &&
               ISAAC_VITA_XINPUT_GET_STATE_CALL_RVA == 0x00569945U &&
               ISAAC_VITA_XINPUT_GET_STATE_RETURN_RVA == 0x0056994bU &&
               ISAAC_VITA_XINPUT_GET_STATE_CACHE_RVA == 0x00803020U,
               "XInputGetState resolver evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_GET_CAPS_NAME_RVA == 0x007651f8U &&
               ISAAC_VITA_XINPUT_GET_CAPS_CALL_RVA == 0x00569956U &&
               ISAAC_VITA_XINPUT_GET_CAPS_RETURN_RVA == 0x0056995cU &&
               ISAAC_VITA_XINPUT_GET_CAPS_CACHE_RVA == 0x00803018U,
               "XInputGetCapabilities resolver evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_CAPS_ROOT_RVA == 0x0059d3b0U &&
               ISAAC_VITA_XINPUT_CAPS_CALL_RVA == 0x0059d4a8U &&
               ISAAC_VITA_XINPUT_CAPS_RETURN_RVA == 0x0059d4aeU,
               "capability caller evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_DETECT_WORKER_SEAM_RVA == 0x005699edU &&
               ISAAC_VITA_XINPUT_DETECT_WORKER_CALL_RVA == 0x00569a00U &&
               ISAAC_VITA_XINPUT_DETECT_WORKER_NEXT_RVA == 0x00569a03U &&
               ISAAC_VITA_XINPUT_DETECT_WORKER_RVA == 0x00569e00U &&
               ISAAC_VITA_XINPUT_DETECT_WORKER_RELOC_RVA == 0x005699fcU,
               "detector-worker bypass evidence drifted");
_Static_assert(ISAAC_VITA_KAGE_THREAD_START_ROOT_RVA == 0x00598a50U &&
               ISAAC_VITA_BEGINTHREADEX_IAT_RVA == 0x006065b0U &&
               ISAAC_VITA_BEGINTHREADEX_RETURN_RVA == 0x00598b5cU,
               "live _beginthreadex stop evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_POLL_ROOT_RVA == 0x0059d720U &&
               ISAAC_VITA_XINPUT_STATE_EX_CALL_RVA == 0x0059d7aeU &&
               ISAAC_VITA_XINPUT_STATE_EX_RETURN_RVA == 0x0059d7b0U &&
               ISAAC_VITA_XINPUT_STATE_CALL_RVA == 0x0059d829U &&
               ISAAC_VITA_XINPUT_STATE_RETURN_RVA == 0x0059d82fU,
               "state caller evidence drifted");
_Static_assert(ISAAC_VITA_XINPUT_BINDINGS_TABLE_RVA == 0x007aaa20U &&
               ISAAC_VITA_XINPUT_BINDINGS_TABLE_SIZE == 0x120U,
               "controller default-binding evidence drifted");

static volatile const char s_frozen_xinput_pe_sha256[] =
    ISAAC_VITA_XINPUT_PE_SHA256;

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

static uint32_t oracle_import_call(CPU *cpu, const char *import_name,
                                   const uint32_t *arguments,
                                   uint32_t argument_count,
                                   unsigned *calls, int *ok)
{
    uint32_t original_esp = cpu->esp;
    uint32_t index;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, UINT32_C(0xaabbccdd));
    if (!isaac_vita_xinput_import_counted(cpu, import_name, calls) ||
        cpu->esp != original_esp)
        *ok = 0;
    return cpu->eax;
}

static int oracle_delegated_import(CPU *cpu, const char *import_name,
                                   const uint32_t *arguments,
                                   uint32_t argument_count,
                                   unsigned *calls)
{
    CPU original = *cpu;
    CPU entry;
    unsigned before = *calls;
    uint32_t index;
    int delegated;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, UINT32_C(0x12345678));
    entry = *cpu;
    delegated =
        isaac_vita_xinput_import_counted(cpu, import_name, calls) == 0 &&
        *calls == before && memcmp(cpu, &entry, sizeof entry) == 0;
    *cpu = original;
    return delegated;
}

static uint32_t oracle_dynamic_call(CPU *cpu, uint32_t token,
                                    const uint32_t *arguments,
                                    uint32_t argument_count,
                                    unsigned *calls, int *ok)
{
    uint32_t original_esp = cpu->esp;
    uint32_t index;

    for (index = argument_count; index != 0U; --index)
        gpush(cpu, arguments[index - 1U]);
    gpush(cpu, UINT32_C(0xddccbbaa));
    if (!isaac_vita_xinput_dynamic_counted(cpu, token, calls) ||
        cpu->esp != original_esp)
        *ok = 0;
    return cpu->eax;
}

int main(void)
{
    static const char xinput_module[] = ISAAC_VITA_XINPUT_14_NAME;
    static const char rejected_module[] = "DINPUT8.dll";
    static const char state_name[] = ISAAC_VITA_XINPUT_GET_STATE_NAME;
    static const char caps_name[] = ISAAC_VITA_XINPUT_GET_CAPS_NAME;
    static const char missing_name[] = "XInputSetState";
    uint32_t stack[64];
    uint32_t arguments[3];
    CPU cpu;
    CPU untouched;
    isaac_vita_xinput_state state;
    isaac_vita_xinput_state_ex state_ex;
    isaac_vita_xinput_capabilities capabilities;
    unsigned imports = 0U;
    unsigned dynamics = 0U;
    uint32_t module;
    uint32_t state_ex_token;
    uint32_t state_token;
    uint32_t caps_token;
    int ok = 1;

    memset(&cpu, 0, sizeof cpu);
    cpu.esp = (uint32_t)(uintptr_t)&stack[64];
    if (sizeof s_frozen_xinput_pe_sha256 != 65U ||
        s_frozen_xinput_pe_sha256[0] != '3' ||
        s_frozen_xinput_pe_sha256[63] != '4')
        return 1;

    arguments[0] = (uint32_t)(uintptr_t)rejected_module;
    if (!oracle_delegated_import(&cpu,
                                 ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME,
                                 arguments, 1U, &imports))
        return 1;

    arguments[0] = (uint32_t)(uintptr_t)xinput_module;
    module = oracle_import_call(&cpu, ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME,
                                arguments, 1U, &imports, &ok);
    if (module != ISAAC_VITA_XINPUT_MODULE_TOKEN)
        ok = 0;

    arguments[0] = UINT32_C(0x7f000099);
    arguments[1] = (uint32_t)(uintptr_t)state_name;
    if (!oracle_delegated_import(&cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
                                 arguments, 2U, &imports))
        return 2;

    arguments[0] = module;
    arguments[1] = ISAAC_VITA_XINPUT_GET_STATE_EX_ORDINAL;
    state_ex_token = oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &ok);
    arguments[1] = (uint32_t)(uintptr_t)state_name;
    state_token = oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &ok);
    arguments[1] = (uint32_t)(uintptr_t)caps_name;
    caps_token = oracle_import_call(
        &cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
        arguments, 2U, &imports, &ok);
    arguments[1] = (uint32_t)(uintptr_t)missing_name;
    if (oracle_import_call(&cpu, ISAAC_VITA_XINPUT_GET_PROC_NAME,
                           arguments, 2U, &imports, &ok) != 0U)
        ok = 0;

    if (!kage_vita_input_initialize())
        return 3;
    (void)kage_vita_input_sample();

    memset(&capabilities, 0xcc, sizeof capabilities);
    arguments[0] = 0U;
    arguments[1] = 0U;
    arguments[2] = (uint32_t)(uintptr_t)&capabilities;
    if (oracle_dynamic_call(&cpu, caps_token, arguments, 3U,
                            &dynamics, &ok) != 0U ||
        capabilities.Type != ISAAC_VITA_XINPUT_DEVTYPE_GAMEPAD ||
        capabilities.SubType != ISAAC_VITA_XINPUT_DEVSUBTYPE_GAMEPAD)
        ok = 0;

    memset(&state_ex, 0xcc, sizeof state_ex);
    arguments[0] = 0U;
    arguments[1] = (uint32_t)(uintptr_t)&state_ex;
    if (oracle_dynamic_call(&cpu, state_ex_token, arguments, 2U,
                            &dynamics, &ok) != 0U ||
        state_ex.dwPaddingReserved != 0U)
        ok = 0;

    memset(&state, 0xcc, sizeof state);
    arguments[0] = 0U;
    arguments[1] = (uint32_t)(uintptr_t)&state;
    if (oracle_dynamic_call(&cpu, state_token, arguments, 2U,
                            &dynamics, &ok) != 0U)
        ok = 0;

    memset(&capabilities, 0xcc, sizeof capabilities);
    arguments[0] = 1U;
    arguments[1] = 0U;
    arguments[2] = (uint32_t)(uintptr_t)&capabilities;
    if (oracle_dynamic_call(&cpu, caps_token, arguments, 3U,
                            &dynamics, &ok) !=
            ISAAC_VITA_XINPUT_ERROR_DEVICE_NOT_CONNECTED)
        ok = 0;
    {
        const unsigned char *bytes =
            (const unsigned char *)(const void *)&capabilities;
        uint32_t i;
        for (i = 0U; i < sizeof capabilities; ++i)
            if (bytes[i] != 0xccU)
                ok = 0;
    }

    untouched = cpu;
    if (isaac_vita_xinput_dynamic_counted(
            &cpu, UINT32_C(0x12345678), &dynamics) != 0 ||
        dynamics != 4U || memcmp(&cpu, &untouched, sizeof cpu) != 0)
        ok = 0;

    arguments[0] = module;
    if (oracle_import_call(&cpu, ISAAC_VITA_XINPUT_FREE_LIBRARY_NAME,
                           arguments, 1U, &imports, &ok) != 1U)
        ok = 0;
    kage_vita_input_deactivate();

    if (!ok ||
        state_ex_token != ISAAC_VITA_XINPUT_GET_STATE_EX_TOKEN ||
        state_token != ISAAC_VITA_XINPUT_GET_STATE_TOKEN ||
        caps_token != ISAAC_VITA_XINPUT_GET_CAPS_TOKEN ||
        imports != 6U || dynamics != 4U)
        return 4;
    return 0;
}
