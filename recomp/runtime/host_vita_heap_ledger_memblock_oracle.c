/* Dirty-page and failure-order oracle for the USER_RW ledger backend. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_heap.h"
#include "host_vita_heap_ledger_memblock.h"

#define FAKE_SLOT_COUNT 8U
#define FAKE_SLOT_BYTES 0x00010000U
#define FAKE_EVENT_COUNT 256U

enum fake_event_kind {
    FAKE_EVENT_ALLOC = 1,
    FAKE_EVENT_GET = 2,
    FAKE_EVENT_FREE = 3
};

typedef struct fake_slot {
    SceUID uid;
    SceSize size;
    int live;
} fake_slot;

typedef struct fake_event {
    int kind;
    SceUID uid;
    SceSize size;
    int result;
} fake_event;

_Alignas(ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES)
static unsigned char s_fake_bytes[FAKE_SLOT_COUNT][FAKE_SLOT_BYTES];
static fake_slot s_fake_slots[FAKE_SLOT_COUNT];
static fake_event s_fake_events[FAKE_EVENT_COUNT];
static size_t s_fake_event_count;
static SceUID s_next_uid = 0x100;
static unsigned s_fail_alloc;
static unsigned s_fail_get;
static unsigned s_misalign_get;
static unsigned s_fail_free;
static size_t s_oracle_calloc_calls;
static size_t s_oracle_free_calls;
static size_t s_oracle_realloc_calls;

static void fake_record(int kind, SceUID uid, SceSize size, int result)
{
    if (s_fake_event_count < FAKE_EVENT_COUNT) {
        s_fake_events[s_fake_event_count].kind = kind;
        s_fake_events[s_fake_event_count].uid = uid;
        s_fake_events[s_fake_event_count].size = size;
        s_fake_events[s_fake_event_count].result = result;
    }
    ++s_fake_event_count;
}

static fake_slot *fake_find(SceUID uid, size_t *index_out)
{
    size_t index;

    for (index = 0U; index < FAKE_SLOT_COUNT; ++index) {
        if (s_fake_slots[index].live && s_fake_slots[index].uid == uid) {
            if (index_out)
                *index_out = index;
            return &s_fake_slots[index];
        }
    }
    return NULL;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    size_t index;
    SceUID uid;

    if (s_fail_alloc) {
        --s_fail_alloc;
        fake_record(FAKE_EVENT_ALLOC, -1, size, -0x101);
        return -0x101;
    }
    if (!name || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || option ||
        !size || size > FAKE_SLOT_BYTES) {
        fake_record(FAKE_EVENT_ALLOC, -1, size, -0x102);
        return -0x102;
    }
    for (index = 0U; index < FAKE_SLOT_COUNT; ++index)
        if (!s_fake_slots[index].live)
            break;
    if (index == FAKE_SLOT_COUNT) {
        fake_record(FAKE_EVENT_ALLOC, -1, size, -0x103);
        return -0x103;
    }
    uid = s_next_uid++;
    s_fake_slots[index].uid = uid;
    s_fake_slots[index].size = size;
    s_fake_slots[index].live = 1;
    memset(s_fake_bytes[index], 0xa5, size);
    fake_record(FAKE_EVENT_ALLOC, uid, size, 0);
    return uid;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    fake_slot *slot;
    size_t index = 0U;

    slot = fake_find(uid, &index);
    if (s_fail_get) {
        --s_fail_get;
        fake_record(FAKE_EVENT_GET, uid, slot ? slot->size : 0U, -0x201);
        return -0x201;
    }
    if (!slot || !base) {
        fake_record(FAKE_EVENT_GET, uid, 0U, -0x202);
        return -0x202;
    }
    if (s_misalign_get) {
        --s_misalign_get;
        *base = s_fake_bytes[index] + 1U;
    }
    else {
        *base = s_fake_bytes[index];
    }
    fake_record(FAKE_EVENT_GET, uid, slot->size, 0);
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    fake_slot *slot = fake_find(uid, NULL);
    int result = 0;

    if (!slot)
        result = -0x301;
    else if (s_fail_free) {
        --s_fail_free;
        result = -0x302;
    }
    else {
        slot->live = 0;
    }
    fake_record(FAKE_EVENT_FREE, uid, slot ? slot->size : 0U, result);
    return result;
}

void *oracle_calloc(size_t count, size_t size)
{
    ++s_oracle_calloc_calls;
    return calloc(count, size);
}

void oracle_free(void *pointer)
{
    ++s_oracle_free_calls;
    free(pointer);
}

void *oracle_realloc(void *pointer, size_t size)
{
    ++s_oracle_realloc_calls;
    return realloc(pointer, size);
}

#ifndef ISAAC_VITA_HEAP_LEDGER_MEMBLOCK_LINK_PROBE

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* Sanitizer instrumentation can keep cold import-dispatch sections which the
 * optimized oracle garbage-collects.  These fail-closed stubs satisfy that
 * test-only edge; the transaction tests never enter it. */
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
    guest_fault(c, pc, "ledger oracle guest stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(
        c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
}

