#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr/mutex.h>

#include "host_vita_openal_pool.h"

#define FAKE_MEMBLOCK_UID ((SceUID)0x4f11)
#define FAKE_MUTEX_UID    ((SceUID)0x4f12)
#define FAKE_RECORD_COUNT 128U

typedef struct fake_record {
    unsigned char *pointer;
    size_t size;
    int live;
} fake_record;

typedef enum fake_base_mode {
    FAKE_BASE_NORMAL = 0,
    FAKE_BASE_NULL,
    FAKE_BASE_MISALIGNED,
    FAKE_BASE_OVERFLOW
} fake_base_mode;

typedef struct fake_mspace {
    unsigned marker;
} fake_mspace;

_Alignas(4096) static unsigned char
    s_backing[ISAAC_VITA_OPENAL_POOL_BYTES];
static fake_record s_records[FAKE_RECORD_COUNT];
static fake_mspace s_mspace;
static pthread_mutex_t s_mutex;
static size_t s_bump;
static int s_memblock_live;
static int s_mutex_live;
static int s_mspace_live;
static int s_fail_memblock_alloc;
static int s_fail_memblock_get;
static int s_fail_memblock_free;
static int s_fail_mutex_create;
static int s_fail_mutex_delete;
static int s_fail_mutex_lock;
static int s_fail_mutex_unlock;
static int s_fail_mspace_create;
static int s_fail_mspace_malloc;
static atomic_int s_pause_mutex_create;
static atomic_int s_mutex_create_entered;
static atomic_int s_release_mutex_create;
static int s_return_span_failure;
static int s_return_out_of_range;
static fake_base_mode s_base_mode;
static unsigned s_memblock_alloc_calls;
static unsigned s_memblock_get_calls;
static unsigned s_memblock_free_calls;
static unsigned s_mutex_create_calls;
static unsigned s_mutex_delete_calls;
static atomic_uint s_mutex_lock_calls;
static unsigned s_mutex_unlock_calls;
static unsigned s_mspace_create_calls;
static unsigned s_mspace_destroy_calls;
static unsigned s_mspace_malloc_calls;
static unsigned s_mspace_free_calls;
static atomic_uint s_native_malloc_calls;
static atomic_uint s_native_calloc_calls;
static atomic_uint s_native_realloc_calls;
static atomic_uint s_native_free_calls;
static atomic_uint s_native_aligned_calls;
static atomic_int s_fail_native_realloc;
static unsigned s_bad_calls;

#ifndef ISAAC_VITA_OPENAL_POOL_ORACLE_TSAN
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__real_aligned_alloc(size_t alignment, size_t size);

void *__wrap_malloc(size_t size)
{
    (void)atomic_fetch_add_explicit(
        &s_native_malloc_calls, 1U, memory_order_relaxed);
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    (void)atomic_fetch_add_explicit(
        &s_native_calloc_calls, 1U, memory_order_relaxed);
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    (void)atomic_fetch_add_explicit(
        &s_native_realloc_calls, 1U, memory_order_relaxed);
    if (atomic_exchange_explicit(&s_fail_native_realloc, 0,
                                 memory_order_relaxed))
        return NULL;
    return __real_realloc(pointer, size);
}

void __wrap_free(void *pointer)
{
    (void)atomic_fetch_add_explicit(
        &s_native_free_calls, 1U, memory_order_relaxed);
    __real_free(pointer);
}

void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    (void)atomic_fetch_add_explicit(
        &s_native_aligned_calls, 1U, memory_order_relaxed);
    return __real_aligned_alloc(alignment, size);
}
#else
#define __real_malloc malloc
#define __real_calloc calloc
#define __real_realloc realloc
#define __real_free free
#define __real_aligned_alloc aligned_alloc
#endif

#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);     \
        return 1;                                                          \
    }                                                                      \
} while (0)

static unsigned native_malloc_calls(void)
{
    return atomic_load_explicit(&s_native_malloc_calls,
                                memory_order_relaxed);
}

static unsigned native_calloc_calls(void)
{
    return atomic_load_explicit(&s_native_calloc_calls,
                                memory_order_relaxed);
}

static unsigned native_realloc_calls(void)
{
    return atomic_load_explicit(&s_native_realloc_calls,
                                memory_order_relaxed);
}

static unsigned native_free_calls(void)
{
    return atomic_load_explicit(&s_native_free_calls,
                                memory_order_relaxed);
}

static unsigned native_aligned_calls(void)
{
    return atomic_load_explicit(&s_native_aligned_calls,
                                memory_order_relaxed);
}

static fake_record *record_find(void *pointer)
{
    size_t index;

    for (index = 0U; index < FAKE_RECORD_COUNT; ++index) {
        if (s_records[index].live && s_records[index].pointer == pointer)
            return &s_records[index];
    }
    return NULL;
}

static size_t record_live_count(void)
{
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < FAKE_RECORD_COUNT; ++index)
        if (s_records[index].live)
            ++count;
    return count;
}

