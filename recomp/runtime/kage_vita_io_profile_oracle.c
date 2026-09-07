#if defined(ISAAC_VITA_IO_WINDOW_PROFILE)
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "kage_vita_io_profile.h"
/* Like the existing logical-shadow fixture, include the actual owner so
 * contention can be exercised without a production-only test hook/thread. */
#include "kage_vita_io_profile.c"

int __wrap_sceIoOpen(const char *, int, int);
int __wrap_sceIoClose(int);
int __wrap_sceIoRead(int, void *, unsigned int);
int __wrap_sceIoPread(int, void *, unsigned int, int64_t);
long __wrap_sceIoLseek32(int, long, int);
int64_t __wrap_sceIoLseek(int, int64_t, int);
int __wrap_sceIoWrite(int, const void *, unsigned int);
int __wrap_sceIoPwrite(int, const void *, unsigned int, int64_t);
int __wrap_sceIoSync(const char *, unsigned int);
int __wrap_sceIoSyncByFd(int, int);

static uint64_t s_now;
static uint64_t s_duration;
static unsigned s_clocks;
static int s_fd = 1;
static int s_result;
static int s_native_errno = 41;
static int s_entry_errno;
static int s_open_flags;
static int s_open_mode;
static const char *s_open_path;
static int64_t s_offset;
static int s_origin;
static int s_take_during_read;
static int s_reuse_during_close;
static int s_reverse;
static const void *s_write_buffer;
static unsigned int s_write_size;
static int s_descriptor;
static const char *s_sync_device;
static unsigned int s_sync_flags;
static int s_sync_fd_flags;
static kage_vita_io_window_snapshot s_mid_read;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita window I/O oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++s_clocks;
    errno = 999; /* even a clock which changes errno must not change I/O */
    return s_now;
}

static void native_end(void)
{
    s_entry_errno = errno;
    s_now = s_reverse ? s_now - s_duration : s_now + s_duration;
    errno = s_native_errno;
}

int __real_sceIoOpen(const char *path, int flags, int mode)
{
    s_open_path = path;
    s_open_flags = flags;
    s_open_mode = mode;
    native_end();
    return s_fd;
}

int __real_sceIoClose(int descriptor)
{
    (void)descriptor;
    if (s_reuse_during_close) {
        s_reuse_during_close = 0;
        (void)__wrap_sceIoOpen("ux0:/data/options.ini", 1, 0);
    }
    native_end();
    return s_result;
}

int __real_sceIoRead(int descriptor, void *buffer, unsigned int size)
{
    (void)descriptor;
    (void)size;
    if (s_take_during_read) {
        s_take_during_read = 0;
        kage_vita_io_window_take(&s_mid_read);
    }
    if (s_result > 0 && buffer)
        *(unsigned char *)buffer = 0x5a;
    native_end();
    return s_result;
}

int __real_sceIoPread(int descriptor, void *buffer, unsigned int size,
                     int64_t offset)
{
    s_offset = offset;
    return __real_sceIoRead(descriptor, buffer, size);
}

long __real_sceIoLseek32(int descriptor, long offset, int origin)
{
    (void)descriptor;
    s_offset = offset;
    s_origin = origin;
    native_end();
    return offset;
}

int64_t __real_sceIoLseek(int descriptor, int64_t offset, int origin)
{
    (void)descriptor;
    s_offset = offset;
    s_origin = origin;
    native_end();
    return offset;
}

int __real_sceIoWrite(int descriptor, const void *buffer, unsigned int size)
{
    s_descriptor = descriptor;
    s_write_buffer = buffer;
    s_write_size = size;
    native_end();
    return s_result;
}

int __real_sceIoPwrite(int descriptor, const void *buffer, unsigned int size,
                      int64_t offset)
{
    s_offset = offset;
    return __real_sceIoWrite(descriptor, buffer, size);
}

int __real_sceIoSync(const char *device, unsigned int flags)
{
    s_sync_device = device;
    s_sync_flags = flags;
    native_end();
    return s_result;
}

int __real_sceIoSyncByFd(int descriptor, int flags)
{
    s_descriptor = descriptor;
    s_sync_fd_flags = flags;
    native_end();
    return s_result;
}

