#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_heap_overflow_mspace.h"
#include "host_vita_room_entry_external.h"
#include "host_vita_room_entry_slab.h"

#define RAW_BYTES ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES
#define EXT_ARENA_BYTES \
    (ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS * \
         ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES + \
     ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES)
#define MAX_POINTERS ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_SLOTS
#define MAX_SEQUENCE 512U

typedef struct fake_external_receipt {
    SceUID uid;
    unsigned char *base;
    int live;
} fake_external_receipt;

_Alignas(8) static unsigned char s_raw_backing[RAW_BYTES];
_Alignas(4096) static unsigned char s_external_arena[EXT_ARENA_BYTES];
static unsigned char s_raw_page_live[
    ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES];
static void *s_pointers[MAX_POINTERS];
static fake_external_receipt s_external_receipts[
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS];
static SceUID s_external_free_uids[MAX_SEQUENCE];
static unsigned char s_free_sequence[MAX_SEQUENCE];

static uint32_t s_raw_success_limit;
static uint32_t s_raw_successes;
static uint32_t s_raw_live;
static uint32_t s_raw_malloc_calls;
static uint32_t s_raw_free_calls;
static uint32_t s_external_successes;
static uint32_t s_external_alloc_calls;
static uint32_t s_external_get_base_calls;
static uint32_t s_external_free_calls;
static uint32_t s_external_oom_remaining;
static uint32_t s_external_get_base_fail_remaining;
static SceUID s_external_free_fail_uid = (SceUID)-1;
static uint32_t s_external_free_fail_remaining;
static uint32_t s_sequence_count;
static uint32_t s_log_calls;
static uint32_t s_log_max_bytes;
static uint32_t s_log_under_lock;
static int s_lock_held;
static int s_case_active;
static int s_saw_external_first;
static int s_saw_external_chunk_power2;
static int s_saw_external_oom_power2;

#define CHECK(expression) do {                                               \
    if (!(expression)) {                                                    \
        fprintf(stderr, "hybrid oracle failed at %s:%d: %s\n",            \
                __FILE__, __LINE__, #expression);                           \
        return 0;                                                           \
    }                                                                       \
} while (0)

static void fake_require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "hybrid fake failed: %s\n", message);
        abort();
    }
}

static void sequence_append(unsigned char kind)
{
    fake_require(s_sequence_count < MAX_SEQUENCE, "sequence overflow");
    s_free_sequence[s_sequence_count++] = kind;
}

void isaac_vita_log(const char *format, ...)
{
    char line[512];
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    fake_require(length >= 0, "logger formatting failed");
    ++s_log_calls;
    if ((uint32_t)length > s_log_max_bytes)
        s_log_max_bytes = (uint32_t)length;
    if (s_lock_held)
        ++s_log_under_lock;
    if (strstr(line, "e=external-first"))
        s_saw_external_first = 1;
    if (strstr(line, "e=external-chunk-power2"))
        s_saw_external_chunk_power2 = 1;
    if (strstr(line, "e=external-oom-power2"))
        s_saw_external_oom_power2 = 1;
}

int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot)
{
    fake_require(s_lock_held, "raw snapshot escaped heap lock");
    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    snapshot->uid = 77;
    snapshot->base = (uintptr_t)s_raw_backing;
    snapshot->end = (uintptr_t)s_raw_backing + sizeof s_raw_backing;
    snapshot->retained_end = snapshot->end + 0x1000U;
    snapshot->capacity_bytes = sizeof s_raw_backing;
    snapshot->retained_bytes = sizeof s_raw_backing + 0x1000U;
    snapshot->live_count = s_raw_live;
    snapshot->internal_live_count = s_raw_live;
    snapshot->internal_requested_bytes =
        (size_t)s_raw_live * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    snapshot->has_mspace = 1;
    return 1;
}

