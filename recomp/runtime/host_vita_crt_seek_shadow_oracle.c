/* Host oracle for ISAAC_VITA_CRT_SEEK_SHADOW (read-only SEEK_END elision).
 *
 * Includes the production CRT TU so the exact private token layout, lane
 * dispatchers and import handlers are exercised.  The guest stdio imports
 * (fopen/fseek/ftell/fread/fwrite/fflush/fclose) are driven through a fake
 * x86 stack against a real temporary file, and every result is compared with
 * a plain libc FILE following the same operation sequence: that plain FILE is
 * the newlib-semantics reference the shadow must be indistinguishable from.
 * The three oracle seams count how many real fseek/ftell/fstat calls the
 * newlib lane issues, which pins the syscall savings.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "guest.h"

#define ISAAC_VITA_ARCHIVE_FILE_CACHE 1
#define ISAAC_VITA_CRT_QSORT_HOST_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX INT32_MAX
#define ISAAC_VITA_CRT_SEEK_SHADOW 1
#define ISAAC_VITA_CRT_SEEK_SHADOW_ORACLE 1
#include "host_vita_crt.c"

#define ROOT "ux0:/data/isaacr001"
#define PACKED ROOT "/resources/packed/"
#define AFTERBIRTHP PACKED "afterbirthp.a"
#define LOOSE_XML ROOT "/resources/rooms/00.special rooms.stb"
#define GAMESTATE ROOT "/Documents/rep_gamestate1.dat"

#define SCE_ENOENT ((int32_t)UINT32_C(0x80010002))
#define SCE_EIO    ((int32_t)UINT32_C(0x80010005))

/* Not a multiple of the 16 KiB FILE buffer or of the 1 KiB seek block. */
#define FIXTURE_BYTES 100003U
#define READ_CAPACITY 65536U

static const unsigned char s_raw_bytes[] = "abcdefghij";
static char s_mode_rb[] = "rb";
static char s_mode_r[] = "r";
static char s_mode_rplus[] = "r+";
static char s_mode_wb[] = "wb";
static char s_mode_a[] = "a";
_Alignas(16) static uint32_t s_guest_stack[64];
_Alignas(16) static unsigned char s_guest_read[READ_CAPACITY];
_Alignas(16) static unsigned char s_reference_read[READ_CAPACITY];
static unsigned char s_fixture[FIXTURE_BYTES];
static char s_fixture_path[1024];
static char s_scratch_path[1024];
/* A fixture copy that the writer-side cases append to; when set, every
 * fake open (any mode) lands on it. */
static char s_append_path[1024];
static const char *s_open_path_override;
static const char *s_mapped_path;
static int s_force_fopen_errno;
static uint32_t s_native_fopen_calls;
static uint32_t s_raw_open_calls;
static uint32_t s_raw_close_calls;
static uint32_t s_diag_records;
static uint32_t s_diag_logs;
static isaac_vita_archive_diag_event s_last_diag;
static char s_last_diag_reason[32];
static uint32_t s_log_lines;
static char s_log_last[2][512];
static uint32_t s_real_fseek[3];
static uint32_t s_real_fseek_other;
static uint32_t s_real_ftell;
static uint32_t s_real_fstat;
static int s_fstat_fail;

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "seek shadow oracle failed at line %u: %s\n",
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

/* ---- oracle seams counted by the shadow ------------------------------- */

int isaac_vita_crt_seek_shadow_oracle_fseek(FILE *stream, long offset,
                                            int origin)
{
    if (origin == SEEK_SET || origin == SEEK_CUR || origin == SEEK_END)
        ++s_real_fseek[origin];
    else
        ++s_real_fseek_other;
    return fseek(stream, offset, origin);
}

long isaac_vita_crt_seek_shadow_oracle_ftell(FILE *stream)
{
    ++s_real_ftell;
    return ftell(stream);
}

int isaac_vita_crt_seek_shadow_oracle_fstat(int descriptor,
                                            struct stat *status)
{
    ++s_real_fstat;
    if (s_fstat_fail) {
        errno = EBADF;
        return -1;
    }
    return fstat(descriptor, status);
}

static void reset_seam_counts(void)
{
    memset(s_real_fseek, 0, sizeof s_real_fseek);
    s_real_fseek_other = 0U;
    s_real_ftell = 0U;
    s_real_fstat = 0U;
}

/* ---- archive cache / diag / platform stubs ----------------------------- */

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

/* The production cache is one retained newlib FILE; here every open is a
 * fresh libc FILE on the fixture (read modes) or the scratch file (write
 * modes) so the shadow is measured against libc alone. */
FILE *isaac_vita_archive_cache_open(isaac_vita_archive_cache_file *file,
                                    const char *native_path,
                                    const char *mode, int *cache_hit)
{
    FILE *stream;

    ++s_native_fopen_calls;
    memset(file, 0, sizeof *file);
    if (cache_hit)
        *cache_hit = 0;
    if (s_force_fopen_errno) {
        errno = s_force_fopen_errno;
        return NULL;
    }
    stream = fopen(s_open_path_override ? s_open_path_override :
                   mode[0] == 'r' ? s_fixture_path : s_scratch_path, mode);
    if (!stream)
        return NULL;
    file->stream = stream;
    (void)isaac_vita_archive_raw_fallback_key(native_path, mode, file->key);
    return stream;
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
    int result = EOF;

    if (file->stream)
        result = fclose(file->stream);
    else
        errno = EBADF;
    memset(file, 0, sizeof *file);
    return result;
}

int isaac_vita_archive_cache_drop(void)
{
    return 0;
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
    length = snprintf(line, capacity, "arcdiag-oracle");
    return length >= 0 && (size_t)length < capacity ? length : -1;
}

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char *slot = s_log_last[s_log_lines & 1U];

    va_start(arguments, format);
    (void)vsnprintf(slot, sizeof s_log_last[0], format, arguments);
    va_end(arguments);
    ++s_log_lines;
    if (strncmp(slot, "KAGE VITA CRT SEEK SHADOW", 25U) == 0)
        printf("%s\n", slot);
    else
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
    ++s_raw_open_calls;
    if (strcmp(path, AFTERBIRTHP) != 0)
        return SCE_ENOENT;
    return 100;
}

int32_t isaac_vita_crt_raw_archive_oracle_get_size(
    int32_t descriptor, int64_t *size)
{
    if (descriptor < 100 || !size)
        return SCE_EIO;
    *size = (int64_t)(sizeof s_raw_bytes - 1U);
    return 0;
}

int32_t isaac_vita_crt_raw_archive_oracle_pread(
    int32_t descriptor, void *buffer, uint32_t size, uint64_t offset)
{
    uint32_t available;
    uint32_t result;

    if (descriptor < 100)
        return SCE_EIO;
    if (offset >= sizeof s_raw_bytes - 1U)
        return 0;
    available = (uint32_t)(sizeof s_raw_bytes - 1U - offset);
    result = size < available ? size : available;
    memcpy(buffer, s_raw_bytes + offset, result);
    return (int32_t)result;
}

