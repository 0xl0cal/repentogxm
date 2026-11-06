/* Host oracle for host_vita_runtime_settings.c
 * (ISAAC_VITA_RUNTIME_SETTINGS=1 ISAAC_VITA_RUNTIME_SETTINGS_ORACLE=1).
 *
 * A fake file replaces sceIoOpen/sceIoRead/sceIoClose through the seams
 * declared in host_vita_runtime_settings.h; isaac_vita_log is captured line
 * by line.  Two compiles: with -DISAAC_VITA_FULLSPEED_SCHEDULER=1 the
 * scheduler setter is a recording stub and the receipt carries no fps
 * suffix; without it the receipt says "(ignored:no-fullspeed-scheduler)"
 * and nothing is pushed.  Golden receipt lines (design section 1.5) for
 * absent, open error, ok (LF and CRLF, duplicates), schema missing, bad
 * schema, garbage, exactly 4096 bytes, 4097+ bytes (too-large, only 4097
 * requested), chunked reads, a read failure after a partial read, the a/b
 * guard, custom remap / chords / restart_hold / stick_swap, rt on a chord
 * key, and the REJECTED second line; plus the I/O discipline: the path, one
 * open, one close per successful open (none after a failed open), every
 * read asks for exactly the remaining room of the 4097-byte buffer and
 * carries the opened descriptor.  A third compile adds
 * -DISAAC_VITA_DISPLAY_RASTER_WIDTH=720 (the CMake block forwards the build
 * raster to the loader): every receipt whose file leaves display_raster
 * unset must then say raster=720x408, an explicit key keeps its value. */
#include "host_vita_runtime_settings.h"
#include "host_vita_settings_parse.h"
#include "platform.h"
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
#include "kage_vita_fullspeed_scheduler.h"
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORACLE_PATH ISAAC_VITA_NATIVE_DATA_ROOT "/port.ini"
#define ORACLE_DESCRIPTOR 0x40010203
#define ORACLE_ENOENT ((int)0x80010002u)
#define ORACLE_EACCES ((int)0x80010013u)
#define ORACLE_EIO ((int)0x80010005u)
#define ORACLE_BUFFER_BYTES 4097u

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
# define FPS_SUFFIX ""
# define VARIANT_BASE "fullspeed-scheduler"
#else
# define FPS_SUFFIX "(ignored:no-fullspeed-scheduler)"
# define VARIANT_BASE "no-fullspeed-scheduler"
#endif
/* Receipt text of the build raster: what every file that leaves
 * display_raster unset must report (design section 1.3). */
#if !defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH) || \
    ISAAC_VITA_DISPLAY_RASTER_WIDTH == 960
# define RASTER_DEFAULT " raster=960x544(ignored:not-compiled)"
# define VARIANT VARIANT_BASE
#elif ISAAC_VITA_DISPLAY_RASTER_WIDTH == 720
# define RASTER_DEFAULT " raster=720x408(ignored:not-compiled)"
# define VARIANT VARIANT_BASE "-raster720"
#elif ISAAC_VITA_DISPLAY_RASTER_WIDTH == 480
# define RASTER_DEFAULT " raster=480x272(ignored:not-compiled)"
# define VARIANT VARIANT_BASE "-raster480"
#else
# error "ISAAC_VITA_DISPLAY_RASTER_WIDTH must be 960, 720 or 480"
#endif
#define RASTER_960 " raster=960x544(ignored:not-compiled)"
#define RASTER_480 " raster=480x272(ignored:not-compiled)"
#define LINE_HEAD "ISAAC VITA SETTINGS: path=" ORACLE_PATH " status="
#define REJECTED_HEAD "ISAAC VITA SETTINGS REJECTED: "

static unsigned s_checks;
static unsigned s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void check_u(unsigned long got, unsigned long expected, const char *what)
{
    ++s_checks;
    if (got != expected) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s: got %lu expected %lu\n", what, got,
                expected);
    }
}

static void check_str(const char *got, const char *expected, const char *what)
{
    ++s_checks;
    if (strcmp(got, expected) != 0) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s:\n  got      \"%s\"\n  expected \"%s\"\n",
                what, got, expected);
    }
}

/* ---- fake file --------------------------------------------------------- */

