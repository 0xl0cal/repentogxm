/* One allocator owns the whole guest UCRT heap family on Vita.  Vita is a
 * 32-bit process and the recompilation uses identity guest addresses, so a
 * newlib allocation is directly usable by translated ld/st helpers. */
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(ISAAC_VITA_STAGE_MEMORY_DIAGNOSTICS) || \
    defined(ISAAC_VITA_STAGE_MEMORY_ORACLE)
#define VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED 1
#endif

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
#include <malloc.h>
#endif

#include "host_vita_heap.h"
#include "host_vita_import_id.h"
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
#include "guest_pe.h"
#include "host_vita_heap_overflow_mspace.h"
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
#include "host_vita_room_entry_slab.h"
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
#include "host_vita_room_entry_external.h"
#endif
#ifdef ISAAC_VITA_HEAP_LEDGER_MEMBLOCK
#include "host_vita_heap_ledger_memblock.h"
#endif
#ifdef ISAAC_VITA_OGG_EMERGENCY
#include "host_vita_ogg_emergency.h"
#endif
#ifdef ISAAC_VITA_ANM2_SCRATCH
#include "host_vita_anm2_scratch.h"
#endif
#ifdef ISAAC_VITA_TEXEL_SCRATCH
#include "host_vita_texel_scratch.h"
#endif
#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXEL_SCRATCH)
#include "kage_vita_texture_memory.h"
#endif

#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE) || \
    defined(VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED)
void isaac_vita_log(const char *format, ...);
#endif

#ifdef ISAAC_VITA_STAGE_MEMORY_ORACLE
struct mallinfo isaac_vita_stage_memory_oracle_mallinfo(void);
#define VITA_STAGE_MEMORY_MALLINFO() \
    isaac_vita_stage_memory_oracle_mallinfo()
#elif defined(VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED)
#define VITA_STAGE_MEMORY_MALLINFO() mallinfo()
#endif

#if defined(ISAAC_VITA_ANM2_SCRATCH) && \
    defined(ISAAC_VITA_TEXEL_SCRATCH)
_Static_assert(ISAAC_VITA_ANM2_LINK_END == KAGE_VITA_TEXEL_LINK_END,
               "ANM2/texel scratch link-end contracts diverged");
_Static_assert(ISAAC_VITA_ANM2_HEAP_RETAINED_END ==
                   KAGE_VITA_TEXEL_HEAP_RETAINED_END,
               "ANM2/texel scratch heap ranges diverged");
_Static_assert(ISAAC_VITA_ANM2_GUEST_IMAGE_BASE ==
                   KAGE_VITA_TEXEL_GUEST_IMAGE_BASE,
               "ANM2/texel scratch image bases diverged");
_Static_assert(ISAAC_VITA_ANM2_GUEST_IMAGE_END ==
                   KAGE_VITA_TEXEL_GUEST_IMAGE_END,
               "ANM2/texel scratch image ends diverged");
#endif

#if defined(ISAAC_VITA_ROOM_ENTRY_SLAB) && \
    !defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
#error RoomConfig Entry slab requires the bounded overflow mspace
#endif
#if defined(ISAAC_VITA_ROOM_ENTRY_HYBRID) && \
    !defined(ISAAC_VITA_ROOM_ENTRY_SLAB)
#error RoomConfig Entry hybrid backing requires the RoomConfig Entry slab
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
_Static_assert((uint32_t)ISAAC_VITA_FLOOR_LIFETIME_PHASE_PRE_FLOOR ==
                   ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_UNSCOPED &&
                   (uint32_t)ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT ==
                   ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT &&
                   (uint32_t)ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD ==
                   ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_ROOM_LOAD &&
                   (uint32_t)ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY ==
                   ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY,
               "heap/slab floor-lifetime phases drifted");
_Static_assert(3U == ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_PHASE_MASK &&
                   4U ==
                   ISAAC_VITA_ROOM_ENTRY_FLOOR_TOKEN_CURRENT,
               "heap/slab floor-lifetime token bits drifted");
#endif

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
#if !defined(ISAAC_VITA_HEAP_LEDGER_MEMBLOCK)
#error Overflow mspace requires the external USER_RW heap ledger
#endif
#if !defined(ISAAC_VITA_HEAP_LEDGER_BACKSHIFT)
#error Overflow mspace requires tombstone-free backshift deletion
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error Overflow mspace requires requested-size range ledger entries
#endif

#define VITA_HEAP_LEDGER_FIXED_CAPACITY \
    ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY
#define VITA_HEAP_LEDGER_FIXED_ENTRY_BYTES \
    ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_BYTES
#define VITA_HEAP_LEDGER_FIXED_ENTRY_TABLE_BYTES \
    ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_TABLE_BYTES
#define VITA_HEAP_FLOOR_FIXED_PHASE_BYTES \
    ISAAC_VITA_GUEST_HEAP_FLOOR_PHASE_BYTES
#define VITA_HEAP_FLOOR_FIXED_CURRENT_BYTES \
    ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES
#define VITA_HEAP_LEDGER_FIXED_USABLE_BYTES \
    ISAAC_VITA_GUEST_HEAP_LEDGER_USABLE_BYTES
#define VITA_HEAP_LEDGER_FIXED_REQUEST_BYTES \
    ISAAC_VITA_GUEST_HEAP_LEDGER_REQUEST_BYTES
#define VITA_HEAP_LEDGER_FIXED_RETAINED_BYTES \
    ISAAC_VITA_GUEST_HEAP_LEDGER_RETAINED_BYTES
#define VITA_HEAP_OVERFLOW_TOTAL_REQUEST_BYTES \
    ISAAC_VITA_GUEST_HEAP_TOTAL_REQUEST_BYTES
#define VITA_HEAP_OVERFLOW_TOTAL_RETAINED_BYTES \
    ISAAC_VITA_GUEST_HEAP_TOTAL_RETAINED_BYTES
#define VITA_HEAP_REQUIRED_NEWLIB_BYTES       0x05100000U
#define VITA_HEAP_PAGE_BYTES                  0x00001000U

_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR > 0U &&
                   ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR > 0U &&
                   ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR <
                       ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR,
               "fixed Vita heap-ledger load ratio is invalid");
_Static_assert(VITA_HEAP_LEDGER_FIXED_CAPACITY %
                       ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR ==
                   0U,
               "fixed Vita heap-ledger capacity does not divide by ratio");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT == 393216U &&
                   ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT <
                       VITA_HEAP_LEDGER_FIXED_CAPACITY,
               "fixed Vita heap-ledger 3/4 live ceiling drifted");
_Static_assert(VITA_HEAP_LEDGER_FIXED_CAPACITY *
                   VITA_HEAP_LEDGER_FIXED_ENTRY_BYTES ==
                   VITA_HEAP_LEDGER_FIXED_ENTRY_TABLE_BYTES,
               "fixed Vita heap-ledger entry-table budget drifted");
_Static_assert(VITA_HEAP_LEDGER_FIXED_CAPACITY ==
                   VITA_HEAP_FLOOR_FIXED_PHASE_BYTES &&
                   VITA_HEAP_LEDGER_FIXED_CAPACITY / 8U ==
                       VITA_HEAP_FLOOR_FIXED_CURRENT_BYTES,
               "fixed Vita floor-lifetime sidecar budget drifted");
_Static_assert(VITA_HEAP_LEDGER_FIXED_USABLE_BYTES ==
                   VITA_HEAP_LEDGER_FIXED_ENTRY_TABLE_BYTES +
                       VITA_HEAP_FLOOR_FIXED_PHASE_BYTES +
                       VITA_HEAP_FLOOR_FIXED_CURRENT_BYTES,
               "fixed Vita combined heap-ledger budget drifted");
_Static_assert(VITA_HEAP_LEDGER_FIXED_REQUEST_BYTES ==
                   VITA_HEAP_LEDGER_FIXED_USABLE_BYTES +
                       VITA_HEAP_PAGE_BYTES,
               "fixed Vita heap-ledger request budget drifted");
_Static_assert(VITA_HEAP_LEDGER_FIXED_RETAINED_BYTES ==
                   VITA_HEAP_LEDGER_FIXED_REQUEST_BYTES +
                       VITA_HEAP_PAGE_BYTES,
               "fixed Vita heap-ledger retention budget drifted");
_Static_assert(VITA_HEAP_OVERFLOW_TOTAL_REQUEST_BYTES ==
                   VITA_HEAP_LEDGER_FIXED_REQUEST_BYTES +
                       ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES,
               "combined overflow USER_RW request budget drifted");
_Static_assert(VITA_HEAP_OVERFLOW_TOTAL_RETAINED_BYTES ==
                   VITA_HEAP_LEDGER_FIXED_RETAINED_BYTES +
                       ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES,
               "combined overflow USER_RW retained budget drifted");
_Static_assert(GUEST_PE_VITA_TARGET_BASE == 0x98000000U &&
                   GUEST_PE_EXPECTED_IMAGE_SIZE == 0x0085f000U,
               "overflow ranges require the frozen Vita PE mapping");
_Static_assert(GUEST_IMAGE_BASE == GUEST_PE_VITA_TARGET_BASE,
               "compiled guest base differs from the frozen Vita PE base");
#if defined(__vita__)
_Static_assert(sizeof(uintptr_t) == 4U && sizeof(size_t) == 4U,
               "Vita overflow router requires a 32-bit address space");
#endif
#endif

typedef void (*vita_heap_import_fn)(CPU *__restrict);

typedef struct vita_heap_import_entry {
    const char *name;
    vita_heap_import_fn fn;
} vita_heap_import_entry;

static int s_new_mode;

/* Exact-base ownership ledger.  Guest pointers are untrusted at the import
 * boundary: handing an interior/foreign/double-freed value to newlib would
 * turn a guest bug into a native Vita crash.  The ledger's own table is an
 * internal native allocation and is deliberately never guest-visible. */
#define VITA_HEAP_LEDGER_INITIAL_CAPACITY 64U
#define VITA_HEAP_LEDGER_TOMBSTONE ((uintptr_t)1U)

#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
static uintptr_t *s_ledger;
#else
typedef struct vita_heap_ledger_entry {
    uintptr_t base;
    size_t requested_size;
} vita_heap_ledger_entry;

#if defined(__vita__) && defined(ISAAC_VITA_HEAP_LEDGER_MEMBLOCK)
_Static_assert(sizeof(vita_heap_ledger_entry) == 8U,
               "Vita range ledger entry must remain exactly eight bytes");
#endif

static vita_heap_ledger_entry *s_ledger;
#endif
static size_t s_ledger_capacity;
static size_t s_ledger_count;
static size_t s_ledger_tombstones;
static int s_ledger_fixed_capacity;
static atomic_flag s_ledger_lock = ATOMIC_FLAG_INIT;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
/* One byte retains the allocation's birth phase after it becomes prior; the
 * separate current bitset is the only sidecar cleared at a floor rollover. */
typedef struct vita_heap_ledger_layout {
    size_t entry_bytes;
    size_t phase_offset;
    size_t phase_bytes;
    size_t current_offset;
    size_t current_bytes;
    size_t combined_bytes;
} vita_heap_ledger_layout;

typedef struct vita_heap_floor_lifetime_state {
    uint32_t epoch;
    uint32_t bootstrap_count;
    uint32_t rollover_count;
    uint32_t phase;
    uint32_t total_count;
    uint32_t unscoped_count;
    uint32_t current_count;
    uint32_t total_requested_bytes;
    uint32_t unscoped_requested_bytes;
    uint32_t current_requested_bytes;
    uint32_t phase_count[3];
    uint32_t phase_requested_bytes[3];
    uint32_t protocol_valid;
    uint32_t accounting_valid;
    uint32_t counter_saturated;
} vita_heap_floor_lifetime_state;

#define VITA_HEAP_FLOOR_LIFETIME_STATE_INITIALIZER { \
    0U, 0U, 0U, ISAAC_VITA_FLOOR_LIFETIME_PHASE_PRE_FLOOR, \
    0U, 0U, 0U, 0U, 0U, 0U, { 0U, 0U, 0U }, \
    { 0U, 0U, 0U }, 1U, 1U, 0U \
}

static uint8_t *s_ledger_floor_phase;
static uint32_t *s_ledger_floor_current;
static vita_heap_floor_lifetime_state s_floor_lifetime =
    VITA_HEAP_FLOOR_LIFETIME_STATE_INITIALIZER;
#endif
#ifdef ISAAC_VITA_HEAP_TESTING
static int s_heap_test_lock_held;
static isaac_vita_guest_heap_test_probe_stats s_heap_test_probe_stats;
#endif

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
typedef enum vita_heap_router_result {
    VITA_HEAP_ROUTER_OK = 0,
    VITA_HEAP_ROUTER_OUT_OF_MEMORY = 1,
    VITA_HEAP_ROUTER_FOREIGN = 2,
    VITA_HEAP_ROUTER_TERMINAL = 3
} vita_heap_router_result;

enum vita_heap_init_state {
    VITA_HEAP_INIT_UNINITIALIZED = 0,
    VITA_HEAP_INIT_READY = 1,
    VITA_HEAP_INIT_FAILED = 2
};

static int s_heap_init_state;
static int s_heap_terminal;

enum vita_heap_telemetry_edge {
    VITA_HEAP_TELEMETRY_EDGE_FIRST_USE = 1U << 0,
    VITA_HEAP_TELEMETRY_EDGE_FIRST_FREE = 1U << 1,
    VITA_HEAP_TELEMETRY_EDGE_FIRST_REALLOC = 1U << 2,
    VITA_HEAP_TELEMETRY_EDGE_NATIVE_TO_POOL = 1U << 3,
    VITA_HEAP_TELEMETRY_EDGE_POOL_TO_NATIVE = 1U << 4,
    VITA_HEAP_TELEMETRY_EDGE_POOL_FAILURE = 1U << 5,
    VITA_HEAP_TELEMETRY_EDGE_TERMINAL = 1U << 6,
    VITA_HEAP_TELEMETRY_EDGE_FINAL = 1U << 7
};

enum vita_heap_telemetry_operation {
    VITA_HEAP_TELEMETRY_OP_NONE = 0,
    VITA_HEAP_TELEMETRY_OP_MALLOC = 1,
    VITA_HEAP_TELEMETRY_OP_CALLOC = 2,
    VITA_HEAP_TELEMETRY_OP_FREE = 3,
    VITA_HEAP_TELEMETRY_OP_REALLOC = 4
};

typedef struct vita_heap_telemetry_state {
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
    uint32_t sequence;
    uint32_t claimed_edges;
    uint32_t active;
    uint32_t accounting_valid;
    uint32_t counter_saturated;
} vita_heap_telemetry_state;

typedef struct vita_heap_telemetry_event {
    uint32_t edges;
    uint32_t operation;
    uint32_t request;
    isaac_vita_guest_heap_telemetry_snapshot snapshot;
} vita_heap_telemetry_event;

static vita_heap_telemetry_state s_heap_telemetry;
#else
typedef enum vita_heap_router_result {
    VITA_HEAP_ROUTER_OK = 0,
    VITA_HEAP_ROUTER_OUT_OF_MEMORY = 1,
    VITA_HEAP_ROUTER_FOREIGN = 2,
    VITA_HEAP_ROUTER_TERMINAL = 3
} vita_heap_router_result;
enum vita_heap_telemetry_operation {
    VITA_HEAP_TELEMETRY_OP_NONE = 0,
    VITA_HEAP_TELEMETRY_OP_MALLOC = 1,
    VITA_HEAP_TELEMETRY_OP_CALLOC = 2,
    VITA_HEAP_TELEMETRY_OP_FREE = 3,
    VITA_HEAP_TELEMETRY_OP_REALLOC = 4
};
typedef struct vita_heap_telemetry_event {
    unsigned unused;
} vita_heap_telemetry_event;
#endif

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
static void vita_heap_room_slab_unlock(
    isaac_vita_room_entry_slab_result slab_result,
    isaac_vita_room_entry_slab_event *slab_event,
    vita_heap_telemetry_event *heap_event);
#endif

/* Cold, bounded lifetime pins for native helpers which must inspect an
 * interior guest range before calling translated destructors.  Tokens are
 * monotonically consumed and never wrapped/reused, so a stale release cannot
 * clear a later lease even if newlib eventually recycles the same base. */
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
#define VITA_HEAP_LEASE_CAPACITY 8U
typedef struct vita_heap_lease_entry {
    uintptr_t base;
    uint32_t token;
} vita_heap_lease_entry;

static vita_heap_lease_entry s_heap_leases[VITA_HEAP_LEASE_CAPACITY];
static size_t s_heap_lease_count;
static uint32_t s_heap_next_lease_token = 1U;
#endif
#ifdef ISAAC_VITA_HEAP_TESTING
static size_t s_realloc_moves;
#if defined(ISAAC_VITA_ROOM_ENTRY_SLAB) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
static isaac_vita_guest_heap_test_slab_move_floor_receipt
    s_heap_test_slab_move_floor_receipt;
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
static unsigned s_heap_test_fail_raw_free;
#endif
#endif

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
static int vita_heap_ledger_combined_layout(
    size_t capacity, vita_heap_ledger_layout *layout)
{
    size_t entry_bytes;
    size_t current_bytes;

    if (layout)
        memset(layout, 0, sizeof *layout);
    if (!layout || capacity < 32U || (capacity & (capacity - 1U)) != 0U ||
        (capacity & 31U) != 0U ||
        capacity > SIZE_MAX / sizeof(vita_heap_ledger_entry))
        return 0;
    entry_bytes = capacity * sizeof(vita_heap_ledger_entry);
    current_bytes = capacity / 8U;
    if (entry_bytes > SIZE_MAX - capacity ||
        entry_bytes + capacity > SIZE_MAX - current_bytes)
        return 0;
    layout->entry_bytes = entry_bytes;
    layout->phase_offset = entry_bytes;
    layout->phase_bytes = capacity;
    layout->current_offset = entry_bytes + capacity;
    layout->current_bytes = current_bytes;
    layout->combined_bytes = layout->current_offset + current_bytes;
    if ((layout->current_offset & (_Alignof(uint32_t) - 1U)) != 0U)
        return 0;
    return 1;
}

static void vita_heap_ledger_sidecars_from_base(
    void *base, const vita_heap_ledger_layout *layout,
    uint8_t **phase_out, uint32_t **current_out)
{
    unsigned char *bytes = (unsigned char *)base;

    *phase_out = bytes ? bytes + layout->phase_offset : NULL;
    *current_out = bytes ?
        (uint32_t *)(void *)(bytes + layout->current_offset) : NULL;
}

static int vita_heap_floor_current_bit_get(
    const uint32_t *bits, size_t slot)
{
    return bits && ((bits[slot >> 5U] >> (slot & 31U)) & 1U) != 0U;
}

static void vita_heap_floor_current_bit_set(
    uint32_t *bits, size_t slot, int current)
{
    uint32_t mask = UINT32_C(1) << (slot & 31U);

    if (current)
        bits[slot >> 5U] |= mask;
    else
        bits[slot >> 5U] &= ~mask;
}
#endif

#ifdef ISAAC_VITA_HEAP_LEDGER_MEMBLOCK
static isaac_vita_heap_ledger_storage s_ledger_storage = {
    .uid = ISAAC_VITA_HEAP_LEDGER_INVALID_UID
};
static isaac_vita_heap_ledger_storage s_ledger_orphan = {
    .uid = ISAAC_VITA_HEAP_LEDGER_INVALID_UID
};
static int s_ledger_storage_poisoned;

typedef struct vita_heap_ledger_candidate {
    isaac_vita_heap_ledger_storage storage;
} vita_heap_ledger_candidate;

static void vita_heap_ledger_storage_clear(
    isaac_vita_heap_ledger_storage *storage)
{
    storage->base = NULL;
    storage->uid = ISAAC_VITA_HEAP_LEDGER_INVALID_UID;
    storage->usable_bytes = 0U;
    storage->block_bytes = 0U;
}

static void vita_heap_ledger_poison_with_orphan(
    const isaac_vita_heap_ledger_storage *orphan)
{
    /* Only one acquisition can be in flight under s_ledger_lock.  Once an
     * orphan exists, every later rehash fails closed without allocating. */
    if (s_ledger_orphan.uid < 0)
        s_ledger_orphan = *orphan;
    s_ledger_storage_poisoned = 1;
}

static int vita_heap_ledger_candidate_prepare(
    size_t bytes, vita_heap_ledger_candidate *candidate)
{
    int result;

    vita_heap_ledger_storage_clear(&candidate->storage);
    if (s_ledger_storage_poisoned)
        return 0;
    result = isaac_vita_heap_ledger_memblock_acquire(
        bytes, &candidate->storage);
    if (result == ISAAC_VITA_HEAP_LEDGER_STORAGE_ORPHANED) {
        vita_heap_ledger_poison_with_orphan(&candidate->storage);
        vita_heap_ledger_storage_clear(&candidate->storage);
        return 0;
    }
    return result == ISAAC_VITA_HEAP_LEDGER_STORAGE_READY;
}

static int vita_heap_ledger_candidate_rollback(
    vita_heap_ledger_candidate *candidate)
{
    int result;

    if (candidate->storage.uid < 0)
        return 1;
    result = isaac_vita_heap_ledger_memblock_release(&candidate->storage);
    if (result < 0) {
        vita_heap_ledger_poison_with_orphan(&candidate->storage);
        vita_heap_ledger_storage_clear(&candidate->storage);
        return 0;
    }
    vita_heap_ledger_storage_clear(&candidate->storage);
    return 1;
}

static int vita_heap_ledger_candidate_commit(
    vita_heap_ledger_candidate *candidate)
{
    if (s_ledger_storage.uid >= 0 &&
        isaac_vita_heap_ledger_memblock_release(&s_ledger_storage) < 0) {
        /* Old authority stays byte-exact.  If rolling back the unpublished
         * candidate also fails, retain its UID and poison future rehashes. */
        (void)vita_heap_ledger_candidate_rollback(candidate);
        return 0;
    }
    s_ledger_storage = candidate->storage;
    vita_heap_ledger_storage_clear(&candidate->storage);
    return 1;
}

#define VITA_HEAP_LEDGER_CANDIDATE_BASE(candidate) \
    ((candidate).storage.base)
#else
typedef struct vita_heap_ledger_candidate {
    void *base;
} vita_heap_ledger_candidate;

static int vita_heap_ledger_candidate_prepare(
    size_t bytes, vita_heap_ledger_candidate *candidate)
{
    candidate->base = calloc(1U, bytes);
    return candidate->base != NULL;
}

static int vita_heap_ledger_candidate_rollback(
    vita_heap_ledger_candidate *candidate)
{
    free(candidate->base);
    candidate->base = NULL;
    return 1;
}

static int vita_heap_ledger_candidate_commit(
    vita_heap_ledger_candidate *candidate)
{
    free(s_ledger);
    candidate->base = NULL;
    return 1;
}

#define VITA_HEAP_LEDGER_CANDIDATE_BASE(candidate) ((candidate).base)
#endif

#ifdef ISAAC_VITA_HEAP_TESTING
static void vita_heap_test_counter_add(size_t *counter, size_t amount)
{
    if (*counter > SIZE_MAX - amount)
        *counter = SIZE_MAX;
    else
        *counter += amount;
}

static void vita_heap_test_record_find(size_t probes)
{
    vita_heap_test_counter_add(&s_heap_test_probe_stats.find_calls, 1U);
    vita_heap_test_counter_add(&s_heap_test_probe_stats.find_probes, probes);
    if (probes > s_heap_test_probe_stats.find_max_probes)
        s_heap_test_probe_stats.find_max_probes = probes;
}
#define VITA_HEAP_TEST_RECORD_FIND(probes) \
    vita_heap_test_record_find((probes))
#else
#define VITA_HEAP_TEST_RECORD_FIND(probes) ((void)(probes))
#endif