static void *fake_allocate(size_t size)
{
    fake_record *record = NULL;
    unsigned char *pointer;
    size_t index;
    size_t offset;

    for (index = 0U; index < FAKE_RECORD_COUNT; ++index) {
        if (!s_records[index].live) {
            record = &s_records[index];
            break;
        }
    }
    if (!record || !size)
        return NULL;
    if (s_return_out_of_range) {
        s_return_out_of_range = 0;
        record->pointer = &s_backing[sizeof s_backing];
        record->size = size;
        record->live = 1;
        return record->pointer;
    }
    if (s_return_span_failure) {
        s_return_span_failure = 0;
        record->pointer = &s_backing[sizeof s_backing - 8U];
        record->size = size;
        record->live = 1;
        return record->pointer;
    }
    offset = (s_bump + 15U) & ~(size_t)15U;
    if (offset > sizeof s_backing || size > sizeof s_backing - offset)
        return NULL;
    pointer = &s_backing[offset];
    record->pointer = pointer;
    record->size = size;
    record->live = 1;
    s_bump = offset + size + 16U;
    memset(pointer, 0x6d, size);
    return pointer;
}

SceUID sceKernelCreateMutex(const char *name, SceUInt attr, int init_count,
                            SceKernelMutexOptParam *option)
{
    ++s_mutex_create_calls;
    if (atomic_load_explicit(&s_pause_mutex_create, memory_order_acquire)) {
        atomic_store_explicit(&s_mutex_create_entered, 1,
                              memory_order_release);
        while (!atomic_load_explicit(&s_release_mutex_create,
                                     memory_order_acquire)) {
        }
    }
    if (s_fail_mutex_create) {
        --s_fail_mutex_create;
        return -101;
    }
    if (!name || strcmp(name, "isaac_openal_pool") != 0 || attr != 0U ||
        init_count != 0 || option || s_mutex_live ||
        pthread_mutex_init(&s_mutex, NULL) != 0) {
        ++s_bad_calls;
        return -102;
    }
    s_mutex_live = 1;
    return FAKE_MUTEX_UID;
}

int sceKernelDeleteMutex(SceUID uid)
{
    ++s_mutex_delete_calls;
    if (s_fail_mutex_delete) {
        --s_fail_mutex_delete;
        return -111;
    }
    if (uid != FAKE_MUTEX_UID || !s_mutex_live ||
        pthread_mutex_destroy(&s_mutex) != 0) {
        ++s_bad_calls;
        return -112;
    }
    s_mutex_live = 0;
    return 0;
}

int sceKernelLockMutex(SceUID uid, int count, unsigned int *timeout)
{
    (void)atomic_fetch_add_explicit(
        &s_mutex_lock_calls, 1U, memory_order_relaxed);
    if (s_fail_mutex_lock) {
        --s_fail_mutex_lock;
        return -121;
    }
    if (uid != FAKE_MUTEX_UID || !s_mutex_live || count != 1 || timeout ||
        pthread_mutex_lock(&s_mutex) != 0) {
        ++s_bad_calls;
        return -122;
    }
    return 0;
}

