/* Executable oracle for the generic requested-size ledger's floor-lifetime
 * sidecars.  The production source is included so collision/backshift and
 * same-key replacement can be checked without a second implementation. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(ISAAC_VITA_STAGE_MEMORY_ORACLE) || \
    !defined(ISAAC_VITA_HEAP_TESTING) || \
    !defined(ISAAC_VITA_HEAP_RANGE_LEASE) || \
    !defined(ISAAC_VITA_TEXEL_OOM_DIAGNOSTIC)
#error Floor-lifetime oracle requires stage, testing, range and OOM diagnostic
#endif
#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE
#error This oracle isolates the generic range ledger without the pool
#endif

#include "host_vita_heap.c"

void kage_vita_texel_oom_diagnostic(
    size_t failed_request, uint32_t owner_return_rva,
    unsigned owner_chain_depth, const char *failure_reason,
    size_t ledger_live_requested,
    size_t ledger_largest_live_request, size_t ledger_live_count,
    int ledger_requested_accounting_exact, uint32_t guest_stack_floor,
    uint32_t guest_stack_ceiling)
{
    (void)failed_request;
    (void)owner_return_rva;
    (void)owner_chain_depth;
    (void)failure_reason;
    (void)ledger_live_requested;
    (void)ledger_largest_live_request;
    (void)ledger_live_count;
    (void)ledger_requested_accounting_exact;
    (void)guest_stack_floor;
    (void)guest_stack_ceiling;
}

#define ORACLE_LOG_CAPACITY 96U
#define ORACLE_LOG_BYTES 384U

static char s_oracle_logs[ORACLE_LOG_CAPACITY][ORACLE_LOG_BYTES];
static uint32_t s_oracle_log_count;
static uint32_t s_oracle_log_truncations;
static uint32_t s_oracle_log_under_lock;
static uint32_t s_oracle_mallinfo_count;
static uint32_t s_oracle_mallinfo_under_lock;
static uint32_t s_oracle_mallinfo_current[ORACLE_LOG_CAPACITY];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "floor-lifetime oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    (void)address;
    if (c)
        c->fault = what ? what : "floor-lifetime oracle fault";
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
    struct mallinfo result;

    memset(&result, 0, sizeof result);
    if (isaac_vita_guest_heap_test_lock_held())
        ++s_oracle_mallinfo_under_lock;
    if (s_oracle_mallinfo_count < ORACLE_LOG_CAPACITY)
        s_oracle_mallinfo_current[s_oracle_mallinfo_count] =
            s_floor_lifetime.current_count;
    ++s_oracle_mallinfo_count;
    return result;
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
    ++s_oracle_log_count;
}

static int log_has(uint32_t index, const char *needle)
{
    return index < s_oracle_log_count && index < ORACLE_LOG_CAPACITY &&
        strstr(s_oracle_logs[index], needle) != NULL;
}

static int oracle_reset_empty(void)
{
    static const vita_heap_floor_lifetime_state floor_initial =
        VITA_HEAP_FLOOR_LIFETIME_STATE_INITIALIZER;
    static const vita_stage_memory_state stage_initial =
        VITA_STAGE_MEMORY_STATE_INITIALIZER;
    vita_heap_ledger_layout layout;
    size_t index;

    CHECK(s_ledger_count == 0U);
#ifndef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    if (s_ledger_tombstones) {
        vita_heap_lock();
        CHECK(vita_heap_ledger_rehash(s_ledger_capacity));
        vita_heap_unlock();
    }
#endif
    for (index = 0U; index < s_ledger_capacity; ++index)
        CHECK(s_ledger[index].base == 0U);
    if (s_ledger_capacity) {
        CHECK(vita_heap_ledger_combined_layout(
            s_ledger_capacity, &layout));
        memset(s_ledger_floor_phase, 0, layout.phase_bytes);
        memset(s_ledger_floor_current, 0, layout.current_bytes);
    }
    s_floor_lifetime = floor_initial;
    s_stage_memory = stage_initial;
    memset(s_oracle_logs, 0, sizeof s_oracle_logs);
    memset(s_oracle_mallinfo_current, 0,
           sizeof s_oracle_mallinfo_current);
    s_oracle_log_count = 0U;
    s_oracle_log_truncations = 0U;
    s_oracle_log_under_lock = 0U;
    s_oracle_mallinfo_count = 0U;
    s_oracle_mallinfo_under_lock = 0U;
    return 0;
}

static int snapshot_get(
    isaac_vita_guest_heap_floor_lifetime_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    return isaac_vita_guest_heap_floor_lifetime_snapshot_get(snapshot);
}

static uintptr_t oracle_key_for_home(
    size_t home, size_t capacity, uintptr_t first)
{
    uintptr_t key = first > VITA_HEAP_LEDGER_TOMBSTONE ?
        first : VITA_HEAP_LEDGER_TOMBSTONE + 1U;
    size_t attempts;

    for (attempts = 0U; attempts < 0x100000U; ++attempts, ++key)
        if (key > VITA_HEAP_LEDGER_TOMBSTONE &&
            (vita_heap_hash(key) & (capacity - 1U)) == home)
            return key;
    return 0U;
}

/* Cold oracle only: exhaustively reconcile every live slot and every empty
 * sidecar against the O(1) production counters.  The producer itself must
 * never pay for this scan under the guest lock. */