static void vita_heap_lock(void)
{
#ifdef ISAAC_VITA_HEAP_TESTING
    size_t spins = 0U;
#endif

    while (atomic_flag_test_and_set_explicit(
               &s_ledger_lock, memory_order_acquire)) {
        /* Guest heap operations are short and may run before thread services
         * are initialized, so this cannot depend on a kernel wait primitive. */
#ifdef ISAAC_VITA_HEAP_TESTING
        if (spins != SIZE_MAX)
            ++spins;
#endif
    }
#ifdef ISAAC_VITA_HEAP_TESTING
    vita_heap_test_counter_add(
        &s_heap_test_probe_stats.lock_acquisitions, 1U);
    vita_heap_test_counter_add(&s_heap_test_probe_stats.lock_spins, spins);
    s_heap_test_lock_held = 1;
#endif
}

static void vita_heap_unlock(void)
{
#ifdef ISAAC_VITA_HEAP_TESTING
    s_heap_test_lock_held = 0;
#endif
    atomic_flag_clear_explicit(&s_ledger_lock, memory_order_release);
}

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
enum {
    VITA_HEAP_FLOOR_TOKEN_PHASE_MASK = 3U,
    VITA_HEAP_FLOOR_TOKEN_CURRENT = 4U
};

static void vita_heap_floor_lifetime_saturate_locked(void)
{
    s_floor_lifetime.accounting_valid = 0U;
    s_floor_lifetime.counter_saturated = 1U;
}

static int vita_heap_floor_lifetime_size_u32_locked(
    size_t size, uint32_t *size_out)
{
#if SIZE_MAX > UINT32_MAX
    if (size > UINT32_MAX) {
        *size_out = UINT32_MAX;
        vita_heap_floor_lifetime_saturate_locked();
        return 0;
    }
#endif
    *size_out = (uint32_t)size;
    return 1;
}

static int vita_heap_floor_counter_add_locked(
    uint32_t *counter, uint32_t amount)
{
    if (s_floor_lifetime.counter_saturated)
        return 0;
    if (*counter > UINT32_MAX - amount) {
        *counter = UINT32_MAX;
        vita_heap_floor_lifetime_saturate_locked();
        return 0;
    }
    *counter += amount;
    return 1;
}

static int vita_heap_floor_counter_sub_locked(
    uint32_t *counter, uint32_t amount)
{
    if (s_floor_lifetime.counter_saturated)
        return 0;
    if (*counter < amount) {
        s_floor_lifetime.accounting_valid = 0U;
        return 0;
    }
    *counter -= amount;
    return 1;
}

static uint32_t vita_heap_floor_lifetime_new_token_locked(void)
{
    uint32_t phase = s_floor_lifetime.phase;

    if (!s_floor_lifetime.protocol_valid || !s_floor_lifetime.epoch ||
        phase < ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY)
        return 0U;
    return phase | VITA_HEAP_FLOOR_TOKEN_CURRENT;
}

static uint32_t vita_heap_floor_lifetime_slot_token(
    size_t slot, const uint8_t *phases, const uint32_t *current_bits)
{
    uint32_t phase = phases ? phases[slot] : 0U;

    if (phase < ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY)
        return 0U;
    return phase | (vita_heap_floor_current_bit_get(current_bits, slot) ?
        VITA_HEAP_FLOOR_TOKEN_CURRENT : 0U);
}

static void vita_heap_floor_lifetime_slot_store(
    size_t slot, uint32_t token, uint8_t *phases, uint32_t *current_bits)
{
    uint32_t phase = token & VITA_HEAP_FLOOR_TOKEN_PHASE_MASK;
    int current = (token & VITA_HEAP_FLOOR_TOKEN_CURRENT) != 0U;

    if (!phase)
        current = 0;
    phases[slot] = (uint8_t)phase;
    vita_heap_floor_current_bit_set(current_bits, slot, current);
}

static void vita_heap_floor_lifetime_account_add_locked(
    size_t requested_size, uint32_t token)
{
    uint32_t bytes;
    uint32_t phase = token & VITA_HEAP_FLOOR_TOKEN_PHASE_MASK;
    int current = (token & VITA_HEAP_FLOOR_TOKEN_CURRENT) != 0U;

    if (!vita_heap_floor_lifetime_size_u32_locked(requested_size, &bytes) ||
        s_floor_lifetime.counter_saturated)
        return;
    if (!vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.total_count, 1U) ||
        !vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.total_requested_bytes, bytes))
        return;
    if (!phase) {
        (void)vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.unscoped_count, 1U);
        (void)vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.unscoped_requested_bytes, bytes);
        return;
    }
    if (phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY) {
        s_floor_lifetime.accounting_valid = 0U;
        return;
    }
    if (current) {
        uint32_t index = phase - 1U;

        (void)vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.current_count, 1U);
        (void)vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.current_requested_bytes, bytes);
        (void)vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.phase_count[index], 1U);
        (void)vita_heap_floor_counter_add_locked(
            &s_floor_lifetime.phase_requested_bytes[index], bytes);
    }
}

static void vita_heap_floor_lifetime_account_remove_locked(
    size_t requested_size, uint32_t token)
{
    uint32_t bytes;
    uint32_t phase = token & VITA_HEAP_FLOOR_TOKEN_PHASE_MASK;
    int current = (token & VITA_HEAP_FLOOR_TOKEN_CURRENT) != 0U;

    if (!vita_heap_floor_lifetime_size_u32_locked(requested_size, &bytes) ||
        s_floor_lifetime.counter_saturated)
        return;
    if (!vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.total_count, 1U) ||
        !vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.total_requested_bytes, bytes))
        return;
    if (!phase) {
        (void)vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.unscoped_count, 1U);
        (void)vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.unscoped_requested_bytes, bytes);
        return;
    }
    if (phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY) {
        s_floor_lifetime.accounting_valid = 0U;
        return;
    }
    if (current) {
        uint32_t index = phase - 1U;

        (void)vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.current_count, 1U);
        (void)vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.current_requested_bytes, bytes);
        (void)vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.phase_count[index], 1U);
        (void)vita_heap_floor_counter_sub_locked(
            &s_floor_lifetime.phase_requested_bytes[index], bytes);
    }
}

static void vita_heap_floor_lifetime_slot_insert_locked(
    size_t slot, size_t requested_size, uint32_t token)
{
    vita_heap_floor_lifetime_slot_store(
        slot, token, s_ledger_floor_phase, s_ledger_floor_current);
    vita_heap_floor_lifetime_account_add_locked(requested_size, token);
}

static uint32_t vita_heap_floor_lifetime_slot_remove_locked(
    size_t slot, size_t requested_size)
{
    uint32_t token = vita_heap_floor_lifetime_slot_token(
        slot, s_ledger_floor_phase, s_ledger_floor_current);

    vita_heap_floor_lifetime_account_remove_locked(requested_size, token);
    return token;
}

static void vita_heap_floor_lifetime_requested_size_replace_locked(
    size_t slot, size_t old_size, size_t new_size)
{
    uint32_t token = vita_heap_floor_lifetime_slot_token(
        slot, s_ledger_floor_phase, s_ledger_floor_current);

    vita_heap_floor_lifetime_account_remove_locked(old_size, token);
    vita_heap_floor_lifetime_account_add_locked(new_size, token);
}

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
static int vita_heap_ledger_lookup(uintptr_t key, size_t *slot_out);
static int vita_heap_floor_lifetime_retag_locked(
    uintptr_t key, uint32_t replacement_token, uint32_t *old_token_out)
{
    size_t slot;
    size_t requested_size;
    uint32_t old_token;
    uint32_t phase = replacement_token &
        VITA_HEAP_FLOOR_TOKEN_PHASE_MASK;

    if (old_token_out)
        *old_token_out = 0U;
    if ((replacement_token &
            ~(VITA_HEAP_FLOOR_TOKEN_PHASE_MASK |
              VITA_HEAP_FLOOR_TOKEN_CURRENT)) != 0U ||
        (!phase &&
         (replacement_token & VITA_HEAP_FLOOR_TOKEN_CURRENT)) ||
        phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY ||
        !vita_heap_ledger_lookup(key, &slot))
        return 0;
    requested_size = s_ledger[slot].requested_size;
    old_token = vita_heap_floor_lifetime_slot_token(
        slot, s_ledger_floor_phase, s_ledger_floor_current);
    if (old_token_out)
        *old_token_out = old_token;
    if (old_token == replacement_token)
        return 1;
    vita_heap_floor_lifetime_account_remove_locked(
        requested_size, old_token);
    vita_heap_floor_lifetime_slot_store(
        slot, replacement_token,
        s_ledger_floor_phase, s_ledger_floor_current);
    vita_heap_floor_lifetime_account_add_locked(
        requested_size, replacement_token);
    return 1;
}
#endif

#if defined(__vita__) || defined(ISAAC_VITA_STAGE_MEMORY_ORACLE) || \
    defined(ISAAC_VITA_HEAP_TESTING)
static void vita_heap_floor_lifetime_invalidate_locked(void)
{
    s_floor_lifetime.protocol_valid = 0U;
    s_floor_lifetime.phase =
        ISAAC_VITA_FLOOR_LIFETIME_PHASE_PRE_FLOOR;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    /* Phase zero is rejected by the slab coordinator and durably latches its
     * diagnostic state invalid without changing allocator behaviour. */
    (void)isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
        ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_UNSCOPED);
#endif
}