static size_t fake_live_count(void)
{
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < FAKE_SLOT_COUNT; ++index)
        if (s_fake_slots[index].live)
            ++count;
    return count;
}

static void fake_clear_events(void)
{
    memset(s_fake_events, 0, sizeof s_fake_events);
    s_fake_event_count = 0U;
}

static int storage_states_equal(
    const isaac_vita_guest_heap_test_storage_state *first,
    const isaac_vita_guest_heap_test_storage_state *second)
{
    return first->capacity == second->capacity &&
        first->live_count == second->live_count &&
        first->tombstones == second->tombstones &&
        first->usable_bytes == second->usable_bytes &&
        first->block_bytes == second->block_bytes &&
        first->uid == second->uid &&
        first->table_hash == second->table_hash;
}

static size_t expected_ledger_entry_bytes(void)
{
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    return sizeof(uintptr_t) + sizeof(size_t);
#else
    return sizeof(uintptr_t);
#endif
}

static size_t expected_ledger_usable_bytes(size_t capacity)
{
    size_t entry_bytes;

    if (capacity > SIZE_MAX / expected_ledger_entry_bytes())
        return SIZE_MAX;
    entry_bytes = capacity * expected_ledger_entry_bytes();
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    /* Independent model of the combined allocation: entry table, one birth
     * phase byte per slot, then one CURRENT bit per slot. */
    if ((capacity & 7U) != 0U || entry_bytes > SIZE_MAX - capacity ||
        entry_bytes + capacity > SIZE_MAX - capacity / 8U)
        return SIZE_MAX;
    return entry_bytes + capacity + capacity / 8U;
#else
    return entry_bytes;
#endif
}

static size_t expected_ledger_block_bytes(size_t usable_bytes)
{
    const size_t page = ISAAC_VITA_HEAP_LEDGER_PAGE_BYTES;
    size_t rounded;
    size_t block;

    if (!usable_bytes || usable_bytes > UINT32_MAX ||
        usable_bytes > SIZE_MAX - (page - 1U))
        return SIZE_MAX;
    rounded = (usable_bytes + page - 1U) & ~(page - 1U);
    if (rounded > UINT32_MAX - page)
        return SIZE_MAX;
    block = rounded + page;
    if ((block & (~block + 1U)) != page) {
        if (block > UINT32_MAX - page)
            return SIZE_MAX;
        block += page;
    }
    return (block & (~block + 1U)) == page ? block : SIZE_MAX;
}

static int reset_heap_and_fake(void)
{
    size_t index;

    s_fail_alloc = 0U;
    s_fail_get = 0U;
    s_misalign_get = 0U;
    s_fail_free = 0U;
    if (!isaac_vita_guest_heap_test_storage_reset())
        return 0;
    for (index = 0U; index < FAKE_SLOT_COUNT; ++index)
        if (s_fake_slots[index].live)
            return 0;
    fake_clear_events();
    s_oracle_calloc_calls = 0U;
    s_oracle_free_calls = 0U;
    s_oracle_realloc_calls = 0U;
    return 1;
}

