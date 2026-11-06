/* Hostile executable oracle for the game log write-behind lane
 * (host_vita_game_log.c built with ISAAC_VITA_GAME_LOG_ORACLE).  The oracle
 * owns the clock, the "disk" and the worker: spans reach the disk only when
 * this file performs a worker pass (or when the module appends on the
 * guest's behalf), so every policy decision is observable.
 *
 * Built by recomp/vita/test_game_log_async.sh at ring 64 and 32 KiB and at
 * every ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY 0..4. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_game_log.h"

#define ROOT "ux0:/data/isaacr001"
#define SAVE_DIR ROOT "/Documents/My Games/Binding of Isaac Repentance"
#define LOG_TXT SAVE_DIR "/log.txt"
#define RING ISAAC_VITA_GAME_LOG_RING_BYTES
#define DURABLE ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY
/* A severity the policy defers (INFO, only when DURABLE >= 1) and one it
 * appends synchronously (DURABLE itself, or 4 = outside the chain). */
#define HAS_DEFERRED (DURABLE >= 1U)
#define DEFERRED_SEVERITY 0U
#define SYNC_SEVERITY (DURABLE <= 3U ? DURABLE : 4U)

/* ---- disk double -------------------------------------------------------- */

static unsigned char s_disk[4U * 1024U * 1024U];
static uint32_t s_disk_size;
static char s_disk_path[ISAAC_VITA_GAME_LOG_PATH_MAX + 1U];
static uint32_t s_disk_opens;
static uint32_t s_disk_trunc_opens;
static uint32_t s_disk_writes;
static uint32_t s_disk_closes;
static uint32_t s_fail_opens;       /* fail this many opens, then succeed */
static uint32_t s_fail_writes;      /* fail this many writes, then succeed */
static uint32_t s_short_write;      /* accept at most this many bytes once */
static uint32_t s_wrong_path_opens;

int32_t isaac_vita_game_log_oracle_open(const char *path, int truncate,
                                        int append)
{
    (void)append;
    if (s_fail_opens) {
        --s_fail_opens;
        return (int32_t)UINT32_C(0x80010005);
    }
    if (!s_disk_path[0])
        memcpy(s_disk_path, path, strlen(path) + 1U);
    else if (strcmp(s_disk_path, path) != 0) {
        ++s_wrong_path_opens;
        return (int32_t)UINT32_C(0x80010002);
    }
    if (truncate) {
        s_disk_size = 0U;
        ++s_disk_trunc_opens;
    }
    ++s_disk_opens;
    g_isaac_vita_game_log_oracle_now_us += 300U;
    return 100;
}

int32_t isaac_vita_game_log_oracle_write(int32_t descriptor,
                                         const void *buffer, uint32_t size)
{
    if (descriptor != 100)
        return (int32_t)UINT32_C(0x80010009);
    if (s_fail_writes) {
        --s_fail_writes;
        return (int32_t)UINT32_C(0x80010005);
    }
    if (s_short_write && size > s_short_write) {
        size = s_short_write;
        s_short_write = 0U;
    }
    if (s_disk_size + size > sizeof s_disk)
        return (int32_t)UINT32_C(0x8001001c);
    memcpy(s_disk + s_disk_size, buffer, size);
    s_disk_size += size;
    ++s_disk_writes;
    g_isaac_vita_game_log_oracle_now_us += 1000U;
    return (int32_t)size;
}

int32_t isaac_vita_game_log_oracle_close(int32_t descriptor)
{
    if (descriptor != 100)
        return (int32_t)UINT32_C(0x80010009);
    ++s_disk_closes;
    return 0;
}

int64_t isaac_vita_game_log_oracle_size(int32_t descriptor)
{
    return descriptor == 100 ? (int64_t)s_disk_size : -1;
}

/* ---- receipt sink ------------------------------------------------------ */

static uint32_t s_logs;
static uint32_t s_log_open;
static uint32_t s_log_append;
static uint32_t s_log_refuse;
static uint32_t s_log_wait;
static uint32_t s_log_summary;
static char s_last_line[1024];
static char s_last_summary[1024];
static char s_last_open[512];

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_last_line, sizeof s_last_line, format, arguments);
    va_end(arguments);
    ++s_logs;
    if (strncmp(s_last_line, "[isaac-gamelog] open ", 21U) == 0) {
        ++s_log_open;
        memcpy(s_last_open, s_last_line, sizeof s_last_open);
        s_last_open[sizeof s_last_open - 1U] = '\0';
    } else if (strncmp(s_last_line, "[isaac-gamelog] append ", 23U) == 0) {
        ++s_log_append;
    } else if (strncmp(s_last_line, "[isaac-gamelog] refuse ", 23U) == 0) {
        ++s_log_refuse;
    } else if (strncmp(s_last_line, "[isaac-gamelog] wait ", 21U) == 0) {
        ++s_log_wait;
    } else if (strncmp(s_last_line, "[isaac-gamelog] summary ", 24U) == 0) {
        ++s_log_summary;
        memcpy(s_last_summary, s_last_line, sizeof s_last_summary);
    }
}

/* ---- harness ------------------------------------------------------------ */

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "game-log oracle failed at line %u: %s\n", line,
            expression);
    if (s_last_line[0])
        fprintf(stderr, "  last receipt: %s\n", s_last_line);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

static unsigned char s_expected[4U * 1024U * 1024U];
static uint32_t s_expected_size;

