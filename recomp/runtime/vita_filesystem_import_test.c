/* Compile/link/source oracle for the Vita narrow-UCRT filesystem boundary. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "host_vita_filesystem.h"
#include "host_vita_startup.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita filesystem oracle requires a 32-bit identity-mapped target
#endif

typedef struct filesystem_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t call_count;
    uint64_t call_hash;
} filesystem_evidence;

typedef struct filesystem_callsite {
    uint32_t owner_rva;
    uint32_t call_rva;
    uint32_t return_rva;
} filesystem_callsite;

static const filesystem_evidence s_evidence[] = {
    { ISAAC_VITA_FILESYSTEM_REMOVE_NAME,
      ISAAC_VITA_FILESYSTEM_REMOVE_IAT_RVA,
      ISAAC_VITA_FILESYSTEM_REMOVE_CALL_COUNT,
      ISAAC_VITA_FILESYSTEM_REMOVE_CALL_FNV64 },
    { ISAAC_VITA_FILESYSTEM_ACCESS_NAME,
      ISAAC_VITA_FILESYSTEM_ACCESS_IAT_RVA,
      ISAAC_VITA_FILESYSTEM_ACCESS_CALL_COUNT,
      ISAAC_VITA_FILESYSTEM_ACCESS_CALL_FNV64 },
};

#define ISAAC_VITA_FILESYSTEM_CALLSITE(owner, call, ret) \
    { owner, call, ret },
static const filesystem_callsite s_remove_calls[] = {
    ISAAC_VITA_FILESYSTEM_REMOVE_CALLS(
        ISAAC_VITA_FILESYSTEM_CALLSITE)
};
#undef ISAAC_VITA_FILESYSTEM_CALLSITE

_Static_assert(sizeof s_evidence / sizeof s_evidence[0] == 2U,
               "filesystem evidence table drifted");
_Static_assert(sizeof s_remove_calls / sizeof s_remove_calls[0] == 8U,
               "remove callsite table drifted");
_Static_assert((ISAAC_VITA_FILESYSTEM_LAST_IAT_RVA -
                ISAAC_VITA_FILESYSTEM_FIRST_IAT_RVA) / 4U + 1U == 2U,
               "filesystem IAT block must contain exactly two slots");
_Static_assert(ISAAC_VITA_FILESYSTEM_REMOVE_CALL_COUNT == 8U &&
                   ISAAC_VITA_FILESYSTEM_ACCESS_CALL_COUNT == 1U &&
                   ISAAC_VITA_FILESYSTEM_PHYSICAL_CALL_COUNT == 9U,
               "filesystem physical census drifted");

int32_t g_isaac_vita_crt_errno;

_Alignas(8) static uint32_t s_frame[8];
static char s_guest_test_path[] =
    "ux0:data/isaacr001/vita_filesystem_oracle.tmp";
static char s_native_test_path[] =
    "ux0:/data/isaacr001/vita_filesystem_oracle.tmp";
static char s_guest_root[] = "ux0:data/isaacr001";
static char s_escape_path[] = "app0:/outside-title-root";
static char s_missing_path[] =
    "ux0:data/isaacr001/vita_filesystem_oracle_missing_9f24.tmp";

void isaac_vita_log(const char *format, ...)
{
    (void)format;
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
    guest_fault(c, address, "filesystem oracle guest stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)pc;
    guest_fault(c, c ? c->esp : 0U,
                "filesystem oracle guest stack owner violation");
    return 0;
}

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint32_t prepare_call(CPU *c, const char *path, uint32_t mode)
{
    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[2] = 0x0badc0deU;
    s_frame[3] = pointer32(path);
    s_frame[4] = mode;
    c->esp = pointer32(&s_frame[2]);
    c->eax = 0x11223344U;
    return c->esp;
}

static uint64_t hash_rva(uint64_t hash, uint32_t rva)
{
    unsigned byte_index;

    for (byte_index = 0U; byte_index < 4U; ++byte_index) {
        hash ^= (uint8_t)(rva >> (byte_index * 8U));
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

static int check_evidence(void)
{
    uint64_t remove_hash = UINT64_C(0xcbf29ce484222325);
    uint64_t access_hash = UINT64_C(0xcbf29ce484222325);
    uint32_t previous = 0U;
    unsigned i;

    if (strcmp(ISAAC_VITA_FILESYSTEM_DLL_NAME,
               "api-ms-win-crt-filesystem-l1-1-0.dll") != 0 ||
        strcmp(s_evidence[0].name,
               "api-ms-win-crt-filesystem-l1-1-0.dll!remove") != 0 ||
        strcmp(s_evidence[1].name,
               "api-ms-win-crt-filesystem-l1-1-0.dll!_access") != 0 ||
        s_evidence[0].iat_rva != 0x006064ecU ||
        s_evidence[1].iat_rva != 0x006064f0U)
        return 0;
    for (i = 0U; i < sizeof s_remove_calls / sizeof s_remove_calls[0]; ++i) {
        if (s_remove_calls[i].call_rva <= previous ||
            s_remove_calls[i].return_rva !=
                s_remove_calls[i].call_rva + 6U ||
            s_remove_calls[i].owner_rva > s_remove_calls[i].call_rva)
            return 0;
        previous = s_remove_calls[i].call_rva;
        remove_hash = hash_rva(remove_hash, s_remove_calls[i].call_rva);
    }
    access_hash = hash_rva(
        access_hash, ISAAC_VITA_FILESYSTEM_ACCESS_CALL_RVA);
    return remove_hash == ISAAC_VITA_FILESYSTEM_REMOVE_CALL_FNV64 &&
           access_hash == ISAAC_VITA_FILESYSTEM_ACCESS_CALL_FNV64 &&
           ISAAC_VITA_FILESYSTEM_ACCESS_RETURN_RVA ==
               ISAAC_VITA_FILESYSTEM_ACCESS_CALL_RVA + 6U &&
           ISAAC_VITA_FILESYSTEM_ACCESS_OWNER_RVA == 0x00596320U;
}

static int check_current_frontier(void)
{
    CPU c;
    char mapped[ISAAC_VITA_STARTUP_PATH_MAX + 1U];

    memset(&c, 0, sizeof c);
    if (ISAAC_VITA_FILESYSTEM_CURRENT_OWNER_RVA != 0x0025e3b0U ||
        ISAAC_VITA_FILESYSTEM_CURRENT_CALL_RVA != 0x0025e5daU ||
        ISAAC_VITA_FILESYSTEM_CURRENT_RETURN_RVA != 0x0025e5e0U ||
        ISAAC_VITA_FILESYSTEM_CURRENT_BUILD_CALL_RVA != 0x0025e412U ||
        ISAAC_VITA_FILESYSTEM_CURRENT_BUILD_RETURN_RVA != 0x0025e417U ||
        ISAAC_VITA_FILESYSTEM_CURRENT_FORMAT_RVA != 0x00742af8U ||
        strcmp(ISAAC_VITA_FILESYSTEM_CURRENT_FORMAT, "%s%s") != 0 ||
        ISAAC_VITA_FILESYSTEM_CURRENT_SUFFIX_RVA != 0x00747470U ||
        strcmp(ISAAC_VITA_FILESYSTEM_CURRENT_SUFFIX,
               "cmd_history.txt") != 0 ||
        ISAAC_VITA_FILESYSTEM_CURRENT_BUFFER_CAPACITY != 0x100U ||
        ISAAC_VITA_FILESYSTEM_USERPROFILE_BUFFER_VA != 0x987fd690U ||
        ISAAC_VITA_FILESYSTEM_USERPROFILE_NAME_RVA != 0x0075a9a8U ||
        strcmp(ISAAC_VITA_FILESYSTEM_USERPROFILE_NAME,
               "USERPROFILE") != 0 ||
        ISAAC_VITA_FILESYSTEM_USERPROFILE_FORMAT_RVA != 0x0075a9a4U ||
        strcmp(ISAAC_VITA_FILESYSTEM_USERPROFILE_FORMAT, "%s/") != 0 ||
        ISAAC_VITA_FILESYSTEM_CURRENT_IMPORT_ORDINAL != 11180950U ||
        ISAAC_VITA_FILESYSTEM_CURRENT_DYNAMIC_COUNT != 8861U ||
        strcmp(ISAAC_VITA_FILESYSTEM_CURRENT_GUEST_PATH,
               "ux0:data/isaacr001/cmd_history.txt") != 0 ||
        strcmp(ISAAC_VITA_FILESYSTEM_CURRENT_NATIVE_PATH,
               "ux0:/data/isaacr001/cmd_history.txt") != 0)
        return 0;
    if (!isaac_vita_startup_map_path(
            &c, pointer32(ISAAC_VITA_FILESYSTEM_CURRENT_GUEST_PATH),
            mapped, sizeof mapped) || c.fault ||
        strcmp(mapped, ISAAC_VITA_FILESYSTEM_CURRENT_NATIVE_PATH) != 0)
        return 0;
    return 1;
}

static int call_access(CPU *c, const char *path, uint32_t mode,
                       unsigned *calls, uint32_t expected_result,
                       int32_t initial_guest_errno,
                       int32_t expected_guest_errno)
{
    uint32_t esp = prepare_call(c, path, mode);
    int saved_errno = 73;

    errno = saved_errno;
    g_isaac_vita_crt_errno = initial_guest_errno;
    if (!isaac_vita_filesystem_import_counted(
            c, ISAAC_VITA_FILESYSTEM_ACCESS_NAME, calls) || c->fault ||
        c->eax != expected_result || c->esp != esp + 4U ||
        g_isaac_vita_crt_errno != expected_guest_errno ||
        errno != saved_errno)
        return 0;
    return 1;
}

static int call_remove(CPU *c, const char *path, unsigned *calls,
                       uint32_t expected_result,
                       int32_t initial_guest_errno,
                       int32_t expected_guest_errno)
{
    uint32_t esp = prepare_call(c, path, 0U);
    int saved_errno = 91;

    errno = saved_errno;
    g_isaac_vita_crt_errno = initial_guest_errno;
    if (!isaac_vita_filesystem_import_counted(
            c, ISAAC_VITA_FILESYSTEM_REMOVE_NAME, calls) || c->fault ||
        c->eax != expected_result || c->esp != esp + 4U ||
        g_isaac_vita_crt_errno != expected_guest_errno ||
        errno != saved_errno)
        return 0;
    return 1;
}

/* This lifecycle is a source oracle.  The task deliberately compiles and
 * links it but never executes ARM code on the build host. */
