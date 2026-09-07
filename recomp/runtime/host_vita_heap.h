#ifndef ISAAC_HOST_VITA_HEAP_H
#define ISAAC_HOST_VITA_HEAP_H

#include <stdint.h>
#include <stddef.h>

#include "guest.h"

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
#include "host_vita_heap_overflow_mspace.h"
#define ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY         524288U
#define ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR   3U
#define ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR 4U
#define ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT \
    ((ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY / \
      ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR) * \
     ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR)
#define ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_BYTES            8U
#define ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_TABLE_BYTES      0x00400000U
#define ISAAC_VITA_GUEST_HEAP_FLOOR_PHASE_BYTES             0x00080000U
#define ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES           0x00010000U
#define ISAAC_VITA_GUEST_HEAP_LEDGER_USABLE_BYTES           0x00490000U
#define ISAAC_VITA_GUEST_HEAP_LEDGER_REQUEST_BYTES          0x00491000U
#define ISAAC_VITA_GUEST_HEAP_LEDGER_RETAINED_BYTES         0x00492000U
#define ISAAC_VITA_GUEST_HEAP_TOTAL_REQUEST_BYTES           0x00c66000U
#define ISAAC_VITA_GUEST_HEAP_TOTAL_RETAINED_BYTES          0x00c68000U
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
#include "host_vita_room_entry_slab.h"
#endif

/* Exact UCRT heap imports from the selected PE.  The representative call
 * sites pin the x86 cdecl convention as well as the IAT assignment. */
#define ISAAC_VITA_HEAP_CALLNEWH_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!_callnewh"
#define ISAAC_VITA_HEAP_CALLNEWH_IAT_RVA     0x006064f8U
#define ISAAC_VITA_HEAP_CALLNEWH_THUNK_RVA   0x005ec18eU
#define ISAAC_VITA_HEAP_CALLNEWH_CALL_RVA    0x005ead00U
#define ISAAC_VITA_HEAP_CALLNEWH_RETURN_RVA  0x005ead05U

#define ISAAC_VITA_HEAP_FREE_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!free"
#define ISAAC_VITA_HEAP_FREE_IAT_RVA         0x006064fcU
#define ISAAC_VITA_HEAP_FREE_CALL_RVA        0x005ddb21U
#define ISAAC_VITA_HEAP_FREE_RETURN_RVA      0x005ddb27U

#define ISAAC_VITA_HEAP_CALLOC_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!calloc"
#define ISAAC_VITA_HEAP_CALLOC_IAT_RVA       0x00606500U
#define ISAAC_VITA_HEAP_CALLOC_CALL_RVA      0x005ab9afU
#define ISAAC_VITA_HEAP_CALLOC_RETURN_RVA    0x005ab9b5U

#define ISAAC_VITA_HEAP_MALLOC_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!malloc"
#define ISAAC_VITA_HEAP_MALLOC_IAT_RVA       0x00606504U
#define ISAAC_VITA_HEAP_MALLOC_CALL_RVA      0x00565878U
#define ISAAC_VITA_HEAP_MALLOC_RETURN_RVA    0x0056587eU

/* The USERENV fallback stores one empty std::string in a vector.  Its first
 * growth requests one 24-byte element through the common malloc wrapper. */
#define ISAAC_VITA_HEAP_USERENV_VECTOR_ALLOC_CALL_RVA   0x00022095U
#define ISAAC_VITA_HEAP_USERENV_VECTOR_ALLOC_RETURN_RVA 0x0002209aU
#define ISAAC_VITA_HEAP_WRAPPER_MALLOC_CALL_RVA         0x005ead0dU
#define ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA       0x005ead12U
#define ISAAC_VITA_HEAP_USERENV_VECTOR_ALLOC_SIZE       24U
#define ISAAC_VITA_HEAP_USERENV_VECTOR_BOOT_CALL_COUNT  1U

#define ISAAC_VITA_HEAP_SET_NEW_MODE_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!_set_new_mode"
#define ISAAC_VITA_HEAP_SET_NEW_MODE_IAT_RVA     0x00606508U
#define ISAAC_VITA_HEAP_SET_NEW_MODE_THUNK_RVA   0x005ec206U
#define ISAAC_VITA_HEAP_SET_NEW_MODE_CALL_RVA    0x005eb6b5U
#define ISAAC_VITA_HEAP_SET_NEW_MODE_RETURN_RVA  0x005eb6baU