int32_t isaac_vita_crt_raw_archive_oracle_close(int32_t descriptor)
{
    ++s_raw_close_calls;
    return descriptor < 100 ? SCE_EIO : 0;
}

/* ---- guest import drivers --------------------------------------------- */

static void reset_oracle(void)
{
    memset(s_file_tokens, 0, sizeof s_file_tokens);
    memset(&s_seek_shadow, 0, sizeof s_seek_shadow);
    s_seek_shadow_write_gen = 0U;
    s_seek_shadow_since_report = 0U;
    s_file_lock = 0U;
    s_archive_diag_logged_first_relevant = 0U;
    s_archive_diag_logged_first_direct = 0U;
    s_mapped_path = LOOSE_XML;
    s_open_path_override = NULL;
    s_force_fopen_errno = 0;
    s_native_fopen_calls = 0U;
    s_raw_open_calls = 0U;
    s_raw_close_calls = 0U;
    s_diag_records = 0U;
    s_diag_logs = 0U;
    s_fstat_fail = 0;
    memset(&s_last_diag, 0, sizeof s_last_diag);
    s_last_diag_reason[0] = '\0';
    reset_seam_counts();
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

/* Every driver returns 0 on a clean cdecl return (no fault, one word
 * popped) and stores the guest-visible EAX. */
static int drive_fopen(char *mode, uint32_t *token)
{
    uint32_t arguments[2] = { 1U, (uint32_t)(uintptr_t)mode };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 2U);

    vita_crt_fopen(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *token = cpu.eax;
    return 0;
}

static int drive_fclose(uint32_t token, int32_t *result)
{
    uint32_t arguments[1] = { token };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 1U);

    vita_crt_fclose(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = (int32_t)cpu.eax;
    return 0;
}

/* site = the import return word at [ESP]; parent/grand populate the frame
 * File::GetSize would own ([EBP+4] and [EBP+16]) when site is its return. */
static int drive_fseek_at(uint32_t token, int32_t offset, int origin,
                          uint32_t site, uint32_t parent, uint32_t grand,
                          int32_t *result)
{
    uint32_t arguments[3] = { token, (uint32_t)offset, (uint32_t)origin };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 3U);
    uint32_t ebp = (uint32_t)(uintptr_t)&s_guest_stack[40];

    if (site) {
        st32(esp, site);
        cpu.ebp = ebp;
        st32(ebp + 4U, parent);
        st32(ebp + 16U, grand);
    }
    vita_crt_fseek(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = (int32_t)cpu.eax;
    return 0;
}

static int drive_fseek(uint32_t token, int32_t offset, int origin,
                       int32_t *result)
{
    return drive_fseek_at(token, offset, origin, 0U, 0U, 0U, result);
}

static int drive_ftell(uint32_t token, int32_t *result)
{
    uint32_t arguments[1] = { token };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 1U);

    vita_crt_ftell(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = (int32_t)cpu.eax;
    return 0;
}

static int drive_fread(uint32_t token, uint32_t size, uint32_t count,
                       uint32_t *result)
{
    uint32_t arguments[4] = {
        (uint32_t)(uintptr_t)s_guest_read, size, count, token
    };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 4U);

    vita_crt_fread(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = cpu.eax;
    return 0;
}

static int drive_fwrite(uint32_t token, uint32_t size, uint32_t count,
                        uint32_t *result)
{
    uint32_t arguments[4] = {
        (uint32_t)(uintptr_t)s_guest_read, size, count, token
    };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 4U);

    vita_crt_fwrite(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = cpu.eax;
    return 0;
}

static int drive_fflush(uint32_t token, int32_t *result)
{
    uint32_t arguments[1] = { token };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 1U);

    vita_crt_fflush(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = (int32_t)cpu.eax;
    return 0;
}

/* The frozen File::GetSize body: ftell; fseek(0, SEEK_END); ftell;
 * fseek(pos, SEEK_SET); return size.  IsEOF is Tell() >= GetSize(). */
static int drive_getsize(uint32_t token, uint32_t site_parent,
                         uint32_t grand, int32_t *size)
{
    int32_t position;
    int32_t result;

    if (drive_ftell(token, &position) != 0 || position < 0)
        return -1;
    if (drive_fseek_at(token, 0, SEEK_END,
                       site_parent ? ISAAC_VITA_CRT_FSEEK_FIRST_RETURN_RVA : 0U,
                       site_parent, grand, &result) != 0 || result != 0)
        return -1;
    if (drive_ftell(token, size) != 0 || *size < 0)
        return -1;
    if (drive_fseek(token, position, SEEK_SET, &result) != 0 || result != 0)
        return -1;
    return 0;
}

static long reference_getsize(FILE *stream)
{
    long position = ftell(stream);
    long size;

    if (position < 0 || fseek(stream, 0L, SEEK_END) != 0)
        return -1L;
    size = ftell(stream);
    if (size < 0 || fseek(stream, position, SEEK_SET) != 0)
        return -1L;
    return size;
}

static FILE *reference_open(const char *mode)
{
    FILE *stream = fopen(mode[0] == 'r' ? s_fixture_path : s_scratch_path,
                         mode);

    if (stream && mode[0] == 'r')
        (void)setvbuf(stream, NULL, _IOFBF,
                      ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
    return stream;
}

static int token_index(uint32_t token)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i) {
        if (vita_crt_dynamic_file_is_live(&s_file_tokens[i]) &&
            s_file_tokens[i].token == token)
            return (int)i;
    }
    return -1;
}

/* ---- tests -------------------------------------------------------------- */

