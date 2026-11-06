/* Compile/link/static softfp oracle for the target-local post-COM imports.
 * test_post_com_imports.sh also builds it with the RENDER60 S6 knobs
 * (ISAAC_VITA_TIMEGETTIME_RECIPROCAL, ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE
 * and PHASE_PROFILE): the edge times below must narrow exactly like the OFF
 * division, the reciprocal self-test must log once with mismatch=0, and the
 * non-site return word 0x0badc0de must never reach the latch (no banner). */
#include <stdint.h>
#include <string.h>
#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL) || \
    defined(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE)
#include <stdarg.h>
#include <stdio.h>
#define POST_COM_TEST_LOGS 1
#endif

#include "guest.h"
#include "host_vita_post_com.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita post-COM oracle requires a 32-bit identity-mapped target
#endif

#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL)
#define POST_COM_TEST_EXPECTED_LOGS 1U
#if defined(ISAAC_VITA_PHASE_PROFILE)
/* the self-test also times 1000 divisions and 1000 reciprocals */
#define POST_COM_TEST_FIRST_EXTRA_READS 3U
#endif
#else
#define POST_COM_TEST_EXPECTED_LOGS 0U
#endif
#ifndef POST_COM_TEST_FIRST_EXTRA_READS
#define POST_COM_TEST_FIRST_EXTRA_READS 0U
#endif

_Alignas(8) static uint32_t s_frame[8];
static uint64_t s_process_time;
static unsigned s_process_time_calls;

uint64_t isaac_vita_get_process_time(void)
{
    ++s_process_time_calls;
    return s_process_time;
}

#if defined(POST_COM_TEST_LOGS)
/* platform.c's logger: count the lines and keep the last one. */
static unsigned s_log_lines;
static char s_last_log[256];

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    ++s_log_lines;
}
#endif

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
        s_process_time_calls != 1U + POST_COM_TEST_FIRST_EXTRA_READS)
        return 3;
#if defined(ISAAC_VITA_TIMEGETTIME_RECIPROCAL)
    /* the first call ran the self-test and logged once */
    if (s_log_lines != 1U ||
        strstr(s_last_log, "KAGE VITA TGT RECIPROCAL: selftest n=4096 "
                           "mismatch=0 use=reciprocal") != s_last_log)
        return 6;
#elif defined(POST_COM_TEST_LOGS)
    if (s_log_lines != 0U)   /* a non-site call never prints the banner */
        return 6;
#endif
    {
        static const uint64_t edges[] = {
            UINT64_C(0), UINT64_C(999), UINT64_C(1000),
            UINT64_C(4294967295999), UINT64_C(4294967296000),
            UINT64_C(0x00000123456789ab), UINT64_C(0xFFFFFFFFFFFFFFFF),
        };
        size_t i;

        for (i = 0U; i < sizeof edges / sizeof edges[0]; ++i) {
            esp = prepare_call(&cpu);
            s_process_time = edges[i];
            s_process_time_calls = 0U;
            if (!isaac_vita_post_com_import_counted(
                    &cpu, ISAAC_VITA_POST_COM_GET_TIME_NAME, &calls) ||
                cpu.fault || cpu.esp != esp + 4U ||
                cpu.eax != (uint32_t)(edges[i] / UINT64_C(1000)) ||
                s_process_time_calls != 1U)
                return 7;
        }
        if (calls != 1U + (unsigned)(sizeof edges / sizeof edges[0]))
            return 7;
        calls = 1U;
#if defined(POST_COM_TEST_LOGS)
        if (s_log_lines != POST_COM_TEST_EXPECTED_LOGS)
            return 7;
#endif
    }

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
