/* Runnable softfp ARM oracle for the pure VCRUNTIME memory/search batch. */
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#endif

#include "guest.h"
#include "host_vita_memory.h"

#if UINTPTR_MAX != UINT32_MAX && \
    !defined(ISAAC_VITA_MEMORY_HOST_LOW_ORACLE)
#error Vita memory oracle requires a 32-bit identity-mapped host
#endif
#if !ISAAC_VITA_MEMORY_RANGE_GUARD
#error Vita memory oracle must exercise the production mapped-range guard
#endif

#if defined(ISAAC_VITA_MEMORY_RANGE_ORACLE)
int isaac_vita_memory_range_oracle_validate(uint32_t address, uint32_t count,
                                            int write);
#endif

typedef struct memory_call_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t thunk_rva;          /* zero for the representative direct call */
    uint32_t call_rva;
    uint32_t return_rva;
} memory_call_evidence;

static const memory_call_evidence s_evidence[] = {
    { ISAAC_VITA_MEMORY_STRCHR_NAME,
      ISAAC_VITA_MEMORY_STRCHR_IAT_RVA, 0U,
      ISAAC_VITA_MEMORY_STRCHR_CALL_RVA,
      ISAAC_VITA_MEMORY_STRCHR_RETURN_RVA },
    { ISAAC_VITA_MEMORY_MEMCHR_NAME,
      ISAAC_VITA_MEMORY_MEMCHR_IAT_RVA,
      ISAAC_VITA_MEMORY_MEMCHR_THUNK_RVA,
      ISAAC_VITA_MEMORY_MEMCHR_CALL_RVA,
      ISAAC_VITA_MEMORY_MEMCHR_RETURN_RVA },
    { ISAAC_VITA_MEMORY_MEMSET_NAME,
      ISAAC_VITA_MEMORY_MEMSET_IAT_RVA,
      ISAAC_VITA_MEMORY_MEMSET_THUNK_RVA,
      ISAAC_VITA_MEMORY_MEMSET_CALL_RVA,
      ISAAC_VITA_MEMORY_MEMSET_RETURN_RVA },
    { ISAAC_VITA_MEMORY_MEMCPY_NAME,
      ISAAC_VITA_MEMORY_MEMCPY_IAT_RVA,
      ISAAC_VITA_MEMORY_MEMCPY_THUNK_RVA,
      ISAAC_VITA_MEMORY_MEMCPY_CALL_RVA,
      ISAAC_VITA_MEMORY_MEMCPY_RETURN_RVA },
    { ISAAC_VITA_MEMORY_STRSTR_NAME,
      ISAAC_VITA_MEMORY_STRSTR_IAT_RVA,
      ISAAC_VITA_MEMORY_STRSTR_THUNK_RVA,
      ISAAC_VITA_MEMORY_STRSTR_CALL_RVA,
      ISAAC_VITA_MEMORY_STRSTR_RETURN_RVA },
    { ISAAC_VITA_MEMORY_MEMMOVE_NAME,
      ISAAC_VITA_MEMORY_MEMMOVE_IAT_RVA,
      ISAAC_VITA_MEMORY_MEMMOVE_THUNK_RVA,
      ISAAC_VITA_MEMORY_MEMMOVE_CALL_RVA,
      ISAAC_VITA_MEMORY_MEMMOVE_RETURN_RVA }
};

_Static_assert(sizeof s_evidence / sizeof s_evidence[0] ==
               ISAAC_VITA_MEMORY_IMPORT_COUNT,
               "memory oracle lost an exact import record");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_MEMORY_MEMSET_IAT_RVA ==
               ISAAC_VITA_MEMORY_MEMSET_IAT_VA,
               "live memset IAT VA no longer matches the selected base");

_Alignas(8) static uint32_t s_frame[12];
_Alignas(8) static uint8_t s_bytes[96];
static char s_haystack[] = "alpha/beta/gamma";
static char s_needle[] = "beta";
static char s_absent[] = "delta";
static char s_empty[] = "";
static CPU s_jump_cpu;
static jmp_buf s_fault_jump;
static unsigned s_jump_count;
static int s_fault_must_jump;
static unsigned s_range_query_calls;
static unsigned s_log_calls;
static int s_range_query_mode;
static char s_last_log[512];

