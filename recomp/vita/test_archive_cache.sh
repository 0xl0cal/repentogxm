#!/usr/bin/env bash
set -eu

root=${ISAAC_ARCHIVE_CACHE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_ARCHIVE_CACHE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-archive-cache.XXXXXX")}
run="$work/run"
host_cc=${CC:-cc}
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_VITA_ARCHIVE_CACHE_ORACLE=1"

mkdir -p "$run/ux0:/data/isaacr001/resources/packed" \
         "$run/ux0:/data/isaacr001/resources/loose"

# Twice: without ISAAC_VITA_CRT_DESCRIPTOR_RECOVER (identity) and with it
# (the newlib fd release repair after a refused close, against a transcribed
# copy of newlib's descriptor table).
for recover in off on; do
recover_flag=
if [ "$recover" = on ]; then
    recover_flag=-DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1
fi
"$host_cc" $flags $recover_flag -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_archive_cache.c" -x c - \
    -o "$work/archive-cache-host-oracle-$recover" <<'EOF'
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_archive_cache.h"

#define ROOT "ux0:/data/isaacr001"
#define ARCHIVE_A ROOT "/resources/packed/graphics.a"
#define ARCHIVE_B ROOT "/resources/packed/repentance.a"
#define ARCHIVE_AFTERBIRTHP ROOT "/resources/packed/afterbirthp.a"
#define ARCHIVE_MUSIC ROOT "/resources/packed/music.a"
#define ARCHIVE_LOOSE ROOT "/resources/loose/graphics.a"

static unsigned s_open_calls;
static unsigned s_close_calls;
static unsigned s_seek_calls;
static unsigned s_tell_calls;
static unsigned s_clearerr_calls;
static unsigned s_live_files;
static unsigned s_peak_live_files;
static int s_fail_next_seek;
static long s_override_next_tell = -1L;
static char s_events[128];
static size_t s_event_count;
static char s_log[512];

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "archive-cache oracle failed at line %u: %s\n",
            line, expression);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

static void event(char value)
{
    if (s_event_count + 1U < sizeof s_events) {
        s_events[s_event_count++] = value;
        s_events[s_event_count] = '\0';
    }
}

FILE *isaac_vita_archive_cache_oracle_fopen(const char *path,
                                            const char *mode)
{
    FILE *stream;

    event('O');
    ++s_open_calls;
    stream = fopen(path, mode);
    if (stream) {
        ++s_live_files;
        if (s_live_files > s_peak_live_files)
            s_peak_live_files = s_live_files;
    }
    return stream;
}

int isaac_vita_archive_cache_oracle_fclose(FILE *stream)
{
    int result;

    event('C');
    ++s_close_calls;
    result = fclose(stream);
    if (s_live_files)
        --s_live_files;
    return result;
}

int isaac_vita_archive_cache_oracle_fseek(FILE *stream, long offset,
                                          int origin)
{
    event('S');
    ++s_seek_calls;
    if (s_fail_next_seek) {
        s_fail_next_seek = 0;
        errno = EIO;
        return -1;
    }
    return fseek(stream, offset, origin);
}

long isaac_vita_archive_cache_oracle_ftell(FILE *stream)
{
    long result;

    event('T');
    ++s_tell_calls;
    if (s_override_next_tell >= 0L) {
        result = s_override_next_tell;
        s_override_next_tell = -1L;
        return result;
    }
    return ftell(stream);
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_log, sizeof s_log, format, arguments);
    va_end(arguments);
    s_log[sizeof s_log - 1U] = '\0';
}

void isaac_vita_archive_cache_oracle_clearerr(FILE *stream)
{
    event('E');
    ++s_clearerr_calls;
    clearerr(stream);
}

static int write_fixture(const char *path, const char *contents)
{
    FILE *stream = fopen(path, "wb");
    size_t length = strlen(contents);
    size_t written;
    int closed;

    if (!stream)
        return 0;
    written = fwrite(contents, 1U, length, stream);
    closed = fclose(stream);
    return written == length && closed == 0;
}

