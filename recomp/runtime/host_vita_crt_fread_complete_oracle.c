/* Host oracle for ISAAC_VITA_CRT_FREAD_COMPLETE (short packed-archive fread
 * completion).
 *
 * Includes the production CRT TU so the exact private token layout, the lane
 * dispatch and the fread import handler are exercised.  The native fread
 * seam (ISAAC_VITA_CRT_FREAD_ORACLE) is the fake lower read layer: a plan of
 * byte counts consumed from the real fixture FILE per native call, so a read
 * comes back short while the stream carries neither the EOF nor the error
 * flag -- the shape the tour-1 "ArchivedFile block header is invalid" fault
 * implies (86 valid blocks, cursor 48 bytes short, fl=08).  The guest stdio
 * imports are driven through a fake x86 stack; results are compared with a
 * plain libc FILE following the same operations, which is what a whole read
 * would have delivered.
 *
 * test_crt_fread_complete.sh builds this file four times: without and with
 * ISAAC_VITA_CRT_FREAD_COMPLETE, each alone and with the release CRT set
 * (ISAAC_VITA_CRT_SEEK_SHADOW, ISAAC_VITA_CRT_DESCRIPTOR_RECOVER,
 * ISAAC_VITA_CRT_FILE_LOOKUP_HINT).  Without the option the short read
 * reaches the guest unchanged and the block chain dies exactly like tour 1;
 * with it every packed-archive read is whole, a KAGE VITA CRT FREAD SHORT
 * receipt is logged for the first eight and the counters carry the rest.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "guest.h"

#if defined(_WIN32) && !defined(S_ISREG)
/* UCRT's <sys/stat.h> has no S_ISREG; the seek shadow's fstat check uses
 * it.  Host oracle only. */
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

#define ISAAC_VITA_ARCHIVE_FILE_CACHE 1
#define ISAAC_VITA_CRT_QSORT_HOST_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE 1
#define ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX INT32_MAX
/* The fread seam is the fake lower read layer (see isaac_vita_crt_oracle_fread). */
#define ISAAC_VITA_CRT_FREAD_ORACLE 1
/* ISAAC_VITA_CRT_FREAD_COMPLETE, ISAAC_VITA_CRT_SEEK_SHADOW,
 * ISAAC_VITA_CRT_DESCRIPTOR_RECOVER and ISAAC_VITA_CRT_FILE_LOOKUP_HINT come
 * from the compiler command line. */
#include "host_vita_crt.c"

#define ROOT "ux0:/data/isaacr001"
#define PACKED ROOT "/resources/packed/"
#define REPENTANCE PACKED "repentance.a"
#define AFTERBIRTHP PACKED "afterbirthp.a"
#define LOOSE_XML ROOT "/resources/rooms/00.special rooms.stb"

#define SCE_ENOENT ((int32_t)UINT32_C(0x80010002))
#define SCE_EIO    ((int32_t)UINT32_C(0x80010005))

/* The fixture is an ArchivedFile block chain: a 4-byte header word (flag bit
 * plus the 1024-byte block length) then 1024 payload bytes, forty times.
 * Payload bytes are never below 0x20, so any 4-byte window that is not
 * exactly a header decodes to a length above the 0x800 bound: a misaligned
 * cursor is caught by the very next header read, as on the device. */
#define BLOCK_PAYLOAD 1024U
#define BLOCK_STRIDE (4U + BLOCK_PAYLOAD)
#define BLOCK_COUNT 40U
#define FIXTURE_BYTES (BLOCK_COUNT * BLOCK_STRIDE)
#define HEADER_WORD UINT32_C(0x80000400)
#define READ_CAPACITY 65536U
#define LOG_RING 8U
#define STARTS_WITH(text, literal) \
    (strncmp((text), (literal), sizeof (literal) - 1U) == 0)

static char s_mode_rb[] = "rb";
static char s_mode_wb[] = "wb";
_Alignas(16) static uint32_t s_guest_stack[64];
_Alignas(16) static unsigned char s_guest_read[READ_CAPACITY];
_Alignas(16) static unsigned char s_reference_read[READ_CAPACITY];
static unsigned char s_fixture[FIXTURE_BYTES];
static char s_fixture_path[1024];
static char s_scratch_path[1024];
static const char *s_mapped_path;
static int s_force_fopen_errno;
static uint32_t s_native_fopen_calls;
static uint32_t s_native_fread_calls;
/* The fake lower layer's plan: byte counts for the next native reads. */
static size_t s_plan[8];
static unsigned s_plan_count;
static unsigned s_plan_next;
static int s_plan_sticky;
/* Raw SceIo lane seams (the exact-ENOMEM fallback), backed by s_fixture. */
static uint32_t s_raw_open_calls;
static uint32_t s_raw_close_calls;
static uint32_t s_pread_calls;
static uint32_t s_pread_fail_call;
/* arcdiag and log capture. */
static uint32_t s_diag_records;
static isaac_vita_archive_diag_event s_last_diag;
static char s_last_diag_reason[32];
static uint32_t s_diag_logs;
static uint32_t s_log_lines;
static char s_log_last[LOG_RING][512];
static uint32_t s_short_receipts;
static uint32_t s_complete_reports;

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "fread complete oracle failed at line %u: %s\n",
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