void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void)
{
    uint32_t offset;

    fake_require(s_lock_held, "raw page allocation escaped heap lock");
    ++s_raw_malloc_calls;
    if (s_raw_successes >= s_raw_success_limit)
        return NULL;
    for (offset = 0U;
         offset < ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES; ++offset) {
        uint32_t page =
            ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES - 1U - offset;

        if (!s_raw_page_live[page]) {
            unsigned char *base = s_raw_backing +
                page * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;

            s_raw_page_live[page] = 1U;
            ++s_raw_successes;
            ++s_raw_live;
            memset(base, 0xa5, ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
            return base;
        }
    }
    return NULL;
}

int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer)
{
    uintptr_t begin = (uintptr_t)s_raw_backing;
    uintptr_t value = (uintptr_t)pointer;
    uintptr_t offset;
    uint32_t page;

    fake_require(s_lock_held, "raw page free escaped heap lock");
    ++s_raw_free_calls;
    sequence_append('R');
    if (value < begin || value >= begin + sizeof s_raw_backing)
        return 0;
    offset = value - begin;
    if (offset % ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES)
        return 0;
    page = (uint32_t)(offset /
        ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES);
    if (page >= ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES ||
        !s_raw_page_live[page] || !s_raw_live)
        return 0;
    s_raw_page_live[page] = 0U;
    --s_raw_live;
    return 1;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    fake_external_receipt *receipt;

    fake_require(s_lock_held, "external allocation escaped heap lock");
    fake_require(name && strcmp(name, "isaac_room_entries") == 0,
                 "external allocation name drifted");
    fake_require(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                 "external allocation type drifted");
    fake_require(size == ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES,
                 "external allocation size drifted");
    fake_require(option == NULL, "external allocation options drifted");
    ++s_external_alloc_calls;
    if (s_external_oom_remaining) {
        --s_external_oom_remaining;
        return -55;
    }
    fake_require(s_external_successes <
                     ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS,
                 "unexpected thirteenth external allocation");
    receipt = &s_external_receipts[s_external_successes];
    receipt->uid = (SceUID)(1000 + s_external_successes);
    receipt->base = s_external_arena +
        (size_t)s_external_successes *
            ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES;
    receipt->live = 1;
    ++s_external_successes;
    return receipt->uid;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    uint32_t index;

    fake_require(s_lock_held, "external base lookup escaped heap lock");
    ++s_external_get_base_calls;
    if (s_external_get_base_fail_remaining) {
        --s_external_get_base_fail_remaining;
        return -77;
    }
    for (index = 0U; index < s_external_successes; ++index) {
        if (s_external_receipts[index].uid == uid &&
            s_external_receipts[index].live) {
            *base = s_external_receipts[index].base;
            return 0;
        }
    }
    return -1;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    uint32_t index;

    fake_require(s_lock_held, "external free escaped heap lock");
    fake_require(s_external_free_calls < MAX_SEQUENCE,
                 "external free log overflow");
    s_external_free_uids[s_external_free_calls++] = uid;
    sequence_append('E');
    if (uid == s_external_free_fail_uid &&
        s_external_free_fail_remaining) {
        --s_external_free_fail_remaining;
        return -66;
    }
    for (index = 0U; index < s_external_successes; ++index) {
        if (s_external_receipts[index].uid == uid &&
            s_external_receipts[index].live) {
            s_external_receipts[index].live = 0;
            return 0;
        }
    }
    return -2;
}

static isaac_vita_heap_overflow_forbidden_ranges forbidden_ranges(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;

    ranges.fixed_image.begin = UINT32_C(0x00010000);
    ranges.fixed_image.end = UINT32_C(0x00011000);
    ranges.guest_stack.begin = UINT32_C(0x00020000);
    ranges.guest_stack.end = UINT32_C(0x00021000);
    ranges.retained_newlib.begin = UINT32_C(0x00030000);
    ranges.retained_newlib.end = UINT32_C(0x00031000);
    return ranges;
}

static isaac_vita_heap_overflow_range raw_request_range(void)
{
    isaac_vita_heap_overflow_range range;

    range.begin = (uintptr_t)s_raw_backing;
    range.end = range.begin + sizeof s_raw_backing;
    return range;
}

