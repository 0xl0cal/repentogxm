/* Hostile executable oracle for the allocation-free packed-archive backend.
 * Include the production CRT TU so the test exercises its exact private slot,
 * token and FILE-semantics helpers without adding a production-only test seam.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

#define ISAAC_VITA_ARCHIVE_FILE_CACHE 1
#define ISAAC_VITA_CRT_QSORT_HOST_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX INT32_MAX
#include "host_vita_crt.c"

#define ROOT "ux0:/data/isaacr001"
#define PACKED ROOT "/resources/packed/"
#define AFTERBIRTHP PACKED "afterbirthp.a"
#define MUSIC PACKED "music.a"
#define LOOSE ROOT "/resources/loose/afterbirthp.a"
#define MUSIC_LOOSE ROOT "/resources/loose/music.a"

#define SCE_ENOENT ((int32_t)UINT32_C(0x80010002))
#define SCE_EIO    ((int32_t)UINT32_C(0x80010005))
#define SCE_ENODEV ((int32_t)UINT32_C(0x80010013))

static const unsigned char s_bytes[] = "abcdefghij";
/* Same ten-byte length as s_bytes; the word at offset 4 is a valid
 * ArchivedFile block header (flag bit set, 4-byte block). */
static const unsigned char s_header_bytes[] = {
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x80, 'i', 'j'
};
static const unsigned char *s_pread_source = s_bytes;
static int64_t s_size_override = -1;
static uint32_t s_reopen_native_calls;
static char s_guest_mode_rb[] = "rb";
_Alignas(16) static uint32_t s_guest_stack[64];
_Alignas(16) static unsigned char s_guest_output[16];
static FILE *const s_fake_stream = (FILE *)(uintptr_t)UINT32_C(0x12340000);
static const char *s_mapped_path;
static uint32_t s_open_calls;
static uint32_t s_pread_calls;
static uint32_t s_size_calls;
static uint32_t s_close_calls;
static uint32_t s_next_descriptor;
static uint32_t s_max_chunk;
static uint32_t s_last_pread_size;
static uint32_t s_fail_pread_call;
static int32_t s_fail_pread_result;
static int32_t s_open_result;
static int32_t s_size_result;
static int32_t s_close_result;
static int s_oversize_next;
static uint32_t s_native_fopen_calls;
static int s_native_fopen_errno;
static uint32_t s_diag_records;
static uint32_t s_diag_logs;
static isaac_vita_archive_diag_event s_last_diag;
static char s_last_diag_reason[32];

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "raw archive oracle failed at line %u: %s\n",
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

static void reset_oracle(void)
{
    memset(s_file_tokens, 0, sizeof s_file_tokens);
    s_open_calls = 0U;
    s_pread_calls = 0U;
    s_size_calls = 0U;
    s_close_calls = 0U;
    s_next_descriptor = 100U;
    s_max_chunk = UINT32_MAX;
    s_last_pread_size = 0U;
    s_fail_pread_call = 0U;
    s_fail_pread_result = SCE_EIO;
    s_open_result = 0;
    s_size_result = 0;
    s_close_result = 0;
    s_oversize_next = 0;
    s_native_fopen_calls = 0U;
    s_native_fopen_errno = ENOMEM;
    s_diag_records = 0U;
    s_diag_logs = 0U;
    memset(&s_last_diag, 0, sizeof s_last_diag);
    s_last_diag_reason[0] = '\0';
    s_archive_diag_logged_first_relevant = 0U;
    s_archive_diag_logged_first_direct = 0U;
    s_file_lock = 0U;
    s_mapped_path = AFTERBIRTHP;
    memset(s_guest_output, 0, sizeof s_guest_output);
    s_pread_source = s_bytes;
    s_size_override = -1;
    s_reopen_native_calls = 0U;
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    memset(&s_descriptor_recover, 0, sizeof s_descriptor_recover);
#endif
}