#define FILE_CAPACITY 8192u
static char s_file[FILE_CAPACITY];
static size_t s_file_length;
static int s_open_result;
static uint32_t s_read_chunk;      /* longest read answered; 0 = unlimited */
static int s_read_fail_index;      /* 0-based read call that fails; -1 never */
static int s_read_fail_error;
static size_t s_read_position;
static unsigned s_open_calls;
static unsigned s_read_calls;
static unsigned s_close_calls;
static int s_read_descriptor_ok;
static int s_read_request_ok;
static int s_close_descriptor;
static char s_open_path[128];

int isaac_vita_runtime_settings_oracle_open(const char *path)
{
    ++s_open_calls;
    (void)snprintf(s_open_path, sizeof s_open_path, "%s", path);
    s_read_position = 0u;
    return s_open_result;
}

int isaac_vita_runtime_settings_oracle_read(int descriptor, void *buffer,
                                            uint32_t size)
{
    unsigned index = s_read_calls++;
    size_t remaining;
    uint32_t count;

    if (descriptor != s_open_result) {
        s_read_descriptor_ok = 0;
    }
    /* The loader must ask for exactly the room left in its 4097-byte
     * buffer: never 0, never past the end. */
    if (s_read_position >= ORACLE_BUFFER_BYTES ||
            size != ORACLE_BUFFER_BYTES - (uint32_t)s_read_position) {
        s_read_request_ok = 0;
    }
    if ((int)index == s_read_fail_index) {
        return s_read_fail_error;
    }
    remaining = s_file_length - s_read_position;
    count = remaining < size ? (uint32_t)remaining : size;
    if (s_read_chunk != 0u && count > s_read_chunk) {
        count = s_read_chunk;
    }
    memcpy(buffer, s_file + s_read_position, count);
    s_read_position += count;
    return (int)count;
}

int isaac_vita_runtime_settings_oracle_close(int descriptor)
{
    ++s_close_calls;
    s_close_descriptor = descriptor;
    return 0;
}

/* ---- captured log ------------------------------------------------------ */

#define LOG_LINES 4u
#define LOG_BYTES 512u
static char s_lines[LOG_LINES][LOG_BYTES];
static unsigned s_line_count;

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
    unsigned slot = s_line_count < LOG_LINES ? s_line_count : LOG_LINES - 1u;

    va_start(arguments, format);
    (void)vsnprintf(s_lines[slot], LOG_BYTES, format, arguments);
    va_end(arguments);
    ++s_line_count;
}

/* ---- consumer stub ----------------------------------------------------- */

#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
static unsigned s_duty_calls;
static uint8_t s_duty_last;

void kage_vita_fullspeed_scheduler_set_duty(uint8_t k)
{
    ++s_duty_calls;
    s_duty_last = k;
}
#endif

/* ---- drivers ----------------------------------------------------------- */

static void load_bytes(const char *bytes, size_t length, int open_result)
{
    if (length > FILE_CAPACITY) {
        fprintf(stderr, "oracle file too long\n");
        ++s_failures;
        return;
    }
    memcpy(s_file, bytes, length);
    s_file_length = length;
    s_open_result = open_result;
    s_open_calls = 0u;
    s_read_calls = 0u;
    s_close_calls = 0u;
    s_read_descriptor_ok = 1;
    s_read_request_ok = 1;
    s_close_descriptor = -1;
    s_open_path[0] = '\0';
    s_line_count = 0u;
    memset(s_lines, 0, sizeof s_lines);
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    s_duty_calls = 0u;
    s_duty_last = 0xffu;
#endif
    isaac_vita_runtime_settings_load();
}

static void load_text(const char *text, int open_result)
{
    load_bytes(text, strlen(text), open_result);
}

static void set_reads(uint32_t chunk, int fail_index, int fail_error)
{
    s_read_chunk = chunk;
    s_read_fail_index = fail_index;
    s_read_fail_error = fail_error;
}