enum {
    ORACLE_RANGE_NORMAL = 0,
    ORACLE_RANGE_DENY_WRITE,
    ORACLE_RANGE_DENY_READ,
    ORACLE_RANGE_UNKNOWN_ACCESS,
    ORACLE_RANGE_FAIL
};

static int oracle_range_within(uintptr_t first, uint32_t size,
                               uintptr_t base, uint32_t capacity)
{
    return first >= base && size <= capacity &&
        first - base <= capacity - size;
}

static int oracle_range_query(
    uintptr_t first, uint32_t size, uint32_t *mapped_base,
    uint32_t *mapped_size, uint32_t *access)
{
    uintptr_t bytes_base = (uintptr_t)s_bytes;
    /* Synthetic enclosing maps; only the reproducer pointers/count below are
     * copied from hardware. */
    const uint32_t reproducer_map_base = 0x83400000U;
    const uint32_t reproducer_map_size = 0x00200000U;
    const uint32_t large_guest_base = 0x84000000U;
    const uint32_t large_guest_size = 0x04000000U;

    ++s_range_query_calls;
    if (!mapped_base || !mapped_size || !access || !size ||
        bytes_base > UINT32_MAX || s_range_query_mode == ORACLE_RANGE_FAIL)
        return -0x1234;

    if (oracle_range_within(first, size, bytes_base,
                            (uint32_t)sizeof s_bytes)) {
        *mapped_base = (uint32_t)bytes_base;
        *mapped_size = (uint32_t)sizeof s_bytes;
    } else if (oracle_range_within(
                   first, size, reproducer_map_base, reproducer_map_size)) {
        *mapped_base = reproducer_map_base;
        *mapped_size = reproducer_map_size;
    } else if (oracle_range_within(
                   first, size, large_guest_base, large_guest_size)) {
        *mapped_base = large_guest_base;
        *mapped_size = large_guest_size;
    } else {
        return -0x1234;
    }
    *access = (s_range_query_mode == ORACLE_RANGE_DENY_READ ||
               s_range_query_mode == ORACLE_RANGE_UNKNOWN_ACCESS
                   ? 0U : 4U) |
        (s_range_query_mode == ORACLE_RANGE_DENY_WRITE ||
         s_range_query_mode == ORACLE_RANGE_UNKNOWN_ACCESS
             ? 0U : 2U);
    return 0;
}

