/* Link-only proof for the complete guest-ABI adapter object.  This is not a
 * hardware audio test; Vita3K or a Vita must exercise the backend itself. */
#include <stdint.h>
#include <string.h>

#include "host_vita_audio.h"

static uint32_t s_probe_stack[4];

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault_addr = address;
    c->fault = message;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

int main(void)
{
    CPU cpu;
    unsigned calls = 0U;
    uint32_t al_error = UINT32_MAX;

    memset(&cpu, 0, sizeof cpu);
    s_probe_stack[0] = 0x12345678U;
    cpu.esp = (uint32_t)(uintptr_t)&s_probe_stack[0];
    if (isaac_vita_audio_import(&cpu, "OpenAL32.dll!not-an-import"))
        return 1;
    if (!isaac_vita_audio_import_counted(
            &cpu, ISAAC_VITA_AUDIO_GET_ERROR_NAME, &calls))
        return 2;
    if (isaac_vita_audio_manager_initialize(0U, &al_error) !=
            ISAAC_VITA_AUDIO_MANAGER_INVALID_ADDRESS ||
            al_error != 0U ||
            !isaac_vita_audio_manager_status_name(
                ISAAC_VITA_AUDIO_MANAGER_OK))
        return 3;
    isaac_vita_audio_manager_close(0U);
    return cpu.fault || calls != 1U ||
           cpu.esp != (uint32_t)(uintptr_t)&s_probe_stack[1];
}
