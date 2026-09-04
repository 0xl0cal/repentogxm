/* Bounded attribution for blocking gameplay -> menu rebuilds.
 *
 * A pre-helper token proves the Manager call began with a live Game.  The
 * helper's exact nonzero work byte qualifies the bracket before any clock read;
 * a sample is promoted only when a frozen direct SetSaveSlot or MenuManager
 * constructor edge proves that the selected helper path performs real work.
 * Up to four completed
 * rebuilds are reported so an unexpected startup match cannot hide Exit.
 * Child calls, GameState records, CRT fread and checksum feed work are
 * aggregated; no individual call writes a log record.
 */
#include "kage_vita_exit_menu_profile.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ISAAC_VITA_EXIT_MENU_PROFILE)
# if defined(ISAAC_VITA_EXIT_MENU_PROFILE_ORACLE)
#  include "kage_vita_exit_menu_profile_test_platform.h"
# else
#  include <psp2/kernel/clib.h>
#  include <psp2/kernel/processmgr.h>
#  include <psp2/kernel/threadmgr.h>
# endif

# if defined(ISAAC_VITA_EXIT_MENU_PROFILE_ORACLE)
#  define exit_menu_profile_log sceClibPrintf
# else
void isaac_vita_log(const char *format, ...);
#  define exit_menu_profile_log isaac_vita_log
# endif

#define EXIT_MENU_FAULT_ORDER       0x00000001u
#define EXIT_MENU_FAULT_CLOCK       0x00000002u
#define EXIT_MENU_FAULT_SATURATED   0x00000004u
#define EXIT_MENU_FAULT_ARITHMETIC  0x00000008u
#define EXIT_MENU_FAULT_RECORDS     0x00000010u
#define EXIT_MENU_FAULT_SLOT        0x00000020u

#define EXIT_MENU_RECORDS 4u
#define EXIT_MENU_REBUILDS 4u
#define EXIT_MENU_NO_RECORD UINT32_MAX

typedef struct ExitMenuSpan {
    uint64_t begin;
    uint64_t total;
    uint64_t maximum;
    uint32_t count;
    uint32_t open;
} ExitMenuSpan;

typedef struct ExitMenuRecord {
    ExitMenuSpan persistent;
    ExitMenuSpan gamestate;
    ExitMenuSpan read;
    ExitMenuSpan fread_native;
    ExitMenuSpan checksum;
    uint64_t fread_requested;
    uint64_t fread_returned;
    uint64_t checksum_bytes;
    uint32_t slot;
} ExitMenuRecord;

typedef struct ExitMenuState {
    ExitMenuSpan total;
    ExitMenuSpan set_save;
    ExitMenuSpan ctor;
    ExitMenuSpan init;
    ExitMenuSpan post;
    ExitMenuRecord record[EXIT_MENU_RECORDS];
    uint32_t record_count;
    uint32_t record_overflow;
    uint32_t persistent_record;
    uint32_t gamestate_record;
    uint32_t read_record;
    uint32_t faults;
} ExitMenuState;

static ExitMenuState s_exit;
static uint64_t s_candidate_begin;
static uint32_t s_candidate_owner;
static uint32_t s_candidate_open;
static uint32_t s_candidate_faults;
static uint32_t s_snapshot_owner;
static uint32_t s_snapshot_live;
static uint32_t s_snapshot_valid;
static uint32_t s_active_owner;
static uint32_t s_active;
static uint32_t s_completed;
static uint32_t s_consumed;

# if defined(ISAAC_VITA_EXIT_MENU_PROFILE_ORACLE)
void isaac_vita_exit_menu_profile_oracle_reset(void)
{
    memset(&s_exit, 0, sizeof s_exit);
    s_candidate_begin = 0u;
    s_candidate_owner = 0u;
    s_candidate_open = 0u;
    s_candidate_faults = 0u;
    s_snapshot_owner = 0u;
    s_snapshot_live = 0u;
    s_snapshot_valid = 0u;
    s_active_owner = 0u;
    s_active = 0u;
    s_completed = 0u;
    s_consumed = 0u;
}
# endif

static uint32_t exit_menu_atomic_load(const uint32_t *value)
{
# if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
# else
    return *value;
# endif
}

static void exit_menu_atomic_store(uint32_t *value, uint32_t replacement)
{
# if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(value, replacement, __ATOMIC_RELEASE);
# else
    *value = replacement;
# endif
}