int sceKernelUnlockMutex(SceUID uid, int count)
{
    int injected = 0;

    ++s_mutex_unlock_calls;
    if (uid != FAKE_MUTEX_UID || !s_mutex_live || count != 1) {
        ++s_bad_calls;
        return -132;
    }
    if (s_fail_mutex_unlock) {
        --s_fail_mutex_unlock;
        injected = 1;
    }
    if (pthread_mutex_unlock(&s_mutex) != 0) {
        ++s_bad_calls;
        return -133;
    }
    return injected ? -131 : 0;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option)
{
    ++s_memblock_alloc_calls;
    if (s_fail_memblock_alloc) {
        --s_fail_memblock_alloc;
        return -201;
    }
    if (!name || strcmp(name, "isaac_openal_pool") != 0 ||
        type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW ||
        size != ISAAC_VITA_OPENAL_POOL_BYTES || option || s_memblock_live) {
        ++s_bad_calls;
        return -202;
    }
    s_memblock_live = 1;
    return FAKE_MEMBLOCK_UID;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    ++s_memblock_get_calls;
    if (s_fail_memblock_get) {
        --s_fail_memblock_get;
        return -211;
    }
    if (uid != FAKE_MEMBLOCK_UID || !s_memblock_live || !base) {
        ++s_bad_calls;
        return -212;
    }
    switch (s_base_mode) {
    case FAKE_BASE_NULL:
        *base = NULL;
        break;
    case FAKE_BASE_MISALIGNED:
        *base = &s_backing[1];
        break;
    case FAKE_BASE_OVERFLOW:
        *base = (void *)(UINTPTR_MAX & ~(uintptr_t)0xfffU);
        break;
    case FAKE_BASE_NORMAL:
    default:
        *base = s_backing;
        break;
    }
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    ++s_memblock_free_calls;
    if (s_fail_memblock_free) {
        --s_fail_memblock_free;
        return -221;
    }
    if (uid != FAKE_MEMBLOCK_UID || !s_memblock_live) {
        ++s_bad_calls;
        return -222;
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
    if (memblock != s_backing || size != ISAAC_VITA_OPENAL_POOL_BYTES ||
        !s_memblock_live || s_mspace_live) {
        ++s_bad_calls;
        return NULL;
    }
    memset(s_records, 0, sizeof s_records);
    s_bump = 4096U;
    s_mspace.marker = 0x4f41U;
    s_mspace_live = 1;
    return &s_mspace;
}

void sceClibMspaceDestroy(SceClibMspace mspace)
{
    ++s_mspace_destroy_calls;
    if (mspace != &s_mspace || !s_mspace_live || record_live_count() != 0U) {
        ++s_bad_calls;
        return;
    }
    s_mspace_live = 0;
}

void *sceClibMspaceMalloc(SceClibMspace mspace, SceSize size)
{
    ++s_mspace_malloc_calls;
    if (mspace != &s_mspace || !s_mspace_live) {
        ++s_bad_calls;
        return NULL;
    }
    if (s_fail_mspace_malloc) {
        --s_fail_mspace_malloc;
        return NULL;
    }
    return fake_allocate(size);
}

void sceClibMspaceFree(SceClibMspace mspace, void *pointer)
{
    fake_record *record = record_find(pointer);

    ++s_mspace_free_calls;
    if (mspace != &s_mspace || !s_mspace_live || !record) {
        ++s_bad_calls;
        return;
    }
    record->live = 0;
}

static void failure_flags_clear(void)
{
    s_fail_memblock_alloc = 0;
    s_fail_memblock_get = 0;
    s_fail_memblock_free = 0;
    s_fail_mutex_create = 0;
    s_fail_mutex_delete = 0;
    s_fail_mutex_lock = 0;
    s_fail_mutex_unlock = 0;
    s_fail_mspace_create = 0;
    s_fail_mspace_malloc = 0;
    s_return_span_failure = 0;
    s_return_out_of_range = 0;
    s_base_mode = FAKE_BASE_NORMAL;
}

static int oracle_reset(void)
{
    failure_flags_clear();
    if (!isaac_vita_openal_pool_test_reset())
        return 0;
    if (s_memblock_live || s_mutex_live || s_mspace_live ||
        record_live_count() != 0U)
        return 0;
    memset(s_records, 0, sizeof s_records);
    s_bump = 0U;
    s_memblock_alloc_calls = 0U;
    s_memblock_get_calls = 0U;
    s_memblock_free_calls = 0U;
    s_mutex_create_calls = 0U;
    s_mutex_delete_calls = 0U;
    atomic_store_explicit(&s_mutex_lock_calls, 0U, memory_order_relaxed);
    s_mutex_unlock_calls = 0U;
    s_mspace_create_calls = 0U;
    s_mspace_destroy_calls = 0U;
    s_mspace_malloc_calls = 0U;
    s_mspace_free_calls = 0U;
    s_bad_calls = 0U;
    atomic_store_explicit(&s_native_malloc_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_native_calloc_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_native_realloc_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_native_free_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_native_aligned_calls, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_fail_native_realloc, 0, memory_order_relaxed);
    atomic_store_explicit(&s_pause_mutex_create, 0, memory_order_relaxed);
    atomic_store_explicit(&s_mutex_create_entered, 0, memory_order_relaxed);
    atomic_store_explicit(&s_release_mutex_create, 0, memory_order_relaxed);
    return 1;
}

#ifndef ISAAC_VITA_OPENAL_POOL_ORACLE_OFF
static void safe_fixed_range(uintptr_t *begin, uintptr_t *end)
{
    *begin = 0x1000U;
    *end = 0x2000U;
}
#endif

static int snapshot_get(isaac_vita_openal_pool_snapshot *snapshot)
{
    return isaac_vita_openal_pool_snapshot_get(snapshot);
}

#ifndef ISAAC_VITA_OPENAL_POOL_ORACLE_OFF
static int test_preinit_forces_session_fallback(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;
    void *pointer;
    void *replacement;
    void *aligned;
    char *copy;

    CHECK(oracle_reset());
    pointer = __real_malloc(32U);
    CHECK(pointer);
    memset(pointer, 0x39, 32U);
    atomic_store_explicit(&s_fail_native_realloc, 1,
                          memory_order_relaxed);
    replacement = isaac_vita_openal_pool_realloc(pointer, 64U);
    CHECK(!replacement && native_realloc_calls() == 1U &&
          ((unsigned char *)pointer)[31] == 0x39U);
    safe_fixed_range(&begin, &end);
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_FALLBACK &&
          snapshot.init_failure ==
              ISAAC_VITA_OPENAL_POOL_INIT_PREINIT_ALLOCATION &&
          snapshot.preinit_native_allocations == 1U);
    isaac_vita_openal_pool_free(pointer);
    CHECK(native_free_calls() == 1U);

    CHECK(oracle_reset());
    pointer = __real_malloc(32U);
    CHECK(pointer);
    memset(pointer, 0x4a, 32U);
    pointer = isaac_vita_openal_pool_realloc(pointer, 64U);
    CHECK(pointer && native_realloc_calls() == 1U &&
          ((unsigned char *)pointer)[0] == 0x4aU);
    safe_fixed_range(&begin, &end);
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_FALLBACK &&
          snapshot.init_failure ==
              ISAAC_VITA_OPENAL_POOL_INIT_PREINIT_ALLOCATION &&
          snapshot.preinit_native_allocations == 1U &&
          s_mutex_create_calls == 0U && s_memblock_alloc_calls == 0U);
    isaac_vita_openal_pool_free(pointer);
    CHECK(native_free_calls() == 1U);

    CHECK(oracle_reset());
    pointer = isaac_vita_openal_pool_malloc(32U);
    CHECK(pointer && native_malloc_calls() == 1U);
    isaac_vita_openal_pool_free(pointer);
    CHECK(native_free_calls() == 1U);
    safe_fixed_range(&begin, &end);
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_FALLBACK &&
          snapshot.init_failure ==
              ISAAC_VITA_OPENAL_POOL_INIT_PREINIT_ALLOCATION &&
          snapshot.preinit_native_allocations == 1U &&
          s_mutex_create_calls == 0U && s_memblock_alloc_calls == 0U);

    pointer = isaac_vita_openal_pool_calloc(2U, 16U);
    aligned = isaac_vita_openal_pool_aligned_alloc(16U, 32U);
    copy = isaac_vita_openal_pool_strdup("fallback");
    CHECK(pointer && aligned && copy && strcmp(copy, "fallback") == 0 &&
          native_calloc_calls() == 1U && native_aligned_calls() == 1U &&
          native_malloc_calls() == 2U);
    pointer = isaac_vita_openal_pool_realloc(pointer, 64U);
    CHECK(pointer && native_realloc_calls() == 1U);
    isaac_vita_openal_pool_free(pointer);
    isaac_vita_openal_pool_free(aligned);
    isaac_vita_openal_pool_free(copy);
    CHECK(native_free_calls() == 4U);
    CHECK(oracle_reset());
    return 0;
}

