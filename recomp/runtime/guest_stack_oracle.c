/* Exhaustive behavioural oracle for the CPU-owned synthetic guest stack.
 *
 * This executes as 32-bit host code.  The matching Vita gate compiles and
 * links the same source as ARMv7 Thumb softfp but deliberately does not run
 * it: target execution belongs to Vita3K/hardware qualification, not CI.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

#if defined(_MSC_VER)
#define ORACLE_NOINLINE __declspec(noinline)
#else
#define ORACLE_NOINLINE __attribute__((noinline))
#endif

enum {
    PC_UNBOUND       = 0x00100010U,
    PC_OWNER         = 0x00100020U,
    PC_INVALID_ESP   = 0x00100030U,
    PC_PUSH          = 0x00100040U,
    PC_POP           = 0x00100050U,
    PC_SET           = 0x00100060U,
    PC_ACCESS        = 0x00100070U,
    PC_ADJUST        = 0x00100080U,
    PC_CALL          = 0x00100090U,
    PC_PROLOGUE      = 0x001000a0U,
    PC_RETURN        = 0x001000b0U,
    PC_VALID         = 0x001000c0U
};

#define STACK_WORDS 18U
#define STACK_FLOOR_INDEX 1U
#define STACK_CEILING_INDEX 17U
#define CANARY_LOW  UINT32_C(0x51acc001)
#define CANARY_HIGH UINT32_C(0x51acc002)

static uint32_t s_stack[STACK_WORDS];
static uint32_t s_external_word;
static uint32_t s_target;
static uint32_t s_size;
static uint32_t s_amount;

/* guest.c's dispatcher owns these platform seams.  This oracle never reaches
 * them; rejecting definitions keep the linked runtime contract complete. */
