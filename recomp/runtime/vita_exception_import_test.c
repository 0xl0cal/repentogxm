/* Softfp ARM source oracle for exception registration and setjmp/longjmp.
 * The build script compile/links and inspects this artifact; it is not host
 * runtime evidence. */
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_exception.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita exception oracle requires a 32-bit identity-mapped host
#endif

typedef struct exception_call_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t iat_va;
    uint32_t call_rva;
    uint32_t return_rva;
    uint32_t argument;
    uint32_t ordinal;
} exception_call_evidence;

static const exception_call_evidence s_current = {
    ISAAC_VITA_EXCEPTION_SET_FILTER_NAME,
    ISAAC_VITA_EXCEPTION_SET_FILTER_IAT_RVA,
    ISAAC_VITA_EXCEPTION_SET_FILTER_IAT_VA,
    ISAAC_VITA_EXCEPTION_SET_FILTER_CALL_RVA,
    ISAAC_VITA_EXCEPTION_SET_FILTER_RETURN_RVA,
    ISAAC_VITA_EXCEPTION_SET_FILTER_CALLBACK_VA,
    ISAAC_VITA_EXCEPTION_SET_FILTER_ORDINAL
};

static const exception_call_evidence s_next = {
    ISAAC_VITA_EXCEPTION_NEXT_NAME,
    ISAAC_VITA_EXCEPTION_NEXT_IAT_RVA,
    ISAAC_VITA_EXCEPTION_NEXT_IAT_VA,
    ISAAC_VITA_EXCEPTION_NEXT_CALL_RVA,
    ISAAC_VITA_EXCEPTION_NEXT_RETURN_RVA,
    ISAAC_VITA_EXCEPTION_NEXT_ARGUMENT,
    ISAAC_VITA_EXCEPTION_NEXT_ORDINAL
};

typedef struct jump_family_evidence {
    const char *setjmp_name;
    uint32_t setjmp_iat_rva;
    uint32_t setjmp_iat_va;
    uint32_t setjmp_thunk_rva;
    uint32_t setjmp_calls[2];
    uint32_t setjmp_returns[2];
    const char *longjmp_name;
    uint32_t longjmp_iat_rva;
    uint32_t longjmp_iat_va;
    uint32_t longjmp_calls[ISAAC_VITA_EXCEPTION_LONGJMP_CALL_COUNT];
    uint32_t longjmp_returns[ISAAC_VITA_EXCEPTION_LONGJMP_CALL_COUNT];
} jump_family_evidence;

static const jump_family_evidence s_jump_family = {
    ISAAC_VITA_EXCEPTION_SETJMP3_NAME,
    ISAAC_VITA_EXCEPTION_SETJMP3_IAT_RVA,
    ISAAC_VITA_EXCEPTION_SETJMP3_IAT_VA,
    ISAAC_VITA_EXCEPTION_SETJMP3_THUNK_RVA,
    {
        ISAAC_VITA_EXCEPTION_SETJMP3_CALL0_RVA,
        ISAAC_VITA_EXCEPTION_SETJMP3_CALL1_RVA
    },
    {
        ISAAC_VITA_EXCEPTION_SETJMP3_RETURN0_RVA,
        ISAAC_VITA_EXCEPTION_SETJMP3_RETURN1_RVA
    },
    ISAAC_VITA_EXCEPTION_LONGJMP_NAME,
    ISAAC_VITA_EXCEPTION_LONGJMP_IAT_RVA,
    ISAAC_VITA_EXCEPTION_LONGJMP_IAT_VA,
    {
        ISAAC_VITA_EXCEPTION_LONGJMP_CALL0_RVA,
        ISAAC_VITA_EXCEPTION_LONGJMP_CALL1_RVA,
        ISAAC_VITA_EXCEPTION_LONGJMP_CALL2_RVA,
        ISAAC_VITA_EXCEPTION_LONGJMP_CALL3_RVA
    },
    {
        ISAAC_VITA_EXCEPTION_LONGJMP_RETURN0_RVA,
        ISAAC_VITA_EXCEPTION_LONGJMP_RETURN1_RVA,
        ISAAC_VITA_EXCEPTION_LONGJMP_RETURN2_RVA,
        ISAAC_VITA_EXCEPTION_LONGJMP_RETURN3_RVA
    }
};