static void reset_events(void)
{
    s_event_count = 0U;
    s_events[0] = '\0';
}

static int test_reset_and_read(void)
{
    isaac_vita_archive_cache_file first =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file second =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    FILE *original;
    char bytes[16];
    int hit = -1;

    original = isaac_vita_archive_cache_open(&first, ARCHIVE_A, "rb", &hit);
    CHECK(original && first.stream == original && hit == 0);
    CHECK(fread(bytes, 1U, sizeof bytes, original) == 6U);
    CHECK(feof(original));
    CHECK(isaac_vita_archive_cache_close(&first) == 0 && !first.stream);
    CHECK(s_open_calls == 1U && s_close_calls == 0U && s_live_files == 1U);

    CHECK(isaac_vita_archive_cache_open(&second, ARCHIVE_A, "rb", &hit) ==
          original);
    CHECK(hit == 1 && ftell(second.stream) == 0L && !feof(second.stream));
    memset(bytes, 0, sizeof bytes);
    CHECK(fread(bytes, 1U, 3U, second.stream) == 3U);
    CHECK(memcmp(bytes, "abc", 3U) == 0);
    CHECK(s_open_calls == 1U && s_seek_calls == 1U &&
          s_tell_calls == 1U && s_clearerr_calls == 1U);
    CHECK(isaac_vita_archive_cache_close(&second) == 0);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);
    return 0;
}

static int test_simultaneous_busy_fallback(void)
{
    isaac_vita_archive_cache_file first =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file second =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    int first_hit = -1;
    int second_hit = -1;

    CHECK(isaac_vita_archive_cache_open(&first, ARCHIVE_A, "rb",
                                        &first_hit));
    CHECK(isaac_vita_archive_cache_open(&second, ARCHIVE_A, "rb",
                                        &second_hit));
    CHECK(first_hit == 0 && second_hit == 0);
    CHECK(first.stream != second.stream && s_live_files == 2U);
    CHECK(fgetc(first.stream) == 'a' && fgetc(first.stream) == 'b');
    CHECK(fgetc(second.stream) == 'a');
    CHECK(ftell(first.stream) == 2L && ftell(second.stream) == 1L);
    CHECK(isaac_vita_archive_cache_close(&first) == 0);
    CHECK(s_live_files == 2U);
    CHECK(isaac_vita_archive_cache_close(&second) == 0);
    CHECK(s_live_files == 1U);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);
    return 0;
}

static int test_miss_evicts_before_open(void)
{
    isaac_vita_archive_cache_file first =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file second =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    int hit;

    CHECK(isaac_vita_archive_cache_open(&first, ARCHIVE_A, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&first) == 0);
    reset_events();
    s_peak_live_files = s_live_files;
    CHECK(isaac_vita_archive_cache_open(&second, ARCHIVE_B, "rb", &hit));
    CHECK(hit == 0 && strcmp(s_events, "CO") == 0);
    CHECK(s_live_files == 1U && s_peak_live_files == 1U);
    CHECK(isaac_vita_archive_cache_close(&second) == 0);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);
    return 0;
}

static int test_bypasses(void)
{
    isaac_vita_archive_cache_file cached =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file bypass =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    unsigned opens;
    unsigned closes;
    int hit;

    /* Even a bypassing fopen is a cache miss: discard idle before opening. */
    CHECK(isaac_vita_archive_cache_open(&cached, ARCHIVE_A, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&cached) == 0);
    reset_events();
    CHECK(isaac_vita_archive_cache_open(&bypass, ARCHIVE_MUSIC, "rb", &hit));
    CHECK(hit == 0 && strcmp(s_events, "CO") == 0);
    CHECK(isaac_vita_archive_cache_close(&bypass) == 0);
    CHECK(strcmp(s_events, "COC") == 0 && s_live_files == 0U);

    opens = s_open_calls;
    closes = s_close_calls;
    CHECK(isaac_vita_archive_cache_open(&bypass, ARCHIVE_MUSIC, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&bypass) == 0);
    CHECK(s_open_calls == opens + 1U && s_close_calls == closes + 1U);

    opens = s_open_calls;
    closes = s_close_calls;
    CHECK(isaac_vita_archive_cache_open(&bypass, ARCHIVE_A, "r", &hit));
    CHECK(isaac_vita_archive_cache_close(&bypass) == 0);
    CHECK(s_open_calls == opens + 1U && s_close_calls == closes + 1U);

    opens = s_open_calls;
    closes = s_close_calls;
    CHECK(isaac_vita_archive_cache_open(&bypass, ARCHIVE_LOOSE, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&bypass) == 0);
    CHECK(s_open_calls == opens + 1U && s_close_calls == closes + 1U);
    CHECK(s_live_files == 0U);
    return 0;
}