static void expect_io(const char *what, unsigned lines, unsigned reads,
                      unsigned closes)
{
    char label[160];

    (void)snprintf(label, sizeof label, "%s: log lines", what);
    check_u(s_line_count, lines, label);
    (void)snprintf(label, sizeof label, "%s: one open", what);
    check_u(s_open_calls, 1u, label);
    (void)snprintf(label, sizeof label, "%s: path", what);
    check_str(s_open_path, ORACLE_PATH, label);
    (void)snprintf(label, sizeof label, "%s: read calls", what);
    check_u(s_read_calls, reads, label);
    (void)snprintf(label, sizeof label, "%s: close calls", what);
    check_u(s_close_calls, closes, label);
    (void)snprintf(label, sizeof label, "%s: reads carry the descriptor",
                   what);
    check(s_read_descriptor_ok, label);
    (void)snprintf(label, sizeof label, "%s: reads ask for the exact room",
                   what);
    check(s_read_request_ok, label);
    if (closes != 0u) {
        (void)snprintf(label, sizeof label, "%s: close descriptor", what);
        check_u((unsigned long)(unsigned)s_close_descriptor,
                (unsigned long)(unsigned)ORACLE_DESCRIPTOR, label);
    }
}

static void expect_duty(const char *what, unsigned k)
{
#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)
    char label[160];

    (void)snprintf(label, sizeof label, "%s: one duty push", what);
    check_u(s_duty_calls, 1u, label);
    (void)snprintf(label, sizeof label, "%s: duty k", what);
    check_u(s_duty_last, k, label);
#else
    (void)what;
    (void)k;
#endif
}

/* Raster text expected by the next expect_receipt() calls: the build raster
 * unless a scenario sets display_raster explicitly. */
static const char *s_raster = RASTER_DEFAULT;

static void expect_receipt(
    const char *what, const char *status, unsigned bytes, unsigned lines,
    unsigned rejected, unsigned unknown, unsigned dup, unsigned fps_lock,
    const char *remap, const char *chords, unsigned restart_hold,
    unsigned stick_swap)
{
    char expected[LOG_BYTES];

    (void)snprintf(
        expected, sizeof expected,
        LINE_HEAD "%s bytes=%u lines=%u rejected=%u unknown=%u dup=%u "
        "fps_lock=%u" FPS_SUFFIX "%s remap=%s chords=%s "
        "restart_hold=%u stick_swap=%u",
        status, bytes, lines, rejected, unknown, dup, fps_lock, s_raster,
        remap, chords, restart_hold, stick_swap);
    check_str(s_lines[0], expected, what);
}

static void expect_rejected(const char *what, const char *tail)
{
    char expected[LOG_BYTES];

    (void)snprintf(expected, sizeof expected, REJECTED_HEAD "%s", tail);
    check_str(s_lines[1], expected, what);
}

/* Receipt expectation derived from the parser for files whose counters are
 * not worth counting by hand (the padded 4096-byte corpus). */
static void expect_receipt_from_parser(const char *what, const char *bytes,
                                       size_t length, unsigned duty_k)
{
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    char scratch[FILE_CAPACITY];

    memcpy(scratch, bytes, length);
    (void)isaac_vita_settings_parse(scratch, length, &parsed, &receipt);
    expect_receipt(
        what, isaac_vita_settings_status_name(
                  (enum isaac_vita_settings_status)receipt.status),
        receipt.bytes, receipt.lines, receipt.rejected, receipt.unknown,
        receipt.dup, parsed.fps_lock, "default", "rb/back", 1u, 0u);
    expect_duty(what, duty_k);
}

/* head, then '#' comment lines of 64 bytes (LF every 64th byte) up to
 * exactly total bytes; the tail line may lack its LF. */
static size_t fill_padded(char *out, size_t total, const char *head)
{
    size_t length = strlen(head);
    size_t i;

    memcpy(out, head, length);
    for (i = length; i < total; ++i) {
        out[i] = ((i - length) % 64u == 63u) ? '\n' : '#';
    }
    return total;
}

/* ---- scenarios --------------------------------------------------------- */

static void test_absent_and_open_error(void)
{
    set_reads(0u, -1, 0);
    load_text("", ORACLE_ENOENT);
    expect_io("absent", 1u, 0u, 0u);
    expect_receipt("absent line", "absent", 0u, 0u, 0u, 0u, 0u, 60u,
                   "default", "rb/back", 1u, 0u);
    expect_duty("absent", 1u);

    load_text("schema=1\nfps_lock=30\n", ORACLE_EACCES);
    expect_io("open error", 1u, 0u, 0u);
    expect_receipt("open error line", "io-error(0x80010013)", 0u, 0u, 0u,
                   0u, 0u, 60u, "default", "rb/back", 1u, 0u);
    expect_duty("open error", 1u);
}

