/* Host oracle for the debug_overlay key (ISAAC_VITA_DEBUG_OVERLAY) of the
 * shared port.ini parser and of the boot-time loader.
 *
 * Compiled with -DISAAC_VITA_DEBUG_OVERLAY=1 together with
 * host_vita_settings_parse.c and host_vita_runtime_settings.c (oracle I/O
 * seams).  host_vita_settings_oracle.c keeps covering the parser without the
 * macro, where the key does not exist.  Checks: default off, the three
 * accepted values, rejected spellings, key table entry and assigned bit,
 * get/set_text, canonical text ends with "debug_overlay=<v>\n", round trip,
 * SERIALIZED_MAX (365) is the worst case exactly, equal() sees the field,
 * the loader appends " debug_overlay=<v>" to the first receipt line and
 * pushes the value to isaac_vita_debug_overlay_set_mode(). */
#include "host_vita_runtime_settings.h"
#include "host_vita_settings_parse.h"
#include "host_vita_debug_overlay.h"
#include "platform.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(ISAAC_VITA_DEBUG_OVERLAY)
#error "build this oracle with -DISAAC_VITA_DEBUG_OVERLAY=1"
#endif

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

/* ---- loader seams ------------------------------------------------------ */

static char s_file[8192];
static size_t s_file_length;
static size_t s_read_position;
static int s_open_result;
static char s_log[4][512];
static unsigned s_log_count;
static int s_pushed = -1;
static unsigned s_push_calls;

int isaac_vita_runtime_settings_oracle_open(const char *path)
{
    (void)path;
    s_read_position = 0u;
    return s_open_result;
}

int isaac_vita_runtime_settings_oracle_read(int descriptor, void *buffer,
                                            uint32_t size)
{
    size_t left = s_file_length - s_read_position;
    size_t n = left < size ? left : size;

    (void)descriptor;
    memcpy(buffer, s_file + s_read_position, n);
    s_read_position += n;
    return (int)n;
}

int isaac_vita_runtime_settings_oracle_close(int descriptor)
{
    (void)descriptor;
    return 0;
}

void isaac_vita_log(const char *format, ...)
{
    va_list args;

    if (s_log_count >= 4u)
        return;
    va_start(args, format);
    vsnprintf(s_log[s_log_count++], sizeof s_log[0], format, args);
    va_end(args);
}

void isaac_vita_debug_overlay_set_mode(uint8_t mode)
{
    s_pushed = mode;
    ++s_push_calls;
}

const char *isaac_vita_debug_overlay_mode_name(uint8_t mode)
{
    switch (mode) {
    case 1u: return "compact";
    case 2u: return "full";
    default: return "off";
    }
}

/* ---- helpers ----------------------------------------------------------- */

static enum isaac_vita_settings_status parse_text(
    const char *text, struct isaac_vita_settings *out,
    struct isaac_vita_settings_receipt *receipt)
{
    return isaac_vita_settings_parse(text, strlen(text), out, receipt);
}

static const char *const k_defaults_text =
    "schema=1\n"
    "fps_lock=60\n"
    "display_raster=960\n"
    "render_scale=1\n"
    "button.cross=a\n"
    "button.circle=b\n"
    "button.square=x\n"
    "button.triangle=y\n"
    "button.l=lb\n"
    "button.r=lt\n"
    "button.select=rt\n"
    "chord.start_l=rb\n"
    "chord.start_r=back\n"
    "chord.restart_hold=on\n"
    "touch.top_left=lt\n"
    "touch.top_right=back\n"
    "touch.bottom_right=rb\n"
    "stick.swap=off\n"
    "debug_overlay=off\n";