static int vita_heap_floor_lifetime_phase_set_locked(uint32_t phase)
{
    if (!s_floor_lifetime.protocol_valid || !s_floor_lifetime.epoch ||
        phase < ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY) {
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    if (!isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(phase)) {
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#endif
    s_floor_lifetime.phase = phase;
    return 1;
}

static int vita_heap_floor_lifetime_bootstrap_locked(uint32_t phase)
{
    if (!s_floor_lifetime.protocol_valid || s_floor_lifetime.epoch ||
        s_floor_lifetime.bootstrap_count ||
        phase < ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY) {
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    if (!isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(phase)) {
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#endif
    s_floor_lifetime.epoch = 1U;
    s_floor_lifetime.bootstrap_count = 1U;
    s_floor_lifetime.phase = phase;
    return 1;
}

static int vita_heap_floor_lifetime_rollover_locked(uint32_t phase)
{
    vita_heap_ledger_layout layout;

    if (!s_floor_lifetime.protocol_valid || !s_floor_lifetime.epoch ||
        s_floor_lifetime.epoch == UINT32_MAX ||
        s_floor_lifetime.rollover_count == UINT32_MAX ||
        phase < ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT ||
        phase > ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY ||
        (s_ledger_capacity &&
         (!s_ledger_floor_current ||
          !vita_heap_ledger_combined_layout(
              s_ledger_capacity, &layout)))) {
        if (s_floor_lifetime.epoch == UINT32_MAX ||
            s_floor_lifetime.rollover_count == UINT32_MAX)
            vita_heap_floor_lifetime_saturate_locked();
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    if (!isaac_vita_room_entry_slab_floor_lifetime_rollover_locked()) {
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#endif
    if (s_ledger_capacity)
        memset(s_ledger_floor_current, 0, layout.current_bytes);
    s_floor_lifetime.current_count = 0U;
    s_floor_lifetime.current_requested_bytes = 0U;
    memset(s_floor_lifetime.phase_count, 0,
           sizeof s_floor_lifetime.phase_count);
    memset(s_floor_lifetime.phase_requested_bytes, 0,
           sizeof s_floor_lifetime.phase_requested_bytes);
    ++s_floor_lifetime.epoch;
    ++s_floor_lifetime.rollover_count;
    s_floor_lifetime.phase = phase;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    if (!isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(phase)) {
        vita_heap_floor_lifetime_invalidate_locked();
        return 0;
    }
#endif
    return 1;
}
#endif

static int vita_heap_floor_lifetime_accounting_valid_locked(void)
{
    uint32_t scoped_count;
    uint32_t scoped_bytes;
    uint32_t phase_count;
    uint32_t phase_bytes;

    if (!s_floor_lifetime.accounting_valid ||
        s_floor_lifetime.counter_saturated ||
#if SIZE_MAX > UINT32_MAX
        s_ledger_count > UINT32_MAX ||
#endif
        s_floor_lifetime.total_count != (uint32_t)s_ledger_count ||
        s_floor_lifetime.total_count < s_floor_lifetime.unscoped_count ||
        s_floor_lifetime.total_requested_bytes <
            s_floor_lifetime.unscoped_requested_bytes)
        return 0;
    scoped_count = s_floor_lifetime.total_count -
        s_floor_lifetime.unscoped_count;
    scoped_bytes = s_floor_lifetime.total_requested_bytes -
        s_floor_lifetime.unscoped_requested_bytes;
    phase_count = s_floor_lifetime.phase_count[0] +
        s_floor_lifetime.phase_count[1];
    if (phase_count < s_floor_lifetime.phase_count[0] ||
        phase_count > UINT32_MAX - s_floor_lifetime.phase_count[2])
        return 0;
    phase_count += s_floor_lifetime.phase_count[2];
    phase_bytes = s_floor_lifetime.phase_requested_bytes[0] +
        s_floor_lifetime.phase_requested_bytes[1];
    if (phase_bytes < s_floor_lifetime.phase_requested_bytes[0] ||
        phase_bytes > UINT32_MAX -
            s_floor_lifetime.phase_requested_bytes[2])
        return 0;
    phase_bytes += s_floor_lifetime.phase_requested_bytes[2];
    return s_floor_lifetime.current_count <= scoped_count &&
        s_floor_lifetime.current_requested_bytes <= scoped_bytes &&
        phase_count == s_floor_lifetime.current_count &&
        phase_bytes == s_floor_lifetime.current_requested_bytes;
}

static void vita_heap_floor_lifetime_snapshot_locked(
    isaac_vita_guest_heap_floor_lifetime_snapshot *snapshot)
{
    uint32_t scoped_count = 0U;
    uint32_t scoped_bytes = 0U;
    uint32_t valid = vita_heap_floor_lifetime_accounting_valid_locked() &&
        s_floor_lifetime.protocol_valid && s_floor_lifetime.epoch &&
        s_floor_lifetime.bootstrap_count == 1U &&
        s_floor_lifetime.epoch ==
            s_floor_lifetime.bootstrap_count +
                s_floor_lifetime.rollover_count &&
        s_floor_lifetime.phase >=
            ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT &&
        s_floor_lifetime.phase <= ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY;

    memset(snapshot, 0, sizeof *snapshot);
    snapshot->epoch = s_floor_lifetime.epoch;
    snapshot->bootstrap_count = s_floor_lifetime.bootstrap_count;
    snapshot->rollover_count = s_floor_lifetime.rollover_count;
    snapshot->phase = s_floor_lifetime.phase;
    snapshot->total_count = s_floor_lifetime.total_count;
    snapshot->unscoped_count = s_floor_lifetime.unscoped_count;
    snapshot->current_count = s_floor_lifetime.current_count;
    snapshot->total_requested_bytes =
        s_floor_lifetime.total_requested_bytes;
    snapshot->unscoped_requested_bytes =
        s_floor_lifetime.unscoped_requested_bytes;
    snapshot->current_requested_bytes =
        s_floor_lifetime.current_requested_bytes;
    memcpy(snapshot->phase_count, s_floor_lifetime.phase_count,
           sizeof snapshot->phase_count);
    memcpy(snapshot->phase_requested_bytes,
           s_floor_lifetime.phase_requested_bytes,
           sizeof snapshot->phase_requested_bytes);
    if (snapshot->total_count >= snapshot->unscoped_count)
        scoped_count = snapshot->total_count - snapshot->unscoped_count;
    else
        valid = 0U;
    if (snapshot->total_requested_bytes >=
            snapshot->unscoped_requested_bytes)
        scoped_bytes = snapshot->total_requested_bytes -
            snapshot->unscoped_requested_bytes;
    else
        valid = 0U;
    if (scoped_count >= snapshot->current_count)
        snapshot->prior_count = scoped_count - snapshot->current_count;
    else
        valid = 0U;
    if (scoped_bytes >= snapshot->current_requested_bytes)
        snapshot->prior_requested_bytes =
            scoped_bytes - snapshot->current_requested_bytes;
    else
        valid = 0U;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    snapshot->terminal = s_heap_terminal ? 1U : 0U;
#endif
    snapshot->counter_saturated =
        s_floor_lifetime.counter_saturated;
    snapshot->valid = valid && !snapshot->counter_saturated;
}
#endif

static size_t vita_heap_hash(uintptr_t key)
{
    key ^= key >> 16U;
    key *= (uintptr_t)0x7feb352dU;
    key ^= key >> 15U;
    key *= (uintptr_t)0x846ca68bU;
    key ^= key >> 16U;
    return (size_t)key;
}

/* Return the one replacement capacity reserve would need now.  Zero means no
 * replacement; a false return means growth is required but not representable.
 * Diagnostics and tests use this same decision so their byte report cannot
 * drift from the allocator transaction. */
static int vita_heap_ledger_reserve_target(
    size_t extra_live, size_t *target_capacity)
{
    size_t required_live;
    size_t target;

    *target_capacity = 0U;
    if (s_ledger_count > SIZE_MAX - extra_live)
        return 0;
    required_live = s_ledger_count + extra_live;
    if (s_ledger_fixed_capacity) {
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
        if (s_ledger_capacity != VITA_HEAP_LEDGER_FIXED_CAPACITY ||
            required_live >
                ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT)
#else
        if (!s_ledger_capacity ||
            required_live > s_ledger_capacity / 2U)
#endif
            return 0;
        return 1;
    }
    if (!s_ledger_capacity)
        target = VITA_HEAP_LEDGER_INITIAL_CAPACITY;
    else {
        if (!extra_live)
            return 1;
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
        if (required_live <= s_ledger_capacity / 2U)
            return 1;
#else
        if (s_ledger_tombstones <=
                SIZE_MAX - s_ledger_count - extra_live &&
            s_ledger_count + s_ledger_tombstones + extra_live <
                s_ledger_capacity / 2U)
            return 1;
        if (required_live < s_ledger_capacity / 2U) {
            *target_capacity = s_ledger_capacity;
            return 1;
        }
#endif
        target = s_ledger_capacity;
    }
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    while (required_live > target / 2U) {
#else
    while (required_live >= target / 2U) {
#endif
        if (target > SIZE_MAX / 2U)
            return 0;
        target *= 2U;
    }
    *target_capacity = target;
    return 1;
}

#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_HEAP_TESTING)
static size_t vita_heap_ledger_rehash_target_bytes_locked(size_t extra_live)
{
    size_t target_capacity = 0U;

    if (!vita_heap_ledger_reserve_target(extra_live, &target_capacity))
        return SIZE_MAX;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    if (target_capacity) {
        vita_heap_ledger_layout layout;

        return vita_heap_ledger_combined_layout(
            target_capacity, &layout) ? layout.combined_bytes : SIZE_MAX;
    }
    return 0U;
#else
    if (target_capacity > SIZE_MAX / sizeof *s_ledger)
        return SIZE_MAX;
    return target_capacity * sizeof *s_ledger;
#endif
}
#endif

#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
static size_t vita_heap_find_slot(const uintptr_t *table, size_t capacity,
                                  uintptr_t key, int *found)
{
    size_t mask;
    size_t slot;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    size_t first_tombstone = SIZE_MAX;
#endif
    size_t probes;

    if (!capacity) {
        *found = 0;
        VITA_HEAP_TEST_RECORD_FIND(0U);
        return SIZE_MAX;
    }
    mask = capacity - 1U;
    slot = vita_heap_hash(key) & mask;
    for (probes = 0U; probes < capacity; ++probes) {
        uintptr_t current = table[slot];
        if (current == key) {
            *found = 1;
            VITA_HEAP_TEST_RECORD_FIND(probes + 1U);
            return slot;
        }
        if (current == 0U) {
            *found = 0;
            VITA_HEAP_TEST_RECORD_FIND(probes + 1U);
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
            return slot;
#else
            return first_tombstone != SIZE_MAX ? first_tombstone : slot;
#endif
        }
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
        if (current == VITA_HEAP_LEDGER_TOMBSTONE &&
            first_tombstone == SIZE_MAX)
            first_tombstone = slot;
#endif
        slot = (slot + 1U) & mask;
    }
    *found = 0;
    VITA_HEAP_TEST_RECORD_FIND(capacity);
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    return SIZE_MAX;
#else
    return first_tombstone;
#endif
}

static int vita_heap_ledger_rehash(size_t new_capacity)
{
    vita_heap_ledger_candidate candidate;
    uintptr_t *replacement;
    size_t index;

    if (new_capacity > SIZE_MAX / sizeof *replacement)
        return 0;
    if (!vita_heap_ledger_candidate_prepare(
            new_capacity * sizeof *replacement, &candidate))
        return 0;
    replacement = (uintptr_t *)VITA_HEAP_LEDGER_CANDIDATE_BASE(candidate);

    for (index = 0U; index < s_ledger_capacity; ++index) {
        uintptr_t key = s_ledger[index];
        if (key > VITA_HEAP_LEDGER_TOMBSTONE) {
            int found;
            size_t slot = vita_heap_find_slot(
                replacement, new_capacity, key, &found);
            if (found || slot == SIZE_MAX) {
                (void)vita_heap_ledger_candidate_rollback(&candidate);
                return 0;
            }
            replacement[slot] = key;
        }
    }
    if (!vita_heap_ledger_candidate_commit(&candidate))
        return 0;
    s_ledger = replacement;
    s_ledger_capacity = new_capacity;
    s_ledger_tombstones = 0U;
    return 1;
}

static int vita_heap_ledger_reserve(size_t extra_live)
{
    size_t new_capacity;

    if (!vita_heap_ledger_reserve_target(extra_live, &new_capacity))
        return 0;
    return !new_capacity || vita_heap_ledger_rehash(new_capacity);
}

static int vita_heap_ledger_insert(uintptr_t key)
{
    int found;
    size_t slot;

    if (key <= VITA_HEAP_LEDGER_TOMBSTONE ||
        !vita_heap_ledger_reserve(1U))
        return 0;
    slot = vita_heap_find_slot(s_ledger, s_ledger_capacity, key, &found);
    if (found || slot == SIZE_MAX)
        return 0;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    if (s_ledger[slot] == VITA_HEAP_LEDGER_TOMBSTONE)
        --s_ledger_tombstones;
#endif
    s_ledger[slot] = key;
    ++s_ledger_count;
    return 1;
}

static int vita_heap_ledger_lookup(uintptr_t key, size_t *slot_out)
{
    int found;
    size_t slot;

    if (!s_ledger_capacity || key <= VITA_HEAP_LEDGER_TOMBSTONE)
        return 0;
    slot = vita_heap_find_slot(s_ledger, s_ledger_capacity, key, &found);
    if (found && slot_out)
        *slot_out = slot;
    return found;
}

#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
static int vita_heap_ledger_remove_preflight_table(
    const uintptr_t *table, size_t capacity, size_t slot)
{
    size_t mask;
    size_t scan;
    size_t probes;

    if (!capacity || slot >= capacity ||
        table[slot] <= VITA_HEAP_LEDGER_TOMBSTONE)
        return 0;
    mask = capacity - 1U;
    scan = (slot + 1U) & mask;
    /* This first pass is deliberately read-only.  A corrupt/full table must
     * fail before free/realloc can make an irreversible native side effect. */
    for (probes = 0U; probes < capacity; ++probes) {
        if (table[scan] == 0U)
            return 1;
        if (table[scan] == VITA_HEAP_LEDGER_TOMBSTONE)
            return 0;
        scan = (scan + 1U) & mask;
    }
    return 0;
}

static void vita_heap_ledger_remove_commit_table(
    uintptr_t *table, size_t capacity, size_t slot)
{
    size_t mask = capacity - 1U;
    size_t hole = slot;
    size_t scan = (slot + 1U) & mask;
    size_t probes;

    /* Preflight established a terminating empty slot while the caller held
     * s_ledger_lock.  Move complete entries using cyclic probe distances. */
    for (probes = 0U; probes < capacity && table[scan] != 0U; ++probes) {
        uintptr_t key = table[scan];
        size_t home = vita_heap_hash(key) & mask;

        if (((scan - home) & mask) > ((hole - home) & mask)) {
            table[hole] = key;
            hole = scan;
        }
        scan = (scan + 1U) & mask;
    }
    table[hole] = 0U;
}
#endif

static int vita_heap_ledger_remove_preflight(size_t slot)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    return vita_heap_ledger_remove_preflight_table(
        s_ledger, s_ledger_capacity, slot);
#else
    return slot < s_ledger_capacity &&
        s_ledger[slot] > VITA_HEAP_LEDGER_TOMBSTONE;
#endif
}

static void vita_heap_ledger_remove_slot_commit(size_t slot)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    vita_heap_ledger_remove_commit_table(s_ledger, s_ledger_capacity, slot);
#else
    s_ledger[slot] = VITA_HEAP_LEDGER_TOMBSTONE;
    ++s_ledger_tombstones;
#endif
    --s_ledger_count;
}

static int vita_heap_ledger_remove_slot(size_t slot)
{
    if (!vita_heap_ledger_remove_preflight(slot))
        return 0;
    vita_heap_ledger_remove_slot_commit(slot);
    return 1;
}
#else
static size_t vita_heap_find_slot(const vita_heap_ledger_entry *table,
                                  size_t capacity, uintptr_t key, int *found)
{
    size_t mask;
    size_t slot;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    size_t first_tombstone = SIZE_MAX;
#endif
    size_t probes;

    if (!capacity) {
        *found = 0;
        VITA_HEAP_TEST_RECORD_FIND(0U);
        return SIZE_MAX;
    }
    mask = capacity - 1U;
    slot = vita_heap_hash(key) & mask;
    for (probes = 0U; probes < capacity; ++probes) {
        uintptr_t current = table[slot].base;
        if (current == key) {
            *found = 1;
            VITA_HEAP_TEST_RECORD_FIND(probes + 1U);
            return slot;
        }
        if (current == 0U) {
            *found = 0;
            VITA_HEAP_TEST_RECORD_FIND(probes + 1U);
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
            return slot;
#else
            return first_tombstone != SIZE_MAX ? first_tombstone : slot;
#endif
        }
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
        if (current == VITA_HEAP_LEDGER_TOMBSTONE &&
            first_tombstone == SIZE_MAX)
            first_tombstone = slot;
#endif
        slot = (slot + 1U) & mask;
    }
    *found = 0;
    VITA_HEAP_TEST_RECORD_FIND(capacity);
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    return SIZE_MAX;
#else
    return first_tombstone;
#endif
}

static int vita_heap_ledger_rehash(size_t new_capacity)
{
    vita_heap_ledger_candidate candidate;
    vita_heap_ledger_entry *replacement;
    vita_heap_ledger_layout layout;
    uint8_t *replacement_phases;
    uint32_t *replacement_current;
    size_t index;

    if (!vita_heap_ledger_combined_layout(new_capacity, &layout))
        return 0;
    if (!vita_heap_ledger_candidate_prepare(
            layout.combined_bytes, &candidate))
        return 0;
    replacement = (vita_heap_ledger_entry *)
        VITA_HEAP_LEDGER_CANDIDATE_BASE(candidate);
    vita_heap_ledger_sidecars_from_base(
        replacement, &layout, &replacement_phases, &replacement_current);

    for (index = 0U; index < s_ledger_capacity; ++index) {
        uintptr_t key = s_ledger[index].base;
        if (key > VITA_HEAP_LEDGER_TOMBSTONE) {
            int found;
            uint32_t token;
            size_t slot = vita_heap_find_slot(
                replacement, new_capacity, key, &found);
            if (!s_ledger_floor_phase || !s_ledger_floor_current ||
                s_ledger_floor_phase[index] >
                    ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY ||
                (!s_ledger_floor_phase[index] &&
                 vita_heap_floor_current_bit_get(
                     s_ledger_floor_current, index)) ||
                found || slot == SIZE_MAX) {
                s_floor_lifetime.accounting_valid = 0U;
                (void)vita_heap_ledger_candidate_rollback(&candidate);
                return 0;
            }
            token = vita_heap_floor_lifetime_slot_token(
                index, s_ledger_floor_phase, s_ledger_floor_current);
            replacement[slot] = s_ledger[index];
            vita_heap_floor_lifetime_slot_store(
                slot, token, replacement_phases, replacement_current);
        }
    }
    if (!vita_heap_ledger_candidate_commit(&candidate))
        return 0;
    s_ledger = replacement;
    s_ledger_floor_phase = replacement_phases;
    s_ledger_floor_current = replacement_current;
    s_ledger_capacity = new_capacity;
    s_ledger_tombstones = 0U;
    return 1;
}

static int vita_heap_ledger_reserve(size_t extra_live)
{
    size_t new_capacity;

    if (!vita_heap_ledger_reserve_target(extra_live, &new_capacity))
        return 0;
    return !new_capacity || vita_heap_ledger_rehash(new_capacity);
}

static int vita_heap_ledger_insert(uintptr_t key, size_t requested_size)
{
    int found;
    size_t slot;
    uint32_t floor_token;

    if (key <= VITA_HEAP_LEDGER_TOMBSTONE ||
        !vita_heap_ledger_reserve(1U))
        return 0;
    slot = vita_heap_find_slot(s_ledger, s_ledger_capacity, key, &found);
    if (found || slot == SIZE_MAX)
        return 0;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    if (s_ledger[slot].base == VITA_HEAP_LEDGER_TOMBSTONE)
        --s_ledger_tombstones;
#endif
    s_ledger[slot].base = key;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    s_ledger[slot].requested_size = requested_size;
#else
    (void)requested_size;
#endif
    ++s_ledger_count;
    floor_token = vita_heap_floor_lifetime_new_token_locked();
    vita_heap_floor_lifetime_slot_insert_locked(
        slot, requested_size, floor_token);
    return 1;
}

static int vita_heap_ledger_lookup(uintptr_t key, size_t *slot_out)
{
    int found;
    size_t slot;

    if (!s_ledger_capacity || key <= VITA_HEAP_LEDGER_TOMBSTONE)
        return 0;
    slot = vita_heap_find_slot(s_ledger, s_ledger_capacity, key, &found);
    if (found && slot_out)
        *slot_out = slot;
    return found;
}

#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
static int vita_heap_ledger_remove_preflight_table(
    const vita_heap_ledger_entry *table, size_t capacity, size_t slot)
{
    size_t mask;
    size_t scan;
    size_t probes;

    if (!capacity || slot >= capacity ||
        table[slot].base <= VITA_HEAP_LEDGER_TOMBSTONE)
        return 0;
    mask = capacity - 1U;
    scan = (slot + 1U) & mask;
    for (probes = 0U; probes < capacity; ++probes) {
        if (table[scan].base == 0U)
            return 1;
        if (table[scan].base == VITA_HEAP_LEDGER_TOMBSTONE)
            return 0;
        scan = (scan + 1U) & mask;
    }
    return 0;
}

static void vita_heap_ledger_remove_commit_table(
    vita_heap_ledger_entry *table, size_t capacity, size_t slot,
    uint8_t *phases, uint32_t *current_bits)
{
    size_t mask = capacity - 1U;
    size_t hole = slot;
    size_t scan = (slot + 1U) & mask;
    size_t probes;

    for (probes = 0U;
         probes < capacity && table[scan].base != 0U;
         ++probes) {
        uintptr_t key = table[scan].base;
        size_t home = vita_heap_hash(key) & mask;

        if (((scan - home) & mask) > ((hole - home) & mask)) {
            table[hole] = table[scan];
            if (phases && current_bits) {
                uint32_t token = vita_heap_floor_lifetime_slot_token(
                    scan, phases, current_bits);
                vita_heap_floor_lifetime_slot_store(
                    hole, token, phases, current_bits);
            }
            hole = scan;
        }
        scan = (scan + 1U) & mask;
    }
    table[hole].base = 0U;
    table[hole].requested_size = 0U;
    if (phases && current_bits)
        vita_heap_floor_lifetime_slot_store(
            hole, 0U, phases, current_bits);
}
#endif

static int vita_heap_ledger_remove_preflight(size_t slot)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    return vita_heap_ledger_remove_preflight_table(
        s_ledger, s_ledger_capacity, slot);
#else
    return slot < s_ledger_capacity &&
        s_ledger[slot].base > VITA_HEAP_LEDGER_TOMBSTONE;
#endif
}

static void vita_heap_ledger_remove_slot_commit(size_t slot)
{
    size_t requested_size = s_ledger[slot].requested_size;

    (void)vita_heap_floor_lifetime_slot_remove_locked(
        slot, requested_size);
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    vita_heap_ledger_remove_commit_table(
        s_ledger, s_ledger_capacity, slot,
        s_ledger_floor_phase, s_ledger_floor_current);
#else
    s_ledger[slot].base = VITA_HEAP_LEDGER_TOMBSTONE;
    s_ledger[slot].requested_size = 0U;
    vita_heap_floor_lifetime_slot_store(
        slot, 0U, s_ledger_floor_phase, s_ledger_floor_current);
    ++s_ledger_tombstones;
#endif
    --s_ledger_count;
}

static int vita_heap_ledger_remove_slot(size_t slot)
{
    if (!vita_heap_ledger_remove_preflight(slot))
        return 0;
    vita_heap_ledger_remove_slot_commit(slot);
    return 1;
}
#endif

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
static int vita_heap_base_is_leased(uintptr_t base)
{
    size_t index;

    if (!s_heap_lease_count)
        return 0;
    for (index = 0U; index < VITA_HEAP_LEASE_CAPACITY; ++index)
        if (s_heap_leases[index].token &&
            s_heap_leases[index].base == base)
            return 1;
    return 0;
}
#endif

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
static void vita_heap_telemetry_counter_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
    else {
        s_heap_telemetry.accounting_valid = 0U;
        s_heap_telemetry.counter_saturated = 1U;
    }
}

static void vita_heap_telemetry_poison_locked(void)
{
    s_heap_telemetry.accounting_valid = 0U;
}

static uint32_t vita_heap_telemetry_size_u32(size_t size)
{
    if (size <= UINT32_MAX)
        return (uint32_t)size;
    s_heap_telemetry.accounting_valid = 0U;
    s_heap_telemetry.counter_saturated = 1U;
    return UINT32_MAX;
}

static void vita_heap_telemetry_validate_locked(void)
{
    isaac_vita_heap_overflow_mspace_snapshot raw;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    uint32_t room_raw_pages;
    uint32_t room_raw_requested;
#endif
    uint32_t raw_live;
    uint32_t raw_stranded;
    uint32_t raw_internal;

    if (!s_heap_telemetry.active ||
        !isaac_vita_heap_overflow_mspace_snapshot_get(&raw) ||
        raw.live_count > UINT32_MAX || raw.stranded_count > UINT32_MAX ||
        raw.internal_live_count > UINT32_MAX ||
        raw.internal_requested_bytes > UINT32_MAX) {
        s_heap_telemetry.accounting_valid = 0U;
        return;
    }
    raw_live = (uint32_t)raw.live_count;
    raw_stranded = (uint32_t)raw.stranded_count;
    raw_internal = (uint32_t)raw.internal_live_count;
    if (s_heap_telemetry.owned_live_count >
            UINT32_MAX - raw_stranded ||
        s_heap_telemetry.owned_live_count + raw_stranded >
            UINT32_MAX - raw_internal ||
        raw_live != s_heap_telemetry.owned_live_count + raw_stranded +
            raw_internal)
        s_heap_telemetry.accounting_valid = 0U;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    /* External slab pages never enter the raw mspace counters.  Pin the raw
     * internal subset to RAW descriptors, not total hybrid pages, so the
     * generic+stranded+internal raw-live identity remains exact. */
    if (!isaac_vita_room_entry_slab_raw_accounting_locked(
            &room_raw_pages, &room_raw_requested) ||
        room_raw_pages != raw_internal ||
        room_raw_requested != raw.internal_requested_bytes)
        s_heap_telemetry.accounting_valid = 0U;
#endif
    if (!s_heap_telemetry.counter_saturated &&
        (s_heap_telemetry.pool_allocations < s_heap_telemetry.pool_frees ||
         s_heap_telemetry.pool_allocations - s_heap_telemetry.pool_frees !=
             s_heap_telemetry.owned_live_count ||
         s_heap_telemetry.peak_live_count <
             s_heap_telemetry.owned_live_count ||
         s_heap_telemetry.peak_requested_bytes <
             s_heap_telemetry.owned_requested_bytes ||
         s_heap_telemetry.native_to_pool >
             s_heap_telemetry.pool_allocations ||
         s_heap_telemetry.pool_to_native > s_heap_telemetry.pool_frees ||
         s_heap_telemetry.consumed_terminal >
             s_heap_telemetry.pool_frees -
                 s_heap_telemetry.pool_to_native))
        s_heap_telemetry.accounting_valid = 0U;
}

static void vita_heap_telemetry_snapshot_locked(
    isaac_vita_guest_heap_telemetry_snapshot *snapshot)
{
    isaac_vita_heap_overflow_mspace_snapshot raw;
    int raw_ok;

    vita_heap_telemetry_validate_locked();
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->sequence = s_heap_telemetry.sequence;
    snapshot->owned_live_count = s_heap_telemetry.owned_live_count;
    snapshot->owned_requested_bytes =
        s_heap_telemetry.owned_requested_bytes;
    snapshot->peak_live_count = s_heap_telemetry.peak_live_count;
    snapshot->peak_requested_bytes = s_heap_telemetry.peak_requested_bytes;
    snapshot->pool_allocations = s_heap_telemetry.pool_allocations;
    snapshot->pool_frees = s_heap_telemetry.pool_frees;
    snapshot->pool_reallocations = s_heap_telemetry.pool_reallocations;
    snapshot->native_failures = s_heap_telemetry.native_failures;
    snapshot->pool_failures = s_heap_telemetry.pool_failures;
    snapshot->native_to_pool = s_heap_telemetry.native_to_pool;
    snapshot->pool_to_native = s_heap_telemetry.pool_to_native;
    snapshot->consumed_terminal = s_heap_telemetry.consumed_terminal;
    raw_ok = isaac_vita_heap_overflow_mspace_snapshot_get(&raw);
    if (raw_ok && raw.live_count <= UINT32_MAX &&
        raw.stranded_count <= UINT32_MAX &&
        raw.internal_live_count <= UINT32_MAX &&
        raw.internal_requested_bytes <= UINT32_MAX) {
        snapshot->raw_live_count = (uint32_t)raw.live_count;
        snapshot->raw_stranded_count = (uint32_t)raw.stranded_count;
        snapshot->raw_internal_live_count =
            (uint32_t)raw.internal_live_count;
        snapshot->raw_internal_requested_bytes =
            (uint32_t)raw.internal_requested_bytes;
        snapshot->mspace_state = (uint32_t)raw.state;
    }
    else {
        snapshot->raw_live_count = UINT32_MAX;
        snapshot->raw_stranded_count = UINT32_MAX;
        snapshot->raw_internal_live_count = UINT32_MAX;
        snapshot->raw_internal_requested_bytes = UINT32_MAX;
        snapshot->mspace_state =
            (uint32_t)ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED;
    }
    snapshot->terminal = s_heap_terminal ? 1U : 0U;
    snapshot->accounting_valid =
        s_heap_telemetry.active && s_heap_telemetry.accounting_valid && raw_ok
            ? 1U : 0U;
    snapshot->counter_saturated = s_heap_telemetry.counter_saturated;
}

static void vita_heap_telemetry_event_init(
    vita_heap_telemetry_event *event, uint32_t operation, size_t request)
{
    event->edges = 0U;
    event->operation = operation;
    event->request = request <= UINT32_MAX ? (uint32_t)request : UINT32_MAX;
}

static void vita_heap_telemetry_claim_locked(
    vita_heap_telemetry_event *event, uint32_t edges)
{
    uint32_t unclaimed;

    if (!event || !s_heap_telemetry.active)
        return;
    unclaimed = edges & ~s_heap_telemetry.claimed_edges;
    if (!unclaimed)
        return;
    s_heap_telemetry.claimed_edges |= unclaimed;
    if (!event->edges) {
        vita_heap_telemetry_counter_increment(&s_heap_telemetry.sequence);
        event->edges = unclaimed;
    }
    else {
        event->edges |= unclaimed;
    }
    vita_heap_telemetry_snapshot_locked(&event->snapshot);
}

static const char *vita_heap_telemetry_edge_name(uint32_t edge)
{
    switch (edge) {
    case VITA_HEAP_TELEMETRY_EDGE_FIRST_USE:
        return "first-use";
    case VITA_HEAP_TELEMETRY_EDGE_FIRST_FREE:
        return "first-free";
    case VITA_HEAP_TELEMETRY_EDGE_FIRST_REALLOC:
        return "first-realloc";
    case VITA_HEAP_TELEMETRY_EDGE_NATIVE_TO_POOL:
        return "native-to-pool";
    case VITA_HEAP_TELEMETRY_EDGE_POOL_TO_NATIVE:
        return "pool-to-native";
    case VITA_HEAP_TELEMETRY_EDGE_POOL_FAILURE:
        return "pool-failure";
    case VITA_HEAP_TELEMETRY_EDGE_TERMINAL:
        return "terminal";
    case VITA_HEAP_TELEMETRY_EDGE_FINAL:
        return "final";
    default:
        return "unknown";
    }
}

static void vita_heap_telemetry_emit(const vita_heap_telemetry_event *event)
{
    uint32_t edge;

    if (!event)
        return;
    for (edge = 1U; edge <= VITA_HEAP_TELEMETRY_EDGE_FINAL; edge <<= 1U) {
        const isaac_vita_guest_heap_telemetry_snapshot *s;

        if (!(event->edges & edge))
            continue;
        s = &event->snapshot;
        /* Worst-case decimal expansion is 355 bytes, below the logger's
         * 384-byte body limit.  Every argument is explicitly 32-bit-safe. */
        isaac_vita_log(
            "heapovf: e=%s q=%u op=%u req=%u live=%u/%u peak=%u/%u "
            "a/f/r=%u/%u/%u n2p/p2n=%u/%u nf/pf=%u/%u cons=%u "
            "raw/str/int=%u/%u/%u/%u state=%u term=%u valid/sat=%u/%u",
            vita_heap_telemetry_edge_name(edge), (unsigned)s->sequence,
            (unsigned)event->operation, (unsigned)event->request,
            (unsigned)s->owned_live_count,
            (unsigned)s->owned_requested_bytes,
            (unsigned)s->peak_live_count,
            (unsigned)s->peak_requested_bytes,
            (unsigned)s->pool_allocations, (unsigned)s->pool_frees,
            (unsigned)s->pool_reallocations,
            (unsigned)s->native_to_pool, (unsigned)s->pool_to_native,
            (unsigned)s->native_failures, (unsigned)s->pool_failures,
            (unsigned)s->consumed_terminal,
            (unsigned)s->raw_live_count, (unsigned)s->raw_stranded_count,
            (unsigned)s->raw_internal_live_count,
            (unsigned)s->raw_internal_requested_bytes,
            (unsigned)s->mspace_state, (unsigned)s->terminal,
            (unsigned)s->accounting_valid,
            (unsigned)s->counter_saturated);
    }
}

static void vita_heap_unlock_with_telemetry(
    vita_heap_telemetry_event *event)
{
    if (event && event->edges)
        vita_heap_telemetry_snapshot_locked(&event->snapshot);
    vita_heap_unlock();
    vita_heap_telemetry_emit(event);
}

static void vita_heap_telemetry_owned_add_locked(
    size_t requested_size, vita_heap_telemetry_event *event,
    uint32_t extra_edges)
{
    uint32_t request = vita_heap_telemetry_size_u32(requested_size);

    if (s_heap_telemetry.owned_live_count == UINT32_MAX ||
        request > UINT32_MAX - s_heap_telemetry.owned_requested_bytes) {
        s_heap_telemetry.accounting_valid = 0U;
        s_heap_telemetry.counter_saturated = 1U;
    }
    else {
        ++s_heap_telemetry.owned_live_count;
        s_heap_telemetry.owned_requested_bytes += request;
        if (s_heap_telemetry.owned_live_count >
            s_heap_telemetry.peak_live_count)
            s_heap_telemetry.peak_live_count =
                s_heap_telemetry.owned_live_count;
        if (s_heap_telemetry.owned_requested_bytes >
            s_heap_telemetry.peak_requested_bytes)
            s_heap_telemetry.peak_requested_bytes =
                s_heap_telemetry.owned_requested_bytes;
    }
    vita_heap_telemetry_counter_increment(
        &s_heap_telemetry.pool_allocations);
    vita_heap_telemetry_validate_locked();
    vita_heap_telemetry_claim_locked(
        event, VITA_HEAP_TELEMETRY_EDGE_FIRST_USE | extra_edges);
}

static void vita_heap_telemetry_owned_remove_locked(size_t requested_size)
{
    uint32_t request = vita_heap_telemetry_size_u32(requested_size);

    if (!s_heap_telemetry.owned_live_count ||
        request > s_heap_telemetry.owned_requested_bytes) {
        s_heap_telemetry.accounting_valid = 0U;
        return;
    }
    --s_heap_telemetry.owned_live_count;
    s_heap_telemetry.owned_requested_bytes -= request;
}

static void vita_heap_telemetry_pool_free_locked(
    size_t requested_size, vita_heap_telemetry_event *event,
    uint32_t extra_edges)
{
    vita_heap_telemetry_owned_remove_locked(requested_size);
    vita_heap_telemetry_counter_increment(&s_heap_telemetry.pool_frees);
    vita_heap_telemetry_validate_locked();
    vita_heap_telemetry_claim_locked(
        event, VITA_HEAP_TELEMETRY_EDGE_FIRST_FREE | extra_edges);
}

static void vita_heap_telemetry_pool_realloc_locked(
    size_t old_size, size_t new_size, vita_heap_telemetry_event *event)
{
    uint32_t old_request = vita_heap_telemetry_size_u32(old_size);
    uint32_t new_request = vita_heap_telemetry_size_u32(new_size);
    uint32_t without_old;

    if (old_request > s_heap_telemetry.owned_requested_bytes) {
        s_heap_telemetry.accounting_valid = 0U;
    }
    else {
        without_old = s_heap_telemetry.owned_requested_bytes - old_request;
        if (new_request > UINT32_MAX - without_old) {
            s_heap_telemetry.accounting_valid = 0U;
            s_heap_telemetry.counter_saturated = 1U;
        }
        else {
            s_heap_telemetry.owned_requested_bytes = without_old + new_request;
            if (s_heap_telemetry.owned_requested_bytes >
                s_heap_telemetry.peak_requested_bytes)
                s_heap_telemetry.peak_requested_bytes =
                    s_heap_telemetry.owned_requested_bytes;
        }
    }
    vita_heap_telemetry_counter_increment(
        &s_heap_telemetry.pool_reallocations);
    vita_heap_telemetry_validate_locked();
    vita_heap_telemetry_claim_locked(
        event, VITA_HEAP_TELEMETRY_EDGE_FIRST_REALLOC);
}

static void vita_heap_telemetry_native_failure_locked(void)
{
    vita_heap_telemetry_counter_increment(&s_heap_telemetry.native_failures);
}

static void vita_heap_telemetry_pool_failure_locked(
    vita_heap_telemetry_event *event)
{
    vita_heap_telemetry_counter_increment(&s_heap_telemetry.pool_failures);
    vita_heap_telemetry_validate_locked();
    vita_heap_telemetry_claim_locked(
        event, VITA_HEAP_TELEMETRY_EDGE_POOL_FAILURE);
}

static void vita_heap_telemetry_consumed_locked(
    size_t old_size, vita_heap_telemetry_event *event)
{
    vita_heap_telemetry_owned_remove_locked(old_size);
    vita_heap_telemetry_counter_increment(&s_heap_telemetry.pool_frees);
    vita_heap_telemetry_counter_increment(
        &s_heap_telemetry.consumed_terminal);
    s_heap_telemetry.accounting_valid = 0U;
    vita_heap_telemetry_validate_locked();
    vita_heap_telemetry_claim_locked(
        event, VITA_HEAP_TELEMETRY_EDGE_FIRST_FREE);
}
#else
static void vita_heap_telemetry_poison_locked(void)
{
}

static void vita_heap_telemetry_event_init(
    vita_heap_telemetry_event *event, uint32_t operation, size_t request)
{
    (void)operation;
    (void)request;
    event->unused = 0U;
}

static void vita_heap_unlock_with_telemetry(
    vita_heap_telemetry_event *event)
{
    (void)event;
    vita_heap_unlock();
}
#endif

static void vita_heap_router_result_set(
    vita_heap_router_result *result_out, vita_heap_router_result result,
    vita_heap_telemetry_event *event)
{
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    /* Every TERMINAL transition is made while the heap lock is held.  Keep a
     * durable latch for callers of the public pointer-only API, including
     * terminal ledger invariants which leave the raw mspace itself READY. */
    if (result == VITA_HEAP_ROUTER_TERMINAL) {
        int first_terminal = !s_heap_terminal;

        s_heap_terminal = 1;
        if (first_terminal) {
            vita_heap_telemetry_claim_locked(
                event, VITA_HEAP_TELEMETRY_EDGE_TERMINAL);
        }
    }
#else
    (void)event;
#endif
    if (result_out)
        *result_out = result;
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
static int vita_heap_ranges_overlap(
    const isaac_vita_heap_overflow_range *first,
    const isaac_vita_heap_overflow_range *second)
{
    return first->begin < second->end && second->begin < first->end;
}

static int vita_heap_build_forbidden_ranges(
    uintptr_t link_end, size_t heap_bytes,
    uint32_t stack_floor, uint32_t stack_ceiling,
    isaac_vita_heap_overflow_forbidden_ranges *ranges_out)
{
    uintptr_t alignment;
    uintptr_t retained_end;
    uintptr_t fixed_end;

    if (!ranges_out || !link_end || !heap_bytes ||
        stack_floor >= stack_ceiling)
        return 0;
    alignment = (uintptr_t)heap_bytes &
        (~(uintptr_t)heap_bytes + (uintptr_t)1U);
    if (alignment < VITA_HEAP_PAGE_BYTES)
        alignment = VITA_HEAP_PAGE_BYTES;
    if (link_end > UINTPTR_MAX - (uintptr_t)heap_bytes ||
        link_end + (uintptr_t)heap_bytes > UINTPTR_MAX - alignment)
        return 0;
    retained_end = link_end + (uintptr_t)heap_bytes + alignment;
    if (retained_end > UINTPTR_MAX - (VITA_HEAP_PAGE_BYTES - 1U))
        return 0;
    retained_end = (retained_end + VITA_HEAP_PAGE_BYTES - 1U) &
        ~(uintptr_t)(VITA_HEAP_PAGE_BYTES - 1U);
    if (!retained_end || retained_end >= GUEST_PE_VITA_TARGET_BASE ||
        GUEST_PE_VITA_TARGET_BASE >
            UINTPTR_MAX - GUEST_PE_EXPECTED_IMAGE_SIZE)
        return 0;
    fixed_end = (uintptr_t)GUEST_PE_VITA_TARGET_BASE +
        (uintptr_t)GUEST_PE_EXPECTED_IMAGE_SIZE;

    ranges_out->retained_newlib.begin = link_end;
    ranges_out->retained_newlib.end = retained_end;
    ranges_out->fixed_image.begin = GUEST_PE_VITA_TARGET_BASE;
    ranges_out->fixed_image.end = fixed_end;
    ranges_out->guest_stack.begin = (uintptr_t)stack_floor;
    ranges_out->guest_stack.end = (uintptr_t)stack_ceiling;
    /* guest_stack_init uses newlib, so the stack is expected to lie inside
     * retained_newlib.  Both are independently forbidden to the new mspace. */
    if (vita_heap_ranges_overlap(&ranges_out->retained_newlib,
                                 &ranges_out->fixed_image) ||
        vita_heap_ranges_overlap(&ranges_out->guest_stack,
                                 &ranges_out->fixed_image))
        return 0;
    return 1;
}

static int vita_heap_initialize_locked(
    uintptr_t link_end, size_t heap_bytes,
    uint32_t stack_floor, uint32_t stack_ceiling)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges;
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    isaac_vita_heap_overflow_range raw_request;
#endif

    if (s_heap_init_state != VITA_HEAP_INIT_UNINITIALIZED ||
        s_heap_terminal) {
        s_heap_init_state = VITA_HEAP_INIT_FAILED;
        s_heap_terminal = 1;
        return 0;
    }
    if (heap_bytes != VITA_HEAP_REQUIRED_NEWLIB_BYTES ||
        !vita_heap_build_forbidden_ranges(
            link_end, heap_bytes, stack_floor, stack_ceiling, &ranges) ||
        s_ledger_count || s_heap_lease_count) {
        s_heap_init_state = VITA_HEAP_INIT_FAILED;
        s_heap_terminal = 1;
        return 0;
    }
#if defined(__vita__)
    {
        size_t ledger_request = 0U;
        vita_heap_ledger_layout ledger_layout;
        if (sizeof(vita_heap_ledger_entry) !=
                VITA_HEAP_LEDGER_FIXED_ENTRY_BYTES ||
            !vita_heap_ledger_combined_layout(
                VITA_HEAP_LEDGER_FIXED_CAPACITY, &ledger_layout) ||
            ledger_layout.entry_bytes !=
                VITA_HEAP_LEDGER_FIXED_ENTRY_TABLE_BYTES ||
            ledger_layout.phase_bytes !=
                VITA_HEAP_FLOOR_FIXED_PHASE_BYTES ||
            ledger_layout.current_bytes !=
                VITA_HEAP_FLOOR_FIXED_CURRENT_BYTES ||
            ledger_layout.combined_bytes !=
                VITA_HEAP_LEDGER_FIXED_USABLE_BYTES ||
            !isaac_vita_heap_ledger_memblock_layout(
                VITA_HEAP_LEDGER_FIXED_USABLE_BYTES, &ledger_request) ||
            ledger_request != VITA_HEAP_LEDGER_FIXED_REQUEST_BYTES) {
            s_heap_init_state = VITA_HEAP_INIT_FAILED;
            s_heap_terminal = 1;
            return 0;
        }
    }
#endif
    /* The ledger backend clears its USER_RW block as part of acquisition.
     * This relies on the kernel memblock allocator never mapping over an
     * existing newlib/image/stack mapping; the separately retained overflow
     * pool still validates its complete returned span against all ranges. */
    if (s_ledger_capacity != VITA_HEAP_LEDGER_FIXED_CAPACITY &&
        !vita_heap_ledger_rehash(VITA_HEAP_LEDGER_FIXED_CAPACITY)) {
        s_heap_init_state = VITA_HEAP_INIT_FAILED;
        s_heap_terminal = 1;
        return 0;
    }
    s_ledger_fixed_capacity = 1;
    if (!isaac_vita_heap_overflow_mspace_init(&ranges) ||
        !isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) ||
        snapshot.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
        snapshot.capacity_bytes !=
            ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES ||
        snapshot.retained_bytes !=
            ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES ||
        snapshot.live_count || snapshot.stranded_count ||
        snapshot.internal_live_count ||
        snapshot.internal_requested_bytes) {
        s_heap_init_state = VITA_HEAP_INIT_FAILED;
        s_heap_terminal = 1;
        return 0;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_HYBRID
    raw_request.begin = snapshot.base;
    raw_request.end = snapshot.end;
    if (!isaac_vita_room_entry_external_init(&ranges, &raw_request)) {
        s_heap_init_state = VITA_HEAP_INIT_FAILED;
        s_heap_terminal = 1;
        return 0;
    }
#endif
    memset(&s_heap_telemetry, 0, sizeof s_heap_telemetry);
    s_heap_telemetry.active = 1U;
    s_heap_telemetry.accounting_valid = 1U;
    vita_heap_telemetry_validate_locked();
    if (!s_heap_telemetry.accounting_valid) {
        s_heap_init_state = VITA_HEAP_INIT_FAILED;
        s_heap_terminal = 1;
        return 0;
    }
    s_heap_init_state = VITA_HEAP_INIT_READY;
    return 1;
}

int isaac_vita_guest_heap_init(uint32_t stack_floor, uint32_t stack_ceiling)
{
    extern char _end;
    extern unsigned int _newlib_heap_size_user;
    int result;

    vita_heap_lock();
    result = vita_heap_initialize_locked(
        (uintptr_t)(void *)&_end, (size_t)_newlib_heap_size_user,
        stack_floor, stack_ceiling);
    vita_heap_unlock();
    return result;
}

static int vita_heap_overflow_is_terminal_locked(void)
{
    isaac_vita_heap_overflow_mspace_snapshot snapshot;

    return !isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot) ||
        snapshot.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
}

static isaac_vita_heap_overflow_mspace_state
vita_heap_overflow_state_locked(void)
{
    isaac_vita_heap_overflow_mspace_snapshot snapshot;

    if (!isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot))
        return ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED;
    return snapshot.state;
}

static int vita_heap_new_allocation_ready_locked(void)
{
    /* Every raw-mspace state transition happens below this same lock and its
     * router branch sets the durable terminal latch before unlocking.  The
     * latch is therefore the exact hot-path readiness cache; polling a raw
     * snapshot here would tax even allocations satisfied by newlib. */
    return !s_heap_terminal &&
        s_heap_init_state == VITA_HEAP_INIT_READY;
}

static int vita_heap_pointer_is_overflow_locked(const void *pointer)
{
    return isaac_vita_heap_overflow_mspace_contains(pointer);
}

static int vita_heap_overflow_free_locked(void *pointer)
{
#ifdef ISAAC_VITA_HEAP_TESTING
    if (s_heap_test_fail_raw_free) {
        --s_heap_test_fail_raw_free;
        return 0;
    }
#endif
    return isaac_vita_heap_overflow_mspace_free(pointer);
}
#endif

static void *vita_heap_guest_malloc_with_reason(
    size_t size, const char **failure_reason,
    vita_heap_router_result *router_result)
{
    void *result;
    vita_heap_router_result local_router_result;
    vita_heap_telemetry_event telemetry;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    int overflow_result = 0;
#endif

    vita_heap_telemetry_event_init(
        &telemetry, VITA_HEAP_TELEMETRY_OP_MALLOC, size);
    if (!router_result)
        router_result = &local_router_result;
    if (failure_reason)
        *failure_reason = "none";
    vita_heap_router_result_set(
        router_result, VITA_HEAP_ROUTER_OK, &telemetry);
    vita_heap_lock();
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (!vita_heap_new_allocation_ready_locked()) {
        if (failure_reason)
            *failure_reason =
                s_heap_init_state == VITA_HEAP_INIT_READY
                    ? "overflow-mspace-corrupted"
                    : "overflow-mspace-not-initialized";
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
#endif
    if (!vita_heap_ledger_reserve(1U)) {
        if (failure_reason)
            *failure_reason = "ledger-reserve-failed";
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
    result = malloc(size);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (result && vita_heap_pointer_is_overflow_locked(result)) {
        free(result);
        result = NULL;
        vita_heap_telemetry_poison_locked();
        if (failure_reason)
            *failure_reason = "native-overlapped-overflow-mspace";
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
    }
    if (!result && *router_result != VITA_HEAP_ROUTER_TERMINAL) {
        if (failure_reason)
            *failure_reason = "native-malloc-failed";
        vita_heap_telemetry_native_failure_locked();
        result = isaac_vita_heap_overflow_mspace_malloc(size);
        overflow_result = result != NULL;
        if (!result)
            vita_heap_telemetry_pool_failure_locked(&telemetry);
        if (!result && vita_heap_overflow_is_terminal_locked()) {
            if (failure_reason)
                *failure_reason = "overflow-mspace-corrupted";
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        }
    }
#else
    if (!result && failure_reason)
        *failure_reason = "native-malloc-failed";
#endif
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    if (result && !vita_heap_ledger_insert((uintptr_t)result, size)) {
#else
    if (result && !vita_heap_ledger_insert((uintptr_t)result)) {
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
        if (overflow_result)
            (void)vita_heap_overflow_free_locked(result);
        else
#endif
            free(result);
        result = NULL;
        vita_heap_telemetry_poison_locked();
        if (failure_reason)
            *failure_reason = "ledger-insert-failed";
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
    }
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (result && overflow_result)
        vita_heap_telemetry_owned_add_locked(size, &telemetry, 0U);
#endif
    if (!result && *router_result == VITA_HEAP_ROUTER_OK)
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, &telemetry);
    vita_heap_unlock_with_telemetry(&telemetry);
    return result;
}

void *isaac_vita_guest_malloc(size_t size)
{
    return vita_heap_guest_malloc_with_reason(size, NULL, NULL);
}

static void *vita_heap_guest_calloc_impl(
    size_t count, size_t size, vita_heap_router_result *router_result)
{
    void *result;
    size_t total = 0U;
    int size_overflow = count && size > SIZE_MAX / count;
    vita_heap_router_result local_router_result;
    vita_heap_telemetry_event telemetry;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    int overflow_result = 0;
#endif

    if (!size_overflow)
        total = count * size;
    vita_heap_telemetry_event_init(
        &telemetry, VITA_HEAP_TELEMETRY_OP_CALLOC,
        size_overflow ? SIZE_MAX : total);
    if (!router_result)
        router_result = &local_router_result;
    vita_heap_router_result_set(
        router_result, VITA_HEAP_ROUTER_OK, &telemetry);
    vita_heap_lock();
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (!vita_heap_new_allocation_ready_locked()) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
#endif
    if (size_overflow) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
    if (!vita_heap_ledger_reserve(1U)) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
    result = calloc(count, size);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (result && vita_heap_pointer_is_overflow_locked(result)) {
        free(result);
        result = NULL;
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
    }
    if (!result && *router_result != VITA_HEAP_ROUTER_TERMINAL) {
        vita_heap_telemetry_native_failure_locked();
        result = isaac_vita_heap_overflow_mspace_calloc(count, size);
        overflow_result = result != NULL;
        if (!result) {
            vita_heap_telemetry_pool_failure_locked(&telemetry);
        }
        if (!result && vita_heap_overflow_is_terminal_locked())
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
    }
#endif
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    if (result && !vita_heap_ledger_insert(
                      (uintptr_t)result, total)) {
#else
    if (result && !vita_heap_ledger_insert((uintptr_t)result)) {
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
        if (overflow_result) {
            if (!vita_heap_overflow_free_locked(result))
                vita_heap_telemetry_poison_locked();
        }
        else
#endif
            free(result);
        result = NULL;
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
    }
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (result && overflow_result)
        vita_heap_telemetry_owned_add_locked(total, &telemetry, 0U);
#endif
    if (!result && *router_result == VITA_HEAP_ROUTER_OK)
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, &telemetry);
    vita_heap_unlock_with_telemetry(&telemetry);
    return result;
}

void *isaac_vita_guest_calloc(size_t count, size_t size)
{
    return vita_heap_guest_calloc_impl(count, size, NULL);
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
static int vita_heap_ledger_replace_preflight(
    size_t old_slot, uintptr_t new_key)
{
    if (new_key <= VITA_HEAP_LEDGER_TOMBSTONE ||
        !vita_heap_ledger_remove_preflight(old_slot))
        return 0;
    if (new_key != s_ledger[old_slot].base &&
        vita_heap_ledger_lookup(new_key, NULL))
        return 0;
    return 1;
}
#endif

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
static int vita_heap_ledger_replace_commit(
    size_t old_slot, uintptr_t new_key, size_t requested_size)
{
    int found;
    size_t new_slot;
    size_t old_size = s_ledger[old_slot].requested_size;
    uint32_t floor_token = vita_heap_floor_lifetime_slot_token(
        old_slot, s_ledger_floor_phase, s_ledger_floor_current);

    if (new_key == s_ledger[old_slot].base) {
        vita_heap_floor_lifetime_requested_size_replace_locked(
            old_slot, old_size, requested_size);
        s_ledger[old_slot].requested_size = requested_size;
        return 1;
    }
    vita_heap_ledger_remove_slot_commit(old_slot);
    new_slot = vita_heap_find_slot(
        s_ledger, s_ledger_capacity, new_key, &found);
    if (found || new_slot == SIZE_MAX)
        return 0;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    if (s_ledger[new_slot].base == VITA_HEAP_LEDGER_TOMBSTONE)
        --s_ledger_tombstones;
#endif
    s_ledger[new_slot].base = new_key;
    s_ledger[new_slot].requested_size = requested_size;
    ++s_ledger_count;
    vita_heap_floor_lifetime_slot_insert_locked(
        new_slot, requested_size, floor_token);
#ifdef ISAAC_VITA_HEAP_TESTING
    ++s_realloc_moves;
#endif
    return 1;
}
#endif

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
static void *vita_heap_migrate_native_to_overflow_locked(
    void *pointer, size_t old_size, size_t size, size_t old_slot,
    vita_heap_router_result *router_result,
    vita_heap_telemetry_event *telemetry)
{
    void *replacement = isaac_vita_heap_overflow_mspace_malloc(size);

    if (!replacement) {
        vita_heap_telemetry_pool_failure_locked(telemetry);
        vita_heap_router_result_set(
            router_result,
            vita_heap_overflow_is_terminal_locked()
                ? VITA_HEAP_ROUTER_TERMINAL
                : VITA_HEAP_ROUTER_OUT_OF_MEMORY,
            telemetry);
        return NULL;
    }
    if (!vita_heap_ledger_replace_preflight(
            old_slot, (uintptr_t)replacement)) {
        if (!vita_heap_overflow_free_locked(replacement))
            vita_heap_telemetry_poison_locked();
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, telemetry);
        return NULL;
    }
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    if (!vita_heap_ledger_replace_commit(
            old_slot, (uintptr_t)replacement, size)) {
        if (!vita_heap_overflow_free_locked(replacement))
            vita_heap_telemetry_poison_locked();
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, telemetry);
        return NULL;
    }
    free(pointer);
    vita_heap_telemetry_counter_increment(&s_heap_telemetry.native_to_pool);
    vita_heap_telemetry_owned_add_locked(
        size, telemetry, VITA_HEAP_TELEMETRY_EDGE_NATIVE_TO_POOL);
    return replacement;
}

static void *vita_heap_migrate_overflow_to_native_locked(
    void *pointer, size_t old_size, size_t size, size_t old_slot,
    vita_heap_router_result *router_result,
    vita_heap_telemetry_event *telemetry)
{
    void *replacement = malloc(size);
    int raw_freed;

    if (!replacement) {
        vita_heap_telemetry_native_failure_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, telemetry);
        return NULL;
    }
    if (vita_heap_pointer_is_overflow_locked(replacement) ||
        !vita_heap_ledger_replace_preflight(
            old_slot, (uintptr_t)replacement)) {
        free(replacement);
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, telemetry);
        return NULL;
    }
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    if (!vita_heap_ledger_replace_commit(
            old_slot, (uintptr_t)replacement, size)) {
        free(replacement);
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, telemetry);
        return NULL;
    }
    raw_freed = vita_heap_overflow_free_locked(pointer);
    if (!raw_freed)
        vita_heap_telemetry_poison_locked();
    vita_heap_telemetry_counter_increment(&s_heap_telemetry.pool_to_native);
    vita_heap_telemetry_pool_free_locked(
        old_size, telemetry, VITA_HEAP_TELEMETRY_EDGE_POOL_TO_NATIVE);
    if (!raw_freed) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, telemetry);
        return NULL;
    }
    return replacement;
}
#endif