static int test_init_failures(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;

    CHECK(oracle_reset());
    CHECK(!isaac_vita_openal_pool_initialize(0U, 0U));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure ==
              ISAAC_VITA_OPENAL_POOL_INIT_INVALID_FIXED_IMAGE &&
          s_mutex_create_calls == 0U);

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    s_fail_mutex_create = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_MUTEX &&
          !s_mutex_live && !s_memblock_live);

    CHECK(oracle_reset());
    s_fail_memblock_alloc = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_MEMBLOCK &&
          !s_mutex_live && !s_memblock_live &&
          s_mutex_delete_calls == 1U);

    CHECK(oracle_reset());
    s_fail_memblock_alloc = 1;
    s_fail_mutex_delete = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_ROLLBACK &&
          s_mutex_live && !s_memblock_live);
    CHECK(oracle_reset());

    s_fail_memblock_get = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_GET_BASE &&
          !s_mutex_live && !s_memblock_live);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_NULL;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_GET_BASE &&
          !s_mutex_live && !s_memblock_live);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_MISALIGNED;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_ADDRESS &&
          !s_mutex_live && !s_memblock_live);

    CHECK(oracle_reset());
    s_base_mode = FAKE_BASE_OVERFLOW;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_ADDRESS);

    CHECK(oracle_reset());
    begin = (uintptr_t)s_backing + ISAAC_VITA_OPENAL_POOL_BYTES;
    end = (uintptr_t)s_backing + ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure ==
              ISAAC_VITA_OPENAL_POOL_INIT_FIXED_IMAGE_OVERLAP &&
          s_mspace_create_calls == 0U && !s_memblock_live);

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    s_fail_mspace_create = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_MSPACE &&
          !s_mutex_live && !s_memblock_live);

    CHECK(oracle_reset());
    s_fail_memblock_get = 1;
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_ROLLBACK &&
          s_memblock_live && !s_mutex_live);
    CHECK(oracle_reset());

    s_fail_memblock_get = 1;
    s_fail_mutex_delete = 1;
    CHECK(!isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK &&
          !s_memblock_live && s_mutex_live);
    CHECK(oracle_reset());
    return 0;
}