static int test_getsize_fresh_and_cached(void)
{
    uint32_t token;
    int32_t size;
    int32_t position;
    uint32_t got;
    int32_t result;
    int index;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0 && s_file_tokens[index].seek_shadow.read_only &&
          strcmp(s_file_tokens[index].seek_shadow.name,
                 "00.special rooms.stb") == 0);
    reset_seam_counts();

    /* Fresh FILE: one fstat, the SEEK_END itself never reaches libc, the
     * back-seek is the only real fseek. */
    CHECK(drive_getsize(token, 0U, 0U, &size) == 0 &&
          size == (int32_t)FIXTURE_BYTES);
    CHECK(s_real_fstat == 1U && s_real_fseek[SEEK_END] == 0U &&
          s_real_fseek[SEEK_SET] == 1U && s_real_ftell == 1U);
    CHECK(s_seek_shadow.seek_end == 1U && s_seek_shadow.elided == 1U &&
          s_seek_shadow.fstat_calls == 1U && s_seek_shadow.ftell_pending == 1U &&
          s_seek_shadow.set_pending == 1U && s_seek_shadow.applied == 0U &&
          !s_file_tokens[index].seek_shadow.pending_end);

    CHECK(drive_fread(token, 1U, 16U, &got) == 0 && got == 16U &&
          memcmp(s_guest_read, s_fixture, 16U) == 0);
    CHECK(drive_ftell(token, &position) == 0 && position == 16);

    /* Second GetSize on the same token: the size is cached, so no fstat. */
    CHECK(drive_getsize(token, 0U, 0U, &size) == 0 &&
          size == (int32_t)FIXTURE_BYTES);
    CHECK(s_real_fstat == 1U && s_real_fseek[SEEK_END] == 0U &&
          s_seek_shadow.seek_end == 2U && s_seek_shadow.elided == 2U &&
          s_seek_shadow.fstat_calls == 1U);
    CHECK(drive_fread(token, 4U, 4U, &got) == 0 && got == 4U &&
          memcmp(s_guest_read, s_fixture + 16U, 16U) == 0);
    CHECK(drive_ftell(token, &position) == 0 && position == 32);

    CHECK(drive_fclose(token, &result) == 0 && result == 0 &&
          !vita_crt_dynamic_file_is_live(&s_file_tokens[index]) &&
          !s_file_tokens[index].seek_shadow.read_only &&
          s_file_tokens[index].seek_shadow.name[0] == '\0');
    return 0;
}

static int test_iseof_loop_matches_reference(void)
{
    uint32_t token;
    FILE *reference;
    int32_t size;
    int32_t position;
    uint32_t got;
    int32_t result;
    unsigned iteration;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    reference = reference_open("rb");
    CHECK(reference != NULL);
    reset_seam_counts();

    /* The gamestate readers: one field read, then IsEOF, hundreds of times.
     * 1,000-byte reads cross the 16 KiB buffer edge repeatedly; the last
     * iterations run past EOF. */
    for (iteration = 0U; iteration < 120U; ++iteration) {
        long reference_position;
        long reference_size;
        size_t reference_got;

        CHECK(drive_fread(token, 1U, 1000U, &got) == 0);
        reference_got = fread(s_reference_read, 1U, 1000U, reference);
        CHECK(got == (uint32_t)reference_got &&
              memcmp(s_guest_read, s_reference_read, reference_got) == 0);

        CHECK(drive_ftell(token, &position) == 0);
        reference_position = ftell(reference);
        CHECK(position == (int32_t)reference_position);
        CHECK(drive_getsize(token, 0U, 0U, &size) == 0);
        reference_size = reference_getsize(reference);
        CHECK(size == (int32_t)reference_size &&
              size == (int32_t)FIXTURE_BYTES);
        CHECK((position >= size) == (reference_position >= reference_size));
        CHECK(drive_ftell(token, &position) == 0 &&
              position == (int32_t)ftell(reference));
    }
    CHECK(position == (int32_t)FIXTURE_BYTES);
    CHECK(s_real_fstat == 1U && s_real_fseek[SEEK_END] == 0U &&
          s_seek_shadow.seek_end == 120U && s_seek_shadow.elided == 120U &&
          s_seek_shadow.applied == 0U && s_seek_shadow.rearmed == 0U);
    /* 120 back-seeks are the only real seeks the lane issued. */
    CHECK(s_real_fseek[SEEK_SET] == 120U && s_real_fseek[SEEK_CUR] == 0U);

    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    (void)fclose(reference);
    return 0;
}

static int test_iseof_at_buffer_boundary(void)
{
    uint32_t token;
    FILE *reference;
    int32_t size;
    int32_t position;
    uint32_t got;
    int32_t result;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    reference = reference_open("rb");
    CHECK(reference != NULL);
    CHECK(drive_fread(token, 1U, ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE, &got) == 0 &&
          got == ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
    CHECK(fread(s_reference_read, 1U, ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE,
                reference) == ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
    reset_seam_counts();
    CHECK(drive_getsize(token, 0U, 0U, &size) == 0 &&
          size == (int32_t)reference_getsize(reference));
    CHECK(s_real_fseek[SEEK_END] == 0U);
    CHECK(drive_fread(token, 1U, 8U, &got) == 0 && got == 8U &&
          fread(s_reference_read, 1U, 8U, reference) == 8U &&
          memcmp(s_guest_read, s_reference_read, 8U) == 0 &&
          memcmp(s_guest_read,
                 s_fixture + ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE, 8U) == 0);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(reference));
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    (void)fclose(reference);
    return 0;
}

static int test_pending_consumers(void)
{
    uint32_t token;
    FILE *reference;
    int32_t position;
    uint32_t got;
    int32_t result;
    int index;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    reference = reference_open("rb");
    CHECK(reference != NULL);

    /* fread after a pending SEEK_END(0): zero items, EOF, identical cursor. */
    reset_seam_counts();
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          s_file_tokens[index].seek_shadow.pending_end &&
          fseek(reference, 0L, SEEK_END) == 0);
    CHECK(drive_fread(token, 1U, 10U, &got) == 0 && got == 0U &&
          fread(s_reference_read, 1U, 10U, reference) == 0U &&
          feof(reference));
    CHECK(!s_file_tokens[index].seek_shadow.pending_end &&
          s_seek_shadow.applied == 1U && s_real_fseek[SEEK_END] == 1U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(reference) &&
          position == (int32_t)FIXTURE_BYTES);

    /* SEEK_END(-10) then fread: the last ten bytes. */
    CHECK(drive_fseek(token, -10, SEEK_END, &result) == 0 && result == 0 &&
          fseek(reference, -10L, SEEK_END) == 0);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES - 10 &&
          position == (int32_t)ftell(reference));
    CHECK(drive_fread(token, 1U, 10U, &got) == 0 && got == 10U &&
          fread(s_reference_read, 1U, 10U, reference) == 10U &&
          memcmp(s_guest_read, s_reference_read, 10U) == 0 &&
          memcmp(s_guest_read, s_fixture + FIXTURE_BYTES - 10U, 10U) == 0);

    /* SEEK_END(+10): ftell reports size + 10, fread finds nothing. */
    CHECK(drive_fseek(token, 10, SEEK_END, &result) == 0 && result == 0 &&
          fseek(reference, 10L, SEEK_END) == 0);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES + 10 &&
          position == (int32_t)ftell(reference));
    CHECK(drive_fread(token, 1U, 4U, &got) == 0 && got == 0U &&
          fread(s_reference_read, 1U, 4U, reference) == 0U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(reference));

    /* SEEK_CUR after a pending SEEK_END is rewritten against the logical
     * cursor, never against the stale real one. */
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          fseek(reference, 0L, SEEK_END) == 0);
    CHECK(drive_fseek(token, -20, SEEK_CUR, &result) == 0 && result == 0 &&
          fseek(reference, -20L, SEEK_CUR) == 0 &&
          s_seek_shadow.cur_pending == 1U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES - 20 &&
          position == (int32_t)ftell(reference));
    CHECK(drive_fread(token, 1U, 20U, &got) == 0 && got == 20U &&
          fread(s_reference_read, 1U, 20U, reference) == 20U &&
          memcmp(s_guest_read, s_reference_read, 20U) == 0);

    /* Negative logical target: EINVAL, cursor unchanged on both sides. */
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          fseek(reference, 0L, SEEK_END) == 0);
    g_isaac_vita_crt_errno = 0;
    CHECK(drive_fseek(token, -(int32_t)FIXTURE_BYTES - 5, SEEK_CUR,
                      &result) == 0 && result == -1 &&
          g_isaac_vita_crt_errno == EINVAL);
    errno = 0;
    CHECK(fseek(reference, -(long)FIXTURE_BYTES - 5L, SEEK_CUR) == -1 &&
          errno == EINVAL);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES &&
          position == (int32_t)ftell(reference));

    /* A failed real SEEK_SET keeps the deferred logical cursor. */
    g_isaac_vita_crt_errno = 0;
    CHECK(drive_fseek(token, -1, SEEK_SET, &result) == 0 && result == -1 &&
          g_isaac_vita_crt_errno == EINVAL &&
          s_file_tokens[index].seek_shadow.pending_end &&
          s_seek_shadow.rearmed == 1U);
    errno = 0;
    CHECK(fseek(reference, -1L, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES &&
          position == (int32_t)ftell(reference));

    /* SEEK_END whose target is negative goes to libc and fails there. */
    reset_seam_counts();
    g_isaac_vita_crt_errno = 0;
    CHECK(drive_fseek(token, -(int32_t)FIXTURE_BYTES - 1, SEEK_END,
                      &result) == 0 && result == -1 &&
          g_isaac_vita_crt_errno == EINVAL &&
          s_real_fseek[SEEK_END] == 1U && s_seek_shadow.real_end == 1U &&
          s_file_tokens[index].seek_shadow.pending_end);
    errno = 0;
    CHECK(fseek(reference, -(long)FIXTURE_BYTES - 1L, SEEK_END) == -1 &&
          errno == EINVAL);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(reference));

    /* fflush on the read stream applies the pending seek first. */
    CHECK(drive_fseek(token, -7, SEEK_END, &result) == 0 && result == 0 &&
          fseek(reference, -7L, SEEK_END) == 0);
    CHECK(drive_fflush(token, &result) == 0 && result == 0 &&
          fflush(reference) == 0 &&
          !s_file_tokens[index].seek_shadow.pending_end);
    CHECK(drive_fread(token, 1U, 7U, &got) == 0 && got == 7U &&
          fread(s_reference_read, 1U, 7U, reference) == 7U &&
          memcmp(s_guest_read, s_reference_read, 7U) == 0);

    /* fwrite on a read-only token fails identically on both sides. */
    CHECK(drive_fseek(token, -3, SEEK_END, &result) == 0 && result == 0 &&
          fseek(reference, -3L, SEEK_END) == 0);
    CHECK(drive_fwrite(token, 1U, 3U, &got) == 0 && got == 0U);
    CHECK(fwrite(s_reference_read, 1U, 3U, reference) == 0U);
    CHECK(!s_file_tokens[index].seek_shadow.pending_end);

    /* fclose with a pending seek simply drops the shadow. */
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0);
    CHECK(drive_fclose(token, &result) == 0 && result == 0 &&
          !s_file_tokens[index].seek_shadow.pending_end);
    (void)fclose(reference);
    return 0;
}

