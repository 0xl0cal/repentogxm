/* Asynchronous logger: one bounded byte ring, one logger thread.  See
 * host_vita_log_async.h for the contract and the thread placement. */
#include "host_vita_log_async.h"

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#if defined(ISAAC_VITA_LOG_ASYNC_ORACLE)
# include <stdio.h>
#else
# include <psp2/kernel/clib.h>
# include <psp2/kernel/cpu.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/threadmgr.h>
#endif

_Static_assert(ISAAC_VITA_LOG_ASYNC_RING_BYTES >=
                   2u * (ISAAC_VITA_LOG_ASYNC_HEADER_BYTES +
                         ISAAC_VITA_LOG_ASYNC_LINE_MAX),
               "log-async ring must hold at least two maximal lines");
_Static_assert(ISAAC_VITA_LOG_ASYNC_LINE_MAX <= 0xffffu,
               "log-async header carries a 16-bit length");
_Static_assert(ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES <=
                   ISAAC_VITA_LOG_ASYNC_LINE_MAX,
               "log-async printf buffer must fit one queued line");

#if defined(__GNUC__)
# define LOG_ASYNC_LOAD_STARTED() \
    __atomic_load_n(&s_started, __ATOMIC_ACQUIRE)
# define LOG_ASYNC_STORE_STARTED(value) \
    __atomic_store_n(&s_started, (value), __ATOMIC_RELEASE)
#else
# define LOG_ASYNC_LOAD_STARTED() (s_started)
# define LOG_ASYNC_STORE_STARTED(value) ((void)(s_started = (value)))
#endif

/* Ring state: every field below is read and written under the ring lock
 * except s_started (published once, atomically). */
static unsigned char s_ring[ISAAC_VITA_LOG_ASYNC_RING_BYTES];
static uint32_t s_head;           /* next byte the drainer reads */
static uint32_t s_tail;           /* next byte a producer writes */
static uint32_t s_used;
static uint32_t s_high_water;
static uint32_t s_enqueued;
static uint32_t s_sunk;
static uint32_t s_dropped;
static int s_started;
static uint32_t s_start_result;
static uint32_t s_thread_priority;
/* Drop reporting: owned by whoever holds the sink lock. */
static uint32_t s_dropped_reported;
static uint32_t s_drop_reports;
static uint64_t s_drop_reported_at_us;

/* ------------------------------------------------------------------------
 * Platform seams. */
#if defined(ISAAC_VITA_LOG_ASYNC_ORACLE)
isaac_vita_log_async_sink_fn g_isaac_vita_log_async_oracle_file_sink;
isaac_vita_log_async_sink_fn g_isaac_vita_log_async_oracle_printf_sink;
uint64_t g_isaac_vita_log_async_oracle_now_us;
uint32_t g_isaac_vita_log_async_oracle_wakes;
uint32_t g_isaac_vita_log_async_oracle_lock_faults;
static int s_oracle_ring_locked;
static int s_oracle_sink_locked;

static void log_async_ring_lock(void)
{
    if (s_oracle_ring_locked)
        ++g_isaac_vita_log_async_oracle_lock_faults;
    s_oracle_ring_locked = 1;
}

static void log_async_ring_unlock(void)
{
    if (!s_oracle_ring_locked)
        ++g_isaac_vita_log_async_oracle_lock_faults;
    s_oracle_ring_locked = 0;
}

static int log_async_sink_lock(void)
{
    if (s_oracle_sink_locked)
        ++g_isaac_vita_log_async_oracle_lock_faults;
    s_oracle_sink_locked = 1;
    return 1;
}

static void log_async_sink_unlock(void)
{
    if (!s_oracle_sink_locked)
        ++g_isaac_vita_log_async_oracle_lock_faults;
    s_oracle_sink_locked = 0;
}

static void log_async_wake(void)
{
    ++g_isaac_vita_log_async_oracle_wakes;
}

static uint64_t log_async_now_us(void)
{
    return g_isaac_vita_log_async_oracle_now_us;
}

static void log_async_sink_file(const char *line, unsigned length)
{
    if (g_isaac_vita_log_async_oracle_file_sink)
        g_isaac_vita_log_async_oracle_file_sink(line, length);
}

static void log_async_sink_printf(const char *line, unsigned length)
{
    if (g_isaac_vita_log_async_oracle_printf_sink)
        g_isaac_vita_log_async_oracle_printf_sink(line, length);
}

static int log_async_vformat(char *buffer, size_t size, const char *format,
                             va_list args)
{
    return vsnprintf(buffer, size, format, args);
}

