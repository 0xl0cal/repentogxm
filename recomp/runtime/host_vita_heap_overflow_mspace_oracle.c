#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include "host_vita_heap_overflow_mspace.h"

#define FAKE_UID ((SceUID)0x4411)
#define FAKE_ALLOCATION_COUNT 32U

typedef struct fake_allocation {
    unsigned char *pointer;
    size_t size;
    int live;
} fake_allocation;

typedef enum fake_base_mode {
    FAKE_BASE_NORMAL = 0,
    FAKE_BASE_NULL,
    FAKE_BASE_MISALIGNED,
    FAKE_BASE_OVERFLOW
} fake_base_mode;

typedef struct fake_mspace {
    int marker;
} fake_mspace;

_Alignas(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES)
static unsigned char s_fake_backing[ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES];
static fake_allocation s_fake_allocations[FAKE_ALLOCATION_COUNT];
static fake_mspace s_fake_mspace;
static size_t s_fake_bump;
static int s_memblock_live;
static int s_mspace_live;
static int s_fail_memblock_alloc;
static int s_fail_memblock_get;
static int s_fail_memblock_free;
static int s_fail_mspace_create;
static int s_fail_mspace_malloc;
static int s_fail_mspace_calloc;
static int s_fail_mspace_realloc;
static int s_misalign_next_result;
static int s_out_of_range_next_result;
static fake_base_mode s_base_mode;
static unsigned s_memblock_alloc_calls;
static unsigned s_memblock_get_calls;
static unsigned s_memblock_free_calls;
static unsigned s_mspace_create_calls;
static unsigned s_mspace_destroy_calls;
static unsigned s_mspace_malloc_calls;
static unsigned s_mspace_calloc_calls;
static unsigned s_mspace_realloc_calls;
static unsigned s_mspace_free_calls;
static unsigned s_bad_api_calls;
static int s_create_saw_dirty_backing;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static size_t fake_allocation_live_count(void)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < FAKE_ALLOCATION_COUNT; ++index)
        if (s_fake_allocations[index].live)
            ++count;
    return count;
}

static fake_allocation *fake_allocation_find(void *pointer)
{
    size_t index;

    for (index = 0U; index < FAKE_ALLOCATION_COUNT; ++index)
        if (s_fake_allocations[index].live &&
            s_fake_allocations[index].pointer == pointer)
            return &s_fake_allocations[index];
    return NULL;
}

static void *fake_mspace_allocate(size_t size, int zero)
{
    fake_allocation *slot = NULL;
    unsigned char *pointer;
    size_t offset;
    size_t index;

    if (!s_mspace_live || !size)
        return NULL;
    for (index = 0U; index < FAKE_ALLOCATION_COUNT; ++index) {
        if (!s_fake_allocations[index].live) {
            slot = &s_fake_allocations[index];
            break;
        }
    }
    if (!slot)
        return NULL;
    if (s_out_of_range_next_result) {
        s_out_of_range_next_result = 0;
        slot->pointer = &s_fake_backing[sizeof s_fake_backing];
        slot->size = size;
        slot->live = 1;
        return slot->pointer;
    }
    offset = (s_fake_bump + 15U) & ~(size_t)15U;
    if (offset > sizeof s_fake_backing ||
        (s_misalign_next_result && offset == sizeof s_fake_backing) ||
        size > sizeof s_fake_backing - offset -
            (s_misalign_next_result ? 1U : 0U))
        return NULL;
    pointer = &s_fake_backing[offset];
    if (s_misalign_next_result) {
        s_misalign_next_result = 0;
        ++pointer;
    }
    slot->pointer = pointer;
    slot->size = size;
    slot->live = 1;
    s_fake_bump = offset + size + 16U;
    if (zero)
        memset(pointer, 0, size);
    else
        memset(pointer, 0x6d, size);
    return pointer;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_memblock_alloc_calls;
    if (s_fail_memblock_alloc) {
        --s_fail_memblock_alloc;
        return -0x101;
    }
    if (!name || strcmp(name, "isaac_heap_overflow") != 0 ||
        type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES || option ||
        s_memblock_live) {
        ++s_bad_api_calls;
        return -0x102;
    }
    memset(s_fake_backing, 0xa5, sizeof s_fake_backing);
    s_memblock_live = 1;
    return FAKE_UID;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_memblock_get_calls;
    if (s_fail_memblock_get) {
        --s_fail_memblock_get;
        return -0x201;
    }
    if (uid != FAKE_UID || !s_memblock_live || !base) {
        ++s_bad_api_calls;
        return -0x202;
    }
    switch (s_base_mode) {
    case FAKE_BASE_NULL:
        *base = NULL;
        break;
    case FAKE_BASE_MISALIGNED:
        *base = &s_fake_backing[1];
        break;
    case FAKE_BASE_OVERFLOW:
        *base = (void *)(UINTPTR_MAX &
            ~(uintptr_t)(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES - 1U));
        break;
    case FAKE_BASE_NORMAL:
    default:
        *base = s_fake_backing;
        break;
    }
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_memblock_free_calls;
    if (s_fail_memblock_free) {
        --s_fail_memblock_free;
        return -0x301;
    }
    if (uid != FAKE_UID || !s_memblock_live) {
        ++s_bad_api_calls;
        return -0x302;
    }
    s_memblock_live = 0;
    return 0;
}