static int check_cold_sidecar_census(void)
{
    uint32_t total_count = 0U;
    uint32_t unscoped_count = 0U;
    uint32_t current_count = 0U;
    uint32_t total_bytes = 0U;
    uint32_t unscoped_bytes = 0U;
    uint32_t current_bytes = 0U;
    uint32_t phase_count[3] = { 0U, 0U, 0U };
    uint32_t phase_bytes[3] = { 0U, 0U, 0U };
    size_t index;

    for (index = 0U; index < s_ledger_capacity; ++index) {
        uint32_t phase = s_ledger_floor_phase[index];
        int current = vita_heap_floor_current_bit_get(
            s_ledger_floor_current, index);
        uint32_t bytes;

        if (s_ledger[index].base <= VITA_HEAP_LEDGER_TOMBSTONE) {
            CHECK(phase == 0U && !current);
            continue;
        }
        CHECK(phase <= ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY);
        CHECK(!current || phase != 0U);
        CHECK(s_ledger[index].requested_size <= UINT32_MAX);
        bytes = (uint32_t)s_ledger[index].requested_size;
        CHECK(total_count != UINT32_MAX &&
              total_bytes <= UINT32_MAX - bytes);
        ++total_count;
        total_bytes += bytes;
        if (!phase) {
            CHECK(unscoped_count != UINT32_MAX &&
                  unscoped_bytes <= UINT32_MAX - bytes);
            ++unscoped_count;
            unscoped_bytes += bytes;
        }
        else if (current) {
            uint32_t bucket = phase - 1U;

            CHECK(current_count != UINT32_MAX &&
                  current_bytes <= UINT32_MAX - bytes &&
                  phase_count[bucket] != UINT32_MAX &&
                  phase_bytes[bucket] <= UINT32_MAX - bytes);
            ++current_count;
            current_bytes += bytes;
            ++phase_count[bucket];
            phase_bytes[bucket] += bytes;
        }
    }
    CHECK(total_count == s_ledger_count);
    CHECK(total_count == s_floor_lifetime.total_count);
    CHECK(unscoped_count == s_floor_lifetime.unscoped_count);
    CHECK(current_count == s_floor_lifetime.current_count);
    CHECK(total_bytes == s_floor_lifetime.total_requested_bytes);
    CHECK(unscoped_bytes ==
          s_floor_lifetime.unscoped_requested_bytes);
    CHECK(current_bytes == s_floor_lifetime.current_requested_bytes);
    CHECK(memcmp(phase_count, s_floor_lifetime.phase_count,
                 sizeof phase_count) == 0);
    CHECK(memcmp(phase_bytes,
                 s_floor_lifetime.phase_requested_bytes,
                 sizeof phase_bytes) == 0);
    return 0;
}