/* ---- the fake lower read layer ----------------------------------------- */

/* The next native fread consumes plan[next] bytes from the real FILE (fewer
 * only at EOF) and reports floor(bytes / size) elements: newlib's shape for
 * a short read whose partial-element bytes were already copied, but with
 * the stream's EOF/error flags untouched, which newlib itself never does.
 * A zero entry is a read that returned nothing and flagged nothing.  With
 * the plan exhausted (or none set) the call is plain libc fread.  A sticky
 * plan repeats its last entry forever. */
static void set_plan(unsigned count, const size_t *entries, int sticky)
{
    unsigned index;

    if (count > sizeof s_plan / sizeof s_plan[0])
        abort();
    for (index = 0U; index < count; ++index)
        s_plan[index] = entries[index];
    s_plan_count = count;
    s_plan_next = 0U;
    s_plan_sticky = sticky;
}

static void clear_plan(void)
{
    s_plan_count = 0U;
    s_plan_next = 0U;
    s_plan_sticky = 0;
}

size_t isaac_vita_crt_oracle_fread(void *buffer, size_t size,
                                   size_t count, FILE *stream)
{
    ++s_native_fread_calls;
    if (s_plan_next < s_plan_count) {
        size_t bytes = s_plan[s_plan_next];
        size_t copied = 0U;

        if (!(s_plan_sticky && s_plan_next + 1U == s_plan_count))
            ++s_plan_next;
        if (bytes > size * count)
            bytes = size * count;
        if (bytes)
            copied = fread(buffer, 1U, bytes, stream);
        return size ? copied / size : 0U;
    }
    return fread(buffer, size, count, stream);
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

/* Every open is a fresh libc FILE on the fixture (read modes) or the
 * scratch file (write modes); the production one-idle cache is not what is
 * measured here. */
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
    stream = fopen(mode[0] == 'r' ? s_fixture_path : s_scratch_path, mode);
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

FILE *isaac_vita_archive_cache_reopen_native(const char *native_path,
                                             const char *mode)
{
    (void)native_path;
    return fopen(mode[0] == 'r' ? s_fixture_path : s_scratch_path, mode);
}

#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
void isaac_vita_archive_fd_repair_counts(uint32_t *repaired, uint32_t *leaked)
{
    *repaired = 0U;
    *leaked = 0U;
}
#endif

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

/* The last LOG_RING lines are kept; every KAGE VITA CRT line is echoed so
 * the run log shows the receipts and the counter lines verbatim. */
void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    char *slot = s_log_last[s_log_lines % LOG_RING];

    va_start(arguments, format);
    (void)vsnprintf(slot, sizeof s_log_last[0], format, arguments);
    va_end(arguments);
    ++s_log_lines;
    if (STARTS_WITH(slot, "KAGE VITA CRT FREAD SHORT:"))
        ++s_short_receipts;
    else if (STARTS_WITH(slot, "KAGE VITA CRT FREAD COMPLETE:"))
        ++s_complete_reports;
    if (STARTS_WITH(slot, "KAGE VITA CRT"))
        printf("%s\n", slot);
    else
        ++s_diag_logs;
}

#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
static const char *last_log_line(void)
{
    return s_log_last[(s_log_lines - 1U) % LOG_RING];
}
#endif

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
    if (strcmp(path, REPENTANCE) != 0 && strcmp(path, AFTERBIRTHP) != 0)
        return SCE_ENOENT;
    return 100;
}

int32_t isaac_vita_crt_raw_archive_oracle_get_size(
    int32_t descriptor, int64_t *size)
{
    if (descriptor < 100 || !size)
        return SCE_EIO;
    *size = (int64_t)FIXTURE_BYTES;
    return 0;
}

