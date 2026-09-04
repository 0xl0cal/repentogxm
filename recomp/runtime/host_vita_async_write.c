/* Asynchronous whole-file writer for the guest's binary save files.
 *
 * See host_vita_async_write.h for the measured rationale.  Threading model:
 * the guest thread owns every image and every queue slot transition except
 * PENDING->RUNNING->DONE, which the single worker performs under the same
 * spin lock.  Lanes are acquired and released on the guest thread only (DONE
 * slots are reclaimed on the next guest-side call); the arena is one
 * USER_RW memblock, never the poisoned C allocator.  The worker touches only
 * the job's path and lane: no guest memory, no CPU context, no FILE token. */
#include "host_vita_async_write.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifndef ISAAC_VITA_ASYNC_WRITE_BUILD_ID
# define ISAAC_VITA_ASYNC_WRITE_BUILD_ID "async-save:unstamped"
#endif

#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
/* Host oracle doubles.  The oracle owns the "worker": jobs stay queued until
 * isaac_vita_async_write_oracle_run_one() performs one, and every wait in
 * this file performs jobs itself instead of blocking. */
int32_t isaac_vita_async_write_oracle_open(const char *path);
int32_t isaac_vita_async_write_oracle_write(int32_t descriptor,
                                            const void *buffer,
                                            uint32_t size);
int32_t isaac_vita_async_write_oracle_close(int32_t descriptor);
uint8_t *isaac_vita_async_write_oracle_arena(void);
uint64_t isaac_vita_async_write_oracle_time_us(void);
int isaac_vita_async_write_oracle_worker_start(void);
# define async_native_open(path) isaac_vita_async_write_oracle_open(path)
# define async_native_write(fd, buffer, size) \
    isaac_vita_async_write_oracle_write((fd), (buffer), (size))
# define async_native_close(fd) isaac_vita_async_write_oracle_close(fd)
# define async_native_time_us() isaac_vita_async_write_oracle_time_us()
#else
# include <psp2/io/fcntl.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/sysmem.h>
# include <psp2/kernel/threadmgr.h>
# define async_native_open(path) \
    sceIoOpen((path), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777)
# define async_native_write(fd, buffer, size) \
    ((int32_t)sceIoWrite((fd), (buffer), (size)))
# define async_native_close(fd) sceIoClose(fd)
# define async_native_time_us() ((uint64_t)sceKernelGetProcessTimeWide())
/* The vitasdk thread example priority; the stall watchdog copies the guest
 * thread's own priority and both are the process default in practice. */
# define ASYNC_WRITE_THREAD_PRIORITY   0x10000100
# define ASYNC_WRITE_THREAD_STACK      (32U * 1024U)
# define ASYNC_WRITE_WAIT_TIMEOUT_US   2000000U
# define ASYNC_WRITE_WAIT_TIMEOUTS_MAX 5U
#endif

/* Hard ceiling on a single read-your-writes wait.  The worker completes even
 * a 68 KiB image in ~12 ms, so a wait that reaches this bound means the worker
 * is wedged (a native write that never returns) or its wakeup was lost.  The
 * guest must never block a startup file probe indefinitely on that, so past
 * the ceiling the wait declares the worker failed and returns best-effort: the
 * previous on-disk file stays intact and the guest proceeds.  Ten seconds is
 * five 2 s event-flag windows; the same figure bounds the oracle by simulated
 * time so the abandonment path is exercised without a real sleep. */
#define ASYNC_WRITE_WAIT_BUDGET_US \
    (ASYNC_WRITE_WAIT_TIMEOUT_US_VALUE * ASYNC_WRITE_WAIT_TIMEOUTS_MAX_VALUE)
#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
# define ASYNC_WRITE_WAIT_TIMEOUT_US_VALUE   2000000U
# define ASYNC_WRITE_WAIT_TIMEOUTS_MAX_VALUE 5U
#else
# define ASYNC_WRITE_WAIT_TIMEOUT_US_VALUE   ASYNC_WRITE_WAIT_TIMEOUT_US
# define ASYNC_WRITE_WAIT_TIMEOUTS_MAX_VALUE ASYNC_WRITE_WAIT_TIMEOUTS_MAX
#endif