#define ISAAC_VITA_HEAP_REALLOC_NAME \
    "api-ms-win-crt-heap-l1-1-0.dll!realloc"
#define ISAAC_VITA_HEAP_REALLOC_IAT_RVA      0x0060650cU
#define ISAAC_VITA_HEAP_REALLOC_CALL_RVA     0x005c7c07U
#define ISAAC_VITA_HEAP_REALLOC_RETURN_RVA   0x005c7c0dU

#define ISAAC_VITA_HEAP_IMPORT_COUNT 6U

/* Shared ownership boundary for every native allocation that is exposed as
 * a guest pointer.  Future `_strdup`/locale/path producers must use this API
 * too; otherwise their pointer will be rejected by imported free/realloc. */
void *isaac_vita_guest_malloc(size_t size);
void *isaac_vita_guest_calloc(size_t count, size_t size);
void *isaac_vita_guest_realloc(void *pointer, size_t size, int *valid_owner);
int isaac_vita_guest_free(void *pointer);
int isaac_vita_guest_heap_owns(const void *pointer);
/* Durable fail-closed state for native helpers which use the public heap API
 * without receiving the import router's per-call status.  It is always
 * available so callers need no overflow feature definition; builds without
 * the overflow router return zero. */
int isaac_vita_guest_heap_terminal(void);

/* Stage/floor memory checkpoints emitted by exact generated-code seams.  The
 * ABI is scalar-only on purpose: generated code must never give a host
 * diagnostic access to CPU or guest-memory pointers.  UINT32_MAX is the
 * required result for every event except ROOM_LOAD_END, whose result is the
 * original boolean AL value (zero or one).  These aggregate near-point deltas
 * localise pressure; they do not prove allocation ownership or lifetime.
 * This is non-release diagnostic instrumentation: mallinfo and synchronous
 * logging can stall.  Only its guest-lock scalar snapshot is bounded. */
typedef enum isaac_vita_stage_memory_event {
    ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN = 1,
    ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE = 2,
    ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN = 3,
    ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD = 4,
    ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END = 5,
    ISAAC_VITA_STAGE_MEMORY_LEVEL_END = 6
} isaac_vita_stage_memory_event;

/* Measurement-only floor lifetime attribution for exact requested-size
 * ledgers.  Existing allocations remain unscoped when the first valid floor
 * boundary bootstraps the epoch.  `phase_*` contains only current-floor live
 * allocations, so its three buckets sum exactly to `current_*`; prior is
 * scoped minus current and is deliberately not split by historical phase. */
typedef enum isaac_vita_floor_lifetime_phase {
    ISAAC_VITA_FLOOR_LIFETIME_PHASE_PRE_FLOOR = 0,
    ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT = 1,
    ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD = 2,
    ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY = 3
} isaac_vita_floor_lifetime_phase;

typedef struct isaac_vita_guest_heap_floor_lifetime_snapshot {
    uint32_t epoch;
    uint32_t bootstrap_count;
    uint32_t rollover_count;
    uint32_t phase;
    uint32_t total_count;
    uint32_t unscoped_count;
    uint32_t prior_count;
    uint32_t current_count;
    uint32_t total_requested_bytes;
    uint32_t unscoped_requested_bytes;
    uint32_t prior_requested_bytes;
    uint32_t current_requested_bytes;
    uint32_t phase_count[3];
    uint32_t phase_requested_bytes[3];
    uint32_t valid;
    uint32_t terminal;
    uint32_t counter_saturated;
} isaac_vita_guest_heap_floor_lifetime_snapshot;

void isaac_vita_stage_memory_note(uint32_t event, uint32_t stage,
                                  uint32_t mode_or_type, uint32_t result);

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
int isaac_vita_guest_heap_floor_lifetime_snapshot_get(
    isaac_vita_guest_heap_floor_lifetime_snapshot *snapshot_out);
#endif

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
/* Complete the guest-visible allocator before translated code can run.  This
 * eagerly fixes the ownership ledger at its production capacity and creates
 * the bounded USER_RW overflow mspace.  Failure is terminal for guest setup;
 * this API never leaves a usable native-only fallback mode. */
