#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_room_entry_external.h"

#define ORACLE_MAX_PLANS 64U
#define ORACLE_MAX_FREES 128U
#define ORACLE_ARENA_BYTES \
    (ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS * \
         ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES + \
     ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES)

typedef struct oracle_alloc_plan {
    SceUID uid;
    void *base;
    int get_base_result;
} oracle_alloc_plan;

static oracle_alloc_plan s_plans[ORACLE_MAX_PLANS];
static uint32_t s_plan_count;
static uint32_t s_plan_next;
static uint32_t s_current_plan;
static uint32_t s_alloc_calls;
static uint32_t s_get_base_calls;
static uint32_t s_free_calls;
static SceUID s_free_uids[ORACLE_MAX_FREES];
static SceUID s_free_fail_uid = (SceUID)-1;
static uint32_t s_free_fail_remaining;
_Alignas(4096) static unsigned char s_arena[ORACLE_ARENA_BYTES];

static const isaac_vita_heap_overflow_forbidden_ranges s_forbidden = {
    {UINT32_C(0x10000000), UINT32_C(0x11000000)},
    {UINT32_C(0x20000000), UINT32_C(0x21000000)},
    {UINT32_C(0x30000000), UINT32_C(0x31000000)}
};
static const isaac_vita_heap_overflow_range s_raw = {
    UINT32_C(0x40000000), UINT32_C(0x407d5000)
};

#define CHECK(expression) do {                                                \
    if (!(expression)) {                                                     \
        fprintf(stderr, "external oracle failed at %s:%d: %s\n",           \
                __FILE__, __LINE__, #expression);                            \
        return 0;                                                            \
    }                                                                        \
} while (0)

static void fake_require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "external oracle fake failed: %s\n", message);
        abort();
    }
}

static void fake_reset(void)
{
    memset(s_plans, 0, sizeof s_plans);
    memset(s_free_uids, 0, sizeof s_free_uids);
    s_plan_count = 0U;
    s_plan_next = 0U;
    s_current_plan = 0U;
    s_alloc_calls = 0U;
    s_get_base_calls = 0U;
    s_free_calls = 0U;
    s_free_fail_uid = (SceUID)-1;
    s_free_fail_remaining = 0U;
}

static void fake_plan(SceUID uid, uintptr_t base, int get_base_result)
{
    fake_require(s_plan_count < ORACLE_MAX_PLANS, "plan overflow");
    s_plans[s_plan_count].uid = uid;
    s_plans[s_plan_count].base = (void *)base;
    s_plans[s_plan_count].get_base_result = get_base_result;
    ++s_plan_count;
}

static uintptr_t arena_address(size_t offset)
{
    fake_require(offset < sizeof s_arena, "arena offset overflow");
    return (uintptr_t)&s_arena[offset];
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    fake_require(name && strcmp(name, "isaac_room_entries") == 0,
                 "allocation name drifted");
    fake_require(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                 "allocation type drifted");
    fake_require(size == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES,
                 "allocation request drifted");
    fake_require(option == NULL, "allocation options must stay NULL");
    fake_require(s_plan_next < s_plan_count, "unexpected allocation call");
    s_current_plan = s_plan_next++;
    ++s_alloc_calls;
    return s_plans[s_current_plan].uid;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    oracle_alloc_plan *plan = &s_plans[s_current_plan];

    fake_require(uid == plan->uid, "GetBase UID drifted");
    fake_require(base != NULL, "GetBase output missing");
    ++s_get_base_calls;
    if (plan->get_base_result >= 0)
        *base = plan->base;
    return plan->get_base_result;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    fake_require(s_free_calls < ORACLE_MAX_FREES, "free log overflow");
    s_free_uids[s_free_calls++] = uid;
    if (uid == s_free_fail_uid && s_free_fail_remaining) {
        --s_free_fail_remaining;
        return -77;
    }
    return 0;
}