static int test_layout_and_storage(void)
{
    isaac_vita_heap_ledger_storage storage;
    size_t block;
    size_t index;

    CHECK(!isaac_vita_heap_ledger_memblock_layout(0U, &block));
    CHECK(isaac_vita_heap_ledger_memblock_layout(1U, &block) &&
          block == 0x3000U);
    CHECK(isaac_vita_heap_ledger_memblock_layout(0x1000U, &block) &&
          block == 0x3000U);
    CHECK(isaac_vita_heap_ledger_memblock_layout(0x1001U, &block) &&
          block == 0x3000U);
    CHECK(isaac_vita_heap_ledger_memblock_layout(0x2000U, &block) &&
          block == 0x3000U);
    CHECK(isaac_vita_heap_ledger_memblock_layout(0x400000U, &block) &&
          block == 0x401000U &&
          (block & (~block + 1U)) == 0x1000U);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(expected_ledger_usable_bytes(64U) ==
              64U * expected_ledger_entry_bytes() + 64U + 8U);
#endif
    CHECK(!isaac_vita_heap_ledger_memblock_layout(
              (size_t)UINT32_MAX, &block));

    CHECK(isaac_vita_heap_ledger_memblock_acquire(0x1000U, &storage) ==
          ISAAC_VITA_HEAP_LEDGER_STORAGE_READY);
    CHECK(storage.base && storage.uid >= 0 && storage.usable_bytes == 0x1000U &&
          storage.block_bytes == 0x3000U);
    for (index = 0U; index < storage.block_bytes; ++index)
        CHECK(((const unsigned char *)storage.base)[index] == 0U);
    CHECK(isaac_vita_heap_ledger_memblock_release(&storage) == 0);

    s_fail_alloc = 1U;
    CHECK(isaac_vita_heap_ledger_memblock_acquire(0x1000U, &storage) ==
          ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED && storage.uid < 0);
    s_fail_get = 1U;
    CHECK(isaac_vita_heap_ledger_memblock_acquire(0x1000U, &storage) ==
          ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED && storage.uid < 0 &&
          fake_live_count() == 0U);
    s_misalign_get = 1U;
    CHECK(isaac_vita_heap_ledger_memblock_acquire(0x1000U, &storage) ==
          ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED && storage.uid < 0 &&
          fake_live_count() == 0U);
    s_fail_get = 1U;
    s_fail_free = 1U;
    CHECK(isaac_vita_heap_ledger_memblock_acquire(0x1000U, &storage) ==
          ISAAC_VITA_HEAP_LEDGER_STORAGE_ORPHANED && storage.uid >= 0 &&
          fake_live_count() == 1U);
    CHECK(isaac_vita_heap_ledger_memblock_release(&storage) == 0 &&
          fake_live_count() == 0U);
    fake_clear_events();
    return 0;
}

static int fill_live(void **pointers, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        pointers[index] = isaac_vita_guest_malloc(16U + index);
        if (!pointers[index])
            return 0;
    }
    return 1;
}

static int free_live(void **pointers, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index)
        if (!isaac_vita_guest_free(pointers[index]))
            return 0;
    return 1;
}