static void reset_case(void)
{
    isaac_vita_game_log_oracle_reset();
    s_expected_size = 0U;
    memset(s_disk, 0, sizeof s_disk);
    s_disk_size = 0U;
    s_disk_path[0] = '\0';
    s_disk_opens = 0U;
    s_disk_trunc_opens = 0U;
    s_disk_writes = 0U;
    s_disk_closes = 0U;
    s_fail_opens = 0U;
    s_fail_writes = 0U;
    s_short_write = 0U;
    s_wrong_path_opens = 0U;
    s_logs = 0U;
    s_log_open = 0U;
    s_log_append = 0U;
    s_log_refuse = 0U;
    s_log_wait = 0U;
    s_log_summary = 0U;
    s_last_line[0] = '\0';
    s_last_summary[0] = '\0';
    s_last_open[0] = '\0';
}

static isaac_vita_game_log_snapshot snap(void)
{
    isaac_vita_game_log_snapshot s;

    isaac_vita_game_log_snapshot_read(&s);
    return s;
}

static void expect_bytes(const void *bytes, uint32_t length)
{
    memcpy(s_expected + s_expected_size, bytes, length);
    s_expected_size += length;
}

static int disk_matches_expected(void)
{
    return s_disk_size == s_expected_size &&
        memcmp(s_disk, s_expected, s_disk_size) == 0;
}

static size_t write_text(const char *text)
{
    size_t length = strlen(text);
    size_t written = isaac_vita_game_log_write(text, 1U, length);

    if (written == length)
        expect_bytes(text, (uint32_t)length);
    return written;
}

/* Ring-sized: one case writes a record of RING - 100 bytes. */
static unsigned char s_record[ISAAC_VITA_GAME_LOG_RING_BYTES];

static size_t write_record(uint32_t length, uint32_t seed)
{
    uint32_t i;
    size_t written;

    for (i = 0U; i < length; ++i)
        s_record[i] = (unsigned char)(seed * 31U + i * 7U + 1U);
    written = isaac_vita_game_log_write(s_record, 1U, length);
    if (written == length)
        expect_bytes(s_record, length);
    return written;
}

/* Summary fields, in receipt order. */
typedef struct summary_fields {
    char bid[41];
    char state[16];
    unsigned records, bytes;
    unsigned flush[6];
    unsigned appends[5];
    unsigned io_us[4];
    unsigned wait[3];
    unsigned ring[2];
    unsigned coherence[7];
    unsigned demote, late, lost_bytes, abandoned_bytes, fail;
} summary_fields;

static int parse_summary(const char *line, summary_fields *f)
{
    int matched = sscanf(
        line,
        "[isaac-gamelog] summary bid40=%40s state=%15s records=%u bytes=%u "
        "flush(info,warn,error,assert,other,null)=%u,%u,%u,%u,%u,%u "
        "appends(worker,guest,exit,abort,direct)=%u,%u,%u,%u,%u "
        "io_us(worker,worker_max,guest,guest_max)=%u,%u,%u,%u "
        "wait(n,us,max)=%u,%u,%u ring(high,full_waits)=%u,%u "
        "coherence(open,remove,access,attrib,find,seek,tell)=%u,%u,%u,%u,%u,%u,%u "
        "demote=%u late=%u lost_bytes=%u abandoned_bytes=%u fail=%u",
        f->bid, f->state, &f->records, &f->bytes,
        &f->flush[0], &f->flush[1], &f->flush[2], &f->flush[3],
        &f->flush[4], &f->flush[5],
        &f->appends[0], &f->appends[1], &f->appends[2], &f->appends[3],
        &f->appends[4],
        &f->io_us[0], &f->io_us[1], &f->io_us[2], &f->io_us[3],
        &f->wait[0], &f->wait[1], &f->wait[2],
        &f->ring[0], &f->ring[1],
        &f->coherence[0], &f->coherence[1], &f->coherence[2],
        &f->coherence[3], &f->coherence[4], &f->coherence[5],
        &f->coherence[6],
        &f->demote, &f->late, &f->lost_bytes, &f->abandoned_bytes, &f->fail);
    return matched == 36;
}

/* ---- cases -------------------------------------------------------------- */

static int case_eligibility(void)
{
    static const char long_path[ISAAC_VITA_GAME_LOG_PATH_MAX + 40U] = { 0 };
    char too_long[ISAAC_VITA_GAME_LOG_PATH_MAX + 40U];
    size_t i;

    reset_case();
    (void)long_path;
    CHECK(isaac_vita_game_log_eligible(LOG_TXT, "w"));
    CHECK(isaac_vita_game_log_eligible(LOG_TXT, "wb"));
    CHECK(isaac_vita_game_log_eligible(LOG_TXT, "a"));
    CHECK(isaac_vita_game_log_eligible(LOG_TXT, "ab"));
    CHECK(isaac_vita_game_log_eligible(ROOT "/Documents/log.txt", "w"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "r"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "rb"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "r+"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "r+b"));
    /* Read modes of the game log stay silent (a reader is not the logger). */
    CHECK(snap().refused == 0U && s_log_refuse == 0U && s_logs == 0U);
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "w+"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "a+"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "wt"));
    /* A write/append mode the lane does not carry is counted and receipted
     * with the guest's mode string: an inert lane is visible on device. */
    CHECK(snap().refused == 3U && s_log_refuse == 3U &&
          strstr(s_last_line, " mode=wt reason=mode") != NULL);
    CHECK(!isaac_vita_game_log_eligible(SAVE_DIR "/options.ini", "w"));
    CHECK(!isaac_vita_game_log_eligible(SAVE_DIR "/gamestate1.dat", "wb"));
    CHECK(!isaac_vita_game_log_eligible(ROOT "/resources/log.txt", "w"));
    CHECK(!isaac_vita_game_log_eligible(SAVE_DIR "/mylog.txt", "w"));
    CHECK(!isaac_vita_game_log_eligible(SAVE_DIR "/log.txt/", "w"));
    CHECK(!isaac_vita_game_log_eligible(ROOT "/Documents/", "w"));
    CHECK(!isaac_vita_game_log_eligible("ux0:data/isaacr001/Documents/log.txt",
                                        "w"));
    CHECK(!isaac_vita_game_log_eligible(NULL, "w"));
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, NULL));
    memcpy(too_long, SAVE_DIR "/", sizeof SAVE_DIR);
    for (i = sizeof SAVE_DIR; i < sizeof too_long - 9U; ++i)
        too_long[i] = 'x';
    memcpy(too_long + sizeof too_long - 9U, "/log.txt", 9U);
    CHECK(strlen(too_long) > ISAAC_VITA_GAME_LOG_PATH_MAX);
    /* Other paths were silent; the over-long game-log path is receipted. */
    CHECK(snap().refused == 3U && s_log_refuse == 3U);
    CHECK(!isaac_vita_game_log_eligible(too_long, "w"));
    CHECK(snap().refused == 4U && s_log_refuse == 4U &&
          strstr(s_last_line, " mode=w reason=path") != NULL);
    /* A second lane while one is live is refused. */
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "w"));
    CHECK(!isaac_vita_game_log_eligible(ROOT "/Documents/log.txt", "a"));
    CHECK(snap().refused == 6U && s_log_refuse == 6U &&
          strstr(s_last_line, " mode=a reason=live") != NULL);
    CHECK(isaac_vita_game_log_open(ROOT "/Documents/log.txt", "a") == 0);
    CHECK(isaac_vita_game_log_close() == 0);
    CHECK(isaac_vita_game_log_eligible(LOG_TXT, "w"));
    /* Receipts are bounded; the counter is not. */
    for (i = 0U; i < 10U; ++i)
        CHECK(!isaac_vita_game_log_eligible(LOG_TXT, "w+"));
    CHECK(snap().refused == 16U && s_log_refuse == 8U);
    return 0;
}