/* The synchronous over-long path: on the Vita sceClibVprintf writes the
 * exact bytes; here the fake printf sink receives them. */
static void log_async_vprintf_sync(const char *format, va_list args)
{
    static char scratch[8192];
    int length = vsnprintf(scratch, sizeof scratch, format, args);

    if (length <= 0)
        return;
    if (length >= (int)sizeof scratch)
        length = (int)sizeof scratch - 1;
    log_async_sink_printf(scratch, (unsigned)length);
}
#else
static SceKernelLwMutexWork s_ring_mutex;
static SceKernelLwMutexWork s_sink_mutex;
static SceUID s_wake_flag = -1;
static SceUID s_thread = -1;

static void log_async_ring_lock(void)
{
    (void)sceKernelLockLwMutex(&s_ring_mutex, 1, NULL);
}

static void log_async_ring_unlock(void)
{
    (void)sceKernelUnlockLwMutex(&s_ring_mutex, 1);
}

/* The sink lock serialises drainers (logger thread, flush callers) so two
 * of them can never reorder lines.  A flush on the exit or abort path must
 * not hang behind a wedged logger, so it waits a bounded time and then
 * proceeds without the lock (ordering is then best effort). */
static int log_async_sink_lock(void)
{
    unsigned int timeout_us = 500000u;

    return sceKernelLockLwMutex(&s_sink_mutex, 1, &timeout_us) >= 0;
}

static void log_async_sink_unlock(void)
{
    (void)sceKernelUnlockLwMutex(&s_sink_mutex, 1);
}

static void log_async_wake(void)
{
    (void)sceKernelSetEventFlag(s_wake_flag, 1u);
}

static uint64_t log_async_now_us(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide();
}

static void log_async_sink_file(const char *line, unsigned length)
{
    isaac_vita_log_sink_file(line, length);
}

static void log_async_sink_printf(const char *line, unsigned length)
{
    (void)length;
    /* One call is one Cat-A-Log / Vita3K record, exactly as before. */
    sceClibPrintf("%s", line);
}

static int log_async_vformat(char *buffer, size_t size, const char *format,
                             va_list args)
{
    return sceClibVsnprintf(buffer, (SceSize)size, format, args);
}

static void log_async_vprintf_sync(const char *format, va_list args)
{
    (void)sceClibVprintf(format, args);
}
#endif

/* ------------------------------------------------------------------------
 * Byte ring (ring lock held). */
static void log_async_ring_put(const unsigned char *source, uint32_t count)
{
    uint32_t first = ISAAC_VITA_LOG_ASYNC_RING_BYTES - s_tail;

    if (first > count)
        first = count;
    memcpy(s_ring + s_tail, source, first);
    if (count > first)
        memcpy(s_ring, source + first, count - first);
    s_tail += count;
    if (s_tail >= ISAAC_VITA_LOG_ASYNC_RING_BYTES)
        s_tail -= ISAAC_VITA_LOG_ASYNC_RING_BYTES;
    s_used += count;
}

static void log_async_ring_take(unsigned char *destination, uint32_t count)
{
    uint32_t first = ISAAC_VITA_LOG_ASYNC_RING_BYTES - s_head;

    if (first > count)
        first = count;
    memcpy(destination, s_ring + s_head, first);
    if (count > first)
        memcpy(destination + first, s_ring, count - first);
    s_head += count;
    if (s_head >= ISAAC_VITA_LOG_ASYNC_RING_BYTES)
        s_head -= ISAAC_VITA_LOG_ASYNC_RING_BYTES;
    s_used -= count;
}

/* ------------------------------------------------------------------------
 * Drain side (sink lock held by the caller). */
static int log_async_pop(unsigned char *buffer, unsigned *sink,
                         uint32_t *length)
{
    unsigned char header[ISAAC_VITA_LOG_ASYNC_HEADER_BYTES];
    uint32_t count;

    log_async_ring_lock();
    if (s_used == 0u) {
        log_async_ring_unlock();
        return 0;
    }
    log_async_ring_take(header, ISAAC_VITA_LOG_ASYNC_HEADER_BYTES);
    count = (uint32_t)header[0] | ((uint32_t)header[1] << 8);
    *sink = header[2];
    log_async_ring_take(buffer, count);
    ++s_sunk;
    log_async_ring_unlock();
    buffer[count] = 0u;
    *length = count;
    return 1;
}

static void log_async_sink(unsigned sink, const char *line, uint32_t length)
{
    if (sink == ISAAC_VITA_LOG_ASYNC_SINK_FILE)
        log_async_sink_file(line, (unsigned)length);
    else
        log_async_sink_printf(line, (unsigned)length);
}