static int cleanup_external(void)
{
    isaac_vita_room_entry_external_test_receipt receipt;
    int newest;

    s_free_fail_remaining = 0U;
    for (;;) {
        newest = isaac_vita_room_entry_external_test_newest_receipt(&receipt);
        CHECK(newest >= 0);
        if (!newest)
            break;
        CHECK(isaac_vita_room_entry_external_test_release_newest(
                  &receipt) == 1);
    }
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int begin_case(void)
{
    CHECK(cleanup_external());
    fake_reset();
    CHECK(isaac_vita_room_entry_external_init(&s_forbidden, &s_raw));
    return 1;
}

static int snapshot(isaac_vita_room_entry_external_snapshot *value)
{
    memset(value, 0xa5, sizeof *value);
    return isaac_vita_room_entry_external_snapshot_get(value);
}

static int reserve_commit(int allow_new,
                          isaac_vita_room_entry_external_ticket *ticket)
{
    CHECK(isaac_vita_room_entry_external_reserve_page(
              allow_new, ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS);
    CHECK(isaac_vita_room_entry_external_commit_page(ticket));
    return 1;
}

static int fill_one_chunk(uintptr_t base, SceUID uid,
                          isaac_vita_room_entry_external_ticket *first,
                          isaac_vita_room_entry_external_ticket *middle,
                          isaac_vita_room_entry_external_ticket *last)
{
    uint32_t page;

    fake_plan(uid, base, 0);
    for (page = 0U;
         page < ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK; ++page) {
        isaac_vita_room_entry_external_ticket ticket;

        CHECK(reserve_commit(page == 0U, &ticket));
        CHECK(ticket.page == (void *)(base +
              page * ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES));
        CHECK(ticket.page_index == page);
        CHECK(ticket.new_chunk == (page == 0U));
        if (page == 0U && first)
            *first = ticket;
        if (page == 7U && middle)
            *middle = ticket;
        if (page + 1U ==
                ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK && last)
            *last = ticket;
    }
    return 1;
}

static int test_success_and_reset(void)
{
    const uintptr_t first_base = arena_address(0U);
    const uintptr_t second_base = first_base +
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES;
    isaac_vita_room_entry_external_ticket first;
    isaac_vita_room_entry_external_ticket middle;
    isaac_vita_room_entry_external_ticket last;
    isaac_vita_room_entry_external_ticket partial;
    isaac_vita_room_entry_external_ticket cancelled;
    isaac_vita_room_entry_external_snapshot state;
    isaac_vita_room_entry_external_test_receipt receipt;
    isaac_vita_room_entry_external_test_receipt retry;

    CHECK(begin_case());
    CHECK(isaac_vita_room_entry_external_reserve_page(0, &partial) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_EMPTY);
    CHECK(s_alloc_calls == 0U);
    CHECK(fill_one_chunk(first_base, 41, &first, &middle, &last));
    CHECK(s_alloc_calls == 1U && s_get_base_calls == 1U);
    CHECK(isaac_vita_room_entry_external_page_matches(
              0U, 0U, first.page));
    CHECK(isaac_vita_room_entry_external_page_matches(
              0U, 7U, middle.page));
    CHECK(isaac_vita_room_entry_external_page_matches(
              0U, 15U, last.page));
    CHECK(!isaac_vita_room_entry_external_page_matches(
               0U, 16U, last.page));
    CHECK(!isaac_vita_room_entry_external_page_matches(
               0U, 7U, first.page));

    /* Actual request spans are exactly adjacent.  Their conservative
     * 0x102000 receipts overlap by 0x1000 and must not reject each other. */
    fake_plan(42, second_base, 0);
    CHECK(reserve_commit(1, &partial));
    CHECK(partial.chunk_index == 1U && partial.page_index == 0U &&
          partial.new_chunk == 1U &&
          partial.page == (void *)second_base);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY);
    CHECK(state.chunks == 2U && state.issued_pages == 17U &&
          state.pending == 0U);
    CHECK(state.requested_bytes ==
              2U * ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES);
    CHECK(state.usable_bytes ==
              2U * ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES);
    CHECK(state.retained_bytes ==
              2U * ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES);
    CHECK(state.allocation_attempts == 2U && state.out_of_memory == 0U);

    CHECK(isaac_vita_room_entry_external_reserve_page(0, &cancelled) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS);
    CHECK(!cancelled.new_chunk && cancelled.page_index == 1U);
    CHECK(isaac_vita_room_entry_external_cancel_page(&cancelled));
    CHECK(s_free_calls == 0U);
    CHECK(snapshot(&state) && state.issued_pages == 17U && !state.pending);

    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&receipt) == 1);
    CHECK(receipt.uid == 42 && receipt.chunk_index == 1U &&
          receipt.issued_pages == 1U && !receipt.orphan);
    s_free_fail_uid = 42;
    s_free_fail_remaining = 1U;
    CHECK(isaac_vita_room_entry_external_test_release_newest(&receipt) == 0);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.chunks == 2U && state.issued_pages == 17U &&
          state.reset_free_attempts == 1U &&
          state.reset_free_failures == 1U);
    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&retry) == 1);
    CHECK(memcmp(&receipt, &retry, sizeof receipt) == 0);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&retry) == 1);
    CHECK(snapshot(&state) && state.chunks == 1U &&
          state.issued_pages == 16U && state.reset_free_attempts == 2U &&
          state.reset_free_failures == 1U);
    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&receipt) == 1);
    CHECK(receipt.uid == 41 && receipt.chunk_index == 0U &&
          receipt.issued_pages == 16U);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&receipt) == 1);
    CHECK(snapshot(&state) && !state.chunks && !state.issued_pages);
    CHECK(s_free_calls == 3U && s_free_uids[0] == 42 &&
          s_free_uids[1] == 42 && s_free_uids[2] == 41);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int test_ordinary_oom(void)
{
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    fake_plan(-123, 0U, 0);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_OUT_OF_MEMORY);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY &&
          state.allocation_attempts == 1U && state.out_of_memory == 1U &&
          !state.chunks && !state.issued_pages && !state.pending &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE &&
          state.last_syscall ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ALLOC &&
          state.last_syscall_result == -123);
    CHECK(s_get_base_calls == 0U && s_free_calls == 0U);
    fake_plan(51, arena_address(0U), 0);
    CHECK(reserve_commit(1, &ticket));
    /* A committed page needs the exact unissue transaction. */
    CHECK(isaac_vita_room_entry_external_unissue_page(&ticket));
    CHECK(snapshot(&state) && state.state ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY && !state.chunks);
    return 1;
}