static int case_open_modes(void)
{
    isaac_vita_game_log_snapshot s;

    reset_case();
    /* Pre-existing 100-byte file: "w" truncates it at open. */
    memcpy(s_disk_path, LOG_TXT, sizeof LOG_TXT);
    s_disk_size = 100U;
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(s_disk_size == 0U && s_disk_trunc_opens == 1U &&
          s_disk_closes == 1U);
    s = snap();
    CHECK(s.live && s.logical_size == 0 &&
          s.worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY &&
          !s.synchronous && s.worker_idle);
    CHECK(s_log_open == 1U && s_logs == 1U);
    CHECK(strstr(s_last_open, " mode=w size=0 ring_kb=") != NULL);
    CHECK(strstr(s_last_open, " worker=1 prio=0x10000100 affinity=0x00000000 rc=0x00000000") != NULL);
    CHECK(isaac_vita_game_log_tell() == 0L);
    CHECK(isaac_vita_game_log_close() == 0);
    /* "a" keeps the file and starts at its size. */
    reset_case();
    memcpy(s_disk_path, LOG_TXT, sizeof LOG_TXT);
    memset(s_disk, 'z', 100U);
    s_disk_size = 100U;
    expect_bytes(s_disk, 100U);
    CHECK(isaac_vita_game_log_open(LOG_TXT, "ab") == 1);
    CHECK(s_disk_size == 100U && s_disk_trunc_opens == 0U);
    CHECK(isaac_vita_game_log_tell() == 100L);
    CHECK(strstr(s_last_open, " mode=ab size=100 ") != NULL);
    CHECK(write_text("appended\n") == 9U);
    CHECK(isaac_vita_game_log_tell() == 109L);
    CHECK(isaac_vita_game_log_flush(ISAAC_VITA_GAME_LOG_SEVERITY_UNKNOWN) == 0);
    CHECK(disk_matches_expected());
    /* Native open failure: nothing adopted, errno carries the native code. */
    reset_case();
    s_fail_opens = 1U;
    errno = 0;
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 0);
    CHECK(errno == 5 && !snap().live && s_log_open == 0U);
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_start_failure(void)
{
    summary_fields f;

    reset_case();
    g_isaac_vita_game_log_oracle_start_fails = 1;
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 0);
    CHECK(s_log_open == 1U && strstr(s_last_open, " worker=2 ") != NULL &&
          strstr(s_last_open, " rc=0x80020001") != NULL);
    CHECK(snap().worker_state == ISAAC_VITA_GAME_LOG_WORKER_FAILED &&
          snap().start_failed && !snap().live && s_disk_opens == 0U);
    /* A later open does not retry the worker and adopts nothing. */
    g_isaac_vita_game_log_oracle_start_fails = 0;
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 0);
    CHECK(s_log_open == 2U && !snap().live);
    /* Every API on a lane that is not live answers like a closed FILE. */
    errno = 0;
    CHECK(isaac_vita_game_log_write("x", 1U, 1U) == 0U && errno == EBADF);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == EOF);
    CHECK(isaac_vita_game_log_tell() == -1L);
    CHECK(isaac_vita_game_log_seek(0L, SEEK_SET) == -1);
    CHECK(isaac_vita_game_log_close() == EOF);
    CHECK(!isaac_vita_game_log_owns_path(LOG_TXT));
    isaac_vita_game_log_sync_path(LOG_TXT, ISAAC_VITA_GAME_LOG_SYNC_REMOVE);
    isaac_vita_game_log_sync_directory(SAVE_DIR);
    isaac_vita_game_log_flush_null();
    isaac_vita_game_log_abort_drain();
    isaac_vita_game_log_shutdown();
    CHECK(s_log_summary == 1U && parse_summary(s_last_summary, &f));
    CHECK(strcmp(f.state, "start-failed") == 0 && f.records == 0U);
    return 0;
}

