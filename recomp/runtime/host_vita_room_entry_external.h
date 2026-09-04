#ifndef ISAAC_HOST_VITA_ROOM_ENTRY_EXTERNAL_H
#define ISAAC_HOST_VITA_ROOM_ENTRY_EXTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "host_vita_heap_overflow_mspace.h"

#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGE_BYTES       0x00010000U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_PAGES_PER_CHUNK          16U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES     0x00100000U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES    0x00101000U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES   0x00102000U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS               12U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_ISSUED_PAGES         192U
#define ISAAC_VITA_ROOM_ENTRY_EXTERNAL_INVALID_UID      ((int32_t)-1)

typedef enum isaac_vita_room_entry_external_state {
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_UNINITIALIZED = 0,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_READY = 1,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAILED = 2,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_DRAIN_ONLY = 3,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_ORPHANED = 4
} isaac_vita_room_entry_external_state;

typedef enum isaac_vita_room_entry_external_result {
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_EMPTY = 0,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SUCCESS = 1,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_OUT_OF_MEMORY = 2,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_LIMIT = 3,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TERMINAL = 4
} isaac_vita_room_entry_external_result;

typedef enum isaac_vita_room_entry_external_fault {
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NONE = 0,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_INIT = 1,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_STATE = 2,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_COUNTER = 3,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_DUPLICATE_UID = 4,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_GET_BASE = 5,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_NULL_BASE = 6,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ALIGNMENT = 7,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_WRAP = 8,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_WRAP = 9,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_FIXED = 10,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_STACK = 11,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_NEWLIB = 12,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_RAW = 13,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_ACTUAL_PRIOR = 14,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_FIXED = 15,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_STACK = 16,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_RETAINED_NEWLIB = 17,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_FAULT_PUBLICATION = 18
} isaac_vita_room_entry_external_fault;

typedef enum isaac_vita_room_entry_external_syscall {
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_NONE = 0,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ALLOC = 1,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_GET_BASE = 2,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_ROLLBACK_FREE = 3,
    ISAAC_VITA_ROOM_ENTRY_EXTERNAL_SYSCALL_RESET_FREE = 4
} isaac_vita_room_entry_external_syscall;

typedef struct isaac_vita_room_entry_external_ticket {
    void *page;
    uint32_t chunk_index;
    uint32_t page_index;
    uint32_t new_chunk;
} isaac_vita_room_entry_external_ticket;

typedef struct isaac_vita_room_entry_external_snapshot {
    isaac_vita_room_entry_external_state state;
    uint32_t chunks;
    uint32_t issued_pages;
    uint32_t pending;
    uint32_t allocation_attempts;
    uint32_t out_of_memory;
    uint32_t post_uid_failures;
    uint32_t rollback_free_attempts;
    uint32_t rollback_free_failures;
    uint32_t reset_free_attempts;
    uint32_t reset_free_failures;
    uint32_t counter_saturated;
    isaac_vita_room_entry_external_fault terminal_fault;
    isaac_vita_room_entry_external_syscall last_syscall;
    int32_t last_syscall_result;
    size_t requested_bytes;
    size_t usable_bytes;
    size_t retained_bytes;
    int32_t orphan_uid;
    size_t orphan_requested_bytes;
    size_t orphan_retained_bytes;
} isaac_vita_room_entry_external_snapshot;

#ifdef ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING
typedef struct isaac_vita_room_entry_external_test_receipt {
    int32_t uid;
    void *base;
    uint32_t chunk_index;
    uint32_t issued_pages;
    uint32_t orphan;
} isaac_vita_room_entry_external_test_receipt;
#endif

/* Policy-free retained backing.  The guest heap lock must cover every call.
 * Initialization caches immutable manual and raw-request ranges; allocation
 * stays lazy until the slab needs a page. */
int isaac_vita_room_entry_external_init(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden,
    const isaac_vita_heap_overflow_range *raw_request);
/* With allow_new_chunk==0 this only reserves an already-paid page and never
 * calls the kernel.  A successful reservation must be committed or cancelled
 * before any other operation. */
isaac_vita_room_entry_external_result
isaac_vita_room_entry_external_reserve_page(
    int allow_new_chunk, isaac_vita_room_entry_external_ticket *ticket_out);
int isaac_vita_room_entry_external_commit_page(
    const isaac_vita_room_entry_external_ticket *ticket);
/* Cancel handles an uncommitted reservation.  Unissue rolls back the exact
 * most recently committed page after a slab publication invariant fails.
 * A newly-empty chunk is freed; a failed Free retains an orphan receipt. */
int isaac_vita_room_entry_external_cancel_page(
    const isaac_vita_room_entry_external_ticket *ticket);
int isaac_vita_room_entry_external_unissue_page(
    const isaac_vita_room_entry_external_ticket *ticket);
int isaac_vita_room_entry_external_page_matches(
    uint32_t chunk_index, uint32_t page_index, const void *page);
int isaac_vita_room_entry_external_snapshot_get(
    isaac_vita_room_entry_external_snapshot *snapshot_out);

#ifdef ISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING
/* Test-only lifecycle cleanup is deliberately one receipt per call.  Asking
 * for the first receipt transitions READY to DRAIN_ONLY so backing can never
 * be reused while teardown is in progress.  The slab
 * first validates the newest chunk's descriptors, then attempts its release,
 * and removes those descriptors immediately after success while still under
 * the heap lock.  A failed Free leaves the exact UID and page receipt intact
 * for retry.  Orphans have no published pages and are always newest. */
int isaac_vita_room_entry_external_test_newest_receipt(
    isaac_vita_room_entry_external_test_receipt *receipt_out);
/* release_newest returns 1 only when Free succeeded (the caller must remove
 * that chunk's descriptors), 0 when Free failed and the receipt is intact,
 * and -1 when validation rejected the request before any syscall. */
int isaac_vita_room_entry_external_test_release_newest(
    const isaac_vita_room_entry_external_test_receipt *receipt);
int isaac_vita_room_entry_external_test_finish_reset(void);
void isaac_vita_room_entry_external_test_corrupt_chunk_count(void);
void isaac_vita_room_entry_external_test_corrupt_receipt_total(void);
void isaac_vita_room_entry_external_test_saturate_rollback_counter(void);
#endif

#endif