static int test_initial_failures(void)
{
    isaac_vita_guest_heap_test_storage_state state;

    CHECK(reset_heap_and_fake());
    s_fail_alloc = 1U;
    CHECK(isaac_vita_guest_malloc(32U) == NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&state) &&
          state.capacity == 0U && state.uid < 0 && !state.poisoned &&
          fake_live_count() == 0U && s_oracle_calloc_calls == 0U &&
          s_oracle_free_calls == 0U);

    CHECK(reset_heap_and_fake());
    s_fail_get = 1U;
    CHECK(isaac_vita_guest_malloc(32U) == NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&state) &&
          state.capacity == 0U && state.uid < 0 && !state.poisoned &&
          fake_live_count() == 0U);

    CHECK(reset_heap_and_fake());
    s_fail_get = 1U;
    s_fail_free = 1U;
    CHECK(isaac_vita_guest_malloc(32U) == NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&state) &&
          state.capacity == 0U && state.uid < 0 && state.poisoned &&
          state.orphan_uid >= 0 && fake_live_count() == 1U);
    fake_clear_events();
    CHECK(isaac_vita_guest_malloc(32U) == NULL && s_fake_event_count == 0U);
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_successful_growth_and_compaction(void)
{
    isaac_vita_guest_heap_test_storage_state before;
    isaac_vita_guest_heap_test_storage_state after;
    void *pointers[33];
    void *trigger;
    size_t calloc_before;
    size_t free_before;
    int backshift = isaac_vita_guest_heap_test_backshift_enabled();
    size_t threshold_live = backshift ? 32U : 31U;

    CHECK(reset_heap_and_fake());
    CHECK(fill_live(pointers, threshold_live));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before) &&
          before.capacity == 64U && before.live_count == threshold_live &&
          before.usable_bytes == expected_ledger_usable_bytes(64U) &&
          before.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(64U)) &&
          isaac_vita_guest_heap_test_next_rehash_bytes() ==
              expected_ledger_usable_bytes(128U));
    calloc_before = s_oracle_calloc_calls;
    free_before = s_oracle_free_calls;
    trigger = isaac_vita_guest_malloc(64U);
    CHECK(trigger != NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          after.capacity == 128U &&
          after.live_count == threshold_live + 1U &&
          after.tombstones == 0U && after.uid != before.uid &&
          after.usable_bytes == expected_ledger_usable_bytes(128U) &&
          after.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(128U)) &&
          s_oracle_calloc_calls == calloc_before &&
          s_oracle_free_calls == free_before && fake_live_count() == 1U);
    pointers[threshold_live] = trigger;
    CHECK(free_live(pointers, threshold_live + 1U));
    CHECK(reset_heap_and_fake());

    CHECK(fill_live(pointers, threshold_live));
    CHECK(free_live(pointers, 21U));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before) &&
          before.capacity == 64U &&
          before.live_count == threshold_live - 21U &&
          before.tombstones == (backshift ? 0U : 21U) &&
          before.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(64U)) &&
          isaac_vita_guest_heap_test_next_rehash_bytes() ==
              (backshift ? 0U : expected_ledger_usable_bytes(64U)));
    calloc_before = s_oracle_calloc_calls;
    free_before = s_oracle_free_calls;
    trigger = isaac_vita_guest_malloc(96U);
    CHECK(trigger != NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          after.capacity == 64U &&
          after.live_count == threshold_live - 20U &&
          after.tombstones == 0U &&
          (backshift ? after.uid == before.uid : after.uid != before.uid) &&
          after.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(64U)) &&
          isaac_vita_guest_heap_test_next_rehash_bytes() == 0U &&
          s_oracle_calloc_calls == calloc_before &&
          s_oracle_free_calls == free_before && fake_live_count() == 1U);
    for (calloc_before = 21U;
         calloc_before < threshold_live;
         ++calloc_before)
        CHECK(isaac_vita_guest_heap_owns(pointers[calloc_before]));
    CHECK(isaac_vita_guest_free(trigger));
    CHECK(free_live(&pointers[21], threshold_live - 21U));
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_existing_candidate_failure_case(
    unsigned fail_alloc, unsigned fail_get, unsigned misalign_get,
    unsigned fail_free, int expect_orphan)
{
    isaac_vita_guest_heap_test_storage_state before;
    isaac_vita_guest_heap_test_storage_state after;
    void *pointers[32];
    size_t threshold_live =
        isaac_vita_guest_heap_test_backshift_enabled() ? 32U : 31U;

    CHECK(reset_heap_and_fake());
    CHECK(fill_live(pointers, threshold_live));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before) &&
          before.capacity == 64U && before.live_count == threshold_live &&
          before.tombstones == 0U && before.uid >= 0 &&
          before.orphan_uid < 0 && !before.poisoned &&
          before.usable_bytes == expected_ledger_usable_bytes(64U) &&
          before.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(64U)) &&
          fake_live_count() == 1U);

    fake_clear_events();
    s_fail_alloc = fail_alloc;
    s_fail_get = fail_get;
    s_misalign_get = misalign_get;
    s_fail_free = fail_free;
    CHECK(isaac_vita_guest_malloc(128U) == NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          storage_states_equal(&before, &after));

    if (fail_alloc) {
        CHECK(s_fake_event_count == 1U &&
              s_fake_events[0].kind == FAKE_EVENT_ALLOC &&
              s_fake_events[0].result < 0 && !after.poisoned &&
              after.orphan_uid < 0 && fake_live_count() == 1U);
    }
    else {
        CHECK(s_fake_event_count == 3U &&
              s_fake_events[0].kind == FAKE_EVENT_ALLOC &&
              s_fake_events[0].result == 0 &&
              s_fake_events[1].kind == FAKE_EVENT_GET &&
              s_fake_events[2].kind == FAKE_EVENT_FREE);
        if (fail_get)
            CHECK(s_fake_events[1].result < 0);
        else
            CHECK(misalign_get && s_fake_events[1].result == 0);
        if (expect_orphan) {
            CHECK(s_fake_events[2].result < 0 && after.poisoned &&
                  after.orphan_uid == s_fake_events[0].uid &&
                  fake_live_count() == 2U);
        }
        else {
            CHECK(s_fake_events[2].result == 0 && !after.poisoned &&
                  after.orphan_uid < 0 && fake_live_count() == 1U);
        }
    }

    CHECK(free_live(pointers, threshold_live));
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_existing_candidate_failures(void)
{
    /* No candidate UID, clean rollback after get/base validation, then both
     * ways an acquired candidate can become the single fail-closed orphan. */
    CHECK(test_existing_candidate_failure_case(1U, 0U, 0U, 0U, 0) == 0);
    CHECK(test_existing_candidate_failure_case(0U, 1U, 0U, 0U, 0) == 0);
    CHECK(test_existing_candidate_failure_case(0U, 0U, 1U, 0U, 0) == 0);
    CHECK(test_existing_candidate_failure_case(0U, 1U, 0U, 1U, 1) == 0);
    CHECK(test_existing_candidate_failure_case(0U, 0U, 1U, 1U, 1) == 0);
    return 0;
}