static int test_ready_lifecycle(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;
    unsigned char *normal;
    unsigned char *zeroed;
    unsigned char *aligned;
    void *native;
    char *copy;
    size_t index;
    unsigned native_malloc_before;
    unsigned mspace_malloc_before;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_READY &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_OK &&
          snapshot.uid == FAKE_MEMBLOCK_UID &&
          snapshot.mutex_uid == FAKE_MUTEX_UID &&
          snapshot.base == (uintptr_t)s_backing &&
          snapshot.end == (uintptr_t)s_backing +
              ISAAC_VITA_OPENAL_POOL_BYTES &&
          snapshot.retained_end == (uintptr_t)s_backing +
              ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES &&
          snapshot.backing_bytes == ISAAC_VITA_OPENAL_POOL_BYTES &&
          snapshot.live_count == 0U && snapshot.stranded_count == 0U &&
          snapshot.has_mspace && snapshot.preinit_native_allocations == 0U &&
          s_mutex_create_calls == 1U && s_memblock_alloc_calls == 1U &&
          s_mspace_create_calls == 1U);

    normal = isaac_vita_openal_pool_malloc(32U);
    zeroed = isaac_vita_openal_pool_calloc(4U, 8U);
    aligned = isaac_vita_openal_pool_aligned_alloc(64U, 128U);
    copy = isaac_vita_openal_pool_strdup("hello");
    CHECK(normal && zeroed && aligned && copy &&
          ((uintptr_t)normal & 7U) == 0U &&
          ((uintptr_t)zeroed & 7U) == 0U &&
          ((uintptr_t)aligned & 63U) == 0U &&
          strcmp(copy, "hello") == 0);
    for (index = 0U; index < 32U; ++index)
        CHECK(zeroed[index] == 0U);
    for (index = 0U; index < 32U; ++index)
        normal[index] = (unsigned char)(index + 1U);
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 4U &&
          snapshot.live_requested_bytes == 198U &&
          snapshot.allocations == 4U && snapshot.frees == 0U &&
          snapshot.peak_live_count == 4U &&
          snapshot.peak_requested_bytes == 198U &&
          native_malloc_calls() == 0U && native_calloc_calls() == 0U &&
          native_aligned_calls() == 0U);

    normal = isaac_vita_openal_pool_realloc(normal, 96U);
    CHECK(normal);
    for (index = 0U; index < 32U; ++index)
        CHECK(normal[index] == (unsigned char)(index + 1U));
    aligned = isaac_vita_openal_pool_realloc(aligned, 256U);
    CHECK(aligned && ((uintptr_t)aligned & 63U) == 0U);
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 4U &&
          snapshot.live_requested_bytes == 390U &&
          snapshot.allocations == 6U && snapshot.frees == 2U &&
          snapshot.reallocations == 2U);

    native = __real_malloc(24U);
    CHECK(native);
    native = isaac_vita_openal_pool_realloc(native, 48U);
    CHECK(native && native_realloc_calls() == 1U);
    isaac_vita_openal_pool_free(native);
    CHECK(native_free_calls() == 1U);
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 4U &&
          snapshot.allocations == 6U && snapshot.frees == 2U);

    native_malloc_before = native_malloc_calls();
    mspace_malloc_before = s_mspace_malloc_calls;
    s_fail_mspace_malloc = 1;
    CHECK(!isaac_vita_openal_pool_malloc(64U));
    CHECK(snapshot_get(&snapshot) && snapshot.failures == 1U &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_READY &&
          native_malloc_calls() == native_malloc_before);
    CHECK(!isaac_vita_openal_pool_calloc(SIZE_MAX, 2U));
    CHECK(!isaac_vita_openal_pool_aligned_alloc(3U, 96U));
    CHECK(!isaac_vita_openal_pool_aligned_alloc(16U, 95U));
    CHECK(s_mspace_malloc_calls == mspace_malloc_before + 1U &&
          native_malloc_calls() == native_malloc_before);

    isaac_vita_openal_pool_free(normal);
    isaac_vita_openal_pool_free(zeroed);
    isaac_vita_openal_pool_free(aligned);
    isaac_vita_openal_pool_free(copy);
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 0U &&
          snapshot.live_requested_bytes == 0U &&
          snapshot.allocations == snapshot.frees &&
          snapshot.reallocations == 2U);
    CHECK(oracle_reset());
    CHECK(s_bad_calls == 0U);
    return 0;
}

static int test_vita_four_byte_alignment_contract(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;
    unsigned char *pointer;
    size_t index;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    pointer = isaac_vita_openal_pool_aligned_alloc(4U, 20U);
    CHECK(pointer && ((uintptr_t)pointer & 7U) == 0U);
    for (index = 0U; index < 20U; ++index)
        pointer[index] = (unsigned char)(0xa0U + index);
    pointer = isaac_vita_openal_pool_realloc(pointer, 36U);
    CHECK(pointer && ((uintptr_t)pointer & 7U) == 0U);
    for (index = 0U; index < 20U; ++index)
        CHECK(pointer[index] == (unsigned char)(0xa0U + index));
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 1U &&
          snapshot.live_requested_bytes == 36U &&
          snapshot.allocations == 2U && snapshot.frees == 1U &&
          snapshot.reallocations == 1U);
    isaac_vita_openal_pool_free(pointer);
    CHECK(oracle_reset());
    return 0;
}

static int test_realloc_edges_and_live_reset_refusal(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;
    unsigned char *pointer;
    size_t index;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    pointer = isaac_vita_openal_pool_realloc(NULL, 24U);
    CHECK(pointer && !isaac_vita_openal_pool_test_reset());
    for (index = 0U; index < 24U; ++index)
        pointer[index] = (unsigned char)(0x30U + index);
    s_fail_mspace_malloc = 1;
    CHECK(!isaac_vita_openal_pool_realloc(pointer, 48U));
    for (index = 0U; index < 24U; ++index)
        CHECK(pointer[index] == (unsigned char)(0x30U + index));
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 1U &&
          snapshot.live_requested_bytes == 24U &&
          snapshot.allocations == 1U && snapshot.frees == 0U &&
          snapshot.reallocations == 0U && snapshot.failures == 1U &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_READY);
    CHECK(!isaac_vita_openal_pool_realloc(pointer, 0U));
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 0U &&
          snapshot.allocations == 1U && snapshot.frees == 1U &&
          snapshot.live_requested_bytes == 0U);
    CHECK(oracle_reset());
    return 0;
}

typedef struct thread_context {
    unsigned seed;
    int failed;
} thread_context;