SceClibMspace sceClibMspaceCreate(void *memblock, SceSize size)
{
    ++s_mspace_create_calls;
    if (s_fail_mspace_create) {
        --s_fail_mspace_create;
        return NULL;
    }
    if (memblock != s_fake_backing ||
        size != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES ||
        !s_memblock_live || s_mspace_live) {
        ++s_bad_api_calls;
        return NULL;
    }
    s_create_saw_dirty_backing =
        s_fake_backing[0] == 0xa5 &&
        s_fake_backing[sizeof s_fake_backing / 2U] == 0xa5 &&
        s_fake_backing[sizeof s_fake_backing - 1U] == 0xa5;
    memset(s_fake_allocations, 0, sizeof s_fake_allocations);
    s_fake_bump = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES;
    s_mspace_live = 1;
    s_fake_mspace.marker = 0x51;
    return &s_fake_mspace;
}

void sceClibMspaceDestroy(SceClibMspace mspace)
{
    ++s_mspace_destroy_calls;
    if (mspace != &s_fake_mspace || !s_mspace_live ||
        fake_allocation_live_count() != 0U) {
        ++s_bad_api_calls;
        return;
    }
    s_mspace_live = 0;
}

void *sceClibMspaceMalloc(SceClibMspace mspace, SceSize size)
{
    ++s_mspace_malloc_calls;
    if (mspace != &s_fake_mspace || !s_mspace_live) {
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_mspace_malloc) {
        --s_fail_mspace_malloc;
        return NULL;
    }
    return fake_mspace_allocate(size, 0);
}

void *sceClibMspaceCalloc(SceClibMspace mspace, SceSize count, SceSize size)
{
    size_t total;

    ++s_mspace_calloc_calls;
    if (mspace != &s_fake_mspace || !s_mspace_live ||
        __builtin_mul_overflow((size_t)count, (size_t)size, &total)) {
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_mspace_calloc) {
        --s_fail_mspace_calloc;
        return NULL;
    }
    return fake_mspace_allocate(total, 1);
}

void *sceClibMspaceRealloc(SceClibMspace mspace, void *pointer, SceSize size)
{
    fake_allocation *old_allocation;
    void *replacement;
    size_t copy_size;

    ++s_mspace_realloc_calls;
    old_allocation = fake_allocation_find(pointer);
    if (mspace != &s_fake_mspace || !s_mspace_live ||
        !old_allocation || !size) {
        ++s_bad_api_calls;
        return NULL;
    }
    if (s_fail_mspace_realloc) {
        --s_fail_mspace_realloc;
        return NULL;
    }
    replacement = fake_mspace_allocate(size, 0);
    if (!replacement)
        return NULL;
    if ((uintptr_t)replacement <
        (uintptr_t)s_fake_backing + sizeof s_fake_backing) {
        copy_size = old_allocation->size < size ? old_allocation->size : size;
        memmove(replacement, pointer, copy_size);
    }
    old_allocation->live = 0;
    return replacement;
}

void sceClibMspaceFree(SceClibMspace mspace, void *pointer)
{
    fake_allocation *allocation;

    ++s_mspace_free_calls;
    allocation = fake_allocation_find(pointer);
    if (mspace != &s_fake_mspace || !s_mspace_live || !allocation) {
        ++s_bad_api_calls;
        return;
    }
    allocation->live = 0;
}

