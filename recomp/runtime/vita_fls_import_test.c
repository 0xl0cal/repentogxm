/* Runnable softfp ARM oracle for the isolated KERNEL32 FLS batch. */
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_fls.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita FLS oracle requires a 32-bit identity-mapped host
#endif

enum {
    CALLBACK_GOOD      = 0x00401000U,
    CALLBACK_FAULT     = 0x00401010U,
    CALLBACK_BAD_STACK = 0x00401020U
};

typedef struct fls_import_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t iat_va;
} fls_import_evidence;

typedef struct fls_callsite_evidence {
    const char *name;
    uint32_t call_rva;
    uint32_t return_rva;
} fls_callsite_evidence;

static const fls_import_evidence s_import_evidence[] = {
    { ISAAC_VITA_FLS_FREE_NAME,
      ISAAC_VITA_FLS_FREE_IAT_RVA, ISAAC_VITA_FLS_FREE_IAT_VA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_IAT_RVA, ISAAC_VITA_FLS_SETVALUE_IAT_VA },
    { ISAAC_VITA_FLS_ALLOC_NAME,
      ISAAC_VITA_FLS_ALLOC_IAT_RVA, ISAAC_VITA_FLS_ALLOC_IAT_VA }
};

static const fls_callsite_evidence s_callsite_evidence[] = {
    { ISAAC_VITA_FLS_FREE_NAME,
      ISAAC_VITA_FLS_FREE_CALL_RVA, ISAAC_VITA_FLS_FREE_RETURN_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_0_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_0_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_1_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_1_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_2_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_2_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_3_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_3_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_4_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_4_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_5_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_5_RVA },
    { ISAAC_VITA_FLS_SETVALUE_NAME,
      ISAAC_VITA_FLS_SETVALUE_CALL_6_RVA,
      ISAAC_VITA_FLS_SETVALUE_RETURN_6_RVA },
    { ISAAC_VITA_FLS_ALLOC_NAME,
      ISAAC_VITA_FLS_ALLOC_CALL_0_RVA,
      ISAAC_VITA_FLS_ALLOC_RETURN_0_RVA },
    { ISAAC_VITA_FLS_ALLOC_NAME,
      ISAAC_VITA_FLS_ALLOC_CALL_1_RVA,
      ISAAC_VITA_FLS_ALLOC_RETURN_1_RVA }
};

_Static_assert(sizeof s_import_evidence / sizeof s_import_evidence[0] ==
               ISAAC_VITA_FLS_IMPORT_COUNT,
               "FLS oracle lost an exact import record");
_Static_assert(sizeof s_callsite_evidence /
               sizeof s_callsite_evidence[0] ==
               ISAAC_VITA_FLS_PHYSICAL_CALLSITE_COUNT,
               "FLS oracle lost an exact physical callsite");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_FLS_FREE_IAT_RVA ==
               ISAAC_VITA_FLS_FREE_IAT_VA,
               "FlsFree IAT VA no longer matches the selected base");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_FLS_SETVALUE_IAT_RVA ==
               ISAAC_VITA_FLS_SETVALUE_IAT_VA,
               "FlsSetValue IAT VA no longer matches the selected base");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_FLS_ALLOC_IAT_RVA ==
               ISAAC_VITA_FLS_ALLOC_IAT_VA,
               "FlsAlloc IAT VA no longer matches the selected base");

_Alignas(8) static uint32_t s_frame[16];
static jmp_buf s_fault_jump;
static unsigned s_calls;
static unsigned *s_callback_observed_count;
static unsigned s_callback_expected_count;
static unsigned s_callback_calls;
static uint32_t s_callback_argument;
static int s_fault_must_jump;

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint32_t prepare_call(CPU *c, uint32_t argument0,
                             uint32_t argument1)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[4]);
    s_frame[4] = 0x0badc0deU;
    s_frame[5] = argument0;
    s_frame[6] = argument1;
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

