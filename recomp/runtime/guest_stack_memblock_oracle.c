/* Transaction and failure-order oracle for the dedicated USER_RW guest stack
 * memblock (guest_stack_memblock.c).  Host build against the fake sysmem in
 * recomp/vita/anm2_scratch_oracle_include; -DGUEST_STACK_MEMBLOCK_LINK_PROBE=1
 * keeps only the fake sysmem (no main) for link-only probes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "guest_stack_memblock.h"

#define FAKE_SLOT_COUNT 4U
#define FAKE_EVENT_COUNT 64U
#define FAKE_DIRTY_BYTE 0xa5U

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

/* One dirty page per slot.  acquire must never touch the block (guest.c zeroes
 * GUEST_STACK_SIZE itself), so a page-aligned address is all the fake needs;
 * any write by acquire is a sanitizer-visible global overflow. */
_Alignas(GUEST_STACK_MEMBLOCK_PAGE_BYTES)
static unsigned char s_fake_bytes[FAKE_SLOT_COUNT]
                                 [GUEST_STACK_MEMBLOCK_PAGE_BYTES];
static fake_slot s_fake_slots[FAKE_SLOT_COUNT];
static fake_event s_fake_events[FAKE_EVENT_COUNT];
static size_t s_fake_event_count;
static SceUID s_next_uid = 0x100;
static unsigned s_fail_alloc;
static unsigned s_fail_get;
static unsigned s_misalign_get;
static unsigned s_null_get;
static unsigned s_wrap_get;
static unsigned s_fail_free;
static char s_last_alloc_name[32];
static SceKernelMemBlockType s_last_alloc_type;
static int s_last_alloc_option_null;

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

    s_last_alloc_name[0] = '\0';
    if (name) {
        strncpy(s_last_alloc_name, name, sizeof s_last_alloc_name - 1U);
        s_last_alloc_name[sizeof s_last_alloc_name - 1U] = '\0';
    }
    s_last_alloc_type = type;
    s_last_alloc_option_null = option == NULL;
    if (s_fail_alloc) {
        --s_fail_alloc;
        fake_record(FAKE_EVENT_ALLOC, -1, size, -0x101);
        return -0x101;
    }
    if (!name || type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || option ||
        !size) {
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
    memset(s_fake_bytes[index], FAKE_DIRTY_BYTE, sizeof s_fake_bytes[index]);
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
        *base = s_fake_bytes[index] + 16U;
    }
    else if (s_null_get) {
        --s_null_get;
        *base = NULL;
    }
    else if (s_wrap_get) {
        /* Page-aligned top page of the address space: never dereferenced. */
        --s_wrap_get;
        *base = (void *)(UINTPTR_MAX - GUEST_STACK_MEMBLOCK_PAGE_BYTES + 1U);
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

#ifndef GUEST_STACK_MEMBLOCK_LINK_PROBE
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define USABLE (GUEST_STACK_MEMBLOCK_REQUEST_BYTES - \
                GUEST_STACK_MEMBLOCK_PAGE_BYTES)

static size_t fake_live_count(void)
{
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < FAKE_SLOT_COUNT; ++index)
        if (s_fake_slots[index].live)
            ++count;
    return count;
}

static void fake_reset(void)
{
    size_t index;

    for (index = 0U; index < FAKE_SLOT_COUNT; ++index)
        s_fake_slots[index].live = 0;
    memset(s_fake_events, 0, sizeof s_fake_events);
    s_fake_event_count = 0U;
    s_fail_alloc = 0U;
    s_fail_get = 0U;
    s_misalign_get = 0U;
    s_null_get = 0U;
    s_wrap_get = 0U;
    s_fail_free = 0U;
}

static int event_is(size_t index, int kind, SceUID uid, SceSize size,
                    int result)
{
    return index < s_fake_event_count && index < FAKE_EVENT_COUNT &&
        s_fake_events[index].kind == kind &&
        s_fake_events[index].uid == uid &&
        s_fake_events[index].size == size &&
        s_fake_events[index].result == result;
}

static int page_is_dirty(const unsigned char *page)
{
    size_t index;

    for (index = 0U; index < GUEST_STACK_MEMBLOCK_PAGE_BYTES; ++index)
        if (page[index] != FAKE_DIRTY_BYTE)
            return 0;
    return 1;
}