static int test_post_uid_fault(
    uintptr_t base, int get_base_result,
    isaac_vita_room_entry_external_fault expected_fault)
{
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;
    uint32_t alloc_before;

    CHECK(begin_case());
    fake_plan(61, base, get_base_result);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.terminal_fault == expected_fault &&
          state.allocation_attempts == 1U &&
          state.post_uid_failures == 1U &&
          state.rollback_free_attempts == 1U &&
          state.rollback_free_failures == 0U &&
          state.last_syscall ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ROLLBACK_FREE &&
          state.last_syscall_result == 0 && !state.chunks &&
          state.orphan_uid == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID);
    CHECK(s_alloc_calls == 1U && s_plan_next == 1U &&
          s_get_base_calls == 1U && s_free_calls == 1U &&
          s_free_uids[0] == 61);
    alloc_before = s_alloc_calls;
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(s_alloc_calls == alloc_before);
    return 1;
}

static int test_range_failures(void)
{
    uintptr_t retained_wrap = UINTPTR_MAX -
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES + 1U;
    uintptr_t actual_wrap = UINTPTR_MAX -
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES + 1U;

    CHECK(test_post_uid_fault(UINT32_C(0x52000000), -9,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_GET_BASE));
    CHECK(test_post_uid_fault(0U, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NULL_BASE));
    CHECK(test_post_uid_fault(UINT32_C(0x52000001), 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ALIGNMENT));
    CHECK((actual_wrap & 0xfffU) == 0U);
    CHECK(test_post_uid_fault(actual_wrap, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_WRAP));
    CHECK((retained_wrap & 0xfffU) == 0U);
    CHECK(test_post_uid_fault(retained_wrap, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_WRAP));

    CHECK(test_post_uid_fault(s_forbidden.fixed_image.begin - 0x1000U, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_FIXED));
    CHECK(test_post_uid_fault(s_forbidden.guest_stack.begin - 0x1000U, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_STACK));
    CHECK(test_post_uid_fault(s_forbidden.retained_newlib.begin - 0x1000U, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_NEWLIB));
    CHECK(test_post_uid_fault(
          s_forbidden.fixed_image.begin -
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_FIXED));
    CHECK(test_post_uid_fault(
          s_forbidden.guest_stack.begin -
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_STACK));
    CHECK(test_post_uid_fault(
          s_forbidden.retained_newlib.begin -
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_NEWLIB));
    CHECK(test_post_uid_fault(s_raw.begin, 0,
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_RAW));
    return 1;
}

