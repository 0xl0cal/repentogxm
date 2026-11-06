/* Write-behind lane for the guest's own game log (Documents/My Games/Binding
 * of Isaac Repentance/log.txt).
 *
 * Measured rationale (Mom's Heart death, 2026-09-10, build nt2p, ph120
 * windows 90/91): the frozen logger sub_0055e330 formats one record, calls
 * the fwrite adapter 0x00596590 and then the fflush adapter 0x005965c0 for
 * every record.  With the game log a 1 KiB-buffered newlib FILE that fflush
 * is one synchronous sceIoWrite of 15-25 ms on the game thread; the two
 * windows paid 45.5 ms (2 calls) and 28.6 ms (2 calls) for it inside a
 * 128 ms and a 317 ms main-loop stall.
 *
 * With ISAAC_VITA_GAME_LOG_ASYNC the CRT adopts the log FILE as this
 * module's lane: fwrite is a memcpy into one static byte ring, one worker
 * thread appends the ring to the file (open/append/close per span), an
 * INFO fflush returns without storage I/O, and a WARN/ERROR/ASSERT fflush
 * (severity read from the frozen logger chain by the CRT) waits for the span
 * in flight, then appends the pending bytes itself before control returns.
 * Spans are always claimed contiguously from the ring head and exactly one
 * drainer appends at a time, so the file content and line order are the
 * synchronous result.  ftell/fseek to the current end answer from the
 * tracked logical size; anything else (another fseek, fopen of the same
 * path, remove/_access/GetFileAttributesA of the path, FindFirstFileW of the
 * directory) first drains the lane, so the guest always reads its writes.
 *
 * What log.txt can lose versus the synchronous per-record flush (reference:
 * "today" = release build, ISAAC_VITA_GAME_LOG_BATCH ON, this lane OFF):
 *
 *   controlled guest exit          lane: nothing (flush_all drains)
 *                                  today: nothing
 *   guest _exit, guest stop,       lane: nothing (entry tail drains first)
 *   controlled fault with scope    today: up to 7 deferred INFO + 1 KiB buffer
 *   abort()/failed assert/fault    lane: nothing unless the lane lock cannot
 *   without run scope                    be taken or the worker is mid-append
 *                                        > 2 s; then the INFO records since
 *                                        the last completed append
 *                                  today: up to 7 deferred INFO + 1 KiB buffer
 *   native data abort, kernel      lane: INFO records not yet appended (the
 *   kill, power loss                     span in flight plus records since
 *                                        the worker's last wake; typically
 *                                        the last 1-8 lines, at most the
 *                                        ring); never a WARN/ERROR/ASSERT
 *                                        record nor anything before it
 *                                        (their fflush waited)
 *                                  today: up to 7 deferred INFO + 1 KiB buffer
 *   storage failure on append      lane: the span is retried once, then
 *                                        dropped (lost_bytes), ferror sticky,
 *                                        next fflush/fclose returns EOF
 *                                  today: newlib drops its buffer, EOF
 *   suspend/PS-button ENODEV       lane: nothing (fresh descriptor per span)
 *                                  today: log.txt stops growing
 *   worker wedged > 10 s while     lane: stranded span abandoned in place
 *   the guest waits                      (may land late), rest appended by
 *                                        the guest, lane synchronous forever
 *
 * ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY (default 1 = WARN) is the lowest
 * severity whose fflush appends synchronously; 0 makes every record durable
 * at fflush, 4 defers every chain severity (a flush outside the frozen chain
 * is always synchronous). */
#ifndef ISAAC_HOST_VITA_GAME_LOG_H
#define ISAAC_HOST_VITA_GAME_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The startup path mapper bounds native paths at 1024 bytes. */
#define ISAAC_VITA_GAME_LOG_PATH_MAX            1024U
#ifndef ISAAC_VITA_GAME_LOG_ASYNC_RING_KB
# define ISAAC_VITA_GAME_LOG_ASYNC_RING_KB      64U
#endif
#ifndef ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY
# define ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY 1U
#endif
#define ISAAC_VITA_GAME_LOG_RING_BYTES \
    (ISAAC_VITA_GAME_LOG_ASYNC_RING_KB * 1024U)
