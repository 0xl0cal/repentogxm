#ifndef KAGE_VITA_IO_PROFILE_H
#define KAGE_VITA_IO_PROFILE_H

#include <stdint.h>

#if defined(ISAAC_VITA_IO_WINDOW_PROFILE)
#if defined(ISAAC_VITA_IO_PROFILE)
#error IO_WINDOW_PROFILE and startup IO_PROFILE have exclusive native wrappers
#endif

#define KAGE_VITA_IO_WINDOW_ABI 1U
enum {
    KAGE_IO_OPEN, KAGE_IO_CLOSE, KAGE_IO_READ, KAGE_IO_PREAD,
    KAGE_IO_SEEK32, KAGE_IO_SEEK64,
    KAGE_IO_WRITE, KAGE_IO_PWRITE, KAGE_IO_SYNC, KAGE_IO_SYNC_BY_FD,
    KAGE_IO_OP_COUNT
};
enum {
    KAGE_IO_UNKNOWN, KAGE_IO_ARCHIVE, KAGE_IO_RESOURCE, KAGE_IO_SAVE,
    KAGE_IO_CONFIG, KAGE_IO_SHADER, KAGE_IO_LOG, KAGE_IO_OTHER,
    KAGE_IO_CLASS_COUNT
};
typedef struct kage_vita_io_window_operation {
    uint32_t calls;
    uint32_t errors;
    uint64_t requested_bytes;
    uint64_t returned_bytes; /* native bytes read or written, not item count */
    uint64_t time_us;
} kage_vita_io_window_operation;
typedef struct kage_vita_io_window_snapshot {
    uint32_t abi_version;
    uint32_t snapshot_valid;
    uint32_t take_misses;
    uint32_t record_drops;
    uint32_t map_drops;
    uint32_t clock_reversals;
    uint32_t map_collisions;
    uint32_t unknown_calls;
    kage_vita_io_window_operation op[KAGE_IO_OP_COUNT];
    uint64_t class_us[KAGE_IO_CLASS_COUNT];
    uint64_t max_us;
    uint32_t max_op;
    uint32_t max_class;
    uint32_t max_requested_bytes;
    int32_t max_error; /* zero on success, negative native error otherwise */
} kage_vita_io_window_snapshot;

/* Physical API durations, inclusive and across all calling threads: not CPU
 * time and not a logical fread profile.  Two clock reads per native operation;
 * no clock/formatting/allocation in take().  An operation is charged to the
 * window in which it completes, even if it began in the preceding window.
 * Class mapping survives take; its fixed 64-slot collisions become UNKNOWN,
 * never the class of a different descriptor.  A caller may discard the first
 * snapshot to exclude startup.  Contention NEVER waits: a failed take returns
 * snapshot_valid=0 without resetting data; the next valid result therefore
 * spans 1+take_misses attempted windows.  record_drops explicitly counts lost
 * operation records; map_drops means attribution became UNKNOWN.  An open or
 * close mapping loss invalidates the map before subsequent lookup, preventing
 * stale-fd class reuse.  Classes are prior observed path classes, not proof
 * against a caller concurrently closing/reusing the same descriptor.
 * Whole-device Sync is UNKNOWN class; SyncByFd uses the observed fd class.
 * Logger writes are LOG when mapped, with no recursive per-operation log.
 * No logs, clock reads, allocation, or scheduler-blocking locks here. */
void kage_vita_io_window_take(kage_vita_io_window_snapshot *out);
#endif

/* Optional startup-only native I/O profiler.  It is linked with
 * --wrap=sceIoRead/--wrap=sceIoLseek32/--wrap=sceIoLseek only in a diagnostic
 * build, so release builds pay no per-I/O clock or atomic cost. */
void kage_vita_io_profile_begin(void);
void kage_vita_io_profile_report(const char *reason);
/* Called only at bounded completed-fread checkpoints.  It leaves the startup
 * epoch active and emits at most one compact progress pair per checkpoint. */
void kage_vita_io_profile_progress(uint32_t completed_calls);

/* Texture uploads are rare enough to time individually, unlike the two
 * million logical fread calls.  KIND is zero for allocation/upload and one
 * for sub-image replacement. */
void kage_vita_io_profile_texture_begin(
    uint32_t kind, int32_t width, int32_t height);
void kage_vita_io_profile_texture_end(void);

#endif