int isaac_vita_archive_raw_fallback_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY])
{
    static const char prefix[] = PACKED;
    const char *name;
    size_t length;

    key[0] = '\0';
    if (!native_path || !mode || strcmp(mode, "rb") != 0 ||
        strncmp(native_path, prefix, sizeof prefix - 1U) != 0)
        return 0;
    name = native_path + sizeof prefix - 1U;
    length = strlen(name);
    if (length <= 2U || length >= ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY ||
        strchr(name, '/') || strchr(name, '\\') ||
        name[length - 2U] != '.' || name[length - 1U] != 'a')
        return 0;
    memcpy(key, name, length + 1U);
    return 1;
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
    errno = s_native_fopen_errno;
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

/* The newlib-lane recovery seam is never taken by a raw token; count it. */
FILE *isaac_vita_archive_cache_reopen_native(const char *native_path,
                                             const char *mode)
{
    (void)native_path;
    (void)mode;
    ++s_reopen_native_calls;
    errno = ENOMEM;
    return NULL;
}

uint32_t isaac_vita_archive_diag_record(
    isaac_vita_archive_diag_event *event)
{
    ++s_diag_records;
    s_last_diag = *event;
    return s_diag_records;
}

uint32_t isaac_vita_archive_diag_snapshot(
    isaac_vita_archive_diag_event *events, uint32_t capacity,
    uint32_t *latest_sequence)
{
    (void)events;
    (void)capacity;
    if (latest_sequence)
        *latest_sequence = s_diag_records;
    return 0U;
}

int isaac_vita_archive_diag_format_event(
    char *line, size_t capacity, const char *build_id,
    const char *reason, const isaac_vita_archive_diag_event *event)
{
    int length;

    (void)build_id;
    s_last_diag = *event;
    (void)snprintf(s_last_diag_reason, sizeof s_last_diag_reason,
                   "%s", reason);
    length = snprintf(line, capacity, "raw-direct");
    return length >= 0 && (size_t)length < capacity ? length : -1;
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

int32_t isaac_vita_crt_raw_archive_oracle_open(const char *path)
{
    ++s_open_calls;
    if (strcmp(path, AFTERBIRTHP) != 0 && strcmp(path, MUSIC) != 0)
        return SCE_ENOENT;
    if (s_open_result < 0)
        return s_open_result;
    return (int32_t)s_next_descriptor++;
}

int32_t isaac_vita_crt_raw_archive_oracle_get_size(
    int32_t descriptor, int64_t *size)
{
    ++s_size_calls;
    if (descriptor < 100 || !size)
        return SCE_EIO;
    if (s_size_result < 0)
        return s_size_result;
    *size = s_size_override >= 0 ? s_size_override
        : (int64_t)(sizeof s_bytes - 1U);
    return 0;
}

int32_t isaac_vita_crt_raw_archive_oracle_pread(
    int32_t descriptor, void *buffer, uint32_t size, uint64_t offset)
{
    uint32_t available;
    uint32_t result;

    ++s_pread_calls;
    s_last_pread_size = size;
    if (descriptor < 100)
        return SCE_EIO;
    if (s_fail_pread_call == s_pread_calls)
        return s_fail_pread_result;
    if (s_oversize_next) {
        s_oversize_next = 0;
        return size == UINT32_MAX ? (int32_t)size : (int32_t)(size + 1U);
    }
    if (offset >= sizeof s_bytes - 1U)
        return 0;
    available = (uint32_t)(sizeof s_bytes - 1U - offset);
    result = size < available ? size : available;
    if (result > s_max_chunk)
        result = s_max_chunk;
    memcpy(buffer, s_pread_source + offset, result);
    return (int32_t)result;
}

int32_t isaac_vita_crt_raw_archive_oracle_close(int32_t descriptor)
{
    ++s_close_calls;
    if (descriptor < 100)
        return SCE_EIO;
    return s_close_result;
}

static int open_slot(uint32_t index)
{
    vita_crt_file_token *entry = &s_file_tokens[index];

    CHECK(index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT);
    errno = 0;
    CHECK(vita_crt_raw_archive_try_open(
              entry, AFTERBIRTHP, "rb", ENOMEM));
    CHECK(entry->raw_archive.live && !entry->file.stream);
    CHECK(entry->raw_archive.position == 0 &&
          entry->raw_archive.size == (int64_t)(sizeof s_bytes - 1U));
    CHECK(strcmp(entry->file.key, "afterbirthp.a") == 0);
    CHECK((uintptr_t)entry <= UINT32_MAX);
    entry->token = (uint32_t)(uintptr_t)entry;
    return 0;
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

static int test_strict_fallback_gate(void)
{
    vita_crt_file_token *entry = &s_file_tokens[0];

    reset_oracle();
    errno = 0;
    CHECK(!vita_crt_raw_archive_try_open(
              entry, AFTERBIRTHP, "rb", EACCES));
    CHECK(errno == EACCES && s_open_calls == 0U &&
          !entry->raw_archive.live && !entry->file.key[0]);
    CHECK(!vita_crt_raw_archive_try_open(entry, MUSIC, "rb", EACCES));
    CHECK(!vita_crt_raw_archive_try_open(entry, MUSIC_LOOSE, "rb", ENOMEM));
    CHECK(!vita_crt_raw_archive_try_open(entry, MUSIC, "r", ENOMEM));
    CHECK(!vita_crt_raw_archive_try_open(entry, LOOSE, "rb", ENOMEM));
    CHECK(!vita_crt_raw_archive_try_open(entry, AFTERBIRTHP, "r", ENOMEM));
    CHECK(s_open_calls == 0U && errno == ENOMEM);

    s_open_result = SCE_ENOENT;
    CHECK(!vita_crt_raw_archive_try_open(
              entry, AFTERBIRTHP, "rb", ENOMEM));
    CHECK(errno == ENOENT && s_open_calls == 1U && s_size_calls == 0U &&
          !entry->raw_archive.live && !entry->file.key[0]);

    s_open_result = 0;
    s_size_result = SCE_EIO;
    CHECK(!vita_crt_raw_archive_try_open(
              entry, AFTERBIRTHP, "rb", ENOMEM));
    CHECK(errno == EIO && s_open_calls == 2U && s_size_calls == 1U &&
          s_close_calls == 1U && !entry->raw_archive.live);
    return 0;
}

static int test_persistent_music_raw_stream(void)
{
    uint32_t fopen_arguments[2] = {
        1U, (uint32_t)(uintptr_t)s_guest_mode_rb
    };
    uint32_t fread_arguments[4] = {
        (uint32_t)(uintptr_t)s_guest_output, 1U, 4U, 0U
    };
    uint32_t fseek_arguments[3];
    uint32_t one_argument[1];
    vita_crt_file_token *entry = &s_file_tokens[0];
    CPU cpu;
    uint32_t esp;
    uint32_t token;
    int32_t descriptor;

    CHECK((uintptr_t)s_guest_mode_rb <= UINT32_MAX &&
          (uintptr_t)&s_guest_output[sizeof s_guest_output] <= UINT32_MAX);
    reset_oracle();
    s_mapped_path = MUSIC;
    g_isaac_vita_crt_errno = EINVAL;
    esp = prepare_cdecl(&cpu, fopen_arguments, 2U);
    errno = EBUSY;
    vita_crt_fopen(&cpu);
    token = cpu.eax;
    CHECK(!cpu.fault && cpu.esp == esp + 4U && token != 0U &&
          token == (uint32_t)(uintptr_t)entry &&
          entry->raw_archive.live && !entry->file.stream &&
          strcmp(entry->file.key, "music.a") == 0 &&
          entry->raw_archive.position == 0 &&
          entry->raw_archive.size == (int64_t)(sizeof s_bytes - 1U) &&
          s_native_fopen_calls == 1U && s_open_calls == 1U &&
          s_size_calls == 1U && s_close_calls == 0U &&
          errno == EBUSY && g_isaac_vita_crt_errno == EINVAL &&
          s_diag_records == 1U && s_diag_logs == 1U &&
          (s_last_diag.flags & ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT) &&
          !(s_last_diag.flags & ISAAC_VITA_ARCHIVE_DIAG_RELEVANT) &&
          strcmp(s_last_diag.key, "music.a") == 0);
    descriptor = entry->raw_archive.descriptor;

    fread_arguments[3] = token;
    esp = prepare_cdecl(&cpu, fread_arguments, 4U);
    errno = EACCES;
    vita_crt_fread(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 4U &&
          memcmp(s_guest_output, "abcd", 4U) == 0 &&
          entry->raw_archive.live && entry->raw_archive.position == 4 &&
          s_pread_calls == 1U && errno == EACCES &&
          g_isaac_vita_crt_errno == EINVAL);

    fseek_arguments[0] = token;
    fseek_arguments[1] = 2U;
    fseek_arguments[2] = SEEK_SET;
    esp = prepare_cdecl(&cpu, fseek_arguments, 3U);
    errno = EACCES;
    vita_crt_fseek(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          entry->raw_archive.live && entry->raw_archive.position == 2 &&
          errno == EACCES);

    one_argument[0] = token;
    esp = prepare_cdecl(&cpu, one_argument, 1U);
    errno = EACCES;
    vita_crt_ftell(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 2U &&
          entry->raw_archive.live && errno == EACCES);

    g_isaac_vita_crt_errno = EINVAL;
    esp = prepare_cdecl(&cpu, one_argument, 1U);
    errno = EACCES;
    vita_crt_fileno(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          g_isaac_vita_crt_errno == EBADF && errno == EACCES &&
          entry->raw_archive.live && entry->raw_archive.position == 2 &&
          !isaac_vita_crt_osfhandle_is_owned((uint32_t)descriptor));

    fread_arguments[0] = (uint32_t)(uintptr_t)&s_guest_output[4];
    fread_arguments[2] = 2U;
    esp = prepare_cdecl(&cpu, fread_arguments, 4U);
    errno = EACCES;
    vita_crt_fread(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 2U &&
          memcmp(&s_guest_output[4], "cd", 2U) == 0 &&
          entry->raw_archive.live && entry->raw_archive.position == 4 &&
          s_pread_calls == 2U && errno == EACCES);

    esp = prepare_cdecl(&cpu, one_argument, 1U);
    errno = EACCES;
    vita_crt_fclose(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          errno == EACCES && s_close_calls == 1U &&
          !vita_crt_dynamic_file_is_live(entry) && !entry->file.key[0] &&
          entry->token == 0U);
    return 0;
}

static int test_outer_fopen_receipt_and_table_full(void)
{
    uint32_t fopen_arguments[2] = {
        1U, (uint32_t)(uintptr_t)s_guest_mode_rb
    };
    uint32_t fclose_arguments[1];
    CPU cpu;
    uint32_t esp;
    uint32_t token;
    uint32_t index;

    CHECK((uintptr_t)s_guest_mode_rb <= UINT32_MAX &&
          (uintptr_t)&s_guest_stack[64] <= UINT32_MAX);

    reset_oracle();
    s_native_fopen_errno = EACCES;
    g_isaac_vita_crt_errno = EINVAL;
    esp = prepare_cdecl(&cpu, fopen_arguments, 2U);
    errno = EBUSY;
    vita_crt_fopen(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          errno == EBUSY && g_isaac_vita_crt_errno == EACCES &&
          s_native_fopen_calls == 1U && s_open_calls == 0U &&
          s_diag_records == 0U && s_diag_logs == 0U);

    reset_oracle();
    g_isaac_vita_crt_errno = EINVAL;
    esp = prepare_cdecl(&cpu, fopen_arguments, 2U);
    errno = EBUSY;
    vita_crt_fopen(&cpu);
    token = cpu.eax;
    CHECK(!cpu.fault && cpu.esp == esp + 4U && token != 0U &&
          token == (uint32_t)(uintptr_t)&s_file_tokens[0] &&
          errno == EBUSY && g_isaac_vita_crt_errno == EINVAL &&
          s_native_fopen_calls == 1U && s_open_calls == 1U &&
          s_file_tokens[0].raw_archive.live &&
          s_diag_records == 1U && s_diag_logs == 1U &&
          strcmp(s_last_diag_reason, "first-direct-open") == 0 &&
          s_last_diag.kind == ISAAC_VITA_ARCHIVE_DIAG_OPEN &&
          (s_last_diag.flags & ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT) &&
          (s_last_diag.flags & ISAAC_VITA_ARCHIVE_DIAG_RELEVANT) &&
          s_last_diag.operation_result == 0 &&
          s_last_diag.operation_errno == ENOMEM &&
          strcmp(s_last_diag.key, "afterbirthp.a") == 0);

    fclose_arguments[0] = token;
    esp = prepare_cdecl(&cpu, fclose_arguments, 1U);
    errno = EBUSY;
    vita_crt_fclose(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          errno == EBUSY && s_close_calls == 1U &&
          !vita_crt_dynamic_file_is_live(&s_file_tokens[0]));

    /* Every direct success enters the ring, but the durable receipt is
     * deliberately first-only so archive reopens cannot flood the log. */
    esp = prepare_cdecl(&cpu, fopen_arguments, 2U);
    errno = EBUSY;
    vita_crt_fopen(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax != 0U &&
          s_diag_records == 2U && s_diag_logs == 1U);
    fclose_arguments[0] = cpu.eax;
    esp = prepare_cdecl(&cpu, fclose_arguments, 1U);
    vita_crt_fclose(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && s_close_calls == 2U);

    reset_oracle();
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        s_file_tokens[index].raw_archive.descriptor =
            (int32_t)(200U + index);
        s_file_tokens[index].raw_archive.live = 1U;
        s_file_tokens[index].token =
            (uint32_t)(uintptr_t)&s_file_tokens[index];
        strcpy(s_file_tokens[index].file.key, "afterbirthp.a");
    }
    esp = prepare_cdecl(&cpu, fopen_arguments, 2U);
    errno = EBUSY;
    vita_crt_fopen(&cpu);
    CHECK(cpu.fault &&
          strcmp(cpu.fault, "Vita CRT FILE token table full") == 0 &&
          cpu.esp == esp && errno == EBUSY && s_open_calls == 1U &&
          s_size_calls == 1U && s_close_calls == 1U &&
          s_diag_records == 0U && s_diag_logs == 0U);
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index)
        CHECK(s_file_tokens[index].raw_archive.live &&
              s_file_tokens[index].raw_archive.descriptor ==
                  (int32_t)(200U + index));
    return 0;
}

static int test_short_reads_and_element_overlap(void)
{
    vita_crt_file_token *entry;
    unsigned char output[16];
    size_t result;

    reset_oracle();
    CHECK(open_slot(0U) == 0);
    entry = &s_file_tokens[0];
    s_max_chunk = 2U;
    memset(output, 0xcc, sizeof output);
    result = vita_crt_raw_archive_fread(entry, output, 3U, 3U);
    CHECK(result == 3U && s_pread_calls == 5U &&
          entry->raw_archive.position == 9 &&
          !entry->raw_archive.eof && !entry->raw_archive.error);
    CHECK(memcmp(output, "abcdefghi", 9U) == 0);

    result = vita_crt_raw_archive_fread(entry, output + 9U, 2U, 2U);
    CHECK(result == 0U && s_pread_calls == 7U &&
          entry->raw_archive.position == 10 && entry->raw_archive.eof &&
          output[9] == 'j');
    CHECK(vita_crt_file_feof(s_fake_stream, 0) &&
          !vita_crt_file_ferror(s_fake_stream, 0));
    CHECK(vita_crt_raw_archive_fseek(entry, 0L, SEEK_SET) == 0 &&
          !entry->raw_archive.eof);

    /* Zero-sized requests are no-ops: no kernel call and no sticky state. */
    entry->raw_archive.eof = 0U;
    entry->raw_archive.error = 0U;
    s_pread_calls = 0U;
    errno = EACCES;
    CHECK(vita_crt_raw_archive_fread(entry, output, 0U, 3U) == 0U &&
          vita_crt_raw_archive_fread(entry, output, 3U, 0U) == 0U &&
          s_pread_calls == 0U && !entry->raw_archive.eof &&
          !entry->raw_archive.error && errno == EACCES);

    /* Preserve the incomplete element's bytes and logical position when the
     * following pread fails, but return only complete elements like fread. */
    s_pread_calls = 0U;
    s_fail_pread_call = 2U;
    memset(output, 0, sizeof output);
    errno = 0;
    result = vita_crt_raw_archive_fread(entry, output, 3U, 2U);
    CHECK(result == 0U && errno == EIO && s_pread_calls == 2U &&
          entry->raw_archive.position == 2 && entry->raw_archive.error &&
          !entry->raw_archive.eof && memcmp(output, "ab", 2U) == 0);
    CHECK(vita_crt_raw_archive_fseek(entry, 0L, SEEK_SET) == 0 &&
          entry->raw_archive.error);

    entry->raw_archive.error = 0U;
    s_pread_calls = 0U;
    s_fail_pread_call = 0U;
    s_oversize_next = 1;
    errno = 0;
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 1U) == 0U &&
          errno == EIO && entry->raw_archive.position == 0 &&
          entry->raw_archive.error);

    /* A guest-sized request can exceed the signed sceIoPread parameter even
     * though the CRT total is uint32-bounded.  Never pass more than INT_MAX. */
    entry->raw_archive.error = 0U;
    s_pread_calls = 0U;
    s_fail_pread_call = 1U;
    errno = 0;
    CHECK(vita_crt_raw_archive_fread(
              entry, output, 1U, (size_t)INT_MAX + 1U) == 0U &&
          s_pread_calls == 1U && s_last_pread_size == (uint32_t)INT_MAX &&
          errno == EIO && entry->raw_archive.position == 0 &&
          entry->raw_archive.error);
    return 0;
}