static int test_fstat_failure_falls_through(void)
{
    uint32_t token;
    int32_t position;
    int32_t result;
    int index;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    reset_seam_counts();
    s_fstat_fail = 1;
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          !s_file_tokens[index].seek_shadow.pending_end &&
          !s_file_tokens[index].seek_shadow.size_valid);
    CHECK(s_real_fstat == 1U && s_real_fseek[SEEK_END] == 1U &&
          s_seek_shadow.fstat_fail == 1U && s_seek_shadow.real_end == 1U &&
          s_seek_shadow.elided == 0U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES && s_real_ftell == 1U);
    s_fstat_fail = 0;
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          s_file_tokens[index].seek_shadow.pending_end &&
          s_real_fstat == 2U && s_real_fseek[SEEK_END] == 1U);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

static int test_write_generation_forces_restat(void)
{
    uint32_t reader;
    uint32_t writer;
    int32_t size;
    int32_t result;
    uint32_t got;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &reader) == 0 && reader != 0U);
    reset_seam_counts();
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 1U);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 1U);

    /* Any write-mode fopen invalidates cached sizes once. */
    CHECK(drive_fopen(s_mode_wb, &writer) == 0 && writer != 0U);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 2U &&
          s_seek_shadow.restat == 1U);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 2U);

    /* So does every fwrite and fflush on a write stream, and its fclose. */
    memcpy(s_guest_read, "0123", 4U);
    CHECK(drive_fwrite(writer, 1U, 4U, &got) == 0 && got == 4U);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 3U);
    CHECK(drive_fflush(writer, &result) == 0 && result == 0);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 4U);
    CHECK(drive_fclose(writer, &result) == 0 && result == 0);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 5U);
    CHECK(drive_getsize(reader, 0U, 0U, &size) == 0 && s_real_fstat == 5U);
    CHECK(size == (int32_t)FIXTURE_BYTES && s_real_fseek[SEEK_END] == 0U);

    /* Write streams never enter the shadow. */
    CHECK(s_seek_shadow.other_end == 0U);
    CHECK(drive_fclose(reader, &result) == 0 && result == 0);
    return 0;
}

static int test_other_modes_pass_through(void)
{
    static char *const modes[] = { s_mode_rplus, s_mode_wb, s_mode_a };
    uint32_t token;
    int32_t position;
    int32_t result;
    unsigned index;

    reset_oracle();
    for (index = 0U; index < 3U; ++index) {
        reset_seam_counts();
        CHECK(drive_fopen(modes[index], &token) == 0 && token != 0U);
        CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0);
        CHECK(drive_ftell(token, &position) == 0 && position >= 0);
        CHECK(drive_fseek(token, 0, SEEK_SET, &result) == 0 && result == 0);
        CHECK(s_real_fseek[SEEK_END] == 1U && s_real_fseek[SEEK_SET] == 1U &&
              s_real_ftell == 1U && s_real_fstat == 0U);
        CHECK(drive_fclose(token, &result) == 0 && result == 0);
    }
    CHECK(s_seek_shadow.seek_end == 0U && s_seek_shadow.other_end == 3U);

    /* "r" (text read) is as read-only as "rb". */
    reset_seam_counts();
    CHECK(drive_fopen(s_mode_r, &token) == 0 && token != 0U);
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          s_real_fseek[SEEK_END] == 0U && s_seek_shadow.elided == 1U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