void guest_call(CPU *__restrict c, uint32_t target)
{
    if (ld32(c->esp) != ISAAC_VITA_FLS_CALLBACK_SENTINEL) {
        guest_fault(c, target, "oracle FLS callback missing return sentinel");
        return;
    }

    ++s_callback_calls;
    s_callback_argument = ld32(c->esp + 4U);
    if (s_callback_observed_count &&
        *s_callback_observed_count != s_callback_expected_count) {
        guest_fault(c, target, "FLS count was not incremented pre-callback");
        return;
    }

    switch (target) {
    case CALLBACK_GOOD:
        (void)gpop(c);
        c->esp += 4U;
        break;
    case CALLBACK_FAULT:
        guest_fault(c, target, "oracle nested FLS callback fault");
        break;
    case CALLBACK_BAD_STACK:
        /* Remove only the return, leaving the stdcall argument behind. */
        (void)gpop(c);
        break;
    default:
        guest_fault(c, target, "oracle received unknown FLS callback");
        break;
    }
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    do {
        hash = hash_byte(hash, *cursor);
    } while (*cursor++ != 0U);
    return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    uint32_t shift;

    for (shift = 0U; shift < 32U; shift += 8U)
        hash = hash_byte(hash, (uint8_t)(value >> shift));
    return hash;
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_FLS_IMPORT_COUNT; ++index) {
        hash = hash_text(hash, s_import_evidence[index].name);
        hash = hash_u32(hash, s_import_evidence[index].iat_rva);
        hash = hash_u32(hash, s_import_evidence[index].iat_va);
    }
    for (index = 0U; index < ISAAC_VITA_FLS_PHYSICAL_CALLSITE_COUNT;
         ++index) {
        hash = hash_text(hash, s_callsite_evidence[index].name);
        hash = hash_u32(hash, s_callsite_evidence[index].call_rva);
        hash = hash_u32(hash, s_callsite_evidence[index].return_rva);
    }
    hash = hash_u32(hash, ISAAC_VITA_FLS_BOOT_CALL_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_FLS_SLOT_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_FLS_BOOT_CALLBACK_VA);
    hash = hash_u32(hash, ISAAC_VITA_FLS_BOOT_FIRST_VALUE);
    hash = hash_u32(hash, ISAAC_VITA_FLS_CALLBACK_SENTINEL);
    return hash;
}

