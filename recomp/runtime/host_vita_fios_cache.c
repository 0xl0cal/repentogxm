/* Bounded Vita FIOS2 RAM-cache lifecycle.
 *
 * The ABI declarations and storage formula come from Andy Nguyen's MIT FIOS
 * helper as retained by fgsfdsfgs/max_vita e92e9ad9, TheOfficialFloW/gtasa_vita
 * 96941714 and Rinnegatamante/baba-is-you-vita f2b7b519.  This owner fixes the
 * helper's partial-init leak: every failure after a UID exists either rolls it
 * back or retains an explicit orphan receipt for one terminal retry. */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_fios_cache.h"
#include "platform.h"

#define ISAAC_FIOS_FH_SIZE       80U
#define ISAAC_FIOS_DH_SIZE       80U
#define ISAAC_FIOS_OP_SIZE      168U
#define ISAAC_FIOS_CHUNK_SIZE    64U

#define ISAAC_FIOS_ALIGN_UP(value, alignment) \
    (((value) + ((alignment) - 1U)) & \
     ~((uintptr_t)((alignment) - 1U)))
#define ISAAC_FIOS_STORAGE_SIZE(count, element_size) \
    ((count) * (element_size) + \
     ISAAC_FIOS_ALIGN_UP(ISAAC_FIOS_ALIGN_UP((count), 8U) / 8U, 8U))
/* Scene helpers expose each buffer as an int64_t array with one spare member.
 * Preserve that proven capacity rather than relying on exact-size acceptance. */
#define ISAAC_FIOS_STORAGE_CAPACITY(bytes) \
    (ISAAC_FIOS_ALIGN_UP((bytes), 8U) + 8U)

#define ISAAC_FIOS_OP_STORAGE_BYTES \
    ISAAC_FIOS_STORAGE_CAPACITY(ISAAC_FIOS_STORAGE_SIZE( \
        ISAAC_VITA_FIOS_OP_COUNT, \
        ISAAC_FIOS_OP_SIZE + ISAAC_VITA_FIOS_PATH_MAX))
#define ISAAC_FIOS_CHUNK_STORAGE_BYTES \
    ISAAC_FIOS_STORAGE_CAPACITY(ISAAC_FIOS_STORAGE_SIZE( \
        ISAAC_VITA_FIOS_CHUNK_COUNT, ISAAC_FIOS_CHUNK_SIZE))
#define ISAAC_FIOS_FH_STORAGE_BYTES \
    ISAAC_FIOS_STORAGE_CAPACITY(ISAAC_FIOS_STORAGE_SIZE( \
        ISAAC_VITA_FIOS_FH_COUNT, \
        ISAAC_FIOS_FH_SIZE + ISAAC_VITA_FIOS_PATH_MAX))
#define ISAAC_FIOS_DH_STORAGE_BYTES \
    ISAAC_FIOS_STORAGE_CAPACITY(ISAAC_FIOS_STORAGE_SIZE( \
        ISAAC_VITA_FIOS_DH_COUNT, \
        ISAAC_FIOS_DH_SIZE + ISAAC_VITA_FIOS_PATH_MAX))
#define ISAAC_FIOS_LAYOUT_BYTES \
    (ISAAC_FIOS_OP_STORAGE_BYTES + ISAAC_FIOS_CHUNK_STORAGE_BYTES + \
     ISAAC_FIOS_FH_STORAGE_BYTES + ISAAC_FIOS_DH_STORAGE_BYTES + \
     ISAAC_VITA_FIOS_CACHE_BYTES)

#define ISAAC_FIOS_NOT_CALLED INT32_MIN
#define ISAAC_FIOS_INVALID_UID (-1)

typedef enum IsaacFiosThreadType {
    ISAAC_FIOS_IO_THREAD = 0,
    ISAAC_FIOS_DECOMPRESSOR_THREAD = 1,
    ISAAC_FIOS_CALLBACK_THREAD = 2,
    ISAAC_FIOS_THREAD_TYPES = 3
} IsaacFiosThreadType;

typedef struct IsaacFiosBuffer {
    void *pPtr;
    size_t length;
} IsaacFiosBuffer;