static int test_reset_failure_falls_back(void)
{
    isaac_vita_archive_cache_file first =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file second =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    int hit;

    CHECK(isaac_vita_archive_cache_open(&first, ARCHIVE_A, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&first) == 0);
    reset_events();
    s_fail_next_seek = 1;
    CHECK(isaac_vita_archive_cache_open(&second, ARCHIVE_A, "rb", &hit));
    CHECK(hit == 0 && strcmp(s_events, "SCO") == 0);
    CHECK(s_live_files == 1U);
    CHECK(isaac_vita_archive_cache_close(&second) == 0);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);
    return 0;
}

static int test_reset_cursor_mismatch_falls_back(void)
{
    isaac_vita_archive_cache_file first =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file second =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_diag_event events[ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY];
    uint32_t latest = 0U;
    uint32_t count;
    int hit;

    CHECK(isaac_vita_archive_cache_open(&first, ARCHIVE_A, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&first) == 0);
    reset_events();
    s_log[0] = '\0';
    s_override_next_tell = 7L;
    CHECK(isaac_vita_archive_cache_open(&second, ARCHIVE_A, "rb", &hit));
    CHECK(hit == 0 && strcmp(s_events, "STCO") == 0);
    CHECK(strstr(s_log, "anomaly=cache-reset") &&
          strstr(s_log, "actual=0x00000007"));
    count = isaac_vita_archive_diag_snapshot(
        events, ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY, &latest);
    CHECK(count && events[count - 1U].sequence == latest);
    CHECK(events[count - 1U].kind == ISAAC_VITA_ARCHIVE_DIAG_CACHE_RESET);
    CHECK(events[count - 1U].operation_result == 0 &&
          events[count - 1U].position_after == 7 &&
          strcmp(events[count - 1U].key, "graphics.a") == 0);
    CHECK(isaac_vita_archive_cache_close(&second) == 0);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);
    return 0;
}

static int test_diag_ring_tail(void)
{
    isaac_vita_archive_diag_event event_value;
    isaac_vita_archive_diag_event events[5];
    uint32_t latest = 0U;
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY + 7U; ++i) {
        memset(&event_value, 0, sizeof event_value);
        event_value.kind = ISAAC_VITA_ARCHIVE_DIAG_FSEEK;
        event_value.requested_offset = (int32_t)i;
        strcpy(event_value.key, "graphics.a");
        CHECK(isaac_vita_archive_diag_record(&event_value) != 0U);
    }
    CHECK(isaac_vita_archive_diag_snapshot(events, 5U, &latest) == 5U);
    CHECK(events[4].sequence == latest);
    for (i = 1U; i < 5U; ++i)
        CHECK(events[i].sequence == events[i - 1U].sequence + 1U);
    CHECK(events[4].requested_offset ==
          (int32_t)(ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY + 6U));
    return 0;
}

