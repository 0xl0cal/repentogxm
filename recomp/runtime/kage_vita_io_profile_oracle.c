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