#define ASYNC_WRITE_SAVE_ROOT ISAAC_VITA_NATIVE_DATA_ROOT "/Documents/"
#define ASYNC_WRITE_SAVE_SUFFIX ".dat"

enum {
    ASYNC_JOB_EMPTY = 0,
    ASYNC_JOB_PENDING = 1,
    ASYNC_JOB_RUNNING = 2,
    ASYNC_JOB_DONE = 3
};

typedef struct async_write_job {
    uint8_t *data;
    uint32_t size;
    uint32_t lane;
    uint32_t state;
    uint32_t sequence;
    uint32_t io_us;
    int32_t result;
    uint64_t queued_us;
    char path[ISAAC_VITA_ASYNC_WRITE_PATH_MAX + 1U];
} async_write_job;

static async_write_job s_jobs[ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY];
static isaac_vita_async_write_snapshot s_stats = {
    .abi = ISAAC_VITA_ASYNC_WRITE_SNAPSHOT_ABI
};
static volatile uint32_t s_lock;
static uint32_t s_sequence;
static uint32_t s_shutdown;
/* Guest thread only: arena base and the lane occupancy mask. */
static uint8_t *s_arena;
static uint32_t s_arena_failed;
static uint32_t s_lane_busy;
#if !defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
static SceUID s_thread = -1;
static SceUID s_wake_sema = -1;
static SceUID s_done_flag = -1;
static volatile uint32_t s_stop;
#endif

static void async_lock(void)
{
    while (__sync_lock_test_and_set(&s_lock, 1U)) {
        while (s_lock) {
        }
    }
    __sync_synchronize();
}

static void async_unlock(void)
{
    __sync_synchronize();
    __sync_lock_release(&s_lock);
}

/* Forward declarations for the guest-thread reclaim used before a lane scan
 * (defined with the rest of the job machinery below). */
static void async_reclaim_locked(void);
static async_write_job *async_take_pending_locked(void);
static void async_run_job(async_write_job *job);
static int async_busy_locked(const char *path);

static void async_counter_add(uint32_t *value, uint32_t addend)
{
    *value = *value > UINT32_MAX - addend ? UINT32_MAX : *value + addend;
}

static void async_counter_max(uint32_t *value, uint32_t candidate)
{
    if (candidate > *value)
        *value = candidate;
}

static const char *async_path_tail(const char *path)
{
    const char *tail = strrchr(path, '/');

    return tail ? tail + 1 : path;
}

int isaac_vita_async_write_eligible(const char *native_path,
                                    const char *mode)
{
    static const char root[] = ASYNC_WRITE_SAVE_ROOT;
    static const char suffix[] = ASYNC_WRITE_SAVE_SUFFIX;
    size_t length;

    if (!native_path || !mode)
        return 0;
    if (strcmp(mode, "wb") != 0 && strcmp(mode, "w") != 0)
        return 0;
    length = strlen(native_path);
    if (length > ISAAC_VITA_ASYNC_WRITE_PATH_MAX ||
        length <= sizeof root - 1U + sizeof suffix - 1U)
        return 0;
    if (strncmp(native_path, root, sizeof root - 1U) != 0)
        return 0;
    if (strcmp(native_path + length - (sizeof suffix - 1U), suffix) != 0)
        return 0;
    /* The tail must be a file name, not a directory separator. */
    if (native_path[length - 1U] == '/' ||
        native_path[length - (sizeof suffix - 1U) - 1U] == '/')
        return 0;
    return 1;
}

/* ---- lane arena (guest thread only) --------------------------------- */