static int test_seek_stdio_surface_and_close(void)
{
    vita_crt_file_token *entry;
    unsigned char output[4];
    FILE *found_stream = (FILE *)(uintptr_t)1U;
    int index = -1;
    int standard = -1;
    int32_t descriptor;

    reset_oracle();
    CHECK(open_slot(0U) == 0);
    entry = &s_file_tokens[0];
    descriptor = entry->raw_archive.descriptor;
    CHECK(vita_crt_dynamic_file_is_live(entry) &&
          vita_crt_dynamic_stream(entry) == NULL);
    CHECK(vita_crt_find_file_token_unlocked(
              entry->token, &found_stream, &index, &standard));
    CHECK(found_stream == NULL && index == 0 && standard == -1);

    errno = 0;
    CHECK(vita_crt_file_fileno(s_fake_stream, 0) == -1 && errno == EBADF &&
          !isaac_vita_crt_osfhandle_is_owned((uint32_t)descriptor));
    errno = 0;
    CHECK(vita_crt_file_fflush(s_fake_stream, 0) == 0 && errno == 0);
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 2U) == 2U &&
          memcmp(output, "ab", 2U) == 0);

    CHECK(vita_crt_file_fseek(s_fake_stream, 0, -2L, SEEK_END) == 0 &&
          vita_crt_file_ftell(s_fake_stream, 0) == 8L);
    CHECK(vita_crt_file_fseek(s_fake_stream, 0, -3L, SEEK_CUR) == 0 &&
          vita_crt_file_ftell(s_fake_stream, 0) == 5L);
    errno = 0;
    CHECK(vita_crt_file_fseek(s_fake_stream, 0, -1L, SEEK_SET) == -1 &&
          errno == EINVAL && vita_crt_file_ftell(s_fake_stream, 0) == 5L);
    errno = 0;
    CHECK(vita_crt_file_fseek(s_fake_stream, 0, 0L, 99) == -1 &&
          errno == EINVAL);
    CHECK(vita_crt_file_fseek(s_fake_stream, 0, 5L, SEEK_END) == 0 &&
          vita_crt_file_ftell(s_fake_stream, 0) == 15L);

    entry->raw_archive.position = (int64_t)INT32_MAX + 1;
    errno = 0;
    CHECK(vita_crt_file_ftell(s_fake_stream, 0) == -1L &&
          errno == EOVERFLOW);
    entry->raw_archive.position = INT64_MAX - 1;
    errno = 0;
    CHECK(vita_crt_file_fseek(s_fake_stream, 0, 2L, SEEK_CUR) == -1 &&
          errno == EOVERFLOW &&
          entry->raw_archive.position == INT64_MAX - 1);
    CHECK(vita_crt_file_fseek(s_fake_stream, 0, 0L, SEEK_SET) == 0);

    entry->raw_archive.error = 0U;
    errno = EACCES;
    CHECK(vita_crt_file_fwrite(
              output, 0U, 1U, s_fake_stream, 0) == 0U &&
          vita_crt_file_fwrite(
              output, 1U, 0U, s_fake_stream, 0) == 0U &&
          errno == EACCES && !entry->raw_archive.error);
    errno = 0;
    CHECK(vita_crt_file_fwrite(
              output, 1U, 1U, s_fake_stream, 0) == 0U &&
          errno == EBADF && entry->raw_archive.error);
    CHECK(vita_crt_raw_archive_close(entry) == 0 &&
          !vita_crt_dynamic_file_is_live(entry) && !entry->file.key[0] &&
          s_close_calls == 1U);

    CHECK(open_slot(0U) == 0);
    s_close_result = SCE_EIO;
    errno = 0;
    CHECK(vita_crt_raw_archive_close(&s_file_tokens[0]) == EOF &&
          errno == EIO && !vita_crt_dynamic_file_is_live(&s_file_tokens[0]));
    return 0;
}

