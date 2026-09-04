#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <psp2/io/dirent.h>

#include "guest.h"
#include "host_vita_find.h"
#include "platform.h"

#define ORACLE_GUEST_BASE UINT32_C(0x10000000)
#define ORACLE_GUEST_SIZE UINT32_C(0x00010000)
#define ORACLE_STACK      (ORACLE_GUEST_BASE + UINT32_C(0x1000))
#define ORACLE_PATTERN    (ORACLE_GUEST_BASE + UINT32_C(0x2000))
#define ORACLE_OUTPUT     (ORACLE_GUEST_BASE + UINT32_C(0x4000))
#define ORACLE_DESCRIPTOR 17

static unsigned s_dopen_calls;
static unsigned s_dread_calls;
static unsigned s_dclose_calls;
static unsigned s_getstat_calls;
static unsigned s_cursor;
static int s_directory_open;

static const char *const s_directory_entries[] = {
    ".", "..", "alpha.txt", "mods", "BETA.BIN"
};

static void oracle_stat(SceIoStat *status, int directory, int64_t size)
{
    memset(status, 0, sizeof *status);
    status->st_mode = directory ? SCE_S_IFDIR : SCE_S_IFREG;
    status->st_size = size;
    status->st_ctime.year = status->st_atime.year =
        status->st_mtime.year = 2026U;
    status->st_ctime.month = status->st_atime.month =
        status->st_mtime.month = 8U;
    status->st_ctime.day = status->st_atime.day =
        status->st_mtime.day = 24U;
}

SceUID sceIoDopen(const char *path)
{
    ++s_dopen_calls;
    if (strcmp(path, ISAAC_VITA_NATIVE_DATA_ROOT "/alpha.txt") == 0)
        return (SceUID)ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY;
    if (strcmp(path, ISAAC_VITA_NATIVE_DATA_ROOT) != 0)
        return (SceUID)ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND;
    if (s_directory_open)
        return -99;
    s_directory_open = 1;
    s_cursor = 0U;
    return ORACLE_DESCRIPTOR;
}

int sceIoDread(SceUID descriptor, SceIoDirent *entry)
{
    const char *name;

    ++s_dread_calls;
    if (descriptor != ORACLE_DESCRIPTOR || !s_directory_open)
        return -98;
    if (s_cursor == sizeof s_directory_entries /
                    sizeof s_directory_entries[0])
        return 0;
    name = s_directory_entries[s_cursor++];
    memset(entry, 0, sizeof *entry);
    strcpy(entry->d_name, name);
    oracle_stat(&entry->d_stat,
                strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
                strcmp(name, "mods") == 0,
                strcmp(name, "alpha.txt") == 0 ? 123 :
                strcmp(name, "BETA.BIN") == 0 ? 456 : 0);
    return 1;
}

int sceIoDclose(SceUID descriptor)
{
    ++s_dclose_calls;
    if (descriptor != ORACLE_DESCRIPTOR || !s_directory_open)
        return -97;
    s_directory_open = 0;
    return 0;
}

int sceIoGetstat(const char *path, SceIoStat *status)
{
    ++s_getstat_calls;
    if (strncmp(path, ISAAC_VITA_NATIVE_DATA_ROOT "/alpha.txt/",
                sizeof ISAAC_VITA_NATIVE_DATA_ROOT "/alpha.txt/" - 1U) == 0)
        return (int)ISAAC_VITA_FIND_NATIVE_ERROR_NOT_DIRECTORY;
    if (strcmp(path, ISAAC_VITA_NATIVE_DATA_ROOT) == 0 ||
        strcmp(path, ISAAC_VITA_NATIVE_DATA_ROOT "/mods") == 0) {
        oracle_stat(status, 1, 0);
        return 0;
    }
    if (strcmp(path, ISAAC_VITA_NATIVE_DATA_ROOT "/alpha.txt") == 0) {
        oracle_stat(status, 0, 123);
        return 0;
    }
    if (strcmp(path, ISAAC_VITA_NATIVE_DATA_ROOT "/BETA.BIN") == 0) {
        oracle_stat(status, 0, 456);
        return 0;
    }
    return (int)ISAAC_VITA_FIND_NATIVE_ERROR_NOT_FOUND;
}