int isaac_vita_guest_heap_init(uint32_t stack_floor, uint32_t stack_ceiling);
#endif

#if defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE) || \
    defined(ISAAC_VITA_IO_PROFILE_HEAP)
/* O(1) process-lifetime telemetry for the exact guest-owned part of the
 * overflow domain.  All fields are deliberately 32-bit so the Vita logger
 * never depends on unsupported 64-bit printf formats.  raw_live_count counts
 * every mspace allocation, including stranded results and internal retained
 * pages; a valid snapshot therefore requires raw_live_count ==
 * owned_live_count + raw_stranded_count + raw_internal_live_count. */
typedef struct isaac_vita_guest_heap_telemetry_snapshot {
    uint32_t sequence;
    uint32_t owned_live_count;
    uint32_t owned_requested_bytes;
    uint32_t peak_live_count;
    uint32_t peak_requested_bytes;
    uint32_t pool_allocations;
    uint32_t pool_frees;
    uint32_t pool_reallocations;
    uint32_t native_failures;
    uint32_t pool_failures;
    uint32_t native_to_pool;
    uint32_t pool_to_native;
    uint32_t consumed_terminal;
    uint32_t raw_live_count;
    uint32_t raw_stranded_count;
    uint32_t raw_internal_live_count;
    uint32_t raw_internal_requested_bytes;
    uint32_t mspace_state;
    uint32_t terminal;
    uint32_t accounting_valid;
    uint32_t counter_saturated;
} isaac_vita_guest_heap_telemetry_snapshot;

int isaac_vita_guest_heap_telemetry_snapshot_get(
    isaac_vita_guest_heap_telemetry_snapshot *snapshot_out);
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
/* Emit one final bounded record.  The snapshot is captured under the heap
 * lock, while formatting and durable I/O happen only after unlocking. */
void isaac_vita_guest_heap_telemetry_log_final(void);
#endif

#ifdef ISAAC_VITA_HEAP_CENSUS
/* ISAAC_VITA_HEAP_CENSUS (ph120.mem): the heap-lock-protected half of the
 * per-window heap receipt.  One call takes the guest heap spin lock exactly
 * once and copies O(1) scalars: the live ledger count and its live requested
 * bytes (floor-lifetime totals), the overflow-mspace telemetry and the room
 * slab fast snapshot.  mallinfo(), the Lua arena and vitaGL are read by the
 * caller outside the lock (mallinfo walks newlib bins and takes newlib's own
 * lock; see the stagemem precedent in host_vita_heap.c).  Every field is
 * uint32_t so the phase-profile oracle compiles the reader without any Vita
 * header.  Fields a feature did not compile in stay zero and clear `valid`
 * only when their accounting is genuinely inconsistent.  heap_total_bytes is
 * the fixed newlib arena contract (81 MiB): the arena grows lazily, so
 * headroom is heap_total_bytes - uordblks and the largest-allocatable bound
 * is keepcost + (heap_total_bytes - arena), never fordblks alone. */
typedef struct isaac_vita_guest_heap_census_locked {
    uint32_t heap_total_bytes;
    uint32_t ledger_live_count;
    uint32_t ledger_live_requested_bytes;
    uint32_t overflow_live_count;
    uint32_t overflow_live_requested_bytes;
    uint32_t overflow_native_failures;
    uint32_t overflow_pool_failures;
    uint32_t overflow_capacity_bytes;
    uint32_t overflow_internal_requested_bytes;
    uint32_t slab_pages;
    uint32_t slab_live_slots;
    uint32_t slab_page_bytes;
    uint32_t ogg_slot_live;
    uint32_t terminal;
    uint32_t valid;
} isaac_vita_guest_heap_census_locked;

/* Returns 1 when the heap is initialised and every compiled-in accounting
 * source reported consistent (== out->valid); 0 otherwise.  Never logs,
 * never allocates, never touches newlib. */
int isaac_vita_guest_heap_census_window_locked_get(
    isaac_vita_guest_heap_census_locked *out);