#if defined(__vita__)
int sceKernelGetMemBlockInfoByAddr(
    void *base, SceKernelMemBlockInfo *info)
{
    uint32_t mapped_base;
    uint32_t mapped_size;
    uint32_t access;
    int result;

    if (!info || info->size != sizeof *info)
        return -0x1234;
    result = oracle_range_query(
        (uintptr_t)base, 1U,
        &mapped_base, &mapped_size, &access);
    if (result < 0)
        return result;
    info->mappedBase = (void *)(uintptr_t)mapped_base;
    info->mappedSize = (SceSize)mapped_size;
    info->memoryType = SCE_KERNEL_MEMORY_TYPE_NORMAL;
    info->access = access;
    info->type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    return 0;
}
#else
int isaac_vita_memory_range_oracle_query(
    uint32_t address, uint32_t count, uint32_t *mapped_base,
    uint32_t *mapped_size, uint32_t *access)
{
    (void)count;
    return oracle_range_query(
        (uintptr_t)address, 1U, mapped_base, mapped_size, access);
}
#endif

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    ++s_log_calls;
    va_start(arguments, format);
    (void)vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    s_last_log[sizeof s_last_log - 1U] = '\0';
}

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint32_t prepare_call(CPU *c, uint32_t argument0,
                             uint32_t argument1, uint32_t argument2)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[3]);
    s_frame[3] = 0x0badc0deU;
    s_frame[4] = argument0;
    s_frame[5] = argument1;
    s_frame[6] = argument2;
    c->esp = esp;
    return esp;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
    if (s_fault_must_jump)
        longjmp(s_fault_jump, 1);
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_MEMORY_IMPORT_COUNT; ++i) {
        const unsigned char *text =
            (const unsigned char *)s_evidence[i].name;
        uint32_t values[4];
        uint32_t j;
        do {
            hash = hash_byte(hash, *text);
        } while (*text++ != 0U);
        values[0] = s_evidence[i].iat_rva;
        values[1] = s_evidence[i].thunk_rva;
        values[2] = s_evidence[i].call_rva;
        values[3] = s_evidence[i].return_rva;
        for (j = 0U; j < 4U; ++j) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash, (uint8_t)(values[j] >> shift));
        }
    }
    {
        const uint32_t live[3] = {
            ISAAC_VITA_MEMORY_MEMSET_LIVE_DST,
            ISAAC_VITA_MEMORY_MEMSET_LIVE_VALUE,
            ISAAC_VITA_MEMORY_MEMSET_LIVE_SIZE
        };
        uint32_t i_live;
        for (i_live = 0U; i_live < 3U; ++i_live) {
            uint32_t shift;
            for (shift = 0U; shift < 32U; shift += 8U)
                hash = hash_byte(hash,
                                 (uint8_t)(live[i_live] >> shift));
        }
    }
    return hash;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    unsigned calls = 0U;
    unsigned before;
    uint32_t esp;
    uint32_t base;
    uint32_t i;

    if (!pointer_fits(s_frame) || !pointer_fits(s_bytes) ||
        !pointer_fits(s_haystack) || !pointer_fits(s_needle) ||
        !pointer_fits(s_absent) || !pointer_fits(s_empty))
        return 1;
    if (evidence_hash() != UINT64_C(0x106543e5ff4e7595))
        return 2;
    if (ISAAC_VITA_MEMORY_MEMSET_LIVE_DST != 0x987a9e34U ||
        ISAAC_VITA_MEMORY_MEMSET_LIVE_VALUE != 0U ||
        ISAAC_VITA_MEMORY_MEMSET_LIVE_SIZE != 0x78U)
        return 3;

#if defined(ISAAC_VITA_MEMORY_RANGE_ORACLE)
    /* Pin the diagnostic threshold without asking libc to perform a synthetic
     * 16 MiB copy.  The threshold itself stays query-free; threshold+1 gets
     * containment and access validation. */
    if (isaac_vita_memory_range_oracle_validate(
            0x84000000U, 0x01000000U, 1) != 0 ||
        s_range_query_calls != 0U)
        return 42;
    if (isaac_vita_memory_range_oracle_validate(
            0x84000000U, 0x01000001U, 1) != 0 ||
        s_range_query_calls != 1U)
        return 43;
    if (isaac_vita_memory_range_oracle_validate(
            0x87000000U, 0x01000001U, 1) != 4 ||
        s_range_query_calls != 2U)
        return 44;
    s_range_query_mode = ORACLE_RANGE_UNKNOWN_ACCESS;
    if (isaac_vita_memory_range_oracle_validate(
            0x84000000U, 0x01000001U, 0) != 0 ||
        s_range_query_calls != 3U)
        return 45;
    s_range_query_mode = ORACLE_RANGE_NORMAL;
    s_range_query_calls = 0U;
