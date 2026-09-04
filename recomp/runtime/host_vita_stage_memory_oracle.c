/* Executable oracle for the exact stage/floor memory producer.  It includes
 * the production heap source so the test can seed otherwise-private fixed
 * telemetry state; allocator entry points not reached by this oracle are
 * discarded with -ffunction-sections/--gc-sections. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(ISAAC_VITA_STAGE_MEMORY_ORACLE) || \
    !defined(ISAAC_VITA_HEAP_TESTING)
#error Stage-memory oracle requires its oracle and heap-testing definitions
#endif

#include "host_vita_heap.c"

#define ORACLE_LOG_CAPACITY 64U
#define ORACLE_LOG_BYTES 384U

static struct mallinfo s_oracle_heap;
static char s_oracle_logs[ORACLE_LOG_CAPACITY][ORACLE_LOG_BYTES];
static unsigned s_oracle_log_count;
static unsigned s_oracle_log_truncations;
static unsigned s_oracle_log_under_lock;
static unsigned s_oracle_mallinfo_calls;
static unsigned s_oracle_mallinfo_under_lock;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
static isaac_vita_heap_overflow_mspace_snapshot s_oracle_raw;
static unsigned s_oracle_raw_ok;
static unsigned s_oracle_raw_calls;
static unsigned s_oracle_raw_without_lock;
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
static isaac_vita_room_entry_slab_fast_snapshot s_oracle_room;
static isaac_vita_room_entry_slab_floor_lifetime_snapshot
    s_oracle_room_floor;
static unsigned s_oracle_room_ok;
static unsigned s_oracle_room_fast_calls;
static unsigned s_oracle_room_fast_without_lock;
static unsigned s_oracle_room_cold_calls;
static unsigned s_oracle_room_floor_bootstrap_calls;
static unsigned s_oracle_room_floor_rollover_calls;
static unsigned s_oracle_room_floor_phase_calls;
static unsigned s_oracle_room_floor_snapshot_calls;
static unsigned s_oracle_room_floor_without_lock;
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "stage-memory oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    (void)address;
    if (c)
        c->fault = what ? what : "stage-memory oracle fault";
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)c;
    (void)pc;
    (void)kind;
    (void)address;
    (void)size;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)c;
    (void)pc;
    return 0;
}

struct mallinfo isaac_vita_stage_memory_oracle_mallinfo(void)
{
    ++s_oracle_mallinfo_calls;
    if (isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_mallinfo_under_lock;
    return s_oracle_heap;
}

void isaac_vita_log(const char *format, ...)
{
    char scratch[ORACLE_LOG_BYTES];
    char *destination = scratch;
    va_list arguments;
    int length;

    if (isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_log_under_lock;
    if (s_oracle_log_count < ORACLE_LOG_CAPACITY)
        destination = s_oracle_logs[s_oracle_log_count];
    va_start(arguments, format);
    length = vsnprintf(destination, ORACLE_LOG_BYTES, format, arguments);
    va_end(arguments);
    if (length < 0 || (unsigned)length >= ORACLE_LOG_BYTES)
        ++s_oracle_log_truncations;
    if (s_oracle_log_count != UINT32_MAX)
        ++s_oracle_log_count;
}

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
int isaac_vita_heap_overflow_mspace_snapshot_get(
    isaac_vita_heap_overflow_mspace_snapshot *snapshot_out)
{
    ++s_oracle_raw_calls;
    if (!isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_raw_without_lock;
    if (!snapshot_out || !s_oracle_raw_ok)
        return 0;
    *snapshot_out = s_oracle_raw;
    return 1;
}

int isaac_vita_heap_overflow_mspace_init(
    const isaac_vita_heap_overflow_forbidden_ranges *forbidden)
{
    (void)forbidden;
    return 0;
}

int isaac_vita_heap_overflow_mspace_contains(const void *pointer)
{
    (void)pointer;
    return 0;
}

void *isaac_vita_heap_overflow_mspace_malloc(size_t size)
{
    (void)size;
    return NULL;
}

void *isaac_vita_heap_overflow_mspace_calloc(size_t count, size_t size)
{
    (void)count;
    (void)size;
    return NULL;
}

void *isaac_vita_heap_overflow_mspace_internal_page_malloc(void)
{
    return NULL;
}

int isaac_vita_heap_overflow_mspace_internal_page_free(void *pointer)
{
    (void)pointer;
    return 0;
}

void *isaac_vita_heap_overflow_mspace_realloc(
    void *pointer, size_t size,
    isaac_vita_heap_overflow_mspace_realloc_status *status_out)
{
    (void)pointer;
    (void)size;
    if (status_out)
        *status_out = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_REALLOC_REJECTED;
    return NULL;
}

int isaac_vita_heap_overflow_mspace_free(void *pointer)
{
    (void)pointer;
    return 0;
}

int isaac_vita_heap_ledger_memblock_acquire(
    size_t usable_bytes, isaac_vita_heap_ledger_storage *storage_out)
{
    (void)usable_bytes;
    (void)storage_out;
    return ISAAC_VITA_HEAP_LEDGER_STORAGE_FAILED;
}

int isaac_vita_heap_ledger_memblock_release(
    const isaac_vita_heap_ledger_storage *storage)
{
    (void)storage;
    return -1;
}
#endif

#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
int isaac_vita_room_entry_slab_snapshot_locked(
    isaac_vita_room_entry_slab_snapshot *snapshot_out)
{
    (void)snapshot_out;
    ++s_oracle_room_cold_calls;
    return 0;
}

int isaac_vita_room_entry_slab_fast_snapshot_locked(
    isaac_vita_room_entry_slab_fast_snapshot *snapshot_out)
{
    ++s_oracle_room_fast_calls;
    if (!isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_room_fast_without_lock;
    if (!snapshot_out || !s_oracle_room_ok)
        return 0;
    *snapshot_out = s_oracle_room;
    return 1;
}

static int oracle_floor_phase_valid(uint32_t phase)
{
    return phase >= ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_LEVEL_INIT &&
        phase <= ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_PLAY;
}

int isaac_vita_room_entry_slab_floor_lifetime_bootstrap_locked(
    uint32_t phase)
{
    ++s_oracle_room_floor_bootstrap_calls;
    if (!isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_room_floor_without_lock;
    if (!s_oracle_room_floor.valid || s_oracle_room_floor.active ||
        s_oracle_room_floor.epoch || !oracle_floor_phase_valid(phase)) {
        s_oracle_room_floor.valid = 0U;
        s_oracle_room_floor.phase =
            ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_UNSCOPED;
        return 0;
    }
    s_oracle_room_floor.active = 1U;
    s_oracle_room_floor.epoch = 1U;
    s_oracle_room_floor.phase = phase;
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_rollover_locked(void)
{
    ++s_oracle_room_floor_rollover_calls;
    if (!isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_room_floor_without_lock;
    if (!s_oracle_room_floor.valid || !s_oracle_room_floor.active ||
        !s_oracle_room_floor.epoch ||
        s_oracle_room_floor.epoch == UINT32_MAX) {
        s_oracle_room_floor.valid = 0U;
        s_oracle_room_floor.phase =
            ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_UNSCOPED;
        return 0;
    }
    ++s_oracle_room_floor.epoch;
    s_oracle_room_floor.prior_slots +=
        s_oracle_room_floor.current_slots;
    s_oracle_room_floor.current_slots = 0U;
    s_oracle_room_floor.level_init_slots = 0U;
    s_oracle_room_floor.room_load_slots = 0U;
    s_oracle_room_floor.play_slots = 0U;
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_phase_set_locked(
    uint32_t phase)
{
    ++s_oracle_room_floor_phase_calls;
    if (!isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_room_floor_without_lock;
    if (!s_oracle_room_floor.valid || !s_oracle_room_floor.active ||
        !oracle_floor_phase_valid(phase)) {
        s_oracle_room_floor.valid = 0U;
        s_oracle_room_floor.phase =
            ISAAC_VITA_ROOM_ENTRY_FLOOR_PHASE_UNSCOPED;
        return 0;
    }
    s_oracle_room_floor.phase = phase;
    return 1;
}

int isaac_vita_room_entry_slab_floor_lifetime_snapshot_locked(
    isaac_vita_room_entry_slab_floor_lifetime_snapshot *snapshot_out)
{
    ++s_oracle_room_floor_snapshot_calls;
    if (!isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_room_floor_without_lock;
    if (!snapshot_out)
        return 0;
    *snapshot_out = s_oracle_room_floor;
    return s_oracle_room_floor.valid != 0U;
}

void isaac_vita_room_entry_slab_event_init(
    isaac_vita_room_entry_slab_event *event)
{
    if (event)
        memset(event, 0, sizeof *event);
}

isaac_vita_room_entry_slab_result isaac_vita_room_entry_slab_malloc_locked(
    uint32_t owner_return_rva, size_t size,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event)
{
    (void)owner_return_rva;
    (void)size;
    (void)decision;
    (void)event;
    return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
}

isaac_vita_room_entry_slab_result isaac_vita_room_entry_slab_free_locked(
    void *pointer, uint32_t free_return_rva,
    isaac_vita_room_entry_slab_lease_predicate is_leased,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event)
{
    (void)pointer;
    (void)free_return_rva;
    (void)is_leased;
    (void)decision;
    (void)event;
    return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
}

isaac_vita_room_entry_slab_result
isaac_vita_room_entry_slab_realloc_begin_locked(
    void *pointer, size_t size,
    isaac_vita_room_entry_slab_lease_predicate is_leased,
    isaac_vita_room_entry_slab_decision *decision,
    isaac_vita_room_entry_slab_event *event)
{
    (void)pointer;
    (void)size;
    (void)is_leased;
    (void)decision;
    (void)event;
    return ISAAC_VITA_ROOM_ENTRY_SLAB_NOT_OWNED;
}

int isaac_vita_room_entry_slab_realloc_cancel_locked(
    void *pointer, isaac_vita_room_entry_slab_event *event)
{
    (void)pointer;
    (void)event;
    return 0;
}

int isaac_vita_room_entry_slab_realloc_commit_locked(
    void *pointer, isaac_vita_room_entry_slab_event *event)
{
    (void)pointer;
    (void)event;
    return 0;
}

int isaac_vita_room_entry_slab_raw_accounting_locked(
    uint32_t *pages_out, uint32_t *requested_bytes_out)
{
    if (pages_out)
        *pages_out = s_oracle_room.raw_pages;
    if (requested_bytes_out)
        *requested_bytes_out = s_oracle_room.raw_pages *
            ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES;
    return pages_out && requested_bytes_out;
}

int isaac_vita_room_entry_slab_owns_exact_locked(const void *pointer)
{
    (void)pointer;
    return 0;
}

int isaac_vita_room_entry_slab_find_containing_locked(
    const void *range, size_t size, uintptr_t *allocation_base_out)
{
    (void)range;
    (void)size;
    (void)allocation_base_out;
    return 0;
}

void isaac_vita_room_entry_slab_claim_final_locked(
    isaac_vita_room_entry_slab_event *event)
{
    (void)event;
}

void isaac_vita_room_entry_slab_log_event(
    const isaac_vita_room_entry_slab_event *event)
{
    (void)event;
}
#endif

static unsigned oracle_stage_log_index(unsigned event_index)
{
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    return event_index * 2U;
#else
    return event_index;
#endif
}

static int log_has(unsigned event_index, const char *needle)
{
    unsigned index = oracle_stage_log_index(event_index);

    return index < s_oracle_log_count && index < ORACLE_LOG_CAPACITY &&
        strstr(s_oracle_logs[index], needle) != NULL;
}

#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
static int floor_log_has(unsigned event_index, const char *needle)
{
    unsigned index = oracle_stage_log_index(event_index) + 1U;

    return index < s_oracle_log_count && index < ORACLE_LOG_CAPACITY &&
        strstr(s_oracle_logs[index], needle) != NULL;
}
#endif

static void oracle_seed_memory(void)
{
    memset(&s_oracle_heap, 0, sizeof s_oracle_heap);
    s_oracle_heap.arena = 0x1000;
    s_oracle_heap.uordblks = 0x900;
    s_oracle_heap.fordblks = 0x700;
    s_oracle_heap.ordblks = 5;
    s_ledger_count = 9U;
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    s_floor_lifetime.total_count = 9U;
    s_floor_lifetime.unscoped_count = 9U;
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    memset(&s_heap_telemetry, 0, sizeof s_heap_telemetry);
    s_heap_telemetry.active = 1U;
    s_heap_telemetry.accounting_valid = 1U;
    s_heap_telemetry.owned_live_count = 3U;
    s_heap_telemetry.owned_requested_bytes = 0x1234U;
    s_heap_terminal = 0;
    memset(&s_oracle_raw, 0, sizeof s_oracle_raw);
    s_oracle_raw.state = ISAAC_VITA_HEAP_OVERFLOW_MSPACE_READY;
    s_oracle_raw.live_count = 6U;
    s_oracle_raw.stranded_count = 1U;
    s_oracle_raw.internal_live_count = 2U;
    s_oracle_raw.internal_requested_bytes = 0x20000U;
    s_oracle_raw.has_mspace = 1;
    s_oracle_raw_ok = 1U;
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    memset(&s_oracle_room, 0, sizeof s_oracle_room);
    s_oracle_room.pages = 3U;
    s_oracle_room.raw_pages = 2U;
    s_oracle_room.external_pages = 1U;
    s_oracle_room.live_slots = 17U;
    s_oracle_room.allocations = 20U;
    s_oracle_room.frees = 3U;
    s_oracle_room_ok = 1U;
    memset(&s_oracle_room_floor, 0, sizeof s_oracle_room_floor);
    s_oracle_room_floor.valid = 1U;
#endif
}

static void oracle_reset(void)
{
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    static const vita_heap_floor_lifetime_state floor_initial =
        VITA_HEAP_FLOOR_LIFETIME_STATE_INITIALIZER;
#endif

    isaac_vita_stage_memory_test_reset();
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    s_floor_lifetime = floor_initial;
#endif
    memset(s_oracle_logs, 0, sizeof s_oracle_logs);
    s_oracle_log_count = 0U;
    s_oracle_log_truncations = 0U;
    s_oracle_log_under_lock = 0U;
    s_oracle_mallinfo_calls = 0U;
    s_oracle_mallinfo_under_lock = 0U;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    s_oracle_raw_calls = 0U;
    s_oracle_raw_without_lock = 0U;
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    s_oracle_room_fast_calls = 0U;
    s_oracle_room_fast_without_lock = 0U;
    s_oracle_room_cold_calls = 0U;
    s_oracle_room_floor_bootstrap_calls = 0U;
    s_oracle_room_floor_rollover_calls = 0U;
    s_oracle_room_floor_phase_calls = 0U;
    s_oracle_room_floor_snapshot_calls = 0U;
    s_oracle_room_floor_without_lock = 0U;
#endif
    memset(&s_heap_test_probe_stats, 0, sizeof s_heap_test_probe_stats);
    oracle_seed_memory();
}

static int check_observer_boundaries(unsigned expected)
{
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(s_oracle_log_count == expected * 2U);
#else
    CHECK(s_oracle_log_count == expected);
#endif
    CHECK(s_oracle_mallinfo_calls == expected);
    CHECK(s_heap_test_probe_stats.lock_acquisitions == expected);
    CHECK(s_oracle_log_under_lock == 0U);
    CHECK(s_oracle_mallinfo_under_lock == 0U);
    CHECK(s_oracle_log_truncations == 0U);
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    CHECK(s_oracle_raw_calls == expected);
    CHECK(s_oracle_raw_without_lock == 0U);
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    CHECK(s_oracle_room_fast_calls == expected);
    CHECK(s_oracle_room_fast_without_lock == 0U);
    CHECK(s_oracle_room_cold_calls == 0U);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(s_oracle_room_floor_snapshot_calls == expected);
    CHECK(s_oracle_room_floor_without_lock == 0U);
#endif
#endif
    return 0;
}

static int check_memory_fields(unsigned index)
{
    CHECK(log_has(index, "mi=1000/900/700/5 ledger=9"));
#if defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB)
    CHECK(log_has(index,
        "ov=3/1234/2/20000 slab=3/2/1/11/14/3 "
        "valid/term/sat=1/0/0"));
#else
    CHECK(log_has(index,
        "ov=0/0/0/0 slab=0/0/0/0/0/0 valid/term/sat=1/0/0"));
#endif
    return 0;
}

static int check_nominal_order(void)
{
    oracle_reset();
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 3U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 4U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 4U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 4U, 1U, 1U);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 3U, 2U, UINT32_MAX);

    CHECK(check_observer_boundaries(6U) == 0);
    CHECK(log_has(0U,
        "q=1 lq/rq=1/0 e=level-begin ctx_valid=1 active=1 "
        "ls=3 lt=2 rs=ffffffff mode=ffffffff ok=ffffffff"));
    CHECK(log_has(1U,
        "q=2 lq/rq=1/2 e=room-reuse ctx_valid=1 active=1 "
        "ls=3 lt=2 rs=0 mode=1 ok=ffffffff"));
    CHECK(log_has(2U,
        "q=3 lq/rq=1/3 e=room-load-begin ctx_valid=1 active=1 "
        "ls=3 lt=2 rs=4 mode=1 ok=ffffffff"));
    CHECK(log_has(3U,
        "q=4 lq/rq=1/3 e=room-after-unload ctx_valid=1 active=1"));
    CHECK(log_has(4U,
        "q=5 lq/rq=1/3 e=room-load-end ctx_valid=1 active=1 "
        "ls=3 lt=2 rs=4 mode=1 ok=1"));
    CHECK(log_has(5U,
        "q=6 lq/rq=1/3 e=level-end ctx_valid=1 active=1 "
        "ls=3 lt=2 rs=4 mode=1 ok=ffffffff"));
    CHECK(check_memory_fields(5U) == 0);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(floor_log_has(0U,
        "floorlife: q=1 e=level-begin epoch=1 boot/roll=1/0 "
        "phase=level-init heap=9/9/0/0,0/0/0/0"));
    CHECK(floor_log_has(2U,
        "floorlife: q=3 e=room-load-begin epoch=1 boot/roll=1/0 "
        "phase=room-load"));
    CHECK(floor_log_has(4U,
        "floorlife: q=5 e=room-load-end epoch=1 boot/roll=1/0 "
        "phase=level-init"));
    CHECK(floor_log_has(5U,
        "floorlife: q=6 e=level-end epoch=1 boot/roll=1/0 phase=play"));
    CHECK(floor_log_has(5U,
        "hphase=0/0/0,0/0/0 slab=0/0/0/0 "
        "sphase=0/0/0 valid/term/sat=1/0/0"));
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    CHECK(s_oracle_room_floor_bootstrap_calls == 1U);
    CHECK(s_oracle_room_floor_rollover_calls == 0U);
    CHECK(s_oracle_room_floor_phase_calls == 3U);
#endif
#endif
    CHECK(s_stage_memory.active == 0U &&
          s_stage_memory.room_open == 0U &&
          s_stage_memory.room_after_unload == 0U &&
          s_stage_memory.level_sequence == 0U &&
          s_stage_memory.room_sequence == 0U &&
          s_stage_memory.level_stage == UINT32_MAX &&
          s_stage_memory.level_type == UINT32_MAX &&
          s_stage_memory.room_stage == UINT32_MAX &&
          s_stage_memory.room_mode == UINT32_MAX);
    return 0;
}

static int check_early_failure_outside_level(void)
{
    oracle_reset();
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 7U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 7U, 2U, 0U);
    CHECK(check_observer_boundaries(2U) == 0);
    CHECK(log_has(0U,
        "q=1 lq/rq=0/1 e=room-load-begin ctx_valid=1 active=0"));
    CHECK(log_has(1U,
        "q=2 lq/rq=0/1 e=room-load-end ctx_valid=1 active=0 "));
    CHECK(log_has(1U, "rs=7 mode=2 ok=0"));
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(floor_log_has(0U,
        "floorlife: q=1 e=room-load-begin epoch=1 boot/roll=1/0 "
        "phase=room-load"));
    CHECK(floor_log_has(1U,
        "floorlife: q=2 e=room-load-end epoch=1 boot/roll=1/0 "
        "phase=play"));
#endif
    CHECK(s_stage_memory.room_open == 0U &&
          s_stage_memory.room_after_unload == 0U &&
          s_stage_memory.room_sequence == 1U &&
          s_stage_memory.room_stage == 7U &&
          s_stage_memory.room_mode == 2U);
    return 0;
}

static int check_floor_rollover(void)
{
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    oracle_reset();
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 1U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 1U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 3U, 4U, UINT32_MAX);
    CHECK(check_observer_boundaries(3U) == 0);
    CHECK(floor_log_has(2U,
        "floorlife: q=3 e=level-begin epoch=2 boot/roll=1/1 "
        "phase=level-init heap=9/9/0/0,0/0/0/0"));
    CHECK(floor_log_has(2U,
        "hphase=0/0/0,0/0/0 slab=0/0/0/0 "
        "sphase=0/0/0 valid/term/sat=1/0/0"));
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    CHECK(s_oracle_room_floor_bootstrap_calls == 1U);
    CHECK(s_oracle_room_floor_rollover_calls == 1U);
    CHECK(s_oracle_room_floor_phase_calls == 2U);
#endif
#endif
    return 0;
}

static int check_success_requires_unload(void)
{
    oracle_reset();
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 7U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 7U, 2U, 1U);
    CHECK(log_has(1U,
        "e=room-load-end ctx_valid=0 active=0"));
    CHECK(s_stage_memory.room_open == 1U);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 7U, 2U, 0U);
    CHECK(log_has(2U,
        "e=room-load-end ctx_valid=1 active=0"));
    CHECK(s_stage_memory.room_open == 0U);

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 9U, 3U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 9U, 3U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 9U, 3U, 0U);
    CHECK(log_has(5U,
        "e=room-load-end ctx_valid=1 active=0"));
    CHECK(s_stage_memory.room_open == 0U);
    CHECK(check_observer_boundaries(6U) == 0);
    return 0;
}

static int check_reuse_without_load(void)
{
    oracle_reset();
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 8U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 0U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 8U, 1U, UINT32_MAX);
    CHECK(check_observer_boundaries(3U) == 0);
    CHECK(log_has(1U,
        "q=2 lq/rq=1/2 e=room-reuse ctx_valid=1 active=1"));
    CHECK(log_has(2U,
        "q=3 lq/rq=1/2 e=level-end ctx_valid=1 active=1 "
        "ls=8 lt=1 rs=0 mode=0 ok=ffffffff"));
    return 0;
}

static int check_malformed_order_is_skipped(void)
{
    oracle_reset();
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 1U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 3U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 9U, 9U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 4U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 5U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 5U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 4U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 4U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 4U, 1U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 5U, 1U, 1U);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 4U, 1U, 1U);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 9U, 2U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 1U, 0U);
    isaac_vita_stage_memory_note(99U, 0U, 0U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 3U, 2U, UINT32_MAX);

    CHECK(check_observer_boundaries(15U) == 0);
    CHECK(log_has(0U, "e=level-end ctx_valid=0 active=0"));
    CHECK(log_has(2U,
        "q=3 lq/rq=2/0 e=level-begin ctx_valid=0 active=1 "
        "ls=3 lt=2"));
    CHECK(log_has(4U,
        "q=5 lq/rq=2/4 e=room-load-begin ctx_valid=0 active=1 "));
    CHECK(log_has(5U, "e=room-after-unload ctx_valid=0"));
    CHECK(log_has(6U, "e=room-after-unload ctx_valid=1"));
    CHECK(log_has(7U, "e=room-after-unload ctx_valid=0"));
    CHECK(log_has(8U, "e=room-load-end ctx_valid=0"));
    CHECK(log_has(9U, "e=room-load-end ctx_valid=0"));
    CHECK(log_has(10U, "e=room-load-end ctx_valid=1"));
    CHECK(log_has(11U, "e=level-end ctx_valid=0 active=1"));
    CHECK(log_has(12U, "e=room-reuse ctx_valid=0 active=1"));
    CHECK(log_has(13U, "e=unknown ctx_valid=0 active=1"));
    CHECK(log_has(14U, "e=level-end ctx_valid=1 active=1"));
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(floor_log_has(0U,
        "floorlife: q=1 e=level-end epoch=0 boot/roll=0/0 "
        "phase=pre-floor"));
    CHECK(floor_log_has(0U, "valid/term/sat=0/0/0"));
    CHECK(floor_log_has(14U,
        "floorlife: q=f e=level-end epoch=0 boot/roll=0/0 "
        "phase=pre-floor"));
    CHECK(floor_log_has(14U, "valid/term/sat=0/0/0"));
#endif
    CHECK(s_stage_memory.active == 0U &&
          s_stage_memory.room_open == 0U);
    return 0;
}

static int check_invalid_memory_snapshots(void)
{
    oracle_reset();
    s_oracle_heap.arena = -1;
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 0U, UINT32_MAX);
    CHECK(log_has(0U, "mi=ffffffff/900/700/5"));
    CHECK(log_has(0U, "valid/term/sat=0/0/0"));

#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    oracle_reset();
    s_oracle_raw_ok = 0U;
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 0U, UINT32_MAX);
    CHECK(log_has(0U, "ov=3/1234/ffffffff/ffffffff"));
    CHECK(log_has(0U, "valid/term/sat=0/0/0"));
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    oracle_reset();
    s_oracle_room_ok = 0U;
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 0U, UINT32_MAX);
    CHECK(log_has(0U,
        "slab=ffffffff/ffffffff/ffffffff/ffffffff/ffffffff/ffffffff"));
    CHECK(log_has(0U, "valid/term/sat=0/0/0"));
#endif
    return 0;
}

static int check_terminal_saturation_orthogonality(void)
{
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    oracle_reset();
    s_oracle_room.terminal = 1U;
    s_oracle_room.counter_saturated = 0U;
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 0U, UINT32_MAX);
    CHECK(check_observer_boundaries(1U) == 0);
    CHECK(log_has(0U,
        "slab=3/2/1/11/14/3 valid/term/sat=1/1/0"));

    oracle_reset();
    s_oracle_room.terminal = 0U;
    s_oracle_room.counter_saturated = 1U;
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_REUSE, 0U, 0U, UINT32_MAX);
    CHECK(check_observer_boundaries(1U) == 0);
    CHECK(log_has(0U,
        "slab=3/2/1/11/14/3 valid/term/sat=0/0/1"));
#endif
    return 0;
}

static int check_saturation_terminal_and_max_line(void)
{
    oracle_reset();
    s_stage_memory.sequence = UINT32_MAX;
    s_stage_memory.level_sequence = UINT32_MAX;
    s_stage_memory.room_sequence = UINT32_MAX;
    s_stage_memory.active = UINT32_MAX;
    s_stage_memory.level_stage = UINT32_MAX;
    s_stage_memory.level_type = UINT32_MAX;
    s_stage_memory.room_open = 1U;
    s_stage_memory.room_after_unload = 0U;
    s_stage_memory.room_stage = UINT32_MAX;
    s_stage_memory.room_mode = UINT32_MAX;
    s_oracle_heap.arena = -1;
    s_oracle_heap.uordblks = -1;
    s_oracle_heap.fordblks = -1;
    s_oracle_heap.ordblks = -1;
    s_ledger_count = SIZE_MAX;
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    s_heap_telemetry.owned_live_count = UINT32_MAX;
    s_heap_telemetry.owned_requested_bytes = UINT32_MAX;
    s_heap_terminal = 1;
    s_oracle_raw.live_count = UINT32_MAX;
    s_oracle_raw.stranded_count = UINT32_MAX;
    s_oracle_raw.internal_live_count = UINT32_MAX;
    s_oracle_raw.internal_requested_bytes = UINT32_MAX;
#endif
#ifdef ISAAC_VITA_ROOM_ENTRY_SLAB
    memset(&s_oracle_room, 0xff, sizeof s_oracle_room);
#endif
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD,
        UINT32_MAX, UINT32_MAX, UINT32_MAX);
#ifdef ISAAC_VITA_HEAP_RANGE_LEASE
    CHECK(s_oracle_log_count == 2U && s_oracle_log_truncations == 0U);
    CHECK(strlen(s_oracle_logs[1]) < ORACLE_LOG_BYTES);
#else
    CHECK(s_oracle_log_count == 1U && s_oracle_log_truncations == 0U);
#endif
#if defined(ISAAC_VITA_HEAP_OVERFLOW_MSPACE) && \
    defined(ISAAC_VITA_ROOM_ENTRY_SLAB)
    CHECK(strlen(s_oracle_logs[0]) == 328U);
#else
    CHECK(strlen(s_oracle_logs[0]) < ORACLE_LOG_BYTES);
#endif
    CHECK(log_has(0U,
        "q=ffffffff lq/rq=ffffffff/ffffffff "
        "e=room-after-unload ctx_valid=1 active=ffffffff"));
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
    CHECK(log_has(0U, "valid/term/sat=0/1/1"));
#else
    CHECK(log_has(0U, "valid/term/sat=0/0/1"));
#endif
    return 0;
}

int main(void)
{
    if (check_nominal_order() ||
        check_early_failure_outside_level() ||
        check_floor_rollover() ||
        check_success_requires_unload() ||
        check_reuse_without_load() ||
        check_malformed_order_is_skipped() ||
        check_invalid_memory_snapshots() ||
        check_terminal_saturation_orthogonality() ||
        check_saturation_terminal_and_max_line())
        return 1;
    puts("stage-memory producer oracle: PASS");
    return 0;
}