static int check_backshift_sidecars(void)
{
    vita_heap_ledger_layout fixed_layout;

    CHECK(vita_heap_ledger_combined_layout(524288U, &fixed_layout));
    CHECK(fixed_layout.entry_bytes ==
              524288U * sizeof(vita_heap_ledger_entry) &&
          fixed_layout.phase_offset == fixed_layout.entry_bytes &&
          fixed_layout.phase_bytes == 0x80000U &&
          fixed_layout.current_offset ==
              fixed_layout.entry_bytes + 0x80000U &&
          fixed_layout.current_bytes == 0x10000U &&
          fixed_layout.combined_bytes ==
              fixed_layout.entry_bytes + 0x90000U);

#ifdef ISAAC_VITA_HEAP_LEDGER_BACKSHIFT
    {
    vita_heap_ledger_entry table[8];
    uint8_t phases[8];
    uint32_t current[1];
    uintptr_t keys[5];
    uint32_t tokens[5] = { 0U, 5U, 2U, 7U, 1U };
    uintptr_t next = 2U;
    size_t index;
    int found;
    size_t slot;

    memset(table, 0, sizeof table);
    memset(phases, 0, sizeof phases);
    memset(current, 0, sizeof current);
    for (index = 0U; index < 5U; ++index) {
        keys[index] = oracle_key_for_home(7U, 8U, next);
        CHECK(keys[index] != 0U);
        next = keys[index] + 1U;
        slot = vita_heap_find_slot(table, 8U, keys[index], &found);
        CHECK(!found && slot != SIZE_MAX);
        table[slot].base = keys[index];
        table[slot].requested_size = 0x20U + index;
        vita_heap_floor_lifetime_slot_store(
            slot, tokens[index], phases, current);
    }
    slot = vita_heap_find_slot(table, 8U, keys[2], &found);
    CHECK(found && vita_heap_ledger_remove_preflight_table(
        table, 8U, slot));
    vita_heap_ledger_remove_commit_table(
        table, 8U, slot, phases, current);
    for (index = 0U; index < 5U; ++index) {
        slot = vita_heap_find_slot(table, 8U, keys[index], &found);
        CHECK(!!found == (index != 2U));
        if (found) {
            CHECK(table[slot].requested_size == 0x20U + index);
            CHECK(vita_heap_floor_lifetime_slot_token(
                slot, phases, current) == tokens[index]);
        }
    }
    for (index = 0U; index < 8U; ++index)
        if (!table[index].base)
            CHECK(phases[index] == 0U &&
                  !vita_heap_floor_current_bit_get(current, index));
    }
#endif
    return 0;
}

static int check_failure_snapshot_combined_layout(void)
{
    vita_heap_ledger_layout layout;
    size_t capacity = SIZE_MAX;
    size_t count = SIZE_MAX;
    size_t tombstones = SIZE_MAX;
    size_t rehash_target_bytes = SIZE_MAX;
    size_t room_live = SIZE_MAX;

    CHECK(s_ledger_capacity == 0U && s_ledger_count == 0U);
    CHECK(vita_heap_ledger_combined_layout(
        VITA_HEAP_LEDGER_INITIAL_CAPACITY, &layout));
    vita_heap_failure_ledger_snapshot(
        &capacity, &count, &tombstones,
        &rehash_target_bytes, &room_live);
    CHECK(capacity == 0U && count == 0U && tombstones == 0U &&
          room_live == 0U &&
          rehash_target_bytes == layout.combined_bytes &&
          isaac_vita_guest_heap_test_next_rehash_bytes() ==
              layout.combined_bytes);
    return 0;
}