static void *vita_heap_guest_realloc_impl(void *pointer, size_t size,
                                          int *valid_owner, int force_move,
                                          void **retired_pointer,
                                          vita_heap_router_result *router_result)
{
    uintptr_t old_key = (uintptr_t)pointer;
    size_t old_slot = 0U;
    void *result;
    vita_heap_router_result local_router_result;
    vita_heap_telemetry_event telemetry;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    int old_is_overflow;
    size_t old_size;
#endif

    vita_heap_telemetry_event_init(
        &telemetry, VITA_HEAP_TELEMETRY_OP_REALLOC, size);
    if (!router_result)
        router_result = &local_router_result;
    vita_heap_router_result_set(
        router_result, VITA_HEAP_ROUTER_OK, &telemetry);

    if (retired_pointer)
        *retired_pointer = NULL;
    if (valid_owner)
        *valid_owner = 1;
    if (!pointer)
        return vita_heap_guest_malloc_with_reason(
            size, NULL, router_result);
    vita_heap_lock();
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (s_heap_init_state != VITA_HEAP_INIT_READY) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
#endif
    if (!vita_heap_ledger_lookup(old_key, &old_slot)) {
        if (valid_owner)
            *valid_owner = 0;
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_FOREIGN, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    if (vita_heap_base_is_leased(old_key)) {
        if (valid_owner)
            *valid_owner = 0;
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_FOREIGN, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    old_is_overflow = vita_heap_pointer_is_overflow_locked(pointer);
    old_size = s_ledger[old_slot].requested_size;
#endif
    if (!size) {
        if (!vita_heap_ledger_remove_slot(old_slot)) {
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
            vita_heap_unlock_with_telemetry(&telemetry);
            return NULL;
        }
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
        if (old_is_overflow) {
            int raw_freed =
                vita_heap_overflow_free_locked(pointer);

            if (!raw_freed)
                vita_heap_telemetry_poison_locked();
            vita_heap_telemetry_pool_free_locked(
                old_size, &telemetry, 0U);
            if (!raw_freed)
                vita_heap_router_result_set(
                    router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        }
        else
#endif
        free(pointer);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (!old_is_overflow) {
        if (s_heap_terminal) {
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
            vita_heap_unlock_with_telemetry(&telemetry);
            return NULL;
        }
    }
    else {
        isaac_vita_heap_overflow_mspace_state overflow_state =
            vita_heap_overflow_state_locked();

        /* Exact pool allocations may still be freed above or evacuated to
         * native storage below after DRAIN_ONLY.  No native-domain realloc,
         * and no other broken raw state, may perform an allocator side effect
         * after the durable terminal latch has fired. */
        if ((overflow_state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY &&
             overflow_state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY) ||
            (s_heap_terminal &&
             overflow_state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY)) {
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
            vita_heap_unlock_with_telemetry(&telemetry);
            return NULL;
        }
    }
#endif

    /* A moved allocation consumes a fresh hash position.  Reserve before
     * calling native realloc so there is guaranteed insertion space
     * afterwards.  Rehash may move old_slot. */
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    if (!vita_heap_ledger_reserve(0U)) {
#else
    if (!vita_heap_ledger_reserve(1U)) {
#endif
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_OUT_OF_MEMORY, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;             /* old allocation remains live and tracked */
    }
    if (!vita_heap_ledger_lookup(old_key, &old_slot)) {
        vita_heap_telemetry_poison_locked();
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
    /* Backshift removal needs a terminating empty slot.  Prove it now, while
     * native realloc can still be skipped without changing the allocation. */
    if (!vita_heap_ledger_remove_preflight(old_slot)) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return NULL;
    }
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    {
        old_size = s_ledger[old_slot].requested_size;
        if (!old_is_overflow) {
#ifdef ISAAC_VITA_HEAP_TESTING
            if (force_move) {
                result = malloc(size);
                if (result)
                    *retired_pointer = pointer;
            }
            else
#endif
            {
                (void)force_move;
                (void)retired_pointer;
                result = realloc(pointer, size);
            }
            if (result) {
                if (vita_heap_pointer_is_overflow_locked(result) ||
                    !vita_heap_ledger_replace_preflight(
                        old_slot, (uintptr_t)result) ||
                    !vita_heap_ledger_replace_commit(
                        old_slot, (uintptr_t)result, size)) {
                    vita_heap_telemetry_poison_locked();
                    vita_heap_router_result_set(
                        router_result, VITA_HEAP_ROUTER_TERMINAL,
                        &telemetry);
                    vita_heap_unlock_with_telemetry(&telemetry);
                    return NULL;
                }
                vita_heap_unlock_with_telemetry(&telemetry);
                return result;
            }
            vita_heap_telemetry_native_failure_locked();
            result = vita_heap_migrate_native_to_overflow_locked(
                pointer, old_size, size, old_slot, router_result,
                &telemetry);
            vita_heap_unlock_with_telemetry(&telemetry);
            return result;
        }
        else {
            isaac_vita_heap_overflow_mspace_realloc_status status;

            (void)force_move;
            (void)retired_pointer;
            result = isaac_vita_heap_overflow_mspace_realloc(
                pointer, size, &status);
            if (status ==
                    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS &&
                result) {
                if (!vita_heap_pointer_is_overflow_locked(result) ||
                    !vita_heap_ledger_replace_preflight(
                        old_slot, (uintptr_t)result) ||
                    !vita_heap_ledger_replace_commit(
                        old_slot, (uintptr_t)result, size)) {
                    vita_heap_telemetry_poison_locked();
                    vita_heap_router_result_set(
                        router_result, VITA_HEAP_ROUTER_TERMINAL,
                        &telemetry);
                    vita_heap_unlock_with_telemetry(&telemetry);
                    return NULL;
                }
                vita_heap_telemetry_pool_realloc_locked(
                    old_size, size, &telemetry);
                vita_heap_unlock_with_telemetry(&telemetry);
                return result;
            }
            if (status ==
                ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_CONSUMED_DRAIN_ONLY) {
                vita_heap_ledger_remove_slot_commit(old_slot);
                vita_heap_telemetry_consumed_locked(
                    old_size, &telemetry);
                vita_heap_router_result_set(
                    router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
                vita_heap_unlock_with_telemetry(&telemetry);
                return NULL;
            }
            if ((status ==
                     ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_OUT_OF_MEMORY &&
                 vita_heap_overflow_state_locked() ==
                     ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY) ||
                (status ==
                     ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_REJECTED &&
                  vita_heap_overflow_state_locked() ==
                      ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY)) {
                if (status ==
                    ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_OUT_OF_MEMORY)
                    vita_heap_telemetry_pool_failure_locked(&telemetry);
                result = vita_heap_migrate_overflow_to_native_locked(
                    pointer, old_size, size, old_slot, router_result,
                    &telemetry);
                vita_heap_unlock_with_telemetry(&telemetry);
                return result;
            }
            if (status ==
                ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_SUCCESS)
                vita_heap_telemetry_poison_locked();
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
            vita_heap_unlock_with_telemetry(&telemetry);
            return NULL;
        }
    }
#else
#ifdef ISAAC_VITA_HEAP_TESTING
    if (force_move) {
        /* Test-only allocator injection: leave the old native allocation live
         * so successive calls cannot reuse an address.  The ledger transaction
         * below is exactly the production moved-realloc path. */
        result = malloc(size);
        if (result)
            *retired_pointer = pointer;
    } else
#endif
    {
        (void)force_move;
        (void)retired_pointer;
        result = realloc(pointer, size);
    }
    if (!result) {
        vita_heap_unlock();
        return NULL;             /* old allocation remains live and tracked */
    }
#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
    if ((uintptr_t)result != old_key) {
        /* Count is unchanged, so the removed slot guarantees insertion space
         * without another native allocation or a post-realloc failure. */
        int found;
        size_t new_slot;
        vita_heap_ledger_remove_slot_commit(old_slot);
        new_slot = vita_heap_find_slot(
            s_ledger, s_ledger_capacity, (uintptr_t)result, &found);
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
        if (s_ledger[new_slot] == VITA_HEAP_LEDGER_TOMBSTONE)
            --s_ledger_tombstones;
#endif
        s_ledger[new_slot] = (uintptr_t)result;
        ++s_ledger_count;
#ifdef ISAAC_VITA_HEAP_TESTING
        ++s_realloc_moves;
#endif
    }
#else
    if (!vita_heap_ledger_replace_commit(
            old_slot, (uintptr_t)result, size)) {
        /* The read-only preflight above makes this unreachable for a valid
         * table.  Fail closed if a corrupt table changed under the lock. */
        s_floor_lifetime.accounting_valid = 0U;
        vita_heap_unlock();
        return NULL;
    }
#endif
    vita_heap_unlock();
    return result;
#endif
}

/* Optional observer for host modules that mirror guest-owned buffers
 * (ISAAC_VITA_NATIVE_VORBIS retires a stream mirror when the game frees its
 * stb_vorbis_alloc buffer).  Weak: NULL when no such module is linked. */
#if defined(__GNUC__)
void isaac_nv_guest_buffer_freed(void *pointer) __attribute__((weak));
#endif

static int vita_heap_guest_free_impl(
    void *pointer, vita_heap_router_result *router_result)
{
    size_t slot;
    vita_heap_router_result local_router_result;
    vita_heap_telemetry_event telemetry;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    int is_overflow;
    size_t old_size;
#endif

#if defined(__GNUC__)
    if (pointer && isaac_nv_guest_buffer_freed)
        isaac_nv_guest_buffer_freed(pointer);
#endif
    vita_heap_telemetry_event_init(
        &telemetry, VITA_HEAP_TELEMETRY_OP_FREE, 0U);
    if (!router_result)
        router_result = &local_router_result;
    vita_heap_router_result_set(
        router_result, VITA_HEAP_ROUTER_OK, &telemetry);

    vita_heap_lock();
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (s_heap_init_state != VITA_HEAP_INIT_READY) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return 0;
    }
#endif
    if (!pointer) {
        vita_heap_unlock_with_telemetry(&telemetry);
        return 1;
    }
    if (!vita_heap_ledger_lookup((uintptr_t)pointer, &slot)) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_FOREIGN, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return 0;
    }
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    if (vita_heap_base_is_leased((uintptr_t)pointer)) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_FOREIGN, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return 0;
    }
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    is_overflow = vita_heap_pointer_is_overflow_locked(pointer);
    old_size = s_ledger[slot].requested_size;
    telemetry.request = vita_heap_telemetry_size_u32(old_size);
#endif
    if (!vita_heap_ledger_remove_slot(slot)) {
        vita_heap_router_result_set(
            router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
        vita_heap_unlock_with_telemetry(&telemetry);
        return 0;
    }
    /* Ownership is invalidated before entering the native allocator. */
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (is_overflow) {
        int raw_freed = vita_heap_overflow_free_locked(pointer);

        if (!raw_freed)
            vita_heap_telemetry_poison_locked();
        vita_heap_telemetry_pool_free_locked(
            old_size, &telemetry, 0U);
        if (!raw_freed) {
            vita_heap_router_result_set(
                router_result, VITA_HEAP_ROUTER_TERMINAL, &telemetry);
            vita_heap_unlock_with_telemetry(&telemetry);
            return 0;
        }
    }
    else
#endif
    free(pointer);
    vita_heap_unlock_with_telemetry(&telemetry);
    return 1;
}

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
static isaac_vita_room_entry_slab_result vita_heap_room_slab_free_route(
    void *pointer, uint32_t free_return_rva,
    isaac_vita_room_entry_slab_decision *decision)
{
    isaac_vita_room_entry_slab_event slab_event;
    vita_heap_telemetry_event heap_event;
    isaac_vita_room_entry_slab_result result;

    isaac_vita_room_entry_slab_event_init(&slab_event);
    vita_heap_telemetry_event_init(
        &heap_event, VITA_HEAP_TELEMETRY_OP_FREE, 0U);
    vita_heap_lock();
    result = isaac_vita_room_entry_slab_free_locked(
        pointer, free_return_rva, vita_heap_base_is_leased,
        decision, &slab_event);
    vita_heap_room_slab_unlock(result, &slab_event, &heap_event);
    return result;
}

static isaac_vita_room_entry_slab_result vita_heap_room_slab_realloc_route(
    void *pointer, size_t size,
    isaac_vita_room_entry_slab_decision *decision, void **result_out)
{
    isaac_vita_room_entry_slab_event slab_event;
    vita_heap_telemetry_event heap_event;
    isaac_vita_room_entry_slab_result result;

    if (result_out)
        *result_out = NULL;
    isaac_vita_room_entry_slab_event_init(&slab_event);
    vita_heap_telemetry_event_init(
        &heap_event, VITA_HEAP_TELEMETRY_OP_REALLOC, size);
    vita_heap_lock();
    result = isaac_vita_room_entry_slab_realloc_begin_locked(
        pointer, size, vita_heap_base_is_leased, decision, &slab_event);
    vita_heap_room_slab_unlock(result, &slab_event, &heap_event);
    if (result == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) {
        if (result_out)
            *result_out = decision ? decision->pointer : NULL;
        return result;
    }
    if (result != ISAAC_VITA_ROOM_ENTRY_SLAB_MOVE)
        return result;
    {
        vita_heap_router_result allocation_result;
        void *replacement = vita_heap_guest_malloc_with_reason(
            size, NULL, &allocation_result);
        int allocation_terminal =
            allocation_result == VITA_HEAP_ROUTER_TERMINAL ||
            isaac_vita_guest_heap_terminal();
        int finish_ok;
        int retag_ok;
        int rollback_ok = 1;
        uint32_t replacement_old_floor_token = 0U;

        if (!replacement || allocation_terminal) {
            int cleanup_ok = 1;

            isaac_vita_room_entry_slab_event_init(&slab_event);
            vita_heap_telemetry_event_init(
                &heap_event, VITA_HEAP_TELEMETRY_OP_REALLOC, size);
            vita_heap_lock();
            finish_ok = isaac_vita_room_entry_slab_realloc_cancel_locked(
                pointer, &slab_event);
            vita_heap_room_slab_unlock(
                finish_ok ? ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED :
                    ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL,
                &slab_event, &heap_event);
            if (replacement) {
                vita_heap_router_result cleanup_result;

                cleanup_ok = vita_heap_guest_free_impl(
                    replacement, &cleanup_result) &&
                    cleanup_result != VITA_HEAP_ROUTER_TERMINAL;
            }
            if (!finish_ok || !cleanup_ok || allocation_terminal ||
                isaac_vita_guest_heap_terminal()) {
                if (!cleanup_ok) {
                    vita_heap_telemetry_event_init(
                        &heap_event, VITA_HEAP_TELEMETRY_OP_REALLOC, size);
                    vita_heap_lock();
                    vita_heap_router_result_set(
                        NULL, VITA_HEAP_ROUTER_TERMINAL, &heap_event);
                    vita_heap_unlock_with_telemetry(&heap_event);
                }
                return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
            }
            /* Ordinary generic allocation failure preserves the old slot. */
            return ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED;
        }
        memcpy(replacement, pointer,
               size < ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES ? size :
                   ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES);
        isaac_vita_room_entry_slab_event_init(&slab_event);
        vita_heap_telemetry_event_init(
            &heap_event, VITA_HEAP_TELEMETRY_OP_REALLOC, size);
        vita_heap_lock();
        if (s_heap_terminal) {
            vita_heap_router_result cleanup_result;

            finish_ok = isaac_vita_room_entry_slab_realloc_cancel_locked(
                pointer, &slab_event);
            vita_heap_room_slab_unlock(
                finish_ok ? ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED :
                    ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL,
                &slab_event, &heap_event);
            (void)vita_heap_guest_free_impl(
                replacement, &cleanup_result);
            return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
        }
#if defined(ISAAC_VITA_HEAP_TESTING) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
        if (s_heap_test_slab_move_floor_receipt.attempts != UINT32_MAX)
            ++s_heap_test_slab_move_floor_receipt.attempts;
        s_heap_test_slab_move_floor_receipt.inherited_token =
            decision ? decision->floorlife_token : UINT32_MAX;
        s_heap_test_slab_move_floor_receipt.replacement_final_token =
            UINT32_MAX;
        s_heap_test_slab_move_floor_receipt.retag_succeeded = 0U;
        s_heap_test_slab_move_floor_receipt.commit_succeeded = 0U;
        s_heap_test_slab_move_floor_receipt.rollback_attempted = 0U;
        s_heap_test_slab_move_floor_receipt.rollback_succeeded = 0U;
#endif
        retag_ok = decision && vita_heap_floor_lifetime_retag_locked(
            (uintptr_t)replacement, decision->floorlife_token,
            &replacement_old_floor_token);
#if defined(ISAAC_VITA_HEAP_TESTING) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
        s_heap_test_slab_move_floor_receipt.replacement_original_token =
            replacement_old_floor_token;
        s_heap_test_slab_move_floor_receipt.retag_succeeded =
            retag_ok ? 1U : 0U;
#endif
        finish_ok = retag_ok &&
            isaac_vita_room_entry_slab_realloc_commit_locked(
                pointer, &slab_event);
#if defined(ISAAC_VITA_HEAP_TESTING) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
        s_heap_test_slab_move_floor_receipt.commit_succeeded =
            finish_ok ? 1U : 0U;
#endif
        if (!finish_ok && retag_ok) {
#if defined(ISAAC_VITA_HEAP_TESTING) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
            s_heap_test_slab_move_floor_receipt.rollback_attempted = 1U;
#endif
            rollback_ok = vita_heap_floor_lifetime_retag_locked(
                (uintptr_t)replacement, replacement_old_floor_token,
                NULL);
#if defined(ISAAC_VITA_HEAP_TESTING) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
            s_heap_test_slab_move_floor_receipt.rollback_succeeded =
                rollback_ok ? 1U : 0U;
#endif
            if (!rollback_ok)
                s_floor_lifetime.accounting_valid = 0U;
        }
#if defined(ISAAC_VITA_HEAP_TESTING) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
        {
            size_t receipt_slot;

            if (vita_heap_ledger_lookup(
                    (uintptr_t)replacement, &receipt_slot))
                s_heap_test_slab_move_floor_receipt.
                    replacement_final_token =
                        vita_heap_floor_lifetime_slot_token(
                            receipt_slot, s_ledger_floor_phase,
                            s_ledger_floor_current);
        }
#endif
        if (finish_ok) {
            vita_heap_room_slab_unlock(
                ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED,
                &slab_event, &heap_event);
            if (result_out)
                *result_out = replacement;
            return ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED;
        }

        /* The old slot is deliberately left live+moving in the slab's
         * terminal state.  Publish the outer terminal before unlocking so no
         * concurrent allocation can enter after corruption.  Generic free
         * intentionally remains legal while terminal and can therefore
         * release the still-valid replacement without stranding ownership. */
        vita_heap_room_slab_unlock(
            ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL,
            &slab_event, &heap_event);
        {
            vita_heap_router_result cleanup_result;
            int cleanup_ok = vita_heap_guest_free_impl(
                replacement, &cleanup_result);

            if (!cleanup_ok ||
                cleanup_result == VITA_HEAP_ROUTER_TERMINAL) {
                /* The outer terminal already prevents further mutation; the
                 * telemetry records any independently failed cleanup. */
            }
        }
        return ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL;
    }
}
#endif

void *isaac_vita_guest_realloc(void *pointer, size_t size, int *valid_owner)
{
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_result room_result;
    void *result = NULL;

    room_result = vita_heap_room_slab_realloc_route(
        pointer, size, &decision, &result);
    if (room_result != ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED) {
        if (valid_owner)
            *valid_owner =
                room_result == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED ? 0 : 1;
        return result;
    }
#endif
    return vita_heap_guest_realloc_impl(
        pointer, size, valid_owner, 0, NULL, NULL);
}

int isaac_vita_guest_free(void *pointer)
{
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    isaac_vita_room_entry_slab_decision decision;
    isaac_vita_room_entry_slab_result room_result =
        vita_heap_room_slab_free_route(pointer, 0U, &decision);

    if (room_result == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED)
        return 1;
    if (room_result != ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED)
        return 0;
#endif
    return vita_heap_guest_free_impl(pointer, NULL);
}

int isaac_vita_guest_heap_owns(const void *pointer)
{
    int result;

    if (!pointer)
        return 0;
    vita_heap_lock();
    result = vita_heap_ledger_lookup((uintptr_t)pointer, NULL);
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    if (!result)
        result = isaac_vita_room_entry_slab_owns_exact_locked(pointer);
#endif
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_terminal(void)
{
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    int result;

    vita_heap_lock();
    result = s_heap_terminal;
    vita_heap_unlock();
    return result;
#else
    return 0;
#endif
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
int isaac_vita_guest_heap_telemetry_snapshot_get(
    isaac_vita_guest_heap_telemetry_snapshot *snapshot_out)
{
    int active;

    if (!snapshot_out)
        return 0;
    vita_heap_lock();
    active = s_heap_telemetry.active ? 1 : 0;
    vita_heap_telemetry_snapshot_locked(snapshot_out);
    vita_heap_unlock();
    return active;
}

void isaac_vita_guest_heap_telemetry_log_final(void)
{
    vita_heap_telemetry_event event;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    isaac_vita_room_entry_slab_event slab_event;
#endif

    vita_heap_telemetry_event_init(
        &event, VITA_HEAP_TELEMETRY_OP_NONE, 0U);
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    isaac_vita_room_entry_slab_event_init(&slab_event);
#endif
    vita_heap_lock();
    vita_heap_telemetry_claim_locked(
        &event, VITA_HEAP_TELEMETRY_EDGE_FINAL);
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    isaac_vita_room_entry_slab_claim_final_locked(&slab_event);
#endif
    vita_heap_unlock_with_telemetry(&event);
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    isaac_vita_room_entry_slab_log_event(&slab_event);
#endif
}
#endif

#if defined(__vita__) || defined(ISAAC_VITA_STAGE_MEMORY_ORACLE) || \
    defined(ISAAC_VITA_STAGE_MEMORY_RELEASE_ORACLE)
typedef struct vita_stage_memory_state {
    uint32_t sequence;
    uint32_t level_sequence;
    uint32_t room_sequence;
    uint32_t active;
    uint32_t level_stage;
    uint32_t level_type;
    uint32_t room_open;
    uint32_t room_after_unload;
    uint32_t room_stage;
    uint32_t room_mode;
    uint32_t counter_saturated;
} vita_stage_memory_state;

#define VITA_STAGE_MEMORY_STATE_INITIALIZER { \
    0U, 0U, 0U, 0U, UINT32_MAX, UINT32_MAX, 0U, 0U, \
    UINT32_MAX, UINT32_MAX, 0U \
}

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
typedef struct vita_stage_memory_snapshot {
    uint32_t arena;
    uint32_t uordblks;
    uint32_t fordblks;
    uint32_t ordblks;
    uint32_t ledger_count;
    uint32_t overflow_live;
    uint32_t overflow_requested;
    uint32_t raw_internal_live;
    uint32_t raw_internal_requested;
    uint32_t slab_pages;
    uint32_t slab_raw_pages;
    uint32_t slab_external_pages;
    uint32_t slab_live;
    uint32_t slab_allocations;
    uint32_t slab_frees;
    uint32_t valid;
    uint32_t terminal;
    uint32_t counter_saturated;
} vita_stage_memory_snapshot;
#endif

typedef struct vita_stage_memory_record {
    uint32_t sequence;
    uint32_t level_sequence;
    uint32_t room_sequence;
    uint32_t event;
    uint32_t context_valid;
    uint32_t active;
    uint32_t level_stage;
    uint32_t level_type;
    uint32_t room_stage;
    uint32_t room_mode;
    uint32_t result;
#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
    vita_stage_memory_snapshot memory;
#endif
} vita_stage_memory_record;

static vita_stage_memory_state s_stage_memory =
    VITA_STAGE_MEMORY_STATE_INITIALIZER;

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
typedef struct vita_floor_lifetime_record {
    isaac_vita_guest_heap_floor_lifetime_snapshot heap;
    uint32_t slab_total;
    uint32_t slab_unscoped;
    uint32_t slab_prior;
    uint32_t slab_current;
    uint32_t slab_phase[3];
    uint32_t valid;
    uint32_t terminal;
    uint32_t counter_saturated;
} vita_floor_lifetime_record;

static const char *vita_floor_lifetime_phase_name(uint32_t phase)
{
    switch (phase) {
    case ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT:
        return "level-init";
    case ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD:
        return "room-load";
    case ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY:
        return "play";
    default:
        return "pre-floor";
    }
}
#endif

static void vita_floor_lifetime_stage_event_locked(
    uint32_t event, uint32_t context_valid, uint32_t level_active)
{
    if (!context_valid) {
        vita_heap_floor_lifetime_invalidate_locked();
        return;
    }
    if (!s_floor_lifetime.protocol_valid)
        return;
    switch (event) {
    case ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN:
        if (!s_floor_lifetime.epoch)
            (void)vita_heap_floor_lifetime_bootstrap_locked(
                ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT);
        else
            (void)vita_heap_floor_lifetime_rollover_locked(
                ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT);
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN:
        if (!s_floor_lifetime.epoch)
            (void)vita_heap_floor_lifetime_bootstrap_locked(
                ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD);
        else
            (void)vita_heap_floor_lifetime_phase_set_locked(
                ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD);
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END:
        (void)vita_heap_floor_lifetime_phase_set_locked(
            level_active ? ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT :
                ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY);
        break;
    case ISAAC_VITA_STAGE_MEMORY_LEVEL_END:
        (void)vita_heap_floor_lifetime_phase_set_locked(
            ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY);
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE:
    case ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD:
        if (!s_floor_lifetime.epoch)
            vita_heap_floor_lifetime_invalidate_locked();
        break;
    default:
        vita_heap_floor_lifetime_invalidate_locked();
        break;
    }
}

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
static void vita_floor_lifetime_record_locked(
    vita_floor_lifetime_record *record)
{
    uint32_t valid;

    memset(record, 0, sizeof *record);
    vita_heap_floor_lifetime_snapshot_locked(&record->heap);
    valid = record->heap.valid;
    record->terminal = record->heap.terminal;
    record->counter_saturated = record->heap.counter_saturated;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_floor_lifetime_snapshot slab;
        uint32_t phase_sum;
        uint32_t live_sum;
        int slab_ok;

        memset(&slab, 0, sizeof slab);
        slab_ok =
            isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(&slab);

        record->slab_total = slab.live_slots;
        record->slab_unscoped = slab.unscoped_slots;
        record->slab_prior = slab.prior_slots;
        record->slab_current = slab.current_slots;
        record->slab_phase[0] = slab.level_init_slots;
        record->slab_phase[1] = slab.room_load_slots;
        record->slab_phase[2] = slab.play_slots;
        record->terminal |= slab.terminal ? 1U : 0U;
        record->counter_saturated |=
            slab.counter_saturated ? 1U : 0U;
        phase_sum = slab.level_init_slots + slab.room_load_slots;
        if (phase_sum < slab.level_init_slots ||
            phase_sum > UINT32_MAX - slab.play_slots)
            valid = 0U;
        else
            phase_sum += slab.play_slots;
        live_sum = slab.unscoped_slots + slab.prior_slots;
        if (live_sum < slab.unscoped_slots ||
            live_sum > UINT32_MAX - slab.current_slots)
            valid = 0U;
        else
            live_sum += slab.current_slots;
        if (!slab_ok || !slab.valid || !slab.active ||
            slab.epoch != record->heap.epoch ||
            slab.phase != record->heap.phase ||
            phase_sum != slab.current_slots ||
            live_sum != slab.live_slots)
            valid = 0U;
    }
#endif
    record->valid = valid && !record->counter_saturated;
}
#endif
#endif

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
static uint32_t vita_stage_memory_size_u32(size_t value,
                                           uint32_t *valid)
{
#if SIZE_MAX > UINT32_MAX
    if (value > UINT32_MAX) {
        *valid = 0U;
        return UINT32_MAX;
    }
#else
    (void)valid;
#endif
    return (uint32_t)value;
}
#endif

static uint32_t vita_stage_memory_next_sequence_locked(void)
{
    if (s_stage_memory.sequence != UINT32_MAX)
        ++s_stage_memory.sequence;
    else
        s_stage_memory.counter_saturated = 1U;
    return s_stage_memory.sequence;
}

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
static void vita_stage_memory_snapshot_locked(
    vita_stage_memory_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->valid = 1U;
    snapshot->ledger_count = vita_stage_memory_size_u32(
        s_ledger_count, &snapshot->valid);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    {
        isaac_vita_heap_overflow_mspace_snapshot raw;
        uint32_t raw_live;
        uint32_t raw_stranded;
        int raw_ok = isaac_vita_heap_overflow_mspace_snapshot_get(&raw);

        snapshot->overflow_live = s_heap_telemetry.owned_live_count;
        snapshot->overflow_requested =
            s_heap_telemetry.owned_requested_bytes;
        snapshot->counter_saturated |=
            s_heap_telemetry.counter_saturated;
        snapshot->terminal |= s_heap_terminal ? 1U : 0U;
        if (!s_heap_telemetry.active ||
            !s_heap_telemetry.accounting_valid || !raw_ok) {
            snapshot->valid = 0U;
        }
        if (raw_ok) {
            raw_live = vita_stage_memory_size_u32(
                raw.live_count, &snapshot->valid);
            raw_stranded = vita_stage_memory_size_u32(
                raw.stranded_count, &snapshot->valid);
            snapshot->raw_internal_live = vita_stage_memory_size_u32(
                raw.internal_live_count, &snapshot->valid);
            snapshot->raw_internal_requested =
                vita_stage_memory_size_u32(
                    raw.internal_requested_bytes, &snapshot->valid);
            if (raw.state != ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY ||
                !raw.has_mspace)
                snapshot->valid = 0U;
            if (raw.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_FAILED ||
                raw.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED ||
                raw.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_DRAIN_ONLY)
                snapshot->terminal = 1U;
            if (snapshot->overflow_live >
                    UINT32_MAX - raw_stranded ||
                snapshot->overflow_live + raw_stranded >
                    UINT32_MAX - snapshot->raw_internal_live ||
                raw_live != snapshot->overflow_live + raw_stranded +
                    snapshot->raw_internal_live)
                snapshot->valid = 0U;
        }
        else {
            snapshot->raw_internal_live = UINT32_MAX;
            snapshot->raw_internal_requested = UINT32_MAX;
        }
    }
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_fast_snapshot room;

        if (!isaac_vita_room_entry_slab_fast_snapshot_locked(&room)) {
            snapshot->valid = 0U;
            snapshot->slab_pages = UINT32_MAX;
            snapshot->slab_raw_pages = UINT32_MAX;
            snapshot->slab_external_pages = UINT32_MAX;
            snapshot->slab_live = UINT32_MAX;
            snapshot->slab_allocations = UINT32_MAX;
            snapshot->slab_frees = UINT32_MAX;
        }
        else {
            snapshot->slab_pages = room.pages;
            snapshot->slab_raw_pages = room.raw_pages;
            snapshot->slab_external_pages = room.external_pages;
            snapshot->slab_live = room.live_slots;
            snapshot->slab_allocations = room.allocations;
            snapshot->slab_frees = room.frees;
            snapshot->terminal |= room.terminal ? 1U : 0U;
            snapshot->counter_saturated |=
                room.counter_saturated ? 1U : 0U;
            if (room.pages >
                    ISAAC_VITA_ROOM_ENTRY_SLAB_TOTAL_MAX_PAGES ||
                room.raw_pages >
                    ISAAC_VITA_ROOM_ENTRY_SLAB_RAW_MAX_PAGES ||
                room.raw_pages > room.pages ||
                room.external_pages != room.pages - room.raw_pages ||
                room.raw_pages !=
                    snapshot->raw_internal_live ||
                room.raw_pages *
                    ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES !=
                    snapshot->raw_internal_requested)
                snapshot->valid = 0U;
        }
    }
#endif
    snapshot->counter_saturated |= s_stage_memory.counter_saturated;
    if (snapshot->counter_saturated)
        snapshot->valid = 0U;
}

static void vita_stage_memory_heap_near_point(
    const struct mallinfo *heap, vita_stage_memory_snapshot *snapshot)
{
    /* This post-boundary sample is intentionally not transactional with the
     * locked ownership snapshot.  newlib exposes size_t fields while glibc's
     * legacy mallinfo uses int; a negative host fixture still fails the same
     * checked conversion without an always-false VitaSDK comparison. */
    snapshot->arena = vita_stage_memory_size_u32(
        (size_t)heap->arena, &snapshot->valid);
    snapshot->uordblks = vita_stage_memory_size_u32(
        (size_t)heap->uordblks, &snapshot->valid);
    snapshot->fordblks = vita_stage_memory_size_u32(
        (size_t)heap->fordblks, &snapshot->valid);
    snapshot->ordblks = vita_stage_memory_size_u32(
        (size_t)heap->ordblks, &snapshot->valid);
}

static const char *vita_stage_memory_event_name(uint32_t event)
{
    switch (event) {
    case ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN:
        return "level-begin";
    case ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE:
        return "room-reuse";
    case ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN:
        return "room-load-begin";
    case ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD:
        return "room-after-unload";
    case ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END:
        return "room-load-end";
    case ISAAC_VITA_STAGE_MEMORY_LEVEL_END:
        return "level-end";
    default:
        return "unknown";
    }
}
#endif

static void vita_stage_memory_context_locked(
    uint32_t event, uint32_t stage, uint32_t mode_or_type,
    uint32_t result, vita_stage_memory_record *record,
    int *clear_level, int *clear_room)
{
    uint32_t no_result = result == UINT32_MAX ? 1U : 0U;

    record->context_valid = 0U;
    *clear_level = 0;
    *clear_room = 0;
    switch (event) {
    case ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN:
        if (!no_result || s_stage_memory.active ||
            s_stage_memory.room_open)
            break;
        s_stage_memory.active = 1U;
        s_stage_memory.level_sequence = record->sequence;
        s_stage_memory.level_stage = stage;
        s_stage_memory.level_type = mode_or_type;
        s_stage_memory.room_sequence = 0U;
        s_stage_memory.room_stage = UINT32_MAX;
        s_stage_memory.room_mode = UINT32_MAX;
        s_stage_memory.room_after_unload = 0U;
        record->context_valid = 1U;
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE:
        if (!no_result || s_stage_memory.room_open)
            break;
        s_stage_memory.room_sequence = record->sequence;
        s_stage_memory.room_stage = stage;
        s_stage_memory.room_mode = mode_or_type;
        s_stage_memory.room_after_unload = 0U;
        record->context_valid = 1U;
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN:
        if (!no_result || s_stage_memory.room_open)
            break;
        s_stage_memory.room_open = 1U;
        s_stage_memory.room_after_unload = 0U;
        s_stage_memory.room_sequence = record->sequence;
        s_stage_memory.room_stage = stage;
        s_stage_memory.room_mode = mode_or_type;
        record->context_valid = 1U;
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD:
        if (!no_result || !s_stage_memory.room_open ||
            s_stage_memory.room_after_unload ||
            s_stage_memory.room_stage != stage ||
            s_stage_memory.room_mode != mode_or_type)
            break;
        s_stage_memory.room_after_unload = 1U;
        record->context_valid = 1U;
        break;
    case ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END:
        if (result > 1U || !s_stage_memory.room_open ||
            s_stage_memory.room_stage != stage ||
            s_stage_memory.room_mode != mode_or_type ||
            (result == 1U && !s_stage_memory.room_after_unload))
            break;
        record->context_valid = 1U;
        *clear_room = 1;
        break;
    case ISAAC_VITA_STAGE_MEMORY_LEVEL_END:
        if (!no_result || !s_stage_memory.active ||
            s_stage_memory.room_open ||
            s_stage_memory.level_stage != stage ||
            s_stage_memory.level_type != mode_or_type)
            break;
        record->context_valid = 1U;
        *clear_level = 1;
        break;
    default:
        break;
    }
}

static void vita_stage_memory_clear_room_locked(void)
{
    s_stage_memory.room_open = 0U;
    s_stage_memory.room_after_unload = 0U;
}

static void vita_stage_memory_clear_level_locked(void)
{
    s_stage_memory.active = 0U;
    s_stage_memory.level_sequence = 0U;
    s_stage_memory.level_stage = UINT32_MAX;
    s_stage_memory.level_type = UINT32_MAX;
    s_stage_memory.room_sequence = 0U;
    s_stage_memory.room_stage = UINT32_MAX;
    s_stage_memory.room_mode = UINT32_MAX;
    vita_stage_memory_clear_room_locked();
}
#endif

void isaac_vita_stage_memory_note(uint32_t event, uint32_t stage,
                                  uint32_t mode_or_type, uint32_t result)
{
#if defined(__vita__) || defined(ISAAC_VITA_STAGE_MEMORY_ORACLE) || \
    defined(ISAAC_VITA_STAGE_MEMORY_RELEASE_ORACLE)
#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
    struct mallinfo heap;
#endif
    vita_stage_memory_record record;
#if defined(ISAAC_VITA_HEAP_RANGE_LEASE) && \
    defined(VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED)
    vita_floor_lifetime_record floor_record;
#endif
    int clear_level;
    int clear_room;

    memset(&record, 0, sizeof record);
    record.event = event;
    record.result = result;
    vita_heap_lock();
    record.sequence = vita_stage_memory_next_sequence_locked();
    vita_stage_memory_context_locked(
        event, stage, mode_or_type, result, &record,
        &clear_level, &clear_room);
    record.level_sequence = s_stage_memory.level_sequence;
    record.room_sequence = s_stage_memory.room_sequence;
    record.active = s_stage_memory.active;
    record.level_stage = s_stage_memory.level_stage;
    record.level_type = s_stage_memory.level_type;
    record.room_stage = s_stage_memory.room_stage;
    record.room_mode = s_stage_memory.room_mode;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    vita_floor_lifetime_stage_event_locked(
        event, record.context_valid, record.active);
#endif
#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
    vita_stage_memory_snapshot_locked(&record.memory);
#if defined(ISAAC_VITA_HEAP_RANGE_LEASE)
    vita_floor_lifetime_record_locked(&floor_record);
#endif
#endif
    if (clear_room)
        vita_stage_memory_clear_room_locked();
    if (clear_level)
        vita_stage_memory_clear_level_locked();
    vita_heap_unlock();

#ifdef VITA_STAGE_MEMORY_DIAGNOSTICS_ENABLED
    /* The floor boundary and its exact ownership snapshots happen first.
     * mallinfo may walk fragmented newlib bins, so this post-boundary sample
     * stays outside the guest lock and is only a non-transactional near point. */
    heap = VITA_STAGE_MEMORY_MALLINFO();
    vita_stage_memory_heap_near_point(&heap, &record.memory);

    /* mi=arena/uordblks/fordblks/ordblks;
     * ov=owned-live/owned-requested/raw-internal-live/raw-internal-requested;
     * slab=pages/raw-pages/external-pages/live/allocations/frees.
     * Worst-case hexadecimal expansion is 328 bytes, below the logger's
     * 384-byte body limit.  Formatting and attempted synchronous file/console
     * I/O are after unlock; this diagnostic-only path is not assumed
     * stutter-free. */
    isaac_vita_log(
        "stagemem: q=%x lq/rq=%x/%x e=%s ctx_valid=%x active=%x "
        "ls=%x lt=%x rs=%x mode=%x ok=%x mi=%x/%x/%x/%x ledger=%x "
        "ov=%x/%x/%x/%x slab=%x/%x/%x/%x/%x/%x "
        "valid/term/sat=%x/%x/%x",
        (unsigned)record.sequence, (unsigned)record.level_sequence,
        (unsigned)record.room_sequence,
        vita_stage_memory_event_name(record.event),
        (unsigned)record.context_valid, (unsigned)record.active,
        (unsigned)record.level_stage, (unsigned)record.level_type,
        (unsigned)record.room_stage, (unsigned)record.room_mode,
        (unsigned)record.result, (unsigned)record.memory.arena,
        (unsigned)record.memory.uordblks,
        (unsigned)record.memory.fordblks,
        (unsigned)record.memory.ordblks,
        (unsigned)record.memory.ledger_count,
        (unsigned)record.memory.overflow_live,
        (unsigned)record.memory.overflow_requested,
        (unsigned)record.memory.raw_internal_live,
        (unsigned)record.memory.raw_internal_requested,
        (unsigned)record.memory.slab_pages,
        (unsigned)record.memory.slab_raw_pages,
        (unsigned)record.memory.slab_external_pages,
        (unsigned)record.memory.slab_live,
        (unsigned)record.memory.slab_allocations,
        (unsigned)record.memory.slab_frees,
        (unsigned)record.memory.valid,
        (unsigned)record.memory.terminal,
        (unsigned)record.memory.counter_saturated);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    /* Heap quartets are total/unscoped/prior/current, first counts then
     * requested bytes.  hphase and sphase split CURRENT only, in
     * level-init/room-load/play order.  The 356-byte worst case remains below
     * the logger's 384-byte body limit. */
    isaac_vita_log(
        "floorlife: q=%x e=%s epoch=%x boot/roll=%x/%x phase=%s "
        "heap=%x/%x/%x/%x,%x/%x/%x/%x "
        "hphase=%x/%x/%x,%x/%x/%x slab=%x/%x/%x/%x "
        "sphase=%x/%x/%x valid/term/sat=%x/%x/%x",
        (unsigned)record.sequence,
        vita_stage_memory_event_name(record.event),
        (unsigned)floor_record.heap.epoch,
        (unsigned)floor_record.heap.bootstrap_count,
        (unsigned)floor_record.heap.rollover_count,
        vita_floor_lifetime_phase_name(floor_record.heap.phase),
        (unsigned)floor_record.heap.total_count,
        (unsigned)floor_record.heap.unscoped_count,
        (unsigned)floor_record.heap.prior_count,
        (unsigned)floor_record.heap.current_count,
        (unsigned)floor_record.heap.total_requested_bytes,
        (unsigned)floor_record.heap.unscoped_requested_bytes,
        (unsigned)floor_record.heap.prior_requested_bytes,
        (unsigned)floor_record.heap.current_requested_bytes,
        (unsigned)floor_record.heap.phase_count[0],
        (unsigned)floor_record.heap.phase_count[1],
        (unsigned)floor_record.heap.phase_count[2],
        (unsigned)floor_record.heap.phase_requested_bytes[0],
        (unsigned)floor_record.heap.phase_requested_bytes[1],
        (unsigned)floor_record.heap.phase_requested_bytes[2],
        (unsigned)floor_record.slab_total,
        (unsigned)floor_record.slab_unscoped,
        (unsigned)floor_record.slab_prior,
        (unsigned)floor_record.slab_current,
        (unsigned)floor_record.slab_phase[0],
        (unsigned)floor_record.slab_phase[1],
        (unsigned)floor_record.slab_phase[2],
        (unsigned)floor_record.valid,
        (unsigned)floor_record.terminal,
        (unsigned)floor_record.counter_saturated);
#endif
#endif
#else
    (void)event;
    (void)stage;
    (void)mode_or_type;
    (void)result;
#endif
}

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
int isaac_vita_guest_heap_floor_lifetime_snapshot_get(
    isaac_vita_guest_heap_floor_lifetime_snapshot *snapshot_out)
{
    int valid;

    if (!snapshot_out)
        return 0;
    vita_heap_lock();
    vita_heap_floor_lifetime_snapshot_locked(snapshot_out);
    valid = snapshot_out->valid != 0U;
    vita_heap_unlock();
    return valid;
}
#endif

#ifdef ISAAC_VITA_HEAP_TESTING
void isaac_vita_stage_memory_test_reset(void)
{
#if defined(__vita__) || defined(ISAAC_VITA_STAGE_MEMORY_ORACLE) || \
    defined(ISAAC_VITA_STAGE_MEMORY_RELEASE_ORACLE)
    static const vita_stage_memory_state initial =
        VITA_STAGE_MEMORY_STATE_INITIALIZER;

    vita_heap_lock();
    s_stage_memory = initial;
    vita_heap_unlock();
#endif
}
#endif

#ifdef ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC
static int vita_heap_diagnostic_snapshot(
    size_t *live_requested, size_t *largest_live_request,
    size_t *live_count)
{
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    size_t requested_sum = 0U;
    size_t largest = 0U;
    size_t counted = 0U;
    size_t index;
    int exact = 1;

    vita_heap_lock();
    for (index = 0U; index < s_ledger_capacity; ++index) {
        size_t requested;

        if (s_ledger[index].base <= VITA_HEAP_LEDGER_TOMBSTONE)
            continue;
        requested = s_ledger[index].requested_size;
        if (requested > SIZE_MAX - requested_sum) {
            exact = 0;
            break;
        }
        requested_sum += requested;
        if (requested > largest)
            largest = requested;
        ++counted;
    }
    if (counted != s_ledger_count)
        exact = 0;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_snapshot room;
        size_t room_bytes;

        if (!isaac_vita_room_entry_slab_snapshot_locked(&room) ||
            room.terminal || room.counter_saturated ||
            room.live_slots > SIZE_MAX /
                ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES ||
            room.live_slots > SIZE_MAX - counted) {
            exact = 0;
        }
        else {
            room_bytes = (size_t)room.live_slots *
                ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES;
            if (room_bytes > SIZE_MAX - requested_sum)
                exact = 0;
            else {
                requested_sum += room_bytes;
                counted += room.live_slots;
                if (room.live_slots && largest <
                        ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES)
                    largest = ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES;
            }
        }
    }
#endif
    *live_requested = requested_sum;
    *largest_live_request = largest;
    *live_count = counted;
    vita_heap_unlock();
    return exact;
#else
    vita_heap_lock();
    *live_requested = 0U;
    *largest_live_request = 0U;
    *live_count = s_ledger_count;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_snapshot room;

        if (isaac_vita_room_entry_slab_snapshot_locked(&room) &&
            room.live_slots <= SIZE_MAX - *live_count)
            *live_count += room.live_slots;
    }
#endif
    vita_heap_unlock();
    return 0;
#endif
}

static void vita_heap_failure_ledger_snapshot(
    size_t *capacity, size_t *count, size_t *tombstones,
    size_t *rehash_target_bytes, size_t *room_live)
{
    vita_heap_lock();
    *capacity = s_ledger_capacity;
    *count = s_ledger_count;
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    *tombstones = 0U;
#else
    *tombstones = s_ledger_tombstones;
#endif
    *rehash_target_bytes =
        vita_heap_ledger_rehash_target_bytes_locked(1U);
    *room_live = 0U;
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_snapshot room;

        if (isaac_vita_room_entry_slab_snapshot_locked(&room))
            *room_live = room.live_slots;
        else
            *room_live = SIZE_MAX;
    }
#endif
    vita_heap_unlock();
}
#endif