/* pread call s_pread_fail_call (1-based) fails with SCE_EIO. */
int32_t isaac_vita_crt_raw_archive_oracle_pread(
    int32_t descriptor, void *buffer, uint32_t size, uint64_t offset)
{
    uint32_t available;
    uint32_t result;

    ++s_pread_calls;
    if (descriptor < 100)
        return SCE_EIO;
    if (s_pread_fail_call && s_pread_calls == s_pread_fail_call)
        return SCE_EIO;
    if (offset >= FIXTURE_BYTES)
        return 0;
    available = (uint32_t)(FIXTURE_BYTES - offset);
    result = size < available ? size : available;
    memcpy(buffer, s_fixture + offset, result);
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
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
    s_file_lookup_hint = ISAAC_VITA_CRT_FILE_TOKEN_COUNT;
    s_file_lookup_hits = s_file_lookup_misses = 0U;
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    memset(&s_seek_shadow, 0, sizeof s_seek_shadow);
    s_seek_shadow_write_gen = 0U;
    s_seek_shadow_since_report = 0U;
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    memset(&s_descriptor_recover, 0, sizeof s_descriptor_recover);
#endif
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    memset(&s_fread_complete, 0, sizeof s_fread_complete);
#endif
    s_file_lock = 0U;
    s_archive_diag_logged_first_relevant = 0U;
    s_archive_diag_logged_first_direct = 0U;
    s_mapped_path = REPENTANCE;
    s_force_fopen_errno = 0;
    s_native_fopen_calls = 0U;
    s_native_fread_calls = 0U;
    clear_plan();
    s_raw_open_calls = 0U;
    s_raw_close_calls = 0U;
    s_pread_calls = 0U;
    s_pread_fail_call = 0U;
    s_diag_records = 0U;
    memset(&s_last_diag, 0, sizeof s_last_diag);
    s_last_diag_reason[0] = '\0';
    s_diag_logs = 0U;
    s_short_receipts = 0U;
    s_complete_reports = 0U;
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

static int drive_fseek(uint32_t token, int32_t offset, int origin,
                       int32_t *result)
{
    uint32_t arguments[3] = { token, (uint32_t)offset, (uint32_t)origin };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 3U);

    vita_crt_fseek(&cpu);
    if (cpu.fault || cpu.esp != esp + 4U)
        return -1;
    *result = (int32_t)cpu.eax;
    return 0;
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

/* fread with the measured adapter frame: [ESP] = the adapter's return,
 * [EBP+4] = the ArchivedFile edge that owns the request.  Returns 0 on a
 * clean cdecl return, 1 when the handler faulted (message in *fault, ESP
 * untouched), -1 on any other frame damage. */
static int drive_fread_at(uint32_t token, uint32_t size, uint32_t count,
                          uint32_t parent, uint32_t *result,
                          const char **fault)
{
    uint32_t arguments[4] = {
        (uint32_t)(uintptr_t)s_guest_read, size, count, token
    };
    CPU cpu;
    uint32_t esp = prepare_cdecl(&cpu, arguments, 4U);
    uint32_t ebp = (uint32_t)(uintptr_t)&s_guest_stack[40];

    st32(esp, ISAAC_VITA_CRT_FREAD_RETURN_RVA);
    cpu.ebp = ebp;
    st32(ebp + 4U, parent);
    vita_crt_fread(&cpu);
    *fault = cpu.fault;
    if (cpu.fault)
        return cpu.esp == esp ? 1 : -1;
    if (cpu.esp != esp + 4U)
        return -1;
    *result = cpu.eax;
    return 0;
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

static FILE *token_stream(int index)
{
    return s_file_tokens[index].file.stream;
}

static FILE *reference_open(void)
{
    FILE *stream = fopen(s_fixture_path, "rb");

    if (stream)
        (void)setvbuf(stream, NULL, _IOFBF,
                      ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
    return stream;
}

/* ---- tests -------------------------------------------------------------- */

/* One payload read of a block, short by the lower layer three times over
 * (1 byte, then 511 of the remaining 1023, then the rest). */
static int test_payload_short_completed(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t calls;
    int32_t position;
    int32_t result;
    const char *fault = NULL;
    int index;
    char expected[256];

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0 &&
          strcmp(s_file_tokens[index].file.key, "repentance.a") == 0);
    CHECK(drive_fseek(token, 4, SEEK_SET, &result) == 0 && result == 0);
    calls = s_native_fread_calls;
    set_plan(3U, (const size_t[]){ 1U, 511U, READ_CAPACITY }, 0);
    memset(s_guest_read, 0xa5, BLOCK_PAYLOAD + 4U);
    g_isaac_vita_crt_errno = 0;
    CHECK(drive_fread_at(token, 1U, BLOCK_PAYLOAD,
                         ISAAC_VITA_CRT_ARCHIVE_TYPE2_PAYLOAD_RETURN_RVA,
                         &got, &fault) == 0);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(got == BLOCK_PAYLOAD &&
          memcmp(s_guest_read, s_fixture + 4U, BLOCK_PAYLOAD) == 0 &&
          s_guest_read[BLOCK_PAYLOAD] == 0xa5U);
    CHECK(s_native_fread_calls == calls + 3U);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)BLOCK_STRIDE);
    CHECK(s_fread_complete.short_reads == 1U &&
          s_fread_complete.completed == 1U &&
          s_fread_complete.incomplete == 0U &&
          s_fread_complete.loops == 2U &&
          s_fread_complete.unknown_pos == 0U &&
          s_fread_complete.logged == 1U);
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA CRT FREAD SHORT: tok=%08x want=1024 got=1 "
                   "loops=2 rc=1024 size=1 key=repentance.a lane=newlib "
                   "pos=1028 eof=0 err=0 errno=0 n=1", (unsigned)token);
    CHECK(s_short_receipts == 1U && strcmp(last_log_line(), expected) == 0);
    CHECK(!feof(token_stream(index)) && !ferror(token_stream(index)));
#else
    (void)expected;
    CHECK(got == 1U && s_guest_read[0] == s_fixture[4] &&
          s_guest_read[1] == 0xa5U);
    CHECK(s_native_fread_calls == calls + 1U);
    CHECK(drive_ftell(token, &position) == 0 && position == 5);
    CHECK(s_short_receipts == 0U);