static int case_info_flow(void)
{
    isaac_vita_game_log_snapshot s;

    if (!HAS_DEFERRED)
        return 0;
    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("[INFO] - first\n") == 15U);
    /* The idle -> busy transition wakes the worker once. */
    CHECK(g_isaac_vita_game_log_oracle_wakes == 1U);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    CHECK(s_disk_size == 0U && s_disk_opens == 1U);
    CHECK(write_text("[INFO] - second\n") == 16U);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    /* No second wake while the worker has not gone idle. */
    CHECK(g_isaac_vita_game_log_oracle_wakes == 1U && s_disk_size == 0U);
    s = snap();
    CHECK(s.used == 31U && s.records == 2U && s.bytes == 31U &&
          s.flush[DEFERRED_SEVERITY] == 2U && s.high == 31U && !s.worker_idle);
    /* One worker pass appends both records in order. */
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    CHECK(disk_matches_expected() && s_disk_opens == 2U &&
          s_disk_writes == 1U && s_disk_closes == 2U);
    s = snap();
    CHECK(s.used == 0U && s.appends[0] == 1U && s.io_us_worker > 0U &&
          s.io_us_worker_max == s.io_us_worker && s.worker_idle);
    CHECK(isaac_vita_game_log_oracle_run_one() == 0);
    /* fflush(NULL) with nothing pending does not wake; with bytes pending
     * and an idle worker it does. */
    isaac_vita_game_log_flush_null();
    CHECK(g_isaac_vita_game_log_oracle_wakes == 1U);
    CHECK(write_text("[INFO] - third\n") == 15U);
    CHECK(g_isaac_vita_game_log_oracle_wakes == 2U);
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    isaac_vita_game_log_flush_null();
    CHECK(g_isaac_vita_game_log_oracle_wakes == 2U);
    /* An INFO flush never blocks and never appends on the guest. */
    CHECK(snap().appends[1] == 0U && snap().waits == 0U);
    CHECK(snap().flush[5] == 2U);
    CHECK(isaac_vita_game_log_close() == 0);
    CHECK(disk_matches_expected() && s_logs == 1U);
    return 0;
}

static int case_warn_flow(void)
{
    isaac_vita_game_log_snapshot s;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("[INFO] - a\n") == 11U);
    /* The worker claims the INFO record and is still inside sceIoWrite. */
    CHECK(isaac_vita_game_log_oracle_strand_one() == 1);
    CHECK(snap().draining == 1U && snap().claimed == 11U);
    CHECK(write_text("[WARN] - b\n") == 11U);
    /* WARN: wait for the span in flight (the oracle completes it inside the
     * wait step), then the guest appends the rest itself. */
    g_isaac_vita_game_log_oracle_service_on_wait = 1;
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    CHECK(disk_matches_expected() && s_disk_opens == 3U);
    s = snap();
    CHECK(s.appends[0] == 1U && s.appends[1] == 1U && s.waits == 1U &&
          s.wait_us >= 2000000U && s.wait_us_max == s.wait_us &&
          s.io_us_guest > 0U && s.flush[SYNC_SEVERITY] == 1U && s.used == 0U &&
          s.draining == 0U);
    /* A WARN flush with nothing pending and no span in flight: no I/O. */
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    CHECK(s_disk_opens == 3U && snap().appends[1] == 1U && snap().waits == 1U);
    /* ERROR and ASSERT with pending bytes and an idle worker: the guest
     * appends immediately, no wait. */
    CHECK(write_text("[ERROR] - c\n") == 12U);
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    CHECK(write_text("[ASSERT] - d\n") == 13U);
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    CHECK(disk_matches_expected());
    s = snap();
    /* Four synchronous flushes so far: WARN, the empty WARN, ERROR, ASSERT. */
    CHECK(s.appends[1] == 3U && s.waits == 1U &&
          s.flush[SYNC_SEVERITY] == 4U);
    CHECK(isaac_vita_game_log_oracle_run_one() == 0);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_durable_matrix(void)
{
    unsigned severity;
    /* A variable, not the macro: DURABLE 0 must not trip -Wtype-limits. */
    unsigned durable = DURABLE;

    for (severity = 0U; severity <= 4U; ++severity) {
        int synchronous = severity == 4U || severity >= durable;

        reset_case();
        CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
        CHECK(write_text("record\n") == 7U);
        CHECK(isaac_vita_game_log_flush(severity) == 0);
        if (synchronous) {
            CHECK(disk_matches_expected() && snap().appends[1] == 1U);
        } else {
            CHECK(s_disk_size == 0U && snap().appends[1] == 0U &&
                  snap().used == 7U);
            CHECK(isaac_vita_game_log_oracle_run_one() == 1);
            CHECK(disk_matches_expected());
        }
        CHECK(snap().flush[severity] == 1U);
        CHECK(isaac_vita_game_log_close() == 0);
    }
    return 0;
}