static int test_physical_memblock_growth(void)
{
    isaac_vita_guest_heap_test_storage_state before;
    isaac_vita_guest_heap_test_storage_state after;
    void *pointers[513];
    size_t trigger_count;
    size_t expected_capacity;
    int backshift = isaac_vita_guest_heap_test_backshift_enabled();

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    trigger_count = backshift ? 257U : 256U;
    expected_capacity = 1024U;
#else
    trigger_count = backshift ? 513U : 512U;
    expected_capacity = 2048U;
#endif
    CHECK(reset_heap_and_fake());
    CHECK(fill_live(pointers, trigger_count - 1U));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before) &&
          before.capacity == expected_capacity / 2U &&
          before.live_count == trigger_count - 1U &&
          before.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(expected_capacity / 2U)) &&
          fake_live_count() == 1U);
    pointers[trigger_count - 1U] = isaac_vita_guest_malloc(777U);
    CHECK(pointers[trigger_count - 1U] != NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          after.capacity == expected_capacity &&
          after.live_count == trigger_count && after.tombstones == 0U &&
          after.uid != before.uid &&
          after.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(expected_capacity)) &&
          after.usable_bytes ==
              expected_ledger_usable_bytes(expected_capacity) &&
          fake_live_count() == 1U);
    CHECK(free_live(pointers, trigger_count));
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_old_authority_failures(void)
{
    isaac_vita_guest_heap_test_storage_state before;
    isaac_vita_guest_heap_test_storage_state after;
    void *pointers[32];
    size_t events_before;
    size_t threshold_live =
        isaac_vita_guest_heap_test_backshift_enabled() ? 32U : 31U;

    CHECK(reset_heap_and_fake());
    CHECK(fill_live(pointers, threshold_live));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before));
    events_before = s_fake_event_count;
    s_fail_free = 1U;
    CHECK(isaac_vita_guest_malloc(128U) == NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          storage_states_equal(&before, &after) && !after.poisoned &&
          after.orphan_uid < 0 && fake_live_count() == 1U);
    CHECK(s_fake_event_count == events_before + 4U &&
          s_fake_events[events_before].kind == FAKE_EVENT_ALLOC &&
          s_fake_events[events_before + 1U].kind == FAKE_EVENT_GET &&
          s_fake_events[events_before + 2U].kind == FAKE_EVENT_FREE &&
          s_fake_events[events_before + 2U].uid == before.uid &&
          s_fake_events[events_before + 2U].result < 0 &&
          s_fake_events[events_before + 3U].kind == FAKE_EVENT_FREE &&
          s_fake_events[events_before + 3U].uid != before.uid &&
          s_fake_events[events_before + 3U].result == 0);
    CHECK(free_live(pointers, threshold_live));
    CHECK(reset_heap_and_fake());

    CHECK(fill_live(pointers, threshold_live));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before));
    events_before = s_fake_event_count;
    s_fail_free = 2U;
    CHECK(isaac_vita_guest_malloc(128U) == NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          storage_states_equal(&before, &after) && after.poisoned &&
          after.orphan_uid >= 0 && after.orphan_uid != before.uid &&
          fake_live_count() == 2U);
    CHECK(s_fake_event_count == events_before + 4U &&
          s_fake_events[events_before + 2U].uid == before.uid &&
          s_fake_events[events_before + 2U].result < 0 &&
          s_fake_events[events_before + 3U].uid == after.orphan_uid &&
          s_fake_events[events_before + 3U].result < 0);
    fake_clear_events();
    CHECK(isaac_vita_guest_malloc(128U) == NULL && s_fake_event_count == 0U);
    CHECK(free_live(pointers, threshold_live));
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_moved_realloc_transaction(void)
{
    isaac_vita_guest_heap_test_storage_state state;
    void *pointer;
    void *moved;
    void *retired = NULL;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    uintptr_t base = 0U;
    uint32_t token;
#endif

    CHECK(reset_heap_and_fake());
    pointer = isaac_vita_guest_malloc(73U);
    CHECK(pointer != NULL);
    fake_clear_events();
    moved = isaac_vita_guest_heap_test_force_move(
        pointer, 257U, &retired);
    CHECK(moved != NULL && retired == pointer && moved != pointer &&
          !isaac_vita_guest_heap_owns(pointer) &&
          isaac_vita_guest_heap_owns(moved) && s_fake_event_count == 0U &&
          isaac_vita_guest_heap_test_realloc_moves() == 1U);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    token = isaac_vita_guest_heap_lease_containing(
        (unsigned char *)moved + 256U, 1U, &base);
    CHECK(token != 0U && base == (uintptr_t)moved &&
          isaac_vita_guest_heap_lease_release(token) == 1);
    CHECK(isaac_vita_guest_heap_lease_containing(
              (unsigned char *)moved + 256U, 2U, &base) == 0U);
#endif
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&state) &&
          state.live_count == 1U &&
          (!isaac_vita_guest_heap_test_backshift_enabled() ||
           state.tombstones == 0U));
    CHECK(isaac_vita_guest_free(moved));
    oracle_free(retired);
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_backshift_churn_has_no_replacement(void)
{
    enum { LIVE = 16, ITERATIONS = 4096 };
    isaac_vita_guest_heap_test_storage_state before;
    isaac_vita_guest_heap_test_storage_state after;
    void *pointers[LIVE];
    size_t iteration;
    size_t slot;

    if (!isaac_vita_guest_heap_test_backshift_enabled())
        return 0;
    CHECK(reset_heap_and_fake());
    CHECK(fill_live(pointers, LIVE));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before) &&
          before.capacity == 64U && before.live_count == LIVE &&
          before.tombstones == 0U && before.uid >= 0);
    fake_clear_events();
    for (iteration = 0U; iteration < ITERATIONS; ++iteration) {
        void *replacement = isaac_vita_guest_malloc(41U + iteration % 97U);

        CHECK(replacement != NULL);
        slot = iteration % LIVE;
        CHECK(isaac_vita_guest_free(pointers[slot]));
        pointers[slot] = replacement;
    }
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          after.capacity == before.capacity && after.uid == before.uid &&
          after.live_count == LIVE && after.tombstones == 0U &&
          s_fake_event_count == 0U);
    CHECK(free_live(pointers, LIVE));
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_backshift_half_full_moved_realloc(void)
{
    isaac_vita_guest_heap_test_storage_state before;
    isaac_vita_guest_heap_test_storage_state after;
    void *pointers[32];
    void *retired = NULL;
    void *moved;

    if (!isaac_vita_guest_heap_test_backshift_enabled())
        return 0;
    CHECK(reset_heap_and_fake());
    CHECK(fill_live(pointers, 32U));
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&before) &&
          before.capacity == 64U && before.live_count == 32U &&
          before.tombstones == 0U);
    fake_clear_events();
    moved = isaac_vita_guest_heap_test_force_move(
        pointers[9], 601U, &retired);
    CHECK(moved != NULL && retired == pointers[9] &&
          !isaac_vita_guest_heap_owns(retired) &&
          isaac_vita_guest_heap_owns(moved));
    pointers[9] = moved;
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&after) &&
          after.capacity == before.capacity && after.uid == before.uid &&
          after.live_count == before.live_count && after.tombstones == 0U &&
          s_fake_event_count == 0U &&
          isaac_vita_guest_heap_test_next_rehash_bytes() ==
              expected_ledger_usable_bytes(128U));
    oracle_free(retired);
    CHECK(free_live(pointers, 32U));
    CHECK(reset_heap_and_fake());
    return 0;
}