static uint64_t exit_menu_now(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static uint32_t exit_menu_thread(void)
{
    return (uint32_t)sceKernelGetThreadId();
}

static void exit_menu_fault(uint32_t *faults, uint32_t bit)
{
    *faults |= bit;
}

static void exit_menu_increment(uint32_t *value, uint32_t *faults)
{
    if (*value != UINT32_MAX)
        ++*value;
    else
        exit_menu_fault(faults, EXIT_MENU_FAULT_SATURATED);
}

static void exit_menu_add(uint64_t *total, uint64_t value,
                          uint32_t *faults)
{
    if (UINT64_MAX - *total < value) {
        *total = UINT64_MAX;
        exit_menu_fault(faults, EXIT_MENU_FAULT_SATURATED);
    } else {
        *total += value;
    }
}

static void exit_menu_span_begin(ExitMenuSpan *span, uint64_t now,
                                 uint32_t *faults)
{
    if (span->open) {
        exit_menu_fault(faults, EXIT_MENU_FAULT_ORDER);
        return;
    }
    span->begin = now;
    span->open = 1u;
}

static uint64_t exit_menu_span_end(ExitMenuSpan *span, uint64_t now,
                                   uint32_t *faults)
{
    uint64_t elapsed;

    if (!span->open) {
        exit_menu_fault(faults, EXIT_MENU_FAULT_ORDER);
        return 0u;
    }
    span->open = 0u;
    if (now < span->begin) {
        elapsed = 0u;
        exit_menu_fault(faults, EXIT_MENU_FAULT_CLOCK);
    } else {
        elapsed = now - span->begin;
    }
    exit_menu_add(&span->total, elapsed, faults);
    if (elapsed > span->maximum)
        span->maximum = elapsed;
    exit_menu_increment(&span->count, faults);
    return elapsed;
}

static uint32_t exit_menu_us32(uint64_t value, uint32_t *faults)
{
    if (value > UINT32_MAX) {
        exit_menu_fault(faults, EXIT_MENU_FAULT_SATURATED);
        return UINT32_MAX;
    }
    return (uint32_t)value;
}

static uint32_t exit_menu_residual(uint64_t total, const uint64_t *parts,
                                   size_t count, uint32_t *faults)
{
    uint64_t used = 0u;
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (UINT64_MAX - used < parts[index]) {
            exit_menu_fault(faults, EXIT_MENU_FAULT_ARITHMETIC);
            return UINT32_MAX;
        }
        used += parts[index];
    }
    if (used > total) {
        exit_menu_fault(faults, EXIT_MENU_FAULT_ARITHMETIC);
        return UINT32_MAX;
    }
    return exit_menu_us32(total - used, faults);
}

static int exit_menu_owner(void)
{
    return exit_menu_atomic_load(&s_active) &&
        exit_menu_thread() == exit_menu_atomic_load(&s_active_owner);
}

static uint32_t exit_menu_new_record(uint32_t slot)
{
    uint32_t index;

    if (s_exit.record_count >= EXIT_MENU_RECORDS) {
        exit_menu_increment(&s_exit.record_overflow, &s_exit.faults);
        exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_RECORDS);
        return EXIT_MENU_NO_RECORD;
    }
    index = s_exit.record_count++;
    memset(&s_exit.record[index], 0, sizeof s_exit.record[index]);
    s_exit.record[index].slot = slot;
    return index;
}

static ExitMenuRecord *exit_menu_record(uint32_t index)
{
    if (index >= s_exit.record_count || index >= EXIT_MENU_RECORDS)
        return NULL;
    return &s_exit.record[index];
}

static uint32_t exit_menu_record_for_gamestate(uint32_t slot)
{
    ExitMenuRecord *record;
    uint32_t index;

    if (s_exit.record_count != 0u) {
        index = s_exit.record_count - 1u;
        record = &s_exit.record[index];
        if (record->gamestate.count == 0u && !record->gamestate.open) {
            if (record->slot != slot)
                exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_SLOT);
            else
                return index;
        }
    }
    return exit_menu_new_record(slot);
}

static void exit_menu_force_close(ExitMenuSpan *span, uint64_t now)
{
    if (span->open) {
        exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_ORDER);
        (void)exit_menu_span_end(span, now, &s_exit.faults);
    }
}