_Static_assert(GUEST_IMAGE_BASE +
               ISAAC_VITA_EXCEPTION_SET_FILTER_IAT_RVA ==
               ISAAC_VITA_EXCEPTION_SET_FILTER_IAT_VA,
               "SetUnhandledExceptionFilter IAT VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_EXCEPTION_NEXT_IAT_RVA ==
               ISAAC_VITA_EXCEPTION_NEXT_IAT_VA,
               "_set_new_mode IAT VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_EXCEPTION_SETJMP3_IAT_RVA ==
               ISAAC_VITA_EXCEPTION_SETJMP3_IAT_VA,
               "_setjmp3 IAT VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_EXCEPTION_LONGJMP_IAT_RVA ==
               ISAAC_VITA_EXCEPTION_LONGJMP_IAT_VA,
               "longjmp IAT VA/base drifted");
_Static_assert(ISAAC_VITA_EXCEPTION_SETJMP3_CALL0_RVA + 5U ==
               ISAAC_VITA_EXCEPTION_SETJMP3_RETURN0_RVA &&
               ISAAC_VITA_EXCEPTION_SETJMP3_CALL1_RVA + 5U ==
               ISAAC_VITA_EXCEPTION_SETJMP3_RETURN1_RVA,
               "_setjmp3 owner call widths drifted");
_Static_assert(ISAAC_VITA_EXCEPTION_LONGJMP_CALL0_RVA + 6U ==
               ISAAC_VITA_EXCEPTION_LONGJMP_RETURN0_RVA &&
               ISAAC_VITA_EXCEPTION_LONGJMP_CALL1_RVA + 6U ==
               ISAAC_VITA_EXCEPTION_LONGJMP_RETURN1_RVA &&
               ISAAC_VITA_EXCEPTION_LONGJMP_CALL2_RVA + 6U ==
               ISAAC_VITA_EXCEPTION_LONGJMP_RETURN2_RVA &&
               ISAAC_VITA_EXCEPTION_LONGJMP_CALL3_RVA + 6U ==
               ISAAC_VITA_EXCEPTION_LONGJMP_RETURN3_RVA,
               "longjmp absolute-IAT call widths drifted");
_Static_assert(ISAAC_VITA_EXCEPTION_SET_FILTER_ORDINAL + 1U ==
               ISAAC_VITA_EXCEPTION_NEXT_ORDINAL,
               "measured boot ordinals are no longer consecutive");
_Static_assert(ISAAC_VITA_EXCEPTION_IMPORT_COUNT == 2U &&
               ISAAC_VITA_EXCEPTION_BOOT_CALL_COUNT == 1U &&
               ISAAC_VITA_EXCEPTION_STATE_SLOT_COUNT == 1U,
               "exception family/state census drifted");

_Alignas(8) static uint32_t s_frame[12];
static CPU s_jump_cpu;
static jmp_buf s_fault_jump;
static unsigned s_jump_count;
static int s_fault_must_jump;
static int s_checked_read_must_fault;
static CPU *s_checked_cpu;
static jmp_buf s_longjmp_jump;
static CPU *s_longjmp_cpu;
static uint32_t s_longjmp_env;
static int32_t s_longjmp_value;
static unsigned s_longjmp_calls;

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint32_t prepare_call(CPU *c, uint32_t callback)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[4]);
    s_frame[4] = 0x0badc0deU;
    s_frame[5] = callback;
    c->esp = esp;
    return esp;
}

static uint32_t prepare_longjmp_call(CPU *c, uint32_t guest_env,
                                     int32_t value)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[4]);
    s_frame[4] = ISAAC_VITA_EXCEPTION_LONGJMP_RETURN1_RVA;
    s_frame[5] = guest_env;
    s_frame[6] = (uint32_t)value;
    c->esp = esp;
    return esp;
}