#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXEL_SCRATCH) || \
    defined(ISAAC_VITA_OGG_EMERGENCY) || \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB)
static int vita_heap_read_stack_word(const CPU *c, uint32_t address,
                                     uint32_t *value)
{
    if (!value || !guest_stack_contains(c, address, 4U))
        return 0;
    *value = ld32(address);
    return 1;
}
#endif

#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXEL_SCRATCH)

static int vita_heap_is_known_texel_loader(uint32_t owner_return_rva)
{
    switch (owner_return_rva) {
    case KAGE_VITA_TEXEL_LOADER_RETURN_0:
    case KAGE_VITA_TEXEL_LOADER_RETURN_1:
    case KAGE_VITA_TEXEL_LOADER_RETURN_2:
    case KAGE_VITA_TEXEL_LOADER_RETURN_3:
    case KAGE_VITA_TEXEL_LOADER_RETURN_4:
    case KAGE_VITA_TEXEL_LOADER_RETURN_5:
        return 1;
    default:
        return 0;
    }
}

static uint32_t vita_heap_texel_owner_return_rva(
    const CPU *c, unsigned *chain_depth)
{
    uint32_t import_return_rva;
    uint32_t wrapper_return_rva;
    uint32_t wrapper_frame;
    uint32_t owner_return_rva;

    if (chain_depth)
        *chain_depth = 0U;
    if (!vita_heap_read_stack_word(c, c->esp, &import_return_rva) ||
        import_return_rva != KAGE_VITA_TEXEL_MALLOC_IAT_RETURN_RVA ||
        !vita_heap_read_stack_word(
            c, c->ebp + 4U, &wrapper_return_rva) ||
        wrapper_return_rva != KAGE_VITA_TEXEL_WRAPPER_RETURN_RVA ||
        !vita_heap_read_stack_word(c, c->ebp, &wrapper_frame) ||
        !vita_heap_read_stack_word(
            c, wrapper_frame + 4U, &owner_return_rva))
        return 0U;

    if (owner_return_rva == KAGE_VITA_TEXEL_ADAPTER_RETURN_RVA) {
        uint32_t adapter_frame;

        if (!vita_heap_read_stack_word(c, wrapper_frame, &adapter_frame) ||
            !vita_heap_read_stack_word(
                c, adapter_frame + 4U, &owner_return_rva))
            return 0U;
        if (chain_depth)
            *chain_depth = 3U;
    }
    else if (chain_depth) {
        *chain_depth = 2U;
    }
    if (!vita_heap_is_known_texel_loader(owner_return_rva)) {
        if (chain_depth)
            *chain_depth = 0U;
        return 0U;
    }
    return owner_return_rva;
}
#endif