#endif

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
/* Pin the one live guest allocation which wholly contains [range, range+size).
 * The returned non-zero token is never reused.  While it is live, exact-base
 * free/realloc of that allocation fail closed before entering newlib; unrelated
 * heap operations and ledger rehashes remain available.  Release is exact and
 * rejects zero, stale, duplicate and ABA-era tokens.
 *
 * This deliberately is a cold range query, not a replacement for exact-base
 * ownership at the imported free/realloc boundary.  It scans exact requested
 * extents recorded by malloc/calloc/realloc, never native allocator padding. */
uint32_t isaac_vita_guest_heap_lease_containing(
    const void *range, size_t size, uintptr_t *allocation_base_out);
int isaac_vita_guest_heap_lease_release(uint32_t token);
#if defined(ISAAC_VITA_PNG_NATIVE_UNFILTER) || \
    defined(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH) || \
    defined(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH) || \
    defined(ISAAC_VITA_WAV_BUFFERED_REWIND) || \
    defined(ISAAC_VITA_ARCHIVE_XOR_FASTPATH) || \
    defined(ISAAC_VITA_LIGHT_SURFACE_RASTER_416) || \
    (defined(ISAAC_VITA_RENDER_SURFACE_RASTER_432) && \
     ISAAC_VITA_RENDER_SURFACE_RASTER_432) || \
    defined(ISAAC_VITA_PNG_PREMULTIPLY_NATIVE) || \
    defined(ISAAC_VITA_ROOM_GRID_INIT_NATIVE) || \
    defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH) || \
    defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
/* Hot counterpart for frozen native seams whose private ABI supplies exact
 * candidate allocation bases.  Prove each with an O(1) ledger lookup rather
 * than scanning the 524288-entry range table.  The lease keeps free/realloc
 * from invalidating the native operation.  No token is installed unless the
 * complete requested range is owned by the supplied exact allocation base. */
uint32_t isaac_vita_guest_heap_lease_exact_range(
    const void *allocation_base, const void *range, size_t size);
#endif
#endif
#ifdef ISAAC_VITA_HEAP_TESTING
/* Deterministic state-machine reset for the stage-memory oracle.  It does not
 * mutate allocator ownership or telemetry. */
void isaac_vita_stage_memory_test_reset(void);
typedef struct isaac_vita_guest_heap_test_probe_stats {
    size_t lock_acquisitions;
    size_t lock_spins;
    size_t find_calls;
    size_t find_probes;
    size_t find_max_probes;
} isaac_vita_guest_heap_test_probe_stats;

void isaac_vita_guest_heap_test_probe_stats_reset(void);
int isaac_vita_guest_heap_test_probe_stats_snapshot(
    isaac_vita_guest_heap_test_probe_stats *stats_out);
size_t isaac_vita_guest_heap_test_capacity(void);
size_t isaac_vita_guest_heap_test_live_count(void);
size_t isaac_vita_guest_heap_test_realloc_moves(void);
size_t isaac_vita_guest_heap_test_next_rehash_bytes(void);
int isaac_vita_guest_heap_test_backshift_enabled(void);
/* Runs synthetic collision/wrap and deterministic model checks against the
 * same table deletion primitive used by production. */
int isaac_vita_guest_heap_test_backshift_oracle(void);
/* Test-only corruption injection: replace every empty slot with a synthetic
 * non-empty key, then restore it.  Used to prove bounded no-empty preflight
 * before a native allocator side effect.  It is unavailable in legacy mode. */
int isaac_vita_guest_heap_test_corrupt_no_empty(void);
int isaac_vita_guest_heap_test_restore_empty(void);
#ifdef ISAAC_VITA_HEAP_LEDGER_MEMBLOCK
typedef struct isaac_vita_guest_heap_test_storage_state {
    size_t capacity;
    size_t live_count;
    size_t tombstones;
    size_t usable_bytes;
    size_t block_bytes;
    int32_t uid;
    int32_t orphan_uid;
    int poisoned;
    uint64_t table_hash;
} isaac_vita_guest_heap_test_storage_state;

int isaac_vita_guest_heap_test_storage_snapshot(
    isaac_vita_guest_heap_test_storage_state *state);
/* Test-only process reset.  It requires no live allocations or leases and
 * releases both the authoritative and any fail-closed orphan memblock. */
