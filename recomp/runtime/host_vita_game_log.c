/* Write-behind lane for the guest's log.txt: one static byte ring, one
 * append worker.  See host_vita_game_log.h for the contract and the loss
 * table.
 *
 * Threading model.  The guest thread is the only producer and the only
 * caller of every API except abort_drain; the worker performs exactly one
 * transition, claim -> append -> complete, under the same spin lock.  Spans
 * are claimed contiguously from the ring head and exactly one drainer (the
 * worker, the guest, or the abort path) owns a span at a time, so appends
 * are FIFO by construction.  The lane lock is held only for cursor and
 * counter updates: never across a syscall, a wait, isaac_vita_log or
 * anything that can abort.  Nothing here allocates (the raw allocator gate
 * poisons malloc): the ring is .bss, the path is an inline array. */
#include "host_vita_game_log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"

#ifndef ISAAC_VITA_GAME_LOG_BUILD_ID
# define ISAAC_VITA_GAME_LOG_BUILD_ID "gamelog:unstamped"
#endif

#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
/* Host oracle doubles: the oracle owns the clock, the "disk" and the worker
 * (spans are appended only when the oracle performs a worker pass). */
uint64_t g_isaac_vita_game_log_oracle_now_us;
int g_isaac_vita_game_log_oracle_service_on_wait;
int g_isaac_vita_game_log_oracle_start_fails;
uint32_t g_isaac_vita_game_log_oracle_wakes;
# define game_log_native_open_trunc(path) \
    isaac_vita_game_log_oracle_open((path), 1, 0)
# define game_log_native_open_append(path) \
    isaac_vita_game_log_oracle_open((path), 0, 1)
# define game_log_native_write(fd, buffer, size) \
    isaac_vita_game_log_oracle_write((fd), (buffer), (size))
# define game_log_native_close(fd) isaac_vita_game_log_oracle_close(fd)
# define game_log_native_size(fd) isaac_vita_game_log_oracle_size(fd)
# define game_log_now_us() (g_isaac_vita_game_log_oracle_now_us)
# define ISAAC_VITA_GAME_LOG_THREAD_PRIORITY 0x10000100
# define ISAAC_VITA_GAME_LOG_THREAD_AFFINITY 0
#else
# include <psp2/io/fcntl.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/threadmgr.h>
# define game_log_native_open_trunc(path) \
    sceIoOpen((path), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777)
# define game_log_native_open_append(path) \
    sceIoOpen((path), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777)
# define game_log_native_write(fd, buffer, size) \
    ((int32_t)sceIoWrite((fd), (buffer), (size)))
# define game_log_native_close(fd) sceIoClose(fd)
# define game_log_native_size(fd) ((int64_t)sceIoLseek((fd), 0, SCE_SEEK_END))
# define game_log_now_us() ((uint64_t)sceKernelGetProcessTimeWide())
/* Process default priority and default affinity (the save writer's
 * placement): the WARN drain waits for the span in flight, and a worker
 * starved behind the OpenAL mixer on CPU 2 would lengthen exactly that wait;
 * the worker is kernel-blocked in sceIoWrite, not CPU-bound.  Override both
 * at configure time for the 191/CPU-2 A/B. */
# ifndef ISAAC_VITA_GAME_LOG_THREAD_PRIORITY
#  define ISAAC_VITA_GAME_LOG_THREAD_PRIORITY 0x10000100
# endif
# ifndef ISAAC_VITA_GAME_LOG_THREAD_AFFINITY
#  define ISAAC_VITA_GAME_LOG_THREAD_AFFINITY \
    SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT
# endif
# define GAME_LOG_THREAD_STACK (16U * 1024U)
#endif

/* One wait step and the ceiling: five 2 s event-flag windows (the save
 * writer's ASYNC_WRITE_WAIT_BUDGET_US precedent).  A worker that has not
 * completed one append in 10 s is wedged in the kernel; the guest then
 * abandons the stranded span and goes synchronous instead of hanging. */
#define GAME_LOG_WAIT_TIMEOUT_US   2000000U
#define GAME_LOG_WAIT_TIMEOUTS_MAX 5U
#define GAME_LOG_ABORT_SPINS       1000000U
#define GAME_LOG_APPEND_RECEIPTS   8U
#define GAME_LOG_REFUSE_RECEIPTS   8U
#define GAME_LOG_ROOT ISAAC_VITA_NATIVE_DATA_ROOT "/Documents/"
#define GAME_LOG_BASENAME "log.txt"

enum {
    GAME_LOG_DRAIN_NONE = 0,
    GAME_LOG_DRAIN_WORKER = 1,
    GAME_LOG_DRAIN_GUEST = 2,
    GAME_LOG_DRAIN_ABORT = 3
};

enum {
    GAME_LOG_WHO_WORKER = 0,
    GAME_LOG_WHO_GUEST = 1,
    GAME_LOG_WHO_EXIT = 2,
    GAME_LOG_WHO_ABORT = 3,
    GAME_LOG_WHO_DIRECT = 4
};

static const char *const s_who_names[5] = {
    "worker", "guest", "exit", "abort", "direct"
};

static uint8_t s_ring[ISAAC_VITA_GAME_LOG_RING_BYTES];
static struct {
    uint32_t head;          /* first byte not yet accepted by the filesystem */
    uint32_t tail;          /* producer cursor */
    uint32_t used;          /* bytes in [head, tail) */
    uint32_t claimed;       /* bytes from head a drainer currently owns */
    uint32_t generation;    /* bumped when the worker is declared FAILED */
    int64_t logical_size;
    uint8_t live;
    uint8_t error;
    uint8_t pending_eof;
    uint8_t synchronous;
    uint8_t append_mode;
    uint8_t worker_idle;
    uint8_t draining;       /* GAME_LOG_DRAIN_* */
    uint8_t reserved;
    char path[ISAAC_VITA_GAME_LOG_PATH_MAX + 1U];
    char mode[4];
} s_lane;
static volatile uint32_t s_lane_lock;
static uint8_t s_worker_state;
static uint8_t s_start_failed;
static uint8_t s_shutdown;
static uint32_t s_start_result;
static uint32_t s_thread_priority;
static uint32_t s_thread_affinity;
static isaac_vita_game_log_snapshot s_stats = {
    .abi = ISAAC_VITA_GAME_LOG_SNAPSHOT_ABI
};
static volatile uint32_t s_append_receipts;
static volatile uint32_t s_refuse_receipts;
#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
static struct {
    int active;
    uint32_t begin;
    uint32_t length;
    uint32_t generation;
} s_stranded;
#else
static SceUID s_thread = -1;
static SceUID s_wake = -1;
static SceUID s_done = -1;
static volatile uint32_t s_stop;
#endif