#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_OGG_EMERGENCY) || \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB)
static uint32_t vita_heap_operator_new_owner_return_rva(const CPU *c)
{
    uint32_t allocator_return_rva;
    uint32_t owner_return_rva;

    /* At the common operator-new malloc import, [ESP] is the exact wrapper
     * return and [ESP+12] is the original caller return.  Arbitrary direct
     * malloc users must never inherit an operator-new owner. */
    if (!vita_heap_read_stack_word(c, c->esp, &allocator_return_rva) ||
        allocator_return_rva !=
            ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA ||
        !vita_heap_read_stack_word(
            c, c->esp + 12U, &owner_return_rva))
        return 0U;
    return owner_return_rva;
}
#endif

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
static uint32_t vita_heap_import_return_rva(const CPU *c)
{
    uint32_t return_rva = 0U;

    (void)vita_heap_read_stack_word(c, c->esp, &return_rva);
    return return_rva;
}

static void vita_heap_room_slab_unlock(
    isaac_vita_room_entry_slab_result slab_result,
    isaac_vita_room_entry_slab_event *slab_event,
    vita_heap_telemetry_event *heap_event)
{
    if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL)
        vita_heap_router_result_set(
            NULL, VITA_HEAP_ROUTER_TERMINAL, heap_event);
    vita_heap_unlock_with_telemetry(heap_event);
    isaac_vita_room_entry_slab_log_event(slab_event);
}
#endif

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
#if defined(ISAAC_VITA_PNG_NATIVE_UNFILTER) || \
    defined(ISAAC_VITA_PNG_INFLATE_FLUSH_FASTPATH) || \
    defined(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)
