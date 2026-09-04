#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_heap_overflow_mspace.h"
#include "host_vita_room_entry_slab.h"

_Alignas(8)
static unsigned char
s_backing[ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES];
static unsigned char s_page_live[ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES];
static isaac_vita_heap_overflow_mspace_state s_raw_state =
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
static uint32_t s_raw_internal_live;
static uint32_t s_raw_internal_bytes;
static uint32_t s_raw_malloc_calls;
static uint32_t s_raw_free_calls;
static uint32_t s_fail_raw_malloc;
static uint32_t s_fail_raw_free;
static uint32_t s_log_calls;
static uint32_t s_log_under_lock;
static size_t s_log_max_bytes;
static int s_lock_held;
static uintptr_t s_leased_base;
static char s_logs[32][384];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void isaac_vita_log(const char *format, ...)
{
    char scratch[384];
    char *destination = scratch;
    va_list arguments;
    int length;

    if (s_lock_held)
        ++s_log_under_lock;
    if (s_log_calls < sizeof s_logs / sizeof s_logs[0])
        destination = s_logs[s_log_calls];
    va_start(arguments, format);
    length = vsnprintf(destination, sizeof scratch, format, arguments);
    va_end(arguments);
    if (length >= 0 && (size_t)length > s_log_max_bytes)
        s_log_max_bytes = (size_t)length;
    ++s_log_calls;
}

int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot)
{
    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->state = s_raw_state;
    snapshot->uid = 1;
    snapshot->base = (uintptr_t)s_backing;
    snapshot->end = (uintptr_t)s_backing + sizeof s_backing;
    snapshot->retained_end = snapshot->end;
    snapshot->capacity_bytes = sizeof s_backing;
    snapshot->retained_bytes = sizeof s_backing;
    snapshot->live_count = s_raw_internal_live;
    snapshot->internal_live_count = s_raw_internal_live;
    snapshot->internal_requested_bytes = s_raw_internal_bytes;
    snapshot->has_mspace = 1;
    return 1;
}

void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void)
{
    uint32_t index;

    ++s_raw_malloc_calls;
    if (s_raw_state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        s_fail_raw_malloc) {
        if (s_fail_raw_malloc)
            --s_fail_raw_malloc;
        return NULL;
    }
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES; ++index) {
        uint32_t candidate =
            ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES - 1U - index;

        if (!s_page_live[candidate]) {
            unsigned char *page = s_backing +
                candidate * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;

            s_page_live[candidate] = 1U;
            ++s_raw_internal_live;
            s_raw_internal_bytes +=
                ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
            memset(page, 0xa5, ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
            return page;
        }
    }
    return NULL;
}

int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer)
{
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t begin = (uintptr_t)s_backing;
    uintptr_t offset;
    uint32_t index;

    ++s_raw_free_calls;
    if (s_fail_raw_free) {
        --s_fail_raw_free;
        return 0;
    }
    if (value < begin || value >= begin + sizeof s_backing)
        return 0;
    offset = value - begin;
    if (offset % ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
        return 0;
    index = (uint32_t)(offset /
        ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
    if (index >= ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES ||
        !s_page_live[index] || !s_raw_internal_live ||
        s_raw_internal_bytes < ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
        return 0;
    s_page_live[index] = 0U;
    --s_raw_internal_live;
    s_raw_internal_bytes -= ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    return 1;
}

static int oracle_is_leased(uintptr_t base)
{
    return base == s_leased_base;
}

static isaac_vita_room_entry_slab_result slab_malloc(
    uint32_t owner, size_t size, void **pointer_out,
    isaac_vita_room_entry_slab_event *event)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_result result;

    isaac_vita_room_entry_slab_event_init(event);
    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_malloc_locked(
        owner, size, &decision, event);
    s_lock_held = 0;
    if (pointer_out)
        *pointer_out = decision.pointer;
    isaac_vita_room_entry_slab_log_event(event);
    return result;
}

static isaac_vita_room_entry_slab_result slab_free(
    void *pointer, uint32_t return_rva,
    isaac_vita_room_entry_slab_decision *decision)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_result result;

    isaac_vita_room_entry_slab_event_init(&event);
    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_free_locked(
        pointer, return_rva, NULL, decision, &event);
    s_lock_held = 0;
    isaac_vita_room_entry_slab_log_event(&event);
    return result;
}

static int reset_slab(void)
{
    int result;

    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_test_reset_locked();
    s_lock_held = 0;
    return result;
}

static int clean_state(void)
{
    uint32_t index;

    if (!reset_slab())
        return 0;
    if (s_raw_internal_live || s_raw_internal_bytes)
        return 0;
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES; ++index)
        if (s_page_live[index])
            return 0;
    s_raw_state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    s_raw_malloc_calls = 0U;
    s_raw_free_calls = 0U;
    s_fail_raw_malloc = 0U;
    s_fail_raw_free = 0U;
    s_log_calls = 0U;
    s_log_under_lock = 0U;
    s_log_max_bytes = 0U;
    s_leased_base = 0U;
    memset(s_logs, 0, sizeof s_logs);
    return 1;
}