static void fake_backing_reset(void)
{
    uint32_t index;

    fake_require(!s_raw_live, "raw pages survived reset");
    for (index = 0U; index < s_external_successes; ++index)
        fake_require(!s_external_receipts[index].live,
                     "external UID survived reset");
    memset(s_raw_page_live, 0, sizeof s_raw_page_live);
    memset(s_external_receipts, 0, sizeof s_external_receipts);
    memset(s_external_free_uids, 0, sizeof s_external_free_uids);
    memset(s_free_sequence, 0, sizeof s_free_sequence);
    s_raw_success_limit = 0U;
    s_raw_successes = 0U;
    s_raw_malloc_calls = 0U;
    s_raw_free_calls = 0U;
    s_external_successes = 0U;
    s_external_alloc_calls = 0U;
    s_external_get_base_calls = 0U;
    s_external_free_calls = 0U;
    s_external_oom_remaining = 0U;
    s_external_get_base_fail_remaining = 0U;
    s_external_free_fail_uid = (SceUID)-1;
    s_external_free_fail_remaining = 0U;
    s_sequence_count = 0U;
    s_log_calls = 0U;
    s_log_max_bytes = 0U;
    s_log_under_lock = 0U;
    s_saw_external_first = 0;
    s_saw_external_chunk_power2 = 0;
    s_saw_external_oom_power2 = 0;
}

static int slab_reset(void)
{
    int result;

    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_test_reset_locked();
    s_lock_held = 0;
    return result;
}

static int finish_case(void)
{
    CHECK(s_case_active);
    CHECK(slab_reset());
    s_case_active = 0;
    return 1;
}

static int start_case(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;
    isaac_vita_heap_overflow_range raw;

    if (s_case_active)
        CHECK(finish_case());
    fake_backing_reset();
    ranges = forbidden_ranges();
    raw = raw_request_range();
    CHECK(isaac_vita_room_entry_external_init(&ranges, &raw));
    s_case_active = 1;
    return 1;
}

static isaac_vita_room_entry_slab_result slab_malloc(
    uint32_t owner, size_t size, void **pointer,
    isaac_vita_room_entry_slab_event *event)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_result result;

    isaac_vita_room_entry_slab_event_init(event);
    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_malloc_locked(
        owner, size, &decision, event);
    s_lock_held = 0;
    *pointer = decision.pointer;
    isaac_vita_room_entry_slab_log_event(event);
    return result;
}

static isaac_vita_room_entry_slab_result slab_free(void *pointer)
{
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_result result;

    isaac_vita_room_entry_slab_event_init(&event);
    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_free_locked(
        pointer, ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA,
        NULL, &decision, &event);
    s_lock_held = 0;
    isaac_vita_room_entry_slab_log_event(&event);
    return result;
}

static int slab_snapshot(isaac_vita_room_entry_slab_snapshot *snapshot)
{
    int result;

    s_lock_held = 1;
    result = isaac_vita_room_entry_slab_snapshot_locked(snapshot);
    s_lock_held = 0;
    return result;
}

static int allocate_slots(uint32_t count)
{
    isaac_vita_room_entry_slab_event event;
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
                          ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
                          &s_pointers[index], &event) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
        CHECK(s_pointers[index] != NULL);
    }
    return 1;
}