static int test_constants(void)
{
    const uint32_t request = GUEST_STACK_MEMBLOCK_REQUEST_BYTES;

    CHECK(GUEST_STACK_MEMBLOCK_PAGE_BYTES == 0x1000U);
    CHECK(request == 0x401000U);
    CHECK(request == 4U * 1024U * 1024U + GUEST_STACK_MEMBLOCK_PAGE_BYTES);
    CHECK((request & (~request + 1U)) == GUEST_STACK_MEMBLOCK_PAGE_BYTES);
    CHECK(GUEST_STACK_MEMBLOCK_RETAINED_BYTES == 0x402000U);
    CHECK(strcmp(GUEST_STACK_MEMBLOCK_NAME, "isaac_guest_stack") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_NONE), "none") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_READY), "ready") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_ALLOC_FAILED), "alloc-failed") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_GET_FAILED),
                 "get-base-failed") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_INVALID_BASE), "invalid-base") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_WRAP), "wrap") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(
                     GUEST_STACK_MEMBLOCK_ORPHANED), "orphaned") == 0);
    CHECK(strcmp(guest_stack_memblock_status_text(99U), "unknown") == 0);
    return 0;
}

static int test_ready_and_release(void)
{
    guest_stack_memblock_snapshot snapshot;
    SceUID uid;

    fake_reset();
    memset(&snapshot, 0xee, sizeof snapshot);
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == 1);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_READY);
    CHECK(snapshot.uid >= 0);
    CHECK(snapshot.get_result == 0 && snapshot.rollback_result == 0);
    CHECK(snapshot.request_bytes == GUEST_STACK_MEMBLOCK_REQUEST_BYTES);
    CHECK(snapshot.base == (uintptr_t)s_fake_bytes[0]);
    CHECK((snapshot.base & (GUEST_STACK_MEMBLOCK_PAGE_BYTES - 1U)) == 0U);
    CHECK(strcmp(s_last_alloc_name, GUEST_STACK_MEMBLOCK_NAME) == 0);
    CHECK(s_last_alloc_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    CHECK(s_last_alloc_option_null);
    CHECK(fake_live_count() == 1U);
    /* acquire does not zero: the page still carries the fake's dirty fill. */
    CHECK(page_is_dirty(s_fake_bytes[0]));
    uid = snapshot.uid;
    CHECK(s_fake_event_count == 2U);
    CHECK(event_is(0U, FAKE_EVENT_ALLOC, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));
    CHECK(event_is(1U, FAKE_EVENT_GET, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));

    CHECK(guest_stack_memblock_release(&snapshot) == 0);
    CHECK(fake_live_count() == 0U);
    CHECK(s_fake_event_count == 3U);
    CHECK(event_is(2U, FAKE_EVENT_FREE, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));
    /* guest_stack_free clears the owner's snapshot after the release; a
     * second release of the cleared snapshot is a no-op. */
    snapshot.uid = -1;
    snapshot.status = GUEST_STACK_MEMBLOCK_NONE;
    CHECK(guest_stack_memblock_release(&snapshot) == 0);
    CHECK(s_fake_event_count == 3U);
    CHECK(guest_stack_memblock_release(NULL) == 0);
    CHECK(s_fake_event_count == 3U);
    return 0;
}

static int test_alloc_failure(void)
{
    guest_stack_memblock_snapshot snapshot;

    fake_reset();
    s_fail_alloc = 1U;
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_ALLOC_FAILED);
    CHECK(snapshot.uid == -0x101);
    CHECK(snapshot.get_result == 0 && snapshot.rollback_result == 0);
    CHECK(snapshot.base == (uintptr_t)0U);
    CHECK(snapshot.request_bytes == GUEST_STACK_MEMBLOCK_REQUEST_BYTES);
    CHECK(fake_live_count() == 0U);
    CHECK(s_fake_event_count == 1U);
    CHECK(event_is(0U, FAKE_EVENT_ALLOC, -1,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, -0x101));
    CHECK(guest_stack_memblock_release(&snapshot) == 0);
    CHECK(s_fake_event_count == 1U);
    return 0;
}

static int test_get_failure_rolls_back(void)
{
    guest_stack_memblock_snapshot snapshot;
    SceUID uid;

    fake_reset();
    s_fail_get = 1U;
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_GET_FAILED);
    CHECK(snapshot.uid >= 0);
    CHECK(snapshot.get_result == -0x201);
    CHECK(snapshot.rollback_result == 0);
    CHECK(snapshot.base == (uintptr_t)0U);
    CHECK(fake_live_count() == 0U);
    uid = snapshot.uid;
    CHECK(s_fake_event_count == 3U);
    CHECK(event_is(0U, FAKE_EVENT_ALLOC, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));
    CHECK(event_is(1U, FAKE_EVENT_GET, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, -0x201));
    CHECK(event_is(2U, FAKE_EVENT_FREE, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));
    /* A rolled-back snapshot keeps uid/get/rollback for the receipt but
     * owns nothing: release must not touch sysmem. */
    CHECK(guest_stack_memblock_release(&snapshot) == 0);
    CHECK(s_fake_event_count == 3U);
    return 0;
}

