/* Bounded fallback allocation domain outside Isaac's fragmented newlib heap. */
#include <stddef.h>
#include <stdint.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include "host_vita_heap_overflow_mspace.h"

_Static_assert((ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES &
                (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES - 1U)) == 0U,
               "overflow mspace page size must be a power of two");
_Static_assert((ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ALIGNMENT &
                (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ALIGNMENT - 1U)) == 0U,
               "overflow mspace alignment must be a power of two");
_Static_assert((ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES &
                (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES - 1U)) == 0U,
               "overflow mspace request must be page aligned");
_Static_assert((ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES &
                (~ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES + 1U)) ==
                   ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES,
               "overflow mspace request must retain only one extra page");
_Static_assert(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES ==
                   ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES +
                       ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES,
               "overflow mspace retained-span receipt changed");

typedef struct overflow_mspace_storage {
    isaac_vita_heap_overflow_mspace_state state;
    SceUID uid;
    void *base;
    uintptr_t end;
    uintptr_t retained_end;
    SceClibMspace mspace;
    size_t live_count;
    size_t stranded_count;
    size_t internal_live_count;
    size_t internal_requested_bytes;
} overflow_mspace_storage;

static overflow_mspace_storage s_overflow = {
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_UNINITIALIZED,
    (SceUID)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_INVALID_UID,
    NULL,
    0U,
    0U,
    NULL,
    0U,
    0U,
    0U,
    0U
};

static int overflow_range_valid(const isaac_vita_heap_overflow_range *range)
{
    return range->begin < range->end;
}

static int overflow_ranges_valid(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden)
{
    return forbidden &&
        overflow_range_valid(&forbidden->fixed_image) &&
        overflow_range_valid(&forbidden->guest_stack) &&
        overflow_range_valid(&forbidden->retained_newlib);
}

static int overflow_ranges_overlap(
    uintptr_t begin, uintptr_t end,
    const isaac_vita_heap_overflow_range *forbidden)
{
    return begin < forbidden->end && forbidden->begin < end;
}

static int overflow_pool_overlaps_forbidden(
    uintptr_t begin, uintptr_t end,
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden)
{
    return overflow_ranges_overlap(begin, end, &forbidden->fixed_image) ||
        overflow_ranges_overlap(begin, end, &forbidden->guest_stack) ||
        overflow_ranges_overlap(begin, end, &forbidden->retained_newlib);
}

static void overflow_clear_resource_fields(void)
{
    s_overflow.uid = (SceUID)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_INVALID_UID;
    s_overflow.base = NULL;
    s_overflow.end = 0U;
    s_overflow.retained_end = 0U;
    s_overflow.mspace = NULL;
    s_overflow.live_count = 0U;
    s_overflow.stranded_count = 0U;
    s_overflow.internal_live_count = 0U;
    s_overflow.internal_requested_bytes = 0U;
}

static int overflow_fail_after_uid(SceUID uid, void *base, uintptr_t end,
                                   uintptr_t retained_end)
{
    s_overflow.uid = uid;
    s_overflow.base = base;
    s_overflow.end = end;
    s_overflow.retained_end = retained_end;
    s_overflow.mspace = NULL;
    s_overflow.live_count = 0U;
    s_overflow.stranded_count = 0U;
    s_overflow.internal_live_count = 0U;
    s_overflow.internal_requested_bytes = 0U;
    if (sceKernelFreeMemBlock(uid) < 0) {
        s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED;
        return 0;
    }
    overflow_clear_resource_fields();
    s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED;
    return 0;
}

static int overflow_state_can_free(void)
{
    return s_overflow.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        s_overflow.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY;
}

static int overflow_size_fits_sce(size_t size)
{
#if SIZE_MAX > UINT32_MAX
    return size <= UINT32_MAX;
#else
    (void)size;
    return 1;
#endif
}

static int overflow_result_valid(const void *pointer, size_t size)
{
    uintptr_t begin;
    uintptr_t base;

    if (!pointer || !size)
        return 0;
    begin = (uintptr_t)pointer;
    base = (uintptr_t)s_overflow.base;
    if ((begin & (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ALIGNMENT - 1U)) != 0U ||
        begin < base || begin >= s_overflow.end)
        return 0;
    return size <= s_overflow.end - begin;
}

static int overflow_pointer_in_pool(const void *pointer)
{
    uintptr_t value;
    uintptr_t base;

    if (!pointer || !s_overflow.base)
        return 0;
    value = (uintptr_t)pointer;
    base = (uintptr_t)s_overflow.base;
    return value >= base && value < s_overflow.end;
}