static int async_arena_ready(void)
{
    if (s_arena)
        return 1;
    if (s_arena_failed)
        return 0;
#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
    s_arena = isaac_vita_async_write_oracle_arena();
#else
    {
        SceUID uid = sceKernelAllocMemBlock(
            "isaac_save_writer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
            ISAAC_VITA_ASYNC_WRITE_ARENA_BYTES, NULL);
        void *base = NULL;

        if (uid >= 0 && sceKernelGetMemBlockBase(uid, &base) >= 0 && base) {
            s_arena = (uint8_t *)base;
        } else {
            isaac_vita_log(
                "[isaac-asyncsave] arena bid40=%.40s state=alloc-failed "
                "uid=0x%08x bytes=%u",
                ISAAC_VITA_ASYNC_WRITE_BUILD_ID, (unsigned)uid,
                (unsigned)ISAAC_VITA_ASYNC_WRITE_ARENA_BYTES);
            if (uid >= 0)
                (void)sceKernelFreeMemBlock(uid);
        }
    }
#endif
    if (!s_arena) {
        s_arena_failed = 1U;
        async_lock();
        async_counter_add(&s_stats.images_arena_failed, 1U);
        async_unlock();
        return 0;
    }
    return 1;
}

static uint8_t *async_lane_acquire(uint32_t *lane_out)
{
    uint32_t lane;

    if (!async_arena_ready())
        return NULL;
    for (lane = 0U; lane < ISAAC_VITA_ASYNC_WRITE_LANE_COUNT; ++lane) {
        if (!(s_lane_busy & (1U << lane))) {
            s_lane_busy |= 1U << lane;
            *lane_out = lane;
            return s_arena + lane * ISAAC_VITA_ASYNC_WRITE_LANE_BYTES;
        }
    }
    return NULL;
}

static void async_lane_release(uint32_t lane)
{
    if (lane < ISAAC_VITA_ASYNC_WRITE_LANE_COUNT)
        s_lane_busy &= ~(1U << lane);
}

/* ---- memory image -------------------------------------------------- */

int isaac_vita_async_write_image_open(isaac_vita_async_write_image *image,
                                      const char *native_path)
{
    size_t length;
    uint32_t lane = 0U;

    if (!image || !native_path)
        return 0;
    memset(image, 0, sizeof *image);
    length = strlen(native_path);
    if (length > ISAAC_VITA_ASYNC_WRITE_PATH_MAX)
        return 0;
    /* Reclaim the lanes of jobs the worker has already completed before the
     * scan.  Lanes are freed only on the guest thread, and a fresh save open
     * is the first guest-side entry after a burst of room-change writes; if
     * DONE jobs still pinned their lanes here, an otherwise-serviceable save
     * would spuriously report the arena full and fall back to a native FILE
     * that later reads cannot observe through the image path. */
    async_lock();
    async_reclaim_locked();
    async_unlock();
    image->data = async_lane_acquire(&lane);
    if (!image->data) {
        if (!s_arena_failed) {
            async_lock();
            async_counter_add(&s_stats.images_lane_exhausted, 1U);
            async_unlock();
        }
        return 0;
    }
    image->lane = (uint8_t)lane;
    image->capacity = ISAAC_VITA_ASYNC_WRITE_LANE_BYTES;
    memcpy(image->path, native_path, length + 1U);
    image->live = 1U;
    async_lock();
    async_counter_add(&s_stats.images_opened, 1U);
    async_unlock();
    return 1;
}

size_t isaac_vita_async_write_image_write(isaac_vita_async_write_image *image,
                                          const void *buffer, size_t size,
                                          size_t count)
{
    uint64_t bytes;
    uint64_t end;

    if (!image || !image->live) {
        errno = EBADF;
        return 0U;
    }
    if (!size || !count)
        return 0U;
    if (!buffer || count > (size_t)UINT32_MAX / size) {
        image->error = 1U;
        errno = EINVAL;
        return 0U;
    }
    bytes = (uint64_t)size * (uint64_t)count;
    end = (uint64_t)image->position + bytes;
    if (end > image->capacity) {
        /* A save larger than the lane is outside every measured shape; the
         * error surfaces at fclose and the previous file stays intact. */
        image->error = 1U;
        errno = EFBIG;
        return 0U;
    }
    /* A seek past the end followed by a write reads back as zeros, exactly
     * like the native sparse file newlib would have produced. */
    if (image->position > image->size)
        memset(image->data + image->size, 0,
               image->position - image->size);
    memcpy(image->data + image->position, buffer, (size_t)bytes);
    image->position = (uint32_t)end;
    if (image->position > image->size)
        image->size = image->position;
    return count;
}