static int test_sticky_eof_and_rearm(void)
{
    vita_crt_file_token *entry;
    unsigned char output[16];
    unsigned char before[16];
    uint32_t calls;
    CPU cpu;
    uint32_t arguments[4];
    uint32_t esp;
    size_t repeated_result;

    reset_oracle();
    CHECK(open_slot(0U) == 0 && open_slot(1U) == 0);
    entry = &s_file_tokens[0];
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, sizeof output) == 10U &&
          s_pread_calls == 2U && entry->raw_archive.eof &&
          !entry->raw_archive.error && entry->raw_archive.position == 10);
    calls = s_pread_calls;
    memset(output, 0xcc, sizeof output);
    memcpy(before, output, sizeof before);
    errno = EACCES;
    repeated_result = vita_crt_raw_archive_fread(entry, output, 1U, 4U);
    if (s_pread_calls != calls)
        fprintf(stderr, "sticky EOF issued extra pread: before=%u after=%u\n",
                (unsigned)calls, (unsigned)s_pread_calls);
    CHECK(repeated_result == 0U &&
          s_pread_calls == calls && errno == EACCES &&
          entry->raw_archive.eof && !entry->raw_archive.error &&
          entry->raw_archive.position == 10 &&
          memcmp(output, before, sizeof output) == 0);

    /* EOF is sticky: a hypothetical later kernel failure must not become a
     * new stream error until a successful positioning operation rearms IO. */
    s_fail_pread_call = calls + 1U;
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 4U) == 0U &&
          s_pread_calls == calls && errno == EACCES && !entry->raw_archive.error);
    CHECK(vita_crt_raw_archive_fseek(entry, -1L, SEEK_SET) == -1 &&
          errno == EINVAL && entry->raw_archive.eof && entry->raw_archive.position == 10);
    CHECK(vita_crt_raw_archive_fseek(entry, 0L, 99) == -1 &&
          errno == EINVAL && entry->raw_archive.eof && entry->raw_archive.position == 10);
    errno = EACCES;
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 4U) == 0U &&
          s_pread_calls == calls && errno == EACCES && !entry->raw_archive.error);

    /* The public import still consumes just its cdecl return and preserves
     * the host errno and existing guest errno on an ordinary EOF result. */
    arguments[0] = (uint32_t)(uintptr_t)s_guest_output;
    arguments[1] = 1U; arguments[2] = 1U; arguments[3] = entry->token;
    esp = prepare_cdecl(&cpu, arguments, 4U);
    g_isaac_vita_crt_errno = EINVAL;
    vita_crt_fread(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          s_pread_calls == calls && errno == EACCES &&
          g_isaac_vita_crt_errno == EINVAL && s_file_lock == 0U);

    /* Another live token retains its own cursor and is not suppressed. */
    s_fail_pread_call = 0U;
    CHECK(vita_crt_raw_archive_fread(&s_file_tokens[1], output, 1U, 2U) == 2U &&
          memcmp(output, "ab", 2U) == 0 && s_file_tokens[1].raw_archive.position == 2 &&
          !s_file_tokens[1].raw_archive.eof && entry->raw_archive.position == 10);
    calls = s_pread_calls;
    CHECK(vita_crt_raw_archive_fseek(entry, 0L, SEEK_CUR) == 0 &&
          !entry->raw_archive.eof && entry->raw_archive.position == 10);
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 1U) == 0U &&
          s_pread_calls == calls + 1U && entry->raw_archive.eof);
    CHECK(vita_crt_raw_archive_fseek(entry, 0L, SEEK_SET) == 0 &&
          !entry->raw_archive.eof);
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 3U) == 3U &&
          memcmp(output, "abc", 3U) == 0 && entry->raw_archive.position == 3);
    CHECK(vita_crt_raw_archive_fseek(entry, -2L, SEEK_END) == 0 &&
          !entry->raw_archive.eof && entry->raw_archive.position == 8);
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 3U) == 2U &&
          memcmp(output, "ij", 2U) == 0 && entry->raw_archive.eof);

    /* Keep the pre-existing zero-size/overflow ordering ahead of EOF. */
    calls = s_pread_calls;
    errno = EACCES;
    CHECK(vita_crt_raw_archive_fread(entry, output, 0U, 3U) == 0U &&
          vita_crt_raw_archive_fread(entry, output, 3U, 0U) == 0U &&
          s_pread_calls == calls && entry->raw_archive.eof &&
          !entry->raw_archive.error && errno == EACCES);
    CHECK(vita_crt_raw_archive_fread(entry, output, UINT32_MAX, 2U) == 0U &&
          s_pread_calls == calls && entry->raw_archive.eof &&
          entry->raw_archive.error && errno == EOVERFLOW);
    errno = EACCES;
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 1U) == 0U &&
          s_pread_calls == calls && entry->raw_archive.eof &&
          entry->raw_archive.error && errno == EACCES);
    entry->raw_archive.position = INT64_MAX - 1;
    entry->raw_archive.error = 0U;
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 2U) == 0U &&
          s_pread_calls == calls && entry->raw_archive.eof &&
          entry->raw_archive.error && errno == EOVERFLOW);
    CHECK(vita_crt_raw_archive_fseek(entry, 2L, SEEK_CUR) == -1 &&
          errno == EOVERFLOW && entry->raw_archive.eof &&
          entry->raw_archive.position == INT64_MAX - 1);

    /* Sticky error alone is NOT an EOF shortcut; successful Seek clears
     * EOF, not error. A re-open clears both, as for a fresh native stream. */
    CHECK(vita_crt_raw_archive_fseek(entry, 0L, SEEK_SET) == 0 &&
          !entry->raw_archive.eof && entry->raw_archive.error);
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 2U) == 2U &&
          s_pread_calls == calls + 1U && entry->raw_archive.error &&
          memcmp(output, "ab", 2U) == 0);
    entry->raw_archive.eof = 1U;
    entry->raw_archive.live = 0U;
    CHECK(vita_crt_raw_archive_fread(entry, output, 1U, 1U) == 0U && errno == EBADF);
    entry->raw_archive.live = 1U;
    CHECK(vita_crt_raw_archive_close(entry) == 0 && open_slot(0U) == 0 &&
          !entry->raw_archive.eof && !entry->raw_archive.error);
    return 0;
}