static int case_fifo_wrap(void)
{
    uint32_t i;
    uint32_t total = 0U;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    /* Records of every size class, wrapping the ring several times, with
     * worker passes at irregular points and a WARN drain every 17th. */
    for (i = 0U; total < 5U * RING; ++i) {
        uint32_t length = 1U + (i * 977U) % 3000U;

        CHECK(write_record(length, i) == length);
        total += length;
        if (i % 3U == 0U)
            CHECK(isaac_vita_game_log_oracle_run_one() == 1);
        if (i % 17U == 16U)
            CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
        if (i % 5U == 4U)
            CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    }
    CHECK(isaac_vita_game_log_flush(ISAAC_VITA_GAME_LOG_SEVERITY_UNKNOWN) == 0);
    CHECK(disk_matches_expected() && s_disk_size == total);
    CHECK(snap().records == i && snap().bytes == total &&
          snap().logical_size == (int64_t)total && snap().fail == 0U &&
          snap().lost_bytes == 0U && snap().full_waits == 0U);
    CHECK(isaac_vita_game_log_tell() == (long)total);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_maximal_records(void)
{
    uint32_t i;
    uint32_t fits = RING / ISAAC_VITA_GAME_LOG_RECORD_MAX;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    /* The ring holds `fits` maximal records without waiting ... */
    for (i = 0U; i < fits; ++i) {
        CHECK(write_record(ISAAC_VITA_GAME_LOG_RECORD_MAX, i) ==
              ISAAC_VITA_GAME_LOG_RECORD_MAX);
        if (HAS_DEFERRED)
            CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    }
    CHECK(snap().full_waits == 0U && s_disk_size == 0U);
    /* ... the next one waits for the worker (serviced inside the wait). */
    g_isaac_vita_game_log_oracle_service_on_wait = 1;
    CHECK(write_record(ISAAC_VITA_GAME_LOG_RECORD_MAX, 99U) ==
          ISAAC_VITA_GAME_LOG_RECORD_MAX);
    CHECK(snap().full_waits == 1U && snap().appends[0] == 1U &&
          s_disk_size == fits * ISAAC_VITA_GAME_LOG_RECORD_MAX);
    CHECK(snap().worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY);
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    CHECK(disk_matches_expected());
    /* A record larger than the ring drains, then goes straight to disk. */
    {
        static unsigned char giant[RING + 5U];
        uint32_t k;

        for (k = 0U; k < sizeof giant; ++k)
            giant[k] = (unsigned char)(k * 3U);
        CHECK(write_text("before\n") == 7U);
        CHECK(isaac_vita_game_log_write(giant, sizeof giant, 1U) == 1U);
        expect_bytes(giant, sizeof giant);
        CHECK(disk_matches_expected() && snap().appends[4] == 1U);
        CHECK(isaac_vita_game_log_tell() == (long)s_expected_size);
        CHECK(!snap().synchronous);
    }
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_ring_full_self_rescue(void)
{
    uint32_t i;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    for (i = 0U; i < RING / 4000U; ++i)
        CHECK(write_record(4000U, i) == 4000U);
    CHECK(snap().used == (RING / 4000U) * 4000U);
    /* No worker service: the wait times out with nobody draining, so the
     * guest appends the ring itself and continues; the worker is not
     * declared failed for that. */
    g_isaac_vita_game_log_oracle_service_on_wait = 0;
    CHECK(write_record(4000U, 77U) == 4000U);
    CHECK(snap().full_waits == 1U && snap().appends[1] == 1U &&
          snap().worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY &&
          !snap().synchronous && s_log_wait == 0U);
    CHECK(s_disk_size == (RING / 4000U) * 4000U && snap().used == 4000U);
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    CHECK(disk_matches_expected());
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_stranded_worker_failed(void)
{
    isaac_vita_game_log_snapshot s;
    summary_fields f;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(isaac_vita_game_log_write("AAAA\n", 1U, 5U) == 5U);
    CHECK(isaac_vita_game_log_oracle_strand_one() == 1);
    CHECK(isaac_vita_game_log_write("BBBBBB\n", 1U, 7U) == 7U);
    /* Five 2 s windows without a completion: the stranded span is
     * abandoned, the rest is appended by the guest, the lane goes
     * synchronous, one wait receipt is logged after the lock is released. */
    g_isaac_vita_game_log_oracle_service_on_wait = 0;
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    s = snap();
    CHECK(s.worker_state == ISAAC_VITA_GAME_LOG_WORKER_FAILED &&
          s.synchronous && s.abandoned_bytes == 5U && s.generation == 1U &&
          s.appends[1] == 1U && s.waits == 1U && s.wait_us == 10000000U &&
          s.used == 0U && s.draining == 0U);
    CHECK(s_disk_size == 7U && memcmp(s_disk, "BBBBBB\n", 7U) == 0);
    CHECK(s_log_wait == 1U);
    CHECK(strstr(s_last_line, " state=abandoned span=5 pending=7 wait_us=10000000") != NULL);
    /* Synchronous forever: every write is a direct append, no wake. */
    CHECK(isaac_vita_game_log_write("CC\n", 1U, 3U) == 3U);
    CHECK(s_disk_size == 10U && memcmp(s_disk + 7U, "CC\n", 3U) == 0 &&
          snap().appends[4] == 1U && snap().records == 3U &&
          snap().logical_size == 15);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0 && s_disk_size == 10U);
    CHECK(g_isaac_vita_game_log_oracle_wakes == 1U);
    /* The worker returns late: it touches no cursor and retires. */
    CHECK(isaac_vita_game_log_oracle_complete_stranded() == 1);
    CHECK(snap().late == 1U && snap().appends[0] == 0U);
    CHECK(s_disk_size == 15U && memcmp(s_disk + 10U, "AAAA\n", 5U) == 0);
    CHECK(isaac_vita_game_log_close() == 0);
    /* Nothing is adopted after the worker failed. */
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 0);
    isaac_vita_game_log_shutdown();
    CHECK(parse_summary(s_last_summary, &f) && strcmp(f.state, "failed") == 0 &&
          f.abandoned_bytes == 5U && f.late == 1U && f.lost_bytes == 0U &&
          f.appends[4] == 1U);
    return 0;
}

static int case_stranded_during_write_wait(void)
{
    isaac_vita_game_log_snapshot s;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_record(RING - 100U, 1U) == RING - 100U);
    CHECK(isaac_vita_game_log_oracle_strand_one() == 1);
    CHECK(write_record(50U, 2U) == 50U);
    /* The ring is full behind a stranded worker: after the budget the
     * writer abandons the span, appends the 50 pending bytes and its own
     * record directly.  The abandoned span never reaches the disk from the
     * ring again. */
    s_expected_size = 0U;
    g_isaac_vita_game_log_oracle_service_on_wait = 0;
    CHECK(write_record(200U, 3U) == 200U);
    s = snap();
    CHECK(s.worker_state == ISAAC_VITA_GAME_LOG_WORKER_FAILED &&
          s.abandoned_bytes == RING - 100U && s.full_waits == 5U &&
          s.appends[1] == 1U && s.appends[4] == 1U && s.synchronous);
    CHECK(s_disk_size == 250U && s_log_wait == 1U);
    CHECK(memcmp(s_disk + 50U, s_record, 200U) == 0);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_append_failures(void)
{
    isaac_vita_game_log_snapshot s;
    unsigned i;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    /* One failed open: the span is retried on a fresh open and lands. */
    CHECK(write_text("retry-open\n") == 11U);
    s_fail_opens = 1U;
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    CHECK(disk_matches_expected() && snap().fail == 0U &&
          snap().lost_bytes == 0U && s_log_append == 1U &&
          strstr(s_last_line, " who=worker bytes=11 rc=0x00000000 retry=1") != NULL);
    /* A short write: the unwritten suffix follows on the same descriptor,
     * never the prefix again; that is not a retry and logs nothing. */
    CHECK(write_text("short-write-record\n") == 19U);
    s_short_write = 6U;
    {
        uint32_t writes_before = s_disk_writes;
        CHECK(isaac_vita_game_log_oracle_run_one() == 1);
        CHECK(s_disk_writes == writes_before + 2U);
    }
    CHECK(disk_matches_expected() && snap().fail == 0U && s_log_append == 1U);
    /* Persistent failure: dropped after one retry, ferror sticky, the next
     * flush answers EOF once, the lane stays live and keeps appending. */
    CHECK(write_text("lost\n") == 5U);
    s_fail_opens = 2U;
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    s_expected_size -= 5U;
    s = snap();
    CHECK(s.fail == 1U && s.lost_bytes == 5U && s.error && s.pending_eof &&
          s.used == 0U && disk_matches_expected());
    CHECK(isaac_vita_game_log_error() == 1);
    errno = 0;
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == EOF && errno == EIO);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    CHECK(isaac_vita_game_log_error() == 1);
    CHECK(write_text("after\n") == 6U);
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0 && disk_matches_expected());
    /* Guest-side failure: EOF from the WARN flush itself. */
    CHECK(write_text("guest-lost\n") == 11U);
    s_fail_writes = 2U;
    errno = 0;
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == EOF && errno == EIO);
    s_expected_size -= 11U;
    CHECK(disk_matches_expected() && snap().fail == 2U &&
          snap().lost_bytes == 16U && !snap().pending_eof);
    /* Failure receipts are bounded at eight per process. */
    for (i = 0U; i < 12U; ++i) {
        CHECK(write_text("x\n") == 2U);
        s_fail_opens = 2U;
        CHECK(isaac_vita_game_log_oracle_run_one() == 1);
        s_expected_size -= 2U;
    }
    CHECK(s_log_append == 8U && snap().fail == 14U && disk_matches_expected());
    CHECK(isaac_vita_game_log_close() == EOF);
    return 0;
}

