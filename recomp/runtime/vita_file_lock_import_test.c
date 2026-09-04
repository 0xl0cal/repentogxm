/* Compile/link/static softfp oracle for the owned-fd file-lock boundary. */
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_file_lock.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita file-lock oracle requires a 32-bit identity-mapped target
#endif

_Alignas(8) static uint32_t s_frame[12];
static uint32_t s_owned_handle;
static unsigned s_ownership_checks;

int isaac_vita_crt_osfhandle_is_owned(uint32_t handle)
{
    ++s_ownership_checks;
    return handle == s_owned_handle;
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
    cpu->last_error = 0x13572468U;
    return cpu->esp;
}

static void prepare_lock_arguments(uint32_t handle, uint32_t flags,
                                   uint32_t reserved, uint32_t overlapped)
{
    s_frame[3] = handle;
    s_frame[4] = flags;
    s_frame[5] = reserved;
    s_frame[6] = 1U;
    s_frame[7] = 0U;
    s_frame[8] = overlapped;
}

static void prepare_unlock_arguments(uint32_t handle, uint32_t reserved,
                                     uint32_t overlapped)
{
    s_frame[3] = handle;
    s_frame[4] = reserved;
    s_frame[5] = 1U;
    s_frame[6] = 0U;
    s_frame[7] = overlapped;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    uint32_t overlapped[5] = { 0U, 0U, 0U, 0U, 0U };
    uint32_t esp;
    unsigned calls = 0U;
    uint32_t flags;

    if ((uintptr_t)s_frame > UINT32_MAX ||
        (uintptr_t)overlapped > UINT32_MAX)
        return 1;
    if (strcmp(ISAAC_VITA_FILE_LOCK_LOCK_NAME,
               "KERNEL32.dll!LockFileEx") != 0 ||
        ISAAC_VITA_FILE_LOCK_LOCK_IAT_RVA != 0x006060d0U ||
        ISAAC_VITA_FILE_LOCK_LOCK_CALL_RVA != 0x00596660U ||
        ISAAC_VITA_FILE_LOCK_LOCK_RETURN_RVA != 0x00596666U ||
        strcmp(ISAAC_VITA_FILE_LOCK_UNLOCK_NAME,
               "KERNEL32.dll!UnlockFileEx") != 0 ||
        ISAAC_VITA_FILE_LOCK_UNLOCK_IAT_RVA != 0x006060d4U ||
        ISAAC_VITA_FILE_LOCK_UNLOCK_CALL_RVA != 0x00596614U ||
        ISAAC_VITA_FILE_LOCK_UNLOCK_RETURN_RVA != 0x0059661aU ||
        ISAAC_VITA_FILE_LOCK_GET_OSFHANDLE_IAT_RVA != 0x006065f0U ||
        ISAAC_VITA_FILE_LOCK_GET_OSFHANDLE_CALL_RVA != 0x005965dbU ||
        ISAAC_VITA_FILE_LOCK_GET_OSFHANDLE_RETURN_RVA != 0x005965e1U ||
        ISAAC_VITA_FILE_LOCK_IMPORT_COUNT != 2U)
        return 2;

    s_owned_handle = 7U;
    s_ownership_checks = 0U;
    for (flags = 0U; flags <= ISAAC_VITA_FILE_LOCK_ALLOWED_FLAGS; ++flags) {
        esp = prepare_call(&cpu);
        prepare_lock_arguments(
            s_owned_handle, flags, 0U, pointer32(overlapped));
        if (!isaac_vita_file_lock_import_counted(
                &cpu, ISAAC_VITA_FILE_LOCK_LOCK_NAME, &calls) ||
            calls != flags + 1U || cpu.fault || cpu.esp != esp + 28U ||
            cpu.eax != 1U || cpu.last_error != 0x13572468U)
            return 3;
    }
    if (s_ownership_checks != 4U)
        return 4;

    esp = prepare_call(&cpu);
    prepare_unlock_arguments(
        s_owned_handle, 0U, pointer32(overlapped));
    if (!isaac_vita_file_lock_import_counted(
            &cpu, ISAAC_VITA_FILE_LOCK_UNLOCK_NAME, &calls) ||
        calls != 5U || cpu.fault || cpu.esp != esp + 24U ||
        cpu.eax != 1U || cpu.last_error != 0x13572468U ||
        s_ownership_checks != 5U)
        return 5;

    esp = prepare_call(&cpu);
    prepare_lock_arguments(99U, 0U, 0U, pointer32(overlapped));
    if (!isaac_vita_file_lock_import_counted(
            &cpu, ISAAC_VITA_FILE_LOCK_LOCK_NAME, &calls) ||
        calls != 6U || cpu.fault || cpu.esp != esp + 28U ||
        cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_FILE_LOCK_ERROR_INVALID_HANDLE)
        return 6;

    esp = prepare_call(&cpu);
    prepare_unlock_arguments(UINT32_MAX, 0U, pointer32(overlapped));
    if (!isaac_vita_file_lock_import_counted(
            &cpu, ISAAC_VITA_FILE_LOCK_UNLOCK_NAME, &calls) ||
        calls != 7U || cpu.fault || cpu.esp != esp + 24U ||
        cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_FILE_LOCK_ERROR_INVALID_HANDLE)
        return 7;

    esp = prepare_call(&cpu);
    prepare_lock_arguments(s_owned_handle, 4U, 0U, pointer32(overlapped));
    if (!isaac_vita_file_lock_import_counted(
            &cpu, ISAAC_VITA_FILE_LOCK_LOCK_NAME, &calls) ||
        calls != 8U || cpu.fault || cpu.esp != esp + 28U ||
        cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_FILE_LOCK_ERROR_INVALID_PARAMETER)
        return 8;

    esp = prepare_call(&cpu);
    prepare_unlock_arguments(s_owned_handle, 1U, pointer32(overlapped));
    if (!isaac_vita_file_lock_import_counted(
            &cpu, ISAAC_VITA_FILE_LOCK_UNLOCK_NAME, &calls) ||
        calls != 9U || cpu.fault || cpu.esp != esp + 24U ||
        cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_FILE_LOCK_ERROR_INVALID_PARAMETER)
        return 9;

    esp = prepare_call(&cpu);
    prepare_lock_arguments(s_owned_handle, 0U, 0U, 0U);
    if (!isaac_vita_file_lock_import_counted(
            &cpu, ISAAC_VITA_FILE_LOCK_LOCK_NAME, &calls) ||
        calls != 10U || cpu.fault || cpu.esp != esp + 28U ||
        cpu.eax != 0U ||
        cpu.last_error != ISAAC_VITA_FILE_LOCK_ERROR_INVALID_PARAMETER)
        return 10;

    esp = prepare_call(&cpu);
    cpu.eax = 0x11223344U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_file_lock_import_counted(
            &cpu, "KERNEL32.dll!not_a_file_lock", &calls) != 0 ||
        calls != 10U || cpu.esp != esp ||
        memcmp(&cpu, &snapshot, sizeof cpu) != 0)
        return 11;
    return 0;
}