static void test_ok_files(void)
{
    static const char k_lf[] = "schema=1\nfps_lock=45\n";
    static const char k_crlf[] = "schema=1\r\nfps_lock=45\r\nfps_lock=40\r\n";
    static const char k_noisy[] =
        "schema=1\n"
        "fps_lock=30\n"
        "colour=red\n"   /* unknown */
        "fps_lock=99\n"  /* rejected (bad value), not a duplicate */
        "bogus line\n"   /* rejected (key grammar) */
        "x=y\n";         /* unknown */

    set_reads(0u, -1, 0);
    load_text(k_lf, ORACLE_DESCRIPTOR);
    expect_io("ok lf", 1u, 2u, 1u);
    expect_receipt("ok lf line", "ok", (unsigned)(sizeof k_lf - 1u), 2u, 0u,
                   0u, 0u, 45u, "default", "rb/back", 1u, 0u);
    expect_duty("ok lf", 2u);

    load_text(k_crlf, ORACLE_DESCRIPTOR);
    expect_io("ok crlf dup", 1u, 2u, 1u);
    expect_receipt("ok crlf dup line", "ok", (unsigned)(sizeof k_crlf - 1u),
                   3u, 0u, 0u, 1u, 40u, "default", "rb/back", 1u, 0u);
    expect_duty("ok crlf dup", 3u);

    load_text(k_noisy, ORACLE_DESCRIPTOR);
    expect_io("ok noisy", 2u, 2u, 1u);
    expect_receipt("ok noisy line", "ok", (unsigned)(sizeof k_noisy - 1u),
                   6u, 2u, 2u, 0u, 30u, "default", "rb/back", 1u, 0u);
    expect_rejected("ok noisy rejected line",
                    "lines=4,5,0,0 unknown=colour,x,,");
    expect_duty("ok noisy", 0u);
}

static void test_remap_fields(void)
{
    static const char k_custom[] = "schema=1\nbutton.l=lt\nbutton.r=lb\n";
    static const char k_chords[] =
        "schema=1\n"
        "chord.start_l=none\n"
        "chord.start_r=lt\n"
        "chord.restart_hold=off\n"
        "stick.swap=on\n"
        "touch.top_left=a\n";
    static const char k_guard[] = "schema=1\nbutton.cross=x\nfps_lock=45\n";
    static const char k_rt[] = "schema=1\nchord.start_l=rt\n";

    set_reads(0u, -1, 0);
    load_text(k_custom, ORACLE_DESCRIPTOR);
    expect_io("custom remap", 1u, 2u, 1u);
    expect_receipt("custom remap line", "ok", (unsigned)(sizeof k_custom - 1u),
                   3u, 0u, 0u, 0u, 60u, "custom", "rb/back", 1u, 0u);
    expect_duty("custom remap", 1u);

    load_text(k_chords, ORACLE_DESCRIPTOR);
    expect_io("chords", 1u, 2u, 1u);
    expect_receipt("chords line", "ok", (unsigned)(sizeof k_chords - 1u), 6u,
                   0u, 0u, 0u, 60u, "custom", "none/lt", 0u, 1u);
    expect_duty("chords", 1u);

    /* No physical button maps to a: the whole remap section falls back to
     * the defaults, fps_lock stays applied. */
    load_text(k_guard, ORACLE_DESCRIPTOR);
    expect_io("guard", 1u, 2u, 1u);
    expect_receipt("guard line", "ok", (unsigned)(sizeof k_guard - 1u), 3u, 0u,
                   0u, 0u, 45u, "default(guard)", "rb/back", 1u, 0u);
    expect_duty("guard", 2u);

    /* rt is not a chord element: line rejected, key keeps its default. */
    load_text(k_rt, ORACLE_DESCRIPTOR);
    expect_io("rt on chord", 2u, 2u, 1u);
    expect_receipt("rt on chord line", "ok", (unsigned)(sizeof k_rt - 1u), 2u,
                   1u, 0u, 0u, 60u, "default", "rb/back", 1u, 0u);
    expect_rejected("rt on chord rejected line", "lines=2,0,0,0 unknown=,,,");
    expect_duty("rt on chord", 1u);
}

