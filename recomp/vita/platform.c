#include "platform.h"
#include "vita_host_services.h"
#if defined(ISAAC_VITA_CONTINUE_PROFILE)
#include "kage_vita_continue_profile.h"
#endif

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(ISAAC_VITA_LOG_ASYNC)
#include "host_vita_log_async.h"
#endif

#ifndef ISAAC_VITA_HEAP_MB
#define ISAAC_VITA_HEAP_MB 64
#endif
#ifndef ISAAC_VITA_PLATFORM_HEAP_MB
#define ISAAC_VITA_PLATFORM_HEAP_MB ISAAC_VITA_HEAP_MB
#endif

#define ISAAC_LOG_DIR  ISAAC_VITA_NATIVE_DATA_ROOT
#define ISAAC_LOG_PATH ISAAC_LOG_DIR "/first-arm-fault.log"
#define ISAAC_LOG_PREV_PATH ISAAC_LOG_DIR "/first-arm-fault.prev.log"

/* Keep these in a direct executable object. Newlib references the heap symbol
 * weakly, so a definition hidden in a static archive is not extracted. */
unsigned int _newlib_heap_size_user =
    ISAAC_VITA_PLATFORM_HEAP_MB * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 4u * 1024u * 1024u;

static void log_write(const char *line, unsigned int size)
{
    SceUID fd;

    /* main() calls isaac_vita_log_reset() before the first record; that is
     * the single owner of directory creation.  Repeating mkdir for every
     * record caused synchronous filesystem work (and EEXIST diagnostics) in
     * hot asset bursts.  Keep per-record open/close so a crash still leaves
     * all completed records durable. */
#if defined(ISAAC_VITA_CONTINUE_PROFILE)
    isaac_vita_continue_profile_log_begin();
#endif
    fd = sceIoOpen(ISAAC_LOG_PATH,
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, size);
        sceIoClose(fd);
    }

    /* One call is one Vita3K log record. */
    sceClibPrintf("%s", line);
#if defined(ISAAC_VITA_CONTINUE_PROFILE)
    isaac_vita_continue_profile_log_end();
#endif
}

#if defined(ISAAC_VITA_LOG_ASYNC)
/* The logger thread's durable sink: the exact synchronous path above,
 * one call per queued line (host_vita_log_async.c). */
void isaac_vita_log_sink_file(const char *line, unsigned int size)
{
    log_write(line, size);
}
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
#define ISAAC_LOG_BATCH_OPEN() \
    sceIoOpen(ISAAC_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777)
#define ISAAC_LOG_BATCH_WRITE(fd, data, size) sceIoWrite(fd, data, size)
#define ISAAC_LOG_BATCH_CLOSE(fd) sceIoClose(fd)
#define ISAAC_LOG_BATCH_DEBUG(line, size) sceClibPrintf("%s", line)
#include "host_vita_log_file_batch_private.h"
#undef ISAAC_LOG_BATCH_OPEN
#undef ISAAC_LOG_BATCH_WRITE
#undef ISAAC_LOG_BATCH_CLOSE
#undef ISAAC_LOG_BATCH_DEBUG

void isaac_vita_log_sink_file_batch(char *payload, const uint16_t *lengths,
                                   unsigned records)
{
#if defined(ISAAC_VITA_CONTINUE_PROFILE)
    isaac_vita_continue_profile_log_begin();
#endif
    isaac_vita_log_file_batch_emit(payload, lengths, records);
#if defined(ISAAC_VITA_CONTINUE_PROFILE)
    isaac_vita_continue_profile_log_end();
#endif
}
#endif
#endif

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    SceDateTime current_utc;
    SceUInt64 value;
    int result;

    if (!filetime)
        return (int)SCE_RTC_ERROR_INVALID_POINTER;

    /* GetSystemTimeAsFileTime is UTC.  Let SceRtc own both calendar rules and
     * the 1601 epoch conversion instead of duplicating either in the port. */
    result = sceRtcGetCurrentClock(&current_utc, 0);
    if (result < 0)
        return result;
    result = sceRtcGetWin32FileTime(&current_utc, &value);
    if (result < 0)
        return result;

    *filetime = (uint64_t)value;
    return 0;
}

uint32_t isaac_vita_get_thread_id(void)
{
    return (uint32_t)sceKernelGetThreadId();
}

uint32_t isaac_vita_get_process_id(void)
{
    return (uint32_t)sceKernelGetProcessId();
}