static int counted_call(CPU *c, const char *name, uint32_t argument0,
                        uint32_t argument1, uint32_t stack_advance,
                        uint32_t expected_eax)
{
    uint32_t esp = prepare_call(c, argument0, argument1);
    unsigned expected_count = s_calls + 1U;

    if (!isaac_vita_fls_import_counted(c, name, &s_calls))
        return 0;
    return !c->fault && s_calls == expected_count &&
           c->esp == esp + stack_advance && c->eax == expected_eax;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    uint32_t esp;
    uint32_t index;
    uint32_t slot;

    if (!pointer_fits(s_frame))
        return 1;
    if (evidence_hash() != UINT64_C(0x018b1167eee6fa77))
        return 2;
    if (ISAAC_VITA_FLS_BOOT_CALL_COUNT != 3U ||
        ISAAC_VITA_FLS_SLOT_COUNT != 128U)
        return 3;

    /* Unknown and absent-family names are mutation-free dispatcher handoffs. */
    esp = prepare_call(&cpu, 1U, 2U);
    cpu.eax = 0x12345678U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_fls_import_counted(
            &cpu, "KERNEL32.dll!FlsGetValue", &s_calls) != 0 ||
        s_calls != 0U || cpu.esp != esp ||
        memcmp(&snapshot, &cpu, sizeof cpu) != 0)
        return 4;
    if (isaac_vita_fls_import_counted(&cpu, NULL, &s_calls) != 0 ||
        s_calls != 0U || memcmp(&snapshot, &cpu, sizeof cpu) != 0)
        return 5;

    /* Invalid Set/Free calls cannot mutate a valid neighboring slot. */
    if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      CALLBACK_GOOD, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0x11223344U, 12U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      1U, 0xaabbccddU, 12U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      1U, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      ISAAC_VITA_FLS_SLOT_COUNT, 1U, 12U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      UINT32_MAX, 0U, 8U, 0U))
        return 6;

    s_callback_observed_count = &s_calls;
    s_callback_expected_count = s_calls + 1U;
    if (!counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 1U) ||
        s_callback_calls != 1U ||
        s_callback_argument != 0x11223344U)
        return 7;
    s_callback_observed_count = NULL;
    if (!counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 0U))
        return 8;

    /* A null callback, or a zero value, suppresses callback execution. */
    if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      0U, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0x55667788U, 12U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      CALLBACK_GOOD, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0U, 12U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 1U) ||
        s_callback_calls != 1U)
        return 9;

    /* Lowest-index allocation, exact exhaustion sentinel, and deterministic
     * reuse are all covered across the entire fixed slot table. */
    for (index = 0U; index < ISAAC_VITA_FLS_SLOT_COUNT; ++index) {
        if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                          0U, 0U, 8U, index))
            return 10;
    }
    if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      0U, 0U, 8U, ISAAC_VITA_FLS_OUT_OF_INDEXES) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      37U, 0U, 8U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      0U, 0U, 8U, 37U))
        return 11;
    for (index = 0U; index < ISAAC_VITA_FLS_SLOT_COUNT; ++index) {
        if (!counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                          index, 0U, 8U, 1U))
            return 12;
    }

    /* A returning nested fault is counted first and leaves the slot intact. */
    if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      CALLBACK_FAULT, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0x89abcdefU, 12U, 1U))
        return 13;
    s_callback_observed_count = &s_calls;
    s_callback_expected_count = s_calls + 1U;
    esp = prepare_call(&cpu, 0U, 0U);
    if (!isaac_vita_fls_import_counted(
            &cpu, ISAAC_VITA_FLS_FREE_NAME, &s_calls) ||
        s_calls != s_callback_expected_count || !cpu.fault ||
        s_callback_calls != 2U ||
        s_callback_argument != 0x89abcdefU || cpu.esp == esp + 8U)
        return 14;
    s_callback_observed_count = NULL;
    if (!counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0U, 12U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 1U))
        return 15;

    /* Bad stdcall cleanup faults and likewise cannot free the live slot. */
    if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      CALLBACK_BAD_STACK, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0x76543210U, 12U, 1U))
        return 16;
    s_callback_observed_count = &s_calls;
    s_callback_expected_count = s_calls + 1U;
    esp = prepare_call(&cpu, 0U, 0U);
    if (!isaac_vita_fls_import_counted(
            &cpu, ISAAC_VITA_FLS_FREE_NAME, &s_calls) ||
        s_calls != s_callback_expected_count || !cpu.fault ||
        s_callback_calls != 3U ||
        s_callback_argument != 0x76543210U || cpu.esp == esp + 8U)
        return 17;
    s_callback_observed_count = NULL;
    if (!counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0U, 12U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 1U))
        return 18;

    /* Production guest_fault unwinds.  Freeze the pre-count guarantee across
     * that nonlocal boundary, then prove the interrupted Free kept its slot. */
    if (!counted_call(&cpu, ISAAC_VITA_FLS_ALLOC_NAME,
                      CALLBACK_FAULT, 0U, 8U, 0U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0xcafef00dU, 12U, 1U))
        return 19;
    s_callback_observed_count = &s_calls;
    s_callback_expected_count = s_calls + 1U;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)prepare_call(&cpu, 0U, 0U);
        (void)isaac_vita_fls_import_counted(
            &cpu, ISAAC_VITA_FLS_FREE_NAME, &s_calls);
        return 20;
    }
    s_fault_must_jump = 0;
    if (s_calls != s_callback_expected_count ||
        s_callback_calls != 4U ||
        s_callback_argument != 0xcafef00dU)
        return 21;
    s_callback_observed_count = NULL;
    if (!counted_call(&cpu, ISAAC_VITA_FLS_SETVALUE_NAME,
                      0U, 0U, 12U, 1U) ||
        !counted_call(&cpu, ISAAC_VITA_FLS_FREE_NAME,
                      0U, 0U, 8U, 1U))
        return 22;

    /* Exercise the uncounted production wrapper without changing the frozen
     * counted total. */
    esp = prepare_call(&cpu, 0U, 0U);
    if (!isaac_vita_fls_import(&cpu, ISAAC_VITA_FLS_ALLOC_NAME) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U)
        return 23;
    slot = cpu.eax;
    esp = prepare_call(&cpu, slot, 0U);
    if (!isaac_vita_fls_import(&cpu, ISAAC_VITA_FLS_FREE_NAME) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 1U)
        return 24;

    if (s_calls != 288U)
        return 25;
    return 0;
}
