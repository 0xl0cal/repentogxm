#include "kage_vita_io_profile.h"
#include "kage_vita_deep_profile.h"

#if defined(ISAAC_VITA_IO_WINDOW_PROFILE)

#include <errno.h>
#include <string.h>
#if defined(ISAAC_VITA_IO_PROFILE_ORACLE)
typedef int SceUID;
typedef int SceMode;
typedef int64_t SceOff;
extern uint64_t sceKernelGetProcessTimeWide(void);
#else
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#endif

/* Signatures copied from vita-headers psp2/io/fcntl.h and common/types.h:
 * SceMode/SceUID/SceSSize are int, SceSize unsigned int, SceOff int64_t. */
extern SceUID __real_sceIoOpen(const char *, int, SceMode);
extern int __real_sceIoClose(SceUID);
extern int __real_sceIoRead(SceUID, void *, unsigned int);
extern int __real_sceIoPread(SceUID, void *, unsigned int, SceOff);
extern long __real_sceIoLseek32(SceUID, long, int);
extern SceOff __real_sceIoLseek(SceUID, SceOff, int);
extern int __real_sceIoWrite(SceUID, const void *, unsigned int);
extern int __real_sceIoPwrite(SceUID, const void *, unsigned int, SceOff);
extern int __real_sceIoSync(const char *, unsigned int);
extern int __real_sceIoSyncByFd(SceUID, int);

static uint32_t s_window_lock;
static uint32_t s_window_take_misses;
static uint32_t s_window_record_drops;
static uint32_t s_window_map_drops;
static uint32_t s_window_map_dirty;
static kage_vita_io_window_snapshot s_window;
static struct {
    int32_t descriptor;
    uint32_t file_class; /* zero is an unused/unknown slot */
} s_window_files[64];

static uint64_t window_now(void)
{
    int saved_errno = errno;
    uint64_t now = sceKernelGetProcessTimeWide();
    errno = saved_errno;
    return now;
}

static int window_try_lock(void)
{
    return !__atomic_exchange_n(&s_window_lock, 1U, __ATOMIC_ACQUIRE);
}

static void window_unlock(void)
{
    __atomic_store_n(&s_window_lock, 0U, __ATOMIC_RELEASE);
}

static void window_add32(uint32_t *value, uint32_t add)
{
    *value = add > UINT32_MAX - *value ? UINT32_MAX : *value + add;
}