typedef struct IsaacFiosParams {
    uint32_t initialized : 1;
    uint32_t paramsSize : 15;
    uint32_t pathMax : 16;
    uint32_t profiling;
    uint32_t ioThreadCount;
    uint32_t threadsPerScheduler;
    uint32_t extraFlag1 : 1;
    uint32_t extraFlags : 31;
    uint32_t maxChunk;
    uint8_t maxDecompressorThreadCount;
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
    intptr_t reserved4;
    intptr_t reserved5;
    IsaacFiosBuffer opStorage;
    IsaacFiosBuffer fhStorage;
    IsaacFiosBuffer dhStorage;
    IsaacFiosBuffer chunkStorage;
    void *pVprintf;
    void *pMemcpy;
    void *pProfileCallback;
    int threadPriority[ISAAC_FIOS_THREAD_TYPES];
    int threadAffinity[ISAAC_FIOS_THREAD_TYPES];
    int threadStackSize[ISAAC_FIOS_THREAD_TYPES];
} IsaacFiosParams;

typedef struct IsaacFiosRamCacheContext {
    size_t sizeOfContext;
    size_t workBufferSize;
    size_t blockSize;
    void *pWorkBuffer;
    const char *pPath;
    intptr_t flags;
    intptr_t reserved[3];
} IsaacFiosRamCacheContext;

/* SceFios2 is shipped by the Vita system but VitaSDK intentionally exposes
 * only its import stubs, not this application-facing header. */
int sceFiosInitialize(const IsaacFiosParams *params);
void sceFiosTerminate(void);
int sceFiosIOFilterAdd(int index, void *callback, void *context);
void sceFiosIOFilterCache(void);

typedef struct isaac_fios_layout {
    void *op;
    void *chunk;
    void *fh;
    void *dh;
    void *cache;
} isaac_fios_layout;

typedef struct isaac_fios_storage {
    isaac_vita_fios_cache_snapshot snapshot;
    void *base;
    int fios_initialized;
    IsaacFiosRamCacheContext cache_context;
} isaac_fios_storage;

static isaac_fios_storage s_fios = {
    .snapshot = {
        .state = ISAAC_VITA_FIOS_CACHE_COLD,
        .failure = ISAAC_VITA_FIOS_CACHE_FAILURE_NONE,
        .uid = ISAAC_FIOS_INVALID_UID,
        .alloc_result = ISAAC_FIOS_NOT_CALLED,
        .get_base_result = ISAAC_FIOS_NOT_CALLED,
        .initialize_result = ISAAC_FIOS_NOT_CALLED,
        .filter_add_result = ISAAC_FIOS_NOT_CALLED,
        .free_result = ISAAC_FIOS_NOT_CALLED,
        .memblock_bytes = ISAAC_VITA_FIOS_MEMBLOCK_BYTES,
        .cache_bytes = ISAAC_VITA_FIOS_CACHE_BYTES
    }
};

_Static_assert(ISAAC_VITA_FIOS_CACHE_BYTES ==
                   ISAAC_VITA_FIOS_CACHE_BLOCK_BYTES *
                       ISAAC_VITA_FIOS_CACHE_BLOCK_COUNT,
               "FIOS cache block layout drifted");
_Static_assert((ISAAC_VITA_FIOS_CACHE_BLOCK_BYTES &
                (ISAAC_VITA_FIOS_CACHE_BLOCK_BYTES - 1U)) == 0U,
               "FIOS cache block size must be a power of two");
_Static_assert((ISAAC_VITA_FIOS_MEMBLOCK_BYTES & 0xfffU) == 0U,
               "FIOS memblock request must be page aligned");
_Static_assert(ISAAC_FIOS_LAYOUT_BYTES == 644024U,
               "FIOS owned-storage byte receipt drifted");
_Static_assert(ISAAC_FIOS_LAYOUT_BYTES + 7U <=
                   ISAAC_VITA_FIOS_MEMBLOCK_BYTES,
               "FIOS owned storage exceeds its memblock");
_Static_assert(ISAAC_VITA_FIOS_MEMBLOCK_BYTES <= 0x100000U,
               "FIOS total USER_RW request exceeds one MiB");
_Static_assert(ISAAC_VITA_FIOS_FH_COUNT >= 4U * 16U,
               "FIOS FH capacity no longer covers four guest token ledgers");
_Static_assert(ISAAC_VITA_FIOS_DH_COUNT >= 16U,
               "FIOS DH capacity no longer covers the guest token ledger");
_Static_assert(sizeof ISAAC_VITA_DATA_ROOT <= ISAAC_VITA_FIOS_PATH_MAX,
               "Isaac FIOS filter root exceeds pathMax");
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(IsaacFiosBuffer) == 8U,
               "FIOS buffer ABI changed");