/* ---- lock ------------------------------------------------------------- */

static void game_log_lock(void)
{
    while (__sync_lock_test_and_set(&s_lane_lock, 1U)) {
        while (s_lane_lock) {
        }
    }
    __sync_synchronize();
}

static int game_log_trylock_bounded(void)
{
    uint32_t spins = 0U;

    while (__sync_lock_test_and_set(&s_lane_lock, 1U)) {
        if (++spins >= GAME_LOG_ABORT_SPINS)
            return 0;
    }
    __sync_synchronize();
    return 1;
}

static void game_log_unlock(void)
{
    __sync_synchronize();
    __sync_lock_release(&s_lane_lock);
}

static void game_log_add(uint32_t *value, uint32_t addend)
{
    *value = *value > UINT32_MAX - addend ? UINT32_MAX : *value + addend;
}

static void game_log_max(uint32_t *value, uint32_t candidate)
{
    if (candidate > *value)
        *value = candidate;
}

static uint32_t game_log_clamp_u64(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/* SceIofilemgr returns encoded 0x800100XX errno-family values directly. */
static int game_log_errno(int32_t result)
{
    uint32_t value = (uint32_t)result;
    uint32_t low = value & UINT32_C(0xff);

    if ((value & UINT32_C(0xffffff00)) == UINT32_C(0x80010000) && low)
        return (int)low;
    return EIO;
}

/* ---- kernel seams ----------------------------------------------------- */

static void game_log_wake(void)
{
#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
    ++g_isaac_vita_game_log_oracle_wakes;
#else
    (void)sceKernelSignalSema(s_wake, 1);
#endif
}

static void game_log_signal_done(void)
{
#if !defined(ISAAC_VITA_GAME_LOG_ORACLE)
    (void)sceKernelSetEventFlag(s_done, 1U);
#endif
}

/* One wait step for the done flag.  1 when a completion was signalled, 0 on
 * timeout.  The oracle advances its clock by the step and, when asked,
 * performs one worker pass itself (finishing a stranded span first). */
static int game_log_wait_done(void)
{
#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
    g_isaac_vita_game_log_oracle_now_us += GAME_LOG_WAIT_TIMEOUT_US;
    if (!g_isaac_vita_game_log_oracle_service_on_wait)
        return 0;
    if (s_stranded.active)
        return isaac_vita_game_log_oracle_complete_stranded();
    return isaac_vita_game_log_oracle_run_one();
#else
    SceUInt timeout = GAME_LOG_WAIT_TIMEOUT_US;
    int result = sceKernelWaitEventFlag(
        s_done, 1U, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, NULL, &timeout);
    return result >= 0 ? 1 : 0;
#endif
}

/* ---- append ----------------------------------------------------------- */

/* Append two byte segments (the second may be empty) to the lane file with
 * one open/write/close; on a failed or short transaction the unwritten
 * suffix is retried once on a fresh open, never replaying accepted bytes.
 * Returns 0 when every byte was accepted, else the failing native code. */
static int32_t game_log_append_segments(const uint8_t *a, uint32_t a_length,
                                        const uint8_t *b, uint32_t b_length,
                                        unsigned who, int quiet,
                                        uint32_t *written_out,
                                        uint32_t *io_us)
{
    uint64_t begin = game_log_now_us();
    uint32_t total = a_length + b_length;
    uint32_t done = 0U;
    uint32_t retries = 0U;
    int32_t result = 0;
    unsigned attempt;

    for (attempt = 0U; attempt < 2U && done < total; ++attempt) {
        int32_t descriptor;

        if (attempt)
            ++retries;
        descriptor = game_log_native_open_append(s_lane.path);
        if (descriptor < 0) {
            result = descriptor;
            continue;
        }
        result = 0;
        while (done < total) {
            const uint8_t *cursor;
            uint32_t length;
            int32_t written;

            if (done < a_length) {
                cursor = a + done;
                length = a_length - done;
            } else {
                cursor = b + (done - a_length);
                length = b_length - (done - a_length);
            }
            written = game_log_native_write(descriptor, cursor, length);
            if (written <= 0) {
                result = written < 0 ? written : -1;
                break;
            }
            done += (uint32_t)written;
        }
        {
            int32_t close_result = game_log_native_close(descriptor);
            if (result == 0 && close_result < 0)
                result = close_result;
        }
    }
    *written_out = done;
    *io_us = game_log_clamp_u64(game_log_now_us() - begin);
    if (done == total)
        result = 0;
    else if (result == 0)
        result = -1;
    if ((result != 0 || retries) && !quiet) {
        uint32_t receipts = __sync_fetch_and_add(&s_append_receipts, 1U);
        if (receipts < GAME_LOG_APPEND_RECEIPTS)
            isaac_vita_log(
                "[isaac-gamelog] append bid40=%.40s who=%s bytes=%u "
                "rc=0x%08x retry=%u",
                ISAAC_VITA_GAME_LOG_BUILD_ID, s_who_names[who],
                (unsigned)total, (unsigned)result, (unsigned)retries);
    }
    return result;
}

/* The ring span [begin, begin + length) modulo the ring size. */
static int32_t game_log_append_ring(uint32_t begin, uint32_t length,
                                    unsigned who, int quiet,
                                    uint32_t *written_out, uint32_t *io_us)
{
    uint32_t first = ISAAC_VITA_GAME_LOG_RING_BYTES - begin;

    if (first > length)
        first = length;
    return game_log_append_segments(s_ring + begin, first, s_ring,
                                    length - first, who, quiet,
                                    written_out, io_us);
}

/* Caller owns the lock.  Claim every pending byte for one drainer. */
static void game_log_claim_locked(uint32_t *begin, uint32_t *length,
                                  unsigned drainer)
{
    *begin = (s_lane.head + s_lane.claimed) % ISAAC_VITA_GAME_LOG_RING_BYTES;
    *length = s_lane.used - s_lane.claimed;
    s_lane.claimed = s_lane.used;
    s_lane.draining = (uint8_t)drainer;
}

/* Caller owns the lock.  Release the owned span and record its outcome. */
static void game_log_complete_locked(uint32_t length, uint32_t written,
                                     int32_t result, uint32_t io_us,
                                     unsigned who)
{
    s_lane.head = (s_lane.head + s_lane.claimed) %
        ISAAC_VITA_GAME_LOG_RING_BYTES;
    s_lane.used -= s_lane.claimed;
    s_lane.claimed = 0U;
    s_lane.draining = GAME_LOG_DRAIN_NONE;
    game_log_add(&s_stats.appends[who], 1U);
    if (who == GAME_LOG_WHO_WORKER) {
        game_log_add(&s_stats.io_us_worker, io_us);
        game_log_max(&s_stats.io_us_worker_max, io_us);
    } else if (who != GAME_LOG_WHO_ABORT) {
        game_log_add(&s_stats.io_us_guest, io_us);
        game_log_max(&s_stats.io_us_guest_max, io_us);
    }
    if (result != 0) {
        s_lane.error = 1U;
        /* A drainer that is not the guest's own flush call reports through
         * the next fflush/fclose result. */
        if (who == GAME_LOG_WHO_WORKER || who == GAME_LOG_WHO_ABORT)
            s_lane.pending_eof = 1U;
        game_log_add(&s_stats.lost_bytes, length - written);
        game_log_add(&s_stats.fail, 1U);
    }
}

/* Caller owns the lock and the pending span is unclaimed.  Perform a whole
 * guest-side drain: claim, unlock, append, relock, complete.  Returns the
 * append result. */
static int32_t game_log_drain_here_locked(unsigned who, unsigned drainer)
{
    uint32_t begin;
    uint32_t length;
    uint32_t written = 0U;
    uint32_t io_us = 0U;
    int32_t result;

    game_log_claim_locked(&begin, &length, drainer);
    game_log_unlock();
    result = game_log_append_ring(begin, length, who, 0, &written, &io_us);
    game_log_lock();
    game_log_complete_locked(length, written, result, io_us, who);
    return result;
}

/* Caller owns the lock while the worker owns a span that has not completed
 * within the budget.  Abandon that span in place (the kernel may still be
 * reading it: nothing is ever written into the ring again), skip it so it is
 * never re-sent, and make the lane synchronous.  The worker, on a late
 * return, sees the generation mismatch and touches no cursor. */
static void game_log_fail_worker_locked(uint32_t *abandoned, uint32_t *rest)
{
    ++s_lane.generation;
    s_worker_state = ISAAC_VITA_GAME_LOG_WORKER_FAILED;
    *abandoned = s_lane.claimed;
    *rest = s_lane.used - s_lane.claimed;
    game_log_add(&s_stats.abandoned_bytes, s_lane.claimed);
    s_lane.head = (s_lane.head + s_lane.claimed) %
        ISAAC_VITA_GAME_LOG_RING_BYTES;
    s_lane.used -= s_lane.claimed;
    s_lane.claimed = 0U;
    s_lane.draining = GAME_LOG_DRAIN_NONE;
    s_lane.synchronous = 1U;
    s_lane.worker_idle = 0U;
}

static void game_log_log_abandoned(uint32_t abandoned, uint32_t rest,
                                   uint32_t wait_us)
{
    isaac_vita_log(
        "[isaac-gamelog] wait bid40=%.40s state=abandoned span=%u "
        "pending=%u wait_us=%u",
        ISAAC_VITA_GAME_LOG_BUILD_ID, (unsigned)abandoned, (unsigned)rest,
        (unsigned)wait_us);
}

/* Guest thread.  Wait for the worker's span, then append every pending byte
 * on this thread.  The guest thread is the only s_file_lock owner (the CRT
 * calls this under it; the worker never takes that lock), so waiting here
 * while holding s_file_lock cannot deadlock. */
static int game_log_flush_sync(unsigned who)
{
    uint64_t wait_begin = 0U;
    uint32_t timeouts = 0U;
    uint32_t abandoned = 0U;
    uint32_t rest = 0U;
    uint32_t wait_us = 0U;
    int waited = 0;
    int failed_worker = 0;
    int result = 0;

    game_log_lock();
    while (s_lane.draining == GAME_LOG_DRAIN_WORKER) {
        int answered;

        if (!waited) {
            waited = 1;
            wait_begin = game_log_now_us();
        }
        game_log_unlock();
        answered = game_log_wait_done();
        game_log_lock();
        if (s_lane.draining != GAME_LOG_DRAIN_WORKER)
            break;
        if (answered) {
            timeouts = 0U;
            continue;
        }
        if (++timeouts >= GAME_LOG_WAIT_TIMEOUTS_MAX) {
            game_log_fail_worker_locked(&abandoned, &rest);
            failed_worker = 1;
            break;
        }
    }
    if (waited) {
        wait_us = game_log_clamp_u64(game_log_now_us() - wait_begin);
        game_log_add(&s_stats.waits, 1U);
        game_log_add(&s_stats.wait_us, wait_us);
        game_log_max(&s_stats.wait_us_max, wait_us);
    }
    if (s_lane.draining == GAME_LOG_DRAIN_NONE &&
        s_lane.used != s_lane.claimed) {
        if (game_log_drain_here_locked(who, GAME_LOG_DRAIN_GUEST) != 0)
            result = EOF;
    }
    if (s_lane.pending_eof) {
        s_lane.pending_eof = 0U;
        result = EOF;
    }
    game_log_unlock();
    game_log_signal_done();
    if (failed_worker)
        game_log_log_abandoned(abandoned, rest, wait_us);
    if (result == EOF)
        errno = EIO;
    return result;
}

/* Guest thread, lane synchronous or record larger than the ring: append
 * straight from the caller's buffer. */
static void game_log_write_direct(const uint8_t *buffer, uint32_t length)
{
    uint32_t written = 0U;
    uint32_t io_us = 0U;
    int32_t result = game_log_append_segments(
        buffer, length, NULL, 0U, GAME_LOG_WHO_DIRECT, 0, &written, &io_us);

    game_log_lock();
    s_lane.logical_size += (int64_t)length;
    game_log_add(&s_stats.records, 1U);
    game_log_add(&s_stats.bytes, length);
    game_log_add(&s_stats.appends[GAME_LOG_WHO_DIRECT], 1U);
    game_log_add(&s_stats.io_us_guest, io_us);
    game_log_max(&s_stats.io_us_guest_max, io_us);
    if (result != 0) {
        s_lane.error = 1U;
        s_lane.pending_eof = 1U;
        game_log_add(&s_stats.lost_bytes, length - written);
        game_log_add(&s_stats.fail, 1U);
    }
    game_log_unlock();
}

/* ---- worker ----------------------------------------------------------- */

#if !defined(ISAAC_VITA_GAME_LOG_ORACLE)
static int game_log_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        (void)sceKernelWaitSema(s_wake, 1, NULL);
        for (;;) {
            uint32_t begin;
            uint32_t length;
            uint32_t generation;
            uint32_t written = 0U;
            uint32_t io_us = 0U;
            int32_t result;
            int stranded;

            game_log_lock();
            if (s_stop) {
                game_log_unlock();
                return 0;
            }
            if (s_lane.draining != GAME_LOG_DRAIN_NONE) {
                /* The guest (or the abort path) is appending: exactly one
                 * drainer at a time keeps the file FIFO. */
                game_log_unlock();
                (void)game_log_wait_done();
                continue;
            }
            if (s_lane.used == s_lane.claimed) {
                s_lane.worker_idle = 1U;
                game_log_unlock();
                break;
            }
            game_log_claim_locked(&begin, &length, GAME_LOG_DRAIN_WORKER);
            generation = s_lane.generation;
            s_lane.worker_idle = 0U;
            game_log_unlock();
            result = game_log_append_ring(begin, length, GAME_LOG_WHO_WORKER,
                                          0, &written, &io_us);
            game_log_lock();
            stranded = generation != s_lane.generation;
            if (!stranded)
                game_log_complete_locked(length, written, result, io_us,
                                         GAME_LOG_WHO_WORKER);
            else
                game_log_add(&s_stats.late, 1U);
            game_log_unlock();
            game_log_signal_done();
            if (stranded)
                return 0;
        }
    }
}