static int check_lifecycle(void)
{
    CPU c;
    CPU snapshot;
    FILE *stream;
    int closed;
    int wrote;
    uint32_t esp;
    unsigned calls = 0U;
    unsigned i;
    static const uint32_t modes[] = { 0U, 2U, 4U, 6U };

    (void)remove(s_native_test_path);
    stream = fopen(s_native_test_path, "wb");
    if (!stream)
        return 0;
    wrote = fputc('x', stream) != EOF;
    closed = fclose(stream) == 0;
    if (!wrote || !closed) {
        (void)remove(s_native_test_path);
        return 0;
    }
    for (i = 0U; i < sizeof modes / sizeof modes[0]; ++i) {
        if (!call_access(&c, s_guest_test_path, modes[i], &calls, 0U,
                         0x1357, 0x1357))
            return 0;
    }
    if (!call_remove(&c, s_guest_test_path, &calls, 0U,
                     0x2468, 0x2468) ||
        !call_access(&c, s_guest_test_path, 0U, &calls, UINT32_MAX,
                     0x3579, ENOENT) ||
        !call_remove(&c, s_missing_path, &calls, UINT32_MAX,
                     0x468a, ENOENT) ||
        !call_remove(&c, s_guest_root, &calls, UINT32_MAX,
                     0x579b, EACCES) ||
        !call_access(&c, s_guest_test_path, 1U, &calls, UINT32_MAX,
                     0x68ac, EINVAL))
        return 0;

    esp = prepare_call(&c, s_escape_path, 0U);
    if (!isaac_vita_filesystem_import_counted(
            &c, ISAAC_VITA_FILESYSTEM_ACCESS_NAME, &calls) || !c.fault ||
        c.esp != esp || calls != 10U)
        return 0;

    esp = prepare_call(&c, s_guest_test_path, 0U);
    memcpy(&snapshot, &c, sizeof snapshot);
    if (isaac_vita_filesystem_import_counted(
            &c, "api-ms-win-crt-filesystem-l1-1-0.dll!unknown",
            &calls) != 0 || calls != 10U || c.esp != esp ||
        memcmp(&c, &snapshot, sizeof c) != 0 ||
        isaac_vita_filesystem_import(&c, NULL) != 0)
        return 0;
    return 1;
}

int main(void)
{
    if ((uintptr_t)s_frame > UINT32_MAX ||
        (uintptr_t)s_guest_test_path > UINT32_MAX ||
        (uintptr_t)s_guest_root > UINT32_MAX ||
        (uintptr_t)s_escape_path > UINT32_MAX ||
        (uintptr_t)s_missing_path > UINT32_MAX)
        return 1;
    if (!check_evidence())
        return 2;
    if (!check_current_frontier())
        return 3;
    if (!check_lifecycle())
        return 4;
    return 0;
}