static void exit_menu_report_record(uint32_t rebuild, uint32_t index)
{
    ExitMenuRecord *record = &s_exit.record[index];
    uint64_t load_parts[1];
    uint64_t read_parts[2];
    uint32_t load_other;
    uint32_t read_other;
    uint32_t persistent_us;
    uint32_t gamestate_us;
    uint32_t read_us;
    uint32_t fread_requested;
    uint32_t fread_returned;
    uint32_t fread_total_us;
    uint32_t fread_max_us;
    uint32_t checksum_bytes;
    uint32_t checksum_total_us;
    uint32_t checksum_max_us;

    load_parts[0] = record->read.total;
    read_parts[0] = record->fread_native.total;
    read_parts[1] = record->checksum.total;
    load_other = exit_menu_residual(
        record->gamestate.total, load_parts, 1u, &s_exit.faults);
    read_other = exit_menu_residual(
        record->read.total, read_parts, 2u, &s_exit.faults);
    /* Each conversion may set the saturation bit.  Keep the writes
     * sequenced instead of placing them in one variadic argument list. */
    persistent_us = exit_menu_us32(
        record->persistent.total, &s_exit.faults);
    gamestate_us = exit_menu_us32(
        record->gamestate.total, &s_exit.faults);
    read_us = exit_menu_us32(record->read.total, &s_exit.faults);
    fread_requested = exit_menu_us32(
        record->fread_requested, &s_exit.faults);
    fread_returned = exit_menu_us32(
        record->fread_returned, &s_exit.faults);
    fread_total_us = exit_menu_us32(
        record->fread_native.total, &s_exit.faults);
    fread_max_us = exit_menu_us32(
        record->fread_native.maximum, &s_exit.faults);
    checksum_bytes = exit_menu_us32(
        record->checksum_bytes, &s_exit.faults);
    checksum_total_us = exit_menu_us32(
        record->checksum.total, &s_exit.faults);
    checksum_max_us = exit_menu_us32(
        record->checksum.maximum, &s_exit.faults);
    exit_menu_profile_log(
        "[isaac-exit] rebuild=%u gs#%u slot=%u pd=%u gs=%u read=%u "
        "load_other=%u read_other=%u fread=%u/%u/%u/%u/%u "
        "checksum=%u/%u/%u/%u",
        (unsigned)rebuild, (unsigned)(index + 1u),
        (unsigned)record->slot,
        (unsigned)persistent_us, (unsigned)gamestate_us,
        (unsigned)read_us,
        (unsigned)load_other, (unsigned)read_other,
        (unsigned)record->fread_native.count,
        (unsigned)fread_requested, (unsigned)fread_returned,
        (unsigned)fread_total_us, (unsigned)fread_max_us,
        (unsigned)record->checksum.count,
        (unsigned)checksum_bytes, (unsigned)checksum_total_us,
        (unsigned)checksum_max_us);
}

static void exit_menu_report(uint64_t now)
{
    uint64_t top_parts[4];
    uint32_t index;
    uint32_t total;
    uint32_t other;
    uint32_t set_save;
    uint32_t ctor;
    uint32_t init;
    uint32_t post;
    uint32_t rebuild;

    /* Publish closed ownership before touching/reporting aggregates so a
     * foreign CRT thread cannot enter while the bounded record is finalized. */
    exit_menu_atomic_store(&s_active, 0u);
    exit_menu_atomic_store(&s_active_owner, 0u);
    for (index = 0u; index < s_exit.record_count; ++index) {
        ExitMenuRecord *record = &s_exit.record[index];
        exit_menu_force_close(&record->checksum, now);
        exit_menu_force_close(&record->fread_native, now);
        exit_menu_force_close(&record->read, now);
        exit_menu_force_close(&record->persistent, now);
        exit_menu_force_close(&record->gamestate, now);
    }
    exit_menu_force_close(&s_exit.set_save, now);
    exit_menu_force_close(&s_exit.ctor, now);
    exit_menu_force_close(&s_exit.init, now);
    exit_menu_force_close(&s_exit.post, now);
    (void)exit_menu_span_end(&s_exit.total, now, &s_exit.faults);

    top_parts[0] = s_exit.set_save.total;
    top_parts[1] = s_exit.ctor.total;
    top_parts[2] = s_exit.init.total;
    top_parts[3] = s_exit.post.total;
    other = exit_menu_residual(
        s_exit.total.total, top_parts, 4u, &s_exit.faults);

    /* Close ownership before writing bounded diagnostic output. */
    s_candidate_open = 0u;
    rebuild = s_completed + 1u;
    s_completed = rebuild;
    if (s_completed >= EXIT_MENU_REBUILDS)
        exit_menu_atomic_store(&s_consumed, 1u);
    for (index = 0u; index < s_exit.record_count; ++index)
        exit_menu_report_record(rebuild, index);

    /* Emit the header last so it carries faults found while formatting every
     * bounded child record as well as faults found while closing spans. */
    total = exit_menu_us32(s_exit.total.total, &s_exit.faults);
    set_save = exit_menu_us32(s_exit.set_save.total, &s_exit.faults);
    ctor = exit_menu_us32(s_exit.ctor.total, &s_exit.faults);
    init = exit_menu_us32(s_exit.init.total, &s_exit.faults);
    post = exit_menu_us32(s_exit.post.total, &s_exit.faults);
    exit_menu_profile_log(
        "[isaac-exit] rebuild=%u total=%u set=%u ctor=%u init=%u post=%u "
        "other=%u records=%u+%u faults=0x%02x",
        (unsigned)rebuild, (unsigned)total, (unsigned)set_save,
        (unsigned)ctor,
        (unsigned)init, (unsigned)post,
        (unsigned)other, (unsigned)s_exit.record_count,
        (unsigned)s_exit.record_overflow, (unsigned)s_exit.faults);
}