static int game_log_worker_start(void)
{
    int start_result;

    s_wake = sceKernelCreateSema("isaac_game_log_wake", 0, 0, 1, NULL);
    s_done = sceKernelCreateEventFlag("isaac_game_log_done", 0, 0, NULL);
    if (s_wake < 0 || s_done < 0) {
        s_start_result = (uint32_t)(s_wake < 0 ? s_wake : s_done);
        goto fail;
    }
    s_lane.worker_idle = 1U;
    s_thread_priority = (uint32_t)ISAAC_VITA_GAME_LOG_THREAD_PRIORITY;
    s_thread_affinity = (uint32_t)ISAAC_VITA_GAME_LOG_THREAD_AFFINITY;
    s_thread = sceKernelCreateThread(
        "isaac_game_log", game_log_worker,
        ISAAC_VITA_GAME_LOG_THREAD_PRIORITY, GAME_LOG_THREAD_STACK, 0U,
        ISAAC_VITA_GAME_LOG_THREAD_AFFINITY, NULL);
    if (s_thread < 0 &&
        (ISAAC_VITA_GAME_LOG_THREAD_PRIORITY != 0x10000100 ||
         ISAAC_VITA_GAME_LOG_THREAD_AFFINITY !=
             SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT)) {
        /* The configured placement was refused: the process default. */
        s_thread_priority = 0x10000100U;
        s_thread_affinity = (uint32_t)SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
        s_thread = sceKernelCreateThread(
            "isaac_game_log", game_log_worker, 0x10000100,
            GAME_LOG_THREAD_STACK, 0U,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, NULL);
    }
    if (s_thread < 0) {
        s_start_result = (uint32_t)s_thread;
        goto fail;
    }
    start_result = sceKernelStartThread(s_thread, 0U, NULL);
    s_start_result = (uint32_t)start_result;
    if (start_result < 0) {
        (void)sceKernelDeleteThread(s_thread);
        s_thread = -1;
        goto fail;
    }
    return 1;
fail:
    if (s_wake >= 0)
        (void)sceKernelDeleteSema(s_wake);
    if (s_done >= 0)
        (void)sceKernelDeleteEventFlag(s_done);
    s_wake = -1;
    s_done = -1;
    return 0;
}