static int test_backshift_no_empty_preflight(void)
{
    void *pointer;
    void *resized;
    size_t free_before;
    size_t realloc_before;
    int valid_owner;

    if (!isaac_vita_guest_heap_test_backshift_enabled())
        return 0;
    CHECK(reset_heap_and_fake());
    pointer = isaac_vita_guest_malloc(83U);
    CHECK(pointer != NULL &&
          isaac_vita_guest_heap_test_corrupt_no_empty());
    free_before = s_oracle_free_calls;
    realloc_before = s_oracle_realloc_calls;

    CHECK(!isaac_vita_guest_free(pointer) &&
          s_oracle_free_calls == free_before &&
          isaac_vita_guest_heap_owns(pointer));
    valid_owner = 0;
    resized = isaac_vita_guest_realloc(pointer, 0U, &valid_owner);
    CHECK(resized == NULL && valid_owner == 1 &&
          s_oracle_free_calls == free_before &&
          s_oracle_realloc_calls == realloc_before &&
          isaac_vita_guest_heap_owns(pointer));
    valid_owner = 0;
    resized = isaac_vita_guest_realloc(pointer, 211U, &valid_owner);
    CHECK(resized == NULL && valid_owner == 1 &&
          s_oracle_free_calls == free_before &&
          s_oracle_realloc_calls == realloc_before &&
          isaac_vita_guest_heap_owns(pointer));

    CHECK(isaac_vita_guest_heap_test_restore_empty() &&
          isaac_vita_guest_free(pointer) &&
          s_oracle_free_calls == free_before + 1U);
    CHECK(reset_heap_and_fake());
    return 0;
}