static int test_raw_lane_untouched(void)
{
    uint32_t token;
    int32_t position;
    int32_t result;
    uint32_t got;
    int index;

    reset_oracle();
    s_mapped_path = AFTERBIRTHP;
    s_force_fopen_errno = ENOMEM;
    reset_seam_counts();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U &&
          s_raw_open_calls == 1U);
    index = token_index(token);
    CHECK(index >= 0 && s_file_tokens[index].raw_archive.live &&
          s_file_tokens[index].seek_shadow.read_only);
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0 &&
          drive_ftell(token, &position) == 0 && position == 10 &&
          !s_file_tokens[index].seek_shadow.pending_end);
    CHECK(drive_fseek(token, 2, SEEK_SET, &result) == 0 && result == 0 &&
          drive_fread(token, 1U, 3U, &got) == 0 && got == 3U &&
          memcmp(s_guest_read, "cde", 3U) == 0);
    CHECK(s_seek_shadow.seek_end == 0U && s_seek_shadow.other_end == 0U &&
          s_real_fseek[SEEK_END] == 0U && s_real_fseek[SEEK_SET] == 0U &&
          s_real_ftell == 0U && s_real_fstat == 0U);
    CHECK(drive_fclose(token, &result) == 0 && result == 0 &&
          s_raw_close_calls == 1U);
    return 0;
}

static int test_archive_ctor_seek_still_tracked(void)
{
    uint32_t token;
    int32_t position;
    int32_t result;
    int index;

    reset_oracle();
    s_mapped_path = PACKED "graphics.a";
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0 && strcmp(s_file_tokens[index].file.key, "graphics.a") == 0 &&
          s_diag_records == 1U &&
          s_last_diag.kind == ISAAC_VITA_ARCHIVE_DIAG_OPEN);

    /* ArchivedFile::Reset -> File::Seek(offset, SEEK_SET) with a pending
     * GetSize: the tracked SEEK_SET is real, lands exactly, no anomaly. */
    CHECK(drive_fseek(token, 0, SEEK_END, &result) == 0 && result == 0);
    CHECK(drive_fseek_at(token, 40000, SEEK_SET,
                         ISAAC_VITA_CRT_ARCHIVE_FSEEK_RETURN_RVA,
                         ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA, 0U,
                         &result) == 0 && result == 0);
    CHECK(s_diag_records == 2U &&
          s_last_diag.kind == ISAAC_VITA_ARCHIVE_DIAG_FSEEK &&
          s_last_diag.position_before == (int32_t)FIXTURE_BYTES &&
          s_last_diag.position_after == 40000 &&
          s_last_diag.caller_return ==
              ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA &&
          s_diag_logs == 0U);
    CHECK(drive_ftell(token, &position) == 0 && position == 40000);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

static int test_attribution_keys(void)
{
    uint32_t token;
    int32_t size;
    int32_t result;
    unsigned index;
    unsigned seen_grand = 0U;
    unsigned seen_parent = 0U;
    unsigned seen_site = 0U;
    unsigned seen_name = 0U;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    /* GetSize called from IsEOF called from a loop at 0x00401234. */
    CHECK(drive_getsize(token, ISAAC_VITA_CRT_SEEK_SHADOW_ISEOF_RETURN_RVA,
                        UINT32_C(0x00401234), &size) == 0);
    CHECK(drive_getsize(token, ISAAC_VITA_CRT_SEEK_SHADOW_ISEOF_RETURN_RVA,
                        UINT32_C(0x00401234), &size) == 0);
    /* GetSize called directly by a loader at 0x00402000. */
    CHECK(drive_getsize(token, UINT32_C(0x00402000), 0U, &size) == 0);
    /* File::Seek(offset, SEEK_END) direct site. */
    CHECK(drive_fseek_at(token, 0, SEEK_END,
                         ISAAC_VITA_CRT_ARCHIVE_FSEEK_RETURN_RVA, 0U, 0U,
                         &result) == 0 && result == 0);
    for (index = 0U; index < VITA_CRT_SEEK_SHADOW_TOP; ++index) {
        if (s_seek_shadow.top_ret[index].key == UINT32_C(0x00401234))
            seen_grand = s_seek_shadow.top_ret[index].count;
        if (s_seek_shadow.top_ret[index].key == UINT32_C(0x00402000))
            seen_parent = s_seek_shadow.top_ret[index].count;
        if (s_seek_shadow.top_ret[index].key ==
                ISAAC_VITA_CRT_ARCHIVE_FSEEK_RETURN_RVA)
            seen_site = s_seek_shadow.top_ret[index].count;
        if (strcmp(s_seek_shadow.top_name[index].name,
                   "00.special rooms.stb") == 0)
            seen_name = s_seek_shadow.top_name[index].count;
    }
    CHECK(seen_grand == 2U && seen_parent == 1U && seen_site == 1U &&
          seen_name == 4U);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);

    /* The two bounded report lines. */
    s_log_lines = 0U;
    isaac_vita_crt_seek_shadow_report("oracle");
    CHECK(s_log_lines == 2U &&
          strncmp(s_log_last[0],
                  "KAGE VITA CRT SEEK SHADOW: why=oracle n=1 seek_end=4 "
                  "elided=4 real_end=0 fstat=1 fstat_fail=0 restat=0 "
                  "applied=0 apply_fail=0 ftell_pending=3 set_pending=3 "
                  "cur_pending=0 rearmed=0 other_end=0 gen=0 live=0",
                  200U) == 0 &&
          strncmp(s_log_last[1],
                  "KAGE VITA CRT SEEK SHADOW TOP: why=oracle "
                  "ret=00401234:2,", 55U) == 0 &&
          strstr(s_log_last[1], "name=00.special rooms.stb:4,") != NULL &&
          strlen(s_log_last[0]) < 300U && strlen(s_log_last[1]) < 300U);
    return 0;
}

/* ---- adversarial review cases ------------------------------------------ */