static int test_vfprintf_rejects_raw_token(void)
{
    vita_crt_file_token *entry;
    CPU cpu;
    uint32_t esp;

    reset_oracle();
    CHECK(open_slot(0U) == 0);
    entry = &s_file_tokens[0];
    memset(&cpu, 0, sizeof cpu);
    memset(s_guest_stack, 0, sizeof s_guest_stack);
    esp = (uint32_t)(uintptr_t)&s_guest_stack[24];
    CHECK((uintptr_t)&s_guest_stack[64] <= UINT32_MAX);
    st32(esp, UINT32_C(0xaabbccdd));
    st32(esp + 4U, 0U);
    st32(esp + 8U, 0U);
    st32(esp + 12U, entry->token);
    st32(esp + 16U, 0U);
    st32(esp + 20U, 0U);
    st32(esp + 24U, 0U);
    cpu.esp = esp;
    cpu.stack_owner = &cpu;
    cpu.stack_floor = (uint32_t)(uintptr_t)&s_guest_stack[0];
    cpu.stack_ceiling = (uint32_t)(uintptr_t)&s_guest_stack[64];
    cpu.stack_low_water = cpu.stack_ceiling;
    g_isaac_vita_crt_errno = EINVAL;
    errno = EACCES;

    vita_crt_stdio_common_vfprintf(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == UINT32_MAX &&
          entry->raw_archive.live && entry->raw_archive.error &&
          g_isaac_vita_crt_errno == EBADF && errno == EACCES &&
          s_file_lock == 0U && s_pread_calls == 0U);
    return 0;
}

