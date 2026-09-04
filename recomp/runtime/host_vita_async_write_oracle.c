/* Host oracle for the asynchronous save writer.
 *
 * The oracle owns the "worker": queued jobs are performed only when the test
 * calls isaac_vita_async_write_oracle_run_one(), and the module's own waits
 * perform jobs the same way.  Native I/O is a recording double so ordering,
 * read-your-writes and fallback lanes are verified without a filesystem. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_vita_async_write.h"

#define SAVE_ROOT "ux0:/data/isaacr001/Documents/My Games/Binding of Isaac Repentance/"
#define GAMESTATE SAVE_ROOT "gamestate1.dat"
#define PERSISTENT SAVE_ROOT "persistentgamedata1.dat"

typedef struct oracle_file {
    char path[ISAAC_VITA_ASYNC_WRITE_PATH_MAX + 1U];
    uint8_t *data;
    uint32_t size;
    uint32_t writes;
} oracle_file;

static oracle_file s_files[8];
static unsigned s_file_count;
static unsigned s_open_calls;
static unsigned s_write_calls;
static unsigned s_close_calls;
static unsigned s_live_descriptors;
static uint8_t s_arena[ISAAC_VITA_ASYNC_WRITE_ARENA_BYTES];
static unsigned s_arena_calls;
static unsigned s_fail_next_open;
static unsigned s_worker_available = 1U;
static uint64_t s_clock_us;
static uint32_t s_write_chunk_limit;
static char s_order[64];
static size_t s_order_count;
static char s_last_log[512];
static unsigned s_log_calls;

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "async-write oracle failed at line %u: %s\n",
            line, expression);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    ++s_log_calls;
}

static oracle_file *oracle_find(const char *path)
{
    unsigned index;

    for (index = 0U; index < s_file_count; ++index) {
        if (strcmp(s_files[index].path, path) == 0)
            return &s_files[index];
    }
    return NULL;
}

static void order_note(char value)
{
    if (s_order_count + 1U < sizeof s_order) {
        s_order[s_order_count++] = value;
        s_order[s_order_count] = '\0';
    }
}

int32_t isaac_vita_async_write_oracle_open(const char *path)
{
    oracle_file *file;

    ++s_open_calls;
    if (s_fail_next_open) {
        --s_fail_next_open;
        return (int32_t)0x80010002;
    }
    file = oracle_find(path);
    if (!file) {
        if (s_file_count == sizeof s_files / sizeof s_files[0])
            return -1;
        file = &s_files[s_file_count++];
        memcpy(file->path, path, strlen(path) + 1U);
    }
    /* SCE_O_TRUNC semantics. */
    free(file->data);
    file->data = NULL;
    file->size = 0U;
    ++s_live_descriptors;
    order_note((char)('a' + (file - s_files)));
    return (int32_t)(file - s_files) + 100;
}

int32_t isaac_vita_async_write_oracle_write(int32_t descriptor,
                                            const void *buffer,
                                            uint32_t size)
{
    oracle_file *file;
    uint8_t *grown;

    ++s_write_calls;
    if (descriptor < 100 || (unsigned)(descriptor - 100) >= s_file_count)
        return (int32_t)0x80010009;
    file = &s_files[descriptor - 100];
    if (s_write_chunk_limit && size > s_write_chunk_limit)
        size = s_write_chunk_limit;
    grown = (uint8_t *)realloc(file->data, file->size + size + 1U);
    if (!grown)
        return -1;
    file->data = grown;
    memcpy(file->data + file->size, buffer, size);
    file->size += size;
    ++file->writes;
    return (int32_t)size;
}

int32_t isaac_vita_async_write_oracle_close(int32_t descriptor)
{
    ++s_close_calls;
    if (descriptor < 100 || (unsigned)(descriptor - 100) >= s_file_count)
        return (int32_t)0x80010009;
    --s_live_descriptors;
    return 0;
}

uint8_t *isaac_vita_async_write_oracle_arena(void)
{
    ++s_arena_calls;
    return s_arena;
}

uint64_t isaac_vita_async_write_oracle_time_us(void)
{
    s_clock_us += 250U;
    return s_clock_us;
}

int isaac_vita_async_write_oracle_worker_start(void)
{
    return s_worker_available != 0U;
}

