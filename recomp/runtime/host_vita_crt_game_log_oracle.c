/* Hostile executable oracle for the CRT side of the game log write-behind
 * lane.  Include the production CRT TU so the test exercises the exact
 * FILE-token dispatch (fopen/fwrite/fseek/ftell/_fileno/_get_osfhandle/
 * fflush/fread/vfprintf/fclose) the frozen guest drives, with the lane module
 * linked in its host oracle configuration (the worker appends only when this
 * file performs a pass).  Built twice by recomp/vita/test_game_log_async.sh:
 * with and without -DISAAC_VITA_GAME_LOG_BATCH=1, so the batch classifier is
 * shown inert for the lane token. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest.h"

static FILE *oracle_native_fopen(const char *path, const char *mode);

#define ISAAC_VITA_ARCHIVE_FILE_CACHE 1
#define ISAAC_VITA_GAME_LOG_ASYNC 1
#define ISAAC_VITA_GAME_LOG_ORACLE 1
#define ISAAC_VITA_CRT_QSORT_HOST_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX INT32_MAX
#define vita_crt_game_log_native_fopen(path, mode) \
    oracle_native_fopen((path), (mode))
#include "host_vita_crt.c"

#define ROOT "ux0:/data/isaacr001"
#define SAVE_DIR ROOT "/Documents/My Games/Binding of Isaac Repentance"
#define LOG_TXT SAVE_DIR "/log.txt"
#define GAMESTATE SAVE_DIR "/gamestate1.dat"
#define SAVEDATAPATH ROOT "/savedatapath.txt"
#define DURABLE ISAAC_VITA_GAME_LOG_ASYNC_DURABLE_SEVERITY

static char s_guest_mode_w[] = "w";
static char s_guest_mode_wb[] = "wb";
static char s_guest_mode_a[] = "a";
static char s_guest_mode_rb[] = "rb";
static char s_guest_format[] = "%d-%s";
static char s_guest_word[] = "ok";
_Alignas(16) static uint32_t s_guest_stack[64];
_Alignas(16) static uint32_t s_guest_varargs[4];
_Alignas(16) static unsigned char s_guest_record[10240];
_Alignas(16) static unsigned char s_guest_output[16];
static const char *s_mapped_path;
static uint32_t s_native_fopen_calls;
static int s_native_open_succeeds;
static uint32_t s_reopen_calls;
static int s_reopen_fails;
static char s_reopen_mode[8];
static char s_reopen_path[ISAAC_VITA_GAME_LOG_PATH_MAX + 1U];
static uint32_t s_diag_logs;
static uint32_t s_summary_logs;
static char s_last_log[1024];

/* lane oracle doubles */
static unsigned char s_disk[1U << 20];
static uint32_t s_disk_size;
static char s_disk_path[ISAAC_VITA_GAME_LOG_PATH_MAX + 1U];
static uint32_t s_disk_opens;
static uint32_t s_disk_trunc_opens;
static uint32_t s_fail_opens;

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "crt game-log oracle failed at line %u: %s\n",
            line, expression);
    if (s_last_log[0])
        fprintf(stderr, "  last receipt: %s\n", s_last_log);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

void guest_fault(CPU *__restrict cpu, uint32_t address, const char *what)
{
    if (!cpu->fault) {
        cpu->fault = what;
        cpu->fault_addr = address;
    }
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    ++s_diag_logs;
    if (strncmp(s_last_log, "[isaac-gamelog] summary ", 24U) == 0)
        ++s_summary_logs;
}

int isaac_vita_startup_map_path(CPU *__restrict cpu, uint32_t guest_path,
                                char *native_path, uint32_t capacity)
{
    size_t length = strlen(s_mapped_path);

    (void)cpu;
    (void)guest_path;
    if (capacity <= length)
        return 0;
    memcpy(native_path, s_mapped_path, length + 1U);
    return 1;
}

/* Archive cache doubles: the native lane fails with EACCES unless a case
 * asks for one real host FILE (the table-full fault path needs an opened
 * stream), and the raw ENOMEM fallback is never taken. */
int isaac_vita_archive_raw_fallback_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY])
{
    (void)native_path;
    (void)mode;
    key[0] = '\0';
    return 0;
}

FILE *isaac_vita_archive_cache_open(isaac_vita_archive_cache_file *file,
                                    const char *native_path,
                                    const char *mode, int *cache_hit)
{
    (void)native_path;
    (void)mode;
    ++s_native_fopen_calls;
    memset(file, 0, sizeof *file);
    if (cache_hit)
        *cache_hit = 0;
    if (s_native_open_succeeds) {
        s_native_open_succeeds = 0;
        file->stream = tmpfile();
        return file->stream;
    }
    errno = EACCES;
    return NULL;
}

int isaac_vita_archive_cache_force_discard(
    isaac_vita_archive_cache_file *file)
{
    if (file->stream)
        (void)fclose(file->stream);
    memset(file, 0, sizeof *file);
    return 0;
}

int isaac_vita_archive_cache_close(isaac_vita_archive_cache_file *file)
{
    FILE *stream;

    if (!file || !file->stream) {
        errno = EBADF;
        return EOF;
    }
    stream = file->stream;
    file->stream = NULL;
    return fclose(stream);
}

uint32_t isaac_vita_archive_diag_record(
    isaac_vita_archive_diag_event *event)
{
    (void)event;
    return 1U;
}

uint32_t isaac_vita_archive_diag_snapshot(
    isaac_vita_archive_diag_event *events, uint32_t capacity,
    uint32_t *latest_sequence)
{
    (void)events;
    (void)capacity;
    if (latest_sequence)
        *latest_sequence = 0U;
    return 0U;
}

int isaac_vita_archive_diag_format_event(
    char *line, size_t capacity, const char *build_id,
    const char *reason, const isaac_vita_archive_diag_event *event)
{
    int length;

    (void)build_id;
    (void)reason;
    (void)event;
    length = snprintf(line, capacity, "game-log-oracle");
    return length >= 0 && (size_t)length < capacity ? length : -1;
}

int32_t isaac_vita_crt_raw_archive_oracle_open(const char *path)
{
    (void)path;
    return (int32_t)UINT32_C(0x80010002);
}

int32_t isaac_vita_crt_raw_archive_oracle_get_size(
    int32_t descriptor, int64_t *size)
{
    (void)descriptor;
    (void)size;
    return -1;
}

int32_t isaac_vita_crt_raw_archive_oracle_pread(
    int32_t descriptor, void *buffer, uint32_t size, uint64_t offset)
{
    (void)descriptor;
    (void)buffer;
    (void)size;
    (void)offset;
    return -1;
}

int32_t isaac_vita_crt_raw_archive_oracle_close(int32_t descriptor)
{
    (void)descriptor;
    return -1;
}

/* The demotion reopen: a real host file opened in the exact native mode the
 * CRT hands newlib ("r+b" opens at 0 and reads; "ab" is write-only and opens
 * at the end), seeded with the disk double's bytes so overwrite placement
 * and readability match the device.  The path is unlinked at once; the
 * mkstemp descriptor stays open as the read-back handle of
 * reopened_content(). */
static char s_reopen_temp[] = "/tmp/isaac-gamelog-reopen-XXXXXX";
static int s_reopen_temp_fd = -1;

static void reopen_temp_discard(void)
{
    if (s_reopen_temp_fd >= 0) {
        (void)close(s_reopen_temp_fd);
        s_reopen_temp_fd = -1;
    }
}

static FILE *oracle_native_fopen(const char *path, const char *mode)
{
    static const char template[] = "/tmp/isaac-gamelog-reopen-XXXXXX";
    FILE *stream;
    int fd;

    ++s_reopen_calls;
    memcpy(s_reopen_path, path, strlen(path) + 1U);
    memcpy(s_reopen_mode, mode, strlen(mode) + 1U);
    if (s_reopen_fails) {
        s_reopen_fails = 0;
        return NULL;
    }
    reopen_temp_discard();
    memcpy(s_reopen_temp, template, sizeof template);
    fd = mkstemp(s_reopen_temp);
    if (fd < 0)
        return NULL;
    if (strcmp(path, s_disk_path) == 0 && s_disk_size != 0U &&
        write(fd, s_disk, s_disk_size) != (ssize_t)s_disk_size) {
        (void)close(fd);
        (void)unlink(s_reopen_temp);
        return NULL;
    }
    stream = fopen(s_reopen_temp, mode);
    (void)unlink(s_reopen_temp);
    if (!stream) {
        (void)close(fd);
        return NULL;
    }
    s_reopen_temp_fd = fd;
    return stream;
}

/* The bytes the demoted FILE has on disk, its own buffer flushed first.
 * UINT32_MAX when there is no reopened file or it exceeds the capacity. */
static uint32_t reopened_content(FILE *stream, unsigned char *out,
                                 uint32_t capacity)
{
    off_t size;
    ssize_t got;

    if (stream)
        (void)fflush(stream);
    if (s_reopen_temp_fd < 0)
        return UINT32_MAX;
    size = lseek(s_reopen_temp_fd, 0, SEEK_END);
    if (size < 0 || (uint64_t)size > capacity)
        return UINT32_MAX;
    got = pread(s_reopen_temp_fd, out, (size_t)size, 0);
    return got < 0 ? UINT32_MAX : (uint32_t)got;
}

int32_t isaac_vita_game_log_oracle_open(const char *path, int truncate,
                                        int append)
{
    (void)append;
    if (s_fail_opens) {
        --s_fail_opens;
        return (int32_t)UINT32_C(0x80010005);
    }
    memcpy(s_disk_path, path, strlen(path) + 1U);
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
    if (descriptor != 100 || s_disk_size + size > sizeof s_disk)
        return -1;
    memcpy(s_disk + s_disk_size, buffer, size);
    s_disk_size += size;
    g_isaac_vita_game_log_oracle_now_us += 1000U;
    return (int32_t)size;
}

int32_t isaac_vita_game_log_oracle_close(int32_t descriptor)
{
    return descriptor == 100 ? 0 : -1;
}

int64_t isaac_vita_game_log_oracle_size(int32_t descriptor)
{
    return descriptor == 100 ? (int64_t)s_disk_size : -1;
}

/* ---- guest-ABI drivers -------------------------------------------------- */

static uint32_t prepare_cdecl(CPU *cpu, const uint32_t *arguments,
                              uint32_t count)
{
    uint32_t esp = (uint32_t)(uintptr_t)&s_guest_stack[24];
    uint32_t index;

    memset(cpu, 0, sizeof *cpu);
    memset(s_guest_stack, 0, sizeof s_guest_stack);
    st32(esp, UINT32_C(0xaabbccdd));
    for (index = 0U; index < count; ++index)
        st32(esp + 4U + index * 4U, arguments[index]);
    cpu->esp = esp;
    cpu->stack_owner = cpu;
    cpu->stack_floor = (uint32_t)(uintptr_t)&s_guest_stack[0];
    cpu->stack_ceiling = (uint32_t)(uintptr_t)&s_guest_stack[64];
    cpu->stack_low_water = cpu->stack_ceiling;
    return esp;
}

static uint32_t guest_fopen(CPU *cpu, const char *mapped, char *guest_mode)
{
    uint32_t arguments[2] = { 1U, (uint32_t)(uintptr_t)guest_mode };
    uint32_t esp;

    s_mapped_path = mapped;
    esp = prepare_cdecl(cpu, arguments, 2U);
    errno = EBUSY;
    vita_crt_fopen(cpu);
    if (cpu->fault || cpu->esp != esp + 4U || errno != EBUSY)
        return UINT32_MAX;
    return cpu->eax;
}

static uint32_t guest_call1(CPU *cpu, void (*handler)(CPU *__restrict),
                            uint32_t argument)
{
    uint32_t esp = prepare_cdecl(cpu, &argument, 1U);

    errno = EBUSY;
    handler(cpu);
    if (cpu->fault || cpu->esp != esp + 4U || errno != EBUSY)
        return UINT32_MAX - 1U;
    return cpu->eax;
}

static uint32_t guest_call3(CPU *cpu, void (*handler)(CPU *__restrict),
                            uint32_t a, uint32_t b, uint32_t d)
{
    uint32_t arguments[3] = { a, b, d };
    uint32_t esp = prepare_cdecl(cpu, arguments, 3U);

    errno = EBUSY;
    handler(cpu);
    if (cpu->fault || cpu->esp != esp + 4U || errno != EBUSY)
        return UINT32_MAX - 1U;
    return cpu->eax;
}

static uint32_t guest_call4(CPU *cpu, void (*handler)(CPU *__restrict),
                            uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    uint32_t arguments[4] = { a, b, d, e };
    uint32_t esp = prepare_cdecl(cpu, arguments, 4U);

    errno = EBUSY;
    handler(cpu);
    if (cpu->fault || cpu->esp != esp + 4U || errno != EBUSY)
        return UINT32_MAX - 1U;
    return cpu->eax;
}

/* The frozen logger chain as imported fflush sees it: the adapter's return
 * word at ESP (0x005965c9), the FILE token at ESP+4, the logger's return
 * word at ESP+8 (0x0055e4fa) and the logger's severity argument at EBP+8. */
static uint32_t guest_fflush_chain(CPU *cpu, uint32_t token,
                                   uint32_t severity)
{
    uint32_t esp = (uint32_t)(uintptr_t)&s_guest_stack[24];
    uint32_t ebp = (uint32_t)(uintptr_t)&s_guest_stack[40];

    memset(cpu, 0, sizeof *cpu);
    memset(s_guest_stack, 0, sizeof s_guest_stack);
    st32(esp, ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA);
    st32(esp + 4U, token);
    st32(esp + 8U, ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA);
    st32(ebp + 8U, severity);
    cpu->esp = esp;
    cpu->ebp = ebp;
    cpu->stack_owner = cpu;
    cpu->stack_floor = (uint32_t)(uintptr_t)&s_guest_stack[0];
    cpu->stack_ceiling = (uint32_t)(uintptr_t)&s_guest_stack[64];
    cpu->stack_low_water = cpu->stack_ceiling;
    errno = EBUSY;
    vita_crt_fflush(cpu);
    if (cpu->fault || cpu->esp != esp + 4U || errno != EBUSY)
        return UINT32_MAX - 1U;
    return cpu->eax;
}

static uint32_t guest_vfprintf(CPU *cpu, uint32_t token, uint32_t value,
                               const char *word)
{
    uint32_t arguments[6] = {
        0U, 0U, token, (uint32_t)(uintptr_t)s_guest_format, 0U,
        (uint32_t)(uintptr_t)s_guest_varargs
    };
    uint32_t esp;

    s_guest_varargs[0] = value;
    s_guest_varargs[1] = (uint32_t)(uintptr_t)word;
    esp = prepare_cdecl(cpu, arguments, 6U);
    errno = EBUSY;
    vita_crt_stdio_common_vfprintf(cpu);
    if (cpu->fault || cpu->esp != esp + 4U || errno != EBUSY)
        return UINT32_MAX - 1U;
    return cpu->eax;
}

static isaac_vita_game_log_snapshot snap(void)
{
    isaac_vita_game_log_snapshot s;

    isaac_vita_game_log_snapshot_read(&s);
    return s;
}

static int batch_inert(void)
{
    isaac_vita_crt_log_batch_snapshot batch;
    int active = isaac_vita_crt_log_batch_get_snapshot(&batch);

#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    if (!active || s_log_batch_token != 0U)
        return 0;
    return batch.info_seen == 0U && batch.info_deferred == 0U &&
        batch.chain_rejects == 0U && batch.native_flush_calls == 0U &&
        batch.warn_forwarded == 0U && batch.error_forwarded == 0U &&
        batch.assert_forwarded == 0U;
#else
    return !active;
#endif
}

static void reset_case(void)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i)
        if (s_file_tokens[i].file.stream)
            (void)fclose(s_file_tokens[i].file.stream);
    memset(s_file_tokens, 0, sizeof s_file_tokens);
    reopen_temp_discard();
    s_game_log_index = -1;
    s_file_lock = 0U;
    isaac_vita_game_log_oracle_reset();
    memset(s_disk, 0, sizeof s_disk);
    s_disk_size = 0U;
    s_disk_path[0] = '\0';
    s_disk_opens = 0U;
    s_disk_trunc_opens = 0U;
    s_fail_opens = 0U;
    s_native_fopen_calls = 0U;
    s_native_open_succeeds = 0;
    s_reopen_calls = 0U;
    s_reopen_fails = 0;
    s_reopen_mode[0] = '\0';
    s_reopen_path[0] = '\0';
    s_diag_logs = 0U;
    s_summary_logs = 0U;
    s_last_log[0] = '\0';
    g_isaac_vita_crt_errno = 0;
}

static uint32_t record_address(void)
{
    return (uint32_t)(uintptr_t)s_guest_record;
}

static void fill_record(uint32_t length, uint32_t seed)
{
    uint32_t i;

    for (i = 0U; i < length; ++i)
        s_guest_record[i] = (unsigned char)(seed * 17U + i * 5U + 3U);
}

/* ---- cases -------------------------------------------------------------- */

static int run(void)
{
    CPU cpu;
    vita_crt_file_token *entry = &s_file_tokens[0];
    uint32_t token;
    uint32_t index;

    CHECK((uintptr_t)&s_guest_stack[64] <= UINT32_MAX &&
          (uintptr_t)&s_guest_record[sizeof s_guest_record] <= UINT32_MAX &&
          (uintptr_t)&s_guest_varargs[4] <= UINT32_MAX &&
          (uintptr_t)s_guest_mode_w <= UINT32_MAX &&
          (uintptr_t)s_guest_format <= UINT32_MAX &&
          (uintptr_t)s_guest_word <= UINT32_MAX &&
          (uintptr_t)&s_file_tokens[ISAAC_VITA_CRT_FILE_TOKEN_COUNT] <=
              UINT32_MAX);
    reset_case();

    /* 1. fopen "w" on log.txt: lane token, no native FILE, errno untouched,
     * one native truncating open through the lane. */
    g_isaac_vita_crt_errno = EINVAL;
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    CHECK(entry->game_log.live && !entry->file.stream &&
          !entry->raw_archive.live && entry->token == token &&
          s_game_log_index == 0 && s_native_fopen_calls == 0U &&
          g_isaac_vita_crt_errno == EINVAL && s_file_lock == 0U);
    CHECK(vita_crt_dynamic_file_is_live(entry) &&
          vita_crt_dynamic_stream(entry) == NULL);
    CHECK(s_disk_opens == 1U && s_disk_trunc_opens == 1U &&
          strcmp(s_disk_path, LOG_TXT) == 0 && snap().live);
    CHECK(isaac_vita_game_log_owns_path(LOG_TXT));

    /* 2. The lock bridge: _fileno publishes the lane pseudo descriptor. */
    CHECK(guest_call1(&cpu, vita_crt_fileno, token) ==
          ISAAC_VITA_CRT_GAME_LOG_FD_BASE);
    CHECK(isaac_vita_crt_osfhandle_is_owned(ISAAC_VITA_CRT_GAME_LOG_FD_BASE));
    CHECK(!isaac_vita_crt_osfhandle_is_owned(
              ISAAC_VITA_CRT_GAME_LOG_FD_BASE + 1U));
    CHECK(!isaac_vita_crt_osfhandle_is_owned(
              ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE));
    CHECK(guest_call1(&cpu, vita_crt_get_osfhandle,
                      ISAAC_VITA_CRT_GAME_LOG_FD_BASE) ==
          ISAAC_VITA_CRT_GAME_LOG_FD_BASE);

    /* 3. fwrite is a memcpy into the ring; tell/seek-to-end answer from the
     * tracked position without I/O. */
    fill_record(100U, 1U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 100U,
                      token) == 100U);
    CHECK(s_disk_size == 0U && snap().used == 100U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 100U);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 0U, (uint32_t)SEEK_END) ==
          0U);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 0U, (uint32_t)SEEK_CUR) ==
          0U);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 100U, (uint32_t)SEEK_SET) ==
          0U);
    CHECK(s_disk_opens == 1U && entry->game_log.live && s_reopen_calls == 0U);

    /* 4. INFO flush from the frozen chain: no storage I/O; the batch
     * classifier never sees the token. */
    if (DURABLE >= 1U) {
        CHECK(guest_fflush_chain(&cpu, token, 0U) == 0U);
        CHECK(s_disk_size == 0U && snap().flush[0] == 1U &&
              snap().appends[1] == 0U && g_isaac_vita_game_log_oracle_wakes == 1U);
        CHECK(guest_fflush_chain(&cpu, token, 0U) == 0U);
        CHECK(s_disk_size == 0U && snap().flush[0] == 2U);
    }
    CHECK(batch_inert());

    /* 5. WARN flush from the chain: the record is on disk before control
     * returns, appended by the guest. */
    CHECK(guest_fflush_chain(&cpu, token, 1U) == 0U);
    if (DURABLE <= 1U) {
        CHECK(s_disk_size == 100U && memcmp(s_disk, s_guest_record, 100U) == 0);
        CHECK(snap().appends[1] == 1U && snap().flush[1] == 1U);
    } else {
        CHECK(s_disk_size == 0U);
    }
    CHECK(batch_inert());
    /* An fflush from outside the chain (plain call) is always synchronous. */
    fill_record(20U, 2U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 4U, 5U,
                      token) == 5U);
    CHECK(guest_call1(&cpu, vita_crt_fflush, token) == 0U);
    CHECK(s_disk_size == 120U && memcmp(s_disk + 100U, s_guest_record, 20U) == 0);
    CHECK(snap().flush[4] == 1U && snap().used == 0U);
    /* fflush(NULL) with the lane live counts and never waits. */
    CHECK(guest_call1(&cpu, vita_crt_fflush, 0U) == 0U);
    CHECK(snap().flush[5] == 1U && s_disk_size == 120U);
    CHECK(batch_inert());

    /* 6. vfprintf formats straight into the lane. */
    CHECK(guest_vfprintf(&cpu, token, 42U, s_guest_word) == 5U);
    CHECK(snap().used == 5U && s_disk_size == 120U);
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    CHECK(s_disk_size == 125U && memcmp(s_disk + 120U, "42-ok", 5U) == 0);
    CHECK(snap().appends[0] == 1U);

    /* 7. fread on the lane: newlib's answer for a "w" stream. */
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_call4(&cpu, vita_crt_fread,
                      (uint32_t)(uintptr_t)s_guest_output, 1U, 4U, token) ==
          0U);
    CHECK(entry->game_log.demoted_error && g_isaac_vita_crt_errno == EBADF &&
          entry->game_log.live);
    /* The lane answers before any FILE is touched: its slot has none.  Pass
     * the slot's stream rather than a literal NULL, which gcc 13 flags
     * (-Wnonnull) on the inlined feof arm the lane never reaches. */
    CHECK(entry->file.stream == NULL);
    CHECK(vita_crt_file_ferror(entry->file.stream, 0) == 1);
    CHECK(vita_crt_file_feof(entry->file.stream, 0) == 0);

    /* 8. A worker failure surfaces once through the next flush. */
    fill_record(8U, 3U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 8U,
                      token) == 8U);
    s_fail_opens = 2U;
    CHECK(isaac_vita_game_log_oracle_run_one() == 1);
    CHECK(s_disk_size == 125U && snap().fail == 1U && snap().lost_bytes == 8U);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_fflush_chain(&cpu, token, 0U) == (uint32_t)EOF);
    CHECK(g_isaac_vita_crt_errno == EIO);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_fflush_chain(&cpu, token, 0U) == 0U);
    CHECK(g_isaac_vita_crt_errno == 0);

    /* 9. Demotion reopen failure: the seek fails with EIO, the lane stays. */
    s_reopen_fails = 1;
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 5U, (uint32_t)SEEK_SET) ==
          UINT32_MAX);
    CHECK(g_isaac_vita_crt_errno == EIO && s_reopen_calls == 1U &&
          entry->game_log.live && s_game_log_index == 0 && snap().live &&
          snap().demote == 1U && snap().demote_fail == 1U);

    /* 10. Demotion via fseek(5, SEEK_SET): drained, "r+b" on the same path,
     * the seek performed on the new FILE, fread still EBADF, later fwrite
     * goes to the FILE, fclose closes it and frees the slot. */
    fill_record(3U, 4U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 3U,
                      token) == 3U);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 5U, (uint32_t)SEEK_SET) ==
          0U);
    CHECK(s_disk_size == 128U && memcmp(s_disk + 125U, s_guest_record, 3U) == 0);
    CHECK(s_reopen_calls == 2U && strcmp(s_reopen_mode, "r+b") == 0 &&
          strcmp(s_reopen_path, LOG_TXT) == 0);
    CHECK(!entry->game_log.live && entry->game_log.demoted_write_only &&
          entry->file.stream != NULL && entry->token == token &&
          s_game_log_index == -1 && !snap().live && snap().demote == 2U &&
          snap().coherence[5] == 5U);
    CHECK(vita_crt_dynamic_file_is_live(entry));
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 5U);
    CHECK(guest_call1(&cpu, vita_crt_fileno, token) ==
          (uint32_t)fileno(entry->file.stream));
    CHECK(!isaac_vita_crt_osfhandle_is_owned(ISAAC_VITA_CRT_GAME_LOG_FD_BASE));
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 3U,
                      token) == 3U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 8U);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_call4(&cpu, vita_crt_fread,
                      (uint32_t)(uintptr_t)s_guest_output, 1U, 4U, token) ==
          0U);
    CHECK(g_isaac_vita_crt_errno == EBADF);
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
    CHECK(!vita_crt_dynamic_file_is_live(entry) && entry->token == 0U &&
          !entry->game_log.demoted_write_only && !entry->game_log.demoted_error);
    CHECK(batch_inert());

    /* 10b. Demotion via a non-zero SEEK_CUR (the only SEEK_CUR that reaches
     * demotion): the reopened "r+b" FILE is placed at the lane's logical
     * position before the guest's relative seek runs, so it lands where the
     * synchronous "wb" FILE would be, the following fwrite overwrites there
     * and the file equals a plain FILE driven by the same script. */
    reset_case();
    {
        FILE *reference = tmpfile();
        unsigned char expected[256];
        unsigned char actual[256];

        CHECK(reference != NULL);
        token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
        CHECK(token == (uint32_t)(uintptr_t)entry);
        fill_record(100U, 9U);
        CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 100U,
                          token) == 100U);
        CHECK(fwrite(s_guest_record, 1U, 100U, reference) == 100U);
        CHECK(guest_call3(&cpu, vita_crt_fseek, token, (uint32_t)-40,
                          (uint32_t)SEEK_CUR) == 0U);
        CHECK(fseek(reference, -40L, SEEK_CUR) == 0 && ftell(reference) == 60L);
        CHECK(s_disk_size == 100U && strcmp(s_reopen_mode, "r+b") == 0 &&
              !entry->game_log.live && entry->game_log.demoted_write_only &&
              entry->file.stream != NULL);
        CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 60U);
        fill_record(3U, 10U);
        CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 3U,
                          token) == 3U);
        CHECK(fwrite(s_guest_record, 1U, 3U, reference) == 3U);
        CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 63U);
        CHECK(guest_call3(&cpu, vita_crt_fseek, token, 0U,
                          (uint32_t)SEEK_END) == 0U);
        CHECK(fseek(reference, 0L, SEEK_END) == 0 && ftell(reference) == 100L);
        CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 100U);
        (void)fflush(reference);
        rewind(reference);
        CHECK(fread(expected, 1U, sizeof expected, reference) == 100U);
        (void)fclose(reference);
        CHECK(reopened_content(entry->file.stream, actual, sizeof actual) ==
              100U);
        CHECK(memcmp(actual, expected, 100U) == 0);
        CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
    }
    /* A relative seek before the start fails on the new FILE exactly as
     * newlib's own would (EINVAL), after the same drain and reopen; the
     * FILE keeps its position. */
    reset_case();
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    fill_record(10U, 11U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 10U,
                      token) == 10U);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, (uint32_t)-20,
                      (uint32_t)SEEK_CUR) == UINT32_MAX);
    CHECK(g_isaac_vita_crt_errno == EINVAL && s_disk_size == 10U &&
          strcmp(s_reopen_mode, "r+b") == 0 && !entry->game_log.live &&
          entry->file.stream != NULL);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 10U);
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);

    /* 11. "a" lane on an existing file, demotion with "ab" through a
     * relative seek: the FILE is placed at the logical end first, so the
     * seek resolves from 50; fread reaches the write-only FILE, which
     * answers EBADF itself (newlib does the same) -- the CRT adds no flag. */
    reset_case();
    memcpy(s_disk_path, LOG_TXT, sizeof LOG_TXT);
    memset(s_disk, 'q', 40U);
    s_disk_size = 40U;
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_a);
    CHECK(token == (uint32_t)(uintptr_t)entry && entry->game_log.live &&
          s_disk_trunc_opens == 0U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 40U);
    fill_record(10U, 5U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 10U,
                      token) == 10U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 50U);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, (uint32_t)-10,
                      (uint32_t)SEEK_CUR) == 0U);
    CHECK(s_disk_size == 50U && strcmp(s_reopen_mode, "ab") == 0 &&
          !entry->game_log.live && !entry->game_log.demoted_write_only);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 40U);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_call4(&cpu, vita_crt_fread,
                      (uint32_t)(uintptr_t)s_guest_output, 1U, 4U, token) ==
          0U);
    CHECK(g_isaac_vita_crt_errno == EBADF && !entry->game_log.demoted_error);
    /* Writes on the "ab" FILE still land at the end, as newlib's would. */
    fill_record(2U, 12U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 2U,
                      token) == 2U);
    {
        unsigned char actual[64];

        CHECK(reopened_content(entry->file.stream, actual, sizeof actual) ==
              52U);
        CHECK(memcmp(actual, s_disk, 50U) == 0 &&
              memcmp(actual + 50U, s_guest_record, 2U) == 0);
    }
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);

    /* 12. A second fopen of the same path demotes the live lane first
     * (read-your-writes), then adopts the fresh "w" as the lane again. */
    reset_case();
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    fill_record(30U, 6U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 30U,
                      token) == 30U);
    {
        uint32_t second = guest_fopen(&cpu, LOG_TXT, s_guest_mode_wb);

        CHECK(second == (uint32_t)(uintptr_t)&s_file_tokens[1]);
        CHECK(s_reopen_calls == 1U && strcmp(s_reopen_mode, "r+b") == 0 &&
              !entry->game_log.live && entry->file.stream != NULL &&
              entry->game_log.demoted_write_only &&
              s_file_tokens[1].game_log.live && s_game_log_index == 1 &&
              snap().coherence[0] == 1U && snap().demote == 1U &&
              s_disk_trunc_opens == 2U && s_disk_size == 0U);
        CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 30U);
        CHECK(guest_call1(&cpu, vita_crt_ftell, second) == 0U);
        CHECK(guest_call1(&cpu, vita_crt_fclose, second) == 0U);
        CHECK(s_game_log_index == -1 && !snap().live);
        CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
    }
    /* Same path opened for reading: demote, then the native path (which the
     * double refuses with EACCES). */
    reset_case();
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    fill_record(7U, 7U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 7U,
                      token) == 7U);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_fopen(&cpu, LOG_TXT, s_guest_mode_rb) == 0U);
    CHECK(g_isaac_vita_crt_errno == EACCES && s_native_fopen_calls == 1U &&
          s_disk_size == 7U && !entry->game_log.live &&
          entry->file.stream != NULL && s_game_log_index == -1);
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);

    /* 13. Path-owner drains on the module see the CRT's bytes. */
    reset_case();
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
    fill_record(12U, 8U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 12U,
                      token) == 12U);
    isaac_vita_game_log_sync_path(SAVE_DIR "/options.ini",
                                  ISAAC_VITA_GAME_LOG_SYNC_ACCESS);
    CHECK(s_disk_size == 0U);
    isaac_vita_game_log_sync_path(LOG_TXT, ISAAC_VITA_GAME_LOG_SYNC_REMOVE);
    CHECK(s_disk_size == 12U && memcmp(s_disk, s_guest_record, 12U) == 0);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 12U,
                      token) == 12U);
    isaac_vita_game_log_sync_directory(SAVE_DIR);
    CHECK(s_disk_size == 24U && snap().coherence[4] == 1U);
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);

    /* 14. Other files keep their existing lanes: log.txt "rb", a text file
     * "w", a save "wb" all go native (the double refuses them). */
    reset_case();
    CHECK(guest_fopen(&cpu, LOG_TXT, s_guest_mode_rb) == 0U);
    CHECK(guest_fopen(&cpu, SAVEDATAPATH, s_guest_mode_w) == 0U);
    CHECK(guest_fopen(&cpu, GAMESTATE, s_guest_mode_wb) == 0U);
    CHECK(guest_fopen(&cpu, SAVE_DIR "/mylog.txt", s_guest_mode_w) == 0U);
    CHECK(s_native_fopen_calls == 4U && s_disk_opens == 0U &&
          s_game_log_index == -1 && !snap().live);
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index)
        CHECK(!vita_crt_dynamic_file_is_live(&s_file_tokens[index]));

    /* 15. A full table keeps the exact native fault path: the lane adopts
     * nothing and the native open's stream is discarded. */
    reset_case();
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index)
        s_file_tokens[index].raw_archive.live = 1U;
    s_native_open_succeeds = 1;
    s_mapped_path = LOG_TXT;
    {
        uint32_t arguments[2] = { 1U, (uint32_t)(uintptr_t)s_guest_mode_w };

        prepare_cdecl(&cpu, arguments, 2U);
        vita_crt_fopen(&cpu);
        CHECK(cpu.fault != NULL &&
              strcmp(cpu.fault, "Vita CRT FILE token table full") == 0);
    }
    CHECK(s_native_fopen_calls == 1U && s_disk_opens == 0U &&
          s_game_log_index == -1 && !snap().live && s_file_lock == 0U);
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index)
        s_file_tokens[index].raw_archive.live = 0U;

    /* 16. Exit: flush_all drains (one exit append), shutdown appends nothing
     * more and logs one summary; afterwards log.txt goes native. */
    reset_case();
    token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    fill_record(64U, 9U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 64U,
                      token) == 64U);
    CHECK(isaac_vita_crt_flush_all() == 0);
    CHECK(s_disk_size == 64U && snap().appends[2] == 1U &&
          snap().used == 0U && s_file_lock == 0U);
    {
        uint32_t opens_before = s_disk_opens;

        isaac_vita_game_log_shutdown();
        CHECK(s_disk_opens == opens_before && s_summary_logs == 1U &&
              strstr(s_last_log, " state=stopped ") != NULL &&
              strstr(s_last_log, " appends(worker,guest,exit,abort,direct)=0,0,1,0,0 ") != NULL);
        isaac_vita_game_log_shutdown();
        CHECK(s_summary_logs == 1U);
    }
    /* The live token still works synchronously after shutdown. */
    fill_record(4U, 10U);
    CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U, 4U,
                      token) == 4U);
    CHECK(s_disk_size == 68U && snap().appends[4] == 1U);
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
    CHECK(guest_fopen(&cpu, LOG_TXT, s_guest_mode_w) == 0U);
    CHECK(s_native_fopen_calls == 1U && s_game_log_index == -1);

    /* 17. Byte identity: a 60-call script (fwrite, vfprintf, INFO/WARN chain
     * flushes, ftell, seek-to-end, worker passes) through the lane against
     * the same script on a plain host FILE. */
    reset_case();
    {
        FILE *reference = tmpfile();
        unsigned char expected[8192];
        size_t expected_size;
        uint32_t call;

        CHECK(reference != NULL);
        token = guest_fopen(&cpu, LOG_TXT, s_guest_mode_w);
        CHECK(token == (uint32_t)(uintptr_t)entry);
        for (call = 0U; call < 60U; ++call) {
            uint32_t kind = call % 6U;
            uint32_t length = 1U + (call * 37U) % 90U;

            if (kind == 0U || kind == 3U) {
                fill_record(length, call);
                CHECK(guest_call4(&cpu, vita_crt_fwrite, record_address(), 1U,
                                  length, token) == length);
                CHECK(fwrite(s_guest_record, 1U, length, reference) == length);
            } else if (kind == 1U) {
                CHECK(guest_vfprintf(&cpu, token, call, s_guest_word) ==
                      (uint32_t)snprintf(NULL, 0U, "%u-%s", call, "ok"));
                CHECK(fprintf(reference, "%u-%s", call, "ok") > 0);
            } else if (kind == 2U) {
                CHECK(guest_fflush_chain(&cpu, token, 0U) == 0U);
            } else if (kind == 4U) {
                CHECK(guest_fflush_chain(&cpu, token, (call / 6U) % 4U) == 0U);
                CHECK(guest_call3(&cpu, vita_crt_fseek, token, 0U,
                                  (uint32_t)SEEK_END) == 0U);
            } else {
                long reference_position;

                (void)fseek(reference, 0L, SEEK_END);
                reference_position = ftell(reference);
                CHECK(reference_position >= 0L);
                CHECK(guest_call1(&cpu, vita_crt_ftell, token) ==
                      (uint32_t)reference_position);
                if (call % 12U == 5U)
                    CHECK(isaac_vita_game_log_oracle_run_one() >= 0);
            }
        }
        CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
        (void)fflush(reference);
        rewind(reference);
        expected_size = fread(expected, 1U, sizeof expected, reference);
        (void)fclose(reference);
        CHECK(expected_size > 0U && expected_size < sizeof expected);
        CHECK(s_disk_size == expected_size &&
              memcmp(s_disk, expected, expected_size) == 0);
        CHECK(snap().records == 30U && snap().lost_bytes == 0U &&
              snap().fail == 0U);
    }
    CHECK(batch_inert());
    return 0;
}

int main(void)
{
    if (run() != 0)
        return 1;
    printf("Vita CRT game log write-behind dispatch host oracle: PASS "
           "(durable %u%s)\n", (unsigned)DURABLE,
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
           ", GAME_LOG_BATCH inert"
#else
           ""
#endif
    );
    return 0;
}