static int test_sixteen_slots_and_independent_positions(void)
{
    uint32_t index;
    uint32_t other;

    reset_oracle();
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        vita_crt_file_token *entry = &s_file_tokens[index];

        vita_crt_raw_archive_clear(entry);
        entry->raw_archive.descriptor = (int32_t)(200U + index);
        entry->raw_archive.live = 1U;
        entry->raw_archive.size = (int64_t)(sizeof s_bytes - 1U);
        strcpy(entry->file.key, "afterbirthp.a");
        CHECK((uintptr_t)entry <= UINT32_MAX);
        entry->token = (uint32_t)(uintptr_t)entry;
        CHECK(vita_crt_dynamic_file_is_live(entry) &&
              vita_crt_dynamic_stream(entry) == NULL);
        for (other = 0U; other < index; ++other)
            CHECK(entry->token != s_file_tokens[other].token);
    }
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        FILE *stream = (FILE *)(uintptr_t)1U;
        int found_index = -1;

        CHECK(vita_crt_find_file_token_unlocked(
                  s_file_tokens[index].token, &stream,
                  &found_index, NULL));
        CHECK(stream == NULL && found_index == (int)index);
        CHECK(!isaac_vita_crt_osfhandle_is_owned(
                  (uint32_t)s_file_tokens[index].raw_archive.descriptor));
    }
    s_file_tokens[0].raw_archive.position = 3;
    s_file_tokens[1].raw_archive.position = 7;
    CHECK(vita_crt_raw_archive_ftell(&s_file_tokens[0]) == 3L &&
          vita_crt_raw_archive_ftell(&s_file_tokens[1]) == 7L);
    return 0;
}

/* fread with the measured adapter frame: [ESP] = the adapter's return,
 * [EBP+4] = the ArchivedFile header edge that owns the four-byte request. */
static uint32_t prepare_archive_header_fread(CPU *cpu, uint32_t token,
                                             uint32_t parent)
{
    uint32_t arguments[4] = {
        (uint32_t)(uintptr_t)s_guest_output, 4U, 1U, token
    };
    uint32_t esp = prepare_cdecl(cpu, arguments, 4U);
    uint32_t ebp = (uint32_t)(uintptr_t)&s_guest_stack[40];

    st32(esp, ISAAC_VITA_CRT_FREAD_RETURN_RVA);
    cpu->ebp = ebp;
    st32(ebp + 4U, parent);
    return esp;
}