static void game_log_worker_stop(void)
{
    if (s_thread < 0)
        return;
    s_stop = 1U;
    __sync_synchronize();
    (void)sceKernelSignalSema(s_wake, 1);
    if (s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY) {
        SceUInt timeout = GAME_LOG_WAIT_TIMEOUT_US;
        (void)sceKernelWaitThreadEnd(s_thread, NULL, &timeout);
    }
    (void)sceKernelDeleteThread(s_thread);
    (void)sceKernelDeleteSema(s_wake);
    (void)sceKernelDeleteEventFlag(s_done);
    s_thread = -1;
    s_wake = -1;
    s_done = -1;
}
#endif

static int game_log_worker_ready(void)
{
    int started;

    if (s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY)
        return 1;
    if (s_worker_state != ISAAC_VITA_GAME_LOG_WORKER_COLD || s_shutdown)
        return 0;
#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
    started = !g_isaac_vita_game_log_oracle_start_fails;
    s_start_result = started ? 0U : UINT32_C(0x80020001);
    s_thread_priority = (uint32_t)ISAAC_VITA_GAME_LOG_THREAD_PRIORITY;
    s_thread_affinity = (uint32_t)ISAAC_VITA_GAME_LOG_THREAD_AFFINITY;
    if (started)
        s_lane.worker_idle = 1U;
#else
    started = game_log_worker_start();
#endif
    s_worker_state = started ? ISAAC_VITA_GAME_LOG_WORKER_READY
                             : ISAAC_VITA_GAME_LOG_WORKER_FAILED;
    s_start_failed = started ? 0U : 1U;
    return started;
}