static int free_slots(uint32_t count)
{
    uint32_t index;

    for (index = 0U; index < count; ++index)
        CHECK(slab_free(s_pointers[index]) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    return 1;
}

static int test_capacity(uint32_t raw_target)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot state;
    uint32_t external_pages =
        ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES - raw_target;
    uint32_t expected_chunks = (external_pages + 15U) / 16U;
    uint32_t expected_raw_calls = raw_target ==
        ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES ? raw_target :
            raw_target + expected_chunks;
    uint32_t raw_calls;
    uint32_t external_calls;
    void *extra = NULL;
    void *stale;
    uint32_t index;

    CHECK(start_case());
    s_raw_success_limit = raw_target;
    CHECK(allocate_slots(MAX_POINTERS));
    CHECK(slab_snapshot(&state));
    CHECK(state.pages == ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES &&
          state.raw_pages == raw_target &&
          state.external_pages == external_pages &&
          state.external_chunks == expected_chunks &&
          state.live_slots == MAX_POINTERS &&
          state.raw_internal_live == raw_target &&
          state.raw_internal_requested ==
              raw_target * ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES &&
          state.external_requested ==
              expected_chunks *
                  ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES &&
          state.external_retained ==
              expected_chunks *
                  ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES);
    if (s_external_alloc_calls != expected_chunks ||
        s_external_get_base_calls != expected_chunks ||
        s_raw_malloc_calls != expected_raw_calls) {
        fprintf(stderr,
                "capacity counters raw=%u: raw calls=%u expected=%u "
                "external alloc/get=%u/%u expected=%u\n",
                (unsigned)raw_target, (unsigned)s_raw_malloc_calls,
                (unsigned)expected_raw_calls,
                (unsigned)s_external_alloc_calls,
                (unsigned)s_external_get_base_calls,
                (unsigned)expected_chunks);
        return 0;
    }
    if (!raw_target) {
        int owns;

        /* 192 sorted page descriptors must retain the advertised bounded
         * binary-classification cost for exact/interior/foreign/stale probes. */
        s_lock_held = 1;
        isaac_vita_room_entry_slab_test_probe_reset_locked();
        owns = isaac_vita_room_entry_slab_owns_exact_locked(
            s_pointers[MAX_POINTERS - 1U]);
        CHECK(owns &&
              isaac_vita_room_entry_slab_test_probe_count_locked() <= 8U);
        isaac_vita_room_entry_slab_test_probe_reset_locked();
        owns = isaac_vita_room_entry_slab_owns_exact_locked(
            (unsigned char *)s_pointers[MAX_POINTERS - 1U] + 1U);
        CHECK(!owns &&
              isaac_vita_room_entry_slab_test_probe_count_locked() <= 8U);
        isaac_vita_room_entry_slab_test_probe_reset_locked();
        owns = isaac_vita_room_entry_slab_owns_exact_locked(&state);
        CHECK(!owns &&
              isaac_vita_room_entry_slab_test_probe_count_locked() <= 8U);
        s_lock_held = 0;
        stale = s_pointers[0];
        CHECK(slab_free(stale) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
        s_lock_held = 1;
        isaac_vita_room_entry_slab_test_probe_reset_locked();
        owns = isaac_vita_room_entry_slab_owns_exact_locked(stale);
        CHECK(!owns &&
              isaac_vita_room_entry_slab_test_probe_count_locked() <= 8U);
        s_lock_held = 0;
        CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
                          ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
                          &s_pointers[0], &event) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    }
    raw_calls = s_raw_malloc_calls;
    external_calls = s_external_alloc_calls;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA,
                      ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES,
                      &extra, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK && !extra);
    CHECK(s_raw_malloc_calls == raw_calls &&
          s_external_alloc_calls == external_calls);
    CHECK(free_slots(MAX_POINTERS));
    CHECK(s_log_under_lock == 0U && s_log_max_bytes < 384U);
    if (external_pages) {
        CHECK(s_saw_external_first && s_saw_external_chunk_power2);
    }
    CHECK(finish_case());
    CHECK(s_external_free_calls == expected_chunks &&
          s_raw_free_calls == raw_target &&
          s_sequence_count == expected_chunks + raw_target);
    for (index = 0U; index < expected_chunks; ++index) {
        CHECK(s_free_sequence[index] == 'E');
        CHECK(s_external_free_uids[index] ==
              (SceUID)(1000 + expected_chunks - 1U - index));
    }
    for (; index < s_sequence_count; ++index)
        CHECK(s_free_sequence[index] == 'R');
    return 1;
}

static int test_cooldown_external_retry(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot state;
    void *pointer = NULL;
    uint32_t request;
    uint32_t logs_after_oom;

    CHECK(start_case());
    s_raw_success_limit = 0U;
    s_external_oom_remaining = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK && !pointer);
    CHECK(slab_snapshot(&state));
    CHECK(state.backing_retry_remaining == 4032U &&
          state.external_allocation_attempts == 1U &&
          state.external_out_of_memory == 1U &&
          state.fallbacks == 1U && s_raw_malloc_calls == 1U &&
          s_external_alloc_calls == 1U && s_saw_external_oom_power2);
    logs_after_oom = s_log_calls;
    for (request = 1U; request <= 4031U; ++request) {
        CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                          &pointer, &event) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK && !pointer);
    }
    CHECK(slab_snapshot(&state));
    CHECK(state.backing_retry_remaining == 1U &&
          state.backing_retry_suppressed == 4031U &&
          state.fallbacks == 4032U && s_raw_malloc_calls == 1U &&
          s_external_alloc_calls == 1U);
    CHECK(s_log_calls == logs_after_oom);
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    CHECK(s_raw_malloc_calls == 2U && s_external_alloc_calls == 2U);
    CHECK(slab_snapshot(&state));
    CHECK(state.backing_retry_remaining == 0U &&
          state.backing_retry_suppressed == 4031U &&
          state.raw_pages == 0U && state.external_pages == 1U);
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(finish_case());
    return 1;
}