static int test_diag_log_bound(void)
{
    isaac_vita_archive_diag_event event_value;
    char build_id[97];
    char line[ISAAC_VITA_ARCHIVE_DIAG_LOG_CAPACITY];
    int length;

    memset(&event_value, 0, sizeof event_value);
    memset(build_id, 'b', sizeof build_id - 1U);
    build_id[sizeof build_id - 1U] = '\0';
    memset(event_value.key, 'k', sizeof event_value.key - 1U);
    event_value.key[sizeof event_value.key - 1U] = '\0';
    event_value.sequence = UINT32_MAX;
    event_value.kind = ISAAC_VITA_ARCHIVE_DIAG_CACHE_RESET;
    event_value.token = UINT32_MAX;
    event_value.flags = UINT32_MAX;
    event_value.requested_offset = -1;
    event_value.origin = INT32_MIN;
    event_value.operation_result = INT32_MIN;
    event_value.position_before = -1;
    event_value.position_after = -1;
    event_value.operation_errno = INT32_MIN;
    event_value.element_size = UINT32_MAX;
    event_value.element_count = UINT32_MAX;
    event_value.elements_returned = UINT32_MAX;
    event_value.caller_return = UINT32_MAX;
    event_value.observed_word = UINT32_MAX;
    memset(line, 'Z', sizeof line);

    CHECK(isaac_vita_archive_diag_format_event(
              line, sizeof line, NULL, "missing-build", &event_value) == -1);
    length = isaac_vita_archive_diag_format_event(
        line, sizeof line, build_id,
        "rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr", &event_value);
    CHECK(length == 382 && (size_t)length == strlen(line));
    CHECK(line[length] == '\0' && line[length + 1] == 'Z');
    CHECK(strstr(line,
          "bid_prefix=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb why=") != NULL);
    CHECK(strstr(line,
          "why=rrrrrrrrrrrrrrrrrrrrrrrr seq=4294967295 op=cache-reset") != NULL);
    CHECK(strstr(line, "word=ffffffff") != NULL);
    return 0;
}

static int test_force_discard_and_drop(void)
{
    isaac_vita_archive_cache_file file =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    unsigned opens;
    unsigned closes;
    int hit;

    opens = s_open_calls;
    closes = s_close_calls;
    CHECK(isaac_vita_archive_cache_open(&file, ARCHIVE_A, "rb", &hit));
    CHECK(isaac_vita_archive_cache_force_discard(&file) == 0);
    CHECK(!file.stream && s_open_calls == opens + 1U &&
          s_close_calls == closes + 1U && s_live_files == 0U);
    CHECK(isaac_vita_archive_cache_open(&file, ARCHIVE_A, "rb", &hit));
    CHECK(hit == 0 && s_open_calls == opens + 2U);
    CHECK(isaac_vita_archive_cache_close(&file) == 0);
    closes = s_close_calls;
    CHECK(isaac_vita_archive_cache_drop() == 0);
    CHECK(s_close_calls == closes + 1U && s_live_files == 0U);
    CHECK(isaac_vita_archive_cache_drop() == 0);
    CHECK(s_close_calls == closes + 1U);

    opens = s_open_calls;
    CHECK(isaac_vita_archive_cache_open(&file, ARCHIVE_A, "rb", &hit));
    CHECK(hit == 0 && s_open_calls == opens + 1U);
    CHECK(isaac_vita_archive_cache_close(&file) == 0);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);
    return 0;
}

/* The CRT's descriptor-recovery open: an idle stream is evicted first
 * exactly like any miss, the fresh stream is never retained, and a later
 * discard closes it without touching the idle slot. */