static int test_exact_allocation_and_free(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_snapshot snapshot;
    uintptr_t containing = 0U;
    unsigned char *pointer = NULL;
    size_t index;

    CHECK(clean_state());
    CHECK(slab_malloc(0x11111111U, 12U, (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED && !pointer &&
          s_raw_malloc_calls == 0U);
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 11U,
                      (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED && !pointer &&
          s_raw_malloc_calls == 0U);
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer &&
          ((uintptr_t)pointer & 7U) == 0U && s_raw_malloc_calls == 1U);
    /* operator new[] is uninitialized storage; only bitmap/control bytes are
     * cleared when a page is published. */
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES; ++index)
        CHECK(pointer[index] == 0xa5U);
    CHECK(isaac_vita_room_entry_slab_owns_exact_locked(pointer));
    CHECK(!isaac_vita_room_entry_slab_owns_exact_locked(pointer + 1U));
    CHECK(isaac_vita_room_entry_slab_find_containing_locked(
              pointer + 7U, 5U, &containing) && containing ==
                  (uintptr_t)pointer);
    CHECK(!isaac_vita_room_entry_slab_find_containing_locked(
              pointer + 7U, 6U, &containing));
    CHECK(!isaac_vita_room_entry_slab_find_containing_locked(
              pointer + 12U, 1U, &containing));
    CHECK(slab_free(pointer + 1U,
                    ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
                    &decision) == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED &&
          decision.fault &&
          isaac_vita_room_entry_slab_owns_exact_locked(pointer));
    CHECK(slab_free(pointer,
                    ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
                    &decision) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          !isaac_vita_room_entry_slab_owns_exact_locked(pointer));
    CHECK(slab_free(pointer,
                    ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
                    &decision) == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED);
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.pages == 1U && snapshot.live_slots == 0U &&
          snapshot.allocations == 1U && snapshot.frees == 1U &&
          snapshot.known_normal_frees == 1U && snapshot.rejected == 2U &&
          snapshot.raw_internal_live == 1U &&
          snapshot.raw_internal_requested ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
    CHECK(reset_slab() && s_raw_internal_live == 0U);
    return 0;
}

static int test_realloc_transaction(void)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot snapshot;
    unsigned char *pointer = NULL;
    uintptr_t containing;

    CHECK(clean_state());
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    pointer[0] = 0x31U;
    isaac_vita_room_entry_slab_event_init(&event);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 12U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          decision.pointer == pointer);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 8U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE);
    CHECK(!isaac_vita_room_entry_slab_find_containing_locked(
              pointer, 1U, &containing));
    CHECK(slab_free(pointer,
                    ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
                    &decision) == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED);
    CHECK(isaac_vita_room_entry_slab_realloc_cancel_locked(pointer, &event));
    CHECK(pointer[0] == 0x31U &&
          isaac_vita_room_entry_slab_owns_exact_locked(pointer));
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.live_slots == 1U && snapshot.moving_slots == 0U &&
          snapshot.move_attempts == 1U && snapshot.moves == 0U);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 8U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE);
    CHECK(isaac_vita_room_entry_slab_realloc_commit_locked(pointer, &event));
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.live_slots == 0U && snapshot.move_attempts == 2U &&
          snapshot.moves == 1U && snapshot.frees == 1U);
    CHECK(reset_slab());

    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 0U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && !decision.pointer);
    CHECK(reset_slab());
    return 0;
}