static int check_repeated_same_home_replace(void)
{
    void *allocation;
    uintptr_t key;
    uintptr_t next = 2U;
    size_t home;
    size_t iteration;
    size_t capacity;
    isaac_vita_guest_heap_floor_lifetime_snapshot snapshot;

    CHECK(oracle_reset_empty() == 0);
    CHECK(isaac_vita_guest_heap_test_floor_lifetime_bootstrap(
        ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT));
    allocation = isaac_vita_guest_malloc(16U);
    CHECK(allocation != NULL);
    key = (uintptr_t)allocation;
    capacity = s_ledger_capacity;
    CHECK(capacity == VITA_HEAP_LEDGER_INITIAL_CAPACITY);
    home = vita_heap_hash(key) & (capacity - 1U);

    for (iteration = 0U; iteration < 96U; ++iteration) {
        uintptr_t replacement = oracle_key_for_home(
            home, capacity, next);
        size_t requested_size = 0x100U + iteration;
        size_t slot;
        uint32_t token;

        CHECK(replacement > VITA_HEAP_LEDGER_TOMBSTONE &&
              replacement != key);
        next = replacement + 1U;
        vita_heap_lock();
        CHECK(vita_heap_ledger_lookup(key, &slot));
        token = vita_heap_floor_lifetime_slot_token(
            slot, s_ledger_floor_phase, s_ledger_floor_current);
        CHECK(token == (ISAAC_VITA_FLOOR_LIFETIME_PHASE_LEVEL_INIT |
                        VITA_HEAP_FLOOR_TOKEN_CURRENT));
        CHECK(vita_heap_ledger_remove_preflight(slot));
        CHECK(vita_heap_ledger_replace_commit(
            slot, replacement, requested_size));
        CHECK(s_ledger_tombstones == 0U);
        CHECK(vita_heap_ledger_lookup(replacement, &slot));
        CHECK(vita_heap_floor_lifetime_slot_token(
                  slot, s_ledger_floor_phase,
                  s_ledger_floor_current) == token);
        vita_heap_unlock();
        key = replacement;

        CHECK(isaac_vita_guest_heap_test_next_rehash_bytes() == 0U);
        CHECK(snapshot_get(&snapshot));
        CHECK(snapshot.total_count == 1U &&
              snapshot.unscoped_count == 0U &&
              snapshot.prior_count == 0U &&
              snapshot.current_count == 1U &&
              snapshot.total_requested_bytes == requested_size &&
              snapshot.current_requested_bytes == requested_size &&
              snapshot.phase_count[0] == 1U &&
              snapshot.phase_requested_bytes[0] == requested_size);
        CHECK(check_cold_sidecar_census() == 0);
    }

    vita_heap_lock();
    {
        size_t slot;

        CHECK(vita_heap_ledger_lookup(key, &slot));
        vita_heap_ledger_remove_slot_commit(slot);
    }
    vita_heap_unlock();
    free(allocation);
    CHECK(s_ledger_count == 0U && check_cold_sidecar_census() == 0);
    return 0;
}