static int fill(uint8_t *buffer, uint32_t size, uint32_t seed)
{
    uint32_t index;

    for (index = 0U; index < size; ++index)
        buffer[index] = (uint8_t)(seed + index * 7U);
    return 1;
}

static int run(void)
{
    isaac_vita_async_write_image image;
    isaac_vita_async_write_image second;
    isaac_vita_async_write_snapshot snapshot;
    static uint8_t save[68741];
    static uint8_t persistent[5996];
    oracle_file *file;
    unsigned index;

    /* Eligibility: exact modes, save root, .dat suffix, bounded length. */
    CHECK(isaac_vita_async_write_eligible(GAMESTATE, "wb"));
    CHECK(isaac_vita_async_write_eligible(PERSISTENT, "w"));
    CHECK(!isaac_vita_async_write_eligible(GAMESTATE, "rb"));
    CHECK(!isaac_vita_async_write_eligible(GAMESTATE, "r"));
    CHECK(!isaac_vita_async_write_eligible(GAMESTATE, "ab"));
    CHECK(!isaac_vita_async_write_eligible(GAMESTATE, "w+"));
    CHECK(!isaac_vita_async_write_eligible(SAVE_ROOT "log.txt", "w"));
    CHECK(!isaac_vita_async_write_eligible(SAVE_ROOT "options.ini", "w"));
    CHECK(!isaac_vita_async_write_eligible(SAVE_ROOT ".dat", "wb"));
    CHECK(!isaac_vita_async_write_eligible(SAVE_ROOT "x/.dat", "wb"));
    CHECK(!isaac_vita_async_write_eligible(
        "ux0:/data/isaacr001/resources/packed/graphics.a", "rb"));
    CHECK(!isaac_vita_async_write_eligible(
        "ux0:/data/isaacr001/kage_mount_points.dat", "wb"));
    CHECK(!isaac_vita_async_write_eligible(NULL, "wb"));
    CHECK(!isaac_vita_async_write_eligible(GAMESTATE, NULL));
    {
        static char long_path[ISAAC_VITA_ASYNC_WRITE_PATH_MAX + 16U];
        memcpy(long_path, SAVE_ROOT, sizeof SAVE_ROOT - 1U);
        memset(long_path + sizeof SAVE_ROOT - 1U, 'g',
               sizeof long_path - sizeof SAVE_ROOT - 4U);
        memcpy(long_path + sizeof long_path - 5U, ".dat", 5U);
        CHECK(!isaac_vita_async_write_eligible(long_path, "wb"));
    }

    /* Nothing is queued before the first image: waits return at once. */
    isaac_vita_async_write_sync_path(GAMESTATE);
    isaac_vita_async_write_drain();
    CHECK(s_open_calls == 0U && s_log_calls == 0U);

    /* One measured gamestate rewrite: one fwrite, fclose queues one job,
     * the worker performs exactly one open/write/close. */
    fill(save, sizeof save, 3U);
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(image.live && image.capacity == ISAAC_VITA_ASYNC_WRITE_LANE_BYTES &&
          image.data == s_arena && image.lane == 0U && s_arena_calls == 1U);
    CHECK(isaac_vita_async_write_image_tell(&image) == 0L);
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, sizeof save) ==
          sizeof save);
    CHECK(image.size == sizeof save);
    CHECK(isaac_vita_async_write_image_tell(&image) == (long)sizeof save);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    CHECK(!image.live && image.data == NULL);
    CHECK(s_open_calls == 0U);
    CHECK(isaac_vita_async_write_oracle_pending() == 1U);
    CHECK(isaac_vita_async_write_oracle_run_one() == 1);
    CHECK(isaac_vita_async_write_oracle_run_one() == 0);
    CHECK(s_open_calls == 1U && s_write_calls == 1U && s_close_calls == 1U);
    file = oracle_find(GAMESTATE);
    CHECK(file && file->size == sizeof save &&
          memcmp(file->data, save, sizeof save) == 0);
    CHECK(strstr(s_last_log, "[isaac-asyncsave] job") != NULL);
    CHECK(strstr(s_last_log, "file=gamestate1.dat bytes=68741") != NULL);
    CHECK(strstr(s_last_log, "rc=0x00000000") != NULL);

    /* Closed image: every operation reports EBADF without touching memory. */
    errno = 0;
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 1U) == 0U &&
          errno == EBADF);
    errno = 0;
    CHECK(isaac_vita_async_write_image_seek(&image, 0L, SEEK_SET) == -1 &&
          errno == EBADF);
    errno = 0;
    CHECK(isaac_vita_async_write_image_tell(&image) == -1L && errno == EBADF);
    errno = 0;
    CHECK(isaac_vita_async_write_image_close(&image) == EOF && errno == EBADF);

    /* Read-your-writes: a second rewrite queued, then a path sync performs
     * it before returning; the file holds the newer bytes and the buffer of
     * the finished job is reclaimed on the guest thread. */
    fill(save, sizeof save, 11U);
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(isaac_vita_async_write_image_write(&image, save, sizeof save, 1U) ==
          1U);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    CHECK(isaac_vita_async_write_oracle_pending() == 1U);
    isaac_vita_async_write_sync_path(PERSISTENT);
    CHECK(isaac_vita_async_write_oracle_pending() == 1U);
    isaac_vita_async_write_sync_path(GAMESTATE);
    CHECK(isaac_vita_async_write_oracle_pending() == 0U);
    CHECK(file->size == sizeof save && memcmp(file->data, save, sizeof save) == 0);
    isaac_vita_async_write_snapshot_read(&snapshot);
    CHECK(snapshot.jobs_queued == 2U && snapshot.jobs_completed == 2U);
    CHECK(snapshot.sync_waits == 1U && snapshot.sync_wait_us > 0U);
    CHECK(snapshot.bytes_written == 2U * sizeof save);

    /* FIFO across two paths, then drain. */
    fill(save, sizeof save, 21U);
    fill(persistent, sizeof persistent, 5U);
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(isaac_vita_async_write_image_open(&second, PERSISTENT));
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, sizeof save) ==
          sizeof save);
    CHECK(isaac_vita_async_write_image_write(
              &second, persistent, 1U, sizeof persistent) == sizeof persistent);
    CHECK(isaac_vita_async_write_image_close(&second) == 0);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    CHECK(isaac_vita_async_write_oracle_pending() == 2U);
    s_order_count = 0U;
    s_order[0] = '\0';
    isaac_vita_async_write_drain();
    CHECK(isaac_vita_async_write_oracle_pending() == 0U);
    CHECK(strcmp(s_order, "ba") == 0);
    CHECK(oracle_find(PERSISTENT) &&
          oracle_find(PERSISTENT)->size == sizeof persistent &&
          memcmp(oracle_find(PERSISTENT)->data, persistent,
                 sizeof persistent) == 0);
    CHECK(memcmp(file->data, save, sizeof save) == 0);
    isaac_vita_async_write_snapshot_read(&snapshot);
    CHECK(snapshot.peak_queue == 2U);

    /* Seek semantics: overwrite inside, sparse zero fill past the end. */
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(isaac_vita_async_write_image_write(&image, "ABCDEFGH", 1U, 8U) == 8U);
    CHECK(isaac_vita_async_write_image_seek(&image, 2L, SEEK_SET) == 0);
    CHECK(isaac_vita_async_write_image_write(&image, "xy", 1U, 2U) == 2U);
    CHECK(isaac_vita_async_write_image_tell(&image) == 4L);
    CHECK(image.size == 8U);
    CHECK(isaac_vita_async_write_image_seek(&image, -2L, SEEK_END) == 0);
    CHECK(isaac_vita_async_write_image_tell(&image) == 6L);
    CHECK(isaac_vita_async_write_image_seek(&image, 6L, SEEK_CUR) == 0);
    CHECK(isaac_vita_async_write_image_tell(&image) == 12L);
    CHECK(isaac_vita_async_write_image_write(&image, "Z", 1U, 1U) == 1U);
    CHECK(image.size == 13U);
    errno = 0;
    CHECK(isaac_vita_async_write_image_seek(&image, -1L, SEEK_SET) == -1 &&
          errno == EINVAL);
    errno = 0;
    CHECK(isaac_vita_async_write_image_seek(&image, 0L, 7) == -1 &&
          errno == EINVAL);
    CHECK(isaac_vita_async_write_image_write(&image, "Q", 0U, 4U) == 0U);
    CHECK(isaac_vita_async_write_image_write(&image, "Q", 4U, 0U) == 0U);
    CHECK(!image.error);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    isaac_vita_async_write_drain();
    CHECK(file->size == 13U &&
          memcmp(file->data, "ABxyEFGH\0\0\0\0Z", 13U) == 0);

    /* Short native writes are completed by the loop, not reported. */
    s_write_chunk_limit = 1000U;
    s_write_calls = 0U;
    fill(persistent, sizeof persistent, 9U);
    CHECK(isaac_vita_async_write_image_open(&image, PERSISTENT));
    CHECK(isaac_vita_async_write_image_write(
              &image, persistent, 1U, sizeof persistent) == sizeof persistent);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    isaac_vita_async_write_drain();
    CHECK(s_write_calls == 6U);
    CHECK(memcmp(oracle_find(PERSISTENT)->data, persistent,
                 sizeof persistent) == 0);
    s_write_chunk_limit = 0U;

    /* Native open failure is counted and logged, never faulted. */
    s_fail_next_open = 1U;
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 16U) == 16U);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    isaac_vita_async_write_drain();
    isaac_vita_async_write_snapshot_read(&snapshot);
    CHECK(snapshot.jobs_failed == 1U);
    CHECK(strstr(s_last_log, "rc=0x80010002") != NULL);
    CHECK(file->size == 13U);

    /* A write past the lane flags the image and fclose refuses to overwrite
     * the older save; the lane is released again. */
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(isaac_vita_async_write_image_seek(
              &image, (long)ISAAC_VITA_ASYNC_WRITE_LANE_BYTES - 2L,
              SEEK_SET) == 0);
    errno = 0;
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 3U) == 0U &&
          errno == EFBIG);
    CHECK(image.error);
    errno = 0;
    CHECK(isaac_vita_async_write_image_seek(
              &image, (long)ISAAC_VITA_ASYNC_WRITE_LANE_BYTES + 1L,
              SEEK_SET) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(isaac_vita_async_write_image_close(&image) == EOF && errno == EIO);
    CHECK(!image.live && image.data == NULL);
    s_open_calls = 0U;
    isaac_vita_async_write_drain();
    CHECK(s_open_calls == 0U && file->size == 13U);
    isaac_vita_async_write_snapshot_read(&snapshot);
    CHECK(snapshot.images_closed_with_error == 1U);

    /* Lane exhaustion: with every lane held by a queued job, a further open
     * reports "not live" so the CRT keeps the native FILE; once the queue
     * drains, lanes are reusable and the arena was reserved exactly once. */
    for (index = 0U; index < ISAAC_VITA_ASYNC_WRITE_LANE_COUNT; ++index) {
        CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
        CHECK(image.lane == index);
        save[0] = (uint8_t)index;
        CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 4U) == 4U);
        CHECK(isaac_vita_async_write_image_close(&image) == 0);
    }
    CHECK(isaac_vita_async_write_oracle_pending() ==
          ISAAC_VITA_ASYNC_WRITE_LANE_COUNT);
    CHECK(!isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(!image.live);
    isaac_vita_async_write_snapshot_read(&snapshot);
    CHECK(snapshot.images_lane_exhausted == 1U &&
          snapshot.images_arena_failed == 0U);
    isaac_vita_async_write_drain();
    CHECK(isaac_vita_async_write_oracle_pending() == 0U);
    CHECK(file->size == 4U && file->data[0] == 3U);
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(image.lane == 0U && s_arena_calls == 1U);
    save[0] = 0xEEU;
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 4U) == 4U);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);

    /* Queue saturation is impossible while lanes bound the queue, but the
     * synchronous lane still exists: with no worker every close writes on
     * the guest thread after the older same-path jobs, preserving order. */
    isaac_vita_async_write_drain();
    CHECK(file->data[0] == 0xEEU);

    /* Shutdown drains, stops accepting work (later closes go synchronous)
     * and publishes exactly one summary. */
    CHECK(isaac_vita_async_write_image_open(&image, PERSISTENT));
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 4U) == 4U);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    CHECK(isaac_vita_async_write_oracle_pending() == 1U);
    isaac_vita_async_write_shutdown();
    CHECK(isaac_vita_async_write_oracle_pending() == 0U);
    CHECK(strstr(s_last_log, "[isaac-asyncsave] summary") != NULL);
    CHECK(strstr(s_last_log, "worker=3") != NULL);
    isaac_vita_async_write_snapshot_read(&snapshot);
    CHECK(snapshot.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_STOPPED);
    CHECK(isaac_vita_async_write_image_open(&image, PERSISTENT));
    CHECK(isaac_vita_async_write_image_write(&image, save, 1U, 4U) == 4U);
    s_open_calls = 0U;
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    CHECK(s_open_calls == 1U);
    isaac_vita_async_write_shutdown();
    CHECK(s_live_descriptors == 0U);
    /* Every lane is free again: a further open takes lane 0. */
    CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
    CHECK(image.lane == 0U);
    CHECK(isaac_vita_async_write_image_close(&image) == 0);
    isaac_vita_async_write_shutdown();

    /* ---- 65,943-byte save: read-your-writes across the 64 KiB boundary ----
     * The device hang appeared with a gamestate1.dat of 65,943 bytes (the
     * largest image ever queued).  Exercise the exact path every intercepted
     * read entry point funnels through -- isaac_vita_async_write_sync_path for
     * fopen/_access/GetFileAttributesA and isaac_vita_async_write_drain for
     * FindFirstFileW -- with the worker both idle and, below, wedged. */
    isaac_vita_async_write_oracle_reset();
    {
        static uint8_t big[65943];
        oracle_file *big_file;

        fill(big, sizeof big, 0x5AU);
        CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
        CHECK(image.capacity == ISAAC_VITA_ASYNC_WRITE_LANE_BYTES);
        /* Whole-buffer write and a chunked write both land intact. */
        CHECK(isaac_vita_async_write_image_write(&image, big, 1U, sizeof big) ==
              sizeof big);
        CHECK(image.size == sizeof big);
        CHECK(isaac_vita_async_write_image_tell(&image) == (long)sizeof big);
        CHECK(isaac_vita_async_write_image_close(&image) == 0);
        CHECK(isaac_vita_async_write_oracle_pending() == 1U);
        /* Worker idle: the read-your-writes wait drains the job before the
         * would-be native open observes the file (sync_path is what fopen,
         * _access and GetFileAttributesA call). */
        isaac_vita_async_write_sync_path(GAMESTATE);
        CHECK(isaac_vita_async_write_oracle_pending() == 0U);
        big_file = oracle_find(GAMESTATE);
        CHECK(big_file && big_file->size == sizeof big &&
              memcmp(big_file->data, big, sizeof big) == 0);
        CHECK(strstr(s_last_log, "bytes=65943") != NULL &&
              strstr(s_last_log, "rc=0x00000000") != NULL);

        /* Same size, but the worker is wedged mid-write (RUNNING, never
         * completing).  Before the fix the wait looped on the event flag
         * forever (0 % CPU); it must now bound out, mark the worker failed and
         * return.  drain() (FindFirstFileW) must behave the same. */
        fill(big, sizeof big, 0x33U);
        CHECK(isaac_vita_async_write_image_open(&image, GAMESTATE));
        CHECK(isaac_vita_async_write_image_write(&image, big, 1U, sizeof big) ==
              sizeof big);
        CHECK(isaac_vita_async_write_image_close(&image) == 0);
        CHECK(isaac_vita_async_write_oracle_pending() == 1U);
        CHECK(isaac_vita_async_write_oracle_strand_one() == 1);
        CHECK(isaac_vita_async_write_oracle_pending() == 1U);
        s_open_calls = 0U;
        isaac_vita_async_write_sync_path(GAMESTATE);
        /* Returned (not hung).  The stranded job was not completed and the
         * previous on-disk image is untouched. */
        isaac_vita_async_write_snapshot_read(&snapshot);
        CHECK(snapshot.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED);
        CHECK(s_open_calls == 0U);
        CHECK(oracle_find(GAMESTATE)->size == sizeof big &&
              memcmp(oracle_find(GAMESTATE)->data, big, sizeof big) != 0);
        CHECK(strstr(s_last_log, "state=abandoned") != NULL);
        /* A dead worker keeps returning promptly rather than paying the
         * ceiling on every later probe. */
        isaac_vita_async_write_drain();
        isaac_vita_async_write_snapshot_read(&snapshot);
        CHECK(snapshot.worker_state == ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED);
    }
    isaac_vita_async_write_oracle_reset();
    return 0;
}

int main(void)
{
    int result = run();

    if (result)
        return result;
    puts("Vita asynchronous save writer oracle: PASS");
    return 0;
}
