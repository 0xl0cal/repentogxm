#include "kage_vita_io_profile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>

#include "host_vita_crt.h"
#include "vita_sync_services.h"
#if defined(ISAAC_VITA_IO_PROFILE_HEAP)
# include "host_vita_heap.h"
#endif

#ifndef ISAAC_VITA_IO_PROFILE_BUILD_ID
# define ISAAC_VITA_IO_PROFILE_BUILD_ID "profile:unstamped"
#endif

#if defined(ISAAC_VITA_IO_PROFILE_ORACLE)
typedef int SceUID;
typedef int64_t SceOff;
extern uint64_t sceKernelGetProcessTimeWide(void);
extern int sceKernelDelayThread(unsigned int delay_us);
extern int sceClibPrintf(const char *format, ...);
#else
# include <psp2/io/fcntl.h>
# include <psp2/kernel/clib.h>
# include <psp2/kernel/processmgr.h>
# include <psp2/kernel/threadmgr.h>
#endif

#if defined(ISAAC_VITA_IO_PROFILE_ORACLE)
# define profile_log sceClibPrintf
#else
void isaac_vita_log(const char *format, ...);
# define profile_log isaac_vita_log
#endif

extern int __real_sceIoRead(SceUID descriptor, void *buffer,
                            unsigned int size);
extern long __real_sceIoLseek32(SceUID descriptor, long offset, int origin);
extern SceOff __real_sceIoLseek(SceUID descriptor, SceOff offset, int origin);

enum {
    KAGE_IO_LATENCY_LT_16 = 0,
    KAGE_IO_LATENCY_LT_64,
    KAGE_IO_LATENCY_LT_256,
    KAGE_IO_LATENCY_LT_1024,
    KAGE_IO_LATENCY_LT_2048,
    KAGE_IO_LATENCY_LT_4096,
    KAGE_IO_LATENCY_LT_8192,
    KAGE_IO_LATENCY_LT_16384,
    KAGE_IO_LATENCY_GE_16384,
    KAGE_IO_LATENCY_BUCKETS
};

static uint32_t s_active;
static uint32_t s_read_calls;
static uint32_t s_read_requested_bytes;
static uint32_t s_read_bytes;
static uint32_t s_read_time_us;
static uint32_t s_read_errors;
static uint32_t s_read_max;
static uint32_t s_read_le_4k;
static uint32_t s_read_le_64k;
static uint32_t s_read_gt_64k;
static uint32_t s_read_latency[KAGE_IO_LATENCY_BUCKETS];
static uint32_t s_seek_calls;
static uint32_t s_seek32_calls;
static uint32_t s_seek64_calls;
static uint32_t s_seek_set_calls;
static uint32_t s_seek_cur_calls;
static uint32_t s_seek_end_calls;
static uint32_t s_seek_other_calls;
static uint32_t s_seek_time_us;
static uint32_t s_seek_errors;
static uint32_t s_seek_latency[KAGE_IO_LATENCY_BUCKETS];
static uint32_t s_texture_image_calls;
static uint32_t s_texture_sub_calls;
static uint32_t s_texture_pixels;
static uint32_t s_texture_time_us;
static uint32_t s_texture_max_pixels;
static uint32_t s_texture_inflight;
static uint32_t s_texture_inflight_kind;
static uint32_t s_texture_inflight_width;
static uint32_t s_texture_inflight_height;
static uint32_t s_texture_inflight_begin_us;
static uint32_t s_update_inflight;
static uint32_t s_progress_busy;
static uint32_t s_progress_last_log_us;
static uint64_t s_epoch_begin_us;

#define KAGE_VITA_IO_PROGRESS_INTERVAL_US 30000000u
#define KAGE_VITA_IO_PROGRESS_POWER_MIN   1024u