static int test_range_lease_rejection(void)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_event event;
    void *pointer = NULL;

    CHECK(clean_state());
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    s_leased_base = (uintptr_t)pointer;
    isaac_vita_room_entry_slab_event_init(&event);
    CHECK(isaac_vita_room_entry_slab_free_locked(
              pointer, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
              oracle_is_leased, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED && decision.fault &&
          isaac_vita_room_entry_slab_owns_exact_locked(pointer));
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 8U, oracle_is_leased, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED && decision.fault &&
          isaac_vita_room_entry_slab_owns_exact_locked(pointer));
    s_leased_base = 0U;
    CHECK(slab_free(pointer,
                    ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
                    NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(reset_slab());
    return 0;
}

static int test_fallback_terminal_and_retry_reset(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot snapshot;
    void *pointer = NULL;

    CHECK(clean_state());
    s_fail_raw_malloc = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK && !pointer);
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.fallbacks == 1U && !snapshot.terminal &&
          snapshot.pages == 0U);
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    CHECK(slab_free(pointer, 0x12345678U, NULL) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.unexpected_frees == 1U);
    s_fail_raw_free = 1U;
    CHECK(!reset_slab() && s_raw_internal_live == 1U);
    CHECK(reset_slab() && s_raw_internal_live == 0U);

    CHECK(clean_state());
    s_raw_state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL && !pointer);
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.terminal);
    s_raw_state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    CHECK(reset_slab());
    return 0;
}