int main(void)
{
    void *pointer;
    isaac_vita_guest_heap_test_storage_state state;

    CHECK(test_layout_and_storage() == 0);
    CHECK(test_initial_failures() == 0);

    CHECK(reset_heap_and_fake());
    CHECK(isaac_vita_guest_heap_test_next_rehash_bytes() ==
          expected_ledger_usable_bytes(64U));
    pointer = isaac_vita_guest_malloc(32U);
    CHECK(pointer != NULL);
    CHECK(isaac_vita_guest_heap_test_storage_snapshot(&state) &&
          state.capacity == 64U && state.live_count == 1U &&
          state.uid >= 0 && state.orphan_uid < 0 && !state.poisoned &&
          state.usable_bytes == expected_ledger_usable_bytes(64U) &&
          state.block_bytes == expected_ledger_block_bytes(
              expected_ledger_usable_bytes(64U)) &&
          s_oracle_calloc_calls == 0U &&
          s_oracle_free_calls == 0U);
    CHECK(isaac_vita_guest_free(pointer));
    CHECK(reset_heap_and_fake());

    CHECK(test_successful_growth_and_compaction() == 0);
    CHECK(test_existing_candidate_failures() == 0);
    CHECK(test_old_authority_failures() == 0);
    CHECK(test_physical_memblock_growth() == 0);
    CHECK(isaac_vita_guest_heap_test_backshift_oracle());
    CHECK(test_moved_realloc_transaction() == 0);
    CHECK(test_backshift_churn_has_no_replacement() == 0);
    CHECK(test_backshift_half_full_moved_realloc() == 0);
    CHECK(test_backshift_no_empty_preflight() == 0);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    puts("Vita heap ledger USER_RW transaction oracle (range lease): PASS");
#else
    puts("Vita heap ledger USER_RW transaction oracle (exact base): PASS");
#endif
    return 0;
}
#elif defined(ISAAC_VITA_HEAP_LEDGER_MEMBLOCK_ARM_LINK_MAIN)
/* The full ARM range-lease oracle supplies these itself.  The default
 * no-range variant needs a tiny runnable owner so its complete memblock
 * object graph is linked and ABI-inspected too. */
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
    guest_fault(c, pc, "ledger ARM link guest stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    return guest_stack_violation(
        c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
}

int main(void)
{
    isaac_vita_guest_heap_test_storage_state state;
    void *pointer = isaac_vita_guest_malloc(32U);

    if (!pointer || !isaac_vita_guest_heap_owns(pointer) ||
        !isaac_vita_guest_heap_test_storage_snapshot(&state) ||
        state.capacity != 64U || state.live_count != 1U ||
        state.usable_bytes != 64U * sizeof(uintptr_t) ||
        state.block_bytes != 0x3000U ||
        !isaac_vita_guest_heap_test_bounded_probe() ||
        !isaac_vita_guest_heap_test_backshift_oracle() ||
        !isaac_vita_guest_free(pointer) ||
        !isaac_vita_guest_heap_test_storage_reset())
        return 1;
    return 0;
}
#endif