static int test_prior_overlap(void)
{
    const uintptr_t base = arena_address(0U);
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    CHECK(fill_one_chunk(base, 71, NULL, NULL, NULL));
    fake_plan(72, base + ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES, 0);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_PRIOR &&
          state.chunks == 1U && state.issued_pages == 16U &&
          state.post_uid_failures == 1U && s_free_calls == 1U &&
          s_free_uids[0] == 72);
    CHECK(isaac_vita_room_entry_external_page_matches(
              0U, 15U,
              (void *)(base + 15U *
                  ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES)));
    return 1;
}

static int test_duplicate_uid(void)
{
    const uintptr_t base = arena_address(0U);
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    CHECK(fill_one_chunk(base, 81, NULL, NULL, NULL));
    fake_plan(81, arena_address(
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES), 0);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_DUPLICATE_UID &&
          state.chunks == 1U && state.issued_pages == 16U &&
          state.post_uid_failures == 0U);
    CHECK(s_alloc_calls == 2U && s_get_base_calls == 1U &&
          s_free_calls == 0U);
    return 1;
}

static int test_orphan_and_retry(void)
{
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;
    isaac_vita_room_entry_external_test_receipt receipt;
    uint32_t alloc_before;

    CHECK(begin_case());
    fake_plan(91, 0U, 0);
    s_free_fail_uid = 91;
    s_free_fail_remaining = 1U;
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NULL_BASE &&
          state.rollback_free_attempts == 1U &&
          state.rollback_free_failures == 1U &&
          state.orphan_uid == 91 &&
          state.orphan_requested_bytes ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES &&
          state.orphan_retained_bytes ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES);
    alloc_before = s_alloc_calls;
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(s_alloc_calls == alloc_before);
    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&receipt) == 1);
    CHECK(receipt.orphan && receipt.uid == 91 && !receipt.issued_pages);
    s_free_fail_remaining = 1U;
    CHECK(isaac_vita_room_entry_external_test_release_newest(&receipt) == 0);
    CHECK(snapshot(&state) && state.state ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED && state.orphan_uid == 91 &&
          state.reset_free_attempts == 1U &&
          state.reset_free_failures == 1U);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&receipt) == 1);
    CHECK(snapshot(&state) && state.state ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.orphan_uid == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int test_cancel_unissue_and_stale(void)
{
    isaac_vita_room_entry_external_ticket first;
    isaac_vita_room_entry_external_ticket second;
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    fake_plan(101, arena_address(0U), 0);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &first) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS);
    CHECK(isaac_vita_room_entry_external_cancel_page(&first));
    CHECK(snapshot(&state) && !state.chunks && !state.issued_pages &&
          state.rollback_free_attempts == 1U && s_free_calls == 1U);

    fake_plan(102, arena_address(0U), 0);
    CHECK(reserve_commit(1, &first));
    CHECK(isaac_vita_room_entry_external_unissue_page(&first));
    CHECK(snapshot(&state) && !state.chunks && !state.issued_pages &&
          state.rollback_free_attempts == 2U && s_free_calls == 2U);

    fake_plan(103, arena_address(0U), 0);
    CHECK(reserve_commit(1, &first));
    CHECK(reserve_commit(0, &second));
    CHECK(!isaac_vita_room_entry_external_unissue_page(&first));
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION &&
          state.chunks == 1U && state.issued_pages == 2U &&
          s_free_calls == 2U);
    return 1;
}