/* SceIofilemgr invalidated the descriptor (suspend/resume): the pread under
 * the exact ArchivedFile header read fails with SCE_ERROR_ERRNO_ENODEV.
 * With ISAAC_VITA_CRT_DESCRIPTOR_RECOVER the token is reopened once at its
 * cursor and the read redone; without it the pre-existing fatal path runs. */
static int test_descriptor_enodev_on_header_read(void)
{
    uint32_t fopen_arguments[2] = {
        1U, (uint32_t)(uintptr_t)s_guest_mode_rb
    };
    uint32_t fread_arguments[4] = {
        (uint32_t)(uintptr_t)s_guest_output, 1U, 4U, 0U
    };
    uint32_t one_argument[1];
    vita_crt_file_token *entry = &s_file_tokens[0];
    CPU cpu;
    uint32_t esp;
    uint32_t token;
    uint32_t logs;
    int32_t descriptor;

    reset_oracle();
    s_pread_source = s_header_bytes;
    esp = prepare_cdecl(&cpu, fopen_arguments, 2U);
    vita_crt_fopen(&cpu);
    token = cpu.eax;
    CHECK(!cpu.fault && cpu.esp == esp + 4U && token != 0U &&
          entry->raw_archive.live && s_open_calls == 1U);
    descriptor = entry->raw_archive.descriptor;
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    CHECK(strcmp(entry->recover.path, AFTERBIRTHP) == 0 &&
          strcmp(entry->recover.mode, "rb") == 0);
#endif

    fread_arguments[3] = token;
    esp = prepare_cdecl(&cpu, fread_arguments, 4U);
    vita_crt_fread(&cpu);
    CHECK(!cpu.fault && cpu.eax == 4U && entry->raw_archive.position == 4 &&
          s_pread_calls == 1U);

    /* The header read at offset 4: its pread fails with ENODEV. */
    s_fail_pread_call = s_pread_calls + 1U;
    s_fail_pread_result = SCE_ENODEV;
    logs = s_diag_logs;
    g_isaac_vita_crt_errno = EINVAL;
    memset(s_guest_output, 0xcc, sizeof s_guest_output);
    esp = prepare_archive_header_fread(
        &cpu, token, ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA);
    errno = EACCES;
    vita_crt_fread(&cpu);
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    /* Recovered: one fresh open+size, the dead descriptor closed after it,
     * the exact header word delivered, cursor advanced, no error flag, no
     * fault, one receipt line, host and guest errno untouched. */
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 1U &&
          ld32((uint32_t)(uintptr_t)s_guest_output) == UINT32_C(0x80000004) &&
          entry->raw_archive.live && entry->raw_archive.position == 8 &&
          !entry->raw_archive.error && !entry->raw_archive.eof &&
          entry->raw_archive.descriptor != descriptor &&
          entry->token == token && strcmp(entry->file.key, "afterbirthp.a") == 0 &&
          s_open_calls == 2U && s_size_calls == 2U && s_close_calls == 1U &&
          s_pread_calls == 3U && s_reopen_native_calls == 0U &&
          s_diag_logs == logs + 1U && errno == EACCES &&
          g_isaac_vita_crt_errno == EINVAL && s_file_lock == 0U);
    CHECK(s_descriptor_recover.enodev == 1U &&
          s_descriptor_recover.recovered == 1U &&
          s_descriptor_recover.unknown_pos == 0U &&
          s_descriptor_recover.reopen_fail == 0U &&
          s_descriptor_recover.redo_fail == 0U);
    descriptor = entry->raw_archive.descriptor;

    /* The fresh descriptor serves the following read at the right cursor. */
    fread_arguments[0] = (uint32_t)(uintptr_t)&s_guest_output[4];
    fread_arguments[1] = 1U;
    fread_arguments[2] = 2U;
    esp = prepare_cdecl(&cpu, fread_arguments, 4U);
    vita_crt_fread(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 2U &&
          memcmp(&s_guest_output[4], "ij", 2U) == 0 &&
          entry->raw_archive.position == 10 && !entry->raw_archive.eof);

    /* A second death whose reopen fails (ENOENT): the token is untouched,
     * the ordinary fatal header path runs, the guest errno is ENODEV. */
    CHECK(vita_crt_raw_archive_fseek(entry, 4L, SEEK_SET) == 0);
    s_open_result = SCE_ENOENT;
    s_fail_pread_call = s_pread_calls + 1U;
    g_isaac_vita_crt_errno = EINVAL;
    esp = prepare_archive_header_fread(
        &cpu, token, ISAAC_VITA_CRT_ARCHIVE_TYPE1_HEADER_RETURN_RVA);
    errno = EACCES;
    vita_crt_fread(&cpu);
    CHECK(cpu.fault &&
          strcmp(cpu.fault, "ArchivedFile block header is invalid") == 0 &&
          cpu.esp == esp && entry->raw_archive.live &&
          entry->raw_archive.error && entry->raw_archive.position == 4 &&
          entry->raw_archive.descriptor == descriptor &&
          s_open_calls == 3U && s_size_calls == 2U && s_close_calls == 1U &&
          strcmp(s_last_diag_reason, "header-bound") == 0 &&
          g_isaac_vita_crt_errno == ENODEV && errno == EACCES &&
          s_file_lock == 0U &&
          s_descriptor_recover.enodev == 2U &&
          s_descriptor_recover.recovered == 1U &&
          s_descriptor_recover.reopen_fail == 1U);
    s_open_result = 0;

    /* A fresh descriptor whose size differs from the one recorded at open
     * is refused and released. */
    entry->raw_archive.error = 0U;
    CHECK(vita_crt_raw_archive_fseek(entry, 4L, SEEK_SET) == 0);
    s_size_override = 11;
    s_fail_pread_call = s_pread_calls + 1U;
    g_isaac_vita_crt_errno = EINVAL;
    esp = prepare_archive_header_fread(
        &cpu, token, ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA);
    vita_crt_fread(&cpu);
    CHECK(cpu.fault && cpu.esp == esp &&
          entry->raw_archive.descriptor == descriptor &&
          entry->raw_archive.error && entry->raw_archive.position == 4 &&
          s_open_calls == 4U && s_size_calls == 3U && s_close_calls == 2U &&
          s_descriptor_recover.reopen_fail == 2U &&
          g_isaac_vita_crt_errno == ENODEV && s_file_lock == 0U);
    s_size_override = -1;

    /* A plain multi-element read dying mid-way (one complete element
     * already delivered) is redone whole: identical bytes, exact cursor.
     * The sticky error flag left by the refused recoveries above is kept,
     * exactly as newlib keeps __SERR across a successful read. */
    CHECK(vita_crt_raw_archive_fseek(entry, 2L, SEEK_SET) == 0 &&
          entry->raw_archive.error);
    s_max_chunk = 2U;
    logs = s_pread_calls;
    s_fail_pread_call = s_pread_calls + 2U;
    fread_arguments[0] = (uint32_t)(uintptr_t)s_guest_output;
    fread_arguments[1] = 2U;
    fread_arguments[2] = 3U;
    esp = prepare_cdecl(&cpu, fread_arguments, 4U);
    memset(s_guest_output, 0xcc, sizeof s_guest_output);
    g_isaac_vita_crt_errno = EINVAL;
    errno = EACCES;
    vita_crt_fread(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 3U);
    CHECK(memcmp(s_guest_output, s_header_bytes + 2, 6U) == 0);
    CHECK(entry->raw_archive.position == 8 && entry->raw_archive.error &&
          entry->raw_archive.descriptor != descriptor);
    CHECK(s_pread_calls == logs + 5U);
    CHECK(s_open_calls == 5U && s_size_calls == 4U && s_close_calls == 3U);
    CHECK(s_descriptor_recover.enodev == 4U &&
          s_descriptor_recover.recovered == 2U &&
          s_descriptor_recover.reopen_fail == 2U);
    CHECK(errno == EACCES && g_isaac_vita_crt_errno == EINVAL);
    s_max_chunk = UINT32_MAX;

    /* The counter line, then fclose releases the fresh descriptor and the
     * retained path with it. */
    logs = s_diag_logs;
    isaac_vita_crt_descriptor_recover_report("oracle");
    CHECK(s_diag_logs == logs + 1U);
    one_argument[0] = token;
    esp = prepare_cdecl(&cpu, one_argument, 1U);
    vita_crt_fclose(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          s_close_calls == 4U && !vita_crt_dynamic_file_is_live(entry) &&
          !entry->recover.path[0] && !entry->recover.mode[0]);