static FILE *reference_open_path(const char *path, const char *mode)
{
    FILE *stream = fopen(path, mode);

    if (stream && mode[0] == 'r')
        (void)setvbuf(stream, NULL, _IOFBF,
                      ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
    return stream;
}

#define PAIR_READ(tok, ref, n) \
    do { \
        CHECK(drive_fread(tok, 1U, (n), &got) == 0); \
        CHECK(got == (uint32_t)fread(s_reference_read, 1U, (n), ref)); \
        CHECK(memcmp(s_guest_read, s_reference_read, got) == 0); \
        CHECK(drive_ftell(tok, &position) == 0 && \
              position == (int32_t)ftell(ref)); \
    } while (0)
#define PAIR_SIZE(tok, ref) \
    do { \
        CHECK(drive_getsize(tok, 0U, 0U, &size) == 0 && \
              size == (int32_t)reference_getsize(ref)); \
        CHECK(drive_ftell(tok, &position) == 0 && \
              position == (int32_t)ftell(ref)); \
    } while (0)
#define PAIR_SEEK(tok, ref, off, org) \
    do { \
        CHECK(drive_fseek(tok, (off), (org), &result) == 0); \
        CHECK((result == 0) == (fseek(ref, (long)(off), (org)) == 0)); \
        CHECK(drive_ftell(tok, &position) == 0 && \
              position == (int32_t)ftell(ref)); \
    } while (0)

static long disk_size(const char *path)
{
    struct stat status;

    if (stat(path, &status) != 0)
        return -1L;
    return (long)status.st_size;
}

/* A reader holding a cached size while a second token appends to the same
 * file.  newlib flushes a write stream's buffer inside fseek (any origin),
 * ftell and a read-after-write on an update stream, not only in fwrite,
 * fflush and fclose, so each of those must force the reader to re-fstat.
 * A plain libc writer mirrors the token writer on the same path (so the
 * file grows by both), but it is a foreign writer the shadow cannot see:
 * the reader is checked only after token-side operations, and there it
 * must report the true on-disk size exactly as newlib's fstat would. */
static int test_writer_side_flush_invalidates(void)
{
    uint32_t reader;
    uint32_t writer;
    FILE *reference_reader;
    FILE *reference_writer;
    int32_t size;
    int32_t position;
    int32_t result;
    uint32_t got;
    uint32_t fstat_before;

#define READER_SIZE_IS_TRUE() \
    do { \
        CHECK(drive_getsize(reader, 0U, 0U, &size) == 0); \
        CHECK(size == (int32_t)disk_size(s_append_path)); \
        CHECK(size == (int32_t)reference_getsize(reference_reader)); \
    } while (0)

    reset_oracle();
    s_open_path_override = s_append_path;
    CHECK(drive_fopen(s_mode_rb, &reader) == 0 && reader != 0U);
    reference_reader = reference_open_path(s_append_path, "rb");
    CHECK(reference_reader != NULL);
    READER_SIZE_IS_TRUE();
    CHECK(size == (int32_t)FIXTURE_BYTES);

    CHECK(drive_fopen(s_mode_a, &writer) == 0 && writer != 0U);
    reference_writer = fopen(s_append_path, "a");
    CHECK(reference_writer != NULL);
    memcpy(s_guest_read, "WXYZ", 4U);
    CHECK(drive_fwrite(writer, 1U, 4U, &got) == 0 && got == 4U);
    CHECK(fwrite("WXYZ", 1U, 4U, reference_writer) == 4U);
    /* Buffered or already on disk, the reader reports the true size. */
    READER_SIZE_IS_TRUE();

    /* fseek on the write stream flushes its buffer (libc and newlib). */
    fstat_before = s_seek_shadow.fstat_calls;
    CHECK(drive_fseek(writer, 0, SEEK_CUR, &result) == 0 && result == 0);
    READER_SIZE_IS_TRUE();
    CHECK(s_seek_shadow.fstat_calls == fstat_before + 1U);
    CHECK(size >= (int32_t)FIXTURE_BYTES + 4);
    CHECK(fseek(reference_writer, 0L, SEEK_CUR) == 0);

    /* ftell on the write stream: newlib flushes, glibc computes; either
     * way the reader must re-fstat instead of trusting its cache. */
    memcpy(s_guest_read, "abc", 3U);
    CHECK(drive_fwrite(writer, 1U, 3U, &got) == 0 && got == 3U);
    CHECK(fwrite("abc", 1U, 3U, reference_writer) == 3U);
    READER_SIZE_IS_TRUE();
    fstat_before = s_seek_shadow.fstat_calls;
    CHECK(drive_ftell(writer, &position) == 0 && position >= 0);
    READER_SIZE_IS_TRUE();
    CHECK(s_seek_shadow.fstat_calls == fstat_before + 1U);
    CHECK((long)position == ftell(reference_writer));

    /* A read on a write/update stream switches modes and flushes first; on
     * this "a" token it fails identically on both sides and still counts as
     * write-side activity. */
    fstat_before = s_seek_shadow.fstat_calls;
    CHECK(drive_fread(writer, 1U, 1U, &got) == 0 && got == 0U);
    READER_SIZE_IS_TRUE();
    CHECK(s_seek_shadow.fstat_calls == fstat_before + 1U);
    CHECK(fread(s_reference_read, 1U, 1U, reference_writer) == 0U);

    /* The foreign writer closes first; the token fclose then re-arms the
     * reader for everything that reached the disk. */
    CHECK(fclose(reference_writer) == 0);
    CHECK(drive_fclose(writer, &result) == 0 && result == 0);
    READER_SIZE_IS_TRUE();
    CHECK(size == (int32_t)FIXTURE_BYTES + 14);

    /* The appended bytes are readable through the pending SEEK_END path. */
    CHECK(drive_fseek(reader, -14, SEEK_END, &result) == 0 && result == 0);
    CHECK(fseek(reference_reader, -14L, SEEK_END) == 0);
    CHECK(drive_fread(reader, 1U, 14U, &got) == 0 && got == 14U);
    CHECK(fread(s_reference_read, 1U, 14U, reference_reader) == 14U);
    CHECK(memcmp(s_guest_read, s_reference_read, 14U) == 0);
    CHECK(memcmp(s_guest_read, "WXYZ", 4U) == 0 &&
          memcmp(s_guest_read + 4U, "WXYZ", 4U) == 0);
    CHECK(drive_ftell(reader, &position) == 0 &&
          position == (int32_t)ftell(reference_reader) &&
          position == (int32_t)FIXTURE_BYTES + 14);

    CHECK(drive_fclose(reader, &result) == 0 && result == 0);
    (void)fclose(reference_reader);
    s_open_path_override = NULL;
    return 0;
#undef READER_SIZE_IS_TRUE
}

/* Two read-only tokens on one file: independent shadows, one fstat each,
 * every interleaved cursor/byte identical to two plain libc FILEs. */
static int test_two_tokens_same_file(void)
{
    uint32_t a;
    uint32_t b;
    FILE *ra;
    FILE *rb;
    int32_t size;
    int32_t position;
    int32_t result;
    uint32_t got;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &a) == 0 && a != 0U);
    CHECK(drive_fopen(s_mode_rb, &b) == 0 && b != 0U && b != a);
    ra = reference_open("rb");
    rb = reference_open("rb");
    CHECK(ra != NULL && rb != NULL);
    reset_seam_counts();

    PAIR_READ(a, ra, 1000U);
    PAIR_SIZE(b, rb);
    PAIR_SIZE(a, ra);
    PAIR_READ(b, rb, 3000U);
    PAIR_SEEK(b, rb, -100, SEEK_END);
    PAIR_READ(a, ra, 20000U);
    PAIR_SIZE(a, ra);
    PAIR_READ(b, rb, 200U);           /* short: 100 bytes then EOF */
    PAIR_SIZE(b, rb);
    PAIR_SEEK(a, ra, 0, SEEK_END);
    PAIR_SEEK(b, rb, -1, SEEK_END);
    PAIR_READ(a, ra, 1U);             /* nothing at the end */
    PAIR_READ(b, rb, 1U);             /* the last byte */
    PAIR_SEEK(a, ra, 16384 - 3, SEEK_SET);
    PAIR_READ(a, ra, 6U);             /* straddles the 16 KiB buffer edge */
    PAIR_SIZE(a, ra);
    PAIR_SEEK(b, rb, -16384, SEEK_CUR);
    PAIR_READ(b, rb, 16384U);
    PAIR_SIZE(b, rb);
    /* One fstat per token; the only real SEEK_ENDs are the deferred ones
     * applied in front of the three reads that followed a SEEK_END. */
    CHECK(s_real_fstat == 2U && s_seek_shadow.real_end == 0U &&
          s_real_fseek[SEEK_END] == s_seek_shadow.applied &&
          s_seek_shadow.applied == 3U);

    CHECK(drive_fclose(a, &result) == 0 && result == 0);
    CHECK(drive_fclose(b, &result) == 0 && result == 0);
    (void)fclose(ra);
    (void)fclose(rb);
    return 0;
}