static int test_publication_orphan(void)
{
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;
    isaac_vita_room_entry_external_test_receipt receipt;
    uint32_t alloc_before;

    CHECK(begin_case());
    fake_plan(111, arena_address(0U), 0);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS);
    s_free_fail_uid = 111;
    s_free_fail_remaining = 1U;
    CHECK(!isaac_vita_room_entry_external_cancel_page(&ticket));
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED &&
          !state.chunks && !state.issued_pages && !state.pending &&
          state.orphan_uid == 111 &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION &&
          state.rollback_free_attempts == 1U &&
          state.rollback_free_failures == 1U);
    alloc_before = s_alloc_calls;
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(s_alloc_calls == alloc_before);
    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&receipt) == 1);
    CHECK(receipt.orphan && receipt.uid == 111);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&receipt) == 1);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());

    CHECK(begin_case());
    fake_plan(112, arena_address(0U), 0);
    CHECK(reserve_commit(1, &ticket));
    s_free_fail_uid = 112;
    s_free_fail_remaining = 1U;
    /* The caller has already unpublished this descriptor.  A zero return
     * means the issued page was consumed and its UID moved to orphan. */
    CHECK(!isaac_vita_room_entry_external_unissue_page(&ticket));
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED &&
          !state.chunks && !state.issued_pages &&
          state.orphan_uid == 112 &&
          !isaac_vita_room_entry_external_page_matches(
              ticket.chunk_index, ticket.page_index, ticket.page));
    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&receipt) == 1);
    CHECK(receipt.orphan && receipt.uid == 112);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&receipt) == 1);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int test_reverse_reset_three(void)
{
    const uintptr_t base = arena_address(0U);
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;
    isaac_vita_room_entry_external_test_receipt newest;
    isaac_vita_room_entry_external_test_receipt middle;
    uint32_t frees_before;

    CHECK(begin_case());
    CHECK(fill_one_chunk(base, 121, NULL, NULL, NULL));
    CHECK(fill_one_chunk(
          base + ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES,
          122, NULL, NULL, NULL));
    fake_plan(123, base +
        2U * ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES, 0);
    CHECK(reserve_commit(1, &ticket));
    CHECK(snapshot(&state) && state.chunks == 3U &&
          state.issued_pages == 33U);

    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&newest) == 1);
    CHECK(newest.uid == 123 && newest.chunk_index == 2U &&
          newest.issued_pages == 1U);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&newest) == 1);
    CHECK(snapshot(&state) && state.chunks == 2U &&
          state.issued_pages == 32U);
    frees_before = s_free_calls;
    CHECK(isaac_vita_room_entry_external_test_release_newest(&newest) == -1);
    CHECK(s_free_calls == frees_before);

    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&middle) == 1);
    CHECK(middle.uid == 122 && middle.chunk_index == 1U &&
          middle.issued_pages == 16U);
    s_free_fail_uid = 122;
    s_free_fail_remaining = 1U;
    CHECK(isaac_vita_room_entry_external_test_release_newest(&middle) == 0);
    CHECK(snapshot(&state) && state.chunks == 2U &&
          state.issued_pages == 32U && state.reset_free_attempts == 2U &&
          state.reset_free_failures == 1U);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&middle) == 1);
    CHECK(snapshot(&state) && state.chunks == 1U &&
          state.issued_pages == 16U);
    CHECK(isaac_vita_room_entry_external_test_newest_receipt(&newest) == 1);
    CHECK(newest.uid == 121 && newest.chunk_index == 0U &&
          newest.issued_pages == 16U);
    CHECK(isaac_vita_room_entry_external_test_release_newest(&newest) == 1);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int test_invalid_init(void)
{
    isaac_vita_heap_overflow_range bad_raw = s_forbidden.fixed_image;
    isaac_vita_room_entry_external_snapshot state;

    CHECK(cleanup_external());
    fake_reset();
    CHECK(!isaac_vita_room_entry_external_init(&s_forbidden, &bad_raw));
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAILED &&
          state.terminal_fault == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_INIT &&
          state.orphan_uid == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID);
    CHECK(!isaac_vita_room_entry_external_init(&s_forbidden, &s_raw));
    CHECK(snapshot(&state) && state.state ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAILED);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int test_rollback_counter_saturation(void)
{
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    isaac_vita_room_entry_external_test_saturate_rollback_counter();
    fake_plan(131, 0U, 0);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL);
    CHECK(snapshot(&state));
    CHECK(state.state == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NULL_BASE &&
          state.counter_saturated == 1U &&
          state.rollback_free_attempts == UINT32_MAX &&
          state.post_uid_failures == 1U &&
          state.orphan_uid == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID &&
          s_free_calls == 1U && s_free_uids[0] == 131);
    return 1;
}

