/* Process-lifetime allocation domain for the native OpenAL mixer. */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/mutex.h>

#include "host_vita_openal_pool.h"

#ifndef ISAAC_VITA_OPENAL_POOL_ACTIVE
#define ISAAC_VITA_OPENAL_POOL_ACTIVE 0
#endif

#define OPENAL_POOL_HEADER_MAGIC 0x4f414c50U
#define OPENAL_POOL_HEADER_DEAD  0x4f414c58U
#define OPENAL_POOL_DEFAULT_ALIGNMENT ((size_t)_Alignof(max_align_t))

_Static_assert((ISAAC_VITA_OPENAL_POOL_ALIGNMENT &
                (ISAAC_VITA_OPENAL_POOL_ALIGNMENT - 1U)) == 0U,
               "OpenAL pool alignment must be a power of two");
_Static_assert(ISAAC_VITA_OPENAL_POOL_MIN_REQUEST_ALIGNMENT <=
                   ISAAC_VITA_OPENAL_POOL_ALIGNMENT &&
               (ISAAC_VITA_OPENAL_POOL_MIN_REQUEST_ALIGNMENT &
                (ISAAC_VITA_OPENAL_POOL_MIN_REQUEST_ALIGNMENT - 1U)) == 0U,
               "OpenAL aligned-allocation request contract changed");
_Static_assert(ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT >=
                   ISAAC_VITA_OPENAL_POOL_ALIGNMENT &&
               (ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT &
                (ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT - 1U)) == 0U,
               "OpenAL pool maximum alignment contract changed");
_Static_assert(OPENAL_POOL_DEFAULT_ALIGNMENT >=
                   ISAAC_VITA_OPENAL_POOL_ALIGNMENT &&
               OPENAL_POOL_DEFAULT_ALIGNMENT <=
                   ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT &&
               (OPENAL_POOL_DEFAULT_ALIGNMENT &
                (OPENAL_POOL_DEFAULT_ALIGNMENT - 1U)) == 0U,
               "OpenAL pool does not satisfy the C malloc alignment contract");
#ifdef __vita__
_Static_assert(OPENAL_POOL_DEFAULT_ALIGNMENT ==
                   ISAAC_VITA_OPENAL_POOL_ALIGNMENT,
               "Vita max_align_t changed the frozen 8-byte allocator ABI");
#endif
_Static_assert((ISAAC_VITA_OPENAL_POOL_BYTES & 0x00000fffU) == 0U,
               "OpenAL pool request must be page aligned");
_Static_assert((ISAAC_VITA_OPENAL_POOL_BYTES &
                (~ISAAC_VITA_OPENAL_POOL_BYTES + 1U)) == 0x00002000U,
               "OpenAL pool request low-bit receipt changed");
_Static_assert(ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES ==
                   ISAAC_VITA_OPENAL_POOL_BYTES + 0x00002000U,
               "OpenAL pool retained-span receipt changed");

typedef struct openal_pool_header {
    uintptr_t raw;
    uint32_t requested;
    uint32_t alignment;
    uint32_t magic;
} openal_pool_header;

typedef struct openal_pool_storage {
    atomic_uint state;
    SceUID uid;
    SceUID mutex_uid;
    void *base;
    uintptr_t end;
    uintptr_t retained_end;
    SceClibMspace mspace;
    uint32_t live_count;
    uint32_t stranded_count;
    uint32_t live_requested_bytes;
    uint32_t peak_live_count;
    uint32_t peak_requested_bytes;
    uint32_t allocations;
    uint32_t frees;
    uint32_t reallocations;
    uint32_t failures;
    atomic_uint corruption;
    atomic_uint preinit_native_allocations;
    atomic_uint preinit_inflight;
    isaac_vita_openal_pool_init_failure init_failure;
} openal_pool_storage;