static int check_level_lifecycle_and_realloc(void)
{
    void *pre[20];
    void *level[20];
    void *room;
    void *post_room;
    void *play;
    void *new_floor;
    void *retired = NULL;
    void *replacement;
    void *failed;
    size_t index;
    size_t slot;
    size_t huge = SIZE_MAX - 0x1000U;
    int valid_owner = 0;
    isaac_vita_guest_heap_floor_lifetime_snapshot snapshot;

    CHECK(oracle_reset_empty() == 0);
    for (index = 0U; index < 20U; ++index) {
        pre[index] = isaac_vita_guest_malloc(10U);
        CHECK(pre[index] != NULL);
    }
    CHECK(check_cold_sidecar_census() == 0);
    CHECK(!snapshot_get(&snapshot));
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 1U, 0U, UINT32_MAX);
    CHECK(s_oracle_log_count == 2U &&
          log_has(1U, "floorlife: q=1 e=level-begin epoch=1 "
                    "boot/roll=1/0 phase=level-init"));
    CHECK(log_has(1U, "heap=14/14/0/0,c8/c8/0/0"));
    CHECK(s_oracle_mallinfo_count == 1U &&
          s_oracle_mallinfo_current[0] == 0U);

    for (index = 0U; index < 20U; ++index) {
        level[index] = isaac_vita_guest_malloc(20U);
        CHECK(level[index] != NULL);
    }
    CHECK(s_ledger_capacity == 128U);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 2U, 0U, UINT32_MAX);
    room = isaac_vita_guest_malloc(30U);
    CHECK(room != NULL);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 2U, 0U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 2U, 0U, 1U);
    post_room = isaac_vita_guest_malloc(40U);
    CHECK(post_room != NULL);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 1U, 0U, UINT32_MAX);
    play = isaac_vita_guest_malloc(50U);
    CHECK(play != NULL);
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.total_count == 43U &&
          snapshot.unscoped_count == 20U &&
          snapshot.prior_count == 0U && snapshot.current_count == 23U);
    CHECK(snapshot.total_requested_bytes == 720U &&
          snapshot.unscoped_requested_bytes == 200U &&
          snapshot.current_requested_bytes == 520U);
    CHECK(snapshot.phase_count[0] == 21U &&
          snapshot.phase_count[1] == 1U &&
          snapshot.phase_count[2] == 1U);
    CHECK(snapshot.phase_requested_bytes[0] == 440U &&
          snapshot.phase_requested_bytes[1] == 30U &&
          snapshot.phase_requested_bytes[2] == 50U);
    CHECK(check_cold_sidecar_census() == 0);

    vita_heap_lock();
    CHECK(vita_heap_ledger_lookup((uintptr_t)level[2], &slot));
    CHECK(vita_heap_ledger_replace_commit(
        slot, (uintptr_t)level[2], 60U));
    vita_heap_unlock();
    replacement = isaac_vita_guest_heap_test_force_move(
        level[0], 25U, &retired);
    CHECK(replacement != NULL && retired == level[0]);
    free(retired);
    level[0] = replacement;
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.total_requested_bytes == 765U &&
          snapshot.current_requested_bytes == 565U &&
          snapshot.phase_requested_bytes[0] == 485U &&
          snapshot.phase_requested_bytes[2] == 50U);
    CHECK(check_cold_sidecar_census() == 0);

    failed = isaac_vita_guest_malloc(huge);
    CHECK(failed == NULL && snapshot_get(&snapshot));
    CHECK(snapshot.total_requested_bytes == 765U &&
          snapshot.current_requested_bytes == 565U);
    replacement = isaac_vita_guest_realloc(
        level[1], huge, &valid_owner);
    CHECK(replacement == NULL && valid_owner == 1);
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.total_requested_bytes == 765U &&
          snapshot.current_requested_bytes == 565U);
    CHECK(check_cold_sidecar_census() == 0);

    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 2U, 0U, UINT32_MAX);
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.epoch == 2U && snapshot.bootstrap_count == 1U &&
          snapshot.rollover_count == 1U && snapshot.prior_count == 23U &&
          snapshot.current_count == 0U &&
          snapshot.prior_requested_bytes == 565U &&
          snapshot.current_requested_bytes == 0U);
    CHECK(snapshot.phase_count[0] == 0U &&
          snapshot.phase_count[1] == 0U &&
          snapshot.phase_count[2] == 0U);
    CHECK(check_cold_sidecar_census() == 0);
    CHECK(s_oracle_mallinfo_current[s_oracle_mallinfo_count - 1U] == 0U);
    CHECK(log_has(s_oracle_log_count - 1U,
        "epoch=2 boot/roll=1/1 phase=level-init"));
    CHECK(log_has(s_oracle_log_count - 1U,
        "hphase=0/0/0,0/0/0"));

    new_floor = isaac_vita_guest_malloc(70U);
    CHECK(new_floor != NULL && snapshot_get(&snapshot));
    CHECK(snapshot.current_count == 1U &&
          snapshot.phase_count[0] == 1U &&
          snapshot.phase_requested_bytes[0] == 70U);

    /* Both replacement shapes now act on PRIOR births.  Neither may acquire
     * the new floor's CURRENT bit or level-init phase bucket. */
    vita_heap_lock();
    CHECK(vita_heap_ledger_lookup((uintptr_t)level[3], &slot));
    CHECK(vita_heap_ledger_replace_commit(
        slot, (uintptr_t)level[3], 22U));
    vita_heap_unlock();
    retired = NULL;
    replacement = isaac_vita_guest_heap_test_force_move(
        level[4], 24U, &retired);
    CHECK(replacement != NULL && retired == level[4]);
    free(retired);
    level[4] = replacement;
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.prior_count == 23U &&
          snapshot.prior_requested_bytes == 571U &&
          snapshot.current_count == 1U &&
          snapshot.current_requested_bytes == 70U &&
          snapshot.phase_count[0] == 1U &&
          snapshot.phase_requested_bytes[0] == 70U);
    CHECK(check_cold_sidecar_census() == 0);

    CHECK(isaac_vita_guest_free(room));
    CHECK(isaac_vita_guest_free(new_floor));
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.total_count == 42U && snapshot.prior_count == 22U &&
          snapshot.current_count == 0U &&
          snapshot.prior_requested_bytes == 541U);
    CHECK(check_cold_sidecar_census() == 0);

    for (index = 0U; index < 20U; ++index)
        CHECK(isaac_vita_guest_free(pre[index]));
    for (index = 0U; index < 20U; ++index)
        CHECK(isaac_vita_guest_free(level[index]));
    CHECK(isaac_vita_guest_free(post_room));
    CHECK(isaac_vita_guest_free(play));
    CHECK(s_ledger_count == 0U);
    CHECK(check_cold_sidecar_census() == 0);
    CHECK(s_oracle_log_under_lock == 0U &&
          s_oracle_mallinfo_under_lock == 0U &&
          s_oracle_log_truncations == 0U);
    return 0;
}