/* The frozen logger formats into a fixed 0x2800-byte buffer
 * (ISAAC_VITA_CRT_ROOM_LOG_BUFFER_VA..END), so one record is at most this. */
#define ISAAC_VITA_GAME_LOG_RECORD_MAX          10240U
/* Pseudo _fileno for the lane token: one per slot, above the async-save
 * range (ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE 0x00400000U + 16). */
#define ISAAC_VITA_CRT_GAME_LOG_FD_BASE         0x00410000U
/* isaac_vita_game_log_seek: the target is not the current end; the CRT hands
 * the lane to a newlib FILE and lets newlib perform the seek. */
#define ISAAC_VITA_GAME_LOG_SEEK_DEMOTE         (-2)
/* Severities 0..3 are the frozen logger's INFO/WARN/ERROR/ASSERT; 4 marks a
 * flush that did not come through the frozen chain (always synchronous). */
#define ISAAC_VITA_GAME_LOG_SEVERITY_UNKNOWN    4U
#define ISAAC_VITA_GAME_LOG_SNAPSHOT_ABI        2U

_Static_assert(ISAAC_VITA_GAME_LOG_RING_BYTES >=
                   2U * ISAAC_VITA_GAME_LOG_RECORD_MAX + 64U,
               "ring must hold two maximal logger records");
_Static_assert(ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY <= 4U,
               "durable severity is 0..4");

enum {
    ISAAC_VITA_GAME_LOG_WORKER_COLD = 0,
    ISAAC_VITA_GAME_LOG_WORKER_READY = 1,
    ISAAC_VITA_GAME_LOG_WORKER_FAILED = 2,
    ISAAC_VITA_GAME_LOG_WORKER_STOPPED = 3
};

/* Caller identity for the path-coherence drains (summary coherence(...)). */
enum {
    ISAAC_VITA_GAME_LOG_SYNC_REMOVE = 0,
    ISAAC_VITA_GAME_LOG_SYNC_ACCESS = 1,
    ISAAC_VITA_GAME_LOG_SYNC_ATTRIB = 2
};

/* Why the CRT hands the lane to newlib (isaac_vita_game_log_release). */
enum {
    ISAAC_VITA_GAME_LOG_RELEASE_SEEK = 0,
    ISAAC_VITA_GAME_LOG_RELEASE_OPEN = 1
};

typedef struct isaac_vita_game_log_snapshot {
    uint32_t abi;
    uint32_t worker_state;      /* ISAAC_VITA_GAME_LOG_WORKER_* */
    uint32_t start_failed;
    uint32_t live;
    uint32_t synchronous;
    uint32_t error;
    uint32_t pending_eof;
    uint32_t head;              /* ring cursors, oracle inspection */
    uint32_t tail;
    uint32_t used;
    uint32_t claimed;
    uint32_t draining;
    uint32_t generation;
    uint32_t worker_idle;
    int64_t logical_size;
    uint32_t records;
    uint32_t bytes;
    uint32_t flush[6];          /* info, warn, error, assert, other, null */
    uint32_t appends[5];        /* worker, guest, exit, abort, direct */
    uint32_t io_us_worker;
    uint32_t io_us_worker_max;
    uint32_t io_us_guest;
    uint32_t io_us_guest_max;
    uint32_t waits;
    uint32_t wait_us;
    uint32_t wait_us_max;
    uint32_t high;
    uint32_t full_waits;
    uint32_t coherence[7];      /* open, remove, access, attrib, find, seek, tell */
    uint32_t demote;
    uint32_t demote_fail;
    uint32_t late;
    uint32_t lost_bytes;
    uint32_t abandoned_bytes;
    uint32_t fail;
    uint32_t wakes;
    uint32_t refused;           /* game-log path not adopted: mode/path/live */
} isaac_vita_game_log_snapshot;

/* 1 iff native_path is exactly ".../Documents/<...>/log.txt" below the data
 * root, mode is one of "w", "wb", "a", "ab", the path fits, and no lane is
 * live.  Everything else keeps its existing FILE lane.  A game-log path the
 * lane refuses for a write/append mode outside that set (or because a lane
 * is live, or the path does not fit) is counted in the snapshot's `refused`
 * and receipted `[isaac-gamelog] refuse ... mode=<guest mode> reason=` for
 * the first GAME_LOG_REFUSE_RECEIPTS occurrences, so a lane that never
 * adopts log.txt is visible in the device log rather than silently inert. */