/* EOF flag, zero-length reads, a rearmed logical end and SEEK_END(+k)
 * followed by SEEK_CUR(-k): every consumer after a pending SEEK_END. */
static int test_eof_and_zero_length_consumers(void)
{
    uint32_t token;
    FILE *ref;
    int32_t size;
    int32_t position;
    int32_t result;
    uint32_t got;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    ref = reference_open("rb");
    CHECK(ref != NULL);

    /* Short read to EOF, GetSize (newlib clears EOF in its real SEEK_END;
     * the shadow defers it), then a read at the end. */
    PAIR_SEEK(token, ref, -5, SEEK_END);
    PAIR_READ(token, ref, 10U);
    CHECK(got == 5U && feof(ref));
    PAIR_SIZE(token, ref);
    PAIR_READ(token, ref, 1U);
    CHECK(got == 0U);
    CHECK(position == (int32_t)FIXTURE_BYTES);

    /* Zero-length reads with a pending SEEK_END. */
    PAIR_SEEK(token, ref, 0, SEEK_SET);
    PAIR_READ(token, ref, 4U);
    PAIR_SEEK(token, ref, -3, SEEK_END);
    CHECK(drive_fread(token, 0U, 5U, &got) == 0 && got == 0U &&
          fread(s_reference_read, 0U, 5U, ref) == 0U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(ref) &&
          position == (int32_t)FIXTURE_BYTES - 3);
    CHECK(drive_fread(token, 4U, 0U, &got) == 0 && got == 0U &&
          fread(s_reference_read, 4U, 0U, ref) == 0U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(ref));
    PAIR_READ(token, ref, 3U);
    CHECK(got == 3U);

    /* A failed SEEK_SET rearms the logical end; the next read consumes it. */
    PAIR_SEEK(token, ref, 0, SEEK_END);
    PAIR_SEEK(token, ref, -1, SEEK_SET);
    CHECK(result == -1);
    PAIR_READ(token, ref, 10U);
    CHECK(got == 0U && position == (int32_t)FIXTURE_BYTES);

    /* SEEK_END(+5) then SEEK_CUR(-5) lands exactly on the end. */
    PAIR_SEEK(token, ref, 5, SEEK_END);
    PAIR_SEEK(token, ref, -5, SEEK_CUR);
    CHECK(position == (int32_t)FIXTURE_BYTES);
    PAIR_READ(token, ref, 1U);
    CHECK(got == 0U);
    PAIR_SEEK(token, ref, -2, SEEK_CUR);
    PAIR_READ(token, ref, 2U);
    CHECK(got == 2U &&
          memcmp(s_guest_read, s_fixture + FIXTURE_BYTES - 2U, 2U) == 0);

    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    (void)fclose(ref);
    return 0;
}

/* fflush(NULL) while a SEEK_END is pending: newlib syncs every read
 * stream's fd to its logical cursor without moving that cursor. */
static int test_fflush_null_keeps_pending(void)
{
    uint32_t token;
    FILE *ref;
    int32_t position;
    int32_t result;
    uint32_t got;
    int index;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    ref = reference_open("rb");
    CHECK(ref != NULL);
    PAIR_READ(token, ref, 100U);
    PAIR_SEEK(token, ref, -40, SEEK_END);
    CHECK(s_file_tokens[index].seek_shadow.pending_end);
    CHECK(drive_fflush(0U, &result) == 0 && result == 0 && fflush(NULL) == 0);
    CHECK(s_file_tokens[index].seek_shadow.pending_end);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)ftell(ref) &&
          position == (int32_t)FIXTURE_BYTES - 40);
    PAIR_READ(token, ref, 40U);
    CHECK(got == 40U);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    (void)fclose(ref);
    return 0;
}

#undef PAIR_READ
#undef PAIR_SIZE
#undef PAIR_SEEK

/* Deterministic xorshift so the sequence is reproducible from the seed. */
static uint32_t s_random_state;

static uint32_t next_random(void)
{
    uint32_t x = s_random_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random_state = x ? x : UINT32_C(0x9e3779b9);
    return s_random_state;
}