int isaac_vita_async_write_image_seek(isaac_vita_async_write_image *image,
                                      long offset, int origin)
{
    int64_t base;
    int64_t target;

    if (!image || !image->live) {
        errno = EBADF;
        return -1;
    }
    if (origin == SEEK_SET)
        base = 0;
    else if (origin == SEEK_CUR)
        base = (int64_t)image->position;
    else if (origin == SEEK_END)
        base = (int64_t)image->size;
    else {
        errno = EINVAL;
        return -1;
    }
    target = base + (int64_t)offset;
    if (target < 0 || target > (int64_t)image->capacity) {
        errno = EINVAL;
        return -1;
    }
    image->position = (uint32_t)target;
    return 0;
}

long isaac_vita_async_write_image_tell(
    const isaac_vita_async_write_image *image)
{
    if (!image || !image->live) {
        errno = EBADF;
        return -1L;
    }
    return (long)image->position;
}

/* ---- job execution (worker thread, or the guest thread as fallback) --- */

static int32_t async_write_perform(const char *path, const uint8_t *data,
                                   uint32_t size, uint32_t *io_us)
{
    uint64_t begin = async_native_time_us();
    uint64_t elapsed;
    int32_t descriptor;
    int32_t result = 0;
    uint32_t done = 0U;

    descriptor = async_native_open(path);
    if (descriptor < 0) {
        result = descriptor;
    } else {
        while (done < size) {
            int32_t written = async_native_write(
                descriptor, data + done, size - done);
            if (written <= 0) {
                result = written < 0 ? written : -1;
                break;
            }
            done += (uint32_t)written;
        }
        {
            int32_t close_result = async_native_close(descriptor);
            if (result == 0 && close_result < 0)
                result = close_result;
        }
    }
    elapsed = async_native_time_us() - begin;
    *io_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    return result;
}

static void async_note_completion_locked(uint32_t io_us, uint32_t size,
                                         int32_t result)
{
    if (result == 0) {
        async_counter_add(&s_stats.jobs_completed, 1U);
        async_counter_add(&s_stats.bytes_written, size);
    } else {
        async_counter_add(&s_stats.jobs_failed, 1U);
    }
    async_counter_add(&s_stats.io_us_total, io_us);
    async_counter_max(&s_stats.io_us_max, io_us);
}

/* Caller owns the lock.  Returns the oldest PENDING job marked RUNNING. */
static async_write_job *async_take_pending_locked(void)
{
    async_write_job *chosen = NULL;
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY; ++index) {
        async_write_job *job = &s_jobs[index];
        if (job->state == ASYNC_JOB_PENDING &&
            (!chosen ||
             (int32_t)(job->sequence - chosen->sequence) < 0))
            chosen = job;
    }
    if (chosen)
        chosen->state = ASYNC_JOB_RUNNING;
    return chosen;
}

