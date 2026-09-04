#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_vita_heap.h"

typedef struct worker_attempt {
    void *pointer;
    int free_result;
    void *realloc_result;
    int realloc_valid_owner;
} worker_attempt;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* Sanitizers retain cold import-dispatch sections which an optimized host
 * link normally discards.  Keep those fail-closed edges linkable without
 * weakening the range-lease transaction exercised below. */
void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)kind;
    (void)address;
    (void)size;
    guest_fault(c, pc, "lease oracle guest stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(
        c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
}

static void *attempt_free_and_realloc(void *opaque)
{
    worker_attempt *attempt = (worker_attempt *)opaque;
    attempt->free_result = isaac_vita_guest_free(attempt->pointer);
    attempt->realloc_valid_owner = 1;
    attempt->realloc_result = isaac_vita_guest_realloc(
        attempt->pointer, 8192U, &attempt->realloc_valid_owner);
    return NULL;
}

int main(void)
{
    enum { LEASE_CAPACITY = 8, EXTRA_ALLOCATION = 1 };
    const size_t record_size = 0x4cU;
    const size_t span = 64U * record_size;
    void *allocations[LEASE_CAPACITY + EXTRA_ALLOCATION];
    uint32_t tokens[LEASE_CAPACITY];
    uintptr_t base;
    uintptr_t raw;
    uintptr_t begin;
    size_t index;
    uint32_t token;
    uint32_t next_token;
    worker_attempt attempt;
    pthread_t worker;
    int valid_owner;
    void *resized;
    void *retired;
    void *unrelated;

    CHECK(isaac_vita_guest_heap_test_active_leases() == 0U);
    CHECK(isaac_vita_guest_heap_lease_containing(NULL, 1U, &base) == 0U);
    CHECK(isaac_vita_guest_heap_lease_containing(
              (void *)(UINTPTR_MAX - 3U), 8U, &base) == 0U);
    CHECK(isaac_vita_guest_heap_lease_exact_range(
              NULL, (void *)1U, 1U) == 0U);
    CHECK(isaac_vita_guest_heap_lease_exact_range(
              (void *)1U, (void *)(UINTPTR_MAX - 3U), 8U) == 0U);

    /* This reproduces MSVC's >=0x1000 vector branch: malloc(span+0x23),
     * begin=(raw+0x23)&~31, raw stored at begin[-1]. */
    raw = (uintptr_t)isaac_vita_guest_malloc(span + 0x23U);
    CHECK(raw != 0U);
    begin = (raw + 0x23U) & ~(uintptr_t)0x1fU;
    CHECK(begin - raw >= 4U && begin - raw <= 0x23U);
    token = isaac_vita_guest_heap_lease_containing(
        (void *)(begin - 4U), span + 4U, &base);
    CHECK(token != 0U && base == raw);

    /* The same allocation cannot be leased twice.  Cross-thread free/realloc
     * are rejected before the native allocator while the validation pin lives. */
    CHECK(isaac_vita_guest_heap_lease_containing(
              (void *)begin, span, &base) == 0U);
    attempt.pointer = (void *)raw;
    attempt.free_result = -1;
    attempt.realloc_result = (void *)(uintptr_t)1U;
    attempt.realloc_valid_owner = 1;
    CHECK(pthread_create(&worker, NULL, attempt_free_and_realloc, &attempt) == 0);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(attempt.free_result == 0 && attempt.realloc_result == NULL &&
          attempt.realloc_valid_owner == 0 &&
          isaac_vita_guest_heap_owns((void *)raw));
    unrelated = isaac_vita_guest_malloc(19U);
    CHECK(unrelated != NULL && isaac_vita_guest_free(unrelated) == 1);
    CHECK(isaac_vita_guest_heap_lease_release(token) == 1);
    CHECK(isaac_vita_guest_heap_test_active_leases() == 0U);

    /* Tokens, not addresses or slots, identify leases.  Reacquiring the exact
     * same base models allocator same-address reuse for stale-release safety. */
    next_token = isaac_vita_guest_heap_lease_containing(
        (void *)(begin - 4U), span + 4U, &base);
    CHECK(next_token != 0U && next_token != token && base == raw);
    CHECK(isaac_vita_guest_heap_lease_release(token) == 0);
    CHECK(isaac_vita_guest_free((void *)raw) == 0);
    CHECK(isaac_vita_guest_heap_lease_release(next_token) == 1);
    CHECK(isaac_vita_guest_free((void *)raw) == 1);

    /* Containment is against requested bytes, never malloc's spare padding. */
    raw = (uintptr_t)isaac_vita_guest_malloc(64U);
    CHECK(raw != 0U);
    token = isaac_vita_guest_heap_lease_exact_range(
        (void *)raw, (void *)(raw + 60U), 4U);
    CHECK(token != 0U);
    CHECK(isaac_vita_guest_heap_lease_exact_range(
              (void *)raw, (void *)(raw + 60U), 4U) == 0U);
    CHECK(isaac_vita_guest_heap_lease_containing(
              (void *)(raw + 60U), 4U, &base) == 0U);
    CHECK(isaac_vita_guest_heap_lease_release(token) == 1);
    CHECK(isaac_vita_guest_heap_lease_exact_range(
              (void *)(raw + 1U), (void *)(raw + 60U), 4U) == 0U);
    CHECK(isaac_vita_guest_heap_lease_exact_range(
              (void *)raw, (void *)(raw + 60U), 5U) == 0U);
    CHECK(isaac_vita_guest_heap_lease_containing(
              (void *)(raw + 60U), 5U, &base) == 0U);

    /* Realloc atomically replaces the exact requested extent in the ledger. */
    valid_owner = 0;
    resized = isaac_vita_guest_realloc((void *)raw, 128U, &valid_owner);
    CHECK(resized != NULL && valid_owner == 1);
    raw = (uintptr_t)resized;
    token = isaac_vita_guest_heap_lease_containing(
        (void *)(raw + 120U), 8U, &base);
    CHECK(token != 0U && base == raw);
    CHECK(isaac_vita_guest_heap_lease_release(token) == 1);
    CHECK(isaac_vita_guest_heap_lease_containing(
              (void *)(raw + 120U), 9U, &base) == 0U);

    retired = NULL;
    resized = isaac_vita_guest_heap_test_force_move(
        (void *)raw, 256U, &retired);
    CHECK(resized != NULL && retired == (void *)raw);
    CHECK(!isaac_vita_guest_heap_owns(retired));
    CHECK(isaac_vita_guest_heap_lease_containing(
              retired, 1U, &base) == 0U);
    token = isaac_vita_guest_heap_lease_containing(
        (unsigned char *)resized + 255U, 1U, &base);
    CHECK(token != 0U && base == (uintptr_t)resized);
    CHECK(isaac_vita_guest_heap_lease_release(token) == 1);
    CHECK(isaac_vita_guest_free(resized) == 1);
    free(retired);

    /* The fixed lease table fails closed at its bound without affecting
     * unrelated allocations, then fully recovers after exact releases. */
    for (index = 0U; index < LEASE_CAPACITY + EXTRA_ALLOCATION; ++index) {
        allocations[index] = isaac_vita_guest_malloc(32U + index);
        CHECK(allocations[index] != NULL);
    }
    for (index = 0U; index < LEASE_CAPACITY; ++index) {
        tokens[index] = isaac_vita_guest_heap_lease_containing(
            allocations[index], 1U, &base);
        CHECK(tokens[index] != 0U && base == (uintptr_t)allocations[index]);
    }
    CHECK(isaac_vita_guest_heap_test_active_leases() == LEASE_CAPACITY);
    CHECK(isaac_vita_guest_heap_lease_containing(
              allocations[LEASE_CAPACITY], 1U, &base) == 0U);
    for (index = 0U; index < LEASE_CAPACITY; ++index)
        CHECK(isaac_vita_guest_heap_lease_release(tokens[index]) == 1);
    for (index = 0U; index < LEASE_CAPACITY + EXTRA_ALLOCATION; ++index)
        CHECK(isaac_vita_guest_free(allocations[index]) == 1);
    CHECK(isaac_vita_guest_heap_test_active_leases() == 0U);
    CHECK(isaac_vita_guest_heap_test_live_count() == 0U);

    puts("Vita guest heap range-lease host oracle: PASS");
    return 0;
}