static int test_random_sequence_matches_reference(unsigned total_ops,
                                                  uint32_t seed)
{
    static const uint32_t element_sizes[6] = { 1U, 2U, 4U, 7U, 1024U, 16384U };
    uint32_t token;
    FILE *reference;
    unsigned op;
    unsigned reopen_at = 0U;
    unsigned counted_reads = 0U;
    unsigned counted_seeks = 0U;
    unsigned counted_getsize = 0U;
    int32_t result;
    int32_t position;
    uint32_t got;

    reset_oracle();
    s_random_state = seed;
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    reference = reference_open("rb");
    CHECK(reference != NULL);
    reset_seam_counts();

    for (op = 0U; op < total_ops; ++op) {
        uint32_t kind = next_random() % 100U;

        if (op == reopen_at + 2500U) {
            reopen_at = op;
            CHECK(drive_fclose(token, &result) == 0 && result == 0);
            CHECK(fclose(reference) == 0);
            CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
            reference = reference_open("rb");
            CHECK(reference != NULL);
        }
        if (kind < 18U) {
            int32_t offset = (int32_t)(next_random() % (FIXTURE_BYTES + 60U)) - 5;
            int reference_result = fseek(reference, (long)offset, SEEK_SET);

            CHECK(drive_fseek(token, offset, SEEK_SET, &result) == 0 &&
                  (result == 0) == (reference_result == 0));
            ++counted_seeks;
        } else if (kind < 30U) {
            int32_t offset = (int32_t)(next_random() % FIXTURE_BYTES) -
                (int32_t)(FIXTURE_BYTES / 2U);
            int reference_result = fseek(reference, (long)offset, SEEK_CUR);

            CHECK(drive_fseek(token, offset, SEEK_CUR, &result) == 0 &&
                  (result == 0) == (reference_result == 0));
            ++counted_seeks;
        } else if (kind < 45U) {
            int32_t offset = 60 - (int32_t)(next_random() % (FIXTURE_BYTES + 70U));
            int reference_result = fseek(reference, (long)offset, SEEK_END);

            CHECK(drive_fseek(token, offset, SEEK_END, &result) == 0 &&
                  (result == 0) == (reference_result == 0));
            ++counted_seeks;
        } else if (kind < 60U) {
            long reference_size = reference_getsize(reference);
            int32_t size;

            CHECK(reference_size >= 0);
            CHECK(drive_getsize(token, 0U, 0U, &size) == 0 &&
                  size == (int32_t)reference_size);
            ++counted_getsize;
        } else if (kind < 66U) {
            /* IsEOF: Tell() >= GetSize(). */
            long reference_position = ftell(reference);
            long reference_size = reference_getsize(reference);
            int32_t size;

            CHECK(drive_ftell(token, &position) == 0 &&
                  position == (int32_t)reference_position);
            CHECK(drive_getsize(token, 0U, 0U, &size) == 0 &&
                  size == (int32_t)reference_size);
            ++counted_getsize;
        } else if (kind < 96U) {
            uint32_t size = element_sizes[next_random() % 6U];
            uint32_t count = next_random() % (40000U / size) + 1U;
            size_t reference_got;

            CHECK(size * count <= READ_CAPACITY);
            memset(s_guest_read, 0xa5, size * count);
            memset(s_reference_read, 0x5a, size * count);
            CHECK(drive_fread(token, size, count, &got) == 0);
            reference_got = fread(s_reference_read, size, count, reference);
            CHECK(got == (uint32_t)reference_got);
            CHECK(memcmp(s_guest_read, s_reference_read,
                         (size_t)got * size) == 0);
            ++counted_reads;
        } else {
            int reference_result = fflush(reference);

            CHECK(drive_fflush(token, &result) == 0 &&
                  (result == 0) == (reference_result == 0));
        }
        CHECK(drive_ftell(token, &position) == 0);
        CHECK(position == (int32_t)ftell(reference));
    }
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    CHECK(fclose(reference) == 0);
    CHECK(s_seek_shadow.fstat_fail == 0U && s_seek_shadow.apply_fail == 0U &&
          s_seek_shadow.other_end == 0U);
    CHECK(s_seek_shadow.seek_end == s_seek_shadow.elided +
              s_seek_shadow.real_end);
    /* Every real SEEK_END the lane issued was either an apply before a
     * consuming operation or an out-of-range request libc had to reject. */
    CHECK(s_real_fseek[SEEK_END] == s_seek_shadow.applied +
              s_seek_shadow.real_end);
    CHECK(s_real_fstat == s_seek_shadow.fstat_calls);
    printf("seek shadow oracle random: seed=%08x ops=%u reads=%u seeks=%u "
           "getsize=%u | seek_end=%u elided=%u real_end=%u fstat=%u "
           "applied=%u ftell_pending=%u set_pending=%u cur_pending=%u "
           "rearmed=%u | libc fseek(set/cur/end)=%u/%u/%u ftell=%u fstat=%u\n",
           (unsigned)seed, total_ops, counted_reads, counted_seeks,
           counted_getsize, (unsigned)s_seek_shadow.seek_end,
           (unsigned)s_seek_shadow.elided, (unsigned)s_seek_shadow.real_end,
           (unsigned)s_seek_shadow.fstat_calls,
           (unsigned)s_seek_shadow.applied,
           (unsigned)s_seek_shadow.ftell_pending,
           (unsigned)s_seek_shadow.set_pending,
           (unsigned)s_seek_shadow.cur_pending,
           (unsigned)s_seek_shadow.rearmed,
           (unsigned)s_real_fseek[SEEK_SET], (unsigned)s_real_fseek[SEEK_CUR],
           (unsigned)s_real_fseek[SEEK_END], (unsigned)s_real_ftell,
           (unsigned)s_real_fstat);
    return 0;
}

static int write_fixture(const char *directory)
{
    FILE *stream;
    size_t index;

    (void)snprintf(s_fixture_path, sizeof s_fixture_path,
                   "%s/seek-shadow-fixture.bin", directory);
    (void)snprintf(s_scratch_path, sizeof s_scratch_path,
                   "%s/seek-shadow-scratch.bin", directory);
    for (index = 0U; index < FIXTURE_BYTES; ++index)
        s_fixture[index] = (unsigned char)(index * 7U + (index >> 8));
    stream = fopen(s_fixture_path, "wb");
    if (!stream || fwrite(s_fixture, 1U, FIXTURE_BYTES, stream) != FIXTURE_BYTES ||
        fclose(stream) != 0)
        return -1;
    stream = fopen(s_scratch_path, "wb");
    if (!stream || fclose(stream) != 0)
        return -1;
    (void)snprintf(s_append_path, sizeof s_append_path,
                   "%s/seek-shadow-append.bin", directory);
    stream = fopen(s_append_path, "wb");
    if (!stream || fwrite(s_fixture, 1U, FIXTURE_BYTES, stream) != FIXTURE_BYTES ||
        fclose(stream) != 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    unsigned ops = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 20000U;

    if ((uintptr_t)s_guest_read > UINT32_MAX ||
        (uintptr_t)&s_guest_stack[64] > UINT32_MAX ||
        (uintptr_t)s_mode_rb > UINT32_MAX) {
        fprintf(stderr, "seek shadow oracle needs 32-bit static addresses\n");
        return 1;
    }
    if (write_fixture(directory) != 0) {
        fprintf(stderr, "seek shadow oracle cannot write %s\n", s_fixture_path);
        return 1;
    }
    if (test_getsize_fresh_and_cached() ||
        test_iseof_loop_matches_reference() ||
        test_iseof_at_buffer_boundary() ||
        test_pending_consumers() ||
        test_fstat_failure_falls_through() ||
        test_write_generation_forces_restat() ||
        test_other_modes_pass_through() ||
        test_raw_lane_untouched() ||
        test_archive_ctor_seek_still_tracked() ||
        test_attribution_keys() ||
        test_writer_side_flush_invalidates() ||
        test_two_tokens_same_file() ||
        test_eof_and_zero_length_consumers() ||
        test_fflush_null_keeps_pending() ||
        test_random_sequence_matches_reference(ops, UINT32_C(0x5eed0001)) ||
        test_random_sequence_matches_reference(ops, UINT32_C(0x00c0ffee)))
        return 1;
    (void)remove(s_fixture_path);
    (void)remove(s_scratch_path);
    (void)remove(s_append_path);
    printf("seek shadow oracle: PASS\n");
    return 0;
}
