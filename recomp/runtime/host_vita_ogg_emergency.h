#ifndef ISAAC_HOST_VITA_OGG_EMERGENCY_H
#define ISAAC_HOST_VITA_OGG_EMERGENCY_H

#include <stddef.h>
#include <stdint.h>

/* Frozen 8,650,240-byte PE facts.  StreamSourceOgg::Open stores this backing
 * at this+0x94 and retains it until ActivateNextQueuedData, Close, or the
 * destructor.  0x005a2413 is a decoder-result store in Queue, not an
 * allocation owner. Queue's distinct owner is admitted only by the opt-in
 * ISAAC_VITA_OGG_QUEUE_EMERGENCY policy; the capacity remains one live slot.
 * The frozen contract also pins Queue -> active transfer and all backing frees. */
#define ISAAC_VITA_OGG_OPEN_BACKING_BYTES          0x0004b000U
#define ISAAC_VITA_OGG_OPEN_ALLOCATION_CALL_RVA    0x005a2330U
#define ISAAC_VITA_OGG_OPEN_ALLOCATION_RETURN_RVA  0x005a2335U
#define ISAAC_VITA_OGG_QUEUE_ALLOCATION_RETURN_RVA 0x005a23fbU
#define ISAAC_VITA_OGG_QUEUE_DECODER_RETURN_RVA    0x005a2413U
#define ISAAC_VITA_OGG_THIRD_SIZE_OWNER_RVA        0x005a36fcU
#define ISAAC_VITA_OGG_MEMBLOCK_PAGE_BYTES         0x00001000U

typedef enum isaac_vita_ogg_emergency_result {
    ISAAC_VITA_OGG_EMERGENCY_REJECTED = -1,
    ISAAC_VITA_OGG_EMERGENCY_NOT_HANDLED = 0,
    ISAAC_VITA_OGG_EMERGENCY_HANDLED = 1
} isaac_vita_ogg_emergency_result;

typedef struct isaac_vita_ogg_emergency_decision {
    void *pointer;
    uint32_t fault_value;
    const char *fault;
} isaac_vita_ogg_emergency_decision;

/* The caller must invoke this only after ordinary guest malloc returned NULL
 * specifically because native malloc failed.  The owner and size are checked
 * again here.  A clean reservation failure, a live slot, or lock contention
 * returns NOT_HANDLED so operator new preserves its original bad_alloc path. */
int isaac_vita_ogg_emergency_malloc(
    uint32_t owner_return_rva, size_t size,
    uint32_t guest_stack_floor, uint32_t guest_stack_ceiling,
    isaac_vita_ogg_emergency_decision *decision);

/* The dedicated USER_RW memblock remains retained for reuse.  Only its exact
 * base is an owned guest pointer; interior, stale, and concurrent operations
 * reject before newlib can see a foreign pointer. */
int isaac_vita_ogg_emergency_free(
    void *pointer, isaac_vita_ogg_emergency_decision *decision);
int isaac_vita_ogg_emergency_realloc(
    void *pointer, size_t size,
    isaac_vita_ogg_emergency_decision *decision);

#ifdef ISAAC_VITA_HEAP_CENSUS
/* ph120.mem slot= field (host_vita_heap.c census).  Live slot count (0 or 1
 * for the one-slot policy) read under the module lock; UINT32_MAX when the
 * lock is busy, so the receipt never blocks a concurrent slot operation. */
uint32_t isaac_vita_ogg_emergency_live_count(void);
#endif

#ifdef ISAAC_VITA_OGG_EMERGENCY_ORACLE
typedef struct isaac_vita_ogg_emergency_snapshot {
    uintptr_t base;
    int32_t uid;
    uint32_t poisoned;
    uint32_t live;
    size_t live_size;
    uint32_t acquired_count;
    uint32_t freed_count;
    uint32_t live_fallback_count;
    uint32_t reserve_failure_count;
} isaac_vita_ogg_emergency_snapshot;

int isaac_vita_ogg_emergency_oracle_snapshot(
    isaac_vita_ogg_emergency_snapshot *snapshot);
int isaac_vita_ogg_emergency_oracle_reset(void);
int isaac_vita_ogg_emergency_oracle_force_lock(int locked);
#endif

#endif
