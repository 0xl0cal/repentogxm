#ifndef ISAAC_HOST_VITA_ROOM_ENTRY_SLAB_H
#define ISAAC_HOST_VITA_ROOM_ENTRY_SLAB_H

#include <stddef.h>
#include <stdint.h>

#define ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA          0x003e5adeU
#define ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA    0x003e3806U
#define ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA     0x003e3b66U
#define ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES             12U

#define ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES           0x00010000U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_BITMAP_BYTES         504U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_DATA_OFFSET          0x00000400U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_SLOT_STRIDE           16U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_SLOTS_PER_PAGE       4032U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES          96U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES       192U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_SLOTS      387072U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_SLOTS    774144U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_BACKING_RETRY_INTERVAL 4032U
/* Legacy first-tier aliases stay macro-independent for every public consumer.
 * Hybrid-aware code uses the explicit TOTAL constants; only slab.c selects
 * its private implementation cap from the feature macro. */
#define ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_PAGES \
    ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES
#define ISAAC_VITA_ROOM_ENTRY_SLAB_MAX_SLOTS \
    ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_SLOTS
#define ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_BACKING_BYTES 0x00600000U
#define ISAAC_VITA_ROOM_ENTRY_SLAB_NOMINAL_RAW_REMAINDER 0x001d5000U

/* Floor-lifetime birth token shared with the generic allocation ledger.
 * Bits 0..1 are the exact birth phase and bit 2 says that the allocation
 * belongs to the current floor epoch.  Phase zero is deliberately unscoped
 * and may never carry CURRENT. */
#define ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_UNSCOPED  0U
#define ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT 1U
#define ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD  2U
#define ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY       3U
#define ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK UINT32_C(0x3)
#define ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT    UINT32_C(0x4)

typedef enum isaac_vita_room_entry_slab_result {
    ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED = 0,
    ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED = 1,
    ISAAC_VITA_ROOM_ENTRY_SLAB_FALLBACK = 2,
    ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED = 3,
    ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL = 4,
    ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE = 5
} isaac_vita_room_entry_slab_result;

typedef struct isaac_vita_room_entry_slab_decision {
    void *pointer;
    const char *fault;
    uint32_t fault_value;
    /* Set for every owned realloc result.  In particular MOVE exposes the
     * old slot's birth token so the replacement ledger entry can be retagged
     * before realloc_commit retires the old slot. */
    uint32_t floorlife_token;
} isaac_vita_room_entry_slab_decision;

typedef struct isaac_vita_room_entry_slab_floor_lifetime_snapshot {
    uint32_t live_slots;
    uint32_t unscoped_slots;
    uint32_t prior_slots;
    uint32_t current_slots;
    /* CURRENT slots split by their immutable birth phase.  Their sum equals
     * current_slots; PRIOR retains its phase bits only for exact frees and
     * realloc token inheritance, not for these hot telemetry buckets. */
    uint32_t level_init_slots;
    uint32_t room_load_slots;
    uint32_t play_slots;
    uint32_t epoch;
    uint32_t phase;
    uint32_t active;
    uint32_t valid;
    uint32_t terminal;
    uint32_t counter_saturated;
} isaac_vita_room_entry_slab_floor_lifetime_snapshot;

typedef struct isaac_vita_room_entry_slab_snapshot {
    uint32_t pages;
    uint32_t raw_pages;
    uint32_t external_pages;
    uint32_t external_chunks;
    uint32_t live_slots;
    uint32_t moving_slots;
    uint32_t peak_live_slots;
    uint32_t allocations;
    uint32_t frees;
    uint32_t reallocations;
    uint32_t move_attempts;
    uint32_t moves;
    uint32_t fallbacks;
    uint32_t known_normal_frees;
    uint32_t known_stage_frees;
    uint32_t unexpected_frees;
    uint32_t rejected;
    uint32_t raw_internal_live;
    uint32_t raw_internal_requested;
    uint32_t external_allocation_attempts;
    uint32_t external_out_of_memory;
    uint32_t external_post_uid_failures;
    uint32_t external_rollback_free_attempts;
    uint32_t external_rollback_free_failures;
    uint32_t external_reset_free_attempts;
    uint32_t external_reset_free_failures;
    uint32_t external_counter_saturated;
    uint32_t external_requested;
    uint32_t external_usable;
    uint32_t external_retained;
    uint32_t external_orphan_requested;
    uint32_t external_orphan_retained;
    uint32_t backing_retry_remaining;
    uint32_t backing_retry_suppressed;
    uint32_t external_state;
    uint32_t external_terminal_fault;
    uint32_t external_last_syscall;
    int32_t external_last_syscall_result;
    int32_t external_orphan_uid;
    uint32_t terminal;
    uint32_t counter_saturated;
} isaac_vita_room_entry_slab_snapshot;

/* Constant-work scalar observer for hot diagnostics which already hold the
 * guest heap lock.  Unlike the full snapshot below, this never scans page
 * bitmaps or external backing receipts. */
typedef struct isaac_vita_room_entry_slab_fast_snapshot {
    uint32_t pages;
    uint32_t raw_pages;
    uint32_t external_pages;
    uint32_t live_slots;
    uint32_t allocations;
    uint32_t frees;
    uint32_t terminal;
    uint32_t counter_saturated;
} isaac_vita_room_entry_slab_fast_snapshot;

typedef struct isaac_vita_room_entry_slab_event {
    uint32_t edges;
    isaac_vita_room_entry_slab_snapshot snapshot;
} isaac_vita_room_entry_slab_event;