#endif

    /* Rejected names are a mutation-free handoff to the outer dispatcher. */
    esp = prepare_call(&cpu, 1U, 2U, 3U);
    cpu.eax = 0x12345678U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_memory_import_counted(
            &cpu, "VCRUNTIME140.dll!not_a_memory_import", &calls) != 0 ||
        calls != 0U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        cpu.esp != esp)
        return 4;

    /* memset: low-byte value, guard preservation, cdecl stack and dst return. */
    memset(s_bytes, 0xa5, sizeof s_bytes);
    base = pointer32(s_bytes);
    esp = prepare_call(&cpu, base + 8U, 0x1234567eU, 24U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMSET_NAME, &calls) ||
        calls != 1U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 8U)
        return 5;
    for (i = 0U; i < sizeof s_bytes; ++i) {
        uint8_t expected = i >= 8U && i < 32U ? 0x7eU : 0xa5U;
        if (s_bytes[i] != expected)
            return 6;
    }

    /* A zero-size memory call does not dereference its null guest pointer. */
    esp = prepare_call(&cpu, 0U, 0x55U, 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMSET_NAME, &calls) ||
        calls != 2U || cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U)
        return 7;

    /* memcpy is non-overlap; both source and destination guards stay exact. */
    for (i = 0U; i < sizeof s_bytes; ++i)
        s_bytes[i] = (uint8_t)i;
    esp = prepare_call(&cpu, base + 56U, base + 8U, 24U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCPY_NAME, &calls) ||
        calls != 3U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 56U)
        return 8;
    for (i = 0U; i < 24U; ++i) {
        if (s_bytes[56U + i] != (uint8_t)(8U + i))
            return 9;
    }
    if (s_bytes[55] != 55U || s_bytes[80] != 80U)
        return 10;

    /* memmove must preserve both overlap directions, unlike memcpy. */
    for (i = 0U; i < sizeof s_bytes; ++i)
        s_bytes[i] = (uint8_t)i;
    esp = prepare_call(&cpu, base + 12U, base + 8U, 24U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMMOVE_NAME, &calls) ||
        calls != 4U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 12U)
        return 11;
    for (i = 0U; i < 24U; ++i) {
        if (s_bytes[12U + i] != (uint8_t)(8U + i))
            return 12;
    }
    for (i = 0U; i < sizeof s_bytes; ++i)
        s_bytes[i] = (uint8_t)i;
    esp = prepare_call(&cpu, base + 8U, base + 12U, 24U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMMOVE_NAME, &calls) ||
        calls != 5U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 8U)
        return 13;
    for (i = 0U; i < 24U; ++i) {
        if (s_bytes[8U + i] != (uint8_t)(12U + i))
            return 14;
    }

    /* strchr includes the terminator in its search and returns guest VAs. */
    base = pointer32(s_haystack);
    esp = prepare_call(&cpu, base, (uint32_t)'/', 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRCHR_NAME, &calls) ||
        calls != 6U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 5U)
        return 15;
    esp = prepare_call(&cpu, base, 0U, 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRCHR_NAME, &calls) ||
        calls != 7U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + sizeof s_haystack - 1U)
        return 16;
    esp = prepare_call(&cpu, base, (uint32_t)'!', 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRCHR_NAME, &calls) ||
        calls != 8U || cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U)
        return 17;

    /* Recognized faults leave the x86 frame intact and are counted first. */
    esp = prepare_call(&cpu, 0U, (uint32_t)'x', 0U);
    cpu.eax = 0xfeedfaceU;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRCHR_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0xfeedfaceU || cpu.fault_addr != 0U || !cpu.fault ||
        strcmp(cpu.fault, "strchr received a null guest pointer") != 0)
        return 18;

    /* memchr truncates the int needle, observes count, and permits 0/null. */
    for (i = 0U; i < sizeof s_bytes; ++i)
        s_bytes[i] = (uint8_t)i;
    base = pointer32(s_bytes);
    esp = prepare_call(&cpu, base + 8U, 0x00000110U, 32U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCHR_NAME, &calls) ||
        calls != 10U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 16U)
        return 19;
    esp = prepare_call(&cpu, base + 8U, 0xffU, 32U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCHR_NAME, &calls) ||
        calls != 11U || cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U)
        return 20;
    esp = prepare_call(&cpu, 0U, 0xffU, 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCHR_NAME, &calls) ||
        calls != 12U || cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U)
        return 21;
    esp = prepare_call(&cpu, 0xfffffff8U, 0U, 16U);
    cpu.eax = 0xabcdef01U;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCHR_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0xabcdef01U || cpu.fault_addr != 0xfffffff8U ||
        !cpu.fault || strcmp(cpu.fault,
            "memchr received an invalid guest range") != 0)
        return 22;

    /* strstr: found, absent, empty-needle, then count-before-null-fault. */
    base = pointer32(s_haystack);
    esp = prepare_call(&cpu, base, pointer32(s_needle), 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRSTR_NAME, &calls) ||
        calls != 14U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != base + 6U)
        return 23;
    esp = prepare_call(&cpu, base, pointer32(s_absent), 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRSTR_NAME, &calls) ||
        calls != 15U || cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0U)
        return 24;
    esp = prepare_call(&cpu, base, pointer32(s_empty), 0U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRSTR_NAME, &calls) ||
        calls != 16U || cpu.fault || cpu.esp != esp + 4U || cpu.eax != base)
        return 25;
    esp = prepare_call(&cpu, base, 0U, 0U);
    cpu.eax = 0x13579bdfU;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_STRSTR_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0x13579bdfU || cpu.fault_addr != 0U || !cpu.fault ||
        strcmp(cpu.fault, "strstr received a null guest pointer") != 0)
        return 26;

    if (calls != 17U)
        return 27;

    /* Ordinary non-empty and zero-sized movers above never enter sysmem. */
    if (s_range_query_calls != 0U || s_log_calls != 0U)
        return 28;

    /* Exact 2026-08-27 hardware reproducer.  This count does not wrap either
     * pointer, but it cannot belong to one mapped guest block.  Reject before
     * native memcpy, retain the cdecl frame, and emit all actionable state in
     * one durable record. */
    esp = prepare_call(&cpu, 0x83403a24U, 0x83403224U, 0x5848f42aU);
    s_frame[3] = 0x0059c417U;
    cpu.eax = 0x83403224U;
    cpu.ecx = 0x00000001U;
    cpu.edx = 0x83533470U;
    cpu.ebx = 0x83403a24U;
    cpu.ebp = 0x83403194U;
    cpu.esi = 0x5848f42aU;
    cpu.edi = 0x83403208U;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCPY_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0x83403224U || cpu.fault_addr != 0x83403a24U ||
        !cpu.fault || strcmp(cpu.fault,
            "memcpy received an invalid destination range") != 0 ||
        s_range_query_calls != 1U || s_log_calls != 1U)
        return 29;
    if (!strstr(s_last_log,
                "op=memcpy side=dst reason=4 query=0x00000000") ||
        !strstr(s_last_log, "map=0x83400000+0x00200000") ||
        !strstr(s_last_log,
                "return=0x0059c417 dst=0x83403a24 "
                "src=0x83403224 count=0x5848f42a") ||
        !strstr(s_last_log,
                "eax=83403224 ecx=00000001 edx=83533470 ebx=83403a24") ||
        !strstr(s_last_log,
                "ebp=83403194 esi=5848f42a edi=83403208"))
        return 30;

    /* Null/wrapping ranges fail before sysmem, and a rejected mover never
     * pops its x86 return or overwrites EAX. */
    esp = prepare_call(&cpu, 0U, 0x55U, 1U);
    cpu.eax = 0x11223344U;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMSET_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0x11223344U || cpu.fault_addr != 0U || !cpu.fault ||
        strcmp(cpu.fault,
            "memset received an invalid destination range") != 0 ||
        s_range_query_calls != 1U || s_log_calls != 2U ||
        !strstr(s_last_log, "op=memset side=dst reason=1"))
        return 31;

    esp = prepare_call(
        &cpu, 0x84000000U, 0x00001234U, 0x01000001U);
    cpu.eax = 0xaabbccddU;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCPY_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0xaabbccddU || cpu.fault_addr != 0x00001234U ||
        !cpu.fault || strcmp(cpu.fault,
            "memcpy received an invalid source range") != 0 ||
        s_range_query_calls != 3U || s_log_calls != 3U ||
        !strstr(s_last_log, "op=memcpy side=src reason=3"))
        return 32;

    s_range_query_mode = ORACLE_RANGE_DENY_WRITE;
    esp = prepare_call(
        &cpu, 0x84000000U, 0x85800000U, 0x01000001U);
    cpu.eax = 0x55667788U;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMMOVE_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0x55667788U || cpu.fault_addr != 0x84000000U ||
        !cpu.fault || strcmp(cpu.fault,
            "memmove received an invalid destination range") != 0 ||
        s_range_query_calls != 4U || s_log_calls != 4U ||
        !strstr(s_last_log, "op=memmove side=dst reason=5"))
        return 33;
    s_range_query_mode = ORACLE_RANGE_NORMAL;

    s_range_query_mode = ORACLE_RANGE_DENY_READ;
    esp = prepare_call(
        &cpu, 0x84000000U, 0x85800000U, 0x01000001U);
    cpu.eax = 0x10203040U;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMMOVE_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0x10203040U || cpu.fault_addr != 0x85800000U ||
        !cpu.fault || strcmp(cpu.fault,
            "memmove received an invalid source range") != 0 ||
        s_range_query_calls != 6U || s_log_calls != 5U ||
        !strstr(s_last_log, "op=memmove side=src reason=5"))
        return 34;
    s_range_query_mode = ORACLE_RANGE_NORMAL;

    esp = prepare_call(&cpu, 0xfffffff8U, base + 8U, 16U);
    cpu.eax = 0x99aabbccU;
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCPY_NAME, &calls) ||
        calls != before + 1U || cpu.esp != esp ||
        cpu.eax != 0x99aabbccU || cpu.fault_addr != 0xfffffff8U ||
        !cpu.fault || strcmp(cpu.fault,
            "memcpy received an invalid destination range") != 0 ||
        s_range_query_calls != 6U || s_log_calls != 6U ||
        !strstr(s_last_log, "op=memcpy side=dst reason=2"))
        return 35;

    /* All three movers preserve the established guest/CRT zero-count/null
     * contract. */
    esp = prepare_call(&cpu, 0U, 0U, 0U);
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMCPY_NAME, &calls) ||
        calls != before + 1U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != 0U || s_range_query_calls != 6U || s_log_calls != 6U)
        return 36;
    esp = prepare_call(&cpu, 0U, 0U, 0U);
    before = calls;
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMMOVE_NAME, &calls) ||
        calls != before + 1U || cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != 0U || s_range_query_calls != 6U || s_log_calls != 6U)
        return 37;

    /* Ordinary movers stay query-free even when the oracle would report an
     * unknown access mask.  The host-only threshold test above separately
     * pins Vita3K's access==0 acceptance. */
    for (i = 0U; i < sizeof s_bytes; ++i)
        s_bytes[i] = (uint8_t)i;
    base = pointer32(s_bytes);
    s_range_query_mode = ORACLE_RANGE_UNKNOWN_ACCESS;
    esp = prepare_call(&cpu, base + 56U, base + 8U, 8U);
    if (!isaac_vita_memory_import_counted(
            &cpu, ISAAC_VITA_MEMORY_MEMMOVE_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != base + 56U ||
        s_range_query_calls != 6U || s_log_calls != 6U ||
        memcmp(s_bytes + 56U, s_bytes + 8U, 8U) != 0)
        return 38;
    s_range_query_mode = ORACLE_RANGE_NORMAL;

    if (calls != 26U || s_range_query_mode == ORACLE_RANGE_FAIL)
        return 39;

    /* Exercise the production control-flow shape, not merely a returning
     * guest_fault mock.  The only state inspected after longjmp is static (or
     * assigned before setjmp), so ISO C's indeterminate-local rule is obeyed. */
    esp = prepare_call(&s_jump_cpu, 0U, (uint32_t)'x', 0U);
    s_jump_cpu.eax = 0x2468ace0U;
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_memory_import_counted(
            &s_jump_cpu, ISAAC_VITA_MEMORY_STRCHR_NAME, &s_jump_count);
        return 40;               /* production guest_fault must not return */
    }
    s_fault_must_jump = 0;
    if (s_jump_count != 27U || s_jump_cpu.esp != esp ||
        s_jump_cpu.eax != 0x2468ace0U || s_jump_cpu.fault_addr != 0U ||
        !s_jump_cpu.fault || strcmp(s_jump_cpu.fault,
            "strchr received a null guest pointer") != 0)
        return 41;

    return 0;
}