int main(void)
{
    kage_vita_io_window_snapshot a;
    unsigned char buffer[16] = {0};
    char bounded[1024];
    const char *path = "ux0:/data/isaacr001/resources/packed/afterbirthp.a";
    unsigned clocks;
    unsigned i;
    static const struct {
        const char *path;
        uint32_t file_class;
    } classes[] = {
        {"UX0:\\data\\resources\\packed\\MUSIC.A", KAGE_IO_ARCHIVE},
        {"ux0:/data/resources/gfx/a.png", KAGE_IO_RESOURCE},
        {"ux0:/data/Documents/My Games/save.dat", KAGE_IO_SAVE},
        {"ux0:/data/config.ini", KAGE_IO_CONFIG},
        {"ux0:/data/shaders/a.gxp", KAGE_IO_SHADER},
        {"ux0:/data/first-arm-fault.log", KAGE_IO_LOG},
        {"ux0:/data/log.txt", KAGE_IO_LOG},
        {"ux0:/data/catalog.txt", KAGE_IO_OTHER}
    };
    kage_vita_io_window_take(NULL);
    kage_vita_io_window_take(&a);
    CHECK(s_clocks == 0 && a.abi_version == KAGE_VITA_IO_WINDOW_ABI);
    CHECK(a.snapshot_valid == 1 && a.take_misses == 0 && a.record_drops == 0);
    s_duration = 50;
    errno = 77;
    CHECK(__wrap_sceIoOpen(path, 0x123, 0x1ff) == 1);
    CHECK(s_open_path == path && s_open_flags == 0x123 && s_open_mode == 0x1ff);
    CHECK(s_entry_errno == 77 && errno == s_native_errno);
    s_result = 7;
    s_duration = 90;
    CHECK(__wrap_sceIoRead(1, buffer, 16) == 7 && buffer[0] == 0x5a);
    s_duration = 123456;
    CHECK(__wrap_sceIoPread(1, buffer, 14, INT64_C(0x123456789)) == 7);
    CHECK(s_offset == INT64_C(0x123456789));
    s_result = -5;
    s_duration = 10;
    CHECK(__wrap_sceIoRead(1, buffer, 13) == -5);
    CHECK(__wrap_sceIoLseek32(1, -9, 2) == -9 && s_origin == 2);
    CHECK(__wrap_sceIoLseek(1, INT64_C(0x123456789), 0) == INT64_C(0x123456789));
    CHECK(s_offset == INT64_C(0x123456789) && s_origin == 0);
    clocks = s_clocks;
    kage_vita_io_window_take(&a);
    CHECK(s_clocks == clocks && clocks == 12);
    CHECK(a.op[KAGE_IO_OPEN].calls == 1 && a.op[KAGE_IO_OPEN].time_us == 50);
    CHECK(a.op[KAGE_IO_READ].calls == 2 && a.op[KAGE_IO_READ].errors == 1);
    CHECK(a.op[KAGE_IO_READ].requested_bytes == 29 && a.op[KAGE_IO_READ].returned_bytes == 7);
    CHECK(a.op[KAGE_IO_PREAD].calls == 1 && a.op[KAGE_IO_PREAD].time_us == 123456);
    CHECK(a.op[KAGE_IO_SEEK32].errors == 1 && a.op[KAGE_IO_SEEK64].errors == 0);
    CHECK(a.max_op == KAGE_IO_PREAD && a.max_class == KAGE_IO_ARCHIVE);
    CHECK(a.max_us == 123456 && a.max_requested_bytes == 14 && a.max_error == 0);
    CHECK(a.class_us[KAGE_IO_ARCHIVE] == 123626 && a.unknown_calls == 0);
    kage_vita_io_window_take(&a);
    CHECK(a.max_us == 0 && a.op[KAGE_IO_READ].calls == 0);

    /* The map survives reset; completion after a take belongs to next window. */
    s_take_during_read = 1;
    s_result = 1;
    CHECK(__wrap_sceIoRead(1, buffer, 1) == 1);
    CHECK(s_mid_read.op[KAGE_IO_READ].calls == 0);
    kage_vita_io_window_take(&a);
    CHECK(a.op[KAGE_IO_READ].calls == 1 && a.max_class == KAGE_IO_ARCHIVE);

    /* 1 and 64 intentionally hash to the same slot; no false old-fd class. */
    s_fd = 64;
    CHECK(__wrap_sceIoOpen("ux0:/data/shaders/a.gxp", 1, 0) == 64);
    CHECK(__wrap_sceIoRead(1, buffer, 1) == 1);
    CHECK(__wrap_sceIoRead(64, buffer, 1) == 1);
    kage_vita_io_window_take(&a);
    CHECK(a.map_collisions == 1 && a.unknown_calls == 1);
    CHECK(a.class_us[KAGE_IO_SHADER] == 20 && a.class_us[KAGE_IO_UNKNOWN] == 10);
    s_result = -7;
    CHECK(__wrap_sceIoClose(64) == -7);
    CHECK(__wrap_sceIoRead(64, buffer, 1) == -7);
    kage_vita_io_window_take(&a);
    CHECK(a.op[KAGE_IO_CLOSE].errors == 1 && a.unknown_calls == 1);

    /* Reopen before old Close returns: removal must not erase the new fd. */
    CHECK(__wrap_sceIoOpen(path, 1, 0) == 64);
    s_reuse_during_close = 1;
    s_result = 0;
    CHECK(__wrap_sceIoClose(64) == 0);
    kage_vita_io_window_take(&a);
    s_result = 1;
    CHECK(__wrap_sceIoRead(64, buffer, 1) == 1);
    kage_vita_io_window_take(&a);
    CHECK(a.max_class == KAGE_IO_CONFIG && a.unknown_calls == 0);

    s_fd = -3;
    CHECK(__wrap_sceIoOpen(NULL, 0, 0) == -3);
    kage_vita_io_window_take(&a);
    CHECK(a.op[KAGE_IO_OPEN].errors == 1 && a.max_class == KAGE_IO_UNKNOWN);
    memset(bounded, 'x', sizeof bounded);
    s_fd = 2;
    CHECK(__wrap_sceIoOpen(bounded, 1, 0) == 2);
    kage_vita_io_window_take(&a);
    CHECK(a.max_class == KAGE_IO_UNKNOWN);
    s_now = 1000;
    s_reverse = 1;
    CHECK(__wrap_sceIoRead(2, buffer, 1) == 1);
    kage_vita_io_window_take(&a);
    CHECK(a.clock_reversals == 1 && a.op[KAGE_IO_READ].time_us == 0);
    s_reverse = 0;
    s_duration = UINT64_MAX;
    for (i = 0; i < 2; ++i) {
        s_now = 0;
        CHECK(__wrap_sceIoRead(2, buffer, UINT32_MAX) == 1);
    }
    kage_vita_io_window_take(&a);
    CHECK(a.op[KAGE_IO_READ].time_us == UINT64_MAX);
    CHECK(a.op[KAGE_IO_READ].requested_bytes == UINT64_C(8589934590));
    CHECK(a.max_us == UINT64_MAX && a.class_us[KAGE_IO_UNKNOWN] == UINT64_MAX);
    s_duration = 5;
    for (i = 0; i < sizeof classes / sizeof classes[0]; ++i) {
        s_now = 0;
        CHECK(__wrap_sceIoOpen(classes[i].path, 1, 0) == 2);
        kage_vita_io_window_take(&a);
        CHECK(a.max_class == classes[i].file_class);
    }
    /* A preempted owner cannot strand the native caller or snapshot. */
    s_fd = 1;
    CHECK(__wrap_sceIoOpen(path, 1, 0) == 1);
    kage_vita_io_window_take(&a);
    __atomic_store_n(&s_window_lock, 1U, __ATOMIC_RELEASE);
    CHECK(__wrap_sceIoClose(1) == 1); /* map loss still invalidates the old fd */
    CHECK(__wrap_sceIoOpen("ux0:/options.ini", 1, 0) == 1);
    kage_vita_io_window_take(&a);
    CHECK(a.snapshot_valid == 0 && a.op[KAGE_IO_READ].calls == 0);
    __atomic_store_n(&s_window_lock, 0U, __ATOMIC_RELEASE);
    CHECK(__wrap_sceIoRead(1, buffer, 1) == 1);
    kage_vita_io_window_take(&a);
    CHECK(a.snapshot_valid == 1 && a.take_misses == 1);
    CHECK(a.record_drops == 2 && a.map_drops == 2);
    CHECK(a.unknown_calls == 1 && a.max_class == KAGE_IO_UNKNOWN);
    CHECK(a.op[KAGE_IO_READ].calls == 1 && a.op[KAGE_IO_OPEN].calls == 0);
    CHECK(__wrap_sceIoOpen("ux0:/options.ini", 1, 0) == 1);
    kage_vita_io_window_take(&a);
    CHECK(__wrap_sceIoRead(1, buffer, 1) == 1);
    kage_vita_io_window_take(&a);
    CHECK(a.max_class == KAGE_IO_CONFIG && a.take_misses == 0);
    CHECK(a.record_drops == 0 && a.map_drops == 0 && a.unknown_calls == 0);
    /* Appended buckets preserve the original six ABI indices. */
    CHECK(KAGE_IO_OPEN == 0 && KAGE_IO_SEEK64 == 5 && KAGE_IO_WRITE == 6);
    CHECK(KAGE_IO_PWRITE == 7 && KAGE_IO_SYNC == 8 && KAGE_IO_SYNC_BY_FD == 9);
    CHECK(__wrap_sceIoOpen("ux0:/data/log.txt", 1, 0) == 1);
    kage_vita_io_window_take(&a);
    s_result = 3;
    s_duration = 123;
    errno = 78;
    clocks = s_clocks;
    CHECK(__wrap_sceIoWrite(1, buffer, sizeof buffer) == 3);
    CHECK(s_descriptor == 1 && s_write_buffer == buffer && s_write_size == sizeof buffer);
    CHECK(buffer[0] == 0x5a && s_entry_errno == 78 && errno == s_native_errno);
    s_result = -12;
    CHECK(__wrap_sceIoWrite(1, buffer, 9) == -12);
    s_result = 2;
    s_duration = 234;
    CHECK(__wrap_sceIoPwrite(1, buffer, 7, INT64_C(0x876543210)) == 2);
    CHECK(s_offset == INT64_C(0x876543210) && s_write_size == 7);
    s_result = -13;
    CHECK(__wrap_sceIoPwrite(1, buffer, 4, INT64_C(-27)) == -13);
    CHECK(s_offset == -27 && s_write_buffer == buffer);
    s_duration = 345;
    s_result = 0;
    CHECK(__wrap_sceIoSyncByFd(1, -7) == 0);
    CHECK(s_descriptor == 1 && s_sync_fd_flags == -7);
    s_result = -14;
    CHECK(__wrap_sceIoSyncByFd(1, 8) == -14);
    s_duration = 456;
    s_result = 0;
    path = "ux0:";
    CHECK(__wrap_sceIoSync(path, UINT32_MAX) == 0);
    CHECK(s_sync_device == path && s_sync_flags == UINT32_MAX);
    s_result = -15;
    CHECK(__wrap_sceIoSync(NULL, 5) == -15);
    CHECK(s_sync_device == NULL && s_sync_flags == 5);
    kage_vita_io_window_take(&a);
    CHECK(s_clocks == clocks + 16);
    CHECK(a.op[KAGE_IO_WRITE].calls == 2 && a.op[KAGE_IO_WRITE].errors == 1);
    CHECK(a.op[KAGE_IO_WRITE].requested_bytes == 25 && a.op[KAGE_IO_WRITE].returned_bytes == 3);
    CHECK(a.op[KAGE_IO_PWRITE].calls == 2 && a.op[KAGE_IO_PWRITE].errors == 1);
    CHECK(a.op[KAGE_IO_PWRITE].requested_bytes == 11 && a.op[KAGE_IO_PWRITE].returned_bytes == 2);
    CHECK(a.op[KAGE_IO_SYNC].calls == 2 && a.op[KAGE_IO_SYNC].errors == 1);
    CHECK(a.op[KAGE_IO_SYNC_BY_FD].calls == 2 && a.op[KAGE_IO_SYNC_BY_FD].errors == 1);
    CHECK(a.op[KAGE_IO_SYNC].returned_bytes == 0 && a.op[KAGE_IO_SYNC_BY_FD].requested_bytes == 0);
    CHECK(a.class_us[KAGE_IO_LOG] == 1404 && a.class_us[KAGE_IO_UNKNOWN] == 912);
    CHECK(a.max_us == 456 && a.max_op == KAGE_IO_SYNC && a.max_class == KAGE_IO_UNKNOWN);
    __atomic_store_n(&s_window_lock, 1U, __ATOMIC_RELEASE);
    CHECK(__wrap_sceIoWrite(1, buffer, 1) == -15);
    CHECK(__wrap_sceIoSync(NULL, 0) == -15);
    __atomic_store_n(&s_window_lock, 0U, __ATOMIC_RELEASE);
    kage_vita_io_window_take(&a);
    CHECK(a.record_drops == 2 && a.map_drops == 1);
    puts("Vita window native-I/O profile oracle: PASS (native calls only, no per-call logs)");
    return 0;
}
#else
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_io_profile.h"
#include "host_vita_crt.h"
#include "vita_sync_services.h"