static int test_reopen_native_is_a_miss(void)
{
    isaac_vita_archive_cache_file cached =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    isaac_vita_archive_cache_file discard =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    FILE *fresh;
    char bytes[8];
    int hit;

    CHECK(isaac_vita_archive_cache_open(&cached, ARCHIVE_A, "rb", &hit));
    CHECK(isaac_vita_archive_cache_close(&cached) == 0 && s_live_files == 1U);
    reset_events();
    fresh = isaac_vita_archive_cache_reopen_native(ARCHIVE_B, "rb");
    CHECK(fresh && strcmp(s_events, "CO") == 0 && s_live_files == 1U);
    CHECK(fread(bytes, 1U, 3U, fresh) == 3U && memcmp(bytes, "uvw", 3U) == 0);
    discard.stream = fresh;
    CHECK(isaac_vita_archive_cache_force_discard(&discard) == 0 &&
          !discard.stream && strcmp(s_events, "COC") == 0 &&
          s_live_files == 0U);
    reset_events();
    CHECK(isaac_vita_archive_cache_open(&cached, ARCHIVE_A, "rb", &hit));
    CHECK(hit == 0 && strcmp(s_events, "O") == 0);
    CHECK(isaac_vita_archive_cache_close(&cached) == 0);
    CHECK(isaac_vita_archive_cache_drop() == 0 && s_live_files == 0U);

    reset_events();
    fresh = isaac_vita_archive_cache_reopen_native(ARCHIVE_A, "rb");
    CHECK(fresh && strcmp(s_events, "O") == 0 && s_live_files == 1U);
    discard.stream = fresh;
    CHECK(isaac_vita_archive_cache_force_discard(&discard) == 0 &&
          s_live_files == 0U);
    errno = 0;
    CHECK(!isaac_vita_archive_cache_reopen_native(NULL, "rb") &&
          errno == EINVAL);
    errno = 0;
    CHECK(!isaac_vita_archive_cache_reopen_native(ARCHIVE_A, NULL) &&
          errno == EINVAL);
    errno = 0;
    CHECK(!isaac_vita_archive_cache_reopen_native(
              ROOT "/resources/packed/missing.a", "rb") &&
          errno == ENOENT && s_live_files == 0U);
    return 0;
}

static int test_key_contracts(void)
{
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY];

    memset(key, 'x', sizeof key);
    CHECK(isaac_vita_archive_cache_key(
              ARCHIVE_AFTERBIRTHP, "rb", key));
    CHECK(strcmp(key, "afterbirthp.a") == 0);
    CHECK(!isaac_vita_archive_cache_key(
              ARCHIVE_AFTERBIRTHP, "r", key) && !key[0]);
    CHECK(!isaac_vita_archive_cache_key(
              ARCHIVE_MUSIC, "rb", key) && !key[0]);
    CHECK(!isaac_vita_archive_cache_key(
              ARCHIVE_LOOSE, "rb", key) && !key[0]);
    CHECK(!isaac_vita_archive_cache_key(
              ROOT "/resources/packed/sub/afterbirthp.a", "rb", key) &&
          !key[0]);
    CHECK(!isaac_vita_archive_cache_key(
              ROOT "/resources/packed/afterbirthp.A", "rb", key) &&
          !key[0]);

    CHECK(isaac_vita_archive_raw_fallback_key(
              ARCHIVE_AFTERBIRTHP, "rb", key));
    CHECK(strcmp(key, "afterbirthp.a") == 0);
    CHECK(isaac_vita_archive_raw_fallback_key(
              ARCHIVE_MUSIC, "rb", key));
    CHECK(strcmp(key, "music.a") == 0);
    CHECK(!isaac_vita_archive_raw_fallback_key(
              ARCHIVE_MUSIC, "r", key) && !key[0]);
    CHECK(!isaac_vita_archive_raw_fallback_key(
              ARCHIVE_LOOSE, "rb", key) && !key[0]);
    CHECK(!isaac_vita_archive_raw_fallback_key(
              ROOT "/resources/packed/sub/music.a", "rb", key) && !key[0]);
    CHECK(!isaac_vita_archive_raw_fallback_key(
              ROOT "/resources/packed/music.A", "rb", key) && !key[0]);
    return 0;
}

#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
/* newlib 0da3cb1 as disassembled (lib_a-syscalls.o _close_r, lib_a-io.o
 * __vita_release_descriptor and __vita_fd_drop): _close_r is -1/EBADF unless
 * the release succeeds; release needs fd 1..255 with a map entry and clears
 * it only when drop >= 0; drop decrements a ref_count above 1, otherwise
 * calls sceIoClose(sce_uid), frees filename whatever the result, and only on
 * success decrements and memsets the entry. */
#define SCE_ENODEV ((int32_t)UINT32_C(0x80010013))