/* Lock-free loss counters have no preempted owner to wait for. */
static void window_loss(uint32_t *value)
{
    uint32_t previous = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (previous != UINT32_MAX &&
           !__atomic_compare_exchange_n(value, &previous, previous + 1U,
               1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}

static void window_map_recover_locked(void)
{
    if (__atomic_exchange_n(&s_window_map_dirty, 0U, __ATOMIC_ACQ_REL))
        memset(s_window_files, 0, sizeof s_window_files);
}

static void window_add64(uint64_t *value, uint64_t add)
{
    *value = add > UINT64_MAX - *value ? UINT64_MAX : *value + add;
}

static uint32_t window_slot(SceUID descriptor)
{
    uint32_t value = (uint32_t)descriptor;
    return (value ^ (value >> 6) ^ (value >> 12)) & 63U;
}

static uint32_t window_class(SceUID descriptor, int closing)
{
    uint32_t slot = window_slot(descriptor);
    uint32_t result = KAGE_IO_UNKNOWN;
    if (!window_try_lock()) {
        window_loss(&s_window_map_drops);
        if (closing)
            __atomic_store_n(&s_window_map_dirty, 1U, __ATOMIC_RELEASE);
        return KAGE_IO_UNKNOWN;
    }
    window_map_recover_locked();
    if (s_window_files[slot].file_class &&
        s_window_files[slot].descriptor == descriptor) {
        result = s_window_files[slot].file_class;
        /* Remove before the native close, so a newly reused fd cannot be
         * erased afterwards.  A failed close loses attribution, not I/O. */
        if (closing)
            s_window_files[slot].file_class = KAGE_IO_UNKNOWN;
    }
    window_unlock();
    return result;
}

static uint32_t window_path_class(const char *path)
{
    char name[1024];
    const char *extension;
    unsigned length;
    /* Called only after successful native Open.  Early NUL, bounded copy;
     * unsuccessful/NULL/overlong names never acquire an invented class. */
    if (!path)
        return KAGE_IO_UNKNOWN;
    for (length = 0U; length < sizeof name; ++length) {
        unsigned char ch = (unsigned char)path[length];
        if (ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch + ('a' - 'A'));
        name[length] = (char)(ch == '\\' ? '/' : ch);
        if (!ch)
            break;
    }
    if (length == sizeof name)
        return KAGE_IO_UNKNOWN;
    extension = strrchr(name, '.');
    if (extension && !strcmp(extension, ".a"))
        return KAGE_IO_ARCHIVE;
    if (strstr(name, "/shader") ||
        (extension && !strcmp(extension, ".gxp")))
        return KAGE_IO_SHADER;
    if (strstr(name, "/documents/") ||
        (extension && !strcmp(extension, ".dat")))
        return KAGE_IO_SAVE;
    if (extension && (!strcmp(extension, ".ini") ||
                      !strcmp(extension, ".cfg")))
        return KAGE_IO_CONFIG;
    if ((extension && !strcmp(extension, ".log")) ||
        (length >= 7U && !strcmp(name + length - 7U, "log.txt") &&
         (length == 7U || name[length - 8U] == '/')))
        return KAGE_IO_LOG;
    if (strstr(name, "/resources/") || strstr(name, "/mods/"))
        return KAGE_IO_RESOURCE;
    return KAGE_IO_OTHER;
}

static void window_opened(SceUID descriptor, uint32_t file_class)
{
    uint32_t slot = window_slot(descriptor);
    if (!window_try_lock()) {
        window_loss(&s_window_map_drops);
        __atomic_store_n(&s_window_map_dirty, 1U, __ATOMIC_RELEASE);
        return;
    }
    window_map_recover_locked();
    if (s_window_files[slot].file_class &&
        s_window_files[slot].descriptor != descriptor)
        window_add32(&s_window.map_collisions, 1U);
    s_window_files[slot].descriptor = descriptor;
    s_window_files[slot].file_class = file_class;
    window_unlock();
}

static void window_record(uint32_t op, uint32_t file_class,
                          uint32_t requested, int64_t result,
                          uint64_t begin, uint64_t end)
{
#if defined(ISAAC_VITA_DEEP_PROFILE)
    /* Reuse the physical call's clocks. The recorder rejects foreign threads
     * so background music and log writes cannot become Update self time. */
    kage_vita_deep_io(op, file_class, requested, result, begin, end);
#endif
    uint64_t elapsed = end >= begin ? end - begin : 0U;
    kage_vita_io_window_operation *entry;
    if (!window_try_lock()) {
        window_loss(&s_window_record_drops);
        return;
    }
    entry = &s_window.op[op];
    window_add32(&entry->calls, 1U);
    if (result < 0)
        window_add32(&entry->errors, 1U);
    if (end < begin)
        window_add32(&s_window.clock_reversals, 1U);
    if (file_class == KAGE_IO_UNKNOWN)
        window_add32(&s_window.unknown_calls, 1U);
    window_add64(&entry->requested_bytes, requested);
    if ((op == KAGE_IO_READ || op == KAGE_IO_PREAD ||
         op == KAGE_IO_WRITE || op == KAGE_IO_PWRITE) && result > 0)
        window_add64(&entry->returned_bytes, (uint64_t)result);
    window_add64(&entry->time_us, elapsed);
    window_add64(&s_window.class_us[file_class], elapsed);
    if (elapsed > s_window.max_us) {
        s_window.max_us = elapsed;
        s_window.max_op = op;
        s_window.max_class = file_class;
        s_window.max_requested_bytes = requested;
        s_window.max_error = result < 0 ? (int32_t)result : 0;
    }
    window_unlock();
}

void kage_vita_io_window_take(kage_vita_io_window_snapshot *out)
{
    if (!out)
        return;
    if (!window_try_lock()) {
        window_loss(&s_window_take_misses);
        memset(out, 0, sizeof *out);
        out->abi_version = KAGE_VITA_IO_WINDOW_ABI;
        return;
    }
    *out = s_window;
    out->abi_version = KAGE_VITA_IO_WINDOW_ABI;
    out->snapshot_valid = 1U;
    out->take_misses = __atomic_exchange_n(
        &s_window_take_misses, 0U, __ATOMIC_ACQ_REL);
    out->record_drops = __atomic_exchange_n(
        &s_window_record_drops, 0U, __ATOMIC_ACQ_REL);
    out->map_drops = __atomic_exchange_n(
        &s_window_map_drops, 0U, __ATOMIC_ACQ_REL);
    memset(&s_window, 0, sizeof s_window);
    window_unlock();
}

SceUID __wrap_sceIoOpen(const char *path, int flags, SceMode mode)
{
    uint64_t begin = window_now();
    SceUID result = __real_sceIoOpen(path, flags, mode);
    int saved_errno = errno;
    uint64_t end = window_now();
    uint32_t file_class = result >= 0 ? window_path_class(path) : KAGE_IO_UNKNOWN;
    if (result >= 0)
        window_opened(result, file_class);
    window_record(KAGE_IO_OPEN, file_class, 0U, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoClose(SceUID descriptor)
{
    uint32_t file_class = window_class(descriptor, 1);
    uint64_t begin = window_now();
    int result = __real_sceIoClose(descriptor);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_CLOSE, file_class, 0U, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoRead(SceUID descriptor, void *buffer, unsigned int size)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    int result = __real_sceIoRead(descriptor, buffer, size);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_READ, file_class, size, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoPread(SceUID descriptor, void *buffer, unsigned int size,
                     SceOff offset)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    int result = __real_sceIoPread(descriptor, buffer, size, offset);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_PREAD, file_class, size, result, begin, end);
    errno = saved_errno;
    return result;
}

long __wrap_sceIoLseek32(SceUID descriptor, long offset, int origin)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    long result = __real_sceIoLseek32(descriptor, offset, origin);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_SEEK32, file_class, 0U, result, begin, end);
    errno = saved_errno;
    return result;
}

SceOff __wrap_sceIoLseek(SceUID descriptor, SceOff offset, int origin)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    SceOff result = __real_sceIoLseek(descriptor, offset, origin);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_SEEK64, file_class, 0U, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoWrite(SceUID descriptor, const void *buffer, unsigned int size)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    int result = __real_sceIoWrite(descriptor, buffer, size);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_WRITE, file_class, size, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoPwrite(SceUID descriptor, const void *buffer, unsigned int size,
                      SceOff offset)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    int result = __real_sceIoPwrite(descriptor, buffer, size, offset);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_PWRITE, file_class, size, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoSync(const char *device, unsigned int flags)
{
    uint64_t begin = window_now();
    int result = __real_sceIoSync(device, flags);
    int saved_errno = errno;
    uint64_t end = window_now();
    /* A mount-wide operation cannot truthfully inherit one file's class.
     * Do not dereference DEVICE: the native call owns its validation. */
    window_record(KAGE_IO_SYNC, KAGE_IO_UNKNOWN, 0U, result, begin, end);
    errno = saved_errno;
    return result;
}

int __wrap_sceIoSyncByFd(SceUID descriptor, int flags)
{
    uint32_t file_class = window_class(descriptor, 0);
    uint64_t begin = window_now();
    int result = __real_sceIoSyncByFd(descriptor, flags);
    int saved_errno = errno;
    uint64_t end = window_now();
    window_record(KAGE_IO_SYNC_BY_FD, file_class, 0U, result, begin, end);
    errno = saved_errno;
    return result;
}

#else /* existing startup-only profiler, unchanged */

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
#endif /* ISAAC_VITA_IO_WINDOW_PROFILE */