#endif
    CHECK(g_isaac_vita_crt_errno == 0);
    CHECK(s_diag_logs == 0U);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

/* The tour-1 shape: the ArchivedFile block reader walks the chain with
 * fread(buf, 4, 1) headers and fread(buf, 1, len) payloads through the
 * File wrapper and ignores the payload count.  Short payload reads on three
 * blocks (one across the 16 KiB FILE buffer boundary, one returning nothing
 * at all) and a header read that consumed two of its four bytes.  Without
 * the option the first short payload leaves the cursor misaligned and the
 * next header read faults; with it the whole chain is read. */
static int test_block_chain_survives_short_reads(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t block;
    int32_t position;
    int32_t result;
    const char *fault = NULL;
    int rc;
    int index;
    int faulted = 0;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    CHECK(drive_fseek(token, 0, SEEK_SET, &result) == 0 && result == 0);
    for (block = 0U; block < BLOCK_COUNT; ++block) {
        const unsigned char *payload = s_fixture + block * BLOCK_STRIDE + 4U;

        if (block == 9U)
            set_plan(1U, (const size_t[]){ 2U }, 0);
        memset(s_guest_read, 0xa5, 8U);
        rc = drive_fread_at(token, 4U, 1U,
                            ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA,
                            &got, &fault);
#if !defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
        if (block == 4U) {
            CHECK(rc == 1 && fault &&
                  strcmp(fault, "ArchivedFile block header is invalid") == 0 &&
                  s_file_lock == 0U);
            faulted = 1;
            break;
        }
#endif
        CHECK(rc == 0 && got == 1U &&
              ld32((uint32_t)(uintptr_t)s_guest_read) == HEADER_WORD);

        if (block == 3U)
            set_plan(3U, (const size_t[]){ 1U, 511U, READ_CAPACITY }, 0);
        else if (block == 17U)
            set_plan(2U, (const size_t[]){ 900U, READ_CAPACITY }, 0);
        else if (block == 30U)
            set_plan(2U, (const size_t[]){ 0U, READ_CAPACITY }, 0);
        memset(s_guest_read, 0xa5, BLOCK_PAYLOAD);
        rc = drive_fread_at(token, 1U, BLOCK_PAYLOAD,
                            ISAAC_VITA_CRT_ARCHIVE_TYPE2_PAYLOAD_RETURN_RVA,
                            &got, &fault);
        CHECK(rc == 0);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
        CHECK(got == BLOCK_PAYLOAD &&
              memcmp(s_guest_read, payload, BLOCK_PAYLOAD) == 0);
#else
        if (block == 3U)
            CHECK(got == 1U && s_guest_read[0] == payload[0]);
        else
            CHECK(got == BLOCK_PAYLOAD &&
                  memcmp(s_guest_read, payload, BLOCK_PAYLOAD) == 0);
#endif
    }
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(!faulted && block == BLOCK_COUNT);
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES);
    CHECK(s_fread_complete.short_reads == 4U &&
          s_fread_complete.completed == 4U &&
          s_fread_complete.incomplete == 0U &&
          s_fread_complete.loops == 5U &&
          s_fread_complete.unknown_pos == 0U &&
          s_fread_complete.logged == 4U && s_short_receipts == 4U);
    CHECK(s_diag_records == 1U &&
          s_last_diag.kind == ISAAC_VITA_ARCHIVE_DIAG_OPEN);
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
#else
    CHECK(faulted && block == 4U && s_short_receipts == 0U);
    /* The fatal header read recorded its event and dumped the ring. */
    CHECK(s_diag_records == 2U &&
          s_last_diag.kind == ISAAC_VITA_ARCHIVE_DIAG_FREAD &&
          s_last_diag.position_before == (int32_t)(3U * BLOCK_STRIDE + 5U) &&
          s_last_diag.elements_returned == 1U &&
          strcmp(s_last_diag_reason, "header-bound") == 0);
    (void)position;
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
#endif
    return 0;
}

/* A wider element partially consumed: fread(buf, 4, 10) with six bytes
 * read reports one element; the cursor before the call reveals the six and
 * the completion starts at byte six of the guest buffer. */
static int test_partial_element_completed(void)
{
    uint32_t token;
    uint32_t got;
    int32_t position;
    int32_t result;
    int index;
    char expected[256];

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    CHECK(drive_fseek(token, 2000, SEEK_SET, &result) == 0 && result == 0);
    set_plan(1U, (const size_t[]){ 6U }, 0);
    memset(s_guest_read, 0xa5, 44U);
    CHECK(drive_fread(token, 4U, 10U, &got) == 0);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(got == 10U && memcmp(s_guest_read, s_fixture + 2000U, 40U) == 0 &&
          s_guest_read[40] == 0xa5U);
    CHECK(drive_ftell(token, &position) == 0 && position == 2040);
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA CRT FREAD SHORT: tok=%08x want=10 got=1 "
                   "loops=1 rc=10 size=4 key=repentance.a lane=newlib "
                   "pos=2040 eof=0 err=0 errno=0 n=1", (unsigned)token);
    CHECK(s_short_receipts == 1U && strcmp(last_log_line(), expected) == 0);
    CHECK(s_fread_complete.completed == 1U &&
          s_fread_complete.unknown_pos == 0U);