static void *overflow_publish_allocation(void *pointer, size_t size)
{
    if (!pointer)
        return NULL; /* Ordinary mspace exhaustion is not a poisoned state. */
    if (!overflow_result_valid(pointer, size)) {
        /* An exact pointer returned inside this mspace can safely be handed
         * back even when its alignment/span violates our stronger contract.
         * A pointer outside the backing range is allocator corruption: do not
         * feed it to MspaceFree.  Account it as permanently stranded and keep
         * the backing alive until process exit. */
        if (overflow_pointer_in_pool(pointer))
            sceClibMspaceFree(s_overflow.mspace, pointer);
        else {
            ++s_overflow.live_count;
            ++s_overflow.stranded_count;
        }
        s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY;
        return NULL;
    }
    ++s_overflow.live_count;
    return pointer;
}

int isaac_vita_heap_overflow_mspace_init(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden)
{
    SceUID uid;
    void *base = NULL;
    uintptr_t begin;
    uintptr_t end;
    uintptr_t retained_end;
    SceClibMspace mspace;

    if (s_overflow.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_UNINITIALIZED)
        return 0;
    if (!overflow_ranges_valid(forbidden)) {
        s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED;
        return 0;
    }

    uid = sceKernelAllocMemBlock(
        "isaac_heap_overflow", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        (SceSize)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES, NULL);
    if (uid < 0) {
        s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED;
        return 0;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base)
        return overflow_fail_after_uid(uid, base, 0U, 0U);

    begin = (uintptr_t)base;
    if ((begin & (ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES - 1U)) != 0U ||
        begin > UINTPTR_MAX -
            ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES)
        return overflow_fail_after_uid(uid, base, 0U, 0U);
    end = begin + ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES;
    retained_end =
        begin + ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES;
    if (overflow_pool_overlaps_forbidden(begin, retained_end, forbidden))
        return overflow_fail_after_uid(uid, base, end, retained_end);

    mspace = sceClibMspaceCreate(
        base, (SceSize)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES);
    if (!mspace)
        return overflow_fail_after_uid(uid, base, end, retained_end);

    s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    s_overflow.uid = uid;
    s_overflow.base = base;
    s_overflow.end = end;
    s_overflow.retained_end = retained_end;
    s_overflow.mspace = mspace;
    s_overflow.live_count = 0U;
    s_overflow.stranded_count = 0U;
    s_overflow.internal_live_count = 0U;
    s_overflow.internal_requested_bytes = 0U;
    return 1;
}

int isaac_vita_heap_overflow_mspace_contains(const void *pointer)
{
    if (!pointer || !overflow_state_can_free())
        return 0;
    return overflow_pointer_in_pool(pointer);
}

void *isaac_vita_heap_overflow_mspace_malloc(size_t size)
{
    void *result;

    if (s_overflow.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        !size || !overflow_size_fits_sce(size))
        return NULL;
    result = sceClibMspaceMalloc(s_overflow.mspace, (SceSize)size);
    return overflow_publish_allocation(result, size);
}

void *isaac_vita_heap_overflow_mspace_calloc(size_t count, size_t size)
{
    size_t total;
    void *result;

    if (s_overflow.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        !count || !size || !overflow_size_fits_sce(count) ||
        !overflow_size_fits_sce(size) ||
        __builtin_mul_overflow(count, size, &total) ||
        !overflow_size_fits_sce(total))
        return NULL;
    result = sceClibMspaceCalloc(
        s_overflow.mspace, (SceSize)count, (SceSize)size);
    return overflow_publish_allocation(result, total);
}

void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void)
{
    const size_t size = ISAAC_VITA_HEAP_OVERFLOW_INTERNAL_PAGE_BYTES;
    void *result;

    if (s_overflow.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        !size || !overflow_size_fits_sce(size) ||
        s_overflow.internal_live_count == SIZE_MAX ||
        size > SIZE_MAX - s_overflow.internal_requested_bytes)
        return NULL;
    result = sceClibMspaceMalloc(s_overflow.mspace, (SceSize)size);
    if (!result)
        return NULL; /* Ordinary mspace exhaustion is retryable/fallback. */
    if (!overflow_result_valid(result, size)) {
        if (overflow_pointer_in_pool(result))
            sceClibMspaceFree(s_overflow.mspace, result);
        else {
            ++s_overflow.live_count;
            ++s_overflow.stranded_count;
        }
        s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY;
        return NULL;
    }
    ++s_overflow.live_count;
    ++s_overflow.internal_live_count;
    s_overflow.internal_requested_bytes += size;
    return result;
}