static int test_raw_recovers_after_retained_external(void)
{
    const uint32_t external_slots =
        ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK *
            ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot state;
    void *pointer = NULL;

    CHECK(start_case());
    CHECK(allocate_slots(external_slots));
    CHECK(slab_snapshot(&state) && state.raw_pages == 0U &&
          state.external_pages == 16U && state.external_chunks == 1U);
    s_raw_success_limit = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    s_pointers[external_slots] = pointer;
    CHECK(slab_snapshot(&state));
    CHECK(state.pages == 17U && state.raw_pages == 1U &&
          state.external_pages == 16U && state.external_chunks == 1U &&
          state.raw_internal_live == 1U);
    CHECK(free_slots(external_slots + 1U));
    CHECK(finish_case());
    CHECK(s_sequence_count == 2U && s_free_sequence[0] == 'E' &&
          s_free_sequence[1] == 'R');
    return 1;
}

static int test_cooldown_raw_recovery(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot state;
    void *pointer = NULL;
    uint32_t request;

    CHECK(start_case());
    s_external_oom_remaining = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK);
    for (request = 1U; request <= 4031U; ++request)
        CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                          &pointer, &event) ==
              ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK);
    s_raw_success_limit = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED && pointer);
    CHECK(s_raw_malloc_calls == 2U && s_external_alloc_calls == 1U);
    CHECK(slab_snapshot(&state) && state.raw_pages == 1U &&
          state.external_pages == 0U &&
          state.backing_retry_remaining == 0U);
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(finish_case());
    return 1;
}

static int test_publication_rollback(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot state;
    void *pointer = NULL;

    CHECK(start_case());
    isaac_vita_room_entry_slab_test_fail_next_page_publish_locked();
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL && !pointer);
    CHECK(slab_snapshot(&state) && state.terminal && !state.pages &&
          !state.external_pages && !state.external_chunks &&
          s_external_free_calls == 1U);
    CHECK(finish_case());

    CHECK(start_case());
    isaac_vita_room_entry_slab_test_fail_after_external_commit_locked();
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL && !pointer);
    CHECK(slab_snapshot(&state) && state.terminal && !state.pages &&
          !state.external_pages && !state.external_chunks &&
          s_external_free_calls == 1U);
    CHECK(finish_case());

    CHECK(start_case());
    s_external_free_fail_uid = 1000;
    s_external_free_fail_remaining = 1U;
    isaac_vita_room_entry_slab_test_fail_after_external_commit_locked();
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL && !pointer);
    CHECK(slab_snapshot(&state) && state.terminal && !state.pages &&
          state.external_state ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED &&
          state.external_orphan_uid == 1000 &&
          state.external_rollback_free_failures == 1U);
    CHECK(finish_case());
    return 1;
}

static int test_post_uid_boundary_terminal(void)
{
    isaac_vita_room_entry_slab_event event;
    isaac_vita_room_entry_slab_snapshot state;
    void *pointer = NULL;

    CHECK(start_case());
    s_external_get_base_fail_remaining = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL && !pointer);
    CHECK(slab_snapshot(&state) && state.terminal && !state.pages &&
          state.fallbacks == 0U && state.external_chunks == 0U &&
          state.external_state ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY &&
          state.external_terminal_fault ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_GET_BASE &&
          s_external_alloc_calls == 1U &&
          s_external_get_base_calls == 1U &&
          s_external_free_calls == 1U);
    CHECK(finish_case());

    CHECK(start_case());
    s_external_get_base_fail_remaining = 1U;
    s_external_free_fail_uid = 1000;
    s_external_free_fail_remaining = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL && !pointer);
    CHECK(slab_snapshot(&state) && state.terminal && !state.pages &&
          state.fallbacks == 0U && state.external_chunks == 0U &&
          state.external_state ==
              ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED &&
          state.external_orphan_uid == 1000 &&
          state.external_rollback_free_failures == 1U &&
          s_external_alloc_calls == 1U &&
          s_external_get_base_calls == 1U &&
          s_external_free_calls == 1U);
    CHECK(finish_case());
    return 1;
}

