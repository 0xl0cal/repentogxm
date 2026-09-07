/* Asynchronous logger for the Vita runtime (ISAAC_VITA_LOG_ASYNC).
 *
 * Every durable log line (isaac_vita_log: ux0 file append plus one
 * sceClibPrintf) and every per-window [kage-vita] record (sceClibPrintf, the
 * kernel debug printf that Cat-A-Log captures over the network) used to be
 * written synchronously by the thread that formatted it.  Measured on the
 * device 2026-09-04: the 14-31 record lines at the end of every 120-loop
 * ph120 window stalled the game thread 60-100 ms every 2 s, outside every
 * profiled phase (window wall time 2069 ms per 2006 ms of loops).
 *
 * With the option ON the producer still formats into its own stack buffer
 * (isaac_vita_log computes its [time thr] prefix at enqueue time, so every
 * byte equals the synchronous output) and copies the line behind a 4-byte
 * header (length, sink tag) into one bounded byte ring shared by every
 * producer thread (game thread, audio worker, save writer, guest sampler,
 * native vorbis worker).  One logger thread drains the ring strictly FIFO and
 * performs the sinks. With FILE_BATCH enabled, already queued adjacent FILE
 * records may share one file append; debug output remains one call per line.
 * A full ring drops the line and
 * counts it; the logger thread reports "[kage-vita] log-async dropped=N" when
 * the count changes, at most once per ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US
 * unless a flush forces it.
 *
 * Before isaac_vita_log_async_start() (early boot, isaac_vita_log_reset)
 * the producers write synchronously as before.  isaac_vita_log_async_flush()
 * drains synchronously on the caller thread: the process-exit path
 * (entry_vita.c) and abort() (GNU ld --wrap=abort, added by the CMake option,
 * so a failed newlib assert flushes too) call it so no queued line is lost
 * when the game dies.  A kernel-level crash of a native thread has no user
 * handler and can still lose the lines queued since the last drain.
 *
 * Logger thread: priority 191, the lowest user priority (game thread default
 * 160, guest sampler 96, native vorbis worker 170), pinned to CPU 2
 * (SCE_KERNEL_CPU_MASK_USER_2) where the OpenAL mixer and the vorbis worker
 * already live. This affinity keeps the logger's user thread off the game
 * and sampler cores; it does not establish where all kernel I/O work runs or
 * eliminate CPU, cache, storage, or audio contention. Both audio threads have
 * higher priority on CPU 2. If the kernel rejects 191 the start falls
 * back to the process default priority and reports it.
 *
 * Host oracle: recomp/runtime/host_vita_log_async_oracle.c drives the same
 * ring with fake sinks, a fake clock and a fake logger-thread step
 * (ISAAC_VITA_LOG_ASYNC_ORACLE). */
#ifndef ISAAC_HOST_VITA_LOG_ASYNC_H
#define ISAAC_HOST_VITA_LOG_ASYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sink tag carried by every queued line.  Both kinds share one ring so they
 * stay in program order. */
enum isaac_vita_log_async_sink {
    /* isaac_vita_log: platform.c log_write (ux0 file append + one
     * sceClibPrintf), via isaac_vita_log_sink_file. */
    ISAAC_VITA_LOG_ASYNC_SINK_FILE = 0,
    /* One sceClibPrintf("%s", line): ph120 records and the other hot-path
     * [kage-vita] lines. */
    ISAAC_VITA_LOG_ASYNC_SINK_PRINTF = 1
};

#ifndef ISAAC_VITA_LOG_ASYNC_RING_KB
#define ISAAC_VITA_LOG_ASYNC_RING_KB 256
#endif
#define ISAAC_VITA_LOG_ASYNC_RING_BYTES \
    ((uint32_t)ISAAC_VITA_LOG_ASYNC_RING_KB * 1024u)
/* Longest queued line.  isaac_vita_log formats into 512 bytes; the ph120
 * records are bounded per record by the phase-profile oracle (ph120.s is the
 * longest at 503 bytes with every field at UINT32_MAX).  A longer enqueue is
 * dropped and counted, never truncated or split. */
#define ISAAC_VITA_LOG_ASYNC_LINE_MAX 1024u
/* Ring bytes per queued line in front of the payload: little-endian uint16
 * length, uint8 sink tag, one reserved byte. */
#define ISAAC_VITA_LOG_ASYNC_HEADER_BYTES 4u
#define ISAAC_VITA_LOG_ASYNC_THREAD_PRIORITY 191
#define ISAAC_VITA_LOG_ASYNC_THREAD_STACK_BYTES (16u * 1024u)
#define ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US 1000000u
/* isaac_vita_log_async_printf formats into this many bytes (one queued
 * line).  Output that does not fit is never truncated: the ring is drained
 * and the line is written synchronously with the exact bytes sceClibPrintf
 * would have produced. */
#define ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES ISAAC_VITA_LOG_ASYNC_LINE_MAX