typedef int (*isaac_vita_room_entry_slab_lease_predicate)(uintptr_t base);

/* The guest heap's one lock must cover every state-changing call.  Event
 * formatting/logging is deliberately separate and must happen after unlock. */
void isaac_vita_room_entry_slab_event_init(
    isaac_vita_room_entry_slab_event *event);
isaac_vita_room_entry_slab_result isaac_vita_room_entry_slab_malloc_locked(
    uint32_t owner_return_rva, size_t size,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event);
isaac_vita_room_entry_slab_result isaac_vita_room_entry_slab_free_locked(
    void *pointer, uint32_t free_return_rva,
    isaac_vita_room_entry_slab_lease_predicate is_leased,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event);
/* Every non-zero size other than the exact 12-byte class returns MOVE and
 * marks the slot moving without consuming it.  This keeps requested extents
 * and range leases truthful after a shrink as well as a growth. */
isaac_vita_room_entry_slab_result
isaac_vita_room_entry_slab_realloc_begin_locked(
    void *pointer, size_t size,
    isaac_vita_room_entry_slab_lease_predicate is_leased,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event);
int isaac_vita_room_entry_slab_realloc_cancel_locked(
    void *pointer, isaac_vita_room_entry_slab_event *event);
int isaac_vita_room_entry_slab_realloc_commit_locked(
    void *pointer, isaac_vita_room_entry_slab_event *event);
/* Full cold observer: a zero result reports a cold invariant mismatch but
 * neither mutates slab state nor latches the outer heap terminal.  Callers
 * must treat zero as an invalid diagnostic snapshot.  State-changing
 * transactions use event claims, which fail closed and propagate TERMINAL
 * before unlock. */
int isaac_vita_room_entry_slab_snapshot_locked(
    isaac_vita_room_entry_slab_snapshot *snapshot_out);
/* Bounded fast observer: zero proves only that scalar/non-full-mask accounting
 * is inconsistent.  It deliberately does not validate page bitmaps or
 * external backing receipts. */
int isaac_vita_room_entry_slab_fast_snapshot_locked(
    isaac_vita_room_entry_slab_fast_snapshot *snapshot_out);
/* Floor-lifetime coordinator API.  The guest heap's single lock is a strict
 * precondition for all four calls.  They perform fixed scalar work except
 * rollover, which always clears the same compile-time-bounded CURRENT side
 * bitmap (96 raw pages or 192 Hybrid pages); none allocates, logs, or scans
 * live-slot bitmaps.  Bootstrap leaves all already-live slots unscoped.
 * A zero lifecycle result latches diagnostic invalidity only: slab allocation
 * and freeing continue unchanged. */
int isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
    uint32_t phase);
int isaac_vita_room_entry_slab_floor_lifetime_rollover_locked(void);
int isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
    uint32_t phase);
int isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot_out);
/* O(1) hot-path accounting observer.  It checks only the slab's bounded
 * scalar/mask invariants; exhaustive bitmap/backing validation belongs to
 * snapshot_locked and transaction claims. */
int isaac_vita_room_entry_slab_raw_accounting_locked(
    uint32_t *pages_out, uint32_t *requested_bytes_out);
int isaac_vita_room_entry_slab_owns_exact_locked(const void *pointer);
int isaac_vita_room_entry_slab_find_containing_locked(
    const void *range, size_t size, uintptr_t *allocation_base_out);
void isaac_vita_room_entry_slab_claim_final_locked(
    isaac_vita_room_entry_slab_event *event);
void isaac_vita_room_entry_slab_log_event(
    const isaac_vita_room_entry_slab_event *event);

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING
/* Retry-safe cleanup: live/moving slots refuse reset; successfully released
 * pages are forgotten one by one so a later retry resumes at the remainder. */
int isaac_vita_room_entry_slab_test_reset_locked(void);
void isaac_vita_room_entry_slab_test_probe_reset_locked(void);
uint32_t isaac_vita_room_entry_slab_test_probe_count_locked(void);
void isaac_vita_room_entry_slab_test_allocation_probe_reset_locked(void);
uint32_t isaac_vita_room_entry_slab_test_allocation_probe_count_locked(void);
void isaac_vita_room_entry_slab_test_fail_next_commit_locked(void);
void isaac_vita_room_entry_slab_test_fail_next_page_publish_locked(void);
void isaac_vita_room_entry_slab_test_fail_after_external_commit_locked(void);
void isaac_vita_room_entry_slab_test_corrupt_nonfull_locked(void);
void isaac_vita_room_entry_slab_test_corrupt_future_nonfull_locked(void);
void isaac_vita_room_entry_slab_test_corrupt_first_page_source_locked(void);
void isaac_vita_room_entry_slab_test_corrupt_first_external_tag_locked(void);
void isaac_vita_room_entry_slab_test_corrupt_first_external_base_locked(void);
int isaac_vita_room_entry_slab_test_floor_lifetime_validate_cold_locked(void);
void isaac_vita_room_entry_slab_test_floor_lifetime_force_epoch_max_locked(
    void);
/* Toggle, rather than overwrite, the first live slot so the oracle can prove
 * cold detection and then restore the exact production metadata before free. */
int isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_phase_locked(
    void);
int isaac_vita_room_entry_slab_test_floor_lifetime_toggle_first_live_current_locked(
    void);
uint32_t isaac_vita_room_entry_slab_test_floor_lifetime_forget_row_moves_locked(
    void);
#endif

#endif