static isaac_vita_heap_overflow_forbidden_ranges safe_ranges(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;

    ranges.fixed_image.begin = 0x1000U;
    ranges.fixed_image.end = 0x2000U;
    ranges.guest_stack.begin = 0x3000U;
    ranges.guest_stack.end = 0x4000U;
    ranges.retained_newlib.begin = 0x5000U;
    ranges.retained_newlib.end = 0x6000U;
    return ranges;
}

static int snapshot_is(isaac_vita_heap_overflow_mspace_state state,
                       size_t live_count, int has_mspace)
{
    isaac_vita_heap_overflow_mspace_snapshot snapshot;

    return isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
        snapshot.state == state && snapshot.live_count == live_count &&
        snapshot.stranded_count == 0U &&
        snapshot.internal_live_count == 0U &&
        snapshot.internal_requested_bytes == 0U &&
        snapshot.has_mspace == has_mspace;
}

static int oracle_outer_ledger_accepts(
    const void *query, const void *first, int first_live,
    const void *second, int second_live)
{
    return (first_live && query == first) ||
        (second_live && query == second);
}

static int oracle_reset(void)
{
    if (!isaac_vita_heap_overflow_mspace_test_reset())
        return 0;
    if (s_memblock_live || s_mspace_live || fake_allocation_live_count())
        return 0;
    s_fail_memblock_alloc = 0;
    s_fail_memblock_get = 0;
    s_fail_memblock_free = 0;
    s_fail_mspace_create = 0;
    s_fail_mspace_malloc = 0;
    s_fail_mspace_calloc = 0;
    s_fail_mspace_realloc = 0;
    s_misalign_next_result = 0;
    s_out_of_range_next_result = 0;
    s_base_mode = FAKE_BASE_NORMAL;
    s_memblock_alloc_calls = 0U;
    s_memblock_get_calls = 0U;
    s_memblock_free_calls = 0U;
    s_mspace_create_calls = 0U;
    s_mspace_destroy_calls = 0U;
    s_mspace_malloc_calls = 0U;
    s_mspace_calloc_calls = 0U;
    s_mspace_realloc_calls = 0U;
    s_mspace_free_calls = 0U;
    s_bad_api_calls = 0U;
    s_create_saw_dirty_backing = 0;
    memset(s_fake_allocations, 0, sizeof s_fake_allocations);
    return 1;
}

static int test_invalid_configuration(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();

    CHECK(oracle_reset());
    CHECK(!isaac_vita_heap_overflow_mspace_init(NULL));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0));
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == 0U);

    CHECK(oracle_reset());
    ranges.fixed_image.end = ranges.fixed_image.begin;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == 0U);
    CHECK(oracle_reset());
    ranges = safe_ranges();
    ranges.guest_stack.begin = ranges.guest_stack.end + 1U;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == 0U);
    CHECK(oracle_reset());
    ranges = safe_ranges();
    ranges.retained_newlib.end = ranges.retained_newlib.begin;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_initial_failures(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    isaac_vita_heap_overflow_mspace_snapshot snapshot;

    CHECK(oracle_reset());
    s_fail_memblock_alloc = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0) &&
          !s_memblock_live && s_memblock_alloc_calls == 1U &&
          s_memblock_get_calls == 0U && s_memblock_free_calls == 0U);

    CHECK(oracle_reset());
    s_fail_memblock_get = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0) &&
          !s_memblock_live && s_memblock_free_calls == 1U);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_NULL;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0) &&
          !s_memblock_live && s_memblock_free_calls == 1U);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_MISALIGNED;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0) &&
          !s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_OVERFLOW;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0) &&
          !s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    s_fail_mspace_create = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED, 0U, 0) &&
          !s_memblock_live && s_mspace_create_calls == 1U &&
          s_memblock_free_calls == 1U);

    CHECK(oracle_reset());
    s_fail_memblock_get = 1;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED &&
          snapshot.uid == FAKE_UID && !snapshot.has_mspace &&
          s_memblock_live);
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == 1U);
    CHECK(oracle_reset());

    s_fail_mspace_create = 1;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED, 0U, 0) &&
          s_memblock_live && !s_mspace_live);
    CHECK(oracle_reset());
    return 0;
}