static void test_parser(void)
{
    struct isaac_vita_settings s;
    struct isaac_vita_settings d;
    struct isaac_vita_settings_receipt r;
    enum isaac_vita_settings_key key;
    char text[512];
    static const char *const rejected[] = {
        "on", "1", "2", "FULL", "Compact", "compact2", "half", "none",
    };
    unsigned i;

    check_u((unsigned long)ISAAC_VITA_SETTINGS_KEY_COUNT, 20u,
            "one more key than the 19 of the base table");
    check_u((unsigned long)ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY, 19u,
            "debug_overlay is the last key (serialization order)");
    check_str(isaac_vita_settings_key_name(ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY),
              "debug_overlay", "key name");
    check(isaac_vita_settings_key_kind(ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY) ==
          ISAAC_VITA_SETTINGS_KIND_DEBUG_OVERLAY, "key kind");
    check(isaac_vita_settings_key_from_text("debug_overlay", 13u, &key) == 0 &&
          key == ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY, "key from text");
    check(!isaac_vita_settings_key_accepts_element(
              ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY,
              ISAAC_VITA_SETTINGS_ELEMENT_A), "takes no element");

    isaac_vita_settings_defaults(&d);
    check_u(d.debug_overlay, ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_OFF,
            "default off");
    check_u(parse_text("", &s, &r), ISAAC_VITA_SETTINGS_STATUS_SCHEMA,
            "empty file");
    check(isaac_vita_settings_equal(&s, &d), "empty file == defaults");

    check_u(parse_text("schema=1\ndebug_overlay=compact\n", &s, &r),
            ISAAC_VITA_SETTINGS_STATUS_OK, "compact status");
    check_u(s.debug_overlay, ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_COMPACT,
            "compact value");
    check(r.assigned == (ISAAC_VITA_SETTINGS_ASSIGNED_BIT(
                             ISAAC_VITA_SETTINGS_KEY_SCHEMA) |
                         ISAAC_VITA_SETTINGS_ASSIGNED_BIT(
                             ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY)),
          "assigned bit 19");
    check_u(r.rejected + r.unknown, 0u, "nothing rejected");
    check_u(parse_text("schema=1\r\n  debug_overlay=full  \r\n", &s, &r),
            ISAAC_VITA_SETTINGS_STATUS_OK, "full status (CRLF, blanks)");
    check_u(s.debug_overlay, ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_FULL,
            "full value");
    check_u(parse_text("schema=1\ndebug_overlay=full\ndebug_overlay=off\n",
                       &s, &r), ISAAC_VITA_SETTINGS_STATUS_OK, "dup status");
    check_u(s.debug_overlay, ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_OFF,
            "last wins");
    check_u(r.dup, 1u, "dup counted");
    for (i = 0u; i < sizeof rejected / sizeof rejected[0]; ++i) {
        char file[64];

        snprintf(file, sizeof file, "schema=1\ndebug_overlay=full\n"
                 "debug_overlay=%s\n", rejected[i]);
        check_u(parse_text(file, &s, &r), ISAAC_VITA_SETTINGS_STATUS_OK,
                "rejected value keeps the file");
        check_u(s.debug_overlay, ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_FULL,
                "rejected value keeps the earlier one");
        check_u(r.rejected, 1u, "rejected value counted");
        check_u(r.rejected_lines[0], 3u, "rejected line number");
    }
    check_u(parse_text("debug_overlay=full\n", &s, &r),
            ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "schema still mandatory");
    check_u(s.debug_overlay, ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_OFF,
            "ignored file leaves the default");

    /* get/set_text */
    isaac_vita_settings_defaults(&s);
    check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY,
                                       "full", 4u) == 0, "set full");
    check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY,
                                         text, sizeof text), 4u, "get full");
    check_str(text, "full", "get full text");
    check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY,
                                       "on", 2u) != 0 &&
          s.debug_overlay == ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_FULL,
          "set refuses on, struct untouched");
    check(!isaac_vita_settings_equal(&s, &d), "equal sees debug_overlay");
    s.debug_overlay = 3u;
    check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY,
                                         text, sizeof text), 0u,
            "out-of-set value has no text");
    check_u(isaac_vita_settings_serialize(&s, text, sizeof text), 0u,
            "out-of-set value is not serializable");

    /* canonical text and round trip */
    check_u(isaac_vita_settings_serialize(&d, text, sizeof text),
            strlen(k_defaults_text), "defaults length");
    check_str(text, k_defaults_text, "defaults text ends with debug_overlay=off");
    {
        struct isaac_vita_settings worst;
        struct isaac_vita_settings back;
        size_t n;

        isaac_vita_settings_defaults(&worst);
        memcpy(worst.preset, "abcdefghijklmno", 16u);
        memset(worst.button, ISAAC_VITA_SETTINGS_ELEMENT_NONE, sizeof worst.button);
        worst.chord_start_l = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
        worst.chord_start_r = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
        worst.chord_restart_hold = 0u;
        memset(worst.touch, ISAAC_VITA_SETTINGS_ELEMENT_NONE, sizeof worst.touch);
        worst.stick_swap = 0u;
        worst.debug_overlay = ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_COMPACT;
        n = isaac_vita_settings_serialize(&worst, text, sizeof text);
        check_u(n, ISAAC_VITA_SETTINGS_SERIALIZED_MAX, "worst case == MAX");
        check_u(ISAAC_VITA_SETTINGS_SERIALIZED_MAX, 365u, "MAX pinned at 365");
        check(n >= 22u && strcmp(text + n - 22u, "debug_overlay=compact\n") == 0,
              "worst case ends with the compact line");
        check_u(isaac_vita_settings_serialize(&worst, text,
                                              ISAAC_VITA_SETTINGS_SERIALIZED_MAX),
                0u, "worst case needs MAX + 1");
        n = isaac_vita_settings_serialize(&worst, text, sizeof text);
        check_u(isaac_vita_settings_parse(text, n, &back, &r),
                ISAAC_VITA_SETTINGS_STATUS_OK, "worst-case round trip status");
        check(isaac_vita_settings_equal(&back, &worst), "worst-case round trip");
    }
}