static size_t vita_heap_free_lease_slot(void)
{
    size_t index;

    for (index = 0U; index < VITA_HEAP_LEASE_CAPACITY; ++index)
        if (!s_heap_leases[index].token)
            return index;
    return SIZE_MAX;
}

uint32_t isaac_vita_guest_heap_lease_exact_range(
    const void *allocation_pointer, const void *range, size_t size)
{
    const uintptr_t allocation_base = (uintptr_t)allocation_pointer;
    const uintptr_t first = (uintptr_t)range;
    size_t free_lease;
    size_t offset;
    size_t requested_size;
    size_t slot;
    uint32_t token;

    if (!allocation_pointer || !range || !size ||
            first < allocation_base ||
            size - 1U > UINTPTR_MAX - first)
        return 0U;

    vita_heap_lock();
    free_lease = vita_heap_free_lease_slot();
    if (!s_heap_next_lease_token || free_lease == SIZE_MAX ||
            !vita_heap_ledger_lookup(allocation_base, &slot) ||
            s_ledger[slot].base != allocation_base ||
            vita_heap_base_is_leased(allocation_base)) {
        vita_heap_unlock();
        return 0U;
    }
    offset = (size_t)(first - allocation_base);
    requested_size = s_ledger[slot].requested_size;
    if (offset > requested_size || size > requested_size - offset) {
        vita_heap_unlock();
        return 0U;
    }

    token = s_heap_next_lease_token;
    if (s_heap_next_lease_token == UINT32_MAX)
        s_heap_next_lease_token = 0U;
    else
        ++s_heap_next_lease_token;
    s_heap_leases[free_lease].base = allocation_base;
    s_heap_leases[free_lease].token = token;
    ++s_heap_lease_count;
    vita_heap_unlock();
    return token;
}
#endif

uint32_t isaac_vita_guest_heap_lease_containing(
    const void *range, size_t size, uintptr_t *allocation_base_out)
{
    uintptr_t first = (uintptr_t)range;
    uintptr_t matched_base = 0U;
    size_t free_lease = SIZE_MAX;
    size_t index;
    uint32_t token;

    if (allocation_base_out)
        *allocation_base_out = 0U;
    if (!range || !size || !allocation_base_out ||
        size - 1U > UINTPTR_MAX - first)
        return 0U;

    vita_heap_lock();
    if (!s_heap_next_lease_token ||
        s_heap_lease_count == VITA_HEAP_LEASE_CAPACITY) {
        vita_heap_unlock();
        return 0U;
    }
    for (index = 0U; index < VITA_HEAP_LEASE_CAPACITY; ++index)
        if (!s_heap_leases[index].token) {
            free_lease = index;
            break;
        }
    if (free_lease == SIZE_MAX) {
        vita_heap_unlock();
        return 0U;
    }

    /* Scan exact requested extents, not allocator padding.  The ledger lock
     * linearizes this scan and lease install against guest free/realloc/rehash. */
    for (index = 0U; index < s_ledger_capacity; ++index) {
        uintptr_t base = s_ledger[index].base;
        size_t offset;
        size_t requested_size;

        if (base <= VITA_HEAP_LEDGER_TOMBSTONE || first < base)
            continue;
        offset = (size_t)(first - base);
        requested_size = s_ledger[index].requested_size;
        if (offset > requested_size || size > requested_size - offset)
            continue;
        if (matched_base) {
            /* Native live allocations cannot overlap.  Treat any corrupted
             * ledger/allocator answer as ambiguity rather than guessing. */
            vita_heap_unlock();
            return 0U;
        }
        matched_base = base;
    }
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        uintptr_t slab_base = 0U;

        if (isaac_vita_room_entry_slab_find_containing_locked(
                range, size, &slab_base)) {
            if (matched_base) {
                /* The ledger and slab are disjoint.  Refuse a corrupted
                 * overlapping ownership answer instead of guessing. */
                vita_heap_unlock();
                return 0U;
            }
            matched_base = slab_base;
        }
    }
#endif
    if (!matched_base || vita_heap_base_is_leased(matched_base)) {
        vita_heap_unlock();
        return 0U;
    }

    token = s_heap_next_lease_token;
    if (s_heap_next_lease_token == UINT32_MAX)
        s_heap_next_lease_token = 0U; /* permanent fail-closed exhaustion */
    else
        ++s_heap_next_lease_token;
    s_heap_leases[free_lease].base = matched_base;
    s_heap_leases[free_lease].token = token;
    ++s_heap_lease_count;
    *allocation_base_out = matched_base;
    vita_heap_unlock();
    return token;
}

int isaac_vita_guest_heap_lease_release(uint32_t token)
{
    size_t index;

    if (!token)
        return 0;
    vita_heap_lock();
    for (index = 0U; index < VITA_HEAP_LEASE_CAPACITY; ++index) {
        if (s_heap_leases[index].token == token) {
            s_heap_leases[index].base = 0U;
            s_heap_leases[index].token = 0U;
            --s_heap_lease_count;
            vita_heap_unlock();
            return 1;
        }
    }
    vita_heap_unlock();
    return 0;
}
#endif

#ifdef ISAAC_VITA_HEAP_TESTING
void isaac_vita_guest_heap_test_probe_stats_reset(void)
{
    vita_heap_lock();
    /* Exclude the reset operation itself from the following receipt. */
    memset(&s_heap_test_probe_stats, 0, sizeof s_heap_test_probe_stats);
    vita_heap_unlock();
}

int isaac_vita_guest_heap_test_probe_stats_snapshot(
    isaac_vita_guest_heap_test_probe_stats *stats_out)
{
    if (!stats_out)
        return 0;
    vita_heap_lock();
    /* The snapshot's own acquisition is intentionally visible.  This keeps
     * the counters synchronized without a test-only unlocked read. */
    *stats_out = s_heap_test_probe_stats;
    vita_heap_unlock();
    return 1;
}

size_t isaac_vita_guest_heap_test_capacity(void)
{
    size_t result;
    vita_heap_lock();
    result = s_ledger_capacity;
    vita_heap_unlock();
    return result;
}

size_t isaac_vita_guest_heap_test_live_count(void)
{
    size_t result;
    vita_heap_lock();
    result = s_ledger_count;
    vita_heap_unlock();
    return result;
}

size_t isaac_vita_guest_heap_test_realloc_moves(void)
{
    size_t result;
    vita_heap_lock();
    result = s_realloc_moves;
    vita_heap_unlock();
    return result;
}

size_t isaac_vita_guest_heap_test_next_rehash_bytes(void)
{
    size_t result;

    vita_heap_lock();
    result = vita_heap_ledger_rehash_target_bytes_locked(1U);
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_test_backshift_enabled(void)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    return 1;
#else
    return 0;
#endif
}

#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
_Static_assert(6U *
                       ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR ==
                   8U *
                       ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR &&
                   48U *
                       ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR ==
                   64U *
                       ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR,
               "backshift oracle load no longer matches fixed 3/4 ceiling");
#endif
static uintptr_t vita_heap_test_key_for_home(
    size_t home, size_t capacity, uintptr_t first)
{
    uintptr_t key = first > VITA_HEAP_LEDGER_TOMBSTONE
        ? first : VITA_HEAP_LEDGER_TOMBSTONE + 1U;
    size_t attempts;

    for (attempts = 0U; attempts < 0x100000U; ++attempts, ++key) {
        if (key <= VITA_HEAP_LEDGER_TOMBSTONE)
            key = VITA_HEAP_LEDGER_TOMBSTONE + 1U;
        if ((vita_heap_hash(key) & (capacity - 1U)) == home)
            return key;
    }
    return 0U;
}

#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
static int vita_heap_test_insert_local(
    uintptr_t *table, size_t capacity, uintptr_t key)
{
    int found;
    size_t slot = vita_heap_find_slot(table, capacity, key, &found);

    if (found || slot == SIZE_MAX)
        return 0;
    table[slot] = key;
    return 1;
}

static int vita_heap_test_verify_local(
    const uintptr_t *table, size_t capacity, const uintptr_t *keys,
    const unsigned char *live, size_t key_count)
{
    size_t index;

    for (index = 0U; index < capacity; ++index)
        if (table[index] == VITA_HEAP_LEDGER_TOMBSTONE)
            return 0;
    for (index = 0U; index < key_count; ++index) {
        int found;
        (void)vita_heap_find_slot(table, capacity, keys[index], &found);
        if (!!found != !!live[index])
            return 0;
    }
    return 1;
}

static int vita_heap_test_collision_cluster(size_t home)
{
    uintptr_t table[8];
    uintptr_t keys[6];
    unsigned char live[6];
    size_t removed;
    size_t index;
    uintptr_t next = 2U;

    for (index = 0U; index < 6U; ++index) {
        keys[index] = vita_heap_test_key_for_home(home, 8U, next);
        if (!keys[index])
            return 0;
        next = keys[index] + 1U;
    }
    for (removed = 0U; removed < 6U; ++removed) {
        memset(table, 0, sizeof table);
        memset(live, 1, sizeof live);
        for (index = 0U; index < 6U; ++index)
            if (!vita_heap_test_insert_local(table, 8U, keys[index]))
                return 0;
        {
            int found;
            size_t slot = vita_heap_find_slot(
                table, 8U, keys[removed], &found);
            if (!found ||
                !vita_heap_ledger_remove_preflight_table(table, 8U, slot))
                return 0;
            vita_heap_ledger_remove_commit_table(table, 8U, slot);
        }
        live[removed] = 0U;
        if (!vita_heap_test_verify_local(table, 8U, keys, live, 6U))
            return 0;
    }
    return 1;
}

static int vita_heap_test_random_model(void)
{
    enum { CAPACITY = 64, KEY_COUNT = 56, LIVE_LIMIT = 48 };
    uintptr_t table[CAPACITY];
    uintptr_t keys[KEY_COUNT];
    unsigned char live[KEY_COUNT];
    uint32_t random = UINT32_C(0x73c5a91d);
    size_t live_count = 0U;
    size_t iteration;
    size_t index;

    memset(table, 0, sizeof table);
    memset(live, 0, sizeof live);
    for (index = 0U; index < KEY_COUNT; ++index)
        keys[index] = (uintptr_t)(0x1003U + index * 0x101U);
    for (index = 0U; index < LIVE_LIMIT; ++index) {
        if (!vita_heap_test_insert_local(table, CAPACITY, keys[index]))
            return 0;
        live[index] = 1U;
        ++live_count;
    }
    if (!vita_heap_test_verify_local(
            table, CAPACITY, keys, live, KEY_COUNT))
        return 0;
    for (iteration = 0U; iteration < 12000U; ++iteration) {
        int found;
        size_t slot;

        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        index = (size_t)(random % KEY_COUNT);
        slot = vita_heap_find_slot(table, CAPACITY, keys[index], &found);
        if (live[index]) {
            if (!found ||
                !vita_heap_ledger_remove_preflight_table(
                    table, CAPACITY, slot))
                return 0;
            vita_heap_ledger_remove_commit_table(table, CAPACITY, slot);
            live[index] = 0U;
            --live_count;
        }
        else if (live_count < LIVE_LIMIT) {
            if (found || slot == SIZE_MAX)
                return 0;
            table[slot] = keys[index];
            live[index] = 1U;
            ++live_count;
        }
        if ((iteration & 31U) == 0U &&
            !vita_heap_test_verify_local(
                table, CAPACITY, keys, live, KEY_COUNT))
            return 0;
    }
    return vita_heap_test_verify_local(
        table, CAPACITY, keys, live, KEY_COUNT);
}
#else
static int vita_heap_test_insert_local(
    vita_heap_ledger_entry *table, size_t capacity, uintptr_t key,
    size_t requested_size)
{
    int found;
    size_t slot = vita_heap_find_slot(table, capacity, key, &found);

    if (found || slot == SIZE_MAX)
        return 0;
    table[slot].base = key;
    table[slot].requested_size = requested_size;
    return 1;
}

static int vita_heap_test_verify_local(
    const vita_heap_ledger_entry *table, size_t capacity,
    const uintptr_t *keys, const size_t *requested_sizes,
    const unsigned char *live, size_t key_count)
{
    size_t index;

    for (index = 0U; index < capacity; ++index)
        if (table[index].base == VITA_HEAP_LEDGER_TOMBSTONE)
            return 0;
    for (index = 0U; index < key_count; ++index) {
        int found;
        size_t slot = vita_heap_find_slot(
            table, capacity, keys[index], &found);
        if (!!found != !!live[index])
            return 0;
        if (found && table[slot].requested_size != requested_sizes[index])
            return 0;
    }
    return 1;
}

static int vita_heap_test_collision_cluster(size_t home)
{
    vita_heap_ledger_entry table[8];
    uintptr_t keys[6];
    size_t requested_sizes[6];
    unsigned char live[6];
    size_t removed;
    size_t index;
    uintptr_t next = 2U;

    for (index = 0U; index < 6U; ++index) {
        keys[index] = vita_heap_test_key_for_home(home, 8U, next);
        requested_sizes[index] = 0x400U + index * 37U;
        if (!keys[index])
            return 0;
        next = keys[index] + 1U;
    }
    for (removed = 0U; removed < 6U; ++removed) {
        memset(table, 0, sizeof table);
        memset(live, 1, sizeof live);
        for (index = 0U; index < 6U; ++index)
            if (!vita_heap_test_insert_local(
                    table, 8U, keys[index], requested_sizes[index]))
                return 0;
        {
            int found;
            size_t slot = vita_heap_find_slot(
                table, 8U, keys[removed], &found);
            if (!found ||
                !vita_heap_ledger_remove_preflight_table(table, 8U, slot))
                return 0;
            vita_heap_ledger_remove_commit_table(
                table, 8U, slot, NULL, NULL);
        }
        live[removed] = 0U;
        if (!vita_heap_test_verify_local(
                table, 8U, keys, requested_sizes, live, 6U))
            return 0;
    }
    return 1;
}

static int vita_heap_test_random_model(void)
{
    enum { CAPACITY = 64, KEY_COUNT = 56, LIVE_LIMIT = 48 };
    vita_heap_ledger_entry table[CAPACITY];
    uintptr_t keys[KEY_COUNT];
    size_t requested_sizes[KEY_COUNT];
    unsigned char live[KEY_COUNT];
    uint32_t random = UINT32_C(0x73c5a91d);
    size_t live_count = 0U;
    size_t iteration;
    size_t index;

    memset(table, 0, sizeof table);
    memset(live, 0, sizeof live);
    for (index = 0U; index < KEY_COUNT; ++index) {
        keys[index] = (uintptr_t)(0x1003U + index * 0x101U);
        requested_sizes[index] = 0x900U + index * 19U;
    }
    for (index = 0U; index < LIVE_LIMIT; ++index) {
        if (!vita_heap_test_insert_local(
                table, CAPACITY, keys[index], requested_sizes[index]))
            return 0;
        live[index] = 1U;
        ++live_count;
    }
    if (!vita_heap_test_verify_local(
            table, CAPACITY, keys, requested_sizes, live, KEY_COUNT))
        return 0;
    for (iteration = 0U; iteration < 12000U; ++iteration) {
        int found;
        size_t slot;

        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        index = (size_t)(random % KEY_COUNT);
        slot = vita_heap_find_slot(table, CAPACITY, keys[index], &found);
        if (live[index]) {
            if (!found ||
                !vita_heap_ledger_remove_preflight_table(
                    table, CAPACITY, slot))
                return 0;
            vita_heap_ledger_remove_commit_table(
                table, CAPACITY, slot, NULL, NULL);
            live[index] = 0U;
            --live_count;
        }
        else if (live_count < LIVE_LIMIT) {
            if (found || slot == SIZE_MAX)
                return 0;
            table[slot].base = keys[index];
            table[slot].requested_size = requested_sizes[index];
            live[index] = 1U;
            ++live_count;
        }
        if ((iteration & 31U) == 0U &&
            !vita_heap_test_verify_local(
                table, CAPACITY, keys, requested_sizes, live, KEY_COUNT))
            return 0;
    }
    return vita_heap_test_verify_local(
        table, CAPACITY, keys, requested_sizes, live, KEY_COUNT);
}
#endif
#endif

int isaac_vita_guest_heap_test_backshift_oracle(void)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    /* Six of eight slots is the production 3/4 load.  Home 2 exercises a
     * normal cluster; home 6 wraps through 7 and 0..3.  Removing every
     * possible head/middle/tail member checks all positions. */
    return vita_heap_test_collision_cluster(2U) &&
        vita_heap_test_collision_cluster(6U) &&
        vita_heap_test_random_model();
#else
    return 1;
#endif
}

int isaac_vita_guest_heap_test_corrupt_no_empty(void)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    const uintptr_t filler = VITA_HEAP_LEDGER_TOMBSTONE + 1U;
    size_t index;

    vita_heap_lock();
    if (!s_ledger_capacity || !s_ledger_count) {
        vita_heap_unlock();
        return 0;
    }
    for (index = 0U; index < s_ledger_capacity; ++index) {
#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
        if (s_ledger[index] == 0U)
            s_ledger[index] = filler;
#else
        if (s_ledger[index].base == 0U)
            s_ledger[index].base = filler;
#endif
    }
    vita_heap_unlock();
    return 1;
#else
    return 0;
#endif
}

int isaac_vita_guest_heap_test_restore_empty(void)
{
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    const uintptr_t filler = VITA_HEAP_LEDGER_TOMBSTONE + 1U;
    size_t index;

    vita_heap_lock();
    if (!s_ledger_capacity) {
        vita_heap_unlock();
        return 0;
    }
    for (index = 0U; index < s_ledger_capacity; ++index) {
#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
        if (s_ledger[index] == filler)
            s_ledger[index] = 0U;
#else
        if (s_ledger[index].base == filler) {
            s_ledger[index].base = 0U;
            s_ledger[index].requested_size = 0U;
        }
#endif
    }
    vita_heap_unlock();
    return 1;
#else
    return 0;
#endif
}

#ifdef ISAAC_VITA_HEAP_LEDGER_MEMBLOCK
int isaac_vita_guest_heap_test_storage_snapshot(
    isaac_vita_guest_heap_test_storage_state *state)
{
    const unsigned char *bytes;
    size_t byte_count;
    size_t index;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!state)
        return 0;
    vita_heap_lock();
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    {
        vita_heap_ledger_layout layout;

        if (s_ledger_capacity &&
            vita_heap_ledger_combined_layout(s_ledger_capacity, &layout))
            byte_count = layout.combined_bytes;
        else
            byte_count = 0U;
    }
#else
    byte_count = s_ledger_capacity * sizeof *s_ledger;
#endif
    bytes = (const unsigned char *)s_ledger;
    for (index = 0U; index < byte_count; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    state->capacity = s_ledger_capacity;
    state->live_count = s_ledger_count;
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    state->tombstones = 0U;
#else
    state->tombstones = s_ledger_tombstones;
#endif
    state->usable_bytes = s_ledger_storage.usable_bytes;
    state->block_bytes = s_ledger_storage.block_bytes;
    state->uid = s_ledger_storage.uid;
    state->orphan_uid = s_ledger_orphan.uid;
    state->poisoned = s_ledger_storage_poisoned;
    state->table_hash = hash;
    vita_heap_unlock();
    return 1;
}