typedef int SceUID;
typedef int64_t SceOff;

int __wrap_sceIoRead(SceUID descriptor, void *buffer, unsigned int size);
long __wrap_sceIoLseek32(SceUID descriptor, long offset, int origin);
SceOff __wrap_sceIoLseek(SceUID descriptor, SceOff offset, int origin);

static uint64_t s_now;
static uint32_t s_read_duration;
static uint32_t s_seek_duration;
static uint32_t s_clock_calls;
static unsigned s_log_count;
static char s_logs[48][640];
static int s_log_lengths[48];
static int s_max_snapshot;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita I/O profile oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

uint64_t sceKernelGetProcessTimeWide(void)
{
    ++s_clock_calls;
    return s_now;
}

int sceKernelDelayThread(unsigned int delay_us)
{
    s_now += delay_us;
    return 0;
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    int result;
    if (s_log_count >= sizeof s_logs / sizeof s_logs[0])
        return -1;
    va_start(arguments, format);
    result = vsnprintf(s_logs[s_log_count], sizeof s_logs[0], format,
                       arguments);
    va_end(arguments);
    s_log_lengths[s_log_count] = result;
    ++s_log_count;
    return result;
}

void isaac_vita_crt_io_profile_begin(void)
{
}

static void oracle_fill_crt_snapshot(
    isaac_vita_crt_io_profile_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    if (s_max_snapshot) {
        snapshot->fread_calls = UINT32_MAX;
        snapshot->fread_returned_bytes = UINT32_MAX;
        snapshot->fread_failures = UINT32_MAX;
        snapshot->archive_sequence = UINT32_MAX;
        snapshot->archive_kind = UINT32_MAX;
        snapshot->archive_flags = UINT32_MAX;
        snapshot->archive_position_before = INT32_MIN;
        snapshot->archive_position_after = INT32_MIN;
        memset(snapshot->archive_key, 'z',
               sizeof snapshot->archive_key - 1u);
        snapshot->archive_key[sizeof snapshot->archive_key - 1u] = '\0';
        return;
    }
    snapshot->fopen_calls = 9u;
    snapshot->fopen_cache_hits = 7u;
    snapshot->fclose_calls = 8u;
    snapshot->fread_calls = 4u;
    snapshot->fread_requested_bytes = 200717u;
    snapshot->fread_returned_bytes = 200704u;
    snapshot->fread_failures = 1u;
    snapshot->fseek_calls = 2u;
    snapshot->fseek_failures = 1u;
    snapshot->live_files = 1u;
    snapshot->shadow_cacheable_fopen_calls = 8u;
    snapshot->shadow_cacheable_fclose_calls = 7u;
    snapshot->shadow_setvbuf_failures = 1u;
    snapshot->shadow_invalid_cache_hits = 2u;
    snapshot->shadow_fread_calls = 4u;
    snapshot->shadow_fread_requested_bytes = 200717u;
    snapshot->shadow_fread_returned_bytes = 200704u;
    snapshot->shadow_fseek_set_calls = 2u;
    snapshot->shadow_fseek_cur_calls = 3u;
    snapshot->shadow_fseek_end_calls = 4u;
    snapshot->shadow_fseek_other_calls = 5u;
    snapshot->shadow_fflush_calls = 1u;
    snapshot->shadow_fflush_all_calls = 2u;
    snapshot->shadow_unmodelled_seeks = 6u;
    snapshot->shadow_unmodelled_fread_calls = 7u;
    snapshot->shadow_unmodelled_fread_bytes = 8u;
    snapshot->shadow_partial_fread_calls = 3u;
    snapshot->shadow_partial_fread_requested_bytes = 4u;
    snapshot->shadow_partial_fread_returned_bytes = 5u;
    snapshot->shadow_unmodelled_fflush_calls = 9u;
    snapshot->shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_8K_INDEX] = 11u;
    snapshot->shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_16K_INDEX] = 12u;
    snapshot->shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_32K_INDEX] = 13u;
    snapshot->shadow_read_calls[ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX] = 14u;
    snapshot->shadow_read_requested_bytes[
        ISAAC_VITA_CRT_IO_SHADOW_8K_INDEX] = 90112u;
    snapshot->shadow_read_requested_bytes[
        ISAAC_VITA_CRT_IO_SHADOW_16K_INDEX] = 196608u;
    snapshot->shadow_read_requested_bytes[
        ISAAC_VITA_CRT_IO_SHADOW_32K_INDEX] = 425984u;
    snapshot->shadow_read_requested_bytes[
        ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX] = 917504u;
    snapshot->archive_sequence = 77u;
    snapshot->archive_kind = 3u;
    snapshot->archive_flags = 9u;
    snapshot->archive_position_before = 1234;
    snapshot->archive_position_after = 5678;
    memset(snapshot->archive_key, 'k', sizeof snapshot->archive_key - 1u);
    snapshot->archive_key[sizeof snapshot->archive_key - 1u] = '\0';
}