typedef struct IsaacVitaLogAsyncStats {
    uint32_t started;         /* logger thread running */
    uint32_t start_result;    /* 0 or the failing SceKernel return code */
    uint32_t thread_priority; /* priority actually granted (191 or default) */
    uint32_t enqueued;        /* lines accepted into the ring */
    uint32_t sunk;            /* lines the logger/flush handed to a sink */
    uint32_t dropped;         /* lines refused (ring full / too long) */
    uint32_t used;            /* ring bytes in flight now */
    uint32_t high_water;      /* most ring bytes ever in flight */
} IsaacVitaLogAsyncStats;

/* Create the ring locks, the wake flag and the logger thread.  1 when the
 * logger runs (idempotent), 0 when a kernel call failed; the failure code is
 * in IsaacVitaLogAsyncStats.start_result and every producer keeps its
 * synchronous path. */
int isaac_vita_log_async_start(void);
int isaac_vita_log_async_started(void);

/* Copy one complete line (exact bytes, including its newline) into the ring.
 *  1  queued; the logger thread owns the sink call.
 * -1  dropped and counted (ring full, line longer than
 *     ISAAC_VITA_LOG_ASYNC_LINE_MAX, or an unknown sink).
 *  0  the logger is not running: the caller must write synchronously. */
int isaac_vita_log_async_enqueue(unsigned sink, const char *line,
                                 unsigned length);

/* sceClibPrintf replacement for one-call-one-record sites: format into a
 * stack buffer (sceClibVsnprintf, the same formatter as sceClibPrintf) and
 * queue it for the PRINTF sink; before start it calls sceClibPrintf itself.
 * Output longer than ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES - 1 bytes is never
 * truncated: the caller drains the ring (program order) and then writes it
 * with sceClibVprintf (exact bytes, no drop). */
#if defined(__GNUC__)
void isaac_vita_log_async_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
#else
void isaac_vita_log_async_printf(const char *format, ...);
#endif

/* Drain every queued line to its sink on the calling thread, in FIFO order,
 * then report a changed drop count.  Safe before start (no-op) and from the
 * process-exit and abort paths. */
void isaac_vita_log_async_flush(void);

void isaac_vita_log_async_get_stats(IsaacVitaLogAsyncStats *stats);

/* Provided by platform.c: the pre-existing synchronous durable sink
 * (log_write), called by the logger thread once per FILE line. */
void isaac_vita_log_sink_file(const char *line, unsigned int size);

#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
/* Private drain/platform seam. No record is held waiting for a future enqueue;
 * debug output still receives one call per original record. */
# define ISAAC_VITA_LOG_ASYNC_FILE_BATCH_RECORDS 8u
# define ISAAC_VITA_LOG_ASYNC_FILE_BATCH_BYTES 4096u
void isaac_vita_log_sink_file_batch(char *payload, const uint16_t *lengths,
                                   unsigned records);
#endif

/* One-call-one-record printf for the hot-path [kage-vita] owners.  With the
 * option OFF every owner defines the same name as sceClibPrintf itself, so the
 * OFF objects are byte-identical to the unrouted source. */
#if defined(ISAAC_VITA_LOG_ASYNC)
# define ISAAC_VITA_LOG_PRINTF isaac_vita_log_async_printf
#else
# define ISAAC_VITA_LOG_PRINTF sceClibPrintf
#endif

#if defined(ISAAC_VITA_LOG_ASYNC_ORACLE)
/* Host oracle seams: fake sinks, fake clock, fake wake and a fake logger
 * thread step.  The production build compiles none of these. */
typedef void (*isaac_vita_log_async_sink_fn)(const char *line,
                                             unsigned length);
extern isaac_vita_log_async_sink_fn g_isaac_vita_log_async_oracle_file_sink;
extern isaac_vita_log_async_sink_fn g_isaac_vita_log_async_oracle_printf_sink;
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
typedef void (*isaac_vita_log_async_batch_sink_fn)(char *payload,
                                                  const uint16_t *lengths,
                                                  unsigned records);
extern isaac_vita_log_async_batch_sink_fn
    g_isaac_vita_log_async_oracle_file_batch_sink;
extern int g_isaac_vita_log_async_oracle_sink_lock_fail;
#endif
extern uint64_t g_isaac_vita_log_async_oracle_now_us;
extern uint32_t g_isaac_vita_log_async_oracle_wakes;
extern uint32_t g_isaac_vita_log_async_oracle_lock_faults;
/* One logger-thread pass: drain to empty, then the rate-bounded drop
 * report.  Returns the number of lines sunk. */
uint32_t isaac_vita_log_async_oracle_service(void);
/* Ring cursors (read offset, write offset, bytes in flight) so the oracle
 * can park a line exactly across the ring end. */
void isaac_vita_log_async_oracle_state(uint32_t *head, uint32_t *tail,
                                       uint32_t *used);
#endif

#ifdef __cplusplus
}
#endif

#endif