static openal_pool_storage s_openal_pool = {
    .state = ATOMIC_VAR_INIT(ISAAC_VITA_OPENAL_POOL_UNINITIALIZED),
    .uid = (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID,
    .mutex_uid = (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID,
    .corruption = ATOMIC_VAR_INIT(0U),
    .preinit_native_allocations = ATOMIC_VAR_INIT(0U),
    .preinit_inflight = ATOMIC_VAR_INIT(0U),
    .init_failure = ISAAC_VITA_OPENAL_POOL_INIT_OK
};

static isaac_vita_openal_pool_state openal_pool_state_load(void)
{
    return (isaac_vita_openal_pool_state)atomic_load_explicit(
        &s_openal_pool.state, memory_order_acquire);
}

static void openal_pool_state_store(isaac_vita_openal_pool_state state)
{
    atomic_store_explicit(&s_openal_pool.state, (unsigned)state,
                          memory_order_release);
}

static int openal_pool_pointer_in_range(const void *pointer)
{
    isaac_vita_openal_pool_state state = openal_pool_state_load();
    uintptr_t value;
    uintptr_t begin;

    if (state != ISAAC_VITA_OPENAL_POOL_READY &&
        state != ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY)
        return 0;
    if (!pointer || !s_openal_pool.base)
        return 0;
    value = (uintptr_t)pointer;
    begin = (uintptr_t)s_openal_pool.base;
    return value >= begin && value < s_openal_pool.end;
}

static int openal_pool_lock(void)
{
    if (s_openal_pool.mutex_uid < 0 ||
        sceKernelLockMutex(s_openal_pool.mutex_uid, 1, NULL) < 0) {
        atomic_store_explicit(&s_openal_pool.corruption, 1U,
                              memory_order_release);
        openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY);
        return 0;
    }
    return 1;
}

static int openal_pool_unlock(void)
{
    if (sceKernelUnlockMutex(s_openal_pool.mutex_uid, 1) < 0) {
        atomic_store_explicit(&s_openal_pool.corruption, 1U,
                              memory_order_release);
        openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY);
        return 0;
    }
    return 1;
}

static int openal_pool_size_fits(size_t size)
{
#if SIZE_MAX > UINT32_MAX
    return size <= UINT32_MAX;
#else
    (void)size;
    return 1;
#endif
}

static void openal_pool_note_failure_locked(void)
{
    if (s_openal_pool.failures != UINT32_MAX)
        ++s_openal_pool.failures;
}

static void openal_pool_poison_locked(void)
{
    atomic_store_explicit(&s_openal_pool.corruption, 1U,
                          memory_order_release);
    openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY);
}

static int openal_pool_alignment_valid(size_t alignment)
{
    return alignment >= ISAAC_VITA_OPENAL_POOL_ALIGNMENT &&
        alignment <= ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT &&
        (alignment & (alignment - 1U)) == 0U;
}

static int openal_pool_requested_alignment_valid(size_t alignment)
{
    return alignment >= ISAAC_VITA_OPENAL_POOL_MIN_REQUEST_ALIGNMENT &&
        alignment <= ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT &&
        (alignment & (alignment - 1U)) == 0U;
}

static int openal_pool_header_locked(void *pointer,
                                     openal_pool_header **header_out)
{
    uintptr_t value;
    uintptr_t begin;
    openal_pool_header *header;

    if (!pointer || !header_out ||
        ((uintptr_t)pointer & (ISAAC_VITA_OPENAL_POOL_ALIGNMENT - 1U)) != 0U)
        return 0;
    value = (uintptr_t)pointer;
    begin = (uintptr_t)s_openal_pool.base;
    if (value < begin + sizeof(*header) || value >= s_openal_pool.end)
        return 0;
    header = (openal_pool_header *)(value - sizeof(*header));
    if (header->magic != OPENAL_POOL_HEADER_MAGIC ||
        header->raw < begin || header->raw >= s_openal_pool.end ||
        !openal_pool_alignment_valid(header->alignment) ||
        (value & ((uintptr_t)header->alignment - 1U)) != 0U ||
        header->raw > (uintptr_t)header ||
        (uintptr_t)header - header->raw >= header->alignment ||
        header->requested > s_openal_pool.end - value)
        return 0;
    *header_out = header;
    return 1;
}