/* Perform one already-RUNNING job and publish it as DONE. */
static void async_run_job(async_write_job *job)
{
    uint32_t io_us = 0U;
    uint32_t queue_wait_us;
    uint64_t now;
    int32_t result;

    now = async_native_time_us();
    queue_wait_us = now > job->queued_us && now - job->queued_us < UINT32_MAX
        ? (uint32_t)(now - job->queued_us) : 0U;
    result = async_write_perform(job->path, job->data, job->size, &io_us);
    async_lock();
    job->result = result;
    job->io_us = io_us;
    job->state = ASYNC_JOB_DONE;
    async_note_completion_locked(io_us, job->size, result);
    async_unlock();
    isaac_vita_log(
        "[isaac-asyncsave] job bid40=%.40s seq=%u file=%.48s bytes=%u "
        "io_us=%u queue_wait_us=%u rc=0x%08x",
        ISAAC_VITA_ASYNC_WRITE_BUILD_ID, (unsigned)job->sequence,
        async_path_tail(job->path), (unsigned)job->size,
        (unsigned)io_us, (unsigned)queue_wait_us, (unsigned)result);
}

/* Guest thread only: release the lanes of DONE jobs. */
static void async_reclaim_locked(void)
{
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY; ++index) {
        async_write_job *job = &s_jobs[index];
        if (job->state != ASYNC_JOB_DONE)
            continue;
        async_lane_release(job->lane);
        job->data = NULL;
        job->size = 0U;
        job->path[0] = '\0';
        job->state = ASYNC_JOB_EMPTY;
    }
}

/* Caller owns the lock.  1 when a PENDING or RUNNING job targets the path,
 * or when any job is live for a NULL path. */
static int async_busy_locked(const char *path)
{
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY; ++index) {
        const async_write_job *job = &s_jobs[index];
        if (job->state != ASYNC_JOB_PENDING && job->state != ASYNC_JOB_RUNNING)
            continue;
        if (!path || strcmp(job->path, path) == 0)
            return 1;
    }
    return 0;
}

/* ---- worker ------------------------------------------------------------ */

#if !defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
static int async_write_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        (void)sceKernelWaitSema(s_wake_sema, 1, NULL);
        if (s_stop)
            return 0;
        for (;;) {
            async_write_job *job;

            async_lock();
            job = async_take_pending_locked();
            async_unlock();
            if (!job)
                break;
            async_run_job(job);
            (void)sceKernelSetEventFlag(s_done_flag, 1U);
        }
    }
}

static int async_worker_start(void)
{
    int start_result;

    s_wake_sema = sceKernelCreateSema("isaac_save_wake", 0, 0, 1024, NULL);
    s_done_flag = sceKernelCreateEventFlag("isaac_save_done", 0, 0, NULL);
    if (s_wake_sema < 0 || s_done_flag < 0)
        goto fail;
    s_thread = sceKernelCreateThread(
        "isaac_save_writer", async_write_worker, ASYNC_WRITE_THREAD_PRIORITY,
        ASYNC_WRITE_THREAD_STACK, 0U,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, NULL);
    if (s_thread < 0)
        goto fail;
    start_result = sceKernelStartThread(s_thread, 0U, NULL);
    if (start_result < 0) {
        (void)sceKernelDeleteThread(s_thread);
        s_thread = -1;
        goto fail;
    }
    isaac_vita_log(
        "[isaac-asyncsave] worker bid40=%.40s state=start thread=0x%08x "
        "priority=0x%08x queue=%u root=%s",
        ISAAC_VITA_ASYNC_WRITE_BUILD_ID, (unsigned)s_thread,
        (unsigned)ASYNC_WRITE_THREAD_PRIORITY,
        (unsigned)ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY,
        ASYNC_WRITE_SAVE_ROOT);
    return 1;
fail:
    isaac_vita_log(
        "[isaac-asyncsave] worker bid40=%.40s state=start-failed "
        "sema=0x%08x flag=0x%08x thread=0x%08x",
        ISAAC_VITA_ASYNC_WRITE_BUILD_ID, (unsigned)s_wake_sema,
        (unsigned)s_done_flag, (unsigned)s_thread);
    if (s_wake_sema >= 0)
        (void)sceKernelDeleteSema(s_wake_sema);
    if (s_done_flag >= 0)
        (void)sceKernelDeleteEventFlag(s_done_flag);
    s_wake_sema = -1;
    s_done_flag = -1;
    return 0;
}