#else
    (void)expected;
    CHECK(got == 1U && memcmp(s_guest_read, s_fixture + 2000U, 6U) == 0 &&
          s_guest_read[6] == 0xa5U);
    CHECK(drive_ftell(token, &position) == 0 && position == 2006);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

/* A real EOF ends the completion: the guest gets what libc would have
 * given for the whole request, the EOF flag stands, the receipt says so.
 * A short read that already carries the EOF flag is never touched. */
static int test_real_eof_passes_through(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t calls;
    int32_t position;
    int32_t result;
    int index;
    FILE *reference;
    char expected[256];

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    reference = reference_open();
    CHECK(reference != NULL);
    CHECK(drive_fseek(token, (int32_t)FIXTURE_BYTES - 10, SEEK_SET,
                      &result) == 0 && result == 0);
    CHECK(fseek(reference, (long)FIXTURE_BYTES - 10L, SEEK_SET) == 0);
    set_plan(1U, (const size_t[]){ 1U }, 0);
    memset(s_guest_read, 0xa5, 100U);
    CHECK(drive_fread(token, 1U, 100U, &got) == 0);
    CHECK(fread(s_reference_read, 1U, 100U, reference) == 10U);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(got == 10U && memcmp(s_guest_read, s_reference_read, 10U) == 0 &&
          s_guest_read[10] == 0xa5U);
    CHECK(feof(token_stream(index)) && !ferror(token_stream(index)));
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES &&
          position == (int32_t)ftell(reference));
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA CRT FREAD SHORT: tok=%08x want=100 got=1 "
                   "loops=1 rc=10 size=1 key=repentance.a lane=newlib "
                   "pos=%ld eof=1 err=0 errno=0 n=1", (unsigned)token,
                   (long)FIXTURE_BYTES);
    CHECK(s_short_receipts == 1U && strcmp(last_log_line(), expected) == 0);
    CHECK(s_fread_complete.short_reads == 1U &&
          s_fread_complete.completed == 0U &&
          s_fread_complete.incomplete == 1U &&
          s_fread_complete.loops == 1U);
#else
    (void)expected;
    CHECK(got == 1U && s_guest_read[0] == s_reference_read[0] &&
          s_guest_read[1] == 0xa5U);
    CHECK(!feof(token_stream(index)));
    CHECK(drive_ftell(token, &position) == 0 &&
          position == (int32_t)FIXTURE_BYTES - 9);
#endif

    /* No fake shortness: the plain short read at EOF is identical to libc
     * and leaves the counters alone. */
    calls = s_native_fread_calls;
    CHECK(drive_fseek(token, (int32_t)FIXTURE_BYTES - 10, SEEK_SET,
                      &result) == 0 && result == 0);
    CHECK(fseek(reference, (long)FIXTURE_BYTES - 10L, SEEK_SET) == 0);
    CHECK(drive_fread(token, 1U, 100U, &got) == 0 && got == 10U &&
          fread(s_reference_read, 1U, 100U, reference) == 10U &&
          memcmp(s_guest_read, s_reference_read, 10U) == 0);
    CHECK(s_native_fread_calls == calls + 1U);
    CHECK(feof(token_stream(index)) && feof(reference));
    CHECK(drive_fread(token, 1U, 100U, &got) == 0 && got == 0U &&
          fread(s_reference_read, 1U, 100U, reference) == 0U);
    CHECK(s_native_fread_calls == calls + 2U);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_fread_complete.short_reads == 1U && s_short_receipts == 1U);
#else
    CHECK(s_short_receipts == 0U);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    (void)fclose(reference);
    return 0;
}

/* The raw SceIo lane (exact-ENOMEM fallback) keeps its own EOF/error flags
 * and its fread already loops to the request; a short read there always
 * carries a flag, so the completion never enters and the newlib seam is
 * never touched. */
static int test_raw_lane_flags_pass_through(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t calls;
    int32_t position;
    int32_t result;
    int index;

    reset_oracle();
    s_force_fopen_errno = ENOMEM;
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U &&
          s_raw_open_calls == 1U);
    index = token_index(token);
    CHECK(index >= 0 && s_file_tokens[index].raw_archive.live &&
          strcmp(s_file_tokens[index].file.key, "repentance.a") == 0);
    calls = s_native_fread_calls;
    CHECK(drive_fread(token, 1U, 16U, &got) == 0 && got == 16U &&
          memcmp(s_guest_read, s_fixture, 16U) == 0);

    /* A failing pread: error flag, short, untouched. */
    s_pread_fail_call = s_pread_calls + 1U;
    g_isaac_vita_crt_errno = 0;
    CHECK(drive_fread(token, 1U, 16U, &got) == 0 && got == 0U);
    CHECK(s_file_tokens[index].raw_archive.error == 1U &&
          s_file_tokens[index].raw_archive.eof == 0U);
    CHECK(g_isaac_vita_crt_errno == EIO);
    CHECK(drive_ftell(token, &position) == 0 && position == 16);

    /* Past the end: EOF flag, short, untouched. */
    s_pread_fail_call = 0U;
    CHECK(drive_fseek(token, (int32_t)FIXTURE_BYTES - 5, SEEK_SET,
                      &result) == 0 && result == 0);
    CHECK(drive_fread(token, 1U, 16U, &got) == 0 && got == 5U &&
          memcmp(s_guest_read, s_fixture + FIXTURE_BYTES - 5U, 5U) == 0);
    CHECK(s_file_tokens[index].raw_archive.eof == 1U);
    CHECK(s_native_fread_calls == calls);
    CHECK(s_short_receipts == 0U);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_fread_complete.short_reads == 0U);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0 &&
          s_raw_close_calls == 1U);
    return 0;
}