static int test_forbidden_ranges(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    uintptr_t base = (uintptr_t)s_fake_backing;
    uintptr_t end = base + ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES;
    uintptr_t retained_end =
        base + ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES;

    CHECK(oracle_reset());
    ranges.fixed_image.begin = base + 0x1000U;
    ranges.fixed_image.end = base + 0x2000U;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          !s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    ranges = safe_ranges();
    ranges.guest_stack.begin = base;
    ranges.guest_stack.end = base + 1U;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          !s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    ranges = safe_ranges();
    ranges.retained_newlib.begin = end - 1U;
    ranges.retained_newlib.end = end;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          !s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    ranges = safe_ranges();
    /* The usable request ends before this range, but Vita3K's additional
     * retained page still collides and must make initialization fail. */
    ranges.fixed_image.begin = end;
    ranges.fixed_image.end = retained_end;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          !s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    ranges = safe_ranges();
    ranges.fixed_image.begin = base - 0x1000U;
    ranges.fixed_image.end = base;
    ranges.guest_stack.begin = retained_end;
    ranges.guest_stack.end = retained_end + 0x1000U;
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(s_create_saw_dirty_backing && s_bad_api_calls == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_validation_rollback_orphans(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    uintptr_t end = (uintptr_t)s_fake_backing +
        ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES;
    uintptr_t retained_end = (uintptr_t)s_fake_backing +
        ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES;

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_NULL;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED, 0U, 0) &&
          s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_MISALIGNED;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED, 0U, 0) &&
          s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_OVERFLOW;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED, 0U, 0) &&
          s_memblock_live && s_mspace_create_calls == 0U);

    CHECK(oracle_reset());
    ranges = safe_ranges();
    ranges.fixed_image.begin = end;
    ranges.fixed_image.end = retained_end;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED, 0U, 0) &&
          s_memblock_live && s_mspace_create_calls == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_ready_lifecycle(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    unsigned char *pointer;
    unsigned malloc_calls;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == 1U);
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY &&
          snapshot.uid == FAKE_UID &&
          snapshot.base == (uintptr_t)s_fake_backing &&
          snapshot.end == (uintptr_t)s_fake_backing +
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES &&
          snapshot.retained_end == (uintptr_t)s_fake_backing +
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES &&
          snapshot.capacity_bytes == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES &&
          snapshot.retained_bytes ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES &&
          snapshot.live_count == 0U && snapshot.has_mspace &&
          s_create_saw_dirty_backing);
    CHECK(isaac_vita_heap_overflow_mspace_contains(s_fake_backing));
    CHECK(!isaac_vita_heap_overflow_mspace_contains(NULL));
    CHECK(!isaac_vita_heap_overflow_mspace_contains(
              (const void *)((uintptr_t)s_fake_backing - 1U)));
    CHECK(!isaac_vita_heap_overflow_mspace_contains(
              s_fake_backing + sizeof s_fake_backing));
    CHECK(!isaac_vita_heap_overflow_mspace_snapshot_get(NULL));

    malloc_calls = s_mspace_malloc_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_malloc(0U) &&
          s_mspace_malloc_calls == malloc_calls);
#if SIZE_MAX > UINT32_MAX
    CHECK(!isaac_vita_heap_overflow_mspace_malloc((size_t)UINT32_MAX + 1U) &&
          s_mspace_malloc_calls == malloc_calls);
#endif
    pointer = isaac_vita_heap_overflow_mspace_malloc(64U);
    CHECK(pointer && ((uintptr_t)pointer & 7U) == 0U &&
          isaac_vita_heap_overflow_mspace_contains(pointer) &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1));
    CHECK(!isaac_vita_heap_overflow_mspace_test_reset() &&
          s_mspace_destroy_calls == 0U && s_memblock_free_calls == 0U);
    CHECK(isaac_vita_heap_overflow_mspace_free(NULL));
    CHECK(isaac_vita_heap_overflow_mspace_free(pointer));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 0U, 1));
    CHECK(oracle_reset());
    CHECK(s_bad_api_calls == 0U);
    return 0;
}