_Static_assert(sizeof(IsaacFiosParams) == 116U,
               "FIOS parameter ABI changed");
_Static_assert(sizeof(IsaacFiosRamCacheContext) == 36U,
               "FIOS cache-context ABI changed");
#endif

static void isaac_fios_params_initialize(IsaacFiosParams *params)
{
    memset(params, 0, sizeof *params);
    params->paramsSize = (uint32_t)sizeof *params;
    params->ioThreadCount = 2U;
    params->threadsPerScheduler = 1U;
    params->maxChunk = 256U * 1024U;
    params->maxDecompressorThreadCount = 2U;
    params->threadPriority[ISAAC_FIOS_IO_THREAD] = 66;
    params->threadPriority[ISAAC_FIOS_DECOMPRESSOR_THREAD] = 189;
    params->threadPriority[ISAAC_FIOS_CALLBACK_THREAD] = 66;
    params->threadAffinity[ISAAC_FIOS_IO_THREAD] = 0x40000;
    params->threadAffinity[ISAAC_FIOS_DECOMPRESSOR_THREAD] = 0;
    params->threadAffinity[ISAAC_FIOS_CALLBACK_THREAD] = 0x40000;
    params->threadStackSize[ISAAC_FIOS_IO_THREAD] = 8 * 1024;
    params->threadStackSize[ISAAC_FIOS_DECOMPRESSOR_THREAD] = 16 * 1024;
    params->threadStackSize[ISAAC_FIOS_CALLBACK_THREAD] = 8 * 1024;
}

static int isaac_fios_layout_build(void *base, isaac_fios_layout *layout)
{
    uintptr_t begin;
    uintptr_t cursor;
    uintptr_t end;

    if (!base || !layout)
        return 0;
    begin = (uintptr_t)base;
    if (begin > UINTPTR_MAX - ISAAC_VITA_FIOS_MEMBLOCK_BYTES)
        return 0;
    end = begin + ISAAC_VITA_FIOS_MEMBLOCK_BYTES;
    cursor = ISAAC_FIOS_ALIGN_UP(begin, 8U);

#define ISAAC_FIOS_TAKE(member, bytes) do { \
        if (cursor > end || (bytes) > end - cursor) \
            return 0; \
        layout->member = (void *)cursor; \
        cursor += (bytes); \
        cursor = ISAAC_FIOS_ALIGN_UP(cursor, 8U); \
    } while (0)
    ISAAC_FIOS_TAKE(op, ISAAC_FIOS_OP_STORAGE_BYTES);
    ISAAC_FIOS_TAKE(chunk, ISAAC_FIOS_CHUNK_STORAGE_BYTES);
    ISAAC_FIOS_TAKE(fh, ISAAC_FIOS_FH_STORAGE_BYTES);
    ISAAC_FIOS_TAKE(dh, ISAAC_FIOS_DH_STORAGE_BYTES);
    ISAAC_FIOS_TAKE(cache, ISAAC_VITA_FIOS_CACHE_BYTES);
#undef ISAAC_FIOS_TAKE
    return cursor <= end;
}

static int isaac_fios_release_memblock(void)
{
    int result;

    if (s_fios.snapshot.uid < 0)
        return 1;
    result = sceKernelFreeMemBlock((SceUID)s_fios.snapshot.uid);
    s_fios.snapshot.free_result = result;
    ++s_fios.snapshot.free_calls;
    if (result < 0) {
        s_fios.snapshot.failure = ISAAC_VITA_FIOS_CACHE_FAILURE_FREE;
        s_fios.snapshot.state = ISAAC_VITA_FIOS_CACHE_ORPHANED;
        return 0;
    }
    s_fios.snapshot.uid = ISAAC_FIOS_INVALID_UID;
    s_fios.base = NULL;
    return 1;
}

static int isaac_fios_disable(isaac_vita_fios_cache_failure failure)
{
    s_fios.snapshot.failure = (uint32_t)failure;
    if (s_fios.fios_initialized) {
        sceFiosTerminate();
        ++s_fios.snapshot.terminate_calls;
        s_fios.fios_initialized = 0;
        s_fios.snapshot.filter_active = 0U;
    }
    if (isaac_fios_release_memblock())
        s_fios.snapshot.state = ISAAC_VITA_FIOS_CACHE_DISABLED;
    return 0;
}