static void *openal_pool_allocate_locked(size_t requested, size_t alignment)
{
    size_t payload = requested ? requested : 1U;
    size_t total;
    void *raw;
    uintptr_t aligned;
    openal_pool_header *header;

    if (openal_pool_state_load() != ISAAC_VITA_OPENAL_POOL_READY ||
        !openal_pool_alignment_valid(alignment) ||
        !openal_pool_size_fits(requested) ||
        payload > SIZE_MAX - sizeof(*header) -
                      (alignment - 1U)) {
        openal_pool_note_failure_locked();
        return NULL;
    }
    total = payload + sizeof(*header) +
        (alignment - 1U);
    if (!openal_pool_size_fits(total)) {
        openal_pool_note_failure_locked();
        return NULL;
    }
    raw = sceClibMspaceMalloc(s_openal_pool.mspace, (SceSize)total);
    if (!raw) {
        openal_pool_note_failure_locked();
        return NULL;
    }
    if (!openal_pool_pointer_in_range(raw) ||
        total > s_openal_pool.end - (uintptr_t)raw) {
        if (openal_pool_pointer_in_range(raw))
            sceClibMspaceFree(s_openal_pool.mspace, raw);
        else if (s_openal_pool.stranded_count != UINT32_MAX)
            ++s_openal_pool.stranded_count;
        openal_pool_poison_locked();
        return NULL;
    }
    aligned = ((uintptr_t)raw + sizeof(*header) +
               (alignment - 1U)) & ~(uintptr_t)(alignment - 1U);
    if (aligned < (uintptr_t)raw || payload > s_openal_pool.end - aligned) {
        sceClibMspaceFree(s_openal_pool.mspace, raw);
        openal_pool_poison_locked();
        return NULL;
    }
    header = (openal_pool_header *)(aligned - sizeof(*header));
    header->raw = (uintptr_t)raw;
    header->requested = (uint32_t)requested;
    header->alignment = (uint32_t)alignment;
    header->magic = OPENAL_POOL_HEADER_MAGIC;
    if (s_openal_pool.live_count == UINT32_MAX ||
        requested > UINT32_MAX - s_openal_pool.live_requested_bytes ||
        s_openal_pool.allocations == UINT32_MAX) {
        header->magic = OPENAL_POOL_HEADER_DEAD;
        sceClibMspaceFree(s_openal_pool.mspace, raw);
        openal_pool_poison_locked();
        return NULL;
    }
    ++s_openal_pool.live_count;
    s_openal_pool.live_requested_bytes += (uint32_t)requested;
    ++s_openal_pool.allocations;
    if (s_openal_pool.live_count > s_openal_pool.peak_live_count)
        s_openal_pool.peak_live_count = s_openal_pool.live_count;
    if (s_openal_pool.live_requested_bytes >
        s_openal_pool.peak_requested_bytes)
        s_openal_pool.peak_requested_bytes =
            s_openal_pool.live_requested_bytes;
    return (void *)aligned;
}

static int openal_pool_free_locked(void *pointer)
{
    openal_pool_header *header;
    void *raw;
    uint32_t requested;

    if (!openal_pool_header_locked(pointer, &header) ||
        !s_openal_pool.live_count ||
        header->requested > s_openal_pool.live_requested_bytes ||
        s_openal_pool.frees == UINT32_MAX) {
        openal_pool_poison_locked();
        return 0;
    }
    raw = (void *)header->raw;
    requested = header->requested;
    header->magic = OPENAL_POOL_HEADER_DEAD;
    --s_openal_pool.live_count;
    s_openal_pool.live_requested_bytes -= requested;
    ++s_openal_pool.frees;
    sceClibMspaceFree(s_openal_pool.mspace, raw);
    return 1;
}