static int fill_limit_state(void)
{
    const uintptr_t base = arena_address(0U);
    uint32_t chunk;

    for (chunk = 0U;
         chunk < ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS; ++chunk) {
        CHECK(fill_one_chunk(
              base + (uintptr_t)chunk *
                  ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES,
              (SceUID)(200 + chunk), NULL, NULL, NULL));
    }
    return 1;
}

static int test_limit_and_compensation(void)
{
    isaac_vita_room_entry_external_ticket ticket;
    isaac_vita_room_entry_external_snapshot state;
    isaac_vita_room_entry_external_test_receipt receipt;
    uint32_t raw_pages;
    uint32_t expected_uid = 211U;

    for (raw_pages = 0U; raw_pages <= 96U; ++raw_pages) {
        uint32_t required_pages = 192U - raw_pages;
        uint32_t required_chunks = (required_pages + 15U) / 16U;

        CHECK(required_chunks <=
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS);
        CHECK(raw_pages + required_chunks * 16U >= 192U);
    }
    CHECK(begin_case());
    CHECK(fill_limit_state());
    CHECK(snapshot(&state));
    CHECK(state.chunks == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS &&
          state.issued_pages ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_ISSUED_PAGES &&
          state.requested_bytes ==
              (size_t)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS *
                  ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES &&
          state.retained_bytes ==
              (size_t)ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS *
                  ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES);
    CHECK(s_alloc_calls == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS);
    CHECK(isaac_vita_room_entry_external_reserve_page(1, &ticket) ==
          ISAAC_VITA_ROOM_ENTRY_EXTERNAL_LIMIT);
    CHECK(s_alloc_calls == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS);
    while (isaac_vita_room_entry_external_test_newest_receipt(&receipt) == 1) {
        CHECK((uint32_t)receipt.uid == expected_uid--);
        CHECK(receipt.issued_pages == 16U);
        CHECK(isaac_vita_room_entry_external_test_release_newest(
                  &receipt) == 1);
    }
    CHECK(expected_uid == 199U);
    CHECK(isaac_vita_room_entry_external_test_finish_reset());
    return 1;
}

static int run_corrupt_count(void)
{
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    isaac_vita_room_entry_external_test_corrupt_chunk_count();
    CHECK(!snapshot(&state));
    return 1;
}

static int run_corrupt_receipts(void)
{
    isaac_vita_room_entry_external_snapshot state;

    CHECK(begin_case());
    CHECK(fill_limit_state());
    isaac_vita_room_entry_external_test_corrupt_receipt_total();
    CHECK(!snapshot(&state));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--corrupt-count") == 0)
        return run_corrupt_count() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--corrupt-receipts") == 0)
        return run_corrupt_receipts() ? 0 : 1;
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--corrupt-count|--corrupt-receipts]\n",
                argv[0]);
        return 2;
    }
    if (!test_invalid_init() || !test_success_and_reset() ||
        !test_reverse_reset_three() || !test_ordinary_oom() ||
        !test_range_failures() || !test_prior_overlap() ||
        !test_duplicate_uid() || !test_orphan_and_retry() ||
        !test_cancel_unissue_and_stale() ||
        !test_publication_orphan() ||
        !test_rollback_counter_saturation() ||
        !test_limit_and_compensation() || !cleanup_external())
        return 1;
    puts("room-entry external oracle: PASS");
    return 0;
}