static int test_reset_corruption_no_syscall(void)
{
    const uint32_t external_slots = 17U *
        ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
    isaac_vita_room_entry_slab_event event;
    void *pointer = NULL;
    uint32_t raw_frees;
    uint32_t external_frees;

    CHECK(start_case());
    s_raw_success_limit = 1U;
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA, 12U,
                      &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    CHECK(slab_free(pointer) == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED);
    raw_frees = s_raw_free_calls;
    external_frees = s_external_free_calls;
    isaac_vita_room_entry_slab_test_corrupt_future_nonfull_locked();
    CHECK(!slab_reset() && s_raw_free_calls == raw_frees &&
          s_external_free_calls == external_frees);
    isaac_vita_room_entry_slab_test_corrupt_future_nonfull_locked();
    isaac_vita_room_entry_slab_test_corrupt_first_page_source_locked();
    CHECK(!slab_reset() && s_raw_free_calls == raw_frees &&
          s_external_free_calls == external_frees);
    isaac_vita_room_entry_slab_test_corrupt_first_page_source_locked();
    CHECK(finish_case());

    CHECK(start_case());
    CHECK(allocate_slots(external_slots));
    CHECK(free_slots(external_slots));
    raw_frees = s_raw_free_calls;
    external_frees = s_external_free_calls;
    isaac_vita_room_entry_slab_test_corrupt_first_page_source_locked();
    CHECK(!slab_reset() && s_raw_free_calls == raw_frees &&
          s_external_free_calls == external_frees);
    isaac_vita_room_entry_slab_test_corrupt_first_page_source_locked();
    isaac_vita_room_entry_slab_test_corrupt_first_external_tag_locked();
    CHECK(!slab_reset() && s_raw_free_calls == raw_frees &&
          s_external_free_calls == external_frees);
    isaac_vita_room_entry_slab_test_corrupt_first_external_tag_locked();
    isaac_vita_room_entry_slab_test_corrupt_first_external_base_locked();
    CHECK(!slab_reset() && s_raw_free_calls == raw_frees &&
          s_external_free_calls == external_frees);
    isaac_vita_room_entry_slab_test_corrupt_first_external_base_locked();
    CHECK(finish_case());
    return 1;
}

static int test_reset_retry_middle(void)
{
    const uint32_t slots = 34U *
        ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE;
    isaac_vita_room_entry_slab_snapshot state;

    CHECK(start_case());
    s_raw_success_limit = 1U;
    CHECK(allocate_slots(slots));
    CHECK(free_slots(slots));
    CHECK(slab_snapshot(&state) && state.raw_pages == 1U &&
          state.external_chunks == 3U && state.external_pages == 33U);
    s_external_free_fail_uid = 1001;
    s_external_free_fail_remaining = 1U;
    CHECK(!slab_reset());
    CHECK(s_external_free_calls == 2U &&
          s_external_free_uids[0] == 1002 &&
          s_external_free_uids[1] == 1001);
    CHECK(s_raw_free_calls == 0U && s_raw_live == 1U);
    CHECK(slab_snapshot(&state) && state.external_chunks == 2U &&
          state.external_pages == 32U && state.raw_pages == 1U &&
          state.pages == 33U &&
          state.external_reset_free_attempts == 2U &&
          state.external_reset_free_failures == 1U);
    CHECK(slab_reset());
    s_case_active = 0;
    CHECK(s_external_free_calls == 4U &&
          s_external_free_uids[2] == 1001 &&
          s_external_free_uids[3] == 1000);
    CHECK(s_raw_free_calls == 1U && s_sequence_count == 5U &&
          s_free_sequence[4] == 'R');
    return 1;
}

static int test_wrong_owner_no_backing(void)
{
    isaac_vita_room_entry_slab_event event;
    void *pointer = NULL;

    CHECK(start_case());
    CHECK(slab_malloc(ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA + 1U,
                      12U, &pointer, &event) ==
          ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED && !pointer);
    CHECK(!s_raw_malloc_calls && !s_external_alloc_calls);
    CHECK(finish_case());
    return 1;
}

int main(void)
{
    if (!test_wrong_owner_no_backing() ||
        !test_capacity(0U) || !test_capacity(1U) ||
        !test_capacity(95U) || !test_capacity(96U) ||
        !test_cooldown_external_retry() ||
        !test_cooldown_raw_recovery() ||
        !test_raw_recovers_after_retained_external() ||
        !test_publication_rollback() ||
        !test_post_uid_boundary_terminal() ||
        !test_reset_corruption_no_syscall() ||
        !test_reset_retry_middle())
        return 1;
    puts("Vita RoomConfig Entry hybrid192 oracle: PASS");
    return 0;
}