static void load(const char *file)
{
    s_file_length = strlen(file);
    memcpy(s_file, file, s_file_length);
    s_open_result = 0x40010203;
    s_log_count = 0u;
    s_pushed = -1;
    s_push_calls = 0u;
    isaac_vita_runtime_settings_load();
}

static int ends_with(const char *text, const char *tail)
{
    size_t a = strlen(text);
    size_t b = strlen(tail);

    return a >= b && strcmp(text + a - b, tail) == 0;
}

static void test_loader(void)
{
    load("schema=1\ndebug_overlay=full\n");
    check_u(s_log_count, 1u, "one receipt line");
    check(ends_with(s_log[0], " stick_swap=0 debug_overlay=full"),
          "receipt ends with debug_overlay=full");
    check(s_push_calls == 1u && s_pushed == 2, "mode full pushed once");
    printf("receipt: %s\n", s_log[0]);

    load("schema=1\ndebug_overlay=compact\n");
    check(ends_with(s_log[0], " debug_overlay=compact") && s_pushed == 1,
          "compact receipt + push");

    s_open_result = (int)0x80010002u;
    s_file_length = 0u;
    s_log_count = 0u;
    s_pushed = -1;
    isaac_vita_runtime_settings_load();
    check(strstr(s_log[0], " status=absent ") != NULL &&
          ends_with(s_log[0], " debug_overlay=off") && s_pushed == 0,
          "absent file: off reported and pushed");

    load("schema=1\ndebug_overlay=loud\n");
    check(ends_with(s_log[0], " debug_overlay=off") && s_pushed == 0 &&
          s_log_count == 2u, "rejected value: off + REJECTED line");
}

int main(void)
{
    test_parser();
    test_loader();
    if (s_failures) {
        fprintf(stderr, "host_vita_settings_debug_overlay_oracle: FAIL "
                "(%u of %u checks)\n", s_failures, s_checks);
        return 1;
    }
    printf("host_vita_settings_debug_overlay_oracle: PASS (%u checks; key "
           "table, values, rejects, get/set, canonical text, MAX=365, loader "
           "receipt + push)\n", s_checks);
    return 0;
}