static uint32_t profile_load(const uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static void profile_store(uint32_t *value, uint32_t replacement)
{
    __atomic_store_n(value, replacement, __ATOMIC_RELAXED);
}

static void profile_add(uint32_t *value, uint32_t addend)
{
    uint32_t current = profile_load(value);

    for (;;) {
        uint32_t replacement = current > UINT32_MAX - addend
            ? UINT32_MAX : current + addend;
        if (__atomic_compare_exchange_n(value, &current, replacement, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return;
    }
}

static void profile_max(uint32_t *value, uint32_t candidate)
{
    uint32_t current = profile_load(value);
    while (candidate > current &&
           !__atomic_compare_exchange_n(value, &current, candidate, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static uint32_t profile_elapsed(uint64_t begin, uint64_t end)
{
    uint64_t elapsed;
    if (end < begin)
        return 0u;
    elapsed = end - begin;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static unsigned profile_latency_bucket(uint32_t elapsed)
{
    if (elapsed < 16u)
        return KAGE_IO_LATENCY_LT_16;
    if (elapsed < 64u)
        return KAGE_IO_LATENCY_LT_64;
    if (elapsed < 256u)
        return KAGE_IO_LATENCY_LT_256;
    if (elapsed < 1024u)
        return KAGE_IO_LATENCY_LT_1024;
    if (elapsed < 2048u)
        return KAGE_IO_LATENCY_LT_2048;
    if (elapsed < 4096u)
        return KAGE_IO_LATENCY_LT_4096;
    if (elapsed < 8192u)
        return KAGE_IO_LATENCY_LT_8192;
    if (elapsed < 16384u)
        return KAGE_IO_LATENCY_LT_16384;
    return KAGE_IO_LATENCY_GE_16384;
}

/* Register every counter writer before doing timed work.  REPORT first closes
 * the epoch and then waits for already-registered writers, so its aggregate is
 * exact without keeping a lock around native I/O. */
static int profile_update_enter(void)
{
    if (!__atomic_load_n(&s_active, __ATOMIC_ACQUIRE))
        return 0;
    (void)__atomic_add_fetch(&s_update_inflight, 1u, __ATOMIC_ACQ_REL);
    if (!__atomic_load_n(&s_active, __ATOMIC_ACQUIRE)) {
        (void)__atomic_sub_fetch(&s_update_inflight, 1u, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

static void profile_update_leave(void)
{
    (void)__atomic_sub_fetch(&s_update_inflight, 1u, __ATOMIC_RELEASE);
}

static uint32_t profile_bounded_sum(uint32_t left, uint32_t right)
{
    return left > UINT32_MAX - right ? UINT32_MAX : left + right;
}

void kage_vita_io_profile_begin(void)
{
    unsigned bucket;

    profile_store(&s_active, 0u);
    profile_store(&s_read_calls, 0u);
    profile_store(&s_read_requested_bytes, 0u);
    profile_store(&s_read_bytes, 0u);
    profile_store(&s_read_time_us, 0u);
    profile_store(&s_read_errors, 0u);
    profile_store(&s_read_max, 0u);
    profile_store(&s_read_le_4k, 0u);
    profile_store(&s_read_le_64k, 0u);
    profile_store(&s_read_gt_64k, 0u);
    for (bucket = 0u; bucket < KAGE_IO_LATENCY_BUCKETS; ++bucket)
        profile_store(&s_read_latency[bucket], 0u);
    profile_store(&s_seek_calls, 0u);
    profile_store(&s_seek32_calls, 0u);
    profile_store(&s_seek64_calls, 0u);
    profile_store(&s_seek_set_calls, 0u);
    profile_store(&s_seek_cur_calls, 0u);
    profile_store(&s_seek_end_calls, 0u);
    profile_store(&s_seek_other_calls, 0u);
    profile_store(&s_seek_time_us, 0u);
    profile_store(&s_seek_errors, 0u);
    for (bucket = 0u; bucket < KAGE_IO_LATENCY_BUCKETS; ++bucket)
        profile_store(&s_seek_latency[bucket], 0u);
    profile_store(&s_texture_image_calls, 0u);
    profile_store(&s_texture_sub_calls, 0u);
    profile_store(&s_texture_pixels, 0u);
    profile_store(&s_texture_time_us, 0u);
    profile_store(&s_texture_max_pixels, 0u);
    profile_store(&s_texture_inflight, 0u);
    profile_store(&s_texture_inflight_kind, 0u);
    profile_store(&s_texture_inflight_width, 0u);
    profile_store(&s_texture_inflight_height, 0u);
    profile_store(&s_texture_inflight_begin_us, 0u);
    profile_store(&s_update_inflight, 0u);
    profile_store(&s_progress_busy, 0u);
    isaac_vita_crt_io_profile_begin();
    s_epoch_begin_us = sceKernelGetProcessTimeWide();
    profile_store(&s_progress_last_log_us, (uint32_t)s_epoch_begin_us);
    __atomic_store_n(&s_active, 1u, __ATOMIC_RELEASE);
}

void kage_vita_io_profile_progress(uint32_t completed_calls)
{
    isaac_vita_crt_io_profile_snapshot crt;
    isaac_vita_sync_mutex_pool_snapshot sync;
    uint64_t now_us;
    uint32_t now32;
    uint32_t last32;
    uint32_t wall_ms;
    uint32_t heap_ok = 0u;
    uint32_t heap_live = 0u;
    uint32_t heap_bytes = 0u;
    uint32_t heap_peak = 0u;
    uint32_t heap_fail = 0u;
    uint32_t sync_fail;
    int is_power;
    int is_time;
    int saved_errno;
    const char *why;
#if defined(ISAAC_VITA_IO_PROFILE_HEAP)
    isaac_vita_guest_heap_telemetry_snapshot heap;
#endif

    if (!profile_update_enter())
        return;
    saved_errno = errno;
    now_us = sceKernelGetProcessTimeWide();
    now32 = (uint32_t)now_us;
    last32 = profile_load(&s_progress_last_log_us);
    is_power = completed_calls >= KAGE_VITA_IO_PROGRESS_POWER_MIN &&
        (completed_calls & (completed_calls - 1u)) == 0u;
    is_time = (uint32_t)(now32 - last32) >=
        KAGE_VITA_IO_PROGRESS_INTERVAL_US;
    if (completed_calls != 1u && !is_power && !is_time) {
        errno = saved_errno;
        profile_update_leave();
        return;
    }
    if (__atomic_exchange_n(&s_progress_busy, 1u, __ATOMIC_ACQUIRE)) {
        errno = saved_errno;
        profile_update_leave();
        return;
    }

    /* Another caller may have emitted while this one waited for the guard. */
    last32 = profile_load(&s_progress_last_log_us);
    is_time = (uint32_t)(now32 - last32) >=
        KAGE_VITA_IO_PROGRESS_INTERVAL_US;
    if (completed_calls != 1u && !is_power && !is_time)
        goto out;
    if (!isaac_vita_crt_io_profile_get_snapshot(&crt))
        goto out;

    why = completed_calls == 1u ? "first" : is_power ? "power" : "time";
    wall_ms = profile_elapsed(s_epoch_begin_us, now_us) / 1000u;
    isaac_vita_sync_get_mutex_pool_snapshot(&sync);
    sync_fail = profile_bounded_sum(
        profile_bounded_sum(sync.wait_failures, sync.owner_failures),
        sync.state_failures);
#if defined(ISAAC_VITA_IO_PROFILE_HEAP)
    heap_ok = isaac_vita_guest_heap_telemetry_snapshot_get(&heap) ? 1u : 0u;
    if (heap_ok) {
        heap_live = heap.owned_live_count;
        heap_bytes = heap.owned_requested_bytes;
        heap_peak = heap.peak_requested_bytes;
        heap_fail = profile_bounded_sum(
            heap.native_failures, heap.pool_failures);
    }
#endif
    profile_store(&s_progress_last_log_us, now32);
    profile_log(
        "[kage-vita-io] progress=preloop-v1 bid40=%.40s why=%s "
        "wall_ms=%u dispatch_fread=%u logical_calls/bytes/fail=%u/%u/%u "
        "native_read_calls/bytes/us/fail=%u/%u/%u/%u "
        "native_seek_calls/us/fail=%u/%u/%u",
        ISAAC_VITA_IO_PROFILE_BUILD_ID, why, wall_ms, completed_calls,
        crt.fread_calls, crt.fread_returned_bytes, crt.fread_failures,
        profile_load(&s_read_calls), profile_load(&s_read_bytes),
        profile_load(&s_read_time_us), profile_load(&s_read_errors),
        profile_load(&s_seek_calls), profile_load(&s_seek_time_us),
        profile_load(&s_seek_errors));
    profile_log(
        "[kage-vita-io] pstate=preloop-v1 bid40=%.40s fread=%u "
        "arc(seq/kind/flags/before/after/key)=%u/%u/%u/%d/%d/%.32s "
        "heap(ok/live/bytes/peak/fail)=%u/%u/%u/%u/%u "
        "sync(live/peak/waits/fail)=%u/%u/%u/%u",
        ISAAC_VITA_IO_PROFILE_BUILD_ID, completed_calls,
        crt.archive_sequence, crt.archive_kind, crt.archive_flags,
        crt.archive_position_before, crt.archive_position_after,
        crt.archive_key, heap_ok, heap_live, heap_bytes, heap_peak,
        heap_fail, sync.live, sync.peak, sync.waits, sync_fail);

out:
    errno = saved_errno;
    __atomic_store_n(&s_progress_busy, 0u, __ATOMIC_RELEASE);
    profile_update_leave();
}

void kage_vita_io_profile_texture_begin(
    uint32_t kind, int32_t width, int32_t height)
{
    uint64_t pixels;
    uint64_t now_us;
    uint32_t bounded;

    if (!profile_update_enter())
        return;
    now_us = sceKernelGetProcessTimeWide();
    if (kind)
        profile_add(&s_texture_sub_calls, 1u);
    else
        profile_add(&s_texture_image_calls, 1u);
    bounded = 0u;
    if (width > 0 && height > 0) {
        pixels = (uint64_t)(uint32_t)width * (uint64_t)(uint32_t)height;
        bounded = pixels > UINT32_MAX ? UINT32_MAX : (uint32_t)pixels;
        profile_add(&s_texture_pixels, bounded);
        profile_max(&s_texture_max_pixels, bounded);
    }
    profile_store(&s_texture_inflight_kind, kind ? 1u : 0u);
    profile_store(&s_texture_inflight_width,
                  width > 0 ? (uint32_t)width : 0u);
    profile_store(&s_texture_inflight_height,
                  height > 0 ? (uint32_t)height : 0u);
    profile_store(&s_texture_inflight_begin_us, (uint32_t)now_us);
    __atomic_store_n(&s_texture_inflight, 1u, __ATOMIC_RELEASE);
    profile_update_leave();
}

void kage_vita_io_profile_texture_end(void)
{
    uint64_t now_us;
    uint32_t begin;

    if (!profile_update_enter())
        return;
    if (!__atomic_load_n(&s_texture_inflight, __ATOMIC_ACQUIRE)) {
        profile_update_leave();
        return;
    }
    now_us = sceKernelGetProcessTimeWide();
    begin = profile_load(&s_texture_inflight_begin_us);
    /* The low word is sufficient for the bounded startup epoch (<71 min);
     * unsigned subtraction also handles one low-word wrap. */
    profile_add(&s_texture_time_us, (uint32_t)now_us - begin);
    __atomic_store_n(&s_texture_inflight, 0u, __ATOMIC_RELEASE);
    profile_update_leave();
}

int __wrap_sceIoRead(SceUID descriptor, void *buffer, unsigned int size)
{
    uint64_t begin;
    uint64_t end;
    uint32_t elapsed;
    int result;

    if (!profile_update_enter())
        return __real_sceIoRead(descriptor, buffer, size);
    begin = sceKernelGetProcessTimeWide();
    result = __real_sceIoRead(descriptor, buffer, size);
    end = sceKernelGetProcessTimeWide();
    elapsed = profile_elapsed(begin, end);
    profile_add(&s_read_calls, 1u);
    profile_add(&s_read_requested_bytes, size);
    profile_add(&s_read_time_us, elapsed);
    profile_add(&s_read_latency[profile_latency_bucket(elapsed)], 1u);
    if (result < 0) {
        profile_add(&s_read_errors, 1u);
    } else {
        uint32_t bytes = (uint32_t)result;
        profile_add(&s_read_bytes, bytes);
        profile_max(&s_read_max, bytes);
    }
    if (size <= 4096u)
        profile_add(&s_read_le_4k, 1u);
    else if (size <= 65536u)
        profile_add(&s_read_le_64k, 1u);
    else
        profile_add(&s_read_gt_64k, 1u);
    profile_update_leave();
    return result;
}

static void profile_record_seek(int origin, uint32_t elapsed,
                                int failed, int api32)
{
    profile_add(&s_seek_calls, 1u);
    profile_add(api32 ? &s_seek32_calls : &s_seek64_calls, 1u);
    /* SCE_SEEK_SET/CUR/END are the ABI values 0/1/2. */
    if (origin == 0)
        profile_add(&s_seek_set_calls, 1u);
    else if (origin == 1)
        profile_add(&s_seek_cur_calls, 1u);
    else if (origin == 2)
        profile_add(&s_seek_end_calls, 1u);
    else
        profile_add(&s_seek_other_calls, 1u);
    profile_add(&s_seek_time_us, elapsed);
    profile_add(&s_seek_latency[profile_latency_bucket(elapsed)], 1u);
    if (failed)
        profile_add(&s_seek_errors, 1u);
}

long __wrap_sceIoLseek32(SceUID descriptor, long offset, int origin)
{
    uint64_t begin;
    uint64_t end;
    long result;

    if (!profile_update_enter())
        return __real_sceIoLseek32(descriptor, offset, origin);
    begin = sceKernelGetProcessTimeWide();
    result = __real_sceIoLseek32(descriptor, offset, origin);
    end = sceKernelGetProcessTimeWide();
    profile_record_seek(
        origin, profile_elapsed(begin, end), result < 0, 1);
    profile_update_leave();
    return result;
}

SceOff __wrap_sceIoLseek(SceUID descriptor, SceOff offset, int origin)
{
    uint64_t begin;
    uint64_t end;
    SceOff result;

    if (!profile_update_enter())
        return __real_sceIoLseek(descriptor, offset, origin);
    begin = sceKernelGetProcessTimeWide();
    result = __real_sceIoLseek(descriptor, offset, origin);
    end = sceKernelGetProcessTimeWide();
    profile_record_seek(
        origin, profile_elapsed(begin, end), result < 0, 0);
    profile_update_leave();
    return result;
}

void kage_vita_io_profile_report(const char *reason)
{
    isaac_vita_crt_io_profile_snapshot crt;
    isaac_vita_sync_mutex_pool_snapshot sync;
    uint32_t inflight;
    uint32_t texture_inflight;
    uint32_t texture_inflight_us;
    uint32_t wall_us;
    uint64_t report_now;
    unsigned wait;
#if defined(ISAAC_VITA_IO_PROFILE_HEAP)
    isaac_vita_guest_heap_telemetry_snapshot heap;
#endif

    if (!__atomic_exchange_n(&s_active, 0u, __ATOMIC_ACQ_REL))
        return;

    (void)isaac_vita_crt_io_profile_stop_and_snapshot(&crt);
    for (wait = 0u; wait < 100u; ++wait) {
        if (!__atomic_load_n(&s_update_inflight, __ATOMIC_ACQUIRE))
            break;
        (void)sceKernelDelayThread(1000u);
    }
    inflight = __atomic_load_n(&s_update_inflight, __ATOMIC_ACQUIRE);
    report_now = sceKernelGetProcessTimeWide();
    wall_us = profile_elapsed(s_epoch_begin_us, report_now);
    texture_inflight = __atomic_load_n(
        &s_texture_inflight, __ATOMIC_ACQUIRE);
    texture_inflight_us = texture_inflight
        ? (uint32_t)report_now -
            profile_load(&s_texture_inflight_begin_us)
        : 0u;

    profile_log(
        "[kage-vita-io] profile=gxm-io-v1 build=%s reason=%s "
        "wall_us=%u writers=%u quiesced=%u; "
        "read calls=%u requested=%u bytes=%u us=%u errors=%u max=%u "
        "req_le4k=%u req_le64k=%u req_gt64k=%u",
        ISAAC_VITA_IO_PROFILE_BUILD_ID,
        reason ? reason : "unknown", wall_us,
        inflight, (unsigned)(inflight == 0u),
        profile_load(&s_read_calls),
        profile_load(&s_read_requested_bytes), profile_load(&s_read_bytes),
        profile_load(&s_read_time_us), profile_load(&s_read_errors),
        profile_load(&s_read_max), profile_load(&s_read_le_4k),
        profile_load(&s_read_le_64k), profile_load(&s_read_gt_64k));
    profile_log(
        "[kage-vita-io] read_latency_us lt16=%u lt64=%u lt256=%u "
        "lt1024=%u lt2048=%u lt4096=%u lt8192=%u lt16384=%u "
        "ge16384=%u",
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_16]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_64]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_256]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_1024]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_2048]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_4096]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_8192]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_LT_16384]),
        profile_load(&s_read_latency[KAGE_IO_LATENCY_GE_16384]));
    profile_log(
        "[kage-vita-io] seek calls=%u api32=%u api64=%u "
        "origin_set/cur/end/other=%u/%u/%u/%u us=%u errors=%u; "
        "latency_us lt16=%u lt64=%u lt256=%u lt1024=%u lt2048=%u "
        "lt4096=%u lt8192=%u lt16384=%u ge16384=%u",
        profile_load(&s_seek_calls), profile_load(&s_seek32_calls),
        profile_load(&s_seek64_calls),
        profile_load(&s_seek_set_calls), profile_load(&s_seek_cur_calls),
        profile_load(&s_seek_end_calls), profile_load(&s_seek_other_calls),
        profile_load(&s_seek_time_us), profile_load(&s_seek_errors),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_16]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_64]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_256]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_1024]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_2048]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_4096]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_8192]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_LT_16384]),
        profile_load(&s_seek_latency[KAGE_IO_LATENCY_GE_16384]));
    profile_log(
        "[kage-vita-io] logical fopen=%u fail=%u cache_hit=%u "
        "fclose=%u live=%u; fread=%u requested=%u returned=%u "
        "fail=%u; fseek=%u fail=%u",
        crt.fopen_calls, crt.fopen_failures, crt.fopen_cache_hits,
        crt.fclose_calls, crt.live_files, crt.fread_calls,
        crt.fread_requested_bytes, crt.fread_returned_bytes,
        crt.fread_failures, crt.fseek_calls, crt.fseek_failures);
    profile_log(
        "[kage-vita-io] shadow packed_rb open=%u close=%u fread=%u "
        "req=%u ret=%u coverage_ret=%u/%u; "
        "fseek_set/cur/end/other=%u/%u/%u/%u fflush/file/all=%u/%u",
        crt.shadow_cacheable_fopen_calls,
        crt.shadow_cacheable_fclose_calls, crt.shadow_fread_calls,
        crt.shadow_fread_requested_bytes,
        crt.shadow_fread_returned_bytes,
        crt.shadow_fread_returned_bytes, crt.fread_returned_bytes,
        crt.shadow_fseek_set_calls,
        crt.shadow_fseek_cur_calls, crt.shadow_fseek_end_calls,
        crt.shadow_fseek_other_calls, crt.shadow_fflush_calls,
        crt.shadow_fflush_all_calls);
    profile_log(
        "[kage-vita-io] shadow validity setvbuf_fail=%u invalid_hit=%u "
        "unmodelled_seek=%u fread_calls/req=%u/%u "
        "partial_calls/req/ret=%u/%u/%u fflush=%u",
        crt.shadow_setvbuf_failures, crt.shadow_invalid_cache_hits,
        crt.shadow_unmodelled_seeks,
        crt.shadow_unmodelled_fread_calls,
        crt.shadow_unmodelled_fread_bytes,
        crt.shadow_partial_fread_calls,
        crt.shadow_partial_fread_requested_bytes,
        crt.shadow_partial_fread_returned_bytes,
        crt.shadow_unmodelled_fflush_calls);
    profile_log(
        "[kage-vita-io] shadow counterfactual packed_rb-only "
        "refill_calls/req 8k=%u/%u 16k=%u/%u 32k=%u/%u 64k=%u/%u",
        crt.shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_8K_INDEX],
        crt.shadow_read_requested_bytes[ISAAC_VITA_CRT_IO_SHADOW_8K_INDEX],
        crt.shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_16K_INDEX],
        crt.shadow_read_requested_bytes[
            ISAAC_VITA_CRT_IO_SHADOW_16K_INDEX],
        crt.shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_32K_INDEX],
        crt.shadow_read_requested_bytes[
            ISAAC_VITA_CRT_IO_SHADOW_32K_INDEX],
        crt.shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX],
        crt.shadow_read_requested_bytes[
            ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX]);
    profile_log(
        "[kage-vita-io] texture image=%u sub=%u pixels=%u us=%u "
        "max_pixels=%u inflight=%u kind=%u width=%u height=%u "
        "inflight_us=%u",
        profile_load(&s_texture_image_calls),
        profile_load(&s_texture_sub_calls),
        profile_load(&s_texture_pixels),
        profile_load(&s_texture_time_us),
        profile_load(&s_texture_max_pixels), texture_inflight,
        profile_load(&s_texture_inflight_kind),
        profile_load(&s_texture_inflight_width),
        profile_load(&s_texture_inflight_height), texture_inflight_us);
    isaac_vita_sync_get_mutex_pool_snapshot(&sync);
    profile_log(
        "[kage-vita-io] sync enabled=%u creates=%u deletes=%u live=%u "
        "peak=%u waits=%u wait_fail=%u owner_fail=%u state_fail=%u",
        sync.enabled, sync.creates, sync.deletes, sync.live, sync.peak,
        sync.waits, sync.wait_failures, sync.owner_failures,
        sync.state_failures);
#if defined(ISAAC_VITA_IO_PROFILE_HEAP)
    if (isaac_vita_guest_heap_telemetry_snapshot_get(&heap)) {
        profile_log(
            "[kage-vita-io] heap live=%u bytes=%u peak=%u/%u "
            "ops=%u/%u/%u native_fail=%u pool_fail=%u terminal=%u "
            "accounting=%u saturated=%u",
            heap.owned_live_count, heap.owned_requested_bytes,
            heap.peak_live_count, heap.peak_requested_bytes,
            heap.pool_allocations, heap.pool_frees,
            heap.pool_reallocations, heap.native_failures,
            heap.pool_failures, heap.terminal, heap.accounting_valid,
            heap.counter_saturated);
    }
#endif
}