int guest_host_import(CPU *__restrict c, const char *name)
{
    (void)c;
    (void)name;
    return 0;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static uint32_t floor_address(void)
{
    return pointer32(&s_stack[STACK_FLOOR_INDEX]);
}

static uint32_t ceiling_address(void)
{
    return pointer32(&s_stack[STACK_CEILING_INDEX]);
}

static void reset_stack_bytes(void)
{
    uint32_t i;
    for (i = 0U; i < STACK_WORDS; ++i)
        s_stack[i] = UINT32_C(0x600d0000) + i;
    s_stack[0] = CANARY_LOW;
    s_stack[STACK_CEILING_INDEX] = CANARY_HIGH;
}

static int reset_bound_cpu(CPU *c)
{
    guest_cpu_init(c);
    reset_stack_bytes();
    return guest_stack_bind(c, floor_address(), ceiling_address());
}

static int canaries_ok(void)
{
    return s_stack[0] == CANARY_LOW &&
           s_stack[STACK_CEILING_INDEX] == CANARY_HIGH;
}

static void action_push(CPU *__restrict c)
{
    gpush_at(c, UINT32_C(0x12345678), s_amount);
}

static void action_pop(CPU *__restrict c)
{
    (void)gpop_at(c, s_amount);
}

static void action_set(CPU *__restrict c)
{
    (void)guest_stack_set(c, s_target, s_amount);
}

static void action_access(CPU *__restrict c)
{
    (void)guest_stack_address(c, s_target, s_size, s_amount);
}

static void action_adjust(CPU *__restrict c)
{
    (void)guest_stack_adjust(c, s_size, s_amount);
}

static void action_ret_imm(CPU *__restrict c)
{
    (void)gpop_at(c, PC_RETURN);
    (void)guest_stack_adjust(c, 4U, PC_RETURN);
}

static ORACLE_NOINLINE void action_generated_push(CPU *__restrict c)
{
    gpush_generated(c, UINT32_C(0x87654321));
    GUEST_STACK_CALLSITE_BARRIER();
}

static ORACLE_NOINLINE void action_generated_pop(CPU *__restrict c)
{
    (void)gpop_generated(c);
    GUEST_STACK_CALLSITE_BARRIER();
}

static ORACLE_NOINLINE void action_generated_set(CPU *__restrict c)
{
    (void)guest_stack_set_generated(c, s_target);
    GUEST_STACK_CALLSITE_BARRIER();
}

static ORACLE_NOINLINE void action_generated_adjust(CPU *__restrict c)
{
    (void)guest_stack_adjust_generated(c, s_amount);
    GUEST_STACK_CALLSITE_BARRIER();
}

static ORACLE_NOINLINE void action_generated_access(CPU *__restrict c)
{
    (void)guest_stack_address_generated(c, s_target, s_size);
    GUEST_STACK_CALLSITE_BARRIER();
}

static int expect_fault(CPU *c, guest_fn action, uint32_t kind,
                        uint32_t pc, uint32_t address, uint32_t size,
                        const uint32_t before[STACK_WORDS],
                        const char *name)
{
    int stopped = guest_run_until_stop(c, action);
    if (stopped != GUEST_RUN_FAULT || !c->fault ||
        c->stack_fault_kind != kind || c->stack_fault_pc != pc ||
        c->stack_fault_native_site != (uintptr_t)0U ||
        c->stack_fault_address != address || c->stack_fault_size != size ||
        (before && memcmp(before, s_stack, sizeof s_stack) != 0) ||
        !canaries_ok()) {
        fprintf(stderr,
                "guest stack oracle: %s FAIL stop=%d fault=%s "
                "kind=%u pc=%08x address=%08x size=%u\n",
                name, stopped, c->fault ? c->fault : "<none>",
                (unsigned)c->stack_fault_kind,
                (unsigned)c->stack_fault_pc,
                (unsigned)c->stack_fault_address,
                (unsigned)c->stack_fault_size);
        return 0;
    }
    return 1;
}

static int expect_native_fault(CPU *c, guest_fn action, uint32_t kind,
                               uint32_t address, uint32_t size,
                               const uint32_t before[STACK_WORDS],
                               const char *name)
{
    uintptr_t owner, site;
    int stopped = guest_run_until_stop(c, action);
    if (stopped != GUEST_RUN_FAULT || !c->fault ||
            c->stack_fault_kind != kind || c->stack_fault_pc != 0U ||
            c->stack_fault_address != address ||
            c->stack_fault_size != size ||
            memcmp(before, s_stack, sizeof s_stack) != 0 ||
            !canaries_ok()) {
        fprintf(stderr,
                "guest stack oracle: generated-%s fault mismatch "
                "stop=%d kind=%u address=%08x size=%u\n",
                name, stopped, (unsigned)c->stack_fault_kind,
                (unsigned)c->stack_fault_address,
                (unsigned)c->stack_fault_size);
        return 0;
    }
    owner = (uintptr_t)action;
    site = c->stack_fault_native_site;
    /* Each action is one direct call plus its return.  A site in this compact
     * owner range proves the helper captured its caller, not its own cold
     * diagnostic callsite.  ARM disassembly supplies the exact BL offset. */
    if (site <= owner || site - owner >= (uintptr_t)512U) {
        fprintf(stderr,
                "guest stack oracle: generated-%s attribution outside owner "
                "owner=%p site=%p\n", name, (void *)owner, (void *)site);
        return 0;
    }
    return 1;
}

static int test_generated_contract(void)
{
    CPU c;
    uint32_t before[STACK_WORDS];
    uint32_t floor = floor_address(), ceiling = ceiling_address();

#define GENERATED_RESET() do {                                               \
        if (reset_bound_cpu(&c) != 0) return 0;                              \
        memcpy(before, s_stack, sizeof before);                              \
    } while (0)
#define GENERATED_EXPECT(action, kind, address, size, name) do {             \
        if (!expect_native_fault(&c, action, kind, address, size, before,     \
                                 name)) return 0;                            \
        guest_stack_free(&c);                                                \
    } while (0)

    guest_cpu_init(&c);
    reset_stack_bytes();
    c.esp = ceiling;
    memcpy(before, s_stack, sizeof before);
    GENERATED_EXPECT(action_generated_push, GUEST_STACK_FAULT_UNBOUND,
                     ceiling, 0U, "unbound");

    GENERATED_RESET();
    c.stack_floor = c.stack_ceiling;
    GENERATED_EXPECT(action_generated_push, GUEST_STACK_FAULT_OWNER,
                     ceiling, 0U, "corrupt-owner");

    GENERATED_RESET();
    s_target = floor - 1U;
    GENERATED_EXPECT(action_generated_set, GUEST_STACK_FAULT_SET,
                     floor - 1U, 0U, "set");

    GENERATED_RESET();
    c.esp = ceiling - 4U;
    s_amount = 8U;
    GENERATED_EXPECT(action_generated_adjust, GUEST_STACK_FAULT_ADJUST,
                     ceiling - 4U, 8U, "adjust");

    GENERATED_RESET();
    s_target = floor - 1U;
    s_size = 1U;
    GENERATED_EXPECT(action_generated_access, GUEST_STACK_FAULT_ACCESS,
                     floor - 1U, 1U, "access");

    GENERATED_RESET();
    c.esp = floor;
    GENERATED_EXPECT(action_generated_push, GUEST_STACK_FAULT_PUSH,
                     floor - 4U, 4U, "push");

    GENERATED_RESET();
    c.esp = ceiling;
    GENERATED_EXPECT(action_generated_pop, GUEST_STACK_FAULT_POP,
                     ceiling, 4U, "pop");

    if (reset_bound_cpu(&c) != 0 ||
            !guest_stack_set_generated(&c, ceiling - 16U) ||
            guest_stack_address_generated(&c, ceiling - 20U, 4U) !=
                ceiling - 20U)
        return 0;
    gpush_generated(&c, UINT32_C(0xabcdef01));
    if (gpop_generated(&c) != UINT32_C(0xabcdef01) ||
            !guest_stack_adjust_generated(&c, 16U) || c.esp != ceiling ||
            guest_stack_high_water(&c) != 20U || !canaries_ok())
        return 0;
    guest_stack_free(&c);