/* A lower layer that keeps answering one byte (or nothing) without a flag
 * is bounded: eight completion reads, then the guest gets the partial
 * count. */
static int test_loop_bound(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t calls;
    int32_t position;
    int32_t result;
    int index;
    char expected[256];

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    CHECK(drive_fseek(token, 0, SEEK_SET, &result) == 0 && result == 0);
    calls = s_native_fread_calls;
    set_plan(1U, (const size_t[]){ 1U }, 1);
    memset(s_guest_read, 0xa5, 100U);
    CHECK(drive_fread(token, 1U, 100U, &got) == 0);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(got == 9U && memcmp(s_guest_read, s_fixture, 9U) == 0 &&
          s_guest_read[9] == 0xa5U);
    CHECK(s_native_fread_calls == calls + 9U);
    CHECK(drive_ftell(token, &position) == 0 && position == 9);
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA CRT FREAD SHORT: tok=%08x want=100 got=1 "
                   "loops=8 rc=9 size=1 key=repentance.a lane=newlib "
                   "pos=9 eof=0 err=0 errno=0 n=1", (unsigned)token);
    CHECK(s_short_receipts == 1U && strcmp(last_log_line(), expected) == 0);
    CHECK(s_fread_complete.short_reads == 1U &&
          s_fread_complete.completed == 0U &&
          s_fread_complete.incomplete == 1U &&
          s_fread_complete.loops == 8U);
#else
    (void)expected;
    CHECK(got == 1U && s_native_fread_calls == calls + 1U);
    CHECK(drive_ftell(token, &position) == 0 && position == 1);
#endif

    /* Nothing at all, forever: no progress, still bounded, rc is what the
     * first read said. */
    calls = s_native_fread_calls;
    set_plan(1U, (const size_t[]){ 0U }, 1);
    CHECK(drive_fread(token, 1U, 100U, &got) == 0 && got == 0U);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_native_fread_calls == calls + 9U);
    CHECK(s_fread_complete.short_reads == 2U &&
          s_fread_complete.incomplete == 2U &&
          s_fread_complete.loops == 16U && s_short_receipts == 2U);
#else
    CHECK(s_native_fread_calls == calls + 1U);
#endif
    clear_plan();
    CHECK(!feof(token_stream(index)) && !ferror(token_stream(index)));
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

/* Receipts stop after the eighth short read; the counter line carries the
 * rest, alone and chained to the seek shadow report. */
static int test_receipts_rate_limited_and_reported(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t i;
    int32_t result;
    int index;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    CHECK(drive_fseek(token, 0, SEEK_SET, &result) == 0 && result == 0);
    for (i = 0U; i < 10U; ++i) {
        set_plan(2U, (const size_t[]){ 1U, READ_CAPACITY }, 0);
        CHECK(drive_fread(token, 1U, 64U, &got) == 0);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
        CHECK(got == 64U &&
              memcmp(s_guest_read, s_fixture + i * 64U, 64U) == 0);
#else
        CHECK(got == 1U);
#endif
    }
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_short_receipts == 8U && s_fread_complete.logged == 8U &&
          s_fread_complete.short_reads == 10U &&
          s_fread_complete.completed == 10U &&
          s_fread_complete.loops == 10U);
    CHECK(strstr(last_log_line(), " n=8") != NULL);
    isaac_vita_crt_fread_complete_report("oracle");
    CHECK(s_complete_reports == 1U &&
          strcmp(last_log_line(),
                 "KAGE VITA CRT FREAD COMPLETE: why=oracle short=10 "
                 "completed=10 incomplete=0 loops=10 unknown_pos=0 "
                 "logged=8") == 0);
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    /* The seek shadow report (the loading screen's periodic line) ends
     * with the completion counters. */
    isaac_vita_crt_seek_shadow_report("oracle");
    CHECK(s_complete_reports == 2U &&
          STARTS_WITH(last_log_line(),
                      "KAGE VITA CRT FREAD COMPLETE: why=oracle short=10 "));
#endif
#else
    CHECK(s_short_receipts == 0U && s_complete_reports == 0U);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

/* Only packed-archive tokens are completed: a loose file gets the short
 * read as is, and a write-mode open never carries an archive key. */