static int test_outer_exact_base_contract(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    unsigned char *first;
    unsigned char *second;
    int first_live = 1;
    int second_live = 1;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    first = isaac_vita_heap_overflow_mspace_malloc(32U);
    second = isaac_vita_heap_overflow_mspace_malloc(32U);
    CHECK(first && second &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 2U, 1));

    /* contains deliberately classifies the domain, not ownership.  The outer
     * exact-base ledger rejects an interior pointer before this raw API. */
    CHECK(isaac_vita_heap_overflow_mspace_contains(first + 1U));
    CHECK(!oracle_outer_ledger_accepts(
              first + 1U, first, first_live, second, second_live));
    CHECK(oracle_outer_ledger_accepts(
              first, first, first_live, second, second_live));
    CHECK(isaac_vita_heap_overflow_mspace_free(first));
    first_live = 0;

    /* A stale/double-free address remains inside the pool while another live
     * allocation exists.  Only the outer ledger can reject it safely. */
    CHECK(isaac_vita_heap_overflow_mspace_contains(first));
    CHECK(!oracle_outer_ledger_accepts(
              first, first, first_live, second, second_live));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1));
    CHECK(oracle_outer_ledger_accepts(
              second, first, first_live, second, second_live));
    CHECK(isaac_vita_heap_overflow_mspace_free(second));
    second_live = 0;
    CHECK(!oracle_outer_ledger_accepts(
              second, first, first_live, second, second_live));
    CHECK(oracle_reset());
    return 0;
}

static int test_internal_page_accounting(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    void *internal;
    void *guest;
    unsigned malloc_calls;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    internal = isaac_vita_heap_overflow_mspace_internal_page_malloc();
    CHECK(internal && ((uintptr_t)internal & 7U) == 0U &&
          isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY &&
          snapshot.live_count == 1U && snapshot.stranded_count == 0U &&
          snapshot.internal_live_count == 1U &&
          snapshot.internal_requested_bytes ==
              ISAAC_VITA_HEAP_OVERFLOW_INTERNAL_PAGE_BYTES);
    guest = isaac_vita_heap_overflow_mspace_malloc(32U);
    CHECK(guest &&
          isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.live_count == 2U && snapshot.internal_live_count == 1U &&
          snapshot.internal_requested_bytes ==
              ISAAC_VITA_HEAP_OVERFLOW_INTERNAL_PAGE_BYTES);
    CHECK(!isaac_vita_heap_overflow_mspace_test_reset() &&
          s_mspace_destroy_calls == 0U && s_memblock_free_calls == 0U);
    CHECK(isaac_vita_heap_overflow_mspace_free(guest));
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.live_count == 1U && snapshot.internal_live_count == 1U);
    CHECK(isaac_vita_heap_overflow_mspace_internal_page_free(internal));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 0U, 1));
    CHECK(!isaac_vita_heap_overflow_mspace_internal_page_free(internal));
    CHECK(oracle_reset());

    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    malloc_calls = s_mspace_malloc_calls;
    s_fail_mspace_malloc = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_internal_page_malloc() &&
          s_mspace_malloc_calls == malloc_calls + 1U &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 0U, 1));
    CHECK(oracle_reset());

    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    s_misalign_next_result = 1;
    malloc_calls = s_mspace_free_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_internal_page_malloc() &&
          isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY &&
          snapshot.live_count == 0U && snapshot.stranded_count == 0U &&
          snapshot.internal_live_count == 0U &&
          snapshot.internal_requested_bytes == 0U &&
          s_mspace_free_calls == malloc_calls + 1U &&
          fake_allocation_live_count() == 0U);
    CHECK(oracle_reset());
    CHECK(s_bad_api_calls == 0U);
    return 0;
}

static int test_internal_out_of_range_stranded(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    unsigned free_calls;
    unsigned destroy_calls;
    unsigned memblock_free_calls;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    free_calls = s_mspace_free_calls;
    s_out_of_range_next_result = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_internal_page_malloc() &&
          isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY &&
          snapshot.live_count == 1U && snapshot.stranded_count == 1U &&
          snapshot.internal_live_count == 0U &&
          snapshot.internal_requested_bytes == 0U &&
          s_mspace_free_calls == free_calls &&
          fake_allocation_live_count() == 1U);
    destroy_calls = s_mspace_destroy_calls;
    memblock_free_calls = s_memblock_free_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_test_reset() &&
          s_mspace_destroy_calls == destroy_calls &&
          s_memblock_free_calls == memblock_free_calls &&
          s_bad_api_calls == 0U);
    return 0;
}