/* display_raster: an explicit key is reported as written (the value is
 * parse/report-only until WP6); an absent or rejected key reports the build
 * raster the CMake block forwarded, never the parser's 960 (section 1.3). */
static void test_raster(void)
{
    static const char k_480[] = "schema=1\ndisplay_raster=480\n";
    static const char k_960[] = "schema=1\ndisplay_raster=960\nfps_lock=45\n";
    static const char k_bad[] = "schema=1\ndisplay_raster=1080\n";

    set_reads(0u, -1, 0);
    s_raster = RASTER_480;
    load_text(k_480, ORACLE_DESCRIPTOR);
    expect_io("raster 480", 1u, 2u, 1u);
    expect_receipt("raster 480 line", "ok", (unsigned)(sizeof k_480 - 1u), 2u,
                   0u, 0u, 0u, 60u, "default", "rb/back", 1u, 0u);
    expect_duty("raster 480", 1u);

    /* Explicit 960 stays 960 on every build (the 720 compile included). */
    s_raster = RASTER_960;
    load_text(k_960, ORACLE_DESCRIPTOR);
    expect_io("raster 960 explicit", 1u, 2u, 1u);
    expect_receipt("raster 960 explicit line", "ok",
                   (unsigned)(sizeof k_960 - 1u), 3u, 0u, 0u, 0u, 45u,
                   "default", "rb/back", 1u, 0u);
    expect_duty("raster 960 explicit", 2u);

    /* Rejected value: line counted, key unset, build raster reported. */
    s_raster = RASTER_DEFAULT;
    load_text(k_bad, ORACLE_DESCRIPTOR);
    expect_io("raster rejected", 2u, 2u, 1u);
    expect_receipt("raster rejected line", "ok", (unsigned)(sizeof k_bad - 1u),
                   2u, 1u, 0u, 0u, 60u, "default", "rb/back", 1u, 0u);
    expect_rejected("raster rejected second line",
                    "lines=2,0,0,0 unknown=,,,");
    expect_duty("raster rejected", 1u);
    check(strstr(s_lines[0], RASTER_DEFAULT) != NULL,
          "raster rejected: build raster in the receipt");
}

static void test_ignored_files(void)
{
    static const char k_no_schema[] = "fps_lock=30\n";
    static const char k_bad_schema[] = "schema=2\nfps_lock=30\n";
    static const char k_garbage[] = "schema=1\nfps\0lock=30\n";

    set_reads(0u, -1, 0);
    load_text(k_no_schema, ORACLE_DESCRIPTOR);
    expect_io("schema missing", 1u, 2u, 1u);
    expect_receipt("schema missing line", "schema",
                   (unsigned)(sizeof k_no_schema - 1u), 1u, 0u, 0u, 0u, 60u,
                   "default", "rb/back", 1u, 0u);
    expect_duty("schema missing", 1u);

    load_text(k_bad_schema, ORACLE_DESCRIPTOR);
    expect_io("bad schema", 2u, 2u, 1u);
    expect_receipt("bad schema line", "schema",
                   (unsigned)(sizeof k_bad_schema - 1u), 2u, 1u, 0u, 0u, 60u,
                   "default", "rb/back", 1u, 0u);
    expect_rejected("bad schema rejected line", "lines=1,0,0,0 unknown=,,,");
    expect_duty("bad schema", 1u);

    load_bytes(k_garbage, sizeof k_garbage - 1u, ORACLE_DESCRIPTOR);
    expect_io("garbage", 1u, 2u, 1u);
    expect_receipt("garbage line", "garbage",
                   (unsigned)(sizeof k_garbage - 1u), 0u, 0u, 0u, 0u, 60u,
                   "default", "rb/back", 1u, 0u);
    expect_duty("garbage", 1u);
}

