#ifndef ISAAC_HOST_VITA_HEAP_OVERFLOW_MSPACE_H
#define ISAAC_HOST_VITA_HEAP_OVERFLOW_MSPACE_H

#include <stddef.h>
#include <stdint.h>

/* Keep the request's low bit at 4 KiB.  Vita3K's USER_RW allocator retains
 * one additional low-bit quantum, so this exact 0x007d5000-byte request has
 * a measured retained span of 0x007d6000 bytes. */
#define ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES          0x007d5000U
#define ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES 0x007d6000U
#define ISAAC_VITA_HEAP_OVERFLOW_MSPACE_PAGE_BYTES     0x00001000U
#define ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ALIGNMENT      8U
#define ISAAC_VITA_HEAP_OVERFLOW_INTERNAL_PAGE_BYTES   0x00010000U
#define ISAAC_VITA_HEAP_OVERFLOW_MSPACE_INVALID_UID    ((int32_t)-1)

typedef struct isaac_vita_heap_overflow_range {
    uintptr_t begin;
    uintptr_t end;
} isaac_vita_heap_overflow_range;

/* All ranges are half-open and must be non-empty. */
typedef struct isaac_vita_heap_overflow_forbidden_ranges {
    isaac_vita_heap_overflow_range fixed_image;
    isaac_vita_heap_overflow_range guest_stack;
    isaac_vita_heap_overflow_range retained_newlib;
} isaac_vita_heap_overflow_forbidden_ranges;

typedef enum isaac_vita_heap_overflow_mspace_state {
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_UNINITIALIZED = 0,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY = 1,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED = 2,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED = 3,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY = 4
} isaac_vita_heap_overflow_mspace_state;

typedef enum isaac_vita_heap_overflow_mspace_realloc_status {
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_REJECTED = 0,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_OUT_OF_MEMORY = 1,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS = 2,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_FREED = 3,
    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_CONSUMED_DRAIN_ONLY = 4
} isaac_vita_heap_overflow_mspace_realloc_status;

typedef struct isaac_vita_heap_overflow_mspace_snapshot {
    isaac_vita_heap_overflow_mspace_state state;
    int32_t uid;
    uintptr_t base;
    uintptr_t end;
    uintptr_t retained_end;
    size_t capacity_bytes;
    size_t retained_bytes;
    size_t live_count;
    size_t stranded_count;
    /* Process-lifetime allocations reserved by native sub-allocators.
     * live_count remains the count of ALL live mspace allocations; these two
     * fields identify the internal subset for exact outer accounting. */
    size_t internal_live_count;
    size_t internal_requested_bytes;
    int has_mspace;
} isaac_vita_heap_overflow_mspace_snapshot;

/* This module deliberately has no internal lock.  The guest-heap router must
 * hold its one heap lock across every call below and across its ownership
 * ledger transaction.  Initialization is one-shot: FAILED and ORPHANED never
 * retry in production. */
int isaac_vita_heap_overflow_mspace_init(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden);
/* Domain classification only: interior and stale addresses still classify as
 * inside this pool.  It is not an ownership predicate. */
int isaac_vita_heap_overflow_mspace_contains(const void *pointer);
void *isaac_vita_heap_overflow_mspace_malloc(size_t size);
void *isaac_vita_heap_overflow_mspace_calloc(size_t count, size_t size);
/* Native sub-allocators may retain bounded backing from this same mspace.
 * The caller must hold the guest heap lock and return the exact page base.
 * Fixed-size pages keep retry/reset accounting independently checkable. */
void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void);
int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer);
/* Non-NULL pointer arguments to realloc/free must already be exact live bases
 * accepted by the outer ownership ledger.  This raw module intentionally has
 * no duplicate/interior/stale-pointer index of its own.
 *
 * A successful underlying realloc can consume the old allocation before this
 * module detects a corrupt alignment/range result.  In that case NULL is
 * returned with CONSUMED_DRAIN_ONLY, never OUT_OF_MEMORY; the outer ledger
 * must retire the old base and fail closed. */
void *isaac_vita_heap_overflow_mspace_realloc(
    void *pointer, size_t size,
    isaac_vita_heap_overflow_mspace_realloc_status *status_out);
int isaac_vita_heap_overflow_mspace_free(void *pointer);
int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot_out);

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING
/* Test-only lifecycle cleanup.  It refuses to destroy an mspace while any
 * published allocation remains live.  It may retry release of an orphaned
 * UID solely so one oracle process can isolate multiple failure cases. */
int isaac_vita_heap_overflow_mspace_test_reset(void);
#endif

#endif