int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer)
{
    const size_t requested_size =
        ISAAC_VITA_HEAP_OVERFLOW_INTERNAL_PAGE_BYTES;
    if (!pointer || !requested_size || !overflow_state_can_free() ||
        !overflow_result_valid(pointer, requested_size) ||
        !s_overflow.live_count || !s_overflow.internal_live_count ||
        requested_size > s_overflow.internal_requested_bytes)
        return 0;
    sceClibMspaceFree(s_overflow.mspace, pointer);
    --s_overflow.live_count;
    --s_overflow.internal_live_count;
    s_overflow.internal_requested_bytes -= requested_size;
    return 1;
}

void *isaac_vita_heap_overflow_mspace_realloc(
    void *pointer, size_t size,
    isaac_vita_heap_overflow_mspace_realloc_status *status_out)
{
    void *result;

    if (!status_out)
        return NULL;
    *status_out = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_REJECTED;
    if (!pointer) {
        if (!size || !overflow_size_fits_sce(size))
            return NULL;
        result = isaac_vita_heap_overflow_mspace_malloc(size);
        if (result)
            *status_out = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS;
        else if (s_overflow.state ==
                 ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY)
            *status_out =
                ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_OUT_OF_MEMORY;
        return result;
    }
    if (!size) {
        if (isaac_vita_heap_overflow_mspace_free(pointer))
            *status_out = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_FREED;
        return NULL;
    }
    if (s_overflow.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        !overflow_size_fits_sce(size) ||
        !isaac_vita_heap_overflow_mspace_contains(pointer) ||
        !s_overflow.live_count)
        return NULL;
    result = sceClibMspaceRealloc(
        s_overflow.mspace, pointer, (SceSize)size);
    if (!result) {
        *status_out =
            ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_OUT_OF_MEMORY;
        return NULL;
    }
    if (!overflow_result_valid(result, size)) {
        if (overflow_pointer_in_pool(result)) {
            sceClibMspaceFree(s_overflow.mspace, result);
            --s_overflow.live_count;
        }
        else {
            /* The old allocation was consumed by successful realloc and the
             * replacement cannot safely be freed.  Total live count stays
             * unchanged, with that one allocation now permanently stranded. */
            ++s_overflow.stranded_count;
        }
        s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY;
        *status_out =
            ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_CONSUMED_DRAIN_ONLY;
        return NULL;
    }
    *status_out = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS;
    return result;
}

int isaac_vita_heap_overflow_mspace_free(void *pointer)
{
    if (!pointer)
        return 1;
    if (!overflow_state_can_free() || !s_overflow.live_count ||
        !isaac_vita_heap_overflow_mspace_contains(pointer))
        return 0;
    sceClibMspaceFree(s_overflow.mspace, pointer);
    --s_overflow.live_count;
    return 1;
}

int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot_out)
{
    if (!snapshot_out)
        return 0;
    snapshot_out->state = s_overflow.state;
    snapshot_out->uid = (int32_t)s_overflow.uid;
    snapshot_out->base = (uintptr_t)s_overflow.base;
    snapshot_out->end = s_overflow.end;
    snapshot_out->retained_end = s_overflow.retained_end;
    snapshot_out->capacity_bytes =
        overflow_state_can_free() ?
            (size_t)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES : 0U;
    snapshot_out->retained_bytes =
        overflow_state_can_free() ?
            (size_t)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES : 0U;
    snapshot_out->live_count = s_overflow.live_count;
    snapshot_out->stranded_count = s_overflow.stranded_count;
    snapshot_out->internal_live_count = s_overflow.internal_live_count;
    snapshot_out->internal_requested_bytes =
        s_overflow.internal_requested_bytes;
    snapshot_out->has_mspace = s_overflow.mspace != NULL;
    return 1;
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING
int isaac_vita_heap_overflow_mspace_test_reset(void)
{
    int free_result;

    if (s_overflow.live_count || s_overflow.internal_live_count ||
        s_overflow.internal_requested_bytes)
        return 0;
    if (s_overflow.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        s_overflow.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY) {
        sceClibMspaceDestroy(s_overflow.mspace);
        s_overflow.mspace = NULL;
    }
    if (s_overflow.uid >= 0) {
        free_result = sceKernelFreeMemBlock(s_overflow.uid);
        if (free_result < 0) {
            s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED;
            return 0;
        }
    }
    overflow_clear_resource_fields();
    s_overflow.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_UNINITIALIZED;
    return 1;
}
#endif