static isaac_vita_archive_newlib_fd s_pool[4];
static isaac_vita_archive_newlib_fd *s_fdmap[ISAAC_VITA_ARCHIVE_NEWLIB_FD_COUNT];
static char s_names[16][32];
static unsigned s_name_next;
static const char *s_freed[16];
static unsigned s_freed_count;
static unsigned s_double_frees;
static unsigned s_sce_close_calls;
static unsigned s_dummy_opens;
static int32_t s_dead_uid = -1;
static int s_dummy_fails;
static int s_dummy_close_fails;

static void fake_free(const char *pointer)
{
    unsigned i;

    for (i = 0U; i < s_freed_count; ++i)
        if (s_freed[i] == pointer)
            ++s_double_frees;
    if (s_freed_count < 16U)
        s_freed[s_freed_count++] = pointer;
}

static int32_t fake_sce_close(int32_t uid)
{
    ++s_sce_close_calls;
    if (uid == s_dead_uid || (uid == 900 && s_dummy_close_fails))
        return SCE_ENODEV;
    return 0;
}

static int fake_drop(isaac_vita_archive_newlib_fd *map)
{
    int32_t result;

    if (map->ref_count != 1) {
        --map->ref_count;
        return 0;
    }
    result = fake_sce_close(map->sce_uid);
    if (map->filename)
        fake_free(map->filename);
    if (result < 0)
        return -(int)(result & 0xff);
    --map->ref_count;
    memset(map, 0, sizeof *map);
    return 0;
}

int isaac_vita_archive_cache_oracle_close_r(void *reent, int fd)
{
    int released = 0;

    if ((unsigned)(fd - 1) <= 254U && s_fdmap[fd] &&
        fake_drop(s_fdmap[fd]) >= 0) {
        s_fdmap[fd] = NULL;
        released = 1;
    }
    *(int *)reent = released ? 0 : EBADF;
    return released ? 0 : -1;
}

isaac_vita_archive_newlib_fd *isaac_vita_archive_cache_oracle_fd_entry(int fd)
{
    return s_fdmap[fd];
}

int32_t isaac_vita_archive_cache_oracle_dummy_open(void)
{
    ++s_dummy_opens;
    return s_dummy_fails ? SCE_ENODEV : 900;
}

static void fake_open(int fd, unsigned slot, int32_t uid, int ref_count)
{
    char *name = s_names[s_name_next++ % 16U];

    memset(&s_pool[slot], 0, sizeof s_pool[slot]);
    s_pool[slot].sce_uid = uid;
    s_pool[slot].ref_count = ref_count;
    (void)snprintf(name, sizeof s_names[0], "ux0:/f%d", fd);
    s_pool[slot].filename = name;
    s_fdmap[fd] = &s_pool[slot];
}

static int pool_entry_clear(unsigned slot)
{
    static const isaac_vita_archive_newlib_fd zero;

    return memcmp(&s_pool[slot], &zero, sizeof zero) == 0;
}