GUEST_NORETURN void guest_longjmp(CPU *__restrict c, uint32_t guest_env,
                                  int32_t value)
{
    s_longjmp_cpu = c;
    s_longjmp_env = guest_env;
    s_longjmp_value = value;
    ++s_longjmp_calls;
    longjmp(s_longjmp_jump, 1);
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
    if (s_fault_must_jump)
        longjmp(s_fault_jump, 1);
}

void guest_check(uint32_t address, uint32_t size, int write)
{
    (void)size;
    if (s_checked_read_must_fault && !write) {
        s_checked_read_must_fault = 0;
        guest_fault(s_checked_cpu, address,
                    "oracle checked exception-filter argument fault");
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

static uint64_t hash_evidence_record(uint64_t hash,
                                     const exception_call_evidence *record)
{
    hash = hash_text(hash, record->name);
    hash = hash_u32(hash, record->iat_rva);
    hash = hash_u32(hash, record->iat_va);
    hash = hash_u32(hash, record->call_rva);
    hash = hash_u32(hash, record->return_rva);
    hash = hash_u32(hash, record->argument);
    return hash_u32(hash, record->ordinal);
}

static uint64_t hash_jump_family(uint64_t hash,
                                 const jump_family_evidence *record)
{
    unsigned i;

    hash = hash_text(hash, record->setjmp_name);
    hash = hash_u32(hash, record->setjmp_iat_rva);
    hash = hash_u32(hash, record->setjmp_iat_va);
    hash = hash_u32(hash, record->setjmp_thunk_rva);
    for (i = 0U; i < 2U; ++i) {
        hash = hash_u32(hash, record->setjmp_calls[i]);
        hash = hash_u32(hash, record->setjmp_returns[i]);
    }
    hash = hash_text(hash, record->longjmp_name);
    hash = hash_u32(hash, record->longjmp_iat_rva);
    hash = hash_u32(hash, record->longjmp_iat_va);
    for (i = 0U; i < ISAAC_VITA_EXCEPTION_LONGJMP_CALL_COUNT; ++i) {
        hash = hash_u32(hash, record->longjmp_calls[i]);
        hash = hash_u32(hash, record->longjmp_returns[i]);
    }
    return hash;
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);

    hash = hash_evidence_record(hash, &s_current);
    hash = hash_evidence_record(hash, &s_next);
    hash = hash_jump_family(hash, &s_jump_family);
    hash = hash_u32(hash, ISAAC_VITA_EXCEPTION_IMPORT_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_EXCEPTION_BOOT_CALL_COUNT);
    return hash_u32(hash, ISAAC_VITA_EXCEPTION_STATE_SLOT_COUNT);
}

static int counted_call(CPU *c, unsigned *calls, uint32_t callback,
                        uint32_t expected_previous)
{
    uint32_t esp = prepare_call(c, callback);
    unsigned expected_count = *calls + 1U;

    if (!isaac_vita_exception_import_counted(
            c, ISAAC_VITA_EXCEPTION_SET_FILTER_NAME, calls))
        return 0;
    return !c->fault && *calls == expected_count &&
           c->esp == esp + 8U && c->eax == expected_previous;
}

static int counted_longjmp(CPU *c, unsigned *calls, uint32_t guest_env,
                           int32_t value)
{
    CPU snapshot;
    uint32_t frame_snapshot[12];
    uint32_t esp = prepare_longjmp_call(c, guest_env, value);
    unsigned expected_count = *calls + 1U;
    unsigned expected_jumps = s_longjmp_calls + 1U;

    c->eax = 0x13579bdfU;
    memcpy(&snapshot, c, sizeof snapshot);
    memcpy(frame_snapshot, s_frame, sizeof frame_snapshot);
    s_longjmp_cpu = NULL;
    s_longjmp_env = 0U;
    s_longjmp_value = 0;
    if (setjmp(s_longjmp_jump) == 0) {
        (void)isaac_vita_exception_import_counted(
            c, ISAAC_VITA_EXCEPTION_LONGJMP_NAME, calls);
        return 0;
    }
    return *calls == expected_count && s_longjmp_calls == expected_jumps &&
           s_longjmp_cpu == c && s_longjmp_env == guest_env &&
           s_longjmp_value == value && c->esp == esp &&
           memcmp(&snapshot, c, sizeof snapshot) == 0 &&
           memcmp(frame_snapshot, s_frame, sizeof frame_snapshot) == 0;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    uint32_t esp;
    uint32_t previous;
    uint32_t replacement;
    unsigned calls = 0U;
    unsigned i;

    if (!pointer_fits(s_frame))
        return 1;
    if (evidence_hash() != UINT64_C(0x49228775bb8398ab))
        return 2;
    if (strcmp(s_current.name,
               "KERNEL32.dll!SetUnhandledExceptionFilter") != 0 ||
        s_current.iat_rva != 0x0060603cU ||
        s_current.iat_va != 0x9860603cU ||
        s_current.call_rva != 0x005ebf7dU ||
        s_current.return_rva != 0x005ebf83U ||
        s_current.argument != 0x985ebf84U || s_current.ordinal != 68U ||
        strcmp(s_next.name,
               "api-ms-win-crt-heap-l1-1-0.dll!_set_new_mode") != 0 ||
        s_next.iat_rva != 0x00606508U ||
        s_next.call_rva != 0x005eb6b5U ||
        s_next.return_rva != 0x005eb6baU ||
        s_next.argument != 0U || s_next.ordinal != 69U ||
        strcmp(s_jump_family.setjmp_name,
               "VCRUNTIME140.dll!_setjmp3") != 0 ||
        s_jump_family.setjmp_iat_rva != 0x0060646cU ||
        s_jump_family.setjmp_iat_va != 0x9860646cU ||
        s_jump_family.setjmp_thunk_rva != 0x005ec352U ||
        s_jump_family.setjmp_calls[0] != 0x005a0f52U ||
        s_jump_family.setjmp_returns[0] != 0x005a0f57U ||
        s_jump_family.setjmp_calls[1] != 0x005b1094U ||
        s_jump_family.setjmp_returns[1] != 0x005b1099U ||
        strcmp(s_jump_family.longjmp_name,
               "VCRUNTIME140.dll!longjmp") != 0 ||
        s_jump_family.longjmp_iat_rva != 0x0060648cU ||
        s_jump_family.longjmp_iat_va != 0x9860648cU ||
        s_jump_family.longjmp_calls[0] != 0x005a1100U ||
        s_jump_family.longjmp_returns[0] != 0x005a1106U ||
        s_jump_family.longjmp_calls[1] != 0x005a12d8U ||
        s_jump_family.longjmp_returns[1] != 0x005a12deU ||
        s_jump_family.longjmp_calls[2] != 0x005a1552U ||
        s_jump_family.longjmp_returns[2] != 0x005a1558U ||
        s_jump_family.longjmp_calls[3] != 0x005c3570U ||
        s_jump_family.longjmp_returns[3] != 0x005c3576U)
        return 3;

    /* Initial NULL, replacement, and clearing expose Windows' previous-filter
     * return contract and exact one-argument stdcall cleanup. */
    if (!counted_call(&cpu, &calls,
                      ISAAC_VITA_EXCEPTION_SET_FILTER_CALLBACK_VA, 0U) ||
        !counted_call(&cpu, &calls, 0x81234567U,
                      ISAAC_VITA_EXCEPTION_SET_FILTER_CALLBACK_VA) ||
        !counted_call(&cpu, &calls, 0U, 0x81234567U))
        return 4;

    /* NULL, a sibling KERNEL32 import and wrong case are mutation-free
     * dispatcher handoffs.  The next accepted call proves state too. */
    esp = prepare_call(&cpu, 0x87654321U);
    cpu.eax = 0x2468ace0U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_exception_import_counted(&cpu, NULL, &calls) != 0 ||
        calls != 3U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_exception_import_counted(
            &cpu, "KERNEL32.dll!UnhandledExceptionFilter", &calls) != 0 ||
        calls != 3U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_exception_import_counted(
            &cpu, "kernel32.dll!SetUnhandledExceptionFilter", &calls) != 0 ||
        calls != 3U || cpu.esp != esp ||
        memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_exception_import_counted(
            &cpu, ISAAC_VITA_EXCEPTION_SETJMP3_NAME, &calls) != 0 ||
        calls != 3U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_exception_import_counted(
            &cpu, "VCRUNTIME140.dll!Longjmp", &calls) != 0 ||
        calls != 3U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        !counted_call(&cpu, &calls, 0x11112222U, 0U))
        return 5;

    /* Repeated replacement exercises the entire 32-bit state channel while
     * retaining exactly one bounded process-global slot. */
    previous = 0x11112222U;
    for (i = 0U; i < 257U; ++i) {
        replacement = 0x80000000U ^ (i * 0x01010101U);
        if (!counted_call(&cpu, &calls, replacement, previous))
            return 6;
        previous = replacement;
    }

    /* The dispatcher commits the dynamic count before a checked stack read
     * takes production's nonlocal guest_fault path.  State stays unchanged. */
    (void)prepare_call(&s_jump_cpu, 0xdeadbeefU);
    s_jump_cpu.eax = 0x13579bdfU;
    s_jump_count = calls;
    s_checked_cpu = &s_jump_cpu;
    s_checked_read_must_fault = 1;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_exception_import_counted(
            &s_jump_cpu, ISAAC_VITA_EXCEPTION_SET_FILTER_NAME,
            &s_jump_count);
        return 7;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != calls + 1U ||
        s_jump_cpu.eax != 0x13579bdfU || !s_jump_cpu.fault ||
        strcmp(s_jump_cpu.fault,
               "oracle checked exception-filter argument fault") != 0 ||
        !counted_call(&cpu, &calls, 0U, previous))
        return 8;

    /* Exercise the uncounted production wrapper without changing CALLS. */
    esp = prepare_call(&cpu, ISAAC_VITA_EXCEPTION_SET_FILTER_CALLBACK_VA);
    if (!isaac_vita_exception_import(
            &cpu, ISAAC_VITA_EXCEPTION_SET_FILTER_NAME) ||
        cpu.fault || cpu.esp != esp + 8U || cpu.eax != 0U || calls != 262U)
        return 9;
    esp = prepare_call(&cpu, 0U);
    if (!isaac_vita_exception_import(
            &cpu, ISAAC_VITA_EXCEPTION_SET_FILTER_NAME) ||
        cpu.fault || cpu.esp != esp + 8U ||
        cpu.eax != ISAAC_VITA_EXCEPTION_SET_FILTER_CALLBACK_VA ||
        calls != 262U)
        return 10;

    /* The Vita dispatcher supplies only the imported half of the family.
     * It forwards the two raw cdecl arguments, increments before transfer,
     * and performs no return-address/argument cleanup.  The shared runtime's
     * guest_longjmp owns value-zero normalization and CPU restoration. */
    if (!counted_longjmp(&cpu, &calls, 0x85ec95b0U, 1) ||
        !counted_longjmp(&cpu, &calls, 0x81234567U, 0) ||
        !counted_longjmp(&cpu, &calls, 0x82345678U, -7) || calls != 265U)
        return 11;

    /* The uncounted production wrapper reaches the same nonlocal seam while
     * leaving the external denominator untouched. */
    esp = prepare_longjmp_call(&cpu, 0x83456789U, 42);
    cpu.eax = 0x2468ace0U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    s_longjmp_cpu = NULL;
    s_longjmp_env = 0U;
    s_longjmp_value = 0;
    i = s_longjmp_calls + 1U;
    if (setjmp(s_longjmp_jump) == 0) {
        (void)isaac_vita_exception_import(
            &cpu, ISAAC_VITA_EXCEPTION_LONGJMP_NAME);
        return 12;
    }
    if (calls != 265U || s_longjmp_calls != i || s_longjmp_cpu != &cpu ||
        s_longjmp_env != 0x83456789U || s_longjmp_value != 42 ||
        cpu.esp != esp || memcmp(&snapshot, &cpu, sizeof cpu) != 0)
        return 13;

    return 0;
}