static int test_api_failure_and_realloc(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    unsigned char *pointer;
    unsigned char *replacement;
    isaac_vita_heap_overflow_mspace_realloc_status realloc_status;
    unsigned calloc_calls;
    unsigned malloc_calls;
    unsigned realloc_calls;
    size_t index;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    s_fail_mspace_malloc = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_malloc(32U) &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 0U, 1));
    s_fail_mspace_calloc = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_calloc(4U, 8U) &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 0U, 1));
    calloc_calls = s_mspace_calloc_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_calloc(0U, 8U) &&
          !isaac_vita_heap_overflow_mspace_calloc(8U, 0U) &&
          !isaac_vita_heap_overflow_mspace_calloc(SIZE_MAX, 2U) &&
          s_mspace_calloc_calls == calloc_calls);

    pointer = isaac_vita_heap_overflow_mspace_calloc(8U, 8U);
    CHECK(pointer && snapshot_is(
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1));
    for (index = 0U; index < 64U; ++index)
        CHECK(pointer[index] == 0U);
    for (index = 0U; index < 64U; ++index)
        pointer[index] = (unsigned char)(index + 1U);
    realloc_calls = s_mspace_realloc_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_realloc(
              pointer, 128U, NULL) &&
          s_mspace_realloc_calls == realloc_calls &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1));
    s_fail_mspace_realloc = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_realloc(
              pointer, 128U, &realloc_status) &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_OUT_OF_MEMORY &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1));
    for (index = 0U; index < 64U; ++index)
        CHECK(pointer[index] == (unsigned char)(index + 1U));
    replacement = isaac_vita_heap_overflow_mspace_realloc(
        pointer, 128U, &realloc_status);
    CHECK(replacement && replacement != pointer &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1));
    for (index = 0U; index < 64U; ++index)
        CHECK(replacement[index] == (unsigned char)(index + 1U));
    CHECK(isaac_vita_heap_overflow_mspace_realloc(
              replacement, 0U, &realloc_status) == NULL &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_FREED &&
          snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 0U, 1));
    malloc_calls = s_mspace_malloc_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_realloc(
              NULL, 0U, &realloc_status) &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_REJECTED &&
          s_mspace_malloc_calls == malloc_calls);
    pointer = isaac_vita_heap_overflow_mspace_realloc(
        NULL, 24U, &realloc_status);
    CHECK(pointer && snapshot_is(
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY, 1U, 1) &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS);
    CHECK(isaac_vita_heap_overflow_mspace_free(pointer));
    CHECK(oracle_reset());
    CHECK(s_bad_api_calls == 0U);
    return 0;
}

static int test_drain_only_malloc(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    void *first;
    void *second;
    isaac_vita_heap_overflow_mspace_realloc_status realloc_status;
    unsigned malloc_calls;
    unsigned calloc_calls;
    unsigned realloc_calls;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    first = isaac_vita_heap_overflow_mspace_malloc(32U);
    second = isaac_vita_heap_overflow_mspace_malloc(32U);
    CHECK(first && second);
    s_misalign_next_result = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_malloc(32U));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY, 2U, 1) &&
          fake_allocation_live_count() == 2U);
    malloc_calls = s_mspace_malloc_calls;
    calloc_calls = s_mspace_calloc_calls;
    realloc_calls = s_mspace_realloc_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_malloc(16U));
    CHECK(!isaac_vita_heap_overflow_mspace_calloc(1U, 16U));
    CHECK(!isaac_vita_heap_overflow_mspace_realloc(
              first, 64U, &realloc_status) &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_REJECTED);
    CHECK(s_mspace_malloc_calls == malloc_calls &&
          s_mspace_calloc_calls == calloc_calls &&
          s_mspace_realloc_calls == realloc_calls);
    CHECK(isaac_vita_heap_overflow_mspace_free(first));
    CHECK(isaac_vita_heap_overflow_mspace_free(second));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY, 0U, 1));
    CHECK(oracle_reset());
    CHECK(s_bad_api_calls == 0U);
    return 0;
}