int isaac_vita_game_log_eligible(const char *native_path, const char *mode);

/* Start the worker lazily, perform the one synchronous open newlib would
 * also perform (truncate for w/wb, size probe for a/ab), and make the lane
 * live.  Returns 0 (nothing adopted; errno set for an open failure) so the
 * caller falls back to the native fopen. */
int isaac_vita_game_log_open(const char *native_path, const char *mode);

/* newlib-compatible fwrite/fflush/ftell/fseek/ferror on the lane. */
size_t isaac_vita_game_log_write(const void *buffer, size_t size,
                                 size_t count);
/* severity >= ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY or UNKNOWN: drain
 * then append synchronously; otherwise count and return.  Returns 0 or EOF
 * (an append failed, or a worker failure was pending). */
int isaac_vita_game_log_flush(unsigned severity);
/* isaac_vita_crt_flush_all (guest exit): synchronous drain, counted as an
 * exit append. */
int isaac_vita_game_log_flush_exit(void);
/* Guest fflush(NULL) while the lane is live: wake the worker, never wait. */
void isaac_vita_game_log_flush_null(void);
long isaac_vita_game_log_tell(void);
/* 0 when the target is the current logical end (no I/O), else
 * ISAAC_VITA_GAME_LOG_SEEK_DEMOTE. */
int isaac_vita_game_log_seek(long offset, int origin);
int isaac_vita_game_log_error(void);
/* Drain, then the lane is no longer live.  0 or EOF. */
int isaac_vita_game_log_close(void);

/* Demotion helper: drain, hand the logical position, the recorded fopen mode
 * and the native path (static storage, valid until the next open) to the
 * CRT, and mark the lane not live.  0 or EOF. */
int isaac_vita_game_log_release(long *position, char mode_out[4],
                                const char **native_path, unsigned reason);
/* The CRT could not open the newlib FILE after release: the lane (already
 * drained, cursors empty) becomes live again in place. */
void isaac_vita_game_log_rearm(void);

int isaac_vita_game_log_owns_path(const char *native_path);
/* Read-your-writes for a path-based import on exactly the lane's path
 * (who = ISAAC_VITA_GAME_LOG_SYNC_*). */
void isaac_vita_game_log_sync_path(const char *native_path, unsigned who);
/* Read-your-writes for a directory scan of exactly the lane's directory. */
void isaac_vita_game_log_sync_directory(const char *native_directory);

/* Process-exit tail (never under the CRT file lock): drain, stop and join the
 * worker, log one summary receipt.  Idempotent. */
void isaac_vita_game_log_shutdown(void);
/* Dying process, any thread: best-effort bounded drain that never logs and
 * never blocks longer than one wait step. */
void isaac_vita_game_log_abort_drain(void);

void isaac_vita_game_log_snapshot_read(isaac_vita_game_log_snapshot *snapshot);

#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
/* Host oracle: fake clock, hand-driven worker and I/O doubles the oracle
 * program must provide. */
extern uint64_t g_isaac_vita_game_log_oracle_now_us;
/* When set, every wait step performs one worker pass (completing a stranded
 * span first) instead of timing out. */
extern int g_isaac_vita_game_log_oracle_service_on_wait;
extern int g_isaac_vita_game_log_oracle_start_fails;
extern uint32_t g_isaac_vita_game_log_oracle_wakes;
/* One worker pass: claim the pending span, append it, complete it.  0 when
 * nothing is runnable. */
int isaac_vita_game_log_oracle_run_one(void);
/* Claim the pending span as the worker and leave it in flight. */
int isaac_vita_game_log_oracle_strand_one(void);
/* Finish the stranded span (late when the worker was declared failed). */
int isaac_vita_game_log_oracle_complete_stranded(void);
void isaac_vita_game_log_oracle_reset(void);
int32_t isaac_vita_game_log_oracle_open(const char *path, int truncate,
                                        int append);
int32_t isaac_vita_game_log_oracle_write(int32_t descriptor,
                                         const void *buffer, uint32_t size);
int32_t isaac_vita_game_log_oracle_close(int32_t descriptor);
int64_t isaac_vita_game_log_oracle_size(int32_t descriptor);
#endif

#ifdef __cplusplus
}
#endif

#endif