/* One receipt per adoption attempt; worker=2 with rc marks a start failure. */
static void game_log_log_open(const char *mode, uint32_t size)
{
    isaac_vita_log(
        "[isaac-gamelog] open bid40=%.40s mode=%s size=%u ring_kb=%u "
        "durable=%u worker=%u prio=0x%08x affinity=0x%08x rc=0x%08x",
        ISAAC_VITA_GAME_LOG_BUILD_ID, mode, (unsigned)size,
        (unsigned)ISAAC_VITA_GAME_LOG_ASYNC_RING_KB,
        (unsigned)ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY,
        (unsigned)s_worker_state, (unsigned)s_thread_priority,
        (unsigned)s_thread_affinity, (unsigned)s_start_result);
}

/* An open of the game log's own path that the lane does not take: counted,
 * and for the first GAME_LOG_REFUSE_RECEIPTS occurrences logged with the
 * guest's mode string, so an inert lane (the frozen PE opening log.txt with
 * a mode outside w/wb/a/ab) shows in the device log instead of silently
 * leaving log.txt on the synchronous newlib FILE. */
static void game_log_refuse(const char *mode, const char *reason)
{
    uint32_t receipts;

    game_log_lock();
    game_log_add(&s_stats.refused, 1U);
    game_log_unlock();
    receipts = __sync_fetch_and_add(&s_refuse_receipts, 1U);
    if (receipts < GAME_LOG_REFUSE_RECEIPTS)
        isaac_vita_log(
            "[isaac-gamelog] refuse bid40=%.40s mode=%s reason=%s",
            ISAAC_VITA_GAME_LOG_BUILD_ID, mode, reason);
}

/* ---- public API ------------------------------------------------------- */

int isaac_vita_game_log_eligible(const char *native_path, const char *mode)
{
    static const char root[] = GAME_LOG_ROOT;
    static const char basename[] = GAME_LOG_BASENAME;
    size_t length;
    const char *tail;

    if (!native_path || !mode)
        return 0;
    length = strlen(native_path);
    if (length < sizeof root - 1U + sizeof basename - 1U ||
        strncmp(native_path, root, sizeof root - 1U) != 0)
        return 0;
    tail = strrchr(native_path, '/');
    if (!tail || strcmp(tail + 1, basename) != 0)
        return 0;
    /* From here on the path is the game log.  Read modes keep their native
     * FILE silently (a reader is not the logger); a write/append mode the
     * lane does not carry, an over-long path or a live lane are receipted. */
    if (strcmp(mode, "w") != 0 && strcmp(mode, "wb") != 0 &&
        strcmp(mode, "a") != 0 && strcmp(mode, "ab") != 0) {
        if (mode[0] == 'w' || mode[0] == 'a')
            game_log_refuse(mode, "mode");
        return 0;
    }
    if (length > ISAAC_VITA_GAME_LOG_PATH_MAX) {
        game_log_refuse(mode, "path");
        return 0;
    }
    if (s_lane.live) {
        game_log_refuse(mode, "live");
        return 0;
    }
    return 1;
}

int isaac_vita_game_log_open(const char *native_path, const char *mode)
{
    size_t length;
    size_t mode_length;
    int32_t descriptor;
    int64_t size = 0;
    int append;

    if (!native_path || !mode || s_lane.live || s_shutdown)
        return 0;
    length = strlen(native_path);
    mode_length = strlen(mode);
    if (length > ISAAC_VITA_GAME_LOG_PATH_MAX || mode_length >= sizeof s_lane.mode)
        return 0;
    if (!game_log_worker_ready()) {
        game_log_log_open(mode, 0U);
        return 0;
    }
    /* Exactly the one native open newlib's fopen would perform, so the file
     * exists, is truncated (w) or measured (a) before the guest continues. */
    memcpy(s_lane.path, native_path, length + 1U);
    append = mode[0] == 'a';
    descriptor = append ? game_log_native_open_append(s_lane.path)
                        : game_log_native_open_trunc(s_lane.path);
    if (descriptor < 0) {
        s_lane.path[0] = '\0';
        errno = game_log_errno(descriptor);
        return 0;
    }
    if (append) {
        size = game_log_native_size(descriptor);
        if (size < 0)
            size = 0;
    }
    (void)game_log_native_close(descriptor);
    game_log_lock();
    s_lane.head = 0U;
    s_lane.tail = 0U;
    s_lane.used = 0U;
    s_lane.claimed = 0U;
    s_lane.draining = GAME_LOG_DRAIN_NONE;
    s_lane.logical_size = size;
    s_lane.error = 0U;
    s_lane.pending_eof = 0U;
    s_lane.append_mode = append ? 1U : 0U;
    memcpy(s_lane.mode, mode, mode_length + 1U);
    s_lane.live = 1U;
    game_log_unlock();
    game_log_log_open(mode, (uint32_t)size);
    return 1;
}