static void *allocation_thread(void *opaque)
{
    thread_context *context = (thread_context *)opaque;
    unsigned iteration;

    for (iteration = 0U; iteration < 400U; ++iteration) {
        size_t size = 16U + ((context->seed + iteration) & 63U);
        unsigned char *pointer = isaac_vita_openal_pool_malloc(size);
        isaac_vita_openal_pool_snapshot snapshot;

        if (!pointer) {
            context->failed = 1;
            return NULL;
        }
        memset(pointer, (int)(context->seed & 0xffU), size);
        pointer = isaac_vita_openal_pool_realloc(pointer, size + 17U);
        if (!pointer || pointer[0] != (unsigned char)(context->seed & 0xffU) ||
            !snapshot_get(&snapshot) ||
            snapshot.state != ISAAC_VITA_OPENAL_POOL_READY) {
            context->failed = 1;
            return NULL;
        }
        isaac_vita_openal_pool_free(pointer);
    }
    return NULL;
}

static int test_concurrency(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    pthread_t threads[4];
    thread_context contexts[4];
    uintptr_t begin;
    uintptr_t end;
    size_t index;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    for (index = 0U; index < 4U; ++index) {
        contexts[index].seed = (unsigned)(index + 1U);
        contexts[index].failed = 0;
        CHECK(pthread_create(&threads[index], NULL, allocation_thread,
                             &contexts[index]) == 0);
    }
    for (index = 0U; index < 4U; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0 &&
              !contexts[index].failed);
    }
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 0U &&
          snapshot.allocations == 3200U && snapshot.frees == 3200U &&
          snapshot.reallocations == 1600U &&
          snapshot.peak_live_count >= 1U && snapshot.corruption == 0U &&
          s_bad_calls == 0U);
    CHECK(oracle_reset());
    return 0;
}

typedef struct initializing_context {
    uintptr_t begin;
    uintptr_t end;
    int initialized;
} initializing_context;

static void *initializing_thread(void *opaque)
{
    initializing_context *context = (initializing_context *)opaque;

    context->initialized = isaac_vita_openal_pool_initialize(
        context->begin, context->end);
    return NULL;
}

static int test_initializing_realloc_preserves_native(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    initializing_context context;
    pthread_t thread;
    unsigned char *pointer;
    void *replacement;
    unsigned realloc_before;

    CHECK(oracle_reset());
    safe_fixed_range(&context.begin, &context.end);
    context.initialized = 0;
    pointer = __real_malloc(32U);
    CHECK(pointer);
    memset(pointer, 0x5c, 32U);
    realloc_before = native_realloc_calls();
    atomic_store_explicit(&s_pause_mutex_create, 1, memory_order_release);
    CHECK(pthread_create(&thread, NULL, initializing_thread, &context) == 0);
    while (!atomic_load_explicit(&s_mutex_create_entered,
                                 memory_order_acquire)) {
    }
    replacement = isaac_vita_openal_pool_realloc(pointer, 64U);
    CHECK(!replacement && native_realloc_calls() == realloc_before &&
          pointer[0] == 0x5cU && pointer[31] == 0x5cU);
    atomic_store_explicit(&s_release_mutex_create, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0 && context.initialized &&
          snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_READY &&
          snapshot.preinit_native_allocations == 0U);
    isaac_vita_openal_pool_free(pointer);
    CHECK(oracle_reset());
    return 0;
}

#ifdef ISAAC_VITA_OPENAL_POOL_ORACLE_TSAN
typedef struct snapshot_race_context {
    atomic_int started;
    atomic_int stop;
    int failed;
    unsigned snapshots;
} snapshot_race_context;

static void *snapshot_race_thread(void *opaque)
{
    snapshot_race_context *context = (snapshot_race_context *)opaque;

    atomic_store_explicit(&context->started, 1, memory_order_release);
    while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
        isaac_vita_openal_pool_snapshot snapshot;

        if (snapshot_get(&snapshot)) {
            if (snapshot.state == ISAAC_VITA_OPENAL_POOL_READY) {
                if (snapshot.base != (uintptr_t)s_backing ||
                    snapshot.end != (uintptr_t)s_backing +
                        ISAAC_VITA_OPENAL_POOL_BYTES)
                    context->failed = 1;
            } else if (snapshot.state !=
                       ISAAC_VITA_OPENAL_POOL_UNINITIALIZED) {
                context->failed = 1;
            }
            ++context->snapshots;
        }
    }
    return NULL;
}

static int test_snapshot_vs_init_race(void)
{
    snapshot_race_context context;
    pthread_t thread;
    uintptr_t begin;
    uintptr_t end;

    CHECK(oracle_reset());
    memset(&context, 0, sizeof context);
    atomic_init(&context.started, 0);
    atomic_init(&context.stop, 0);
    CHECK(pthread_create(&thread, NULL, snapshot_race_thread, &context) == 0);
    while (!atomic_load_explicit(&context.started, memory_order_acquire)) {
    }
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    atomic_store_explicit(&context.stop, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0 && !context.failed &&
          context.snapshots != 0U);
    CHECK(oracle_reset());
    return 0;
}

typedef struct preinit_race_context {
    atomic_int start;
    void *pointer;
} preinit_race_context;