#undef GENERATED_EXPECT
#undef GENERATED_RESET
    return 1;
}

static int test_unbound(void)
{
    CPU c;
    uint32_t before[STACK_WORDS];
    guest_cpu_init(&c);
    reset_stack_bytes();
    c.esp = ceiling_address();
    memcpy(before, s_stack, sizeof before);
    s_amount = PC_UNBOUND;
    return expect_fault(&c, action_push, GUEST_STACK_FAULT_UNBOUND,
                        PC_UNBOUND, c.esp, 0U, before, "unbound");
}

static int test_copied_owner(void)
{
    CPU owner, copy;
    uint32_t before[STACK_WORDS];
    int ok;
    if (reset_bound_cpu(&owner) != 0)
        return 0;
    copy = owner;
    memcpy(before, s_stack, sizeof before);
    s_amount = PC_OWNER;
    ok = expect_fault(&copy, action_push, GUEST_STACK_FAULT_OWNER,
                      PC_OWNER, copy.esp, 0U, before, "copied-owner");
    guest_stack_free(&owner);
    return ok;
}

static int test_fault_boundaries(void)
{
    CPU c;
    uint32_t before[STACK_WORDS];
    uint32_t floor = floor_address(), ceiling = ceiling_address();

#define RESET_SNAPSHOT() do {                                                \
        if (reset_bound_cpu(&c) != 0) return 0;                              \
        memcpy(before, s_stack, sizeof before);                              \
    } while (0)
#define EXPECT(action, kind, pc, address, size, name) do {                    \
        if (!expect_fault(&c, action, kind, pc, address, size, before, name)) \
            return 0;                                                        \
        guest_stack_free(&c);                                                \
    } while (0)

    RESET_SNAPSHOT();
    c.esp = UINT32_MAX;
    s_target = c.esp; s_size = 4U; s_amount = PC_INVALID_ESP;
    EXPECT(action_access, GUEST_STACK_FAULT_ACCESS, PC_INVALID_ESP,
           UINT32_MAX, 4U, "invalid-esp");

    RESET_SNAPSHOT();
    c.esp = floor; s_amount = PC_PUSH;
    EXPECT(action_push, GUEST_STACK_FAULT_PUSH, PC_PUSH,
           floor - 4U, 4U, "push-underflow");

    RESET_SNAPSHOT();
    c.esp = ceiling; s_amount = PC_POP;
    EXPECT(action_pop, GUEST_STACK_FAULT_POP, PC_POP,
           ceiling, 4U, "pop-overflow");

    RESET_SNAPSHOT();
    s_target = floor - 1U; s_amount = PC_SET;
    EXPECT(action_set, GUEST_STACK_FAULT_SET, PC_SET,
           floor - 1U, 0U, "set-underflow");

    RESET_SNAPSHOT();
    s_target = ceiling + 1U; s_amount = PC_SET;
    EXPECT(action_set, GUEST_STACK_FAULT_SET, PC_SET,
           ceiling + 1U, 0U, "set-overflow");

    RESET_SNAPSHOT();
    s_target = floor - 1U; s_size = 1U; s_amount = PC_ACCESS;
    EXPECT(action_access, GUEST_STACK_FAULT_ACCESS, PC_ACCESS,
           floor - 1U, 1U, "access-underflow");

    RESET_SNAPSHOT();
    s_target = ceiling - 2U; s_size = 4U; s_amount = PC_ACCESS;
    EXPECT(action_access, GUEST_STACK_FAULT_ACCESS, PC_ACCESS,
           ceiling - 2U, 4U, "access-overflow");

    RESET_SNAPSHOT();
    s_target = floor; s_size = 0U; s_amount = PC_ACCESS;
    EXPECT(action_access, GUEST_STACK_FAULT_ACCESS, PC_ACCESS,
           floor, 0U, "zero-size-access");

    RESET_SNAPSHOT();
    c.esp = ceiling - 4U; s_size = 8U; s_amount = PC_ADJUST;
    EXPECT(action_adjust, GUEST_STACK_FAULT_ADJUST, PC_ADJUST,
           ceiling - 4U, 8U, "cleanup-overflow");

    RESET_SNAPSHOT();
    c.esp = floor; s_amount = PC_CALL;
    EXPECT(action_push, GUEST_STACK_FAULT_PUSH, PC_CALL,
           floor - 4U, 4U, "call-underflow");

    RESET_SNAPSHOT();
    s_target = floor - 4U; s_amount = PC_PROLOGUE;
    EXPECT(action_set, GUEST_STACK_FAULT_SET, PC_PROLOGUE,
           floor - 4U, 0U, "prologue-underflow");

    if (reset_bound_cpu(&c) != 0)
        return 0;
    if (!guest_stack_set(&c, ceiling - 4U, PC_RETURN))
        return 0;
    st32(c.esp, UINT32_C(0xcafebabe));
    memcpy(before, s_stack, sizeof before);
    if (!expect_fault(&c, action_ret_imm, GUEST_STACK_FAULT_ADJUST,
                      PC_RETURN, ceiling, 4U, before,
                      "ret-imm-overflow") || c.esp != ceiling)
        return 0;
    guest_stack_free(&c);

