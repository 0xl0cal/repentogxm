/* Runnable ARM softfp oracle for the Vita UCRT heap boundary. */
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "host_vita_heap.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita heap oracle requires a 32-bit identity-mapped host
#endif

_Alignas(8) static uint32_t s_frame[12];
_Alignas(8) static uint8_t s_foreign[32];
static CPU s_jump_cpu;
static jmp_buf s_fault_jump;
static unsigned s_jump_count;
static int s_fault_must_jump;
static void *s_churn[96];

static uint32_t prepare_call(CPU *c, uint32_t a0, uint32_t a1)
{
    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[3] = 0x0badc0deU;
    s_frame[4] = a0;
    s_frame[5] = a1;
    c->esp = (uint32_t)(uintptr_t)&s_frame[3];
    return c->esp;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
    if (s_fault_must_jump)
        longjmp(s_fault_jump, 1);
}

/* This focused oracle links the header's guarded stack helpers without the
 * full runtime.  Match guest.c's fail-closed return shape for cold violations. */
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind;
    (void)address;
    (void)size;
    guest_fault(c, pc, "heap oracle guest stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    unsigned calls = 0U;
    uint32_t esp;
    uint32_t p;
    uint32_t q;
    uint32_t i;
    uint32_t wave;
    size_t stable_capacity;
    uintptr_t allocation_base;
    uintptr_t raw;
    uintptr_t begin;
    uint32_t lease;
    uint32_t next_lease;
    uint32_t leases[8];
    int valid_owner;

    if ((uintptr_t)s_frame > UINT32_MAX)
        return 1;

    esp = prepare_call(&cpu, 1U, 2U);
    cpu.eax = 0x12345678U;
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_heap_import_counted(
            &cpu, "api-ms-win-crt-heap-l1-1-0.dll!unknown", &calls) ||
        calls || memcmp(&cpu, &snapshot, sizeof cpu) || cpu.esp != esp)
        return 2;

    /* Startup sets new-mode to zero.  Also pin stateful previous-value and
     * mutation-free rejection semantics. */
    esp = prepare_call(&cpu, 0U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_SET_NEW_MODE_NAME, &calls) ||
        calls != 1U || cpu.fault || cpu.eax != 0U || cpu.esp != esp + 4U)
        return 3;
    esp = prepare_call(&cpu, 1U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_SET_NEW_MODE_NAME, &calls) ||
        calls != 2U || cpu.fault || cpu.eax != 0U || cpu.esp != esp + 4U)
        return 4;
    esp = prepare_call(&cpu, 0U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_SET_NEW_MODE_NAME, &calls) ||
        calls != 3U || cpu.fault || cpu.eax != 1U || cpu.esp != esp + 4U)
        return 5;

    /* All allocated objects come from one native allocator and remain direct
     * 32-bit guest pointers across malloc/realloc/calloc/free. */
    esp = prepare_call(&cpu, 32U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_MALLOC_NAME, &calls) ||
        calls != 4U || cpu.fault || !cpu.eax || cpu.esp != esp + 4U)
        return 6;
    p = cpu.eax;
    if (!isaac_vita_guest_heap_owns((void *)(uintptr_t)p))
        return 7;
    for (i = 0U; i < 32U; ++i)
        ((uint8_t *)(uintptr_t)p)[i] = (uint8_t)(i ^ 0x5aU);

    esp = prepare_call(&cpu, p, 64U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_REALLOC_NAME, &calls) ||
        calls != 5U || cpu.fault || !cpu.eax || cpu.esp != esp + 4U)
        return 8;
    p = cpu.eax;
    if (!isaac_vita_guest_heap_owns((void *)(uintptr_t)p))
        return 9;
    for (i = 0U; i < 32U; ++i) {
        if (((uint8_t *)(uintptr_t)p)[i] != (uint8_t)(i ^ 0x5aU))
            return 10;
    }

    esp = prepare_call(&cpu, 8U, 4U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_CALLOC_NAME, &calls) ||
        calls != 6U || cpu.fault || !cpu.eax || cpu.esp != esp + 4U)
        return 11;
    q = cpu.eax;
    if (!isaac_vita_guest_heap_owns((void *)(uintptr_t)q))
        return 12;
    for (i = 0U; i < 32U; ++i) {
        if (((uint8_t *)(uintptr_t)q)[i] != 0U)
            return 13;
    }

    /* Foreign/interior pointers fault before reaching native newlib and do
     * not disturb the still-owned base allocation. */
    esp = prepare_call(&cpu, p + 1U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_FREE_NAME, &calls) ||
        calls != 7U || cpu.esp != esp || !cpu.fault ||
        !isaac_vita_guest_heap_owns((void *)(uintptr_t)p) ||
        strcmp(cpu.fault, "free received a foreign guest pointer") != 0)
        return 14;
    esp = prepare_call(&cpu, (uint32_t)(uintptr_t)s_foreign, 64U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_REALLOC_NAME, &calls) ||
        calls != 8U || cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault, "realloc received a foreign guest pointer") != 0)
        return 15;

    /* A failed realloc must retain the old allocation and its contents. */
    esp = prepare_call(&cpu, p, UINT32_MAX);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_REALLOC_NAME, &calls) ||
        calls != 9U || cpu.fault || cpu.eax != 0U || cpu.esp != esp + 4U ||
        !isaac_vita_guest_heap_owns((void *)(uintptr_t)p))
        return 16;
    for (i = 0U; i < 32U; ++i) {
        if (((uint8_t *)(uintptr_t)p)[i] != (uint8_t)(i ^ 0x5aU))
            return 17;
    }

    esp = prepare_call(&cpu, p, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_FREE_NAME, &calls) ||
        calls != 10U || cpu.fault || cpu.esp != esp + 4U ||
        isaac_vita_guest_heap_owns((void *)(uintptr_t)p))
        return 18;
    esp = prepare_call(&cpu, p, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_FREE_NAME, &calls) ||
        calls != 11U || cpu.esp != esp || !cpu.fault ||
        strcmp(cpu.fault, "free received a foreign guest pointer") != 0)
        return 19;
    esp = prepare_call(&cpu, q, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_FREE_NAME, &calls) ||
        calls != 12U || cpu.fault || cpu.esp != esp + 4U ||
        isaac_vita_guest_heap_owns((void *)(uintptr_t)q))
        return 20;
    esp = prepare_call(&cpu, 0U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_FREE_NAME, &calls) ||
        calls != 13U || cpu.fault || cpu.esp != esp + 4U)
        return 21;

    /* realloc(pointer, 0) frees exactly once; NULL is malloc semantics. */
    esp = prepare_call(&cpu, 16U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_MALLOC_NAME, &calls) ||
        calls != 14U || cpu.fault || !cpu.eax || cpu.esp != esp + 4U)
        return 22;
    p = cpu.eax;
    esp = prepare_call(&cpu, p, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_REALLOC_NAME, &calls) ||
        calls != 15U || cpu.fault || cpu.eax != 0U ||
        cpu.esp != esp + 4U ||
        isaac_vita_guest_heap_owns((void *)(uintptr_t)p))
        return 23;
    esp = prepare_call(&cpu, 0U, 8U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_REALLOC_NAME, &calls) ||
        calls != 16U || cpu.fault || !cpu.eax || cpu.esp != esp + 4U ||
        !isaac_vita_guest_heap_owns((void *)(uintptr_t)cpu.eax))
        return 24;
    p = cpu.eax;
    esp = prepare_call(&cpu, p, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_FREE_NAME, &calls) ||
        calls != 17U || cpu.fault || cpu.esp != esp + 4U)
        return 25;

    esp = prepare_call(&cpu, 4096U, 0U);
    if (!isaac_vita_heap_import_counted(
            &cpu, ISAAC_VITA_HEAP_CALLNEWH_NAME, &calls) ||
        calls != 18U || cpu.fault || cpu.eax != 0U || cpu.esp != esp + 4U)
        return 26;

    /* Production faults nonlocally, so count must be committed first and the
     * guest cdecl frame must remain available for the fault record. */
    esp = prepare_call(&s_jump_cpu, 2U, 0U);
    s_jump_cpu.eax = 0x2468ace0U;
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_heap_import_counted(
            &s_jump_cpu, ISAAC_VITA_HEAP_SET_NEW_MODE_NAME, &s_jump_count);
        return 27;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != 19U || s_jump_cpu.esp != esp ||
        s_jump_cpu.eax != 0x2468ace0U || s_jump_cpu.fault_addr != 2U ||
        !s_jump_cpu.fault || strcmp(s_jump_cpu.fault,
            "_set_new_mode requires 0 or 1") != 0)
        return 28;

    /* Cross both 64->128 and 128->256 rehash thresholds, then prove that
     * tombstone-only churn compacts at the same capacity instead of growing
     * without bound.  These are direct service calls, so import count stays
     * frozen at the ABI oracle's value above. */
    if (!isaac_vita_guest_heap_test_bounded_probe() ||
        isaac_vita_guest_heap_test_live_count() != 0U)
        return 29;
    for (i = 0U; i < 96U; ++i) {
        s_churn[i] = isaac_vita_guest_malloc(24U + (i & 7U));
        if (!s_churn[i] || !isaac_vita_guest_heap_owns(s_churn[i]))
            return 30;
    }
    stable_capacity = isaac_vita_guest_heap_test_capacity();
    if (stable_capacity < 256U ||
        isaac_vita_guest_heap_test_live_count() != 96U)
        return 31;
    for (i = 0U; i < 96U; ++i) {
        if (!isaac_vita_guest_free(s_churn[i]))
            return 32;
    }
    for (wave = 0U; wave < 8U; ++wave) {
        for (i = 0U; i < 96U; ++i) {
            s_churn[i] = isaac_vita_guest_malloc(17U + (i & 3U));
            if (!s_churn[i])
                return 33;
        }
        for (i = 0U; i < 96U; ++i) {
            if (!isaac_vita_guest_free(s_churn[i]))
                return 34;
        }
        if (isaac_vita_guest_heap_test_capacity() != stable_capacity ||
            isaac_vita_guest_heap_test_live_count() != 0U)
            return 35;
    }

    /* More than one complete table's worth of deterministic moves must finish
     * with one live owner and without capacity growth.  Retired native blocks
     * stay allocated during the loop, so every replacement base is distinct. */
    {
        size_t move;
        size_t move_count;
        size_t moves_before = isaac_vita_guest_heap_test_realloc_moves();
        void **retired;

        if (stable_capacity > SIZE_MAX - 17U)
            return 36;
        move_count = stable_capacity + 17U;
        if (move_count > SIZE_MAX / sizeof *retired)
            return 36;
        retired = (void **)calloc(move_count, sizeof *retired);
        if (!retired)
            return 36;
        s_churn[0] = isaac_vita_guest_malloc(64U);
        if (!s_churn[0])
            return 36;
        for (move = 0U; move < move_count; ++move) {
            void *moved = isaac_vita_guest_heap_test_force_move(
                s_churn[0], 64U + (move & 15U), &retired[move]);
            if (!moved || !retired[move] ||
                isaac_vita_guest_heap_owns(retired[move]) ||
                !isaac_vita_guest_heap_owns(moved) ||
                isaac_vita_guest_heap_test_live_count() != 1U ||
                isaac_vita_guest_heap_test_capacity() != stable_capacity ||
                isaac_vita_guest_heap_test_realloc_moves() !=
                    moves_before + move + 1U)
                return 37;
            s_churn[0] = moved;
        }
        if (!isaac_vita_guest_free(s_churn[0]) ||
            isaac_vita_guest_heap_test_live_count() != 0U)
            return 38;
        for (move = 0U; move < move_count; ++move)
            free(retired[move]);
        free(retired);
    }

    /* MSVC's >=0x1000 vector branch exposes a 32-byte-aligned interior begin,
     * with the raw malloc base in begin[-1].  Containment must use the exact
     * requested span and pin that raw allocation during validation. */
    raw = (uintptr_t)isaac_vita_guest_malloc(64U * 0x4cU + 0x23U);
    if (!raw)
        return 39;
    begin = (raw + 0x23U) & ~(uintptr_t)0x1fU;
    if (begin - raw < 4U || begin - raw > 0x23U)
        return 40;
    lease = isaac_vita_guest_heap_lease_containing(
        (void *)(begin - 4U), 64U * 0x4cU + 4U, &allocation_base);
    if (!lease || allocation_base != raw ||
        isaac_vita_guest_heap_test_active_leases() != 1U)
        return 41;
    valid_owner = 1;
    if (isaac_vita_guest_free((void *)raw) != 0 ||
        isaac_vita_guest_realloc((void *)raw, 8192U, &valid_owner) != NULL ||
        valid_owner != 0 || !isaac_vita_guest_heap_owns((void *)raw))
        return 42;
    if (!isaac_vita_guest_heap_lease_release(lease) ||
        isaac_vita_guest_heap_test_active_leases() != 0U)
        return 43;

    /* A stale token cannot clear a later lease on the same address (the lease
     * table's same-address reuse/ABA case). */
    next_lease = isaac_vita_guest_heap_lease_containing(
        (void *)(begin - 4U), 64U * 0x4cU + 4U, &allocation_base);
    if (!next_lease || next_lease == lease ||
        isaac_vita_guest_heap_lease_release(lease) != 0 ||
        isaac_vita_guest_free((void *)raw) != 0 ||
        !isaac_vita_guest_heap_lease_release(next_lease) ||
        !isaac_vita_guest_free((void *)raw))
        return 44;

    /* Requested-size accounting must not admit allocator padding. */
    raw = (uintptr_t)isaac_vita_guest_malloc(64U);
    if (!raw)
        return 45;
    lease = isaac_vita_guest_heap_lease_containing(
        (void *)(raw + 60U), 4U, &allocation_base);
    if (!lease || allocation_base != raw ||
        !isaac_vita_guest_heap_lease_release(lease) ||
        isaac_vita_guest_heap_lease_containing(
            (void *)(raw + 60U), 5U, &allocation_base) != 0U ||
        !isaac_vita_guest_free((void *)raw))
        return 46;

    /* The fixed eight-lease bound is fail-closed and fully recoverable. */
    for (i = 0U; i < 9U; ++i) {
        s_churn[i] = isaac_vita_guest_malloc(32U + i);
        if (!s_churn[i])
            return 47;
    }
    for (i = 0U; i < 8U; ++i) {
        leases[i] = isaac_vita_guest_heap_lease_containing(
            s_churn[i], 1U, &allocation_base);
        if (!leases[i] || allocation_base != (uintptr_t)s_churn[i])
            return 48;
    }
    if (isaac_vita_guest_heap_lease_containing(
            s_churn[8], 1U, &allocation_base) != 0U)
        return 49;
    for (i = 0U; i < 8U; ++i)
        if (!isaac_vita_guest_heap_lease_release(leases[i]))
            return 50;
    for (i = 0U; i < 9U; ++i)
        if (!isaac_vita_guest_free(s_churn[i]))
            return 51;
    if (isaac_vita_guest_heap_test_active_leases() != 0U ||
        isaac_vita_guest_heap_test_live_count() != 0U)
        return 52;

    return 0;
}