typedef struct preinit_realloc_race_context {
    atomic_int start;
    atomic_int entered;
    void *pointer;
} preinit_realloc_race_context;

static void *preinit_race_thread(void *opaque)
{
    preinit_race_context *context = (preinit_race_context *)opaque;

    while (!atomic_load_explicit(&context->start, memory_order_acquire)) {
    }
    context->pointer = isaac_vita_openal_pool_malloc(37U);
    return NULL;
}

static void *preinit_realloc_race_thread(void *opaque)
{
    preinit_realloc_race_context *context =
        (preinit_realloc_race_context *)opaque;
    void *replacement;

    while (!atomic_load_explicit(&context->start, memory_order_acquire)) {
    }
    atomic_store_explicit(&context->entered, 1, memory_order_release);
    replacement = isaac_vita_openal_pool_realloc(context->pointer, 73U);
    if (replacement)
        context->pointer = replacement;
    return NULL;
}

static int test_preinit_vs_init_race(void)
{
    unsigned iteration;

    for (iteration = 0U; iteration < 64U; ++iteration) {
        isaac_vita_openal_pool_snapshot snapshot;
        preinit_race_context context;
        pthread_t thread;
        uintptr_t begin;
        uintptr_t end;
        int initialized;

        CHECK(oracle_reset());
        memset(&context, 0, sizeof context);
        atomic_init(&context.start, 0);
        CHECK(pthread_create(&thread, NULL, preinit_race_thread,
                             &context) == 0);
        atomic_store_explicit(&context.start, 1, memory_order_release);
        safe_fixed_range(&begin, &end);
        initialized = isaac_vita_openal_pool_initialize(begin, end);
        CHECK(pthread_join(thread, NULL) == 0 && snapshot_get(&snapshot));
        CHECK((initialized && snapshot.state == ISAAC_VITA_OPENAL_POOL_READY) ||
              (!initialized &&
               snapshot.state == ISAAC_VITA_OPENAL_POOL_FALLBACK &&
               snapshot.preinit_native_allocations == 1U &&
               context.pointer));
        isaac_vita_openal_pool_free(context.pointer);
        CHECK(oracle_reset());
    }
    return 0;
}

static int test_preinit_realloc_vs_init_race(void)
{
    unsigned iteration;

    for (iteration = 0U; iteration < 64U; ++iteration) {
        isaac_vita_openal_pool_snapshot snapshot;
        preinit_realloc_race_context context;
        pthread_t thread;
        uintptr_t begin;
        uintptr_t end;
        int initialized;

        CHECK(oracle_reset());
        memset(&context, 0, sizeof context);
        atomic_init(&context.start, 0);
        atomic_init(&context.entered, 0);
        context.pointer = __real_malloc(37U);
        CHECK(context.pointer);
        CHECK(pthread_create(&thread, NULL, preinit_realloc_race_thread,
                             &context) == 0);
        atomic_store_explicit(&context.start, 1, memory_order_release);
        while (!atomic_load_explicit(&context.entered, memory_order_acquire)) {
        }
        safe_fixed_range(&begin, &end);
        initialized = isaac_vita_openal_pool_initialize(begin, end);
        CHECK(pthread_join(thread, NULL) == 0 && context.pointer &&
              snapshot_get(&snapshot));
        CHECK((initialized &&
               snapshot.state == ISAAC_VITA_OPENAL_POOL_READY &&
               snapshot.preinit_native_allocations == 0U) ||
              (!initialized &&
               snapshot.state == ISAAC_VITA_OPENAL_POOL_FALLBACK &&
               snapshot.init_failure ==
                   ISAAC_VITA_OPENAL_POOL_INIT_PREINIT_ALLOCATION &&
               snapshot.preinit_native_allocations == 1U));
        isaac_vita_openal_pool_free(context.pointer);
        CHECK(oracle_reset());
    }
    return 0;
}
#endif

static int test_corruption_and_unlock(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;
    unsigned char *pointer;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    pointer = isaac_vita_openal_pool_malloc(32U);
    CHECK(pointer);
    isaac_vita_openal_pool_free(pointer + 8U);
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY &&
          snapshot.corruption == 1U && snapshot.live_count == 1U);
    CHECK(!isaac_vita_openal_pool_malloc(8U));
    isaac_vita_openal_pool_free(pointer);
    CHECK(snapshot_get(&snapshot) && snapshot.live_count == 0U);
    CHECK(oracle_reset());

    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    pointer = isaac_vita_openal_pool_malloc(32U);
    CHECK(pointer);
    isaac_vita_openal_pool_free(pointer);
    isaac_vita_openal_pool_free(pointer);
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY &&
          snapshot.corruption == 1U && snapshot.live_count == 0U);
    CHECK(oracle_reset());

    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    pointer = isaac_vita_openal_pool_malloc(32U);
    CHECK(pointer);
    s_fail_mutex_unlock = 1;
    CHECK(!snapshot_get(&snapshot));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY &&
          snapshot.corruption == 1U);
    isaac_vita_openal_pool_free(pointer);
    CHECK(oracle_reset());

    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    s_return_span_failure = 1;
    CHECK(!isaac_vita_openal_pool_malloc(32U));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY &&
          snapshot.stranded_count == 0U && record_live_count() == 0U);
    CHECK(oracle_reset());

    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    s_fail_mutex_lock = 1;
    CHECK(!isaac_vita_openal_pool_malloc(32U));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY &&
          snapshot.corruption == 1U && snapshot.live_count == 0U);
    CHECK(oracle_reset());
    return 0;
}