static int test_drain_only_calloc_and_realloc(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    void *first;
    void *second;
    isaac_vita_heap_overflow_mspace_realloc_status realloc_status;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    first = isaac_vita_heap_overflow_mspace_malloc(32U);
    CHECK(first);
    s_misalign_next_result = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_calloc(2U, 16U));
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY, 1U, 1));
    CHECK(isaac_vita_heap_overflow_mspace_free(first));
    CHECK(oracle_reset());

    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    first = isaac_vita_heap_overflow_mspace_malloc(32U);
    second = isaac_vita_heap_overflow_mspace_malloc(32U);
    CHECK(first && second);
    s_misalign_next_result = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_realloc(
              first, 64U, &realloc_status) &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_CONSUMED_DRAIN_ONLY);
    /* Successful realloc consumed first; rejecting/freeing its bad result
     * leaves only the unrelated old allocation drainable. */
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY, 1U, 1) &&
          fake_allocation_live_count() == 1U);
    CHECK(isaac_vita_heap_overflow_mspace_free(second));
    CHECK(oracle_reset());
    CHECK(s_bad_api_calls == 0U);
    return 0;
}

static int test_drain_only_out_of_range_realloc(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    isaac_vita_heap_overflow_mspace_realloc_status realloc_status;
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    void *first;
    void *second;
    unsigned free_calls;
    unsigned destroy_calls;
    unsigned memblock_free_calls;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    first = isaac_vita_heap_overflow_mspace_malloc(32U);
    second = isaac_vita_heap_overflow_mspace_malloc(32U);
    CHECK(first && second);
    free_calls = s_mspace_free_calls;
    s_out_of_range_next_result = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_realloc(
              first, 64U, &realloc_status) &&
          realloc_status ==
              ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_CONSUMED_DRAIN_ONLY);
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY &&
          snapshot.live_count == 2U && snapshot.stranded_count == 1U &&
          snapshot.has_mspace && fake_allocation_live_count() == 2U &&
          s_mspace_free_calls == free_calls);
    CHECK(!isaac_vita_heap_overflow_mspace_contains(
              &s_fake_backing[sizeof s_fake_backing]));
    CHECK(isaac_vita_heap_overflow_mspace_free(second));
    CHECK(isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) &&
          snapshot.live_count == 1U && snapshot.stranded_count == 1U &&
          fake_allocation_live_count() == 1U);
    destroy_calls = s_mspace_destroy_calls;
    memblock_free_calls = s_memblock_free_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_test_reset() &&
          s_mspace_destroy_calls == destroy_calls &&
          s_memblock_free_calls == memblock_free_calls &&
          s_bad_api_calls == 0U);
    return 0;
}

static int test_reset_release_failure(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = safe_ranges();
    unsigned alloc_calls;

    CHECK(oracle_reset());
    CHECK(isaac_vita_heap_overflow_mspace_init(&ranges));
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_heap_overflow_mspace_test_reset());
    CHECK(snapshot_is(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED, 0U, 0) &&
          s_memblock_live && !s_mspace_live &&
          s_mspace_destroy_calls == 1U);
    alloc_calls = s_memblock_alloc_calls;
    CHECK(!isaac_vita_heap_overflow_mspace_init(&ranges) &&
          s_memblock_alloc_calls == alloc_calls);
    CHECK(oracle_reset());
    CHECK(s_bad_api_calls == 0U);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--internal-stranded") == 0) {
        CHECK(test_internal_out_of_range_stranded() == 0);
        puts("Vita heap overflow internal stranded oracle: PASS");
        return 0;
    }
    CHECK(argc == 1);
    CHECK(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES == 0x007d5000U);
    CHECK(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES == 0x007d6000U);
    CHECK(test_invalid_configuration() == 0);
    CHECK(test_initial_failures() == 0);
    CHECK(test_forbidden_ranges() == 0);
    CHECK(test_validation_rollback_orphans() == 0);
    CHECK(test_ready_lifecycle() == 0);
    CHECK(test_outer_exact_base_contract() == 0);
    CHECK(test_internal_page_accounting() == 0);
    CHECK(test_api_failure_and_realloc() == 0);
    CHECK(test_drain_only_malloc() == 0);
    CHECK(test_drain_only_calloc_and_realloc() == 0);
    CHECK(test_reset_release_failure() == 0);
    CHECK(oracle_reset());
    /* Last by design: corrupt out-of-range success is stranded and its
     * backing must remain owned until process exit. */
    CHECK(test_drain_only_out_of_range_realloc() == 0);
    puts("Vita heap overflow SceClibMspace oracle: PASS");
    return 0;
}