#ifdef ISAAC_VITA_OPENAL_POOL_TESTING
static void openal_pool_clear(void)
{
    s_openal_pool.uid = (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
    s_openal_pool.mutex_uid = (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
    s_openal_pool.base = NULL;
    s_openal_pool.end = 0U;
    s_openal_pool.retained_end = 0U;
    s_openal_pool.mspace = NULL;
    s_openal_pool.live_count = 0U;
    s_openal_pool.stranded_count = 0U;
    s_openal_pool.live_requested_bytes = 0U;
    s_openal_pool.peak_live_count = 0U;
    s_openal_pool.peak_requested_bytes = 0U;
    s_openal_pool.allocations = 0U;
    s_openal_pool.frees = 0U;
    s_openal_pool.reallocations = 0U;
    s_openal_pool.failures = 0U;
    atomic_store_explicit(&s_openal_pool.corruption, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_openal_pool.preinit_native_allocations, 0U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_openal_pool.preinit_inflight, 0U,
                          memory_order_relaxed);
    s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_OK;
}
#endif

static void openal_pool_preinit_note_allocation(void *pointer)
{
    unsigned value;

    if (!pointer)
        return;
    value = atomic_load_explicit(
        &s_openal_pool.preinit_native_allocations, memory_order_seq_cst);
    while (value != UINT32_MAX &&
           !atomic_compare_exchange_weak_explicit(
               &s_openal_pool.preinit_native_allocations, &value, value + 1U,
               memory_order_seq_cst, memory_order_seq_cst)) {
    }
}

static int openal_pool_preinit_begin(void)
{
    (void)atomic_fetch_add_explicit(&s_openal_pool.preinit_inflight, 1U,
                                    memory_order_seq_cst);
    if ((isaac_vita_openal_pool_state)atomic_load_explicit(
            &s_openal_pool.state, memory_order_seq_cst) !=
        ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
        (void)atomic_fetch_sub_explicit(&s_openal_pool.preinit_inflight, 1U,
                                        memory_order_seq_cst);
        return 0;
    }
    return 1;
}

static void openal_pool_preinit_end(void)
{
    (void)atomic_fetch_sub_explicit(&s_openal_pool.preinit_inflight, 1U,
                                    memory_order_seq_cst);
}

static int openal_pool_ranges_overlap(uintptr_t first_begin,
                                      uintptr_t first_end,
                                      uintptr_t second_begin,
                                      uintptr_t second_end)
{
    return first_begin < second_end && second_begin < first_end;
}

int isaac_vita_openal_pool_initialize(uintptr_t fixed_image_begin,
                                      uintptr_t fixed_image_end)
{
    unsigned expected = ISAAC_VITA_OPENAL_POOL_UNINITIALIZED;
    SceUID mutex_uid;
    SceUID uid;
    void *base = NULL;
    uintptr_t begin;
    SceClibMspace mspace;
    int orphaned = 0;

    if (!atomic_compare_exchange_strong_explicit(
            &s_openal_pool.state, &expected,
            ISAAC_VITA_OPENAL_POOL_INITIALIZING,
            memory_order_seq_cst, memory_order_seq_cst))
        return expected == ISAAC_VITA_OPENAL_POOL_READY;
#if !ISAAC_VITA_OPENAL_POOL_ACTIVE
    s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_DISABLED;
    openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_FALLBACK);
    return 0;
#endif
    while (atomic_load_explicit(&s_openal_pool.preinit_inflight,
                                memory_order_seq_cst) != 0U) {
    }
    if (atomic_load_explicit(&s_openal_pool.preinit_native_allocations,
                             memory_order_seq_cst) != 0U) {
        s_openal_pool.init_failure =
            ISAAC_VITA_OPENAL_POOL_INIT_PREINIT_ALLOCATION;
        openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_FALLBACK);
        return 0;
    }
    if (!fixed_image_begin || fixed_image_begin >= fixed_image_end) {
        s_openal_pool.init_failure =
            ISAAC_VITA_OPENAL_POOL_INIT_INVALID_FIXED_IMAGE;
        openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_FALLBACK);
        return 0;
    }
    mutex_uid = sceKernelCreateMutex(
        "isaac_openal_pool", 0U, 0, NULL);
    if (mutex_uid < 0) {
        s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_MUTEX;
        openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_FALLBACK);
        return 0;
    }
    uid = sceKernelAllocMemBlock(
        "isaac_openal_pool", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        (SceSize)ISAAC_VITA_OPENAL_POOL_BYTES, NULL);
    if (uid < 0) {
        s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_MEMBLOCK;
        if (sceKernelDeleteMutex(mutex_uid) < 0)
            orphaned = 1;
        s_openal_pool.mutex_uid = orphaned ? mutex_uid :
            (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
        if (orphaned)
            s_openal_pool.init_failure =
                ISAAC_VITA_OPENAL_POOL_INIT_ROLLBACK;
        openal_pool_state_store(orphaned ?
            ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK :
            ISAAC_VITA_OPENAL_POOL_FALLBACK);
        return 0;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_GET_BASE;
        goto init_failed;
    }
    begin = (uintptr_t)base;
    if ((begin & 0x00000fffU) != 0U ||
        begin > UINTPTR_MAX - ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES) {
        s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_ADDRESS;
        goto init_failed;
    }
    if (openal_pool_ranges_overlap(
            begin, begin + ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES,
            fixed_image_begin, fixed_image_end)) {
        s_openal_pool.init_failure =
            ISAAC_VITA_OPENAL_POOL_INIT_FIXED_IMAGE_OVERLAP;
        goto init_failed;
    }
    mspace = sceClibMspaceCreate(
        base, (SceSize)ISAAC_VITA_OPENAL_POOL_BYTES);
    if (!mspace) {
        s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_MSPACE;
        goto init_failed;
    }

    s_openal_pool.uid = uid;
    s_openal_pool.mutex_uid = mutex_uid;
    s_openal_pool.base = base;
    s_openal_pool.end = begin + ISAAC_VITA_OPENAL_POOL_BYTES;
    s_openal_pool.retained_end =
        begin + ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES;
    s_openal_pool.mspace = mspace;
    s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_OK;
    openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_READY);
    return 1;