static int case_tell_seek_release(void)
{
    long position = -1L;
    char mode[4] = "??";
    const char *path = NULL;
    isaac_vita_game_log_snapshot s;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "wb") == 1);
    CHECK(write_text("0123456789\n") == 11U);
    CHECK(isaac_vita_game_log_tell() == 11L);
    CHECK(isaac_vita_game_log_seek(0L, SEEK_END) == 0);
    CHECK(isaac_vita_game_log_seek(0L, SEEK_CUR) == 0);
    CHECK(isaac_vita_game_log_seek(11L, SEEK_SET) == 0);
    CHECK(s_disk_size == 0U);
    CHECK(isaac_vita_game_log_seek(5L, SEEK_SET) ==
          ISAAC_VITA_GAME_LOG_SEEK_DEMOTE);
    CHECK(isaac_vita_game_log_seek(-1L, SEEK_END) ==
          ISAAC_VITA_GAME_LOG_SEEK_DEMOTE);
    CHECK(isaac_vita_game_log_seek(1L, SEEK_CUR) ==
          ISAAC_VITA_GAME_LOG_SEEK_DEMOTE);
    CHECK(isaac_vita_game_log_seek(0L, 7) == ISAAC_VITA_GAME_LOG_SEEK_DEMOTE);
    s = snap();
    CHECK(s.coherence[5] == 7U && s.coherence[6] == 1U && s.used == 11U);
    /* release: drain, hand position/mode/path to the CRT, lane not live. */
    CHECK(isaac_vita_game_log_release(&position, mode, &path,
                                      ISAAC_VITA_GAME_LOG_RELEASE_SEEK) == 0);
    CHECK(position == 11L && strcmp(mode, "wb") == 0 && path != NULL &&
          strcmp(path, LOG_TXT) == 0 && disk_matches_expected());
    s = snap();
    CHECK(!s.live && s.demote == 1U && s.coherence[0] == 0U &&
          s.appends[1] == 1U);
    CHECK(!isaac_vita_game_log_owns_path(LOG_TXT));
    /* The CRT could not reopen: the lane is live again in place. */
    isaac_vita_game_log_rearm();
    CHECK(snap().live && snap().demote_fail == 1U &&
          isaac_vita_game_log_owns_path(LOG_TXT) &&
          isaac_vita_game_log_tell() == 11L);
    CHECK(write_text("more\n") == 5U);
    CHECK(isaac_vita_game_log_release(&position, mode, &path,
                                      ISAAC_VITA_GAME_LOG_RELEASE_OPEN) == 0);
    CHECK(position == 16L && disk_matches_expected() &&
          snap().coherence[0] == 1U && snap().demote == 2U);
    CHECK(isaac_vita_game_log_open(LOG_TXT, "a") == 1);
    CHECK(isaac_vita_game_log_tell() == 16L);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_sync_path_directory(void)
{
    isaac_vita_game_log_snapshot s;

    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("pending\n") == 8U);
    /* Other paths and other directories: no count, no drain. */
    isaac_vita_game_log_sync_path(SAVE_DIR "/options.ini",
                                  ISAAC_VITA_GAME_LOG_SYNC_REMOVE);
    isaac_vita_game_log_sync_path(SAVE_DIR "/log.txt2",
                                  ISAAC_VITA_GAME_LOG_SYNC_ACCESS);
    isaac_vita_game_log_sync_directory(ROOT "/Documents");
    isaac_vita_game_log_sync_directory(SAVE_DIR "/sub");
    isaac_vita_game_log_sync_directory(SAVE_DIR "/log.txt");
    CHECK(s_disk_size == 0U && snap().used == 8U);
    s = snap();
    CHECK(s.coherence[1] == 0U && s.coherence[2] == 0U &&
          s.coherence[3] == 0U && s.coherence[4] == 0U);
    /* Exact path: drain and count per caller. */
    CHECK(isaac_vita_game_log_owns_path(LOG_TXT));
    isaac_vita_game_log_sync_path(LOG_TXT, ISAAC_VITA_GAME_LOG_SYNC_REMOVE);
    CHECK(disk_matches_expected() && snap().coherence[1] == 1U &&
          snap().appends[1] == 1U);
    isaac_vita_game_log_sync_path(LOG_TXT, ISAAC_VITA_GAME_LOG_SYNC_ACCESS);
    isaac_vita_game_log_sync_path(LOG_TXT, ISAAC_VITA_GAME_LOG_SYNC_ATTRIB);
    CHECK(snap().coherence[2] == 1U && snap().coherence[3] == 1U &&
          snap().appends[1] == 1U);
    /* Exact directory, with and without the trailing slash. */
    CHECK(write_text("dir\n") == 4U);
    isaac_vita_game_log_sync_directory(SAVE_DIR);
    CHECK(disk_matches_expected() && snap().coherence[4] == 1U);
    CHECK(write_text("dir2\n") == 5U);
    isaac_vita_game_log_sync_directory(SAVE_DIR "/");
    CHECK(disk_matches_expected() && snap().coherence[4] == 2U &&
          snap().appends[1] == 3U);
    /* A stranded worker span is waited for first (serviced in the wait). */
    CHECK(write_text("strand\n") == 7U);
    CHECK(isaac_vita_game_log_oracle_strand_one() == 1);
    CHECK(write_text("tail\n") == 5U);
    g_isaac_vita_game_log_oracle_service_on_wait = 1;
    isaac_vita_game_log_sync_path(LOG_TXT, ISAAC_VITA_GAME_LOG_SYNC_ATTRIB);
    CHECK(disk_matches_expected() && snap().waits == 1U &&
          snap().appends[0] == 1U && snap().appends[1] == 4U);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