int isaac_vita_crt_io_profile_get_snapshot(
    isaac_vita_crt_io_profile_snapshot *snapshot)
{
    oracle_fill_crt_snapshot(snapshot);
    return 1;
}

int isaac_vita_crt_io_profile_stop_and_snapshot(
    isaac_vita_crt_io_profile_snapshot *snapshot)
{
    oracle_fill_crt_snapshot(snapshot);
    return 1;
}

void isaac_vita_sync_get_mutex_pool_snapshot(
    isaac_vita_sync_mutex_pool_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof *snapshot);
    if (s_max_snapshot) {
        snapshot->live = UINT32_MAX;
        snapshot->peak = UINT32_MAX;
        snapshot->waits = UINT32_MAX;
        snapshot->wait_failures = UINT32_MAX;
        snapshot->owner_failures = UINT32_MAX;
        snapshot->state_failures = UINT32_MAX;
        return;
    }
    snapshot->enabled = 1u;
    snapshot->creates = 8193u;
    snapshot->deletes = 1u;
    snapshot->live = 8192u;
    snapshot->peak = 8192u;
    snapshot->waits = 3u;
}

int __real_sceIoRead(SceUID descriptor, void *buffer, unsigned int size)
{
    (void)descriptor;
    (void)buffer;
    s_now += s_read_duration;
    return size == 13u ? -5 : (int)size;
}