static int test_partial_reset_retry(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    s_fail_mutex_delete = 1;
    CHECK(!isaac_vita_openal_pool_test_reset());
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK &&
          snapshot.uid == ISAAC_VITA_OPENAL_POOL_INVALID_UID &&
          snapshot.mutex_uid == FAKE_MUTEX_UID && !s_memblock_live &&
          s_mutex_live);
    CHECK(isaac_vita_openal_pool_test_reset());
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_UNINITIALIZED);

    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    s_fail_memblock_free = 1;
    CHECK(!isaac_vita_openal_pool_test_reset());
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK &&
          snapshot.uid == FAKE_MEMBLOCK_UID &&
          snapshot.mutex_uid == ISAAC_VITA_OPENAL_POOL_INVALID_UID &&
          s_memblock_live && !s_mutex_live);
    CHECK(isaac_vita_openal_pool_test_reset());
    CHECK(oracle_reset());
    return 0;
}

static int test_out_of_range_strand_last(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    uintptr_t begin;
    uintptr_t end;

    CHECK(oracle_reset());
    safe_fixed_range(&begin, &end);
    CHECK(isaac_vita_openal_pool_initialize(begin, end));
    s_return_out_of_range = 1;
    CHECK(!isaac_vita_openal_pool_malloc(32U));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY &&
          snapshot.stranded_count == 1U && snapshot.live_count == 0U &&
          record_live_count() == 1U && snapshot.corruption == 1U);
    CHECK(!isaac_vita_openal_pool_test_reset());
    return 0;
}
#endif

#ifdef ISAAC_VITA_OPENAL_POOL_ORACLE_TSAN
int main(void)
{
    CHECK(test_snapshot_vs_init_race() == 0);
    CHECK(test_preinit_vs_init_race() == 0);
    CHECK(test_preinit_realloc_vs_init_race() == 0);
    CHECK(test_initializing_realloc_preserves_native() == 0);
    CHECK(test_concurrency() == 0);
    puts("Vita OpenAL pool TSAN concurrency oracle: PASS");
    return 0;
}
#elif defined(ISAAC_VITA_OPENAL_POOL_ORACLE_OFF)
int main(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    void *pointer;
    void *zeroed;
    void *aligned;
    char *copy;

    CHECK(oracle_reset());
    CHECK(!isaac_vita_openal_pool_initialize(0x1000U, 0x2000U));
    CHECK(snapshot_get(&snapshot) &&
          snapshot.state == ISAAC_VITA_OPENAL_POOL_FALLBACK &&
          snapshot.init_failure == ISAAC_VITA_OPENAL_POOL_INIT_DISABLED &&
          s_mutex_create_calls == 0U && s_memblock_alloc_calls == 0U);
    pointer = isaac_vita_openal_pool_malloc(32U);
    CHECK(pointer && native_malloc_calls() == 1U);
    pointer = isaac_vita_openal_pool_realloc(pointer, 64U);
    CHECK(pointer && native_realloc_calls() == 1U);
    isaac_vita_openal_pool_free(pointer);
    zeroed = isaac_vita_openal_pool_calloc(4U, 16U);
    CHECK(zeroed && ((unsigned char *)zeroed)[63] == 0U &&
          native_calloc_calls() == 1U);
    isaac_vita_openal_pool_free(zeroed);
    aligned = isaac_vita_openal_pool_aligned_alloc(64U, 128U);
    CHECK(aligned && ((uintptr_t)aligned & 63U) == 0U &&
          native_aligned_calls() == 1U);
    isaac_vita_openal_pool_free(aligned);
    copy = isaac_vita_openal_pool_strdup("disabled-fallback");
    CHECK(copy && strcmp(copy, "disabled-fallback") == 0 &&
          native_malloc_calls() == 2U);
    isaac_vita_openal_pool_free(copy);
    CHECK(native_free_calls() == 4U && oracle_reset());
    puts("Vita OpenAL pool disabled-session oracle: PASS");
    return 0;
}
#else
int main(void)
{
    CHECK(ISAAC_VITA_OPENAL_POOL_BYTES == 0x00bd6000U);
    CHECK(ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES == 0x00bd8000U);
    CHECK(test_preinit_forces_session_fallback() == 0);
    CHECK(test_init_failures() == 0);
    CHECK(test_ready_lifecycle() == 0);
    CHECK(test_vita_four_byte_alignment_contract() == 0);
    CHECK(test_realloc_edges_and_live_reset_refusal() == 0);
    CHECK(test_initializing_realloc_preserves_native() == 0);
    CHECK(test_concurrency() == 0);
    CHECK(test_corruption_and_unlock() == 0);
    CHECK(test_partial_reset_retry() == 0);
    CHECK(test_out_of_range_strand_last() == 0);
    puts("Vita OpenAL dedicated mspace oracle: PASS");
    return 0;
}
#endif