static int case_close_reuse(void)
{
    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("one\n") == 4U);
    CHECK(isaac_vita_game_log_close() == 0);
    CHECK(disk_matches_expected() && !snap().live && snap().used == 0U);
    CHECK(!isaac_vita_game_log_owns_path(LOG_TXT));
    errno = 0;
    CHECK(isaac_vita_game_log_write("x", 1U, 1U) == 0U && errno == EBADF);
    /* The same lane is reused; "w" truncates, "a" continues. */
    CHECK(isaac_vita_game_log_open(LOG_TXT, "a") == 1);
    CHECK(isaac_vita_game_log_tell() == 4L);
    CHECK(write_text("two\n") == 4U);
    CHECK(isaac_vita_game_log_close() == 0);
    CHECK(disk_matches_expected());
    s_expected_size = 0U;
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(s_disk_size == 0U && isaac_vita_game_log_tell() == 0L);
    CHECK(write_text("three\n") == 6U);
    CHECK(isaac_vita_game_log_close() == 0);
    CHECK(disk_matches_expected() && snap().records == 3U);
    /* Only one open receipt per open. */
    CHECK(s_log_open == 3U && s_logs == 3U);
    return 0;
}

static int case_abort_drain(void)
{
    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("[INFO] - dying\n") == 15U);
    CHECK(write_text("[ASSERT] - assertion\n") == 21U);
    /* The abort path appends the pending bytes and logs nothing. */
    isaac_vita_game_log_abort_drain();
    CHECK(disk_matches_expected() && snap().appends[3] == 1U &&
          snap().used == 0U && s_logs == 1U);
    isaac_vita_game_log_abort_drain();
    CHECK(snap().appends[3] == 1U);
    /* Behind a stranded worker: one wait step, then give up quietly. */
    CHECK(write_text("stranded\n") == 9U);
    CHECK(isaac_vita_game_log_oracle_strand_one() == 1);
    CHECK(write_text("rest\n") == 5U);
    g_isaac_vita_game_log_oracle_service_on_wait = 0;
    {
        uint64_t before = g_isaac_vita_game_log_oracle_now_us;
        isaac_vita_game_log_abort_drain();
        CHECK(g_isaac_vita_game_log_oracle_now_us - before == 2000000U);
    }
    CHECK(snap().appends[3] == 1U && snap().draining == 1U && s_logs == 1U &&
          snap().worker_state == ISAAC_VITA_GAME_LOG_WORKER_READY);
    /* With the worker completing inside the wait step, the rest lands. */
    g_isaac_vita_game_log_oracle_service_on_wait = 1;
    isaac_vita_game_log_abort_drain();
    CHECK(disk_matches_expected() && snap().appends[3] == 2U &&
          snap().appends[0] == 1U && s_logs == 1U);
    /* A failed abort append is counted like any other. */
    CHECK(write_text("fail\n") == 5U);
    s_fail_opens = 2U;
    isaac_vita_game_log_abort_drain();
    s_expected_size -= 5U;
    CHECK(disk_matches_expected() && snap().fail == 1U &&
          snap().lost_bytes == 5U && s_logs == 1U && s_log_append == 0U);
    CHECK(isaac_vita_game_log_close() == EOF);
    return 0;
}