SceOff __real_sceIoLseek(SceUID descriptor, SceOff offset, int origin)
{
    (void)descriptor;
    (void)origin;
    s_now += s_seek_duration;
    return offset < 0 ? (SceOff)-9 : offset;
}

long __real_sceIoLseek32(SceUID descriptor, long offset, int origin)
{
    (void)descriptor;
    (void)origin;
    s_now += s_seek_duration;
    return offset < 0 ? -9L : offset;
}

int main(void)
{
    char buffer[16];
    uint32_t clocks;

    s_read_duration = 500u;
    CHECK(__wrap_sceIoRead(1, buffer, 7u) == 7);
    kage_vita_io_profile_report("inactive");
    CHECK(s_log_count == 0u);

    kage_vita_io_profile_begin();
    s_read_duration = 10u;
    CHECK(__wrap_sceIoRead(1, buffer, 4096u) == 4096);
    s_read_duration = 80u;
    CHECK(__wrap_sceIoRead(1, buffer, 65536u) == 65536);
    s_read_duration = 1300u;
    CHECK(__wrap_sceIoRead(1, buffer, 131072u) == 131072);
    s_read_duration = 10u;
    CHECK(__wrap_sceIoRead(1, buffer, 13u) == -5);
    s_read_duration = 2500u;
    CHECK(__wrap_sceIoRead(1, buffer, 8u) == 8);
    s_read_duration = 5000u;
    CHECK(__wrap_sceIoRead(1, buffer, 8u) == 8);
    s_read_duration = 10000u;
    CHECK(__wrap_sceIoRead(1, buffer, 8u) == 8);
    s_read_duration = 20000u;
    CHECK(__wrap_sceIoRead(1, buffer, 8u) == 8);
    s_seek_duration = 50u;
    CHECK(__wrap_sceIoLseek(1, 1234, 0) == 1234);
    CHECK(__wrap_sceIoLseek(1, -1, 0) == -9);
    s_seek_duration = 10u;
    CHECK(__wrap_sceIoLseek32(1, 10L, 0) == 10L);
    s_seek_duration = 80u;
    CHECK(__wrap_sceIoLseek32(1, 20L, 1) == 20L);
    s_seek_duration = 1300u;
    CHECK(__wrap_sceIoLseek32(1, 30L, 2) == 30L);
    s_seek_duration = 20000u;
    CHECK(__wrap_sceIoLseek32(1, -1L, 7) == -9L);
    s_now = 2000u;
    kage_vita_io_profile_texture_begin(0u, 512, 4096);
    s_now = 2600u;
    kage_vita_io_profile_texture_end();
    s_now = 3000u;
    kage_vita_io_profile_texture_begin(1u, 16, 16);
    s_now = 3020u;
    kage_vita_io_profile_texture_end();

    kage_vita_io_profile_report("loading-complete");
    CHECK(s_log_count == 9u);
    CHECK(strstr(s_logs[0],
                 "profile=gxm-io-v1 build=") != NULL);
    CHECK(strstr(s_logs[0], "reason=loading-complete") != NULL);
    CHECK(strstr(s_logs[0], "wall_us=2520") != NULL);
    CHECK(strstr(s_logs[0], "writers=0 quiesced=1") != NULL);
    CHECK(strstr(s_logs[0],
                 "calls=8 requested=200749 bytes=200736 us=38900 errors=1") !=
          NULL);
    CHECK(strstr(s_logs[0], "max=131072") != NULL);
    CHECK(strstr(s_logs[0], "req_le4k=6 req_le64k=1 req_gt64k=1") != NULL);
    CHECK(strstr(s_logs[1],
                 "lt16=2 lt64=0 lt256=1 lt1024=0 lt2048=1 lt4096=1 "
                 "lt8192=1 lt16384=1 ge16384=1") != NULL);
    CHECK(strstr(s_logs[2],
                 "seek calls=6 api32=4 api64=2 "
                 "origin_set/cur/end/other=3/1/1/1 us=21490 errors=2") !=
          NULL);
    CHECK(strstr(s_logs[2],
                 "lt16=1 lt64=2 lt256=1 lt1024=0 lt2048=1 lt4096=0 "
                 "lt8192=0 lt16384=0 ge16384=1") != NULL);
    CHECK(strstr(s_logs[3], "fopen=9 fail=0 cache_hit=7") != NULL);
    CHECK(strstr(s_logs[3], "fread=4 requested=200717 returned=200704") != NULL);
    CHECK(strstr(s_logs[4],
                 "shadow packed_rb open=8 close=7 fread=4 req=200717 "
                 "ret=200704 coverage_ret=200704/200704") != NULL);
    CHECK(strstr(s_logs[4],
                 "fseek_set/cur/end/other=2/3/4/5 fflush/file/all=1/2") !=
          NULL);
    CHECK(strstr(s_logs[5],
                 "setvbuf_fail=1 invalid_hit=2 unmodelled_seek=6 "
                 "fread_calls/req=7/8 partial_calls/req/ret=3/4/5 "
                 "fflush=9") != NULL);
    CHECK(strstr(s_logs[6],
                 "8k=11/90112 16k=12/196608 32k=13/425984 "
                 "64k=14/917504") != NULL);
    CHECK(strstr(s_logs[7], "texture image=1 sub=1 pixels=2097408") != NULL);
    CHECK(strstr(s_logs[7], "us=620 max_pixels=2097152 inflight=0") != NULL);
    CHECK(strstr(s_logs[8], "creates=8193 deletes=1 live=8192") != NULL);
    kage_vita_io_profile_report("duplicate");
    CHECK(s_log_count == 9u);

    /* A report raised synchronously from inside glTexImage2D must retain the
     * unfinished upload's dimensions and elapsed time. */
    kage_vita_io_profile_begin();
    s_now = 5000u;
    kage_vita_io_profile_texture_begin(1u, 128, 64);
    s_now = 5250u;
    kage_vita_io_profile_report("gxm-reserve");
    CHECK(s_log_count == 18u);
    CHECK(strstr(s_logs[9], "reason=gxm-reserve") != NULL);
    CHECK(strstr(s_logs[16], "image=0 sub=1 pixels=8192 us=0") != NULL);
    CHECK(strstr(s_logs[16],
                  "inflight=1 kind=1 width=128 height=64 inflight_us=250") != NULL);
    kage_vita_io_profile_report("duplicate");
    CHECK(s_log_count == 18u);

    s_read_duration = 300u;
    CHECK(__wrap_sceIoRead(1, buffer, 9u) == 9);
    clocks = s_clock_calls;
    kage_vita_io_profile_texture_begin(0u, 1, 1);
    kage_vita_io_profile_texture_end();
    CHECK(s_clock_calls == clocks);
    CHECK(s_log_count == 18u);

    /* Live progress snapshots must not terminate the epoch.  512 is merely
     * a cheap poll, 1024 is a durable power-of-two boundary, and thereafter
     * a non-power checkpoint writes only when thirty seconds elapsed. */
    kage_vita_io_profile_begin();
    kage_vita_io_profile_progress(1u);
    CHECK(s_log_count == 20u);
    CHECK(strstr(s_logs[18],
                 "progress=preloop-v1 bid40=") !=
          NULL);
    CHECK(strstr(s_logs[18], "why=first") != NULL);
    CHECK(strstr(s_logs[18],
                 "dispatch_fread=1 logical_calls/bytes/fail=4/200704/1") !=
          NULL);
    CHECK(strstr(s_logs[19],
                 "arc(seq/kind/flags/before/after/key)=77/3/9/1234/5678/") !=
          NULL);
    kage_vita_io_profile_progress(512u);
    CHECK(s_log_count == 20u);
    s_now += 1000000u;
    kage_vita_io_profile_progress(1024u);
    CHECK(s_log_count == 22u);
    CHECK(strstr(s_logs[20], "why=power") != NULL);
    s_now += 29999000u;
    kage_vita_io_profile_progress(1536u);
    CHECK(s_log_count == 22u);
    s_now += 1000u;
    kage_vita_io_profile_progress(1536u);
    CHECK(s_log_count == 24u);
    CHECK(strstr(s_logs[22], "why=time") != NULL);
    for (clocks = 18u; clocks < 24u; ++clocks) {
        CHECK(s_log_lengths[clocks] > 0);
        CHECK(s_log_lengths[clocks] < 384);
    }
    kage_vita_io_profile_report("progress-terminal");
    CHECK(s_log_count == 33u);
    kage_vita_io_profile_progress(2048u);
    CHECK(s_log_count == 33u);

    /* Pin the durable 383-byte body contract with the longest supported
     * build identity, signed positions and every displayed counter at its
     * widest decimal representation. */
    s_max_snapshot = 1;
    kage_vita_io_profile_begin();
    s_now += 30000000u;
    kage_vita_io_profile_progress(UINT32_MAX);
    CHECK(s_log_count == 35u);
    CHECK(strstr(s_logs[33], "bid40=") != NULL);
    CHECK(strstr(s_logs[34],
                 "arc(seq/kind/flags/before/after/key)="
                 "4294967295/4294967295/4294967295/"
                 "-2147483648/-2147483648/") != NULL);
    CHECK(s_log_lengths[33] > 0 && s_log_lengths[33] < 384);
    CHECK(s_log_lengths[34] > 0 && s_log_lengths[34] < 384);
    puts("Vita startup native-I/O profile oracle: PASS");
    return 0;
}
#endif