size_t isaac_vita_game_log_write(const void *buffer, size_t size,
                                 size_t count)
{
    uint64_t bytes;
    uint32_t length;
    uint32_t first;
    uint32_t timeouts = 0U;
    uint32_t abandoned = 0U;
    uint32_t rest = 0U;
    int wake = 0;

    if (!s_lane.live) {
        errno = EBADF;
        return 0U;
    }
    if (!size || !count)
        return 0U;
    if (!buffer || count > (size_t)UINT32_MAX / size) {
        s_lane.error = 1U;
        errno = EINVAL;
        return 0U;
    }
    bytes = (uint64_t)size * (uint64_t)count;
    length = (uint32_t)bytes;
    if (s_lane.synchronous) {
        game_log_write_direct(buffer, length);
        return count;
    }
    if (length > ISAAC_VITA_GAME_LOG_RING_BYTES) {
        /* Not a frozen-logger record (10240 bytes at most): drain, then
         * append straight from the caller. */
        (void)game_log_flush_sync(GAME_LOG_WHO_GUEST);
        game_log_write_direct(buffer, length);
        return count;
    }
    game_log_lock();
    while (length > ISAAC_VITA_GAME_LOG_RING_BYTES - s_lane.used) {
        uint32_t used_before = s_lane.used;
        int answered;

        game_log_add(&s_stats.full_waits, 1U);
        if (s_lane.worker_idle &&
            s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY) {
            s_lane.worker_idle = 0U;
            game_log_unlock();
            game_log_wake();
        } else {
            game_log_unlock();
        }
        answered = game_log_wait_done();
        game_log_lock();
        if (s_lane.used != used_before) {
            timeouts = 0U;
            continue;
        }
        if (s_lane.draining == GAME_LOG_DRAIN_NONE) {
            /* Nobody owns the pending bytes: append them here. */
            (void)game_log_drain_here_locked(GAME_LOG_WHO_GUEST,
                                             GAME_LOG_DRAIN_GUEST);
            game_log_unlock();
            game_log_signal_done();
            game_log_lock();
            timeouts = 0U;
            continue;
        }
        if (!answered && s_lane.draining == GAME_LOG_DRAIN_WORKER &&
            ++timeouts >= GAME_LOG_WAIT_TIMEOUTS_MAX) {
            game_log_fail_worker_locked(&abandoned, &rest);
            if (s_lane.used)
                (void)game_log_drain_here_locked(GAME_LOG_WHO_GUEST,
                                                 GAME_LOG_DRAIN_GUEST);
            game_log_unlock();
            game_log_log_abandoned(abandoned, rest,
                                   timeouts * GAME_LOG_WAIT_TIMEOUT_US);
            game_log_write_direct(buffer, length);
            return count;
        }
    }
    first = ISAAC_VITA_GAME_LOG_RING_BYTES - s_lane.tail;
    if (first > length)
        first = length;
    memcpy(s_ring + s_lane.tail, buffer, first);
    if (length > first)
        memcpy(s_ring, (const uint8_t *)buffer + first, length - first);
    s_lane.tail = (s_lane.tail + length) % ISAAC_VITA_GAME_LOG_RING_BYTES;
    s_lane.used += length;
    s_lane.logical_size += (int64_t)length;
    game_log_add(&s_stats.records, 1U);
    game_log_add(&s_stats.bytes, length);
    game_log_max(&s_stats.high, s_lane.used);
    if (s_lane.worker_idle &&
        s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY) {
        /* The ring's idle -> busy transition wakes the worker, independent
         * of the fflush policy that follows. */
        s_lane.worker_idle = 0U;
        wake = 1;
    }
    game_log_unlock();
    if (wake)
        game_log_wake();
    return count;
}

int isaac_vita_game_log_flush(unsigned severity)
{
    unsigned index = severity < 4U ? severity : 4U;
    /* A variable, not the macro: DURABLE_SEVERITY=0 must not trip
     * -Wtype-limits on the unsigned comparison. */
    unsigned durable = ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY;
    int result = 0;

    if (!s_lane.live) {
        errno = EBADF;
        return EOF;
    }
    game_log_lock();
    game_log_add(&s_stats.flush[index], 1U);
    game_log_unlock();
    if (severity == ISAAC_VITA_GAME_LOG_SEVERITY_UNKNOWN ||
        severity >= durable)
        return game_log_flush_sync(GAME_LOG_WHO_GUEST);
    /* INFO policy: the bytes are in the ring and the worker is awake (or
     * about to be); nothing to wait for. */
    game_log_lock();
    if (s_lane.pending_eof) {
        s_lane.pending_eof = 0U;
        result = EOF;
    }
    game_log_unlock();
    if (result == EOF)
        errno = EIO;
    return result;
}

int isaac_vita_game_log_flush_exit(void)
{
    if (!s_lane.live)
        return 0;
    return game_log_flush_sync(GAME_LOG_WHO_EXIT);
}

void isaac_vita_game_log_flush_null(void)
{
    int wake = 0;

    if (!s_lane.live)
        return;
    game_log_lock();
    game_log_add(&s_stats.flush[5], 1U);
    if (s_lane.worker_idle && s_lane.used != s_lane.claimed &&
        s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY) {
        s_lane.worker_idle = 0U;
        wake = 1;
    }
    game_log_unlock();
    if (wake)
        game_log_wake();
}

long isaac_vita_game_log_tell(void)
{
    if (!s_lane.live) {
        errno = EBADF;
        return -1L;
    }
    game_log_lock();
    game_log_add(&s_stats.coherence[6], 1U);
    game_log_unlock();
    return (long)s_lane.logical_size;
}

int isaac_vita_game_log_seek(long offset, int origin)
{
    int64_t target;

    if (!s_lane.live) {
        errno = EBADF;
        return -1;
    }
    game_log_lock();
    game_log_add(&s_stats.coherence[5], 1U);
    game_log_unlock();
    if (origin == SEEK_SET)
        target = (int64_t)offset;
    else if (origin == SEEK_CUR || origin == SEEK_END)
        target = s_lane.logical_size + (int64_t)offset;
    else
        return ISAAC_VITA_GAME_LOG_SEEK_DEMOTE;
    return target == s_lane.logical_size ? 0
                                         : ISAAC_VITA_GAME_LOG_SEEK_DEMOTE;
}

int isaac_vita_game_log_error(void)
{
    return s_lane.error != 0U;
}

int isaac_vita_game_log_close(void)
{
    int result;

    if (!s_lane.live) {
        errno = EBADF;
        return EOF;
    }
    result = game_log_flush_sync(GAME_LOG_WHO_GUEST);
    game_log_lock();
    s_lane.live = 0U;
    game_log_unlock();
    return result;
}

