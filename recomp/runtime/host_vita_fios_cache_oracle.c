/* Host lifecycle/fault oracle for the bounded FIOS2 owner. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>

#include "host_vita_fios_cache.h"

#define CHECK(condition) do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                    __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

enum oracle_event {
    ORACLE_ALLOC = 1,
    ORACLE_GET_BASE,
    ORACLE_INITIALIZE,
    ORACLE_FILTER_ADD,
    ORACLE_TERMINATE,
    ORACLE_FREE
};

typedef struct OracleFiosBuffer {
    void *pPtr;
    size_t length;
} OracleFiosBuffer;

/* Exact duplicate of the ABI passed by host_vita_fios_cache.c. */
typedef struct OracleFiosParams {
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
    OracleFiosBuffer opStorage;
    OracleFiosBuffer fhStorage;
    OracleFiosBuffer dhStorage;
    OracleFiosBuffer chunkStorage;
    void *pVprintf;
    void *pMemcpy;
    void *pProfileCallback;
    int threadPriority[3];
    int threadAffinity[3];
    int threadStackSize[3];
} OracleFiosParams;

typedef struct OracleFiosRamCacheContext {
    size_t sizeOfContext;
    size_t workBufferSize;
    size_t blockSize;
    void *pWorkBuffer;
    const char *pPath;
    intptr_t flags;
    intptr_t reserved[3];
} OracleFiosRamCacheContext;

#define ORACLE_OP_BYTES     27152U
#define ORACLE_CHUNK_BYTES  65672U
#define ORACLE_FH_BYTES     21520U
#define ORACLE_DH_BYTES      5392U

static _Alignas(4096) unsigned char s_memblock[ISAAC_VITA_FIOS_MEMBLOCK_BYTES];
static int s_alloc_result = 41;
static int s_get_base_result;
static int s_get_base_null;
static int s_get_base_invalid;
static int s_initialize_result;
static int s_filter_result;
static int s_free_result;
static unsigned s_alloc_calls;
static unsigned s_get_base_calls;
static unsigned s_initialize_calls;
static unsigned s_filter_calls;
static unsigned s_terminate_calls;
static unsigned s_free_calls;
static unsigned s_zero_at_initialize;
static enum oracle_event s_events[16];
static unsigned s_event_count;
static OracleFiosParams s_params;
static OracleFiosRamCacheContext s_context;
static void *s_filter_callback;
static int s_filter_index;
static char s_alloc_name[32];
static SceKernelMemBlockType s_alloc_type;
static SceSize s_alloc_size;
static SceKernelAllocMemBlockOpt *s_alloc_options;

static void note_event(enum oracle_event event)
{
    if (s_event_count < sizeof s_events / sizeof s_events[0])
        s_events[s_event_count++] = event;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *options)
{
    note_event(ORACLE_ALLOC);
    ++s_alloc_calls;
    snprintf(s_alloc_name, sizeof s_alloc_name, "%s", name ? name : "");
    s_alloc_type = type;
    s_alloc_size = size;
    s_alloc_options = options;
    return (SceUID)s_alloc_result;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    note_event(ORACLE_GET_BASE);
    ++s_get_base_calls;
    if (base)
        *base = s_get_base_null ? NULL :
            (s_get_base_invalid ? (void *)(UINTPTR_MAX - 7U) : s_memblock);
    return uid == (SceUID)s_alloc_result ? s_get_base_result : -999;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    note_event(ORACLE_FREE);
    ++s_free_calls;
    return uid == (SceUID)s_alloc_result ? s_free_result : -998;
}

int sceFiosInitialize(const void *opaque)
{
    const OracleFiosParams *params = (const OracleFiosParams *)opaque;
    size_t index;

    note_event(ORACLE_INITIALIZE);
    ++s_initialize_calls;
    s_params = *params;
    s_zero_at_initialize = 1U;
    for (index = 0U; index < sizeof s_memblock; ++index) {
        if (s_memblock[index] != 0U) {
            s_zero_at_initialize = 0U;
            break;
        }
    }
    return s_initialize_result;
}

void sceFiosTerminate(void)
{
    note_event(ORACLE_TERMINATE);
    ++s_terminate_calls;
}

int sceFiosIOFilterAdd(int index, void *callback, void *context)
{
    note_event(ORACLE_FILTER_ADD);
    ++s_filter_calls;
    s_filter_index = index;
    s_filter_callback = callback;
    s_context = *(const OracleFiosRamCacheContext *)context;
    return s_filter_result;
}

void sceFiosIOFilterCache(void)
{
}

static int check_event(unsigned index, enum oracle_event expected)
{
    return index < s_event_count && s_events[index] == expected;
}