int sceKernelDelayThread(unsigned int delay)
{
    (void)delay;
    return 0;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)pc;
    (void)kind;
    (void)size;
    guest_fault(c, address, "unexpected FindFile oracle stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)pc;
    guest_fault(c, c ? c->esp : 0U,
                "unexpected FindFile oracle stack owner violation");
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

static uint32_t oracle_prepare(CPU *cpu, uint32_t a0, uint32_t a1)
{
    uint32_t previous_error = cpu->last_error;

    memset(cpu, 0, sizeof *cpu);
    cpu->last_error = previous_error;
    cpu->esp = ORACLE_STACK;
    st32(cpu->esp, UINT32_C(0xabcdef01));
    st32(cpu->esp + 4U, a0);
    st32(cpu->esp + 8U, a1);
    return cpu->esp;
}

static void oracle_write_wide_ascii(const char *text)
{
    uint32_t index;

    for (index = 0U; text[index]; ++index)
        st16(ORACLE_PATTERN + index * 2U, (uint8_t)text[index]);
    st16(ORACLE_PATTERN + index * 2U, 0U);
}

static int oracle_name_is(const char *expected)
{
    uint32_t index;

    for (index = 0U; expected[index]; ++index) {
        if (ld16(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_NAME_OFFSET +
                 index * 2U) != (uint8_t)expected[index])
            return 0;
    }
    return ld16(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_NAME_OFFSET +
                index * 2U) == 0U;
}

static uint32_t oracle_first(CPU *cpu, const char *pattern)
{
    uint32_t esp;

    oracle_write_wide_ascii(pattern);
    memset((void *)(uintptr_t)ORACLE_OUTPUT, 0xcc,
           ISAAC_VITA_FIND_DATA_SIZE);
    esp = oracle_prepare(cpu, ORACLE_PATTERN, ORACLE_OUTPUT);
    if (!isaac_vita_find_import(cpu, ISAAC_VITA_FIND_FIRST_NAME) ||
        cpu->esp != esp + 12U)
        return 0U;
    return cpu->eax;
}

static int oracle_next(CPU *cpu, uint32_t token)
{
    uint32_t esp = oracle_prepare(cpu, token, ORACLE_OUTPUT);

    if (!isaac_vita_find_import(cpu, ISAAC_VITA_FIND_NEXT_NAME) ||
        cpu->esp != esp + 12U)
        return -1;
    return (int)cpu->eax;
}

static int oracle_close(CPU *cpu, uint32_t token)
{
    uint32_t esp = oracle_prepare(cpu, token, 0U);

    if (!isaac_vita_find_import(cpu, ISAAC_VITA_FIND_CLOSE_NAME) ||
        cpu->esp != esp + 8U)
        return 0;
    return cpu->eax == 1U && !cpu->fault;
}

static int oracle_wildcard(void)
{
    CPU cpu;
    uint32_t token;

    memset(&cpu, 0, sizeof cpu);
    cpu.last_error = 77U;
    token = oracle_first(&cpu, ISAAC_VITA_DATA_ROOT "\\*");
    if (token == ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || !token ||
        cpu.fault || cpu.last_error != 77U ||
        !oracle_name_is("alpha.txt") ||
        ld32(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_ATTRIBUTES_OFFSET) !=
            ISAAC_VITA_FIND_FILE_ATTRIBUTE_ARCHIVE ||
        s_dopen_calls != 1U || s_dread_calls != 3U)
        return 0;
    if (oracle_next(&cpu, token) != 1 || cpu.fault ||
        !oracle_name_is("mods") ||
        ld32(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_ATTRIBUTES_OFFSET) !=
            ISAAC_VITA_FIND_FILE_ATTRIBUTE_DIRECTORY)
        return 0;
    if (oracle_next(&cpu, token) != 1 || cpu.fault ||
        !oracle_name_is("BETA.BIN"))
        return 0;
    if (oracle_next(&cpu, token) != 0 || cpu.fault ||
        cpu.last_error != ISAAC_VITA_FIND_ERROR_NO_MORE_FILES)
        return 0;
    return oracle_close(&cpu, token) && s_dclose_calls == 1U;
}

static int oracle_exact(void)
{
    CPU cpu;
    uint32_t token;
    unsigned dopen_before = s_dopen_calls;
    unsigned dclose_before = s_dclose_calls;

    memset(&cpu, 0, sizeof cpu);
    cpu.last_error = 88U;
    token = oracle_first(&cpu, ISAAC_VITA_DATA_ROOT "\\alpha.txt");
    if (token == ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || !token ||
        cpu.fault || cpu.last_error != 88U ||
        !oracle_name_is("alpha.txt") ||
        ld32(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_ATTRIBUTES_OFFSET) !=
            ISAAC_VITA_FIND_FILE_ATTRIBUTE_ARCHIVE ||
        ld32(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_SIZE_LOW_OFFSET) != 123U ||
        s_dopen_calls != dopen_before)
        return 0;
    if (oracle_next(&cpu, token) != 0 || cpu.fault ||
        cpu.last_error != ISAAC_VITA_FIND_ERROR_NO_MORE_FILES ||
        !oracle_close(&cpu, token) || s_dclose_calls != dclose_before)
        return 0;

    cpu.last_error = 89U;
    token = oracle_first(&cpu, ISAAC_VITA_DATA_ROOT "\\mods");
    if (token == ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || !token ||
        cpu.fault || cpu.last_error != 89U || !oracle_name_is("mods") ||
        ld32(ORACLE_OUTPUT + ISAAC_VITA_FIND_DATA_ATTRIBUTES_OFFSET) !=
            ISAAC_VITA_FIND_FILE_ATTRIBUTE_DIRECTORY ||
        !oracle_close(&cpu, token) || s_dclose_calls != dclose_before)
        return 0;
    return 1;
}

static int oracle_missing(void)
{
    CPU cpu;
    uint32_t token;

    memset(&cpu, 0, sizeof cpu);
    token = oracle_first(&cpu, ISAAC_VITA_DATA_ROOT "\\absent.txt");
    if (token != ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || cpu.fault ||
        cpu.last_error != ISAAC_VITA_FIND_ERROR_FILE_NOT_FOUND)
        return 0;
    token = oracle_first(&cpu,
                         ISAAC_VITA_DATA_ROOT "\\missing\\absent.txt");
    if (token != ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || cpu.fault ||
        cpu.last_error != ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND)
        return 0;
    token = oracle_first(&cpu, ISAAC_VITA_DATA_ROOT "\\missing\\*");
    if (token != ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || cpu.fault ||
        cpu.last_error != ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND)
        return 0;
    token = oracle_first(&cpu,
                         ISAAC_VITA_DATA_ROOT "\\alpha.txt\\child.bin");
    if (token != ISAAC_VITA_FIND_INVALID_HANDLE_VALUE || cpu.fault ||
        cpu.last_error != ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND)
        return 0;
    token = oracle_first(&cpu, ISAAC_VITA_DATA_ROOT "\\alpha.txt\\*");
    return token == ISAAC_VITA_FIND_INVALID_HANDLE_VALUE && !cpu.fault &&
           cpu.last_error == ISAAC_VITA_FIND_ERROR_PATH_NOT_FOUND;
}

int main(void)
{
    void *mapping = mmap((void *)(uintptr_t)ORACLE_GUEST_BASE,
                         ORACLE_GUEST_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                         -1, 0);
    int ok;

    if (mapping == MAP_FAILED ||
        (uintptr_t)mapping != (uintptr_t)ORACLE_GUEST_BASE) {
        perror("mmap");
        return 1;
    }
    ok = oracle_wildcard() && oracle_exact() && oracle_missing() &&
         !s_directory_open && s_getstat_calls == 7U;
    if (munmap(mapping, ORACLE_GUEST_SIZE) != 0)
        return 2;
    if (!ok)
        return 3;
    puts("Vita FindFile host semantics oracle: PASS");
    return 0;
}
