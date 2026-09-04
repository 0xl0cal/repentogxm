/* Compile/link/static softfp oracle for the target-local post-COM imports. */
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_post_com.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita post-COM oracle requires a 32-bit identity-mapped target
#endif

_Alignas(8) static uint32_t s_frame[8];
static uint64_t s_process_time;
static unsigned s_process_time_calls;

uint64_t isaac_vita_get_process_time(void)
{
    ++s_process_time_calls;
    return s_process_time;
}

static uint32_t pointer32(void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint32_t prepare_call(CPU *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[2] = 0x0badc0deU;
    cpu->esp = pointer32(&s_frame[2]);
    return cpu->esp;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    unsigned calls = 0U;
    uint32_t esp;

    if ((uintptr_t)s_frame > UINT32_MAX)
        return 1;
    if (strcmp(ISAAC_VITA_POST_COM_GET_TIME_NAME,
               "WINMM.dll!timeGetTime") != 0 ||
        ISAAC_VITA_POST_COM_GET_TIME_IAT_RVA != 0x006064c0U ||
        ISAAC_VITA_POST_COM_GET_TIME_FIRST_CALL_RVA != 0x0050b810U ||
        ISAAC_VITA_POST_COM_GET_TIME_FIRST_RETURN_RVA != 0x0050b812U ||
        ISAAC_VITA_POST_COM_GET_TIME_FIRST_ORDINAL != 2776U ||
        ISAAC_VITA_POST_COM_GET_TIME_SECOND_CALL_RVA != 0x0050b8eaU ||
        ISAAC_VITA_POST_COM_GET_TIME_SECOND_RETURN_RVA != 0x0050b8ecU ||
        ISAAC_VITA_POST_COM_GET_TIME_THIRD_CALL_RVA != 0x0050b927U ||
        ISAAC_VITA_POST_COM_GET_TIME_THIRD_RETURN_RVA != 0x0050b929U ||
        strcmp(ISAAC_VITA_POST_COM_END_PERIOD_NAME,
               "WINMM.dll!timeEndPeriod") != 0 ||
        ISAAC_VITA_POST_COM_END_PERIOD_IAT_RVA != 0x006064b4U ||
        ISAAC_VITA_POST_COM_END_PERIOD_CALL_RVA != 0x00598eeeU ||
        ISAAC_VITA_POST_COM_END_PERIOD_RETURN_RVA != 0x00598ef4U ||
        ISAAC_VITA_POST_COM_IMPORT_COUNT != 6U)
        return 2;

    esp = prepare_call(&cpu);
    s_process_time = UINT64_C(0x00000123456789ab);
    s_process_time_calls = 0U;
    if (!isaac_vita_post_com_import_counted(
            &cpu, ISAAC_VITA_POST_COM_GET_TIME_NAME, &calls) ||
        calls != 1U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != (uint32_t)(s_process_time / UINT64_C(1000)) ||
        s_process_time_calls != 1U)
        return 3;

    esp = prepare_call(&cpu);
    s_frame[3] = ISAAC_VITA_POST_COM_PERIOD_MIN;
    if (!isaac_vita_post_com_import_counted(
            &cpu, ISAAC_VITA_POST_COM_END_PERIOD_NAME, &calls) ||
        calls != 2U || cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != ISAAC_VITA_POST_COM_TIMERR_NOERROR)
        return 4;

    esp = prepare_call(&cpu);
    cpu.eax = 0x11223344U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_post_com_import_counted(
            &cpu, "WINMM.dll!not_a_timer", &calls) != 0 ||
        calls != 2U || cpu.esp != esp ||
        memcmp(&cpu, &snapshot, sizeof cpu) != 0)
        return 5;
    return 0;
}