static int log_async_snprintf(char *buffer, size_t size, const char *format,
                              ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = log_async_vformat(buffer, size, format, args);
    va_end(args);
    return result;
}

/* A changed drop count is written by the drainer itself (never through the
 * ring, which may be the reason).  Rate: at most one report per
 * ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US unless forced (flush). */
static void log_async_report_drops(int force)
{
    char line[64];
    uint32_t dropped;
    uint64_t now;
    int length;

    log_async_ring_lock();
    dropped = s_dropped;
    log_async_ring_unlock();
    if (dropped == s_dropped_reported)
        return;
    now = log_async_now_us();
    if (!force && s_drop_reports != 0u &&
            now - s_drop_reported_at_us < ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US)
        return;
    s_dropped_reported = dropped;
    s_drop_reported_at_us = now;
    ++s_drop_reports;
    length = log_async_snprintf(line, sizeof line,
                                "[kage-vita] log-async dropped=%u\n",
                                (unsigned)dropped);
    if (length <= 0)
        return;
    if (length >= (int)sizeof line)
        length = (int)sizeof line - 1;
    log_async_sink_printf(line, (unsigned)length);
}

static uint32_t log_async_drain(unsigned char *buffer, int force_report)
{
    uint32_t count = 0u;
    uint32_t length;
    unsigned sink;
    int locked = log_async_sink_lock();

    while (log_async_pop(buffer, &sink, &length)) {
        log_async_sink(sink, (const char *)buffer, length);
        ++count;
        /* Under sustained overload the ring may never empty; keep the drop
         * report flowing (rate bounded) every 64 lines. */
        if ((count & 63u) == 0u)
            log_async_report_drops(0);
    }
    log_async_report_drops(force_report);
    if (locked)
        log_async_sink_unlock();
    return count;
}

/* ------------------------------------------------------------------------
 * Logger thread. */
#if !defined(ISAAC_VITA_LOG_ASYNC_ORACLE)
static int log_async_thread(SceSize args, void *argp)
{
    unsigned char buffer[ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u];

    (void)args;
    (void)argp;
    for (;;) {
        /* The 1 s timeout is a safety net for a lost wake, not a poll: a
         * producer sets the flag whenever the ring goes from empty to
         * non-empty, so the thread otherwise sleeps until the next line. */
        unsigned int timeout_us = 1000000u;

        (void)log_async_drain(buffer, 0);
        (void)sceKernelWaitEventFlag(s_wake_flag, 1u,
                                     SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
                                     NULL, &timeout_us);
    }
    return 0;
}

static SceUID log_async_create_thread(int priority)
{
    return sceKernelCreateThread(
        "isaac_log_async", log_async_thread, priority,
        ISAAC_VITA_LOG_ASYNC_THREAD_STACK_BYTES, 0U,
        SCE_KERNEL_CPU_MASK_USER_2, NULL);
}
#endif

int isaac_vita_log_async_start(void)
{
    if (LOG_ASYNC_LOAD_STARTED())
        return 1;
#if defined(ISAAC_VITA_LOG_ASYNC_ORACLE)
    s_start_result = 0u;
    s_thread_priority = ISAAC_VITA_LOG_ASYNC_THREAD_PRIORITY;
    LOG_ASYNC_STORE_STARTED(1);
    return 1;
#else
    {
        int rc;

        rc = sceKernelCreateLwMutex(&s_ring_mutex, "isaac_log_ring", 0, 0,
                                    NULL);
        if (rc < 0) {
            s_start_result = (uint32_t)rc;
            return 0;
        }
        rc = sceKernelCreateLwMutex(&s_sink_mutex, "isaac_log_sink", 0, 0,
                                    NULL);
        if (rc < 0) {
            s_start_result = (uint32_t)rc;
            return 0;
        }
        s_wake_flag = sceKernelCreateEventFlag("isaac_log_wake", 0, 0, NULL);
        if (s_wake_flag < 0) {
            s_start_result = (uint32_t)s_wake_flag;
            return 0;
        }
        s_thread_priority = ISAAC_VITA_LOG_ASYNC_THREAD_PRIORITY;
        s_thread = log_async_create_thread((int)s_thread_priority);
        if (s_thread < 0) {
            /* Lowest user priority refused: run at the process default
             * (the value the save writer and stall probe use) and say so. */
            s_thread_priority = 0x10000100u;
            s_thread = log_async_create_thread((int)s_thread_priority);
        }
        if (s_thread < 0) {
            s_start_result = (uint32_t)s_thread;
            return 0;
        }
        LOG_ASYNC_STORE_STARTED(1);
        rc = sceKernelStartThread(s_thread, 0U, NULL);
        if (rc < 0) {
            unsigned char buffer[ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u];

            LOG_ASYNC_STORE_STARTED(0);
            (void)log_async_drain(buffer, 1);
            (void)sceKernelDeleteThread(s_thread);
            s_thread = -1;
            s_start_result = (uint32_t)rc;
            return 0;
        }
        s_start_result = 0u;
        return 1;
    }
#endif
}