static void async_worker_stop(void)
{
    if (s_thread < 0)
        return;
    s_stop = 1U;
    __sync_synchronize();
    (void)sceKernelSignalSema(s_wake_sema, 1);
    {
        SceUInt timeout = ASYNC_WRITE_WAIT_TIMEOUT_US;
        (void)sceKernelWaitThreadEnd(s_thread, NULL, &timeout);
    }
    (void)sceKernelDeleteThread(s_thread);
    (void)sceKernelDeleteSema(s_wake_sema);
    (void)sceKernelDeleteEventFlag(s_done_flag);
    s_thread = -1;
    s_wake_sema = -1;
    s_done_flag = -1;
}
#endif

static int async_worker_ready(void)
{
    if (s_stats.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_READY)
        return 1;
    if (s_stats.worker_state != ISAAC_VITA_ASYNC_WRITE_WORKER_COLD ||
        s_shutdown)
        return 0;
#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
    s_stats.worker_state = isaac_vita_async_write_oracle_worker_start()
        ? ISAAC_VITA_ASYNC_WRITE_WORKER_READY
        : ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED;
#else
    s_stats.worker_state = async_worker_start()
        ? ISAAC_VITA_ASYNC_WRITE_WORKER_READY
        : ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED;
#endif
    return s_stats.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_READY;
}

/* One wait step: give the worker a chance to publish a completion.  Returns 1
 * when the worker answered (a job may have finished; the caller re-polls) and
 * 0 when it did not respond within the step window.  The oracle drives the
 * "worker" by performing one queued job itself; run_one returns 0 when nothing
 * is runnable (an empty queue or a job the test has stranded in RUNNING). */
static int async_wait_step(void)
{
#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
    return isaac_vita_async_write_oracle_run_one();
#else
    SceUInt timeout = ASYNC_WRITE_WAIT_TIMEOUT_US;
    int result = sceKernelWaitEventFlag(
        s_done_flag, 1U, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
        NULL, &timeout);
    return result >= 0 ? 1 : 0;
#endif
}

/* Guest thread: block until no live job matches (NULL = any job).
 *
 * The wait is bounded and self-healing:
 *   - while the worker keeps answering, re-poll until the path is clear
 *     (normal read-your-writes: the worker signals the flag in ~ms);
 *   - if the worker goes silent, rescue any PENDING job on this thread so a
 *     never-scheduled queue still drains;
 *   - if only a RUNNING job remains and the worker stays silent past the
 *     ceiling, declare the worker failed and return best-effort rather than
 *     block startup forever.  A worker already marked failed never blocks: it
 *     drains PENDING work directly and returns, so a wedged write cannot make
 *     every later file probe pay the ceiling again. */