int isaac_vita_game_log_release(long *position, char mode_out[4],
                                const char **native_path, unsigned reason)
{
    int result;

    if (!s_lane.live) {
        errno = EBADF;
        return EOF;
    }
    result = game_log_flush_sync(GAME_LOG_WHO_GUEST);
    game_log_lock();
    s_lane.live = 0U;
    if (position)
        *position = (long)s_lane.logical_size;
    if (mode_out)
        memcpy(mode_out, s_lane.mode, sizeof s_lane.mode);
    if (native_path)
        *native_path = s_lane.path;
    game_log_add(&s_stats.demote, 1U);
    if (reason == ISAAC_VITA_GAME_LOG_RELEASE_OPEN)
        game_log_add(&s_stats.coherence[0], 1U);
    game_log_unlock();
    return result;
}

void isaac_vita_game_log_rearm(void)
{
    game_log_lock();
    if (s_lane.path[0] && !s_shutdown) {
        s_lane.live = 1U;
        game_log_add(&s_stats.demote_fail, 1U);
    }
    game_log_unlock();
}

int isaac_vita_game_log_owns_path(const char *native_path)
{
    return native_path && s_lane.live &&
        strcmp(s_lane.path, native_path) == 0;
}

void isaac_vita_game_log_sync_path(const char *native_path, unsigned who)
{
    unsigned index = who <= ISAAC_VITA_GAME_LOG_SYNC_ATTRIB ? 1U + who : 3U;
    int pending;

    if (!isaac_vita_game_log_owns_path(native_path))
        return;
    game_log_lock();
    game_log_add(&s_stats.coherence[index], 1U);
    pending = s_lane.used != 0U;
    game_log_unlock();
    if (pending)
        (void)game_log_flush_sync(GAME_LOG_WHO_GUEST);
}

void isaac_vita_game_log_sync_directory(const char *native_directory)
{
    const char *slash;
    size_t length;
    int pending;

    if (!native_directory || !s_lane.live)
        return;
    slash = strrchr(s_lane.path, '/');
    if (!slash)
        return;
    length = strlen(native_directory);
    if (length && native_directory[length - 1U] == '/')
        --length;
    if (length != (size_t)(slash - s_lane.path) ||
        memcmp(s_lane.path, native_directory, length) != 0)
        return;
    game_log_lock();
    game_log_add(&s_stats.coherence[4], 1U);
    pending = s_lane.used != 0U;
    game_log_unlock();
    if (pending)
        (void)game_log_flush_sync(GAME_LOG_WHO_GUEST);
}

static const char *game_log_state_name(void)
{
    if (s_start_failed)
        return "start-failed";
    switch (s_worker_state) {
    case ISAAC_VITA_GAME_LOG_WORKER_READY:
        return "live";
    case ISAAC_VITA_GAME_LOG_WORKER_FAILED:
        return "failed";
    case ISAAC_VITA_GAME_LOG_WORKER_STOPPED:
        return "stopped";
    default:
        return "cold";
    }
}

void isaac_vita_game_log_shutdown(void)
{
    isaac_vita_game_log_snapshot snapshot;

    if (s_shutdown)
        return;
    if (s_lane.live && s_lane.used)
        (void)game_log_flush_sync(GAME_LOG_WHO_EXIT);
    s_shutdown = 1U;
#if !defined(ISAAC_VITA_GAME_LOG_ORACLE)
    game_log_worker_stop();
#endif
    game_log_lock();
    if (s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY)
        s_worker_state = ISAAC_VITA_GAME_LOG_WORKER_STOPPED;
    s_lane.synchronous = 1U;
    game_log_unlock();
    isaac_vita_game_log_snapshot_read(&snapshot);
    isaac_vita_log(
        "[isaac-gamelog] summary bid40=%.40s state=%s records=%u bytes=%u "
        "flush(info,warn,error,assert,other,null)=%u,%u,%u,%u,%u,%u "
        "appends(worker,guest,exit,abort,direct)=%u,%u,%u,%u,%u "
        "io_us(worker,worker_max,guest,guest_max)=%u,%u,%u,%u "
        "wait(n,us,max)=%u,%u,%u ring(high,full_waits)=%u,%u "
        "coherence(open,remove,access,attrib,find,seek,tell)=%u,%u,%u,%u,%u,%u,%u "
        "demote=%u late=%u lost_bytes=%u abandoned_bytes=%u fail=%u",
        ISAAC_VITA_GAME_LOG_BUILD_ID, game_log_state_name(),
        (unsigned)snapshot.records, (unsigned)snapshot.bytes,
        (unsigned)snapshot.flush[0], (unsigned)snapshot.flush[1],
        (unsigned)snapshot.flush[2], (unsigned)snapshot.flush[3],
        (unsigned)snapshot.flush[4], (unsigned)snapshot.flush[5],
        (unsigned)snapshot.appends[0], (unsigned)snapshot.appends[1],
        (unsigned)snapshot.appends[2], (unsigned)snapshot.appends[3],
        (unsigned)snapshot.appends[4],
        (unsigned)snapshot.io_us_worker, (unsigned)snapshot.io_us_worker_max,
        (unsigned)snapshot.io_us_guest, (unsigned)snapshot.io_us_guest_max,
        (unsigned)snapshot.waits, (unsigned)snapshot.wait_us,
        (unsigned)snapshot.wait_us_max,
        (unsigned)snapshot.high, (unsigned)snapshot.full_waits,
        (unsigned)snapshot.coherence[0], (unsigned)snapshot.coherence[1],
        (unsigned)snapshot.coherence[2], (unsigned)snapshot.coherence[3],
        (unsigned)snapshot.coherence[4], (unsigned)snapshot.coherence[5],
        (unsigned)snapshot.coherence[6],
        (unsigned)snapshot.demote, (unsigned)snapshot.late,
        (unsigned)snapshot.lost_bytes, (unsigned)snapshot.abandoned_bytes,
        (unsigned)snapshot.fail);
}