static int check_active_layout(void)
{
    uintptr_t base = (uintptr_t)s_memblock;
    uintptr_t op = base;
    uintptr_t chunk = op + ORACLE_OP_BYTES;
    uintptr_t fh = chunk + ORACLE_CHUNK_BYTES;
    uintptr_t dh = fh + ORACLE_FH_BYTES;
    uintptr_t cache = dh + ORACLE_DH_BYTES;

    CHECK(strcmp(s_alloc_name, "isaac-fios-cache") == 0);
    CHECK(s_alloc_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    CHECK(s_alloc_size == ISAAC_VITA_FIOS_MEMBLOCK_BYTES);
    CHECK(s_alloc_options == NULL);
    CHECK(s_zero_at_initialize == 1U);
    CHECK(s_params.initialized == 0U);
    CHECK(s_params.paramsSize == sizeof s_params);
    CHECK(s_params.pathMax == ISAAC_VITA_FIOS_PATH_MAX);
    CHECK(s_params.ioThreadCount == 2U);
    CHECK(s_params.threadsPerScheduler == 1U);
    CHECK(s_params.maxChunk == 256U * 1024U);
    CHECK(s_params.maxDecompressorThreadCount == 2U);
    CHECK((uintptr_t)s_params.opStorage.pPtr == op);
    CHECK(s_params.opStorage.length == ORACLE_OP_BYTES);
    CHECK((uintptr_t)s_params.chunkStorage.pPtr == chunk);
    CHECK(s_params.chunkStorage.length == ORACLE_CHUNK_BYTES);
    CHECK((uintptr_t)s_params.fhStorage.pPtr == fh);
    CHECK(s_params.fhStorage.length == ORACLE_FH_BYTES);
    CHECK((uintptr_t)s_params.dhStorage.pPtr == dh);
    CHECK(s_params.dhStorage.length == ORACLE_DH_BYTES);
    CHECK(s_filter_index == 0);
    CHECK(s_filter_callback == (void *)(uintptr_t)&sceFiosIOFilterCache);
    CHECK(s_context.sizeOfContext == sizeof s_context);
    CHECK(s_context.workBufferSize == ISAAC_VITA_FIOS_CACHE_BYTES);
    CHECK(s_context.blockSize == ISAAC_VITA_FIOS_CACHE_BLOCK_BYTES);
    CHECK((uintptr_t)s_context.pWorkBuffer == cache);
    CHECK(strcmp(s_context.pPath, "ux0:data/isaacr001") == 0);
    CHECK(s_context.flags == 0);
    CHECK(cache + ISAAC_VITA_FIOS_CACHE_BYTES == base + 644024U);
    CHECK(cache + ISAAC_VITA_FIOS_CACHE_BYTES <=
          base + ISAAC_VITA_FIOS_MEMBLOCK_BYTES);
    return 0;
}

static int check_success(void)
{
    isaac_vita_fios_cache_snapshot snapshot;

    memset(s_memblock, 0xa5, sizeof s_memblock);
    CHECK(isaac_vita_fios_cache_initialize() == 1);
    CHECK(isaac_vita_fios_cache_initialize() == 1);
    CHECK(s_alloc_calls == 1U && s_get_base_calls == 1U);
    CHECK(s_initialize_calls == 1U && s_filter_calls == 1U);
    CHECK(s_terminate_calls == 0U && s_free_calls == 0U);
    CHECK(check_active_layout() == 0);
    CHECK(s_event_count == 4U);
    CHECK(check_event(0U, ORACLE_ALLOC));
    CHECK(check_event(1U, ORACLE_GET_BASE));
    CHECK(check_event(2U, ORACLE_INITIALIZE));
    CHECK(check_event(3U, ORACLE_FILTER_ADD));
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_READY);
    CHECK(snapshot.failure == ISAAC_VITA_FIOS_CACHE_FAILURE_NONE);
    CHECK(snapshot.uid == s_alloc_result && snapshot.filter_active == 1U);
    CHECK(snapshot.memblock_bytes == ISAAC_VITA_FIOS_MEMBLOCK_BYTES);
    CHECK(snapshot.cache_bytes == ISAAC_VITA_FIOS_CACHE_BYTES);

    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_TERMINATED);
    CHECK(snapshot.uid == -1 && snapshot.filter_active == 0U);
    CHECK(snapshot.terminate_calls == 1U && snapshot.free_calls == 1U);
    CHECK(s_event_count == 6U);
    CHECK(check_event(4U, ORACLE_TERMINATE));
    CHECK(check_event(5U, ORACLE_FREE));
    return 0;
}