int isaac_vita_guest_heap_test_storage_reset(void)
{
    int result = 0;

    vita_heap_lock();
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    if (s_ledger_count || s_heap_lease_count) {
#else
    if (s_ledger_count) {
#endif
        vita_heap_unlock();
        return 0;
    }
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING
#if defined(ISAAC_VITA_ROOM_ENTRY_SLAB) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
    if (!isaac_vita_room_entry_slab_test_reset_locked()) {
        vita_heap_unlock();
        return 0;
    }
#endif
    if (!isaac_vita_heap_overflow_mspace_test_reset()) {
        vita_heap_unlock();
        return 0;
    }
#else
    if (s_heap_init_state != VITA_HEAP_INIT_UNINITIALIZED) {
        vita_heap_unlock();
        return 0;
    }
#endif
#endif
    if (s_ledger_orphan.uid >= 0) {
        result = isaac_vita_heap_ledger_memblock_release(&s_ledger_orphan);
        if (result < 0) {
            vita_heap_unlock();
            return 0;
        }
        vita_heap_ledger_storage_clear(&s_ledger_orphan);
    }
    if (s_ledger_storage.uid >= 0) {
        result = isaac_vita_heap_ledger_memblock_release(&s_ledger_storage);
        if (result < 0) {
            vita_heap_unlock();
            return 0;
        }
        vita_heap_ledger_storage_clear(&s_ledger_storage);
    }
    s_ledger = NULL;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    {
        static const vita_heap_floor_lifetime_state floor_initial =
            VITA_HEAP_FLOOR_LIFETIME_STATE_INITIALIZER;

        s_ledger_floor_phase = NULL;
        s_ledger_floor_current = NULL;
        s_floor_lifetime = floor_initial;
    }
#endif
    s_ledger_capacity = 0U;
    s_ledger_count = 0U;
    s_ledger_tombstones = 0U;
    s_ledger_fixed_capacity = 0;
    s_ledger_storage_poisoned = 0;
    s_realloc_moves = 0U;
#if defined(ISAAC_VITA_ROOM_ENTRY_SLAB) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
    memset(&s_heap_test_slab_move_floor_receipt, 0,
           sizeof s_heap_test_slab_move_floor_receipt);
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    s_heap_init_state = VITA_HEAP_INIT_UNINITIALIZED;
    s_heap_terminal = 0;
    s_heap_test_fail_raw_free = 0U;
    memset(&s_heap_telemetry, 0, sizeof s_heap_telemetry);
#endif
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    memset(s_heap_leases, 0, sizeof s_heap_leases);
    s_heap_lease_count = 0U;
    s_heap_next_lease_token = 1U;
#endif
    vita_heap_unlock();
    return 1;
}
#endif

int isaac_vita_guest_heap_test_lock_held(void)
{
    return s_heap_test_lock_held;
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
void isaac_vita_guest_heap_test_fail_next_raw_free(void)
{
    vita_heap_lock();
    ++s_heap_test_fail_raw_free;
    vita_heap_unlock();
}
#endif

#if defined(ISAAC_VITA_ROOM_ENTRY_SLAB) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB_TESTING)
void isaac_vita_guest_heap_test_room_fail_next_commit(void)
{
    vita_heap_lock();
    isaac_vita_room_entry_slab_test_fail_next_commit_locked();
    vita_heap_unlock();
}

void isaac_vita_guest_heap_test_room_corrupt_nonfull(void)
{
    vita_heap_lock();
    isaac_vita_room_entry_slab_test_corrupt_nonfull_locked();
    vita_heap_unlock();
}

int isaac_vita_guest_heap_test_room_snapshot(
    isaac_vita_room_entry_slab_snapshot *snapshot_out)
{
    int result;

    vita_heap_lock();
    result = isaac_vita_room_entry_slab_snapshot_locked(snapshot_out);
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_test_room_floor_lifetime_snapshot(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot_out)
{
    int result;

    vita_heap_lock();
    result = isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(
        snapshot_out);
    vita_heap_unlock();
    return result;
}

void isaac_vita_guest_heap_test_slab_move_floor_receipt_reset(void)
{
    vita_heap_lock();
    memset(&s_heap_test_slab_move_floor_receipt, 0,
           sizeof s_heap_test_slab_move_floor_receipt);
    vita_heap_unlock();
}

int isaac_vita_guest_heap_test_slab_move_floor_receipt_snapshot(
    isaac_vita_guest_heap_test_slab_move_floor_receipt *receipt_out)
{
    if (!receipt_out)
        return 0;
    vita_heap_lock();
    *receipt_out = s_heap_test_slab_move_floor_receipt;
    vita_heap_unlock();
    return 1;
}
#endif

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
size_t isaac_vita_guest_heap_test_active_leases(void)
{
    size_t result;
    vita_heap_lock();
    result = s_heap_lease_count;
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_test_floor_lifetime_bootstrap(uint32_t phase)
{
    int result;

    vita_heap_lock();
    result = vita_heap_floor_lifetime_bootstrap_locked(phase);
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_test_floor_lifetime_rollover(uint32_t phase)
{
    int result;

    vita_heap_lock();
    result = vita_heap_floor_lifetime_rollover_locked(phase);
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_test_floor_lifetime_phase_set(uint32_t phase)
{
    int result;

    vita_heap_lock();
    result = vita_heap_floor_lifetime_phase_set_locked(phase);
    vita_heap_unlock();
    return result;
}
#endif

#ifndef ISAAC_VITA_HEAP_RANGE_LEASE
int isaac_vita_guest_heap_test_bounded_probe(void)
{
    uintptr_t table[8];
    uintptr_t key = (uintptr_t)0xf00dbab5U;
    size_t index;
    size_t slot;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    size_t expected;
#endif
    int found;

    for (index = 0U; index < sizeof table / sizeof table[0]; ++index)
        table[index] = (uintptr_t)(0x100U + index * 4U);
    slot = vita_heap_find_slot(
        table, sizeof table / sizeof table[0], key, &found);
    if (found || slot != SIZE_MAX)
        return 0;

    for (index = 0U; index < sizeof table / sizeof table[0]; ++index)
        table[index] = VITA_HEAP_LEDGER_TOMBSTONE;
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    slot = vita_heap_find_slot(
        table, sizeof table / sizeof table[0], key, &found);
    return !found && slot == SIZE_MAX;
#else
    expected = vita_heap_hash(key) &
               (sizeof table / sizeof table[0] - 1U);
    slot = vita_heap_find_slot(
        table, sizeof table / sizeof table[0], key, &found);
    return !found && slot == expected;
#endif
}
#else
int isaac_vita_guest_heap_test_bounded_probe(void)
{
    vita_heap_ledger_entry table[8];
    uintptr_t key = (uintptr_t)0xf00dbab5U;
    size_t index;
    size_t slot;
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    size_t expected;
#endif
    int found;

    for (index = 0U; index < sizeof table / sizeof table[0]; ++index)
        table[index].base = (uintptr_t)(0x100U + index * 4U);
    slot = vita_heap_find_slot(
        table, sizeof table / sizeof table[0], key, &found);
    if (found || slot != SIZE_MAX)
        return 0;

    for (index = 0U; index < sizeof table / sizeof table[0]; ++index)
        table[index].base = VITA_HEAP_LEDGER_TOMBSTONE;
#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    slot = vita_heap_find_slot(
        table, sizeof table / sizeof table[0], key, &found);
    return !found && slot == SIZE_MAX;
#else
    expected = vita_heap_hash(key) &
               (sizeof table / sizeof table[0] - 1U);
    slot = vita_heap_find_slot(
        table, sizeof table / sizeof table[0], key, &found);
    return !found && slot == expected;
#endif
}
#endif

void *isaac_vita_guest_heap_test_force_move(void *pointer, size_t size,
                                             void **retired_pointer)
{
    int valid_owner = 0;

    if (retired_pointer)
        *retired_pointer = NULL;
    if (!pointer || !size || !retired_pointer)
        return NULL;
    return vita_heap_guest_realloc_impl(
        pointer, size, &valid_owner, 1, retired_pointer, NULL);
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
_Static_assert((int)VITA_HEAP_ROUTER_OK ==
                   (int)ISAAC_VITA_GUEST_HEAP_TEST_OK &&
                   (int)VITA_HEAP_ROUTER_OUT_OF_MEMORY ==
                   (int)ISAAC_VITA_GUEST_HEAP_TEST_OUT_OF_MEMORY &&
                   (int)VITA_HEAP_ROUTER_FOREIGN ==
                   (int)ISAAC_VITA_GUEST_HEAP_TEST_FOREIGN &&
                   (int)VITA_HEAP_ROUTER_TERMINAL ==
                   (int)ISAAC_VITA_GUEST_HEAP_TEST_TERMINAL,
               "test/public heap router status values drifted");

static void vita_heap_test_result_set(
    isaac_vita_guest_heap_test_result *result_out,
    vita_heap_router_result result)
{
    if (result_out)
        *result_out = (isaac_vita_guest_heap_test_result)result;
}

int isaac_vita_guest_heap_test_forbidden_ranges(
    uintptr_t link_end, size_t heap_bytes,
    uint32_t stack_floor, uint32_t stack_ceiling,
    isaac_vita_heap_overflow_forbidden_ranges *ranges_out)
{
    return vita_heap_build_forbidden_ranges(
        link_end, heap_bytes, stack_floor, stack_ceiling, ranges_out);
}

int isaac_vita_guest_heap_test_init(
    uintptr_t link_end, size_t heap_bytes,
    uint32_t stack_floor, uint32_t stack_ceiling)
{
    int result;

    vita_heap_lock();
    result = vita_heap_initialize_locked(
        link_end, heap_bytes, stack_floor, stack_ceiling);
    vita_heap_unlock();
    return result;
}

int isaac_vita_guest_heap_test_fixed_capacity(void)
{
    int result;

    vita_heap_lock();
    result = s_ledger_fixed_capacity;
    vita_heap_unlock();
    return result;
}

void *isaac_vita_guest_heap_test_malloc_ex(
    size_t size, isaac_vita_guest_heap_test_result *result_out,
    const char **failure_reason_out)
{
    vita_heap_router_result result;
    void *pointer = vita_heap_guest_malloc_with_reason(
        size, failure_reason_out, &result);

    vita_heap_test_result_set(result_out, result);
    return pointer;
}

void *isaac_vita_guest_heap_test_calloc_ex(
    size_t count, size_t size,
    isaac_vita_guest_heap_test_result *result_out)
{
    vita_heap_router_result result;
    void *pointer = vita_heap_guest_calloc_impl(count, size, &result);

    vita_heap_test_result_set(result_out, result);
    return pointer;
}

void *isaac_vita_guest_heap_test_realloc_ex(
    void *pointer, size_t size, int *valid_owner,
    isaac_vita_guest_heap_test_result *result_out)
{
    vita_heap_router_result result;
    void *replacement = vita_heap_guest_realloc_impl(
        pointer, size, valid_owner, 0, NULL, &result);

    vita_heap_test_result_set(result_out, result);
    return replacement;
}

int isaac_vita_guest_heap_test_free_ex(
    void *pointer, isaac_vita_guest_heap_test_result *result_out)
{
    vita_heap_router_result result;
    int freed = vita_heap_guest_free_impl(pointer, &result);

    vita_heap_test_result_set(result_out, result);
    return freed;
}
#endif
#endif

static uint32_t vita_heap_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_heap_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

static void vita_heap_callnewh(CPU *__restrict c)
{
    (void)vita_heap_arg(c, 0U);
    /* This PE imports no new-handler setter.  No guest callback can have
     * been installed, so the UCRT's honest answer is "do not retry". */
    c->eax = 0U;
    vita_heap_cdecl_return(c);
}

static void vita_heap_free(CPU *__restrict c)
{
    uint32_t pointer = vita_heap_arg(c, 0U);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (isaac_vita_guest_heap_terminal()) {
        guest_fault(c, pointer, "guest heap overflow domain is corrupted");
        return;
    }
#endif
#ifdef ISAAC_VITA_OGG_EMERGENCY
    {
        isaac_vita_ogg_emergency_decision emergency;
        int emergency_result = isaac_vita_ogg_emergency_free(
            (void *)(uintptr_t)pointer, &emergency);

        if (emergency_result == ISAAC_VITA_OGG_EMERGENCY_REJECTED) {
            guest_fault(c, emergency.fault_value, emergency.fault);
            return;
        }
        if (emergency_result == ISAAC_VITA_OGG_EMERGENCY_HANDLED) {
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
#ifdef ISAAC_VITA_ANM2_SCRATCH
    isaac_vita_anm2_scratch_decision scratch;
    int scratch_result = isaac_vita_anm2_scratch_free(
        (void *)(uintptr_t)pointer, &scratch);

    if (scratch_result == ISAAC_VITA_ANM2_SCRATCH_REJECTED) {
        guest_fault(c, scratch.fault_value, scratch.fault);
        return;
    }
    if (scratch_result == ISAAC_VITA_ANM2_SCRATCH_HANDLED) {
        vita_heap_cdecl_return(c);
        return;
    }
#endif
#ifdef ISAAC_VITA_TEXEL_SCRATCH
    {
        isaac_vita_texel_scratch_decision scratch;
        int scratch_result = isaac_vita_texel_scratch_free(
            (void *)(uintptr_t)pointer, &scratch);

        if (scratch_result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED) {
            guest_fault(c, scratch.fault_value, scratch.fault);
            return;
        }
        if (scratch_result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED) {
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_decision room;
        isaac_vita_room_entry_slab_result slab_result;
        uint32_t free_return_rva = vita_heap_import_return_rva(c);

        slab_result = vita_heap_room_slab_free_route(
            (void *)(uintptr_t)pointer, free_return_rva, &room);
        if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL ||
            isaac_vita_guest_heap_terminal()) {
            guest_fault(c, pointer, "room-entry slab is corrupted");
            return;
        }
        if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED) {
            guest_fault(c, room.fault_value, room.fault);
            return;
        }
        if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) {
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    {
        vita_heap_router_result router_result;
        int freed = vita_heap_guest_free_impl(
            (void *)(uintptr_t)pointer, &router_result);

        if (router_result == VITA_HEAP_ROUTER_TERMINAL ||
            isaac_vita_guest_heap_terminal()) {
            guest_fault(c, pointer, "guest heap overflow domain is corrupted");
            return;
        }
        if (!freed) {
            guest_fault(c, pointer, "free received a foreign guest pointer");
            return;
        }
    }
#else
    if (!isaac_vita_guest_free((void *)(uintptr_t)pointer)) {
        guest_fault(c, pointer, "free received a foreign guest pointer");
        return;
    }
#endif
    vita_heap_cdecl_return(c);
}

static void vita_heap_calloc(CPU *__restrict c)
{
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    vita_heap_router_result router_result;
    size_t count = (size_t)vita_heap_arg(c, 0U);
    size_t size = (size_t)vita_heap_arg(c, 1U);
    void *result;

    if (isaac_vita_guest_heap_terminal()) {
        guest_fault(c, (uint32_t)size,
                    "guest heap overflow domain is corrupted");
        return;
    }
    result = vita_heap_guest_calloc_impl(count, size, &router_result);

    if (router_result == VITA_HEAP_ROUTER_TERMINAL ||
        isaac_vita_guest_heap_terminal()) {
        guest_fault(c, vita_heap_arg(c, 1U),
                    "guest heap overflow domain is corrupted");
        return;
    }
    c->eax = (uint32_t)(uintptr_t)result;
#else
    c->eax = (uint32_t)(uintptr_t)isaac_vita_guest_calloc(
        (size_t)vita_heap_arg(c, 0U), (size_t)vita_heap_arg(c, 1U));
#endif
    vita_heap_cdecl_return(c);
}

static void vita_heap_malloc(CPU *__restrict c)
{
#if defined(ISAAC_VITA_ANM2_SCRATCH) || \
    defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXEL_SCRATCH) || \
    defined(ISAAC_VITA_OGG_EMERGENCY) || \
    defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
    size_t request = (size_t)vita_heap_arg(c, 0U);
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (isaac_vita_guest_heap_terminal()) {
        guest_fault(c, (uint32_t)request,
                    "guest heap overflow domain is corrupted");
        return;
    }
#endif
#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXEL_SCRATCH)
    unsigned owner_chain_depth = 0U;
    uint32_t owner_return_rva;
#endif
#ifdef ISAAC_VITA_ANM2_SCRATCH
    {
        isaac_vita_anm2_scratch_decision scratch;
        uint32_t owner_return_rva = vita_heap_arg(c, 2U);
        int scratch_result = isaac_vita_anm2_scratch_malloc(
            owner_return_rva, request, c->stack_floor, c->stack_ceiling,
            &scratch);

        if (scratch_result == ISAAC_VITA_ANM2_SCRATCH_REJECTED) {
            guest_fault(c, scratch.fault_value, scratch.fault);
            return;
        }
        if (scratch_result == ISAAC_VITA_ANM2_SCRATCH_HANDLED) {
            c->eax = (uint32_t)(uintptr_t)scratch.pointer;
            vita_heap_cdecl_return(c);
            return;
        }
        if (scratch_result != ISAAC_VITA_ANM2_SCRATCH_NOT_HANDLED &&
            scratch_result !=
                ISAAC_VITA_ANM2_SCRATCH_ROUTE_GUEST_HEAP) {
            guest_fault(c, owner_return_rva,
                        "ANM2 scratch returned an unknown allocation route");
            return;
        }
        /* NO_FREE_PHYSICAL_PAGE is the one explicit ordinary route.  The
         * common exact-base ledger below tries newlib, then the already-live
         * overflow mspace, and owns the resulting free/realloc lifetime. */
    }
#endif
#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_TEXEL_SCRATCH)
    owner_return_rva = vita_heap_texel_owner_return_rva(
        c, &owner_chain_depth);
#endif
#ifdef ISAAC_VITA_TEXEL_SCRATCH
    {
        isaac_vita_texel_scratch_decision scratch;
        int scratch_result = isaac_vita_texel_scratch_malloc(
            owner_return_rva, request, c->stack_floor, c->stack_ceiling,
            &scratch);

        if (scratch_result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED) {
            guest_fault(c, scratch.fault_value, scratch.fault);
            return;
        }
        if (scratch_result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED) {
            c->eax = (uint32_t)(uintptr_t)scratch.pointer;
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    if (request == ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES) {
        uint32_t room_owner_return_rva =
            vita_heap_operator_new_owner_return_rva(c);

        if (room_owner_return_rva ==
                ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA) {
            isaac_vita_room_entry_slab_decision room;
            isaac_vita_room_entry_slab_event slab_event;
            vita_heap_telemetry_event heap_event;
            isaac_vita_room_entry_slab_result slab_result;

            isaac_vita_room_entry_slab_event_init(&slab_event);
            vita_heap_telemetry_event_init(
                &heap_event, VITA_HEAP_TELEMETRY_OP_MALLOC, request);
            vita_heap_lock();
            slab_result = isaac_vita_room_entry_slab_malloc_locked(
                room_owner_return_rva, request, &room, &slab_event);
            vita_heap_room_slab_unlock(
                slab_result, &slab_event, &heap_event);
            if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL ||
                isaac_vita_guest_heap_terminal()) {
                guest_fault(c, (uint32_t)request,
                            "room-entry slab is corrupted");
                return;
            }
            if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED) {
                guest_fault(c, room.fault_value, room.fault);
                return;
            }
            if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) {
                c->eax = (uint32_t)(uintptr_t)room.pointer;
                vita_heap_cdecl_return(c);
                return;
            }
            /* Page/capacity/mspace OOM is an ordinary fallback.  Preserve
             * the original guest allocator path and its failure semantics. */
        }
    }
#endif
#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_OGG_EMERGENCY) || \
    defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE)
    const char *failure_reason;
    vita_heap_router_result router_result;
    void *result = vita_heap_guest_malloc_with_reason(
        request, &failure_reason, &router_result);
#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_OGG_EMERGENCY)
    uint32_t new_owner_return_rva = 0U;
#endif

    if (router_result == VITA_HEAP_ROUTER_TERMINAL ||
        isaac_vita_guest_heap_terminal()) {
        guest_fault(c, (uint32_t)request,
                    "guest heap overflow domain is corrupted");
        return;
    }
#if defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC) || \
    defined(ISAAC_VITA_OGG_EMERGENCY)
    if (!result) {
        new_owner_return_rva =
            vita_heap_operator_new_owner_return_rva(c);
#ifdef ISAAC_VITA_OGG_EMERGENCY
        if (strcmp(failure_reason, "native-malloc-failed") == 0) {
            isaac_vita_ogg_emergency_decision emergency;
            int emergency_result = isaac_vita_ogg_emergency_malloc(
                new_owner_return_rva, request,
                c->stack_floor, c->stack_ceiling, &emergency);

            if (emergency_result == ISAAC_VITA_OGG_EMERGENCY_REJECTED) {
                guest_fault(c, emergency.fault_value, emergency.fault);
                return;
            }
            if (emergency_result == ISAAC_VITA_OGG_EMERGENCY_HANDLED)
                result = emergency.pointer;
        }
#endif
    }
#endif
#ifdef ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC
    if (!result) {
        size_t ledger_capacity;
        size_t ledger_count;
        size_t ledger_tombstones;
        size_t rehash_target_bytes;
        size_t room_slab_live;

        vita_heap_failure_ledger_snapshot(
            &ledger_capacity, &ledger_count, &ledger_tombstones,
            &rehash_target_bytes, &room_slab_live);
        isaac_vita_log(
            "guest heap allocation failed: request=%u owner=0x%08x "
            "reason=%s ledger_capacity=%u ledger_count=%u "
            "ledger_tombstones=%u rehash_target_bytes=%u "
            "room_slab_live=%u",
            (unsigned)request, (unsigned)new_owner_return_rva,
            failure_reason, (unsigned)ledger_capacity,
            (unsigned)ledger_count, (unsigned)ledger_tombstones,
            (unsigned)rehash_target_bytes, (unsigned)room_slab_live);

        /* A generic allocation failure must not consume the one-shot texture
         * diagnostic.  Only the exact frozen loader stack is authoritative. */
        if (owner_return_rva) {
            size_t live_requested;
            size_t largest_live_request;
            size_t live_count;
            int requested_accounting_exact =
                vita_heap_diagnostic_snapshot(
                    &live_requested, &largest_live_request, &live_count);

            kage_vita_texel_oom_diagnostic(
                request, owner_return_rva, owner_chain_depth,
                failure_reason,
                live_requested, largest_live_request, live_count,
                requested_accounting_exact, c->stack_floor,
                c->stack_ceiling);
        }
    }
#endif
    c->eax = (uint32_t)(uintptr_t)result;
#else
    c->eax = (uint32_t)(uintptr_t)isaac_vita_guest_malloc(
        (size_t)vita_heap_arg(c, 0U));
#endif
    vita_heap_cdecl_return(c);
}

static void vita_heap_set_new_mode(CPU *__restrict c)
{
    uint32_t mode = vita_heap_arg(c, 0U);
    int previous;

    if (mode > 1U) {
        guest_fault(c, mode, "_set_new_mode requires 0 or 1");
        return;
    }
    vita_heap_lock();
    previous = s_new_mode;
    s_new_mode = (int)mode;
    vita_heap_unlock();
    c->eax = (uint32_t)previous;
    vita_heap_cdecl_return(c);
}

static void vita_heap_realloc(CPU *__restrict c)
{
    uint32_t pointer = vita_heap_arg(c, 0U);
    size_t size = (size_t)vita_heap_arg(c, 1U);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    if (isaac_vita_guest_heap_terminal()) {
        guest_fault(c, pointer, "guest heap overflow domain is corrupted");
        return;
    }
#endif
#ifdef ISAAC_VITA_OGG_EMERGENCY
    {
        isaac_vita_ogg_emergency_decision emergency;
        int emergency_result = isaac_vita_ogg_emergency_realloc(
            (void *)(uintptr_t)pointer, size, &emergency);

        if (emergency_result == ISAAC_VITA_OGG_EMERGENCY_REJECTED) {
            guest_fault(c, emergency.fault_value, emergency.fault);
            return;
        }
        if (emergency_result == ISAAC_VITA_OGG_EMERGENCY_HANDLED) {
            c->eax = (uint32_t)(uintptr_t)emergency.pointer;
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
#ifdef ISAAC_VITA_TEXEL_SCRATCH
    {
        isaac_vita_texel_scratch_decision scratch;
        int scratch_result = isaac_vita_texel_scratch_realloc(
            (void *)(uintptr_t)pointer, size, &scratch);

        if (scratch_result == ISAAC_VITA_TEXEL_SCRATCH_REJECTED) {
            guest_fault(c, scratch.fault_value, scratch.fault);
            return;
        }
        if (scratch_result == ISAAC_VITA_TEXEL_SCRATCH_HANDLED) {
            c->eax = (uint32_t)(uintptr_t)scratch.pointer;
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    {
        isaac_vita_room_entry_slab_decision room;
        isaac_vita_room_entry_slab_result slab_result;
        void *room_pointer = NULL;

        slab_result = vita_heap_room_slab_realloc_route(
            (void *)(uintptr_t)pointer, size, &room, &room_pointer);
        if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_TERMINAL ||
            isaac_vita_guest_heap_terminal()) {
            guest_fault(c, pointer, "room-entry slab is corrupted");
            return;
        }
        if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_REJECTED) {
            guest_fault(c, room.fault_value, room.fault);
            return;
        }
        if (slab_result == ISAAC_VITA_ROOM_ENTRY_SLAB_HANDLED) {
            c->eax = (uint32_t)(uintptr_t)room_pointer;
            vita_heap_cdecl_return(c);
            return;
        }
    }
#endif
    int valid_owner;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    vita_heap_router_result router_result;
    void *result = vita_heap_guest_realloc_impl(
        (void *)(uintptr_t)pointer, size, &valid_owner, 0, NULL,
        &router_result);
    if (router_result == VITA_HEAP_ROUTER_TERMINAL ||
        isaac_vita_guest_heap_terminal()) {
        guest_fault(c, pointer, "guest heap overflow domain is corrupted");
        return;
    }
#else
    void *result = isaac_vita_guest_realloc(
        (void *)(uintptr_t)pointer, size, &valid_owner);
#endif
    if (!valid_owner) {
        guest_fault(c, pointer, "realloc received a foreign guest pointer");
        return;
    }
    c->eax = (uint32_t)(uintptr_t)result;
    vita_heap_cdecl_return(c);
}

static const vita_heap_import_entry s_vita_heap_imports[] = {
    { ISAAC_VITA_HEAP_CALLNEWH_NAME,     vita_heap_callnewh },
    { ISAAC_VITA_HEAP_FREE_NAME,         vita_heap_free },
    { ISAAC_VITA_HEAP_CALLOC_NAME,       vita_heap_calloc },
    { ISAAC_VITA_HEAP_MALLOC_NAME,       vita_heap_malloc },
    { ISAAC_VITA_HEAP_SET_NEW_MODE_NAME, vita_heap_set_new_mode },
    { ISAAC_VITA_HEAP_REALLOC_NAME,      vita_heap_realloc }
};

_Static_assert(sizeof s_vita_heap_imports / sizeof s_vita_heap_imports[0] ==
               ISAAC_VITA_HEAP_IMPORT_COUNT,
               "Vita heap import count drifted from exact PE batch");

const char *isaac_vita_heap_import_name(uint32_t index)
{
    return index < ISAAC_VITA_HEAP_IMPORT_COUNT
        ? s_vita_heap_imports[index].name : NULL;
}

int isaac_vita_heap_import_indexed(CPU *__restrict c, uint32_t index,
                                   unsigned *call_count)
{
    if (index >= ISAAC_VITA_HEAP_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count; /* guest_fault may not return */
    s_vita_heap_imports[index].fn(c);
    return 1;
}

static int vita_heap_dispatch(CPU *__restrict c, const char *name,
                              unsigned *call_count)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_HEAP_IMPORT_COUNT; ++i) {
        if (strcmp(s_vita_heap_imports[i].name, name) == 0)
            return isaac_vita_heap_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_heap_import(CPU *__restrict c, const char *name)
{
    return vita_heap_dispatch(c, name, NULL);
}

int isaac_vita_heap_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count)
{
    return vita_heap_dispatch(c, name, call_count);
}