void isaac_vita_game_log_abort_drain(void)
{
    uint32_t begin;
    uint32_t length;
    uint32_t written = 0U;
    uint32_t io_us = 0U;
    int32_t result;

    if (!s_lane.live)
        return;
    if (!game_log_trylock_bounded())
        return;
    if (s_lane.draining == GAME_LOG_DRAIN_WORKER) {
        game_log_unlock();
        (void)game_log_wait_done();
        if (!game_log_trylock_bounded())
            return;
    }
    if (!s_lane.live || s_lane.draining != GAME_LOG_DRAIN_NONE ||
        s_lane.used == s_lane.claimed) {
        game_log_unlock();
        return;
    }
    game_log_claim_locked(&begin, &length, GAME_LOG_DRAIN_ABORT);
    game_log_unlock();
    result = game_log_append_ring(begin, length, GAME_LOG_WHO_ABORT, 1,
                                  &written, &io_us);
    if (!game_log_trylock_bounded())
        return;
    game_log_complete_locked(length, written, result, io_us,
                             GAME_LOG_WHO_ABORT);
    game_log_unlock();
}

void isaac_vita_game_log_snapshot_read(isaac_vita_game_log_snapshot *snapshot)
{
    if (!snapshot)
        return;
    game_log_lock();
    *snapshot = s_stats;
    snapshot->abi = ISAAC_VITA_GAME_LOG_SNAPSHOT_ABI;
    snapshot->worker_state = s_worker_state;
    snapshot->start_failed = s_start_failed;
    snapshot->live = s_lane.live;
    snapshot->synchronous = s_lane.synchronous;
    snapshot->error = s_lane.error;
    snapshot->pending_eof = s_lane.pending_eof;
    snapshot->head = s_lane.head;
    snapshot->tail = s_lane.tail;
    snapshot->used = s_lane.used;
    snapshot->claimed = s_lane.claimed;
    snapshot->draining = s_lane.draining;
    snapshot->generation = s_lane.generation;
    snapshot->worker_idle = s_lane.worker_idle;
    snapshot->logical_size = s_lane.logical_size;
#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
    snapshot->wakes = g_isaac_vita_game_log_oracle_wakes;
#endif
    game_log_unlock();
}

/* ---- host oracle worker ------------------------------------------------ */

#if defined(ISAAC_VITA_GAME_LOG_ORACLE)
int isaac_vita_game_log_oracle_run_one(void)
{
    uint32_t begin;
    uint32_t length;
    uint32_t generation;
    uint32_t written = 0U;
    uint32_t io_us = 0U;
    int32_t result;

    game_log_lock();
    if (s_worker_state != ISAAC_VITA_GAME_LOG_WORKER_READY ||
        s_lane.draining != GAME_LOG_DRAIN_NONE ||
        s_lane.used == s_lane.claimed) {
        if (s_lane.draining == GAME_LOG_DRAIN_NONE &&
            s_lane.used == s_lane.claimed &&
            s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY)
            s_lane.worker_idle = 1U;
        game_log_unlock();
        return 0;
    }
    game_log_claim_locked(&begin, &length, GAME_LOG_DRAIN_WORKER);
    generation = s_lane.generation;
    s_lane.worker_idle = 0U;
    game_log_unlock();
    result = game_log_append_ring(begin, length, GAME_LOG_WHO_WORKER, 0,
                                  &written, &io_us);
    game_log_lock();
    if (generation == s_lane.generation)
        game_log_complete_locked(length, written, result, io_us,
                                 GAME_LOG_WHO_WORKER);
    else
        game_log_add(&s_stats.late, 1U);
    if (s_lane.used == s_lane.claimed &&
        s_lane.draining == GAME_LOG_DRAIN_NONE)
        s_lane.worker_idle = 1U;
    game_log_unlock();
    return 1;
}

int isaac_vita_game_log_oracle_strand_one(void)
{
    game_log_lock();
    if (s_stranded.active ||
        s_worker_state != ISAAC_VITA_GAME_LOG_WORKER_READY ||
        s_lane.draining != GAME_LOG_DRAIN_NONE ||
        s_lane.used == s_lane.claimed) {
        game_log_unlock();
        return 0;
    }
    game_log_claim_locked(&s_stranded.begin, &s_stranded.length,
                          GAME_LOG_DRAIN_WORKER);
    s_stranded.generation = s_lane.generation;
    s_stranded.active = 1;
    s_lane.worker_idle = 0U;
    game_log_unlock();
    return 1;
}

int isaac_vita_game_log_oracle_complete_stranded(void)
{
    uint32_t written = 0U;
    uint32_t io_us = 0U;
    int32_t result;

    if (!s_stranded.active)
        return 0;
    result = game_log_append_ring(s_stranded.begin, s_stranded.length,
                                  GAME_LOG_WHO_WORKER, 0, &written, &io_us);
    game_log_lock();
    if (s_stranded.generation == s_lane.generation)
        game_log_complete_locked(s_stranded.length, written, result, io_us,
                                 GAME_LOG_WHO_WORKER);
    else
        game_log_add(&s_stats.late, 1U);
    if (s_lane.used == s_lane.claimed &&
        s_lane.draining == GAME_LOG_DRAIN_NONE &&
        s_worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY)
        s_lane.worker_idle = 1U;
    s_stranded.active = 0;
    game_log_unlock();
    return 1;
}

void isaac_vita_game_log_oracle_reset(void)
{
    game_log_lock();
    memset(&s_lane, 0, sizeof s_lane);
    memset(&s_stats, 0, sizeof s_stats);
    memset(&s_stranded, 0, sizeof s_stranded);
    s_stats.abi = ISAAC_VITA_GAME_LOG_SNAPSHOT_ABI;
    s_worker_state = ISAAC_VITA_GAME_LOG_WORKER_COLD;
    s_start_failed = 0U;
    s_shutdown = 0U;
    s_start_result = 0U;
    s_append_receipts = 0U;
    s_refuse_receipts = 0U;
    g_isaac_vita_game_log_oracle_wakes = 0U;
    g_isaac_vita_game_log_oracle_service_on_wait = 0;
    g_isaac_vita_game_log_oracle_start_fails = 0;
    game_log_unlock();
}
#endif