static int check_simple_failure(const char *scenario)
{
    isaac_vita_fios_cache_snapshot snapshot;
    uint32_t expected_failure;

    if (strcmp(scenario, "alloc-fail") == 0) {
        s_alloc_result = -11;
        expected_failure = ISAAC_VITA_FIOS_CACHE_FAILURE_ALLOC;
    } else if (strcmp(scenario, "getbase-fail") == 0) {
        s_get_base_result = -12;
        expected_failure = ISAAC_VITA_FIOS_CACHE_FAILURE_GET_BASE;
    } else if (strcmp(scenario, "getbase-null") == 0) {
        s_get_base_null = 1;
        expected_failure = ISAAC_VITA_FIOS_CACHE_FAILURE_GET_BASE;
    } else if (strcmp(scenario, "layout-fail") == 0) {
        s_get_base_invalid = 1;
        expected_failure = ISAAC_VITA_FIOS_CACHE_FAILURE_LAYOUT;
    } else if (strcmp(scenario, "initialize-fail") == 0) {
        s_initialize_result = -13;
        expected_failure = ISAAC_VITA_FIOS_CACHE_FAILURE_INITIALIZE;
    } else {
        return 2;
    }

    CHECK(isaac_vita_fios_cache_initialize() == 0);
    CHECK(isaac_vita_fios_cache_initialize() == 0);
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_DISABLED);
    CHECK(snapshot.failure == expected_failure);
    CHECK(snapshot.filter_active == 0U);
    CHECK(s_terminate_calls == 0U);
    if (strcmp(scenario, "alloc-fail") == 0) {
        CHECK(s_get_base_calls == 0U && s_free_calls == 0U);
    } else {
        CHECK(s_free_calls == 1U);
    }
    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_TERMINATED);
    return 0;
}

static int check_cold_shutdown(void)
{
    isaac_vita_fios_cache_snapshot snapshot;

    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_TERMINATED);
    CHECK(snapshot.failure == ISAAC_VITA_FIOS_CACHE_FAILURE_NONE);
    CHECK(snapshot.uid == -1);
    CHECK(s_event_count == 0U);
    CHECK(snapshot.initialize_calls == 0U);
    CHECK(snapshot.filter_add_calls == 0U);
    CHECK(snapshot.terminate_calls == 0U);
    CHECK(snapshot.free_calls == 0U);
    return 0;
}

static int check_filter_failure(int orphan)
{
    isaac_vita_fios_cache_snapshot snapshot;

    s_filter_result = -14;
    s_free_result = orphan ? -15 : 0;
    CHECK(isaac_vita_fios_cache_initialize() == 0);
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(s_initialize_calls == 1U && s_filter_calls == 1U);
    CHECK(s_terminate_calls == 1U && s_free_calls == 1U);
    CHECK(snapshot.filter_active == 0U);
    CHECK(check_event(4U, ORACLE_TERMINATE));
    CHECK(check_event(5U, ORACLE_FREE));
    if (!orphan) {
        CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_DISABLED);
        CHECK(snapshot.failure == ISAAC_VITA_FIOS_CACHE_FAILURE_FILTER_ADD);
        return 0;
    }
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_ORPHANED);
    CHECK(snapshot.failure == ISAAC_VITA_FIOS_CACHE_FAILURE_FREE);
    CHECK(snapshot.uid == s_alloc_result);
    s_free_result = 0;
    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_TERMINATED);
    CHECK(snapshot.uid == -1);
    CHECK(snapshot.terminate_calls == 1U && snapshot.free_calls == 2U);
    return 0;
}

static int check_shutdown_orphan(void)
{
    isaac_vita_fios_cache_snapshot snapshot;

    CHECK(isaac_vita_fios_cache_initialize() == 1);
    s_free_result = -16;
    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_ORPHANED);
    CHECK(snapshot.uid == s_alloc_result);
    CHECK(snapshot.terminate_calls == 1U && snapshot.free_calls == 1U);
    s_free_result = 0;
    isaac_vita_fios_cache_shutdown();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    CHECK(snapshot.state == ISAAC_VITA_FIOS_CACHE_TERMINATED);
    CHECK(snapshot.uid == -1);
    CHECK(snapshot.terminate_calls == 1U && snapshot.free_calls == 2U);
    return 0;
}

int main(int argc, char **argv)
{
    const char *scenario = argc == 2 ? argv[1] : "success";
    int result;

    if (strcmp(scenario, "success") == 0)
        result = check_success();
    else if (strcmp(scenario, "filter-fail") == 0)
        result = check_filter_failure(0);
    else if (strcmp(scenario, "filter-free-fail") == 0)
        result = check_filter_failure(1);
    else if (strcmp(scenario, "shutdown-free-fail") == 0)
        result = check_shutdown_orphan();
    else if (strcmp(scenario, "cold-shutdown") == 0)
        result = check_cold_shutdown();
    else
        result = check_simple_failure(scenario);
    if (result == 0)
        printf("FIOS cache lifecycle oracle (%s): PASS\n", scenario);
    return result;
}