int isaac_vita_log_async_started(void)
{
    return LOG_ASYNC_LOAD_STARTED() != 0;
}

/* ------------------------------------------------------------------------
 * Producers. */
int isaac_vita_log_async_enqueue(unsigned sink, const char *line,
                                 unsigned length)
{
    unsigned char header[ISAAC_VITA_LOG_ASYNC_HEADER_BYTES];
    int was_empty;

    if (!LOG_ASYNC_LOAD_STARTED())
        return 0;
    if (!line || length == 0u)
        return 1;
    header[0] = (unsigned char)(length & 0xffu);
    header[1] = (unsigned char)((length >> 8) & 0xffu);
    header[2] = (unsigned char)sink;
    header[3] = 0u;
    log_async_ring_lock();
    if (length > ISAAC_VITA_LOG_ASYNC_LINE_MAX ||
            sink > ISAAC_VITA_LOG_ASYNC_SINK_PRINTF ||
            s_used + ISAAC_VITA_LOG_ASYNC_HEADER_BYTES + length >
                ISAAC_VITA_LOG_ASYNC_RING_BYTES) {
        ++s_dropped;
        log_async_ring_unlock();
        return -1;
    }
    was_empty = s_used == 0u;
    log_async_ring_put(header, ISAAC_VITA_LOG_ASYNC_HEADER_BYTES);
    log_async_ring_put((const unsigned char *)line, length);
    if (s_used > s_high_water)
        s_high_water = s_used;
    ++s_enqueued;
    log_async_ring_unlock();
    /* The logger waits only after it saw the ring empty under the lock, so
     * exactly the empty -> non-empty transition needs a wake. */
    if (was_empty)
        log_async_wake();
    return 1;
}

void isaac_vita_log_async_printf(const char *format, ...)
{
    char line[ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES];
    va_list args;
    va_list again;
    int length;

    va_start(args, format);
    va_copy(again, args);
    length = log_async_vformat(line, sizeof line, format, args);
    va_end(args);
    if (length >= (int)sizeof line) {
        /* Longer than one queued line: never truncate a record (the newline
         * would be lost and Cat-A-Log would merge it with the next one).
         * Drain what is queued so program order holds, then write the exact
         * bytes synchronously, as the unrouted call did. */
        isaac_vita_log_async_flush();
        log_async_vprintf_sync(format, again);
        va_end(again);
        return;
    }
    va_end(again);
    if (length <= 0)
        return;
    if (isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, line,
                                     (unsigned)length) != 0)
        return;
    log_async_sink_printf(line, (unsigned)length);
}

void isaac_vita_log_async_flush(void)
{
    unsigned char buffer[ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u];

    if (!LOG_ASYNC_LOAD_STARTED())
        return;
    (void)log_async_drain(buffer, 1);
}

void isaac_vita_log_async_get_stats(IsaacVitaLogAsyncStats *stats)
{
    int started;

    if (!stats)
        return;
    started = LOG_ASYNC_LOAD_STARTED();
    /* The ring lock exists only once start created it. */
    if (started)
        log_async_ring_lock();
    stats->started = started ? 1u : 0u;
    stats->start_result = s_start_result;
    stats->thread_priority = s_thread_priority;
    stats->enqueued = s_enqueued;
    stats->sunk = s_sunk;
    stats->dropped = s_dropped;
    stats->used = s_used;
    stats->high_water = s_high_water;
    if (started)
        log_async_ring_unlock();
}

#if defined(ISAAC_VITA_LOG_ASYNC_ORACLE)
uint32_t isaac_vita_log_async_oracle_service(void)
{
    unsigned char buffer[ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u];

    return log_async_drain(buffer, 0);
}

void isaac_vita_log_async_oracle_state(uint32_t *head, uint32_t *tail,
                                       uint32_t *used)
{
    log_async_ring_lock();
    if (head)
        *head = s_head;
    if (tail)
        *tail = s_tail;
    if (used)
        *used = s_used;
    log_async_ring_unlock();
}
#else
/* GNU ld --wrap=abort (added by the CMake option): every abort(), including
 * a failed newlib assert, drains the ring before the process dies. */
extern void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void)
{
    isaac_vita_log_async_flush();
    __real_abort();
}
#endif