static int test_invalid_base_rolls_back(void)
{
    guest_stack_memblock_snapshot snapshot;

    fake_reset();
    s_misalign_get = 1U;
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_INVALID_BASE);
    CHECK(snapshot.uid >= 0 && snapshot.get_result == 0);
    CHECK(snapshot.base == (uintptr_t)s_fake_bytes[0] + 16U);
    CHECK(snapshot.rollback_result == 0);
    CHECK(fake_live_count() == 0U);
    CHECK(s_fake_event_count == 3U);
    CHECK(event_is(2U, FAKE_EVENT_FREE, snapshot.uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));

    fake_reset();
    s_null_get = 1U;
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_INVALID_BASE);
    CHECK(snapshot.base == (uintptr_t)0U);
    CHECK(fake_live_count() == 0U);
    CHECK(s_fake_event_count == 3U);
    return 0;
}

static int test_wrap_rolls_back(void)
{
    guest_stack_memblock_snapshot snapshot;

    fake_reset();
    s_wrap_get = 1U;
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_WRAP);
    CHECK(snapshot.uid >= 0 && snapshot.get_result == 0);
    CHECK(snapshot.base ==
          UINTPTR_MAX - GUEST_STACK_MEMBLOCK_PAGE_BYTES + 1U);
    CHECK(snapshot.rollback_result == 0);
    CHECK(fake_live_count() == 0U);
    CHECK(s_fake_event_count == 3U);
    CHECK(event_is(2U, FAKE_EVENT_FREE, snapshot.uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));
    return 0;
}

static int test_orphaned_keeps_uid(void)
{
    guest_stack_memblock_snapshot snapshot;
    SceUID uid;

    fake_reset();
    s_fail_get = 1U;
    s_fail_free = 1U;
    CHECK(guest_stack_memblock_acquire(USABLE, &snapshot) == -1);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_ORPHANED);
    CHECK(snapshot.uid >= 0);
    CHECK(snapshot.get_result == -0x201);
    CHECK(snapshot.rollback_result == -0x302);
    CHECK(fake_live_count() == 1U);
    uid = snapshot.uid;
    CHECK(s_fake_event_count == 3U);
    CHECK(event_is(2U, FAKE_EVENT_FREE, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, -0x302));
    /* The orphaned UID stays owned; a later release retries the free. */
    CHECK(guest_stack_memblock_release(&snapshot) == 0);
    CHECK(fake_live_count() == 0U);
    CHECK(s_fake_event_count == 4U);
    CHECK(event_is(3U, FAKE_EVENT_FREE, uid,
                   GUEST_STACK_MEMBLOCK_REQUEST_BYTES, 0));
    return 0;
}

static int test_contract_rejections(void)
{
    guest_stack_memblock_snapshot snapshot;

    fake_reset();
    CHECK(guest_stack_memblock_acquire(USABLE, NULL) == 0);
    CHECK(s_fake_event_count == 0U);
    CHECK(guest_stack_memblock_acquire(USABLE - 0x1000U, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_ALLOC_FAILED);
    CHECK(snapshot.uid == -1);
    CHECK(snapshot.get_result == 0 && snapshot.rollback_result == 0);
    CHECK(snapshot.base == (uintptr_t)0U);
    CHECK(snapshot.request_bytes == GUEST_STACK_MEMBLOCK_REQUEST_BYTES);
    CHECK(s_fake_event_count == 0U);
    CHECK(guest_stack_memblock_acquire(
              GUEST_STACK_MEMBLOCK_REQUEST_BYTES, &snapshot) == 0);
    CHECK(snapshot.status == GUEST_STACK_MEMBLOCK_ALLOC_FAILED);
    CHECK(snapshot.uid == -1);
    CHECK(s_fake_event_count == 0U);
    CHECK(guest_stack_memblock_acquire(0U, &snapshot) == 0);
    CHECK(s_fake_event_count == 0U);
    CHECK(guest_stack_memblock_release(&snapshot) == 0);
    CHECK(s_fake_event_count == 0U);
    CHECK(fake_live_count() == 0U);
    return 0;
}

int main(void)
{
    CHECK(test_constants() == 0);
    CHECK(test_ready_and_release() == 0);
    CHECK(test_alloc_failure() == 0);
    CHECK(test_get_failure_rolls_back() == 0);
    CHECK(test_invalid_base_rolls_back() == 0);
    CHECK(test_wrap_rolls_back() == 0);
    CHECK(test_orphaned_keeps_uid() == 0);
    CHECK(test_contract_rejections() == 0);
    CHECK(fake_live_count() == 0U);
    puts("guest stack memblock oracle: PASS");
    return 0;
}
#endif