static void test_sizes_and_reads(void)
{
    static char file[FILE_CAPACITY];
    char first_line[LOG_BYTES];
    size_t length;

    /* Exactly the cap: accepted, one data read plus the EOF read. */
    length = fill_padded(file, 4096u, "schema=1\nfps_lock=45\n");
    set_reads(0u, -1, 0);
    load_bytes(file, length, ORACLE_DESCRIPTOR);
    expect_io("4096 bytes", 1u, 2u, 1u);
    expect_receipt_from_parser("4096 bytes line", file, length, 2u);
    check(strstr(s_lines[0], " status=ok bytes=4096 ") != NULL,
          "4096 bytes: status ok, bytes 4096");
    check(strstr(s_lines[0], " fps_lock=45") != NULL,
          "4096 bytes: fps_lock applied");
    memcpy(first_line, s_lines[0], sizeof first_line);

    /* Same file answered 1000 bytes at a time: 5 data reads + EOF, same
     * receipt byte for byte. */
    set_reads(1000u, -1, 0);
    load_bytes(file, length, ORACLE_DESCRIPTOR);
    expect_io("4096 bytes chunked", 1u, 6u, 1u);
    check_str(s_lines[0], first_line, "4096 bytes chunked: identical receipt");
    expect_duty("4096 bytes chunked", 2u);

    /* One byte over: the loader stops at 4097 (no EOF read), the parser
     * reports too-large, defaults are pushed. */
    length = fill_padded(file, 4097u, "schema=1\nfps_lock=45\n");
    set_reads(0u, -1, 0);
    load_bytes(file, length, ORACLE_DESCRIPTOR);
    expect_io("4097 bytes", 1u, 1u, 1u);
    expect_receipt("4097 bytes line", "too-large", 4097u, 0u, 0u, 0u, 0u, 60u,
                   "default", "rb/back", 1u, 0u);
    expect_duty("4097 bytes", 1u);

    /* Far over the cap, chunked: still exactly 4097 bytes requested. */
    length = fill_padded(file, 6000u, "schema=1\nfps_lock=45\n");
    set_reads(1000u, -1, 0);
    load_bytes(file, length, ORACLE_DESCRIPTOR);
    expect_io("6000 bytes chunked", 1u, 5u, 1u);
    expect_receipt("6000 bytes chunked line", "too-large", 4097u, 0u, 0u, 0u,
                   0u, 60u, "default", "rb/back", 1u, 0u);
    expect_duty("6000 bytes chunked", 1u);

    /* Read failure after one 10-byte chunk: io-error with the code, the
     * partial byte count, nothing parsed, descriptor still closed. */
    set_reads(10u, 1, ORACLE_EIO);
    load_text("schema=1\nfps_lock=45\n", ORACLE_DESCRIPTOR);
    expect_io("read error", 1u, 2u, 1u);
    expect_receipt("read error line", "io-error(0x80010005)", 10u, 0u, 0u, 0u,
                   0u, 60u, "default", "rb/back", 1u, 0u);
    expect_duty("read error", 1u);

    /* Read failure on the very first read: bytes=0. */
    set_reads(0u, 0, ORACLE_EIO);
    load_text("schema=1\nfps_lock=45\n", ORACLE_DESCRIPTOR);
    expect_io("first read error", 1u, 1u, 1u);
    expect_receipt("first read error line", "io-error(0x80010005)", 0u, 0u,
                   0u, 0u, 0u, 60u, "default", "rb/back", 1u, 0u);
    expect_duty("first read error", 1u);

    /* A small file after the large ones: no stale bytes leak. */
    set_reads(0u, -1, 0);
    load_text("schema=1\nfps_lock=40\n", ORACLE_DESCRIPTOR);
    expect_io("small after large", 1u, 2u, 1u);
    expect_receipt("small after large line", "ok", 21u, 2u, 0u, 0u, 0u, 40u,
                   "default", "rb/back", 1u, 0u);
    expect_duty("small after large", 3u);
}

int main(void)
{
    test_absent_and_open_error();
    test_ok_files();
    test_remap_fields();
    test_raster();
    test_ignored_files();
    test_sizes_and_reads();
    if (s_failures != 0u) {
        fprintf(stderr,
                "host_vita_runtime_settings_oracle: %u of %u checks failed\n",
                s_failures, s_checks);
        return 1;
    }
    printf("host_vita_runtime_settings_oracle: PASS (%u checks; variant=%s; "
           "absent/io-error/ok/schema/garbage/too-large receipts, chunked "
           "and failing reads, exact 4097-byte requests, guard, custom "
           "remap/chords, build-raster default%s, REJECTED line, duty "
           "pushes)\n",
           s_checks, VARIANT, RASTER_DEFAULT);
    return 0;
}