static void async_wait(const char *path)
{
    uint64_t begin = async_native_time_us();
    uint64_t elapsed;
    int waited = 0;

    for (;;) {
        async_write_job *job;
        int busy;

        async_lock();
        async_reclaim_locked();
        busy = async_busy_locked(path);
        async_unlock();
        if (!busy)
            break;
        waited = 1;

        if (s_stats.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED) {
            /* A dead worker never completes a RUNNING job and never signals
             * the flag.  Rescue queued work directly; never wait on it. */
            async_lock();
            job = async_take_pending_locked();
            async_unlock();
            if (job) {
                async_run_job(job);
                continue;
            }
            break;
        }

        if (async_wait_step())
            continue;

        /* No progress this step: take over PENDING work ourselves. */
        async_lock();
        job = async_take_pending_locked();
        async_unlock();
        if (job) {
            async_run_job(job);
            begin = async_native_time_us();
            continue;
        }

        /* Only a stranded RUNNING job remains and the worker is silent.  Do
         * not block indefinitely: past the ceiling, fail the worker and
         * return so a wedged write cannot freeze startup or later file ops. */
        elapsed = async_native_time_us() - begin;
        if (elapsed >= ASYNC_WRITE_WAIT_BUDGET_US) {
            async_lock();
            s_stats.worker_state = ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED;
            async_unlock();
            isaac_vita_log(
                "[isaac-asyncsave] wait bid40=%.40s state=abandoned "
                "path=%.48s wait_us=%u",
                ISAAC_VITA_ASYNC_WRITE_BUILD_ID,
                path ? async_path_tail(path) : "<all>",
                elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
            break;
        }
    }
    if (waited) {
        elapsed = async_native_time_us() - begin;
        async_lock();
        async_counter_add(&s_stats.sync_waits, 1U);
        async_counter_add(&s_stats.sync_wait_us,
                          elapsed > UINT32_MAX ? UINT32_MAX
                                               : (uint32_t)elapsed);
        async_counter_max(&s_stats.sync_wait_us_max,
                          elapsed > UINT32_MAX ? UINT32_MAX
                                               : (uint32_t)elapsed);
        async_unlock();
    }
}

static void async_synchronous(const char *path, uint8_t *data, uint32_t size,
                              uint32_t lane)
{
    uint32_t io_us = 0U;
    int32_t result;

    /* Keep same-path ordering even on the fallback lane. */
    async_wait(path);
    result = async_write_perform(path, data, size, &io_us);
    async_lock();
    async_counter_add(&s_stats.jobs_synchronous, 1U);
    async_note_completion_locked(io_us, size, result);
    async_unlock();
    isaac_vita_log(
        "[isaac-asyncsave] sync bid40=%.40s file=%.48s bytes=%u io_us=%u "
        "rc=0x%08x worker=%u",
        ISAAC_VITA_ASYNC_WRITE_BUILD_ID, async_path_tail(path),
        (unsigned)size, (unsigned)io_us, (unsigned)result,
        (unsigned)s_stats.worker_state);
    async_lane_release(lane);
}

int isaac_vita_async_write_image_close(isaac_vita_async_write_image *image)
{
    async_write_job *slot = NULL;
    uint8_t *data;
    uint32_t size;
    uint32_t lane;
    uint32_t index;
    uint32_t live;

    if (!image || !image->live) {
        errno = EBADF;
        return EOF;
    }
    image->live = 0U;
    data = image->data;
    size = image->size;
    lane = image->lane;
    image->data = NULL;
    if (image->error) {
        /* The native path would have left a partial file behind a failed
         * fwrite.  Keeping the previous save intact is the safer answer for
         * a file the guest will reload; report the failure exactly once. */
        async_lane_release(lane);
        async_lock();
        async_counter_add(&s_stats.images_closed_with_error, 1U);
        async_unlock();
        errno = EIO;
        return EOF;
    }
    if (!async_worker_ready()) {
        async_synchronous(image->path, data, size, lane);
        return 0;
    }
    async_lock();
    async_reclaim_locked();
    live = 0U;
    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY; ++index) {
        if (s_jobs[index].state == ASYNC_JOB_EMPTY) {
            if (!slot)
                slot = &s_jobs[index];
        } else {
            ++live;
        }
    }
    if (!slot) {
        async_unlock();
        async_synchronous(image->path, data, size, lane);
        return 0;
    }
    slot->data = data;
    slot->size = size;
    slot->lane = lane;
    slot->sequence = ++s_sequence;
    slot->io_us = 0U;
    slot->result = 0;
    slot->queued_us = async_native_time_us();
    memcpy(slot->path, image->path, strlen(image->path) + 1U);
    slot->state = ASYNC_JOB_PENDING;
    async_counter_add(&s_stats.jobs_queued, 1U);
    async_counter_max(&s_stats.peak_queue, live + 1U);
    async_unlock();
#if !defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
    (void)sceKernelSignalSema(s_wake_sema, 1);
#endif
    return 0;
}

void isaac_vita_async_write_sync_path(const char *native_path)
{
    if (!native_path)
        return;
    if (s_stats.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_COLD)
        return;
    async_wait(native_path);
}