static int test_loose_and_write_tokens_untouched(void)
{
    uint32_t token;
    uint32_t got;
    uint32_t calls;
    int32_t position;
    int32_t result;
    int index;

    reset_oracle();
    s_mapped_path = LOOSE_XML;
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0 && s_file_tokens[index].file.key[0] == '\0');
    calls = s_native_fread_calls;
    set_plan(2U, (const size_t[]){ 1U, READ_CAPACITY }, 0);
    memset(s_guest_read, 0xa5, 64U);
    CHECK(drive_fread(token, 1U, 64U, &got) == 0 && got == 1U &&
          s_guest_read[0] == s_fixture[0] && s_guest_read[1] == 0xa5U);
    CHECK(s_native_fread_calls == calls + 1U);
    CHECK(drive_ftell(token, &position) == 0 && position == 1);
    clear_plan();
    CHECK(drive_fclose(token, &result) == 0 && result == 0);

    s_mapped_path = REPENTANCE;
    CHECK(drive_fopen(s_mode_wb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0 && s_file_tokens[index].file.key[0] == '\0');
    CHECK(drive_fclose(token, &result) == 0 && result == 0);

    CHECK(s_short_receipts == 0U);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_fread_complete.short_reads == 0U);
#endif
    return 0;
}

/* Zero-length requests are outside the completion. */
static int test_zero_request_untouched(void)
{
    uint32_t token;
    uint32_t got;
    int32_t position;
    int32_t result;
    int index;

    reset_oracle();
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    set_plan(1U, (const size_t[]){ 0U }, 1);
    CHECK(drive_fread(token, 1U, 0U, &got) == 0 && got == 0U);
    CHECK(drive_fread(token, 0U, 5U, &got) == 0 && got == 0U);
    clear_plan();
    CHECK(drive_ftell(token, &position) == 0 && position == 0);
    CHECK(s_short_receipts == 0U);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_fread_complete.short_reads == 0U);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

/* afterbirth*.a short reads are arcdiag anomalies today; a completed read
 * is whole and records none, an uncompleted one still does. */
static int test_relevant_key_anomaly(void)
{
    uint32_t token;
    uint32_t got;
    int32_t result;
    int index;

    reset_oracle();
    s_mapped_path = AFTERBIRTHP;
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0 &&
          strcmp(s_file_tokens[index].file.key, "afterbirthp.a") == 0 &&
          s_diag_records == 1U);
    set_plan(2U, (const size_t[]){ 7U, READ_CAPACITY }, 0);
    CHECK(drive_fread(token, 1U, 64U, &got) == 0);
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(got == 64U && s_diag_records == 1U && s_short_receipts == 1U &&
          strstr(last_log_line(), "key=afterbirthp.a") != NULL);
#else
    CHECK(got == 7U && s_diag_records == 2U &&
          s_last_diag.kind == ISAAC_VITA_ARCHIVE_DIAG_FREAD &&
          s_last_diag.elements_returned == 7U &&
          strcmp(s_last_diag_reason, "short-read") == 0);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    return 0;
}

/* Deterministic xorshift so the sequence is reproducible from the seed. */
static uint32_t s_random_state;

static uint32_t next_random(void)
{
    uint32_t x = s_random_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_random_state = x;
    return x;
}

/* Random seeks and reads (sizes 1/2/4/16, up to 16 KiB) against a plain
 * libc FILE following the same operations.  With the option, half the
 * reads are made short by the lower layer (one to three flagless partial
 * answers, then the rest) and every result, byte and cursor must still
 * equal the plain FILE's: a completed read is indistinguishable from a
 * whole one.  Without the option no read is faked and the CRT path itself
 * is checked against libc. */
static int test_random_sequence_matches_reference(unsigned ops, uint32_t seed)
{
    static const uint32_t sizes[4] = { 1U, 2U, 4U, 16U };
    uint32_t token;
    uint32_t got;
    uint32_t expected;
    int32_t position;
    int32_t result;
    unsigned op;
    int index;
    FILE *reference;

    reset_oracle();
    s_random_state = seed;
    CHECK(drive_fopen(s_mode_rb, &token) == 0 && token != 0U);
    index = token_index(token);
    CHECK(index >= 0);
    reference = reference_open();
    CHECK(reference != NULL);
    for (op = 0U; op < ops; ++op) {
        uint32_t roll = next_random();

        clear_plan();
        if ((roll & 3U) == 0U) {
            int32_t offset = (int32_t)(next_random() % (FIXTURE_BYTES + 1U));

            CHECK(drive_fseek(token, offset, SEEK_SET, &result) == 0 &&
                  result == 0);
            CHECK(fseek(reference, (long)offset, SEEK_SET) == 0);
        } else {
            uint32_t size = sizes[(roll >> 2) & 3U];
            uint32_t count = 1U + next_random() % (16384U / size);
            uint32_t bytes = size * count;

#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
            if (roll & 16U) {
                size_t plan[4];
                unsigned entries = 1U + next_random() % 3U;
                unsigned i;

                for (i = 0U; i < entries; ++i)
                    plan[i] = next_random() % (bytes + 1U);
                plan[entries] = READ_CAPACITY;
                set_plan(entries + 1U, plan, 0);
            }
#endif
            memset(s_guest_read, 0xa5, bytes);
            memset(s_reference_read, 0x5a, bytes);
            CHECK(drive_fread(token, size, count, &got) == 0);
            expected = (uint32_t)fread(s_reference_read, size, count,
                                       reference);
            CHECK(got == expected);
            CHECK(memcmp(s_guest_read, s_reference_read,
                         (size_t)got * size) == 0);
        }
        CHECK(drive_ftell(token, &position) == 0 &&
              position == (int32_t)ftell(reference));
        CHECK((feof(token_stream(index)) != 0) == (feof(reference) != 0));
        CHECK(!ferror(token_stream(index)) && !ferror(reference));
    }
    clear_plan();
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_fread_complete.short_reads > 0U &&
          s_fread_complete.unknown_pos == 0U &&
          s_fread_complete.logged == 8U && s_short_receipts == 8U);
    printf("fread complete oracle: %u ops, %u short, %u completed, "
           "%u incomplete (EOF), %u completion reads\n", ops,
           (unsigned)s_fread_complete.short_reads,
           (unsigned)s_fread_complete.completed,
           (unsigned)s_fread_complete.incomplete,
           (unsigned)s_fread_complete.loops);
