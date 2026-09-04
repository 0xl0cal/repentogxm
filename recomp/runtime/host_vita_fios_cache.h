#ifndef ISAAC_HOST_VITA_FIOS_CACHE_H
#define ISAAC_HOST_VITA_FIOS_CACHE_H

#include <stdint.h>

/* The cache and every FIOS-owned bookkeeping buffer live in one dedicated
 * USER_RW memblock.  Keep the complete request below one MiB: late physical
 * gameplay has shown only about 11.5 MiB of USER memory free. */
#define ISAAC_VITA_FIOS_PATH_MAX             256U
#define ISAAC_VITA_FIOS_OP_COUNT              64U
#define ISAAC_VITA_FIOS_CHUNK_COUNT         1024U
#define ISAAC_VITA_FIOS_FH_COUNT               64U
#define ISAAC_VITA_FIOS_DH_COUNT               16U
#define ISAAC_VITA_FIOS_CACHE_BLOCK_BYTES  0x10000U
#define ISAAC_VITA_FIOS_CACHE_BLOCK_COUNT       8U
#define ISAAC_VITA_FIOS_CACHE_BYTES         0x80000U
#define ISAAC_VITA_FIOS_MEMBLOCK_BYTES      0x9e000U

typedef enum isaac_vita_fios_cache_state {
    ISAAC_VITA_FIOS_CACHE_COLD = 0,
    ISAAC_VITA_FIOS_CACHE_INITIALIZING,
    ISAAC_VITA_FIOS_CACHE_READY,
    ISAAC_VITA_FIOS_CACHE_DISABLED,
    ISAAC_VITA_FIOS_CACHE_ORPHANED,
    ISAAC_VITA_FIOS_CACHE_TERMINATED
} isaac_vita_fios_cache_state;

typedef enum isaac_vita_fios_cache_failure {
    ISAAC_VITA_FIOS_CACHE_FAILURE_NONE = 0,
    ISAAC_VITA_FIOS_CACHE_FAILURE_ALLOC,
    ISAAC_VITA_FIOS_CACHE_FAILURE_GET_BASE,
    ISAAC_VITA_FIOS_CACHE_FAILURE_LAYOUT,
    ISAAC_VITA_FIOS_CACHE_FAILURE_INITIALIZE,
    ISAAC_VITA_FIOS_CACHE_FAILURE_FILTER_ADD,
    ISAAC_VITA_FIOS_CACHE_FAILURE_FREE
} isaac_vita_fios_cache_failure;

typedef struct isaac_vita_fios_cache_snapshot {
    uint32_t state;
    uint32_t failure;
    int32_t uid;
    int32_t alloc_result;
    int32_t get_base_result;
    int32_t initialize_result;
    int32_t filter_add_result;
    int32_t free_result;
    uint32_t initialize_calls;
    uint32_t filter_add_calls;
    uint32_t terminate_calls;
    uint32_t free_calls;
    uint32_t memblock_bytes;
    uint32_t cache_bytes;
    uint32_t filter_active;
} isaac_vita_fios_cache_snapshot;

#ifdef __cplusplus
extern "C" {
#endif

/* Return one only when the RAM-cache filter is active.  Zero is an intentional
 * fail-open result: all ordinary stdio/sceIo behaviour remains available. */
int isaac_vita_fios_cache_initialize(void);

/* Idempotently terminate FIOS before releasing its backing memblock. */
void isaac_vita_fios_cache_shutdown(void);

void isaac_vita_fios_cache_snapshot_get(
    isaac_vita_fios_cache_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif
