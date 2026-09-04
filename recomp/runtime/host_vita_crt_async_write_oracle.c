/* Hostile executable oracle for the CRT side of the asynchronous save writer.
 * Include the production CRT TU so the test exercises the exact FILE-token
 * dispatch (fopen/fwrite/fseek/ftell/_fileno/fflush/fread/vfprintf/fclose)
 * that the frozen guest drives, with the writer module linked in its host
 * oracle configuration (jobs run only when this file performs them). */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

#define ISAAC_VITA_ARCHIVE_FILE_CACHE 1
#define ISAAC_VITA_ASYNC_SAVE_WRITE 1
#define ISAAC_VITA_ASYNC_WRITE_ORACLE 1
#define ISAAC_VITA_CRT_QSORT_HOST_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX INT32_MAX
#include "host_vita_crt.c"

#define ROOT "ux0:/data/isaacr001"
#define SAVE_DIR ROOT "/Documents/My Games/Binding of Isaac Repentance/"
#define GAMESTATE SAVE_DIR "gamestate1.dat"
#define LOG_TXT SAVE_DIR "log.txt"

static char s_guest_mode_wb[] = "wb";
static char s_guest_mode_rb[] = "rb";
static char s_guest_mode_w[] = "w";
static char s_guest_format[] = "%d-%s";
static char s_guest_word[] = "ok";
_Alignas(16) static uint32_t s_guest_stack[64];
_Alignas(16) static uint32_t s_guest_varargs[4];
_Alignas(16) static unsigned char s_guest_save[68741];
_Alignas(16) static unsigned char s_guest_output[16];
static const char *s_mapped_path;
static uint32_t s_native_fopen_calls;
static uint32_t s_pending_at_native_open;
static uint32_t s_diag_logs;

/* writer oracle doubles */
static uint8_t s_arena[ISAAC_VITA_ASYNC_WRITE_ARENA_BYTES];
static unsigned char s_disk[ISAAC_VITA_ASYNC_WRITE_LANE_BYTES];
static uint32_t s_disk_size;
static char s_disk_path[ISAAC_VITA_ASYNC_WRITE_PATH_MAX + 1U];
static uint32_t s_disk_opens;
static uint32_t s_disk_writes;
static uint64_t s_clock;

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "crt async-write oracle failed at line %u: %s\n",
            line, expression);
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
    (void)format;
    ++s_diag_logs;
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

/* Archive cache doubles: the native lane always fails with EACCES so no real
 * FILE is created, and the raw ENOMEM fallback is never taken. */
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
    s_pending_at_native_open = isaac_vita_async_write_oracle_pending();
    memset(file, 0, sizeof *file);
    if (cache_hit)
        *cache_hit = 0;
    errno = EACCES;
    return NULL;
}

int isaac_vita_archive_cache_force_discard(
    isaac_vita_archive_cache_file *file)
{
    memset(file, 0, sizeof *file);
    return 0;
}

int isaac_vita_archive_cache_close(isaac_vita_archive_cache_file *file)
{
    (void)file;
    errno = EBADF;
    return EOF;
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
    length = snprintf(line, capacity, "async-oracle");
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

int32_t isaac_vita_async_write_oracle_open(const char *path)
{
    ++s_disk_opens;
    memcpy(s_disk_path, path, strlen(path) + 1U);
    s_disk_size = 0U;
    return 100;
}

int32_t isaac_vita_async_write_oracle_write(int32_t descriptor,
                                            const void *buffer,
                                            uint32_t size)
{
    if (descriptor != 100 || s_disk_size + size > sizeof s_disk)
        return -1;
    ++s_disk_writes;
    memcpy(s_disk + s_disk_size, buffer, size);
    s_disk_size += size;
    return (int32_t)size;
}

int32_t isaac_vita_async_write_oracle_close(int32_t descriptor)
{
    return descriptor == 100 ? 0 : -1;
}

uint8_t *isaac_vita_async_write_oracle_arena(void)
{
    return s_arena;
}

uint64_t isaac_vita_async_write_oracle_time_us(void)
{
    s_clock += 100U;
    return s_clock;
}

int isaac_vita_async_write_oracle_worker_start(void)
{
    return 1;
}

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

static int run(void)
{
    CPU cpu;
    vita_crt_file_token *entry = &s_file_tokens[0];
    uint32_t token;
    uint32_t save = (uint32_t)(uintptr_t)s_guest_save;
    uint32_t index;

    CHECK((uintptr_t)&s_guest_stack[64] <= UINT32_MAX &&
          (uintptr_t)&s_guest_save[sizeof s_guest_save] <= UINT32_MAX &&
          (uintptr_t)&s_guest_varargs[4] <= UINT32_MAX &&
          (uintptr_t)s_guest_mode_wb <= UINT32_MAX &&
          (uintptr_t)s_guest_format <= UINT32_MAX &&
          (uintptr_t)s_guest_word <= UINT32_MAX &&
          (uintptr_t)&s_file_tokens[ISAAC_VITA_CRT_FILE_TOKEN_COUNT] <=
              UINT32_MAX);
    memset(s_file_tokens, 0, sizeof s_file_tokens);
    for (index = 0U; index < sizeof s_guest_save; ++index)
        s_guest_save[index] = (unsigned char)(index * 13U + 5U);

    /* fopen "wb" on the save: image token, no native FILE, errno untouched. */
    g_isaac_vita_crt_errno = EINVAL;
    token = guest_fopen(&cpu, GAMESTATE, s_guest_mode_wb);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    CHECK(entry->async_write.live && !entry->file.stream &&
          !entry->raw_archive.live && entry->token == token &&
          s_native_fopen_calls == 0U && g_isaac_vita_crt_errno == EINVAL &&
          s_file_lock == 0U);
    CHECK(vita_crt_dynamic_file_is_live(entry) &&
          vita_crt_dynamic_stream(entry) == NULL);

    /* One whole-buffer fwrite, the measured save shape. */
    CHECK(guest_call4(&cpu, vita_crt_fwrite, save, 1U,
                      (uint32_t)sizeof s_guest_save, token) ==
          (uint32_t)sizeof s_guest_save);
    CHECK(entry->async_write.size == sizeof s_guest_save &&
          s_disk_opens == 0U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) ==
          (uint32_t)sizeof s_guest_save);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 0U, (uint32_t)SEEK_SET) ==
          0U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 0U);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, 0U, (uint32_t)SEEK_END) ==
          0U);
    CHECK(guest_call1(&cpu, vita_crt_ftell, token) ==
          (uint32_t)sizeof s_guest_save);
    CHECK(guest_call3(&cpu, vita_crt_fseek, token, (uint32_t)-1,
                      (uint32_t)SEEK_SET) == UINT32_MAX &&
          g_isaac_vita_crt_errno == EINVAL);

    /* The lock bridge: _fileno publishes a pseudo descriptor that
     * _get_osfhandle/LockFileEx ownership accepts; neighbours do not. */
    CHECK(guest_call1(&cpu, vita_crt_fileno, token) ==
          ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE);
    CHECK(isaac_vita_crt_osfhandle_is_owned(
              ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE));
    CHECK(!isaac_vita_crt_osfhandle_is_owned(
              ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE + 1U));
    CHECK(guest_call1(&cpu, vita_crt_get_osfhandle,
                      ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE) ==
          ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE);
    CHECK(guest_call1(&cpu, vita_crt_fflush, token) == 0U);
    CHECK(!entry->async_write.error);

    /* fclose queues one job; the file appears only when the worker runs. */
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
    CHECK(!vita_crt_dynamic_file_is_live(entry) && entry->token == 0U);
    CHECK(isaac_vita_async_write_oracle_pending() == 1U && s_disk_opens == 0U);
    CHECK(isaac_vita_async_write_oracle_run_one() == 1);
    CHECK(s_disk_opens == 1U && s_disk_writes == 1U &&
          s_disk_size == sizeof s_guest_save &&
          strcmp(s_disk_path, GAMESTATE) == 0 &&
          memcmp(s_disk, s_guest_save, sizeof s_guest_save) == 0);
    CHECK(!isaac_vita_crt_osfhandle_is_owned(
              ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE));

    /* Read-your-writes: a queued rewrite is completed before any native
     * open of the same path, whatever the mode. */
    token = guest_fopen(&cpu, GAMESTATE, s_guest_mode_wb);
    CHECK(token == (uint32_t)(uintptr_t)entry);
    s_guest_save[0] ^= 0xffU;
    CHECK(guest_call4(&cpu, vita_crt_fwrite, save, (uint32_t)sizeof s_guest_save,
                      1U, token) == 1U);
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
    CHECK(isaac_vita_async_write_oracle_pending() == 1U);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_fopen(&cpu, GAMESTATE, s_guest_mode_rb) == 0U);
    CHECK(s_native_fopen_calls == 1U && s_pending_at_native_open == 0U &&
          s_disk_opens == 2U && s_disk[0] == s_guest_save[0] &&
          g_isaac_vita_crt_errno == EACCES);

    /* Ineligible "w" below the same directory stays native. */
    CHECK(guest_fopen(&cpu, LOG_TXT, s_guest_mode_w) == 0U);
    CHECK(s_native_fopen_calls == 2U);
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index)
        CHECK(!vita_crt_dynamic_file_is_live(&s_file_tokens[index]));

    /* vfprintf formats straight into the image. */
    token = guest_fopen(&cpu, GAMESTATE, s_guest_mode_w);
    CHECK(token == (uint32_t)(uintptr_t)entry && entry->async_write.live);
    {
        uint32_t arguments[6] = {
            0U, 0U, token, (uint32_t)(uintptr_t)s_guest_format, 0U,
            (uint32_t)(uintptr_t)s_guest_varargs
        };
        uint32_t esp;

        s_guest_varargs[0] = 42U;
        s_guest_varargs[1] = (uint32_t)(uintptr_t)s_guest_word;
        esp = prepare_cdecl(&cpu, arguments, 6U);
        errno = EBUSY;
        vita_crt_stdio_common_vfprintf(&cpu);
        CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 5U &&
              errno == EBUSY);
    }
    CHECK(entry->async_write.size == 5U &&
          memcmp(entry->async_write.data, "42-ok", 5U) == 0 &&
          !entry->async_write.error);

    /* fread on a write image is an error the guest sees; the image is then
     * refused at fclose and the earlier file survives untouched. */
    CHECK(guest_call4(&cpu, vita_crt_fread,
                      (uint32_t)(uintptr_t)s_guest_output, 1U, 4U, token) ==
          0U);
    CHECK(entry->async_write.error && g_isaac_vita_crt_errno == EBADF);
    g_isaac_vita_crt_errno = 0;
    CHECK(guest_call1(&cpu, vita_crt_fclose, token) == (uint32_t)EOF);
    CHECK(g_isaac_vita_crt_errno == EIO &&
          !vita_crt_dynamic_file_is_live(entry) &&
          isaac_vita_async_write_oracle_pending() == 0U &&
          s_disk_opens == 2U && s_disk_size == sizeof s_guest_save);

    /* 65,943-byte save through the CRT dispatch: a "wb" open, chunked
     * fwrite, fclose queues the job, and a "rb" open (fopen's read-your-writes
     * sync_path) drains the still-queued -- busy -- job before the native open,
     * so the reader always observes the completed 65,943-byte image.  This is
     * the size the device hang appeared at. */
    {
        uint32_t disk_opens_before = s_disk_opens;
        uint32_t native_before = s_native_fopen_calls;
        uint32_t chunk = 40000U;

        token = guest_fopen(&cpu, GAMESTATE, s_guest_mode_wb);
        CHECK(token == (uint32_t)(uintptr_t)entry && entry->async_write.live);
        CHECK(guest_call4(&cpu, vita_crt_fwrite, save, 1U, chunk, token) ==
              chunk);
        CHECK(guest_call4(&cpu, vita_crt_fwrite, save + chunk, 1U,
                          65943U - chunk, token) == 65943U - chunk);
        CHECK(guest_call1(&cpu, vita_crt_ftell, token) == 65943U);
        CHECK(entry->async_write.size == 65943U && s_disk_opens ==
              disk_opens_before);
        CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
        CHECK(isaac_vita_async_write_oracle_pending() == 1U);
        /* Busy worker (job still queued): the "rb" open must drain it first. */
        CHECK(guest_fopen(&cpu, GAMESTATE, s_guest_mode_rb) == 0U);
        CHECK(isaac_vita_async_write_oracle_pending() == 0U);
        CHECK(s_pending_at_native_open == 0U);
        CHECK(s_disk_opens == disk_opens_before + 1U);
        CHECK(s_native_fopen_calls == native_before + 1U);
        CHECK(s_disk_size == 65943U &&
              memcmp(s_disk, s_guest_save, 65943U) == 0);
    }

    /* Teardown drains and leaves every slot free. */
    {
        uint32_t opens_before = s_disk_opens;

        token = guest_fopen(&cpu, GAMESTATE, s_guest_mode_wb);
        CHECK(token == (uint32_t)(uintptr_t)entry);
        CHECK(guest_call4(&cpu, vita_crt_fwrite, save, 1U, 8U, token) == 8U);
        CHECK(guest_call1(&cpu, vita_crt_fclose, token) == 0U);
        CHECK(isaac_vita_async_write_oracle_pending() == 1U);
        isaac_vita_crt_async_write_shutdown();
        CHECK(isaac_vita_async_write_oracle_pending() == 0U &&
              s_disk_opens == opens_before + 1U && s_disk_size == 8U &&
              s_file_lock == 0U);
    }
    return 0;
}

int main(void)
{
    if (run() != 0)
        return 1;
    puts("Vita CRT asynchronous save writer dispatch host oracle: PASS");
    return 0;
}