static int case_shutdown_summary(void)
{
    summary_fields f;

    if (!HAS_DEFERRED)
        return 0;
    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("[INFO] - one\n") == 13U);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    CHECK(write_text("[WARN] - two\n") == 13U);
    CHECK(isaac_vita_game_log_flush(SYNC_SEVERITY) == 0);
    CHECK(write_text("[INFO] - three\n") == 15U);
    CHECK(isaac_vita_game_log_flush(DEFERRED_SEVERITY) == 0);
    CHECK(isaac_vita_game_log_tell() == 41L);
    /* Exit: pending bytes are appended (counted as exit), the worker stops,
     * one summary is logged; a second shutdown logs nothing; no lane is
     * adopted afterwards and the live lane is synchronous. */
    isaac_vita_game_log_shutdown();
    CHECK(disk_matches_expected() && s_log_summary == 1U);
    CHECK(parse_summary(s_last_summary, &f));
    CHECK(strcmp(f.bid, "perf:game-log-oracle") == 0);
    CHECK(strcmp(f.state, "stopped") == 0);
    CHECK(f.records == 3U && f.bytes == 41U);
    CHECK(f.flush[DEFERRED_SEVERITY] == 2U && f.flush[SYNC_SEVERITY] == 1U &&
          f.flush[5] == 0U);
    CHECK(f.appends[0] == 0U && f.appends[1] == 1U && f.appends[2] == 1U &&
          f.appends[3] == 0U && f.appends[4] == 0U);
    CHECK(f.io_us[2] > 0U && f.io_us[3] > 0U && f.io_us[0] == 0U);
    /* High-water: the two records before the WARN flush (13 + 13). */
    CHECK(f.wait[0] == 0U && f.ring[0] == 26U && f.ring[1] == 0U);
    CHECK(f.coherence[6] == 1U && f.demote == 0U && f.late == 0U &&
          f.lost_bytes == 0U && f.abandoned_bytes == 0U && f.fail == 0U);
    CHECK(snap().worker_state == ISAAC_VITA_GAME_LOG_WORKER_STOPPED &&
          snap().synchronous && snap().live);
    isaac_vita_game_log_shutdown();
    CHECK(s_log_summary == 1U);
    CHECK(write_text("late line\n") == 10U);
    CHECK(disk_matches_expected() && snap().appends[4] == 1U);
    CHECK(isaac_vita_game_log_close() == 0);
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 0);
    isaac_vita_game_log_rearm();
    CHECK(!snap().live);
    /* Shutdown without any lane: a cold summary. */
    reset_case();
    isaac_vita_game_log_shutdown();
    CHECK(parse_summary(s_last_summary, &f) && strcmp(f.state, "cold") == 0 &&
          f.records == 0U && s_disk_opens == 0U);
    /* Shutdown with an idle live lane and no pending bytes: no I/O. */
    reset_case();
    CHECK(isaac_vita_game_log_open(LOG_TXT, "w") == 1);
    CHECK(write_text("x\n") == 2U);
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    isaac_vita_game_log_shutdown();
    CHECK(s_disk_opens == 2U && parse_summary(s_last_summary, &f) &&
          f.appends[2] == 0U && strcmp(f.state, "stopped") == 0);
    return 0;
}

static int case_binary_bytes(void)
{
    unsigned char bytes[256];
    unsigned i;

    reset_case();
    for (i = 0U; i < 256U; ++i)
        bytes[i] = (unsigned char)i;
    CHECK(isaac_vita_game_log_open(LOG_TXT, "wb") == 1);
    CHECK(isaac_vita_game_log_write(bytes, 1U, 256U) == 256U);
    expect_bytes(bytes, 256U);
    CHECK(isaac_vita_game_log_write(bytes, 16U, 16U) == 16U);
    expect_bytes(bytes, 256U);
    CHECK(isaac_vita_game_log_write(bytes, 0U, 16U) == 0U);
    CHECK(isaac_vita_game_log_write(bytes, 16U, 0U) == 0U);
    CHECK(isaac_vita_game_log_write(bytes, 1U, 1U) == 1U);
    expect_bytes(bytes, 1U);
    CHECK(isaac_vita_game_log_flush(ISAAC_VITA_GAME_LOG_SEVERITY_UNKNOWN) == 0);
    CHECK(disk_matches_expected() && s_disk_size == 513U &&
          snap().records == 3U && snap().bytes == 513U);
    CHECK(isaac_vita_game_log_close() == 0);
    return 0;
}

int main(void)
{
    static int (*const cases[])(void) = {
        case_eligibility,
        case_open_modes,
        case_start_failure,
        case_info_flow,
        case_warn_flow,
        case_durable_matrix,
        case_fifo_wrap,
        case_maximal_records,
        case_ring_full_self_rescue,
        case_stranded_worker_failed,
        case_stranded_during_write_wait,
        case_append_failures,
        case_tell_seek_release,
        case_sync_path_directory,
        case_close_reuse,
        case_abort_drain,
        case_shutdown_summary,
        case_binary_bytes,
    };
    size_t i;

    for (i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        s_expected_size = 0U;
        if (cases[i]() != 0)
            return 1;
    }
    printf("Vita game log write-behind host oracle: PASS (ring %u KiB, "
           "durable %u, %u cases)\n",
           (unsigned)ISAAC_VITA_GAME_LOG_ASYNC_RING_KB, (unsigned)DURABLE,
           (unsigned)(sizeof cases / sizeof cases[0]));
    return 0;
}