static int test_cold_rejects_future_nonfull_bit(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot snapshot;
    void *pointer = NULL;

    CHECK(clean_state());
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    isaac_vita_room_entry_slab_test_corrupt_future_nonfull_locked();
    CHECK(!isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.terminal && snapshot.pages == 1U &&
          snapshot.live_slots == 1U);
    /* snapshot_locked is a pure observer.  Restoring the injected bit makes
     * the same live slab valid again, without changing process state. */
    isaac_vita_room_entry_slab_test_corrupt_future_nonfull_locked();
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          !snapshot.terminal);
    CHECK(slab_free(pointer,
                    ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
                    NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(reset_slab());
    return 0;
}

static int test_capacity_and_bounded_logs(void)
{
    static void *pointers[ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_SLOTS];
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot snapshot;
    uint32_t index;
    uint32_t released_index =
        47U * ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE + 123U;
    void *extra = NULL;

    CHECK(clean_state());
    for (index = 0U; index < ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_SLOTS; ++index)
        CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                          &pointers[index], &event) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointers[index]);
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.pages == ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES &&
          snapshot.live_slots == ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_SLOTS &&
          snapshot.raw_internal_live ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES &&
          snapshot.raw_internal_requested ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES);
    /* The fake raw allocator publishes high addresses first.  All later page
     * descriptors therefore exercise sorted insertion/memmove. */
    CHECK((uintptr_t)pointers[0] >
          (uintptr_t)pointers[ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE]);
    isaac_vita_room_entry_slab_test_allocation_probe_reset_locked();
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &extra, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK && !extra &&
          isaac_vita_room_entry_slab_test_allocation_probe_count_locked() ==
              0U);
    isaac_vita_room_entry_slab_test_probe_reset_locked();
    CHECK(!isaac_vita_room_entry_slab_owns_exact_locked(
              s_backing + sizeof s_backing) &&
          isaac_vita_room_entry_slab_test_probe_count_locked() <= 7U);
    isaac_vita_room_entry_slab_test_probe_reset_locked();
    CHECK(isaac_vita_room_entry_slab_owns_exact_locked(pointers[0]) &&
          isaac_vita_room_entry_slab_test_probe_count_locked() <= 7U);
    isaac_vita_room_entry_slab_test_probe_reset_locked();
    CHECK(!isaac_vita_room_entry_slab_owns_exact_locked(
              (unsigned char *)pointers[0] + 1U) &&
          isaac_vita_room_entry_slab_test_probe_count_locked() <= 7U);
    CHECK(slab_free(pointers[0],
                    ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA,
                    NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    isaac_vita_room_entry_slab_test_probe_reset_locked();
    CHECK(!isaac_vita_room_entry_slab_owns_exact_locked(pointers[0]) &&
          isaac_vita_room_entry_slab_test_probe_count_locked() <= 7U);
    CHECK(slab_free(pointers[released_index],
                    ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA,
                    NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    isaac_vita_room_entry_slab_test_allocation_probe_reset_locked();
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &extra, &event) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED &&
          extra == pointers[released_index] &&
          isaac_vita_room_entry_slab_test_allocation_probe_count_locked() ==
              1U);
    CHECK(slab_free(extra, ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA,
                    NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    for (index = 1U; index < ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_SLOTS; ++index) {
        if (index == released_index)
            continue;
        CHECK(slab_free(pointers[index],
                        ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA,
                        NULL) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    isaac_vita_room_entry_slab_event_init(&event);
    isaac_vita_room_entry_slab_claim_final_locked(&event);
    isaac_vita_room_entry_slab_log_event(&event);
    CHECK(s_log_under_lock == 0U && s_log_calls <= 12U &&
          s_log_max_bytes < sizeof s_logs[0]);
    memset(&event, 0xff, sizeof event);
    s_log_max_bytes = 0U;
    isaac_vita_room_entry_slab_log_event(&event);
    CHECK(s_log_max_bytes < sizeof s_logs[0]);
    CHECK(reset_slab() && s_raw_internal_live == 0U);
    return 0;
}

static int test_commit_failure_is_process_terminal(void)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot snapshot;
    unsigned char *pointer = NULL;
    unsigned foreign = 0U;

    CHECK(clean_state());
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              pointer, 8U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE);
    isaac_vita_room_entry_slab_test_fail_next_commit_locked();
    isaac_vita_room_entry_slab_event_init(&event);
    CHECK(!isaac_vita_room_entry_slab_realloc_commit_locked(pointer, &event));
    CHECK(isaac_vita_room_entry_slab_snapshot_locked(&snapshot) &&
          snapshot.terminal && snapshot.live_slots == 1U &&
          snapshot.moving_slots == 1U && snapshot.moves == 0U);
    /* Terminal state applies only to addresses in retained slab pages.  An
     * unrelated generic pointer must still reach the generic owner route. */
    CHECK(isaac_vita_room_entry_slab_free_locked(
              &foreign, 0U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED);
    CHECK(isaac_vita_room_entry_slab_realloc_begin_locked(
              &foreign, 8U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED);
    CHECK(isaac_vita_room_entry_slab_free_locked(
              pointer, 0U, NULL, &decision, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL);
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      (void **)&pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL);
    CHECK(!reset_slab());
    return 0;
}

int main(void)
{
    CHECK(ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES == 96U);
    CHECK(ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES == 0x00600000U);
    CHECK(ISAAC_VITA_ROOM_ENTRY_SLAB_NOMINAL_RAW_REMAINDER == 0x001d5000U);
    CHECK(test_exact_allocation_and_free() == 0);
    CHECK(test_realloc_transaction() == 0);
    CHECK(test_range_lease_rejection() == 0);
    CHECK(test_fallback_terminal_and_retry_reset() == 0);
    CHECK(test_cold_rejects_future_nonfull_bit() == 0);
    CHECK(test_capacity_and_bounded_logs() == 0);
    CHECK(test_commit_failure_is_process_terminal() == 0);
    puts("Vita RoomConfig Entry slab oracle: PASS");
    return 0;
}