uint64_t isaac_vita_get_process_time(void)
{
    /* QueryPerformanceCounter needs a monotonic, high-resolution counter.
     * Vita process time is monotonic and expressed in microseconds. */
    return (uint64_t)sceKernelGetProcessTimeWide();
}

/* Keep the previous launch's log as first-arm-fault.prev.log.  Truncating
 * on every start destroyed the only evidence of a crash as soon as the
 * player launched again.  The outcome is reported in the fresh log, right
 * after the logger receipt; a failed remove or rename never blocks logging,
 * the log is then truncated in place exactly as before.  The remove code is
 * always shown: 0x80010002 means there was no earlier .prev file. */
static void log_rotate(char *receipt, unsigned int size)
{
    SceIoStat stat;
    int remove_result;
    int rename_result;

    if (sceIoGetstat(ISAAC_LOG_PATH, &stat) < 0) {
        snprintf(receipt, size, "none");
        return;
    }
    remove_result = sceIoRemove(ISAAC_LOG_PREV_PATH);
    rename_result = sceIoRename(ISAAC_LOG_PATH, ISAAC_LOG_PREV_PATH);
    if (rename_result < 0) {
        snprintf(receipt, size, "rename_fail:0x%08x remove=0x%08x",
                 (unsigned)rename_result, (unsigned)remove_result);
        return;
    }
    snprintf(receipt, size, "ok remove=0x%08x", (unsigned)remove_result);
}

void isaac_vita_log_reset(void)
{
    SceUID fd;
    char rotate[64];

    sceIoMkdir(ISAAC_LOG_DIR, 0777);
    log_rotate(rotate, sizeof(rotate));
    fd = sceIoOpen(ISAAC_LOG_PATH,
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0)
        sceIoClose(fd);
#if defined(ISAAC_VITA_LOG_ASYNC)
    {
        /* The logger thread starts once the log file exists; every line
         * from here on is queued and this receipt is the first of them.
         * Before this point, and if the start fails, isaac_vita_log
         * writes synchronously exactly as before. */
        int started = isaac_vita_log_async_start();
        IsaacVitaLogAsyncStats stats;

        isaac_vita_log_async_get_stats(&stats);
        isaac_vita_log(
            "KAGE VITA LOG ASYNC: started=%d rc=0x%08x ring_kb=%u "
            "line_max=%u printf_bytes=%u prio=%u affinity=0x%08x",
            started, (unsigned)stats.start_result,
            (unsigned)ISAAC_VITA_LOG_ASYNC_RING_KB,
            (unsigned)ISAAC_VITA_LOG_ASYNC_LINE_MAX,
            (unsigned)ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES,
            (unsigned)stats.thread_priority, 0x00040000u);
#if defined(ISAAC_VITA_LOG_ASYNC_PRINTF_FILE) && ISAAC_VITA_LOG_ASYNC_PRINTF_FILE
        isaac_vita_log(
            "KAGE VITA LOG PRINTF FILE: sink=file+debug path=" ISAAC_LOG_PATH
            " bounded_bytes=%u overlong=debug+receipt started=%d",
            (unsigned)ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES - 1u, started);
#endif
    }
#endif
    /* rotate=ok|none|rename_fail:0x...; the second line of the log (the
     * first without ISAAC_VITA_LOG_ASYNC). */
    isaac_vita_log("KAGE VITA LOG ROTATE: rotate=%s prev=" ISAAC_LOG_PREV_PATH,
                   rotate);
}

void isaac_vita_log(const char *format, ...)
{
    char body[384];
    char line[512];
    va_list args;
    int body_length;
    int line_length;
    uint32_t timestamp_us;

    va_start(args, format);
    body_length = vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (body_length < 0)
        return;
    body[sizeof(body) - 1u] = '\0';

    /* One low-word sample keeps seconds/milliseconds coherent at rollover. */
    timestamp_us = sceKernelGetProcessTimeLow();
    line_length = snprintf(
        line, sizeof(line), "[%u.%03u thr 0x%x] %s\n",
        (unsigned)(timestamp_us / 1000000u),
        (unsigned)((timestamp_us / 1000u) % 1000u),
        (unsigned)sceKernelGetThreadId(), body);
    if (line_length <= 0)
        return;
    if (line_length >= (int)sizeof(line))
        line_length = (int)sizeof(line) - 1;
#if defined(ISAAC_VITA_LOG_ASYNC)
    /* 1 = queued, -1 = dropped and counted: the logger thread owns the
     * sinks either way.  0 = not started yet: the synchronous path. */
    if (isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE, line,
                                     (unsigned int)line_length) != 0)
        return;
#endif
    log_write(line, (unsigned int)line_length);
}