#undef EXPECT
#undef RESET_SNAPSHOT
    return 1;
}

static int test_valid_boundaries(void)
{
    CPU c;
    uint32_t floor = floor_address(), ceiling = ceiling_address();
    uint32_t value;

    if (reset_bound_cpu(&c) != 0)
        return 0;
    if (guest_stack_capacity(&c) != ceiling - floor ||
        guest_stack_high_water(&c) != 0U ||
        !guest_stack_contains(&c, floor, ceiling - floor) ||
        !guest_stack_contains(&c, ceiling - 1U, 1U) ||
        guest_stack_contains(&c, ceiling, 1U))
        return 0;

    /* Generic guest pointers remain generic: this native oracle word is not
     * part of the synthetic stack and must not be classified as one. */
    s_external_word = UINT32_C(0x0ddba11);
    if (ld32(pointer32(&s_external_word)) != s_external_word)
        return 0;

    gpush_at(&c, UINT32_C(0xaabbccdd), PC_CALL);
    if (c.esp != ceiling - 4U || ld32(c.esp) != UINT32_C(0xaabbccdd))
        return 0;
    value = gpop_at(&c, PC_RETURN);
    if (value != UINT32_C(0xaabbccdd) || c.esp != ceiling)
        return 0;

    if (!guest_stack_set(&c, ceiling - 32U, PC_PROLOGUE) ||
        guest_stack_high_water(&c) != 32U ||
        guest_stack_address(&c, ceiling - 40U, 8U, PC_VALID) !=
            ceiling - 40U ||
        guest_stack_high_water(&c) != 40U ||
        !guest_stack_adjust(&c, 32U, PC_RETURN) || c.esp != ceiling ||
        !canaries_ok() || c.stack_fault_kind != GUEST_STACK_FAULT_NONE)
        return 0;

    if (!guest_stack_set(&c, floor + 4U, PC_VALID))
        return 0;
    gpush_at(&c, UINT32_C(0x11223344), PC_VALID);
    if (c.esp != floor || gpop_at(&c, PC_VALID) != UINT32_C(0x11223344) ||
        c.esp != floor + 4U || guest_stack_high_water(&c) != ceiling - floor)
        return 0;

    guest_stack_free(&c);
    return 1;
}

int main(void)
{
    if ((uintptr_t)s_stack > UINT32_MAX ||
        (uintptr_t)&s_stack[STACK_WORDS] > UINT32_MAX ||
        (uintptr_t)&s_external_word > UINT32_MAX) {
        fputs("guest stack oracle: requires a 32-bit process\n", stderr);
        return 2;
    }
    if (!test_unbound() || !test_copied_owner() ||
        !test_generated_contract() ||
        !test_fault_boundaries() || !test_valid_boundaries()) {
        fputs("guest stack oracle: FAIL\n", stderr);
        return 1;
    }
    puts("guest stack oracle: PASS (owner/floor/ceiling/high-water/native-LR; "
         "invalid ESP/push/pop/call/prologue/ret cleanup; generic pointers)");
    return 0;
}
