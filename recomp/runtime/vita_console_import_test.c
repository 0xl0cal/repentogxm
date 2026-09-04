/* Softfp ARM oracle for the exact GetStdHandle/WriteConsoleA boot pair. */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_console.h"
#include "platform.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita console oracle requires a 32-bit identity-mapped host
#endif

_Alignas(8) static uint32_t s_frame[12];
_Alignas(8) static char s_text[ISAAC_VITA_CONSOLE_LOG_CHUNK + 16U];
_Alignas(8) static uint32_t s_written[3];
static unsigned s_log_calls;
static int s_bad_log_format;
static char s_logged[sizeof s_text];

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint32_t prepare(CPU *c)
{
    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[2] = 0x0badc0deU;
    c->esp = pointer32(&s_frame[2]);
    return c->esp;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

void isaac_vita_log(const char *format, ...)
{
    va_list args;
    const char *text;
    ++s_log_calls;
    if (strcmp(format, "%s") != 0)
        s_bad_log_format = 1;
    va_start(args, format);
    text = va_arg(args, const char *);
    va_end(args);
    strncat(s_logged, text, sizeof s_logged - strlen(s_logged) - 1U);
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    uint32_t esp;
    uint32_t i;
    unsigned calls = 0U;

    if ((uintptr_t)s_frame > UINT32_MAX || (uintptr_t)s_text > UINT32_MAX ||
        (uintptr_t)s_written > UINT32_MAX)
        return 1;
    if (strcmp(ISAAC_VITA_CONSOLE_GET_STD_HANDLE_NAME,
               "KERNEL32.dll!GetStdHandle") != 0 ||
        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_IAT_RVA != 0x00606098U ||
        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_CALL_RVA != 0x005e348bU ||
        ISAAC_VITA_CONSOLE_GET_STD_HANDLE_RETURN_RVA != 0x005e3491U ||
        ISAAC_VITA_CONSOLE_STD_ERROR_ARGUMENT != UINT32_C(0xfffffff4) ||
        strcmp(ISAAC_VITA_CONSOLE_WRITE_NAME,
               "KERNEL32.dll!WriteConsoleA") != 0 ||
        ISAAC_VITA_CONSOLE_WRITE_IAT_RVA != 0x00606090U ||
        ISAAC_VITA_CONSOLE_WRITE_CALL_RVA != 0x005e34cbU ||
        ISAAC_VITA_CONSOLE_WRITE_RETURN_RVA != 0x005e34d1U ||
        ISAAC_VITA_CONSOLE_WRITE_LIVE_BUFFER != 0x987edd88U)
        return 2;

    esp = prepare(&cpu);
    s_frame[3] = ISAAC_VITA_CONSOLE_STD_ERROR_ARGUMENT;
    if (!isaac_vita_console_import_counted(
            &cpu, ISAAC_VITA_CONSOLE_GET_STD_HANDLE_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != ISAAC_VITA_CONSOLE_STDERR_TOKEN || calls != 1U)
        return 3;

    strcpy(s_text, "guest format %x stays data\n");
    memset(s_logged, 0, sizeof s_logged);
    s_written[0] = 0x11223344U;
    s_written[1] = 0xccccccccU;
    s_written[2] = 0x55667788U;
    esp = prepare(&cpu);
    s_frame[3] = ISAAC_VITA_CONSOLE_STDERR_TOKEN;
    s_frame[4] = pointer32(s_text);
    s_frame[5] = (uint32_t)strlen(s_text);
    s_frame[6] = pointer32(&s_written[1]);
    s_frame[7] = 0U;
    if (!isaac_vita_console_import_counted(
            &cpu, ISAAC_VITA_CONSOLE_WRITE_NAME, &calls) || cpu.fault ||
        cpu.esp != esp + 24U || cpu.eax != 1U || calls != 2U ||
        s_written[0] != 0x11223344U ||
        s_written[1] != (uint32_t)strlen(s_text) ||
        s_written[2] != 0x55667788U || s_log_calls != 1U ||
        s_bad_log_format || strcmp(s_logged, s_text) != 0)
        return 4;

    /* A write crossing the fixed stack buffer boundary is emitted as two
     * constant-format records and still reports the complete guest count. */
    for (i = 0U; i < ISAAC_VITA_CONSOLE_LOG_CHUNK + 7U; ++i)
        s_text[i] = (char)('A' + i % 23U);
    s_text[i] = '\0';
    s_logged[0] = '\0';
    s_log_calls = 0U;
    s_written[1] = 0xccccccccU;
    esp = prepare(&cpu);
    s_frame[3] = ISAAC_VITA_CONSOLE_STDERR_TOKEN;
    s_frame[4] = pointer32(s_text);
    s_frame[5] = i;
    s_frame[6] = pointer32(&s_written[1]);
    s_frame[7] = 0U;
    if (!isaac_vita_console_import_counted(
            &cpu, ISAAC_VITA_CONSOLE_WRITE_NAME, &calls) || cpu.fault ||
        cpu.esp != esp + 24U || cpu.eax != 1U || calls != 3U ||
        s_written[1] != i || s_log_calls != 2U || s_bad_log_format ||
        strcmp(s_logged, s_text) != 0)
        return 7;

    /* A bad exact shape is counted before its loud, frame-preserving fault. */
    esp = prepare(&cpu);
    cpu.eax = 0xaabbccddU;
    s_frame[3] = ISAAC_VITA_CONSOLE_STDERR_TOKEN;
    s_frame[4] = pointer32(s_text);
    s_frame[5] = 1U;
    s_frame[6] = pointer32(&s_written[1]);
    s_frame[7] = 1U;
    if (!isaac_vita_console_import_counted(
            &cpu, ISAAC_VITA_CONSOLE_WRITE_NAME, &calls) ||
        calls != 4U || cpu.esp != esp || cpu.eax != 0xaabbccddU ||
        !cpu.fault || cpu.fault_addr !=
            GUEST_IMAGE_BASE + ISAAC_VITA_CONSOLE_WRITE_IAT_RVA)
        return 5;

    esp = prepare(&cpu);
    cpu.eax = 0xdeadbeefU;
    s_frame[3] = ISAAC_VITA_CONSOLE_STDERR_TOKEN;
    s_frame[4] = pointer32(s_text);
    s_frame[5] = 1U;
    s_frame[6] = UINT32_MAX - 1U;
    s_frame[7] = 0U;
    if (!isaac_vita_console_import_counted(
            &cpu, ISAAC_VITA_CONSOLE_WRITE_NAME, &calls) || calls != 5U ||
        cpu.esp != esp || cpu.eax != 0xdeadbeefU ||
        cpu.fault_addr != UINT32_MAX - 1U || !cpu.fault ||
        strcmp(cpu.fault,
               "WriteConsoleA received an invalid guest range") != 0)
        return 8;

    esp = prepare(&cpu);
    cpu.eax = 0x12345678U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_console_import_counted(
            &cpu, "KERNEL32.dll!not_console", &calls) != 0 || calls != 5U ||
        memcmp(&cpu, &snapshot, sizeof cpu) != 0 || cpu.esp != esp)
        return 6;
    return 0;
}