void isaac_vita_exit_menu_profile_note(uint32_t event, uint32_t value)
{
    ExitMenuRecord *record;
    uint64_t now;
    uint32_t index;

    if (exit_menu_atomic_load(&s_consumed))
        return;

    if (event == ISAAC_VITA_EXIT_MENU_GAME_SNAPSHOT) {
        s_snapshot_owner = exit_menu_thread();
        s_snapshot_live = value != 0u;
        s_snapshot_valid = 1u;
        return;
    }
    if (event == ISAAC_VITA_EXIT_MENU_CANDIDATE_BEGIN) {
        uint32_t owner = exit_menu_thread();
        uint32_t qualified = s_snapshot_valid && s_snapshot_live &&
            value != 0u && owner == s_snapshot_owner;
        uint32_t overlapping = s_candidate_open;
        /* The token is adjacent to this hook in the frozen Manager body and
         * is consumed even on rejection; it can never qualify a later call. */
        s_snapshot_valid = 0u;
        s_snapshot_live = 0u;
        s_snapshot_owner = 0u;
        if (!qualified) {
            s_candidate_open = 0u;
            return;
        }
        s_candidate_begin = exit_menu_now();
        s_candidate_owner = owner;
        s_candidate_open = 1u;
        s_candidate_faults = overlapping ? EXIT_MENU_FAULT_ORDER : 0u;
        return;
    }
    if ((event == ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN ||
            event == ISAAC_VITA_EXIT_MENU_CTOR_BEGIN) &&
            !exit_menu_atomic_load(&s_active)) {
        if (!s_candidate_open || exit_menu_thread() != s_candidate_owner)
            return;
        memset(&s_exit, 0, sizeof s_exit);
        s_exit.persistent_record = EXIT_MENU_NO_RECORD;
        s_exit.gamestate_record = EXIT_MENU_NO_RECORD;
        s_exit.read_record = EXIT_MENU_NO_RECORD;
        s_exit.faults = s_candidate_faults;
        s_exit.total.begin = s_candidate_begin;
        s_exit.total.open = 1u;
        exit_menu_atomic_store(&s_active_owner, s_candidate_owner);
        exit_menu_atomic_store(&s_active, 1u);
        now = exit_menu_now();
        if (event == ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN)
            exit_menu_span_begin(&s_exit.set_save, now, &s_exit.faults);
        else
            exit_menu_span_begin(&s_exit.ctor, now, &s_exit.faults);
        return;
    }
    if (event == ISAAC_VITA_EXIT_MENU_CANDIDATE_END) {
        if (!s_candidate_open || exit_menu_thread() != s_candidate_owner)
            return;
        now = exit_menu_now();
        if (exit_menu_atomic_load(&s_active) && s_exit.record_count != 0u) {
            exit_menu_report(now);
        } else {
            exit_menu_atomic_store(&s_active, 0u);
            exit_menu_atomic_store(&s_active_owner, 0u);
            s_candidate_open = 0u;
        }
        return;
    }
    if (!exit_menu_owner())
        return;

    now = exit_menu_now();
    switch (event) {
    case ISAAC_VITA_EXIT_MENU_SET_SAVE_BEGIN:
        exit_menu_span_begin(&s_exit.set_save, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_SET_SAVE_END:
        (void)exit_menu_span_end(&s_exit.set_save, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_CTOR_BEGIN:
        exit_menu_span_begin(&s_exit.ctor, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_CTOR_END:
        (void)exit_menu_span_end(&s_exit.ctor, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_INIT_BEGIN:
        exit_menu_span_begin(&s_exit.init, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_INIT_END:
        (void)exit_menu_span_end(&s_exit.init, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_POST_BEGIN:
        exit_menu_span_begin(&s_exit.post, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_POST_END:
        (void)exit_menu_span_end(&s_exit.post, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_PERSISTENT_BEGIN:
        index = exit_menu_new_record(value);
        s_exit.persistent_record = index;
        record = exit_menu_record(index);
        if (record != NULL)
            exit_menu_span_begin(&record->persistent, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_PERSISTENT_END:
        record = exit_menu_record(s_exit.persistent_record);
        if (record == NULL)
            exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_ORDER);
        else
            (void)exit_menu_span_end(
                &record->persistent, now, &s_exit.faults);
        s_exit.persistent_record = EXIT_MENU_NO_RECORD;
        break;
    case ISAAC_VITA_EXIT_MENU_GAMESTATE_BEGIN:
        index = exit_menu_record_for_gamestate(value);
        s_exit.gamestate_record = index;
        record = exit_menu_record(index);
        if (record != NULL)
            exit_menu_span_begin(&record->gamestate, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_GAMESTATE_END:
        record = exit_menu_record(s_exit.gamestate_record);
        if (record == NULL)
            exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_ORDER);
        else
            (void)exit_menu_span_end(
                &record->gamestate, now, &s_exit.faults);
        s_exit.gamestate_record = EXIT_MENU_NO_RECORD;
        break;
    case ISAAC_VITA_EXIT_MENU_READ_BEGIN:
        s_exit.read_record = s_exit.gamestate_record;
        record = exit_menu_record(s_exit.read_record);
        if (record == NULL || !record->gamestate.open)
            exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_ORDER);
        else
            exit_menu_span_begin(&record->read, now, &s_exit.faults);
        break;
    case ISAAC_VITA_EXIT_MENU_READ_END:
        record = exit_menu_record(s_exit.read_record);
        if (record == NULL)
            exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_ORDER);
        else
            (void)exit_menu_span_end(&record->read, now, &s_exit.faults);
        s_exit.read_record = EXIT_MENU_NO_RECORD;
        break;
    case ISAAC_VITA_EXIT_MENU_CHECKSUM_BEGIN:
        record = exit_menu_record(s_exit.read_record);
        if (record != NULL && record->read.open) {
            exit_menu_add(&record->checksum_bytes, value, &s_exit.faults);
            exit_menu_span_begin(&record->checksum, now, &s_exit.faults);
        }
        break;
    case ISAAC_VITA_EXIT_MENU_CHECKSUM_END:
        record = exit_menu_record(s_exit.read_record);
        if (record != NULL && record->read.open)
            (void)exit_menu_span_end(
                &record->checksum, now, &s_exit.faults);
        break;
    default:
        exit_menu_fault(&s_exit.faults, EXIT_MENU_FAULT_ORDER);
        break;
    }
}

void isaac_vita_exit_menu_profile_fread_begin(uint32_t requested_bytes)
{
    ExitMenuRecord *record;
    uint64_t now;

    if (!exit_menu_owner())
        return;
    record = exit_menu_record(s_exit.read_record);
    if (record == NULL || !record->read.open)
        return;
    now = exit_menu_now();
    exit_menu_add(
        &record->fread_requested, requested_bytes, &s_exit.faults);
    exit_menu_span_begin(&record->fread_native, now, &s_exit.faults);
}

void isaac_vita_exit_menu_profile_fread_end(uint32_t returned_bytes)
{
    ExitMenuRecord *record;
    uint64_t now;

    if (!exit_menu_owner())
        return;
    record = exit_menu_record(s_exit.read_record);
    if (record == NULL || !record->read.open)
        return;
    now = exit_menu_now();
    exit_menu_add(&record->fread_returned, returned_bytes, &s_exit.faults);
    (void)exit_menu_span_end(
        &record->fread_native, now, &s_exit.faults);
}

#else

void isaac_vita_exit_menu_profile_note(uint32_t event, uint32_t value)
{
    (void)event;
    (void)value;
}

void isaac_vita_exit_menu_profile_fread_begin(uint32_t requested_bytes)
{
    (void)requested_bytes;
}

void isaac_vita_exit_menu_profile_fread_end(uint32_t returned_bytes)
{
    (void)returned_bytes;
}

#endif