init_failed:
    s_openal_pool.uid = uid;
    s_openal_pool.mutex_uid = mutex_uid;
    s_openal_pool.base = base;
    if (sceKernelFreeMemBlock(uid) < 0)
        orphaned = 1;
    else {
        s_openal_pool.uid = (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
        s_openal_pool.base = NULL;
    }
    if (sceKernelDeleteMutex(mutex_uid) < 0)
        orphaned = 1;
    else
        s_openal_pool.mutex_uid =
            (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
    if (orphaned)
        s_openal_pool.init_failure = ISAAC_VITA_OPENAL_POOL_INIT_ROLLBACK;
    openal_pool_state_store(orphaned ?
        ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK :
        ISAAC_VITA_OPENAL_POOL_FALLBACK);
    return 0;
}

void *isaac_vita_openal_pool_malloc(size_t size)
{
    isaac_vita_openal_pool_state state = openal_pool_state_load();
    void *result;

    if (state == ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
        if (!openal_pool_preinit_begin())
            return isaac_vita_openal_pool_malloc(size);
        result = malloc(size);
        openal_pool_preinit_note_allocation(result);
        openal_pool_preinit_end();
        return result;
    }
    if (state == ISAAC_VITA_OPENAL_POOL_FALLBACK ||
        state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK)
        return malloc(size);
    if (state != ISAAC_VITA_OPENAL_POOL_READY)
        return NULL;
    if (!openal_pool_lock())
        return NULL;
    result = openal_pool_allocate_locked(
        size, OPENAL_POOL_DEFAULT_ALIGNMENT);
    (void)openal_pool_unlock();
    return result;
}

void *isaac_vita_openal_pool_calloc(size_t count, size_t size)
{
    isaac_vita_openal_pool_state state = openal_pool_state_load();
    size_t total;
    void *result;

    if (state == ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
        if (!openal_pool_preinit_begin())
            return isaac_vita_openal_pool_calloc(count, size);
        result = calloc(count, size);
        openal_pool_preinit_note_allocation(result);
        openal_pool_preinit_end();
        return result;
    }
    if (state == ISAAC_VITA_OPENAL_POOL_FALLBACK ||
        state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK)
        return calloc(count, size);
    if (state != ISAAC_VITA_OPENAL_POOL_READY ||
        __builtin_mul_overflow(count, size, &total))
        return NULL;
    if (!openal_pool_lock())
        return NULL;
    result = openal_pool_allocate_locked(
        total, OPENAL_POOL_DEFAULT_ALIGNMENT);
    (void)openal_pool_unlock();
    if (result)
        memset(result, 0, total);
    return result;
}

void *isaac_vita_openal_pool_realloc(void *pointer, size_t size)
{
    isaac_vita_openal_pool_state state;
    openal_pool_header *header;
    void *replacement;
    size_t copy_size;

    if (!pointer)
        return isaac_vita_openal_pool_malloc(size);
    state = openal_pool_state_load();
    if (state == ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
        if (!openal_pool_preinit_begin())
            return isaac_vita_openal_pool_realloc(pointer, size);
        /* A non-NULL native pointer is already outstanding even when libc
         * realloc subsequently fails.  Publish that ownership before the
         * native call so initialize cannot select the pool concurrently. */
        openal_pool_preinit_note_allocation(pointer);
        replacement = realloc(pointer, size);
        openal_pool_preinit_end();
        return replacement;
    }
    if (state == ISAAC_VITA_OPENAL_POOL_FALLBACK ||
        state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK)
        return realloc(pointer, size);
    if (state == ISAAC_VITA_OPENAL_POOL_INITIALIZING)
        return NULL;
    if (!openal_pool_pointer_in_range(pointer))
        return realloc(pointer, size);
    if (!size) {
        isaac_vita_openal_pool_free(pointer);
        return NULL;
    }
    if (state != ISAAC_VITA_OPENAL_POOL_READY || !openal_pool_lock())
        return NULL;
    if (!openal_pool_header_locked(pointer, &header)) {
        openal_pool_poison_locked();
        (void)openal_pool_unlock();
        return NULL;
    }
    copy_size = header->requested < size ? header->requested : size;
    replacement = openal_pool_allocate_locked(size, header->alignment);
    if (!replacement) {
        (void)openal_pool_unlock();
        return NULL;
    }
    memcpy(replacement, pointer, copy_size);
    if (!openal_pool_free_locked(pointer)) {
        (void)openal_pool_free_locked(replacement);
        (void)openal_pool_unlock();
        return NULL;
    }
    if (s_openal_pool.reallocations == UINT32_MAX)
        openal_pool_poison_locked();
    else
        ++s_openal_pool.reallocations;
    (void)openal_pool_unlock();
    return replacement;
}

void *isaac_vita_openal_pool_aligned_alloc(size_t alignment, size_t size)
{
    isaac_vita_openal_pool_state state = openal_pool_state_load();
    size_t effective_alignment;
    void *result;

    if (state == ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
        if (!openal_pool_preinit_begin())
            return isaac_vita_openal_pool_aligned_alloc(alignment, size);
        result = aligned_alloc(alignment, size);
        openal_pool_preinit_note_allocation(result);
        openal_pool_preinit_end();
        return result;
    }
    if (state == ISAAC_VITA_OPENAL_POOL_FALLBACK ||
        state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK)
        return aligned_alloc(alignment, size);
    if (state != ISAAC_VITA_OPENAL_POOL_READY ||
        !openal_pool_requested_alignment_valid(alignment) ||
        (size & (alignment - 1U)) != 0U)
        return NULL;
    effective_alignment = alignment < OPENAL_POOL_DEFAULT_ALIGNMENT ?
        OPENAL_POOL_DEFAULT_ALIGNMENT : alignment;
    if (!openal_pool_lock())
        return NULL;
    result = openal_pool_allocate_locked(size, effective_alignment);
    (void)openal_pool_unlock();
    return result;
}

char *isaac_vita_openal_pool_strdup(const char *string)
{
    size_t length;
    char *copy;

    if (!string)
        return NULL;
    length = strlen(string);
    if (length == SIZE_MAX)
        return NULL;
    copy = (char *)isaac_vita_openal_pool_malloc(length + 1U);
    if (copy)
        memcpy(copy, string, length + 1U);
    return copy;
}

void isaac_vita_openal_pool_free(void *pointer)
{
    if (!pointer)
        return;
    if (!openal_pool_pointer_in_range(pointer)) {
        free(pointer);
        return;
    }
    if (!openal_pool_lock())
        return;
    (void)openal_pool_free_locked(pointer);
    (void)openal_pool_unlock();
}

int isaac_vita_openal_pool_snapshot_get(
    isaac_vita_openal_pool_snapshot *snapshot_out)
{
    isaac_vita_openal_pool_state state;
    int locked = 0;

    if (!snapshot_out)
        return 0;
    state = openal_pool_state_load();
    if (state == ISAAC_VITA_OPENAL_POOL_INITIALIZING)
        return 0;
    if (state == ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
        /* Do not inspect any plain resource field: initialize may win its
         * UNINITIALIZED->INITIALIZING CAS immediately after our state load.
         * A canonical pre-init snapshot is race-free and sufficient to decide
         * whether direct allocator calls have already selected libc. */
        memset(snapshot_out, 0, sizeof *snapshot_out);
        snapshot_out->state = ISAAC_VITA_OPENAL_POOL_UNINITIALIZED;
        snapshot_out->uid = ISAAC_VITA_OPENAL_POOL_INVALID_UID;
        snapshot_out->mutex_uid = ISAAC_VITA_OPENAL_POOL_INVALID_UID;
        snapshot_out->preinit_native_allocations = atomic_load_explicit(
            &s_openal_pool.preinit_native_allocations,
            memory_order_acquire);
        snapshot_out->init_failure = ISAAC_VITA_OPENAL_POOL_INIT_OK;
        return 1;
    }
    if ((state == ISAAC_VITA_OPENAL_POOL_READY ||
         state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY) &&
        s_openal_pool.mutex_uid >= 0) {
        if (!openal_pool_lock())
            return 0;
        locked = 1;
        state = openal_pool_state_load();
    }
    snapshot_out->state = state;
    snapshot_out->uid = (int32_t)s_openal_pool.uid;
    snapshot_out->mutex_uid = (int32_t)s_openal_pool.mutex_uid;
    snapshot_out->base = (uintptr_t)s_openal_pool.base;
    snapshot_out->end = s_openal_pool.end;
    snapshot_out->retained_end = s_openal_pool.retained_end;
    snapshot_out->backing_bytes =
        s_openal_pool.mspace ? ISAAC_VITA_OPENAL_POOL_BYTES : 0U;
    snapshot_out->live_count = s_openal_pool.live_count;
    snapshot_out->stranded_count = s_openal_pool.stranded_count;
    snapshot_out->live_requested_bytes =
        s_openal_pool.live_requested_bytes;
    snapshot_out->peak_live_count = s_openal_pool.peak_live_count;
    snapshot_out->peak_requested_bytes =
        s_openal_pool.peak_requested_bytes;
    snapshot_out->allocations = s_openal_pool.allocations;
    snapshot_out->frees = s_openal_pool.frees;
    snapshot_out->reallocations = s_openal_pool.reallocations;
    snapshot_out->failures = s_openal_pool.failures;
    snapshot_out->corruption = atomic_load_explicit(
        &s_openal_pool.corruption, memory_order_acquire);
    snapshot_out->preinit_native_allocations = atomic_load_explicit(
        &s_openal_pool.preinit_native_allocations, memory_order_acquire);
    snapshot_out->init_failure = s_openal_pool.init_failure;
    snapshot_out->has_mspace = s_openal_pool.mspace != NULL;
    if (locked && !openal_pool_unlock())
        return 0;
    return 1;
}

#ifdef ISAAC_VITA_OPENAL_POOL_TESTING
int isaac_vita_openal_pool_test_reset(void)
{
    isaac_vita_openal_pool_state state = openal_pool_state_load();
    int result = 1;

    if (s_openal_pool.live_count || s_openal_pool.stranded_count)
        return 0;
    if ((state == ISAAC_VITA_OPENAL_POOL_READY ||
         state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY) &&
        s_openal_pool.mspace) {
        sceClibMspaceDestroy(s_openal_pool.mspace);
        s_openal_pool.mspace = NULL;
    }
    if (s_openal_pool.uid >= 0) {
        if (sceKernelFreeMemBlock(s_openal_pool.uid) < 0)
            result = 0;
        else {
            s_openal_pool.uid =
                (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
            s_openal_pool.base = NULL;
            s_openal_pool.end = 0U;
            s_openal_pool.retained_end = 0U;
        }
    }
    if (s_openal_pool.mutex_uid >= 0) {
        if (sceKernelDeleteMutex(s_openal_pool.mutex_uid) < 0)
            result = 0;
        else
            s_openal_pool.mutex_uid =
                (SceUID)ISAAC_VITA_OPENAL_POOL_INVALID_UID;
    }
    if (!result) {
        openal_pool_state_store(
            ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK);
        return 0;
    }
    openal_pool_clear();
    openal_pool_state_store(ISAAC_VITA_OPENAL_POOL_UNINITIALIZED);
    return 1;
}
#endif