static int check_continue_bootstrap(void)
{
    void *pre;
    void *room;
    void *play;
    isaac_vita_guest_heap_floor_lifetime_snapshot snapshot;

    CHECK(oracle_reset_empty() == 0);
    pre = isaac_vita_guest_malloc(11U);
    CHECK(pre != NULL);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_BEGIN, 4U, 0U, UINT32_MAX);
    CHECK(snapshot_get(&snapshot));
    CHECK(snapshot.epoch == 1U && snapshot.phase ==
          ISAAC_VITA_FLOOR_LIFETIME_PHASE_ROOM_LOAD &&
          snapshot.unscoped_count == 1U && snapshot.current_count == 0U);
    room = isaac_vita_guest_malloc(12U);
    CHECK(room != NULL);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_AFTER_UNLOAD, 4U, 0U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_ROOM_LOAD_END, 4U, 0U, 1U);
    play = isaac_vita_guest_malloc(13U);
    CHECK(play != NULL && snapshot_get(&snapshot));
    CHECK(snapshot.phase == ISAAC_VITA_FLOOR_LIFETIME_PHASE_PLAY &&
          snapshot.phase_count[0] == 0U &&
          snapshot.phase_count[1] == 1U &&
          snapshot.phase_count[2] == 1U);
    CHECK(check_cold_sidecar_census() == 0);
    CHECK(isaac_vita_guest_free(pre));
    CHECK(isaac_vita_guest_free(room));
    CHECK(isaac_vita_guest_free(play));
    CHECK(check_cold_sidecar_census() == 0);
    return 0;
}

static int check_invalid_order_and_saturation(void)
{
    void *pointer;

    CHECK(oracle_reset_empty() == 0);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 1U, 0U, UINT32_MAX);
    CHECK(log_has(1U, "epoch=0 boot/roll=0/0 phase=pre-floor"));
    CHECK(log_has(1U, "valid/term/sat=0/0/0"));
    pointer = isaac_vita_guest_malloc(9U);
    CHECK(pointer != NULL &&
          s_floor_lifetime.unscoped_count == 1U &&
          s_floor_lifetime.current_count == 0U);
    CHECK(isaac_vita_guest_free(pointer));

    CHECK(oracle_reset_empty() == 0);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 1U, 0U, UINT32_MAX);
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_END, 1U, 0U, UINT32_MAX);
    s_floor_lifetime.epoch = UINT32_MAX;
    s_floor_lifetime.rollover_count = UINT32_MAX - 1U;
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 2U, 0U, UINT32_MAX);
    CHECK(log_has(s_oracle_log_count - 1U,
        "epoch=ffffffff boot/roll=1/fffffffe phase=pre-floor"));
    CHECK(log_has(s_oracle_log_count - 1U,
        "valid/term/sat=0/0/1"));
    CHECK(s_oracle_log_truncations == 0U);
    return 0;
}

int main(void)
{
    if (check_backshift_sidecars() ||
        check_failure_snapshot_combined_layout() ||
        check_repeated_same_home_replace() ||
        check_level_lifecycle_and_realloc() ||
        check_continue_bootstrap() ||
        check_invalid_order_and_saturation())
        return 1;
    puts("generic floor-lifetime ledger oracle: PASS");
    return 0;
}