#else
    /* The pre-existing fatal path: the header word never arrived. */
    CHECK(cpu.fault &&
          strcmp(cpu.fault, "ArchivedFile block header is invalid") == 0 &&
          cpu.esp == esp && entry->raw_archive.live &&
          entry->raw_archive.error && entry->raw_archive.position == 4 &&
          entry->raw_archive.descriptor == descriptor &&
          s_open_calls == 1U && s_size_calls == 1U && s_close_calls == 0U &&
          s_pread_calls == 2U && s_reopen_native_calls == 0U &&
          strcmp(s_last_diag_reason, "header-bound") == 0 &&
          g_isaac_vita_crt_errno == ENODEV && errno == EACCES &&
          s_file_lock == 0U);
    (void)logs;
    one_argument[0] = token;
    esp = prepare_cdecl(&cpu, one_argument, 1U);
    vita_crt_fclose(&cpu);
    CHECK(!cpu.fault && cpu.esp == esp + 4U && cpu.eax == 0U &&
          s_close_calls == 1U && !vita_crt_dynamic_file_is_live(entry));
#endif
    return 0;
}

#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
static int test_recover_publish_bounds(void)
{
    static char long_path[ISAAC_VITA_STARTUP_PATH_MAX + 8U];
    vita_crt_file_token *entry = &s_file_tokens[0];

    reset_oracle();
    memset(long_path, 'p', sizeof long_path - 1U);
    long_path[sizeof long_path - 1U] = '\0';
    vita_crt_recover_publish(entry, "rb", long_path);
    CHECK(!entry->recover.path[0] && !entry->recover.mode[0]);
    vita_crt_recover_publish(entry, "wb", AFTERBIRTHP);
    CHECK(!entry->recover.path[0]);
    vita_crt_recover_publish(entry, "r+", AFTERBIRTHP);
    CHECK(!entry->recover.path[0]);
    vita_crt_recover_publish(entry, "a", AFTERBIRTHP);
    CHECK(!entry->recover.path[0]);
    vita_crt_recover_publish(entry, "r", AFTERBIRTHP);
    CHECK(strcmp(entry->recover.path, AFTERBIRTHP) == 0 &&
          strcmp(entry->recover.mode, "r") == 0);
    vita_crt_recover_clear(entry);
    CHECK(!entry->recover.path[0] && !entry->recover.mode[0]);
    return 0;
}
#endif

int main(void)
{
    CHECK(test_strict_fallback_gate() == 0);
    CHECK(test_persistent_music_raw_stream() == 0);
    CHECK(test_outer_fopen_receipt_and_table_full() == 0);
    CHECK(test_short_reads_and_element_overlap() == 0);
    CHECK(test_seek_stdio_surface_and_close() == 0);
    CHECK(test_sticky_eof_and_rearm() == 0);
    CHECK(test_vfprintf_rejects_raw_token() == 0);
    CHECK(test_sixteen_slots_and_independent_positions() == 0);
    CHECK(test_descriptor_enodev_on_header_read() == 0);
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    CHECK(test_recover_publish_bounds() == 0);
    puts("Vita CRT raw packed-archive ENOMEM fallback host oracle "
         "(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER): PASS");
#else
    puts("Vita CRT raw packed-archive ENOMEM fallback host oracle: PASS");
#endif
    return 0;
}