int isaac_vita_fios_cache_initialize(void)
{
    IsaacFiosParams params;
    isaac_fios_layout layout;
    SceUID uid;
    void *base = NULL;
    int result;

    if (s_fios.snapshot.state == ISAAC_VITA_FIOS_CACHE_READY)
        return 1;
    if (s_fios.snapshot.state != ISAAC_VITA_FIOS_CACHE_COLD)
        return 0;
    s_fios.snapshot.state = ISAAC_VITA_FIOS_CACHE_INITIALIZING;

    uid = sceKernelAllocMemBlock(
        "isaac-fios-cache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        ISAAC_VITA_FIOS_MEMBLOCK_BYTES, NULL);
    s_fios.snapshot.alloc_result = (int32_t)uid;
    if (uid < 0)
        return isaac_fios_disable(ISAAC_VITA_FIOS_CACHE_FAILURE_ALLOC);
    s_fios.snapshot.uid = (int32_t)uid;

    result = sceKernelGetMemBlockBase(uid, &base);
    s_fios.snapshot.get_base_result = result;
    if (result < 0 || !base)
        return isaac_fios_disable(ISAAC_VITA_FIOS_CACHE_FAILURE_GET_BASE);
    s_fios.base = base;

    memset(&layout, 0, sizeof layout);
    if (!isaac_fios_layout_build(base, &layout))
        return isaac_fios_disable(ISAAC_VITA_FIOS_CACHE_FAILURE_LAYOUT);
    memset(base, 0, ISAAC_VITA_FIOS_MEMBLOCK_BYTES);

    isaac_fios_params_initialize(&params);
    params.pathMax = ISAAC_VITA_FIOS_PATH_MAX;
    params.opStorage.pPtr = layout.op;
    params.opStorage.length = ISAAC_FIOS_OP_STORAGE_BYTES;
    params.chunkStorage.pPtr = layout.chunk;
    params.chunkStorage.length = ISAAC_FIOS_CHUNK_STORAGE_BYTES;
    params.fhStorage.pPtr = layout.fh;
    params.fhStorage.length = ISAAC_FIOS_FH_STORAGE_BYTES;
    params.dhStorage.pPtr = layout.dh;
    params.dhStorage.length = ISAAC_FIOS_DH_STORAGE_BYTES;

    ++s_fios.snapshot.initialize_calls;
    result = sceFiosInitialize(&params);
    s_fios.snapshot.initialize_result = result;
    if (result < 0)
        return isaac_fios_disable(
            ISAAC_VITA_FIOS_CACHE_FAILURE_INITIALIZE);
    s_fios.fios_initialized = 1;

    memset(&s_fios.cache_context, 0, sizeof s_fios.cache_context);
    s_fios.cache_context.sizeOfContext = sizeof s_fios.cache_context;
    s_fios.cache_context.workBufferSize = ISAAC_VITA_FIOS_CACHE_BYTES;
    s_fios.cache_context.blockSize = ISAAC_VITA_FIOS_CACHE_BLOCK_BYTES;
    s_fios.cache_context.pWorkBuffer = layout.cache;
    s_fios.cache_context.pPath = ISAAC_VITA_DATA_ROOT;

    ++s_fios.snapshot.filter_add_calls;
    result = sceFiosIOFilterAdd(
        0, (void *)(uintptr_t)&sceFiosIOFilterCache,
        &s_fios.cache_context);
    s_fios.snapshot.filter_add_result = result;
    if (result < 0)
        return isaac_fios_disable(
            ISAAC_VITA_FIOS_CACHE_FAILURE_FILTER_ADD);

    s_fios.snapshot.filter_active = 1U;
    s_fios.snapshot.failure = ISAAC_VITA_FIOS_CACHE_FAILURE_NONE;
    s_fios.snapshot.state = ISAAC_VITA_FIOS_CACHE_READY;
    return 1;
}

void isaac_vita_fios_cache_shutdown(void)
{
    if (s_fios.snapshot.state == ISAAC_VITA_FIOS_CACHE_TERMINATED)
        return;

    if (s_fios.fios_initialized) {
        sceFiosTerminate();
        ++s_fios.snapshot.terminate_calls;
        s_fios.fios_initialized = 0;
        s_fios.snapshot.filter_active = 0U;
    }
    if (isaac_fios_release_memblock())
        s_fios.snapshot.state = ISAAC_VITA_FIOS_CACHE_TERMINATED;
}

void isaac_vita_fios_cache_snapshot_get(
    isaac_vita_fios_cache_snapshot *snapshot)
{
    if (snapshot)
        *snapshot = s_fios.snapshot;
}