static int test_fd_close_repair(void)
{
    uint32_t repaired;
    uint32_t leaked;
    int reent_errno;

    /* A healthy close: one sceIoClose, released, nothing repaired. */
    fake_open(5, 0U, 100, 1);
    reent_errno = -1;
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 5) == 0);
    CHECK(reent_errno == 0 && !s_fdmap[5] && pool_entry_clear(0U) &&
          s_sce_close_calls == 1U && s_dummy_opens == 0U);

    /* The leak itself, through newlib's plain close: the refused close
     * keeps map and pool entry (ref_count 1) and frees the filename it still
     * points at. */
    fake_open(6, 1U, 200, 1);
    s_dead_uid = 200;
    CHECK(isaac_vita_archive_cache_oracle_close_r(&reent_errno, 6) == -1);
    CHECK(reent_errno == EBADF && s_fdmap[6] == &s_pool[1] &&
          s_pool[1].ref_count == 1 && s_pool[1].filename == s_names[1] &&
          s_freed_count == 2U && s_freed[1] == s_names[1]);

    /* The repair: the same refused close, then the release again on the
     * dummy handle; the fd and pool entry are back, the dangling filename
     * is not freed twice, the caller still sees -1/EBADF. */
    fake_open(7, 2U, 200, 1);
    s_sce_close_calls = 0U;
    reent_errno = 0;
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 7) == -1);
    CHECK(reent_errno == EBADF && !s_fdmap[7] && pool_entry_clear(2U) &&
          s_sce_close_calls == 2U && s_dummy_opens == 1U &&
          s_double_frees == 0U && s_freed_count == 3U);
    isaac_vita_archive_fd_repair_counts(&repaired, &leaked);
    CHECK(repaired == 1U && leaked == 0U);

    /* No dummy handle: counted, the entry stays as newlib left it. */
    fake_open(8, 3U, 200, 1);
    s_dummy_fails = 1;
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 8) == -1);
    CHECK(reent_errno == EBADF && s_fdmap[8] == &s_pool[3] &&
          s_dummy_opens == 2U && s_double_frees == 0U);
    s_dummy_fails = 0;
    /* The dummy's own close refused: counted, never freed twice. */
    fake_open(9, 0U, 200, 1);
    s_dummy_close_fails = 1;
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 9) == -1);
    CHECK(reent_errno == EBADF && s_fdmap[9] == &s_pool[0] &&
          s_dummy_opens == 3U && s_double_frees == 0U);
    s_dummy_close_fails = 0;
    isaac_vita_archive_fd_repair_counts(&repaired, &leaked);
    CHECK(repaired == 1U && leaked == 2U);

    /* Never touched: a standard or out-of-range fd, an unmapped fd, a
     * directory entry, and a shared (dup'd) entry whose close succeeds. */
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 2) == -1);
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 256) == -1);
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 40) == -1);
    fake_open(10, 1U, 200, 1);
    s_pool[1].type = 1U;
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 10) == -1);
    CHECK(s_fdmap[10] == &s_pool[1] && s_dummy_opens == 3U);
    fake_open(11, 2U, 200, 2);
    CHECK(isaac_vita_archive_fd_close_repair(&reent_errno, &reent_errno, 11) == 0);
    CHECK(reent_errno == 0 && !s_fdmap[11] &&
          s_pool[2].ref_count == 1 && s_dummy_opens == 3U);
    isaac_vita_archive_fd_repair_counts(&repaired, &leaked);
    CHECK(repaired == 1U && leaked == 2U && s_double_frees == 0U);
    return 0;
}
#endif

int main(void)
{
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    CHECK(test_fd_close_repair() == 0);
#endif
    CHECK(write_fixture(ARCHIVE_A, "abcdef"));
    CHECK(write_fixture(ARCHIVE_B, "uvwxyz"));
    CHECK(write_fixture(ARCHIVE_MUSIC, "music"));
    CHECK(write_fixture(ARCHIVE_LOOSE, "loose"));

    CHECK(test_reset_and_read() == 0);
    CHECK(test_simultaneous_busy_fallback() == 0);
    CHECK(test_miss_evicts_before_open() == 0);
    CHECK(test_bypasses() == 0);
    CHECK(test_reset_failure_falls_back() == 0);
    CHECK(test_reset_cursor_mismatch_falls_back() == 0);
    CHECK(test_force_discard_and_drop() == 0);
    CHECK(test_reopen_native_is_a_miss() == 0);
    CHECK(test_key_contracts() == 0);
    CHECK(test_diag_ring_tail() == 0);
    CHECK(test_diag_log_bound() == 0);
    CHECK(s_live_files == 0U);
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    puts("Vita archive one-idle FILE cache host oracle "
         "(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER, fd release repair): PASS");
#else
    puts("Vita archive one-idle FILE cache host oracle: PASS");
#endif
    return 0;
}
EOF

(CDPATH= cd -- "$run" && "$work/archive-cache-host-oracle-$recover")
done

sha256sum \
    "$root/runtime/host_vita_archive_cache.h" \
    "$root/runtime/host_vita_archive_cache.c" \
    "$root/vita/test_archive_cache.sh" \
    "$work/archive-cache-host-oracle-off" \
    "$work/archive-cache-host-oracle-on"
echo "Vita archive one-idle FILE cache host behavior: PASS"