#endif
    CHECK(drive_fclose(token, &result) == 0 && result == 0);
    (void)fclose(reference);
    return 0;
}

/* The shutdown path emits the counter line once, whether or not the seek
 * shadow report is the one carrying it. */
static int test_shutdown_reports(void)
{
    reset_oracle();
    isaac_vita_crt_archive_cache_shutdown();
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
    CHECK(s_complete_reports == 1U &&
          strcmp(last_log_line(),
                 "KAGE VITA CRT FREAD COMPLETE: why=shutdown short=0 "
                 "completed=0 incomplete=0 loops=0 unknown_pos=0 "
                 "logged=0") == 0);
#else
    CHECK(s_complete_reports == 0U);
#endif
    return 0;
}

static int write_fixture(const char *directory)
{
    FILE *stream;
    uint32_t block;

    (void)snprintf(s_fixture_path, sizeof s_fixture_path,
                   "%s/fread-complete-fixture.bin", directory);
    (void)snprintf(s_scratch_path, sizeof s_scratch_path,
                   "%s/fread-complete-scratch.bin", directory);
    for (block = 0U; block < BLOCK_COUNT; ++block) {
        unsigned char *at = s_fixture + block * BLOCK_STRIDE;
        uint32_t index;

        at[0] = (unsigned char)(HEADER_WORD & 0xffU);
        at[1] = (unsigned char)((HEADER_WORD >> 8) & 0xffU);
        at[2] = (unsigned char)((HEADER_WORD >> 16) & 0xffU);
        at[3] = (unsigned char)((HEADER_WORD >> 24) & 0xffU);
        for (index = 0U; index < BLOCK_PAYLOAD; ++index) {
            uint32_t i = block * BLOCK_PAYLOAD + index;

            at[4U + index] = (unsigned char)(0x20U + (i * 7U + (i >> 8)) % 0xe0U);
        }
    }
    stream = fopen(s_fixture_path, "wb");
    if (!stream ||
        fwrite(s_fixture, 1U, FIXTURE_BYTES, stream) != FIXTURE_BYTES ||
        fclose(stream) != 0)
        return -1;
    stream = fopen(s_scratch_path, "wb");
    if (!stream || fclose(stream) != 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    unsigned ops = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 2000U;

    if ((uintptr_t)s_guest_read > UINT32_MAX ||
        (uintptr_t)&s_guest_stack[64] > UINT32_MAX ||
        (uintptr_t)s_mode_rb > UINT32_MAX) {
        fprintf(stderr, "fread complete oracle needs 32-bit static addresses\n");
        return 1;
    }
    if (write_fixture(directory) != 0) {
        fprintf(stderr, "fread complete oracle cannot write %s\n",
                s_fixture_path);
        return 1;
    }
    if (test_payload_short_completed() ||
        test_block_chain_survives_short_reads() ||
        test_partial_element_completed() ||
        test_real_eof_passes_through() ||
        test_raw_lane_flags_pass_through() ||
        test_loop_bound() ||
        test_receipts_rate_limited_and_reported() ||
        test_loose_and_write_tokens_untouched() ||
        test_zero_request_untouched() ||
        test_relevant_key_anomaly() ||
        test_random_sequence_matches_reference(ops, UINT32_C(0x5eed0002)) ||
        test_random_sequence_matches_reference(ops, UINT32_C(0x00c0ffee)) ||
        test_shutdown_reports())
        return 1;
    (void)remove(s_fixture_path);
    (void)remove(s_scratch_path);
    printf("fread complete oracle (ISAAC_VITA_CRT_FREAD_COMPLETE %s, "
           "seek shadow %s): PASS\n",
#if defined(ISAAC_VITA_CRT_FREAD_COMPLETE)
           "on",
#else
           "off",
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
           "on"
#else
           "off"
#endif
    );
    return 0;
}