void isaac_vita_async_write_drain(void)
{
    if (s_stats.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_COLD)
        return;
    async_wait(NULL);
}

void isaac_vita_async_write_snapshot_read(
    isaac_vita_async_write_snapshot *snapshot)
{
    if (!snapshot)
        return;
    async_lock();
    *snapshot = s_stats;
    async_unlock();
}

void isaac_vita_async_write_shutdown(void)
{
    isaac_vita_async_write_snapshot snapshot;

    if (s_shutdown)
        return;
    isaac_vita_async_write_drain();
    s_shutdown = 1U;
#if !defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
    async_worker_stop();
#endif
    async_lock();
    async_reclaim_locked();
    if (s_stats.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_READY)
        s_stats.worker_state = ISAAC_VITA_ASYNC_WRITE_WORKER_STOPPED;
    snapshot = s_stats;
    async_unlock();
    isaac_vita_log(
        "[isaac-asyncsave] summary bid40=%.40s worker=%u "
        "images(open,lane_full,arena_fail,err)=%u/%u/%u/%u "
        "jobs(q,s,ok,fail)=%u,%u,%u,%u bytes=%u io_us(total,max)=%u,%u "
        "sync_wait(n,us,max)=%u,%u,%u peak_queue=%u",
        ISAAC_VITA_ASYNC_WRITE_BUILD_ID, (unsigned)snapshot.worker_state,
        (unsigned)snapshot.images_opened,
        (unsigned)snapshot.images_lane_exhausted,
        (unsigned)snapshot.images_arena_failed,
        (unsigned)snapshot.images_closed_with_error,
        (unsigned)snapshot.jobs_queued, (unsigned)snapshot.jobs_synchronous,
        (unsigned)snapshot.jobs_completed, (unsigned)snapshot.jobs_failed,
        (unsigned)snapshot.bytes_written, (unsigned)snapshot.io_us_total,
        (unsigned)snapshot.io_us_max, (unsigned)snapshot.sync_waits,
        (unsigned)snapshot.sync_wait_us, (unsigned)snapshot.sync_wait_us_max,
        (unsigned)snapshot.peak_queue);
}

#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
int isaac_vita_async_write_oracle_run_one(void)
{
    async_write_job *job;

    async_lock();
    job = async_take_pending_locked();
    async_unlock();
    if (!job)
        return 0;
    async_run_job(job);
    return 1;
}

uint32_t isaac_vita_async_write_oracle_pending(void)
{
    uint32_t index;
    uint32_t pending = 0U;

    async_lock();
    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY; ++index) {
        if (s_jobs[index].state == ASYNC_JOB_PENDING ||
            s_jobs[index].state == ASYNC_JOB_RUNNING)
            ++pending;
    }
    async_unlock();
    return pending;
}

/* Test seam: mark the oldest PENDING job RUNNING without completing it,
 * modelling a worker that accepted the write but never returned from the
 * native call (the device hang).  A later async_wait on that path must not
 * block forever; it must bound out and mark the worker failed. */
int isaac_vita_async_write_oracle_strand_one(void)
{
    async_write_job *job;

    async_lock();
    job = async_take_pending_locked();
    async_unlock();
    return job != NULL;
}

/* Test seam: return the module to a pristine, non-shutdown state so an
 * independent case can run after the lifecycle tests.  The lane arena stays
 * reserved (it is a one-shot reservation on the device too). */
void isaac_vita_async_write_oracle_reset(void)
{
    uint32_t index;

    async_lock();
    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY; ++index) {
        s_jobs[index].state = ASYNC_JOB_EMPTY;
        s_jobs[index].data = NULL;
        s_jobs[index].size = 0U;
        s_jobs[index].path[0] = '\0';
    }
    s_lane_busy = 0U;
    s_sequence = 0U;
    s_shutdown = 0U;
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.abi = ISAAC_VITA_ASYNC_WRITE_SNAPSHOT_ABI;
    async_unlock();
}
#endif
