#ifndef ISAAC_HOST_VITA_OPENAL_POOL_H
#define ISAAC_HOST_VITA_OPENAL_POOL_H

#include <stddef.h>
#include <stdint.h>

/* This is the exact one-block size that still succeeded in the v0.1.8
 * failure-time USER_RW probe.  Vita3K retains one additional low-bit quantum
 * (0x2000) for this request.  The mspace metadata and each allocation's header
 * and alignment padding live inside this backing span, so it is not a claim
 * that every requested byte is simultaneously available as payload. */
#define ISAAC_VITA_OPENAL_POOL_BYTES          0x00bd6000U
#define ISAAC_VITA_OPENAL_POOL_RETAINED_BYTES 0x00bd8000U
#define ISAAC_VITA_OPENAL_POOL_MIN_REQUEST_ALIGNMENT 4U
#define ISAAC_VITA_OPENAL_POOL_ALIGNMENT      8U
#define ISAAC_VITA_OPENAL_POOL_MAX_ALIGNMENT  4096U
#define ISAAC_VITA_OPENAL_POOL_INVALID_UID    ((int32_t)-1)

typedef enum isaac_vita_openal_pool_state {
    ISAAC_VITA_OPENAL_POOL_UNINITIALIZED = 0,
    ISAAC_VITA_OPENAL_POOL_INITIALIZING = 1,
    ISAAC_VITA_OPENAL_POOL_READY = 2,
    ISAAC_VITA_OPENAL_POOL_FALLBACK = 3,
    ISAAC_VITA_OPENAL_POOL_ORPHANED_FALLBACK = 4,
    ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY = 5
} isaac_vita_openal_pool_state;

typedef enum isaac_vita_openal_pool_init_failure {
    ISAAC_VITA_OPENAL_POOL_INIT_OK = 0,
    ISAAC_VITA_OPENAL_POOL_INIT_DISABLED = 1,
    ISAAC_VITA_OPENAL_POOL_INIT_PREINIT_ALLOCATION = 2,
    ISAAC_VITA_OPENAL_POOL_INIT_INVALID_FIXED_IMAGE = 3,
    ISAAC_VITA_OPENAL_POOL_INIT_MUTEX = 4,
    ISAAC_VITA_OPENAL_POOL_INIT_MEMBLOCK = 5,
    ISAAC_VITA_OPENAL_POOL_INIT_GET_BASE = 6,
    ISAAC_VITA_OPENAL_POOL_INIT_ADDRESS = 7,
    ISAAC_VITA_OPENAL_POOL_INIT_FIXED_IMAGE_OVERLAP = 8,
    ISAAC_VITA_OPENAL_POOL_INIT_MSPACE = 9,
    ISAAC_VITA_OPENAL_POOL_INIT_ROLLBACK = 10
} isaac_vita_openal_pool_init_failure;

typedef struct isaac_vita_openal_pool_snapshot {
    isaac_vita_openal_pool_state state;
    int32_t uid;
    int32_t mutex_uid;
    uintptr_t base;
    uintptr_t end;
    uintptr_t retained_end;
    uint32_t backing_bytes;
    uint32_t live_count;
    uint32_t stranded_count;
    uint32_t live_requested_bytes;
    uint32_t peak_live_count;
    uint32_t peak_requested_bytes;
    uint32_t allocations;
    uint32_t frees;
    uint32_t reallocations;
    /* Failed READY-pool allocation attempts.  Invalid API arguments and
     * whole-session libc fallback failures are deliberately not included. */
    uint32_t failures;
    uint32_t corruption;
    uint32_t preinit_native_allocations;
    isaac_vita_openal_pool_init_failure init_failure;
    int has_mspace;
} isaac_vita_openal_pool_snapshot;

/* Call once on the owner thread before the first OpenAL API.  A clean init
 * failure selects the old newlib domain for the whole session.  Once READY,
 * allocation exhaustion never falls back per allocation and therefore cannot
 * silently reintroduce mixer/newlib sharing.  The complete retained pool span
 * is rejected if it intersects the already-mapped fixed guest image. */
int isaac_vita_openal_pool_initialize(uintptr_t fixed_image_begin,
                                      uintptr_t fixed_image_end);

/* These six symbols are the target of the pinned OpenAL archive's compile-
 * time malloc/calloc/realloc/free/aligned_alloc/strdup redirect.  Pointers allocated before init or
 * during whole-session fallback retain native-libc routing; pool pointers are
 * selected only by the immutable half-open backing range. */
void *isaac_vita_openal_pool_malloc(size_t size);
void *isaac_vita_openal_pool_calloc(size_t count, size_t size);
void *isaac_vita_openal_pool_realloc(void *pointer, size_t size);
void isaac_vita_openal_pool_free(void *pointer);
void *isaac_vita_openal_pool_aligned_alloc(size_t alignment, size_t size);
char *isaac_vita_openal_pool_strdup(const char *string);

int isaac_vita_openal_pool_snapshot_get(
    isaac_vita_openal_pool_snapshot *snapshot_out);

#ifdef ISAAC_VITA_OPENAL_POOL_TESTING
int isaac_vita_openal_pool_test_reset(void);
#endif

#endif