int isaac_vita_guest_heap_test_storage_reset(void);
#endif
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
size_t isaac_vita_guest_heap_test_active_leases(void);
/* Exact coordinator seams for executable integration oracles.  They run the
 * same heap+slab floor transition under the real guest lock; production uses
 * only validated stage-memory events. */
int isaac_vita_guest_heap_test_floor_lifetime_bootstrap(uint32_t phase);
int isaac_vita_guest_heap_test_floor_lifetime_rollover(uint32_t phase);
int isaac_vita_guest_heap_test_floor_lifetime_phase_set(uint32_t phase);
#endif
int isaac_vita_guest_heap_test_bounded_probe(void);
/* Lets the integrated logger oracle prove that no formatting or I/O is ever
 * entered while the non-recursive heap lock is held. */
int isaac_vita_guest_heap_test_lock_held(void);
/* Deterministically exercise the moved-realloc ledger path.  The old native
 * block is returned through retired_pointer for raw test-only cleanup. */
void *isaac_vita_guest_heap_test_force_move(void *pointer, size_t size,
                                              void **retired_pointer);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
typedef enum isaac_vita_guest_heap_test_result {
    ISAAC_VITA_GUEST_HEAP_TEST_OK = 0,
    ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY = 1,
    ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN = 2,
    ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL = 3
} isaac_vita_guest_heap_test_result;

/* Pure retained-range math used by the production initializer. */
int isaac_vita_guest_heap_test_forbidden_ranges(
    uintptr_t link_end, size_t heap_bytes,
    uint32_t stack_floor, uint32_t stack_ceiling,
    isaac_vita_heap_overflow_forbidden_ranges *ranges_out);
/* Host oracle entry which substitutes explicit link/heap values for &_end and
 * _newlib_heap_size_user while executing the real eager setup transaction. */
int isaac_vita_guest_heap_test_init(
    uintptr_t link_end, size_t heap_bytes,
    uint32_t stack_floor, uint32_t stack_ceiling);
int isaac_vita_guest_heap_test_fixed_capacity(void);
void *isaac_vita_guest_heap_test_malloc_ex(
    size_t size, isaac_vita_guest_heap_test_result *result_out,
    const char **failure_reason_out);
void *isaac_vita_guest_heap_test_calloc_ex(
    size_t count, size_t size,
    isaac_vita_guest_heap_test_result *result_out);
void *isaac_vita_guest_heap_test_realloc_ex(
    void *pointer, size_t size, int *valid_owner,
    isaac_vita_guest_heap_test_result *result_out);
int isaac_vita_guest_heap_test_free_ex(
    void *pointer, isaac_vita_guest_heap_test_result *result_out);
/* Fail the next router-owned raw-pool free before entering the raw allocator.
 * This isolates the post-ledger-commit fail-closed accounting branch. */
void isaac_vita_guest_heap_test_fail_next_raw_free(void);
#endif
#if defined(ISAAC_VITA_ROOM_ENTRY_SLAB) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
typedef struct isaac_vita_guest_heap_test_slab_move_floor_receipt {
    uint32_t attempts;
    uint32_t replacement_original_token;
    uint32_t inherited_token;
    uint32_t replacement_final_token;
    uint32_t retag_succeeded;
    uint32_t commit_succeeded;
    uint32_t rollback_attempted;
    uint32_t rollback_succeeded;
} isaac_vita_guest_heap_test_slab_move_floor_receipt;

void isaac_vita_guest_heap_test_room_fail_next_commit(void);
void isaac_vita_guest_heap_test_room_corrupt_nonfull(void);
int isaac_vita_guest_heap_test_room_snapshot(
    isaac_vita_room_entry_slab_snapshot *snapshot_out);
int isaac_vita_guest_heap_test_room_floor_lifetime_snapshot(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot_out);
void isaac_vita_guest_heap_test_slab_move_floor_receipt_reset(void);
int isaac_vita_guest_heap_test_slab_move_floor_receipt_snapshot(
    isaac_vita_guest_heap_test_slab_move_floor_receipt *receipt_out);
#endif
#endif

int isaac_vita_heap_import(CPU *__restrict c, const char *name);
int isaac_vita_heap_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count);

#endif
