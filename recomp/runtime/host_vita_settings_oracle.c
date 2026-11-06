/* Host oracle for host_vita_settings_parse.c (the shared port.ini TU).
 *
 * Grammar corpus of the port.ini design, sections 1.2/1.3:
 * empty file (status schema, struct == defaults), comments only, CRLF, no
 * trailing LF, exactly 4096 bytes accepted vs 4097 too-large, NUL -> garbage,
 * 0x80 -> that line rejected only, duplicates (last wins, counted), unknown
 * keys (counted, first four named, 15-char truncation), bad schema value,
 * schema missing, every allowed value of every key accepted, every forbidden
 * value rejected (rt on chord/touch keys, start / dpad_* anywhere, key
 * longer than 31, value longer than 15, uppercase, blanks around '='),
 * guard failures with reset_remap, parse(serialize(s)) == s for the defaults
 * and a fully customised struct, serialization golden bytes and overflow,
 * cross-kind values (a value legal for another key is rejected), the
 * assigned-key mask (absent display_raster -> the loader keeps the build
 * raster), preset labels that are unterminated or outside the grammar are
 * not serializable, the SERIALIZED_MAX pin, receipt struct goldens (the
 * section 1.5 text formatter needs stdio and lives in
 * host_vita_runtime_settings.c, not in the parser TU; the oracle checks the
 * receipt fields it is formatted from).  Buffers handed to the parser are
 * never NUL-terminated: every corpus entry is copied into a scratch buffer
 * whose tail is filled with 'x' so an over-read shows up. */
#include "host_vita_settings_parse.h"

#include <stdio.h>
#include <string.h>

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
        fprintf(stderr, "FAIL: %s: got \"%s\" expected \"%s\"\n", what, got,
                expected);
    }
}

/* Scratch copy with a poisoned tail; the parser sees exactly len bytes. */
#define SCRATCH_BYTES 8192u
static char s_scratch[SCRATCH_BYTES];

static enum isaac_vita_settings_status parse_bytes(
    const char *text, size_t len, struct isaac_vita_settings *out,
    struct isaac_vita_settings_receipt *receipt)
{
    if (len > SCRATCH_BYTES) {
        fprintf(stderr, "corpus entry too long\n");
        return ISAAC_VITA_SETTINGS_STATUS_IO_ERROR;
    }
    memset(s_scratch, 'x', SCRATCH_BYTES);
    memcpy(s_scratch, text, len);
    return isaac_vita_settings_parse(s_scratch, len, out, receipt);
}

static enum isaac_vita_settings_status parse_text(
    const char *text, struct isaac_vita_settings *out,
    struct isaac_vita_settings_receipt *receipt)
{
    return parse_bytes(text, strlen(text), out, receipt);
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
    "stick.swap=off\n";

static void test_defaults_and_empty(void)
{
    struct isaac_vita_settings defaults;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;

    isaac_vita_settings_defaults(&defaults);
    check_u(defaults.fps_lock, 60u, "default fps_lock");
    check_u(defaults.display_raster, 960u, "default display_raster");
    check_u(defaults.render_scale, 1u, "default render_scale");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_CROSS],
            ISAAC_VITA_SETTINGS_ELEMENT_A, "default cross");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_CIRCLE],
            ISAAC_VITA_SETTINGS_ELEMENT_B, "default circle");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_SQUARE],
            ISAAC_VITA_SETTINGS_ELEMENT_X, "default square");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_TRIANGLE],
            ISAAC_VITA_SETTINGS_ELEMENT_Y, "default triangle");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_L],
            ISAAC_VITA_SETTINGS_ELEMENT_LB, "default l");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_R],
            ISAAC_VITA_SETTINGS_ELEMENT_LT, "default r");
    check_u(defaults.button[ISAAC_VITA_SETTINGS_BUTTON_SELECT],
            ISAAC_VITA_SETTINGS_ELEMENT_RT, "default select");
    check_u(defaults.chord_start_l, ISAAC_VITA_SETTINGS_ELEMENT_RB,
            "default chord.start_l");
    check_u(defaults.chord_start_r, ISAAC_VITA_SETTINGS_ELEMENT_BACK,
            "default chord.start_r");
    check_u(defaults.chord_restart_hold, 1u, "default chord.restart_hold");
    check_u(defaults.touch[ISAAC_VITA_SETTINGS_TOUCH_TOP_LEFT],
            ISAAC_VITA_SETTINGS_ELEMENT_LT, "default touch.top_left");
    check_u(defaults.touch[ISAAC_VITA_SETTINGS_TOUCH_TOP_RIGHT],
            ISAAC_VITA_SETTINGS_ELEMENT_BACK, "default touch.top_right");
    check_u(defaults.touch[ISAAC_VITA_SETTINGS_TOUCH_BOTTOM_RIGHT],
            ISAAC_VITA_SETTINGS_ELEMENT_RB, "default touch.bottom_right");
    check_u(defaults.stick_swap, 0u, "default stick.swap");
    check_str(defaults.preset, "", "default preset absent");
    check(isaac_vita_settings_validate_remap(&defaults),
          "defaults satisfy the a/b guard");

    /* parse("") == defaults; status schema (no schema=1 line). */
    memset(&parsed, 0xA5, sizeof parsed);
    status = parse_bytes("", 0u, &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "empty -> schema");
    check(isaac_vita_settings_equal(&parsed, &defaults),
          "parse(\"\") == defaults");
    check(memcmp(&parsed, &defaults, sizeof parsed) == 0,
          "parse(\"\") byte-identical to defaults (padding zeroed)");
    check_u(receipt.status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA,
            "empty receipt status");
    check_u(receipt.bytes, 0u, "empty receipt bytes");
    check_u(receipt.lines, 0u, "empty receipt lines");
    check_u(receipt.rejected, 0u, "empty receipt rejected");
    check_u(receipt.unknown, 0u, "empty receipt unknown");
    check_u(receipt.dup, 0u, "empty receipt dup");
    check_u(receipt.assigned, 0u, "empty receipt assigned");
    check_u(receipt.remap_guard, 0u, "empty receipt guard");

    /* NULL receipt is allowed. */
    status = parse_text("schema=1\nfps_lock=45\n", &parsed, NULL);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "NULL receipt parse ok");
    check_u(parsed.fps_lock, 45u, "NULL receipt value applied");

    /* Duty mapping. */
    check_u(isaac_vita_settings_fps_lock_duty_k(60u), 1u, "duty 60 -> 1");
    check_u(isaac_vita_settings_fps_lock_duty_k(45u), 2u, "duty 45 -> 2");
    check_u(isaac_vita_settings_fps_lock_duty_k(40u), 3u, "duty 40 -> 3");
    check_u(isaac_vita_settings_fps_lock_duty_k(30u), 0u, "duty 30 -> 0");
    check_u(isaac_vita_settings_fps_lock_duty_k(0u), 1u, "duty other -> 1");
    check_u(isaac_vita_settings_fps_lock_duty_k(59u), 1u, "duty 59 -> 1");
}

static void test_comments_and_layout(void)
{
    struct isaac_vita_settings defaults;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;

    isaac_vita_settings_defaults(&defaults);

    /* Comments only: no schema line -> schema, nothing rejected. */
    status = parse_text("# port.ini\n; another comment\n\n[section]\n",
                        &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "comments only -> schema");
    check(isaac_vita_settings_equal(&parsed, &defaults),
          "comments only leaves defaults");
    check_u(receipt.lines, 4u, "comments only lines");
    check_u(receipt.rejected, 0u, "comments only rejected");

    /* Comments plus schema -> ok. */
    status = parse_text("# c\r\n;c\r\n[port]\r\nschema=1\r\n", &parsed,
                        &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "comments + schema ok");
    check_u(receipt.lines, 4u, "comments + schema lines");
    check_u(receipt.rejected, 0u, "comments + schema rejected");

    /* CRLF file with values. */
    status = parse_text("schema=1\r\nfps_lock=40\r\nstick.swap=on\r\n",
                        &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "CRLF ok");
    check_u(parsed.fps_lock, 40u, "CRLF fps_lock");
    check_u(parsed.stick_swap, 1u, "CRLF stick.swap");
    check_u(receipt.lines, 3u, "CRLF lines");
    check_u(receipt.rejected, 0u, "CRLF rejected");

    /* No trailing LF. */
    status = parse_text("schema=1\nfps_lock=30", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "no trailing LF ok");
    check_u(parsed.fps_lock, 30u, "no trailing LF value");
    check_u(receipt.lines, 2u, "no trailing LF lines");

    /* No trailing LF, CR-terminated last line. */
    status = parse_text("schema=1\r\nfps_lock=45\r", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "trailing CR ok");
    check_u(parsed.fps_lock, 45u, "trailing CR value");

    /* Blanks trimmed around the line but not around '='. */
    status = parse_text("  schema=1  \n\t fps_lock=45\t\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "trimmed ok");
    check_u(parsed.fps_lock, 45u, "trimmed value");
    check_u(receipt.rejected, 0u, "trimmed rejected");
    status = parse_text("schema=1\nfps_lock = 45\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "blanks around = still ok");
    check_u(parsed.fps_lock, 60u, "blanks around = keep default");
    check_u(receipt.rejected, 1u, "blanks around = rejected");
    check_u(receipt.rejected_lines[0], 2u, "blanks around = line number");

    /* Schema anywhere in the file. */
    status = parse_text("fps_lock=45\nschema=1\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "late schema ok");
    check_u(parsed.fps_lock, 45u, "late schema value");

    /* Malformed section header is rejected, well-formed one ignored. */
    status = parse_text("schema=1\n[port\n[ok]\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "section ok");
    check_u(receipt.rejected, 1u, "open section rejected");
    check_u(receipt.rejected_lines[0], 2u, "open section line");

    /* Lines without '=', empty key, empty value. */
    status = parse_text("schema=1\nfps_lock\n=45\nfps_lock=\n", &parsed,
                        &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "malformed lines ok");
    check_u(receipt.rejected, 3u, "malformed lines rejected");
    check_u(receipt.rejected_lines[0], 2u, "malformed line 2");
    check_u(receipt.rejected_lines[1], 3u, "malformed line 3");
    check_u(receipt.rejected_lines[2], 4u, "malformed line 4");
    check_u(receipt.rejected_lines[3], 0u, "malformed slot 4 empty");
    check_u(parsed.fps_lock, 60u, "malformed lines keep default");

    /* Blank-only and CR-only lines are ignored. */
    status = parse_text("\n \n\t\n\r\nschema=1\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "blank lines ok");
    check_u(receipt.lines, 5u, "blank lines counted");
    check_u(receipt.rejected, 0u, "blank lines not rejected");
}

static void test_size_and_bytes(void)
{
    static char big[ISAAC_VITA_SETTINGS_MAX_BYTES + 1u];
    struct isaac_vita_settings defaults;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;
    const char head[] = "schema=1\nfps_lock=45\n";
    size_t head_len = sizeof head - 1u;
    size_t i;

    isaac_vita_settings_defaults(&defaults);

    /* Exactly 4096 bytes: head + '#' comment padding, one '#' per line
     * position so no line exceeds sanity; the last byte is a LF. */
    memcpy(big, head, head_len);
    for (i = head_len; i < ISAAC_VITA_SETTINGS_MAX_BYTES; ++i) {
        big[i] = ((i - head_len) % 64u == 63u) ? '\n' : '#';
    }
    big[ISAAC_VITA_SETTINGS_MAX_BYTES - 1u] = '\n';
    status = parse_bytes(big, ISAAC_VITA_SETTINGS_MAX_BYTES, &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "4096 bytes accepted");
    check_u(parsed.fps_lock, 45u, "4096 bytes value");
    check_u(receipt.bytes, ISAAC_VITA_SETTINGS_MAX_BYTES, "4096 receipt bytes");
    check_u(receipt.rejected, 0u, "4096 rejected");

    /* 4097 bytes: too-large, struct stays defaults, counters zero. */
    big[ISAAC_VITA_SETTINGS_MAX_BYTES] = '\n';
    status = parse_bytes(big, ISAAC_VITA_SETTINGS_MAX_BYTES + 1u, &parsed,
                         &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE, "4097 too-large");
    check(isaac_vita_settings_equal(&parsed, &defaults),
          "too-large leaves defaults");
    check_u(receipt.status, ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE,
            "too-large receipt status");
    check_u(receipt.bytes, ISAAC_VITA_SETTINGS_MAX_BYTES + 1u,
            "too-large receipt bytes");
    check_u(receipt.lines, 0u, "too-large lines");
    check_u(receipt.rejected, 0u, "too-large rejected");

    /* NUL byte anywhere -> garbage, even with a valid schema line. */
    {
        const char nul[] = "schema=1\nfps_lock=45\n\0#c\n";
        status = parse_bytes(nul, sizeof nul - 1u, &parsed, &receipt);
        check_u(status, ISAAC_VITA_SETTINGS_STATUS_GARBAGE, "NUL -> garbage");
        check(isaac_vita_settings_equal(&parsed, &defaults),
              "garbage leaves defaults");
        check_u(receipt.bytes, sizeof nul - 1u, "garbage bytes");
        check_u(receipt.lines, 0u, "garbage lines");
    }
    {
        const char nul_first[] = "\0";
        status = parse_bytes(nul_first, 1u, &parsed, &receipt);
        check_u(status, ISAAC_VITA_SETTINGS_STATUS_GARBAGE,
                "single NUL -> garbage");
    }

    /* 0x80 byte rejects that line only; the rest applies. */
    {
        const char high[] = "schema=1\nfps_lock=\x80" "45\nstick.swap=on\n";
        status = parse_bytes(high, sizeof high - 1u, &parsed, &receipt);
        check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "0x80 file ok");
        check_u(parsed.fps_lock, 60u, "0x80 line keeps default");
        check_u(parsed.stick_swap, 1u, "0x80 rest applies");
        check_u(receipt.rejected, 1u, "0x80 rejected count");
        check_u(receipt.rejected_lines[0], 2u, "0x80 rejected line");
        check_u(receipt.lines, 3u, "0x80 lines");
    }
    /* 0xFF, 0x7F and a control byte are rejected too; a comment line with a
     * high byte is still rejected (byte set applies before the comment test). */
    {
        const char bytes[] = "schema=1\n# caf\xC3\xA9\nfps_lock=4\x7F\n"
                             "fps_lock=\x01" "40\nfps_lock=\xFF\n";
        status = parse_bytes(bytes, sizeof bytes - 1u, &parsed, &receipt);
        check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "bytes file ok");
        check_u(receipt.rejected, 4u, "bytes rejected");
        check_u(receipt.rejected_lines[0], 2u, "bytes line 2");
        check_u(receipt.rejected_lines[1], 3u, "bytes line 3");
        check_u(receipt.rejected_lines[2], 4u, "bytes line 4");
        check_u(receipt.rejected_lines[3], 5u, "bytes line 5");
    }
    /* Non-terminated buffer: the byte after len is a valid-looking
     * continuation and must not be read. */
    {
        const char cut[] = "schema=1\nfps_lock=4";
        status = parse_bytes(cut, sizeof cut - 1u, &parsed, &receipt);
        check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "cut file ok");
        check_u(parsed.fps_lock, 60u, "cut value rejected");
        check_u(receipt.rejected, 1u, "cut rejected");
    }
}

static void test_duplicates_unknown_schema(void)
{
    struct isaac_vita_settings defaults;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;

    isaac_vita_settings_defaults(&defaults);

    /* Duplicates: last wins, each repeat counted. */
    status = parse_text("schema=1\nfps_lock=45\nfps_lock=40\nfps_lock=30\n"
                        "stick.swap=on\nstick.swap=off\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "dup ok");
    check_u(parsed.fps_lock, 30u, "dup last wins");
    check_u(parsed.stick_swap, 0u, "dup last wins onoff");
    check_u(receipt.dup, 3u, "dup count");
    check_u(receipt.rejected, 0u, "dup rejected");

    /* Documented rule (header): a rejected line never changes state.  A
     * rejected repeat does not count as a duplicate and does not undo the
     * earlier valid value. */
    status = parse_text("schema=1\nfps_lock=45\nfps_lock=50\n", &parsed,
                        &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "dup+bad ok");
    check_u(parsed.fps_lock, 45u, "dup+bad keeps earlier valid");
    check_u(receipt.dup, 0u, "dup+bad dup count");
    check_u(receipt.rejected, 1u, "dup+bad rejected");

    /* schema repeated: counted as dup, still ok. */
    status = parse_text("schema=1\nschema=1\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "schema dup ok");
    check_u(receipt.dup, 1u, "schema dup counted");
    /* schema=1 then schema=2: the bad line is rejected, the accepted schema
     * stands (same rule as any other key). */
    status = parse_text("schema=1\nschema=2\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "schema then bad schema ok");
    check_u(receipt.dup, 0u, "schema then bad schema dup");
    check_u(receipt.rejected, 1u, "schema then bad schema rejected");
    check_u(receipt.rejected_lines[0], 2u, "schema then bad schema line");

    /* Unknown keys: counted, first four named, truncated to 15 chars. */
    status = parse_text("schema=1\n"
                        "foo=1\n"
                        "button.dpad_up=a\n"
                        "abcdefghijklmnopqrstuvwxyz=1\n"
                        "a.b_c9=x\n"
                        "fifth=5\n"
                        "fps_lock=40\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "unknown ok");
    check_u(receipt.unknown, 5u, "unknown count");
    check_str(receipt.unknown_keys[0], "foo", "unknown 0");
    check_str(receipt.unknown_keys[1], "button.dpad_up", "unknown 1");
    check_str(receipt.unknown_keys[2], "abcdefghijklmno", "unknown 2 truncated");
    check_str(receipt.unknown_keys[3], "a.b_c9", "unknown 3");
    check_u(receipt.rejected, 0u, "unknown not rejected");
    check_u(parsed.fps_lock, 40u, "unknown rest applies");
    check_u(receipt.lines, 7u, "unknown lines");

    /* Unknown key with an invalid value is a rejected line, not unknown. */
    status = parse_text("schema=1\nfoo=BAD\n", &parsed, &receipt);
    check_u(receipt.unknown, 0u, "unknown bad value not counted unknown");
    check_u(receipt.rejected, 1u, "unknown bad value rejected");

    /* Bad schema value: line rejected, status schema, struct defaults even
     * though other lines were valid. */
    status = parse_text("schema=2\nfps_lock=45\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "schema=2 -> schema");
    check(isaac_vita_settings_equal(&parsed, &defaults),
          "schema=2 leaves defaults");
    check_u(receipt.rejected, 1u, "schema=2 rejected");
    check_u(receipt.rejected_lines[0], 1u, "schema=2 line");
    check_u(receipt.lines, 2u, "schema=2 lines counted");
    status = parse_text("schema=01\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "schema=01 -> schema");
    status = parse_text("schema=1x\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "schema=1x -> schema");
    status = parse_text("SCHEMA=1\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "SCHEMA=1 -> schema");
    check_u(receipt.rejected, 1u, "SCHEMA=1 rejected (uppercase key)");

    /* Schema missing with otherwise valid content. */
    status = parse_text("fps_lock=45\nstick.swap=on\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "missing schema");
    check(isaac_vita_settings_equal(&parsed, &defaults),
          "missing schema leaves defaults");
    check_u(receipt.lines, 2u, "missing schema lines");
    check_u(receipt.rejected, 0u, "missing schema rejected");
    check_u(receipt.remap_guard, 0u, "missing schema no guard flag");

    /* Rejected line slots keep only the first four. */
    status = parse_text("schema=1\n!\n!\n!\n!\n!\n!\n", &parsed, &receipt);
    check_u(receipt.rejected, 6u, "six rejected");
    check_u(receipt.rejected_lines[0], 2u, "slot 0");
    check_u(receipt.rejected_lines[1], 3u, "slot 1");
    check_u(receipt.rejected_lines[2], 4u, "slot 2");
    check_u(receipt.rejected_lines[3], 5u, "slot 3");
}

struct allowed_row {
    enum isaac_vita_settings_key key;
    const char *const *values;
    unsigned count;
};

static const char *const k_fps[] = { "60", "45", "40", "30" };
static const char *const k_raster[] = { "960", "720", "480" };
static const char *const k_scale[] = { "1", "2" };
static const char *const k_phys[] = {
    "none", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "back"
};
static const char *const k_synth[] = {
    "none", "a", "b", "x", "y", "lb", "rb", "lt", "back"
};
static const char *const k_onoff[] = { "on", "off" };
static const char *const k_labels[] = {
    "default", "speed-run", "a", "abcdefghijklmno", "0_9-z"
};

static const struct allowed_row k_allowed[] = {
    { ISAAC_VITA_SETTINGS_KEY_PRESET, k_labels, 5u },
    { ISAAC_VITA_SETTINGS_KEY_FPS_LOCK, k_fps, 4u },
    { ISAAC_VITA_SETTINGS_KEY_DISPLAY_RASTER, k_raster, 3u },
    { ISAAC_VITA_SETTINGS_KEY_RENDER_SCALE, k_scale, 2u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_CIRCLE, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_SQUARE, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_TRIANGLE, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_L, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_R, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_BUTTON_SELECT, k_phys, 10u },
    { ISAAC_VITA_SETTINGS_KEY_CHORD_START_L, k_synth, 9u },
    { ISAAC_VITA_SETTINGS_KEY_CHORD_START_R, k_synth, 9u },
    { ISAAC_VITA_SETTINGS_KEY_CHORD_RESTART_HOLD, k_onoff, 2u },
    { ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_LEFT, k_synth, 9u },
    { ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_RIGHT, k_synth, 9u },
    { ISAAC_VITA_SETTINGS_KEY_TOUCH_BOTTOM_RIGHT, k_synth, 9u },
    { ISAAC_VITA_SETTINGS_KEY_STICK_SWAP, k_onoff, 2u },
};

static const char *const k_forbidden_everywhere[] = {
    "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "A", "Lb", "BACK", "ON", "abcdefghijklmnop", "", "a b", "a=b", "60.0",
    "0x3c", "yes", "no", "3", "01", "99", "true", "false", "-", "_"
};

static int row_allows(const struct allowed_row *r, const char *value)
{
    unsigned v;
    for (v = 0u; v < r->count; ++v) {
        if (strcmp(r->values[v], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_every_value(void)
{
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;
    char file[128];
    char got[32];
    unsigned row;
    unsigned other;
    unsigned v;
    unsigned key_index;

    /* The allowed table covers every key but SCHEMA exactly once, so a key
     * added to the parser without an oracle row fails here. */
    check_u(sizeof k_allowed / sizeof k_allowed[0],
            (unsigned long)ISAAC_VITA_SETTINGS_KEY_COUNT - 1u,
            "k_allowed has one row per non-schema key");
    for (key_index = 1u; key_index < (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT;
            ++key_index) {
        unsigned hits = 0u;
        for (row = 0u; row < sizeof k_allowed / sizeof k_allowed[0]; ++row) {
            if ((unsigned)k_allowed[row].key == key_index) {
                ++hits;
            }
        }
        check_u(hits, 1u, isaac_vita_settings_key_name(
                              (enum isaac_vita_settings_key)key_index));
    }

    /* Key name table matches the key enum order and count (canonical order
     * of the serializer). */
    check_str(isaac_vita_settings_key_name(ISAAC_VITA_SETTINGS_KEY_SCHEMA),
              "schema", "key 0 name");
    check_str(isaac_vita_settings_key_name(ISAAC_VITA_SETTINGS_KEY_PRESET),
              "preset", "key 1 name");
    check_str(isaac_vita_settings_key_name(ISAAC_VITA_SETTINGS_KEY_STICK_SWAP),
              "stick.swap", "last key name");
    check_str(isaac_vita_settings_key_name(ISAAC_VITA_SETTINGS_KEY_COUNT), "",
              "out-of-range key name");
    check_str(isaac_vita_settings_element_name(ISAAC_VITA_SETTINGS_ELEMENT_COUNT),
              "", "out-of-range element name");
    check_str(isaac_vita_settings_status_name(ISAAC_VITA_SETTINGS_STATUS_ABSENT),
              "absent", "status absent");
    check_str(isaac_vita_settings_status_name(ISAAC_VITA_SETTINGS_STATUS_OK),
              "ok", "status ok");
    check_str(isaac_vita_settings_status_name(ISAAC_VITA_SETTINGS_STATUS_SCHEMA),
              "schema", "status schema");
    check_str(isaac_vita_settings_status_name(ISAAC_VITA_SETTINGS_STATUS_GARBAGE),
              "garbage", "status garbage");
    check_str(isaac_vita_settings_status_name(
                  ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE),
              "too-large", "status too-large");
    check_str(isaac_vita_settings_status_name(
                  ISAAC_VITA_SETTINGS_STATUS_IO_ERROR),
              "io-error", "status io-error");
    check_str(isaac_vita_settings_status_name(ISAAC_VITA_SETTINGS_STATUS_COUNT),
              "", "status out of range");
    for (key_index = 0u; key_index < (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT;
            ++key_index) {
        enum isaac_vita_settings_key key = (enum isaac_vita_settings_key)key_index;
        enum isaac_vita_settings_key back = ISAAC_VITA_SETTINGS_KEY_COUNT;
        const char *name = isaac_vita_settings_key_name(key);
        check(strlen(name) >= 1u && strlen(name) <= ISAAC_VITA_SETTINGS_KEY_MAX,
              "key name length in grammar");
        check(isaac_vita_settings_key_from_text(name, strlen(name), &back) == 0 &&
                  back == key,
              "key name round trip");
    }
    for (v = 0u; v < (unsigned)ISAAC_VITA_SETTINGS_ELEMENT_COUNT; ++v) {
        enum isaac_vita_settings_element element =
            (enum isaac_vita_settings_element)v;
        enum isaac_vita_settings_element back = ISAAC_VITA_SETTINGS_ELEMENT_COUNT;
        const char *name = isaac_vita_settings_element_name(element);
        check(isaac_vita_settings_element_from_text(name, strlen(name), &back)
                  == 0 && back == element,
              "element name round trip");
        check_u((unsigned long)isaac_vita_settings_key_accepts_element(
                    ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS, element),
                1u, "phys accepts every element");
        check_u((unsigned long)isaac_vita_settings_key_accepts_element(
                    ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_LEFT, element),
                element == ISAAC_VITA_SETTINGS_ELEMENT_RT ? 0u : 1u,
                "synth accepts every element but rt");
        check_u((unsigned long)isaac_vita_settings_key_accepts_element(
                    ISAAC_VITA_SETTINGS_KEY_FPS_LOCK, element),
                0u, "fps_lock accepts no element");
    }
    {
        enum isaac_vita_settings_element element;
        check(isaac_vita_settings_element_from_text("rtx", 3u, &element) != 0,
              "rtx is not an element");
        check(isaac_vita_settings_element_from_text("start", 5u, &element) != 0,
              "start is not an element");
        check(isaac_vita_settings_element_from_text("dpad_up", 7u, &element)
                  != 0, "dpad_up is not an element");
        check(isaac_vita_settings_element_from_text("a", 0u, &element) != 0,
              "empty element text");
    }

    /* Every allowed value of every key accepted and read back. */
    for (row = 0u; row < sizeof k_allowed / sizeof k_allowed[0]; ++row) {
        const struct allowed_row *r = &k_allowed[row];
        const char *name = isaac_vita_settings_key_name(r->key);
        for (v = 0u; v < r->count; ++v) {
            int written = snprintf(file, sizeof file, "schema=1\n%s=%s\n",
                                   name, r->values[v]);
            check(written > 0 && (size_t)written < sizeof file, "file fits");
            status = parse_text(file, &parsed, &receipt);
            check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, file);
            check_u(receipt.rejected, 0u, "allowed value not rejected");
            check_u(receipt.unknown, 0u, "allowed key known");
            (void)isaac_vita_settings_get_text(&parsed, r->key, got,
                                               sizeof got);
            check_str(got, r->values[v], file);
        }
    }

    /* Every forbidden value rejected on every key; struct stays defaults. */
    for (row = 0u; row < sizeof k_allowed / sizeof k_allowed[0]; ++row) {
        const struct allowed_row *r = &k_allowed[row];
        const char *name = isaac_vita_settings_key_name(r->key);
        struct isaac_vita_settings defaults;
        isaac_vita_settings_defaults(&defaults);
        for (v = 0u; v < sizeof k_forbidden_everywhere /
                         sizeof k_forbidden_everywhere[0]; ++v) {
            const char *value = k_forbidden_everywhere[v];
            int expect_ok = 0;
            /* The preset label accepts anything grammar-valid. */
            if (r->key == ISAAC_VITA_SETTINGS_KEY_PRESET) {
                expect_ok = strlen(value) >= 1u &&
                            strlen(value) <= ISAAC_VITA_SETTINGS_VALUE_MAX &&
                            strspn(value, "abcdefghijklmnopqrstuvwxyz"
                                          "0123456789_-") == strlen(value);
            }
            (void)snprintf(file, sizeof file, "schema=1\n%s=%s\n", name, value);
            status = parse_text(file, &parsed, &receipt);
            check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "forbidden file ok");
            if (expect_ok) {
                check_u(receipt.rejected, 0u, file);
            } else {
                check_u(receipt.rejected, 1u, file);
                check_u(receipt.rejected_lines[0], 2u, "forbidden line no");
                check(isaac_vita_settings_equal(&parsed, &defaults), file);
            }
            check_u(receipt.unknown, 0u, "forbidden key still known");
        }
    }

    /* Cross-kind values: every allowed value of every other key is rejected
     * unless the target key allows it too (synth values are a subset of
     * phys values; the preset label accepts every grammar-valid value and is
     * skipped as a target).  Catches a kind-dispatch or table mix-up in
     * set_text that generic garbage cannot. */
    for (row = 0u; row < sizeof k_allowed / sizeof k_allowed[0]; ++row) {
        const struct allowed_row *r = &k_allowed[row];
        const char *name = isaac_vita_settings_key_name(r->key);
        struct isaac_vita_settings defaults;
        unsigned crossed = 0u;
        if (r->key == ISAAC_VITA_SETTINGS_KEY_PRESET) {
            continue;
        }
        isaac_vita_settings_defaults(&defaults);
        for (other = 0u; other < sizeof k_allowed / sizeof k_allowed[0];
                ++other) {
            const struct allowed_row *o = &k_allowed[other];
            if (isaac_vita_settings_key_kind(o->key) ==
                    isaac_vita_settings_key_kind(r->key)) {
                continue;
            }
            for (v = 0u; v < o->count; ++v) {
                const char *value = o->values[v];
                if (row_allows(r, value)) {
                    continue;
                }
                (void)snprintf(file, sizeof file, "schema=1\n%s=%s\n", name,
                               value);
                status = parse_text(file, &parsed, &receipt);
                check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, file);
                check_u(receipt.rejected, 1u, file);
                check_u(receipt.rejected_lines[0], 2u, "cross-kind line no");
                check_u(receipt.unknown, 0u, "cross-kind key known");
                check(isaac_vita_settings_equal(&parsed, &defaults), file);
                check_u(receipt.assigned,
                        ISAAC_VITA_SETTINGS_ASSIGNED_BIT(
                            ISAAC_VITA_SETTINGS_KEY_SCHEMA),
                        "cross-kind leaves the key unassigned");
                ++crossed;
            }
        }
        check(crossed >= 5u, "cross-kind loop fed every key");
    }

    /* rt on every synth key is rejected; on every phys key accepted. */
    for (row = 0u; row < sizeof k_allowed / sizeof k_allowed[0]; ++row) {
        const struct allowed_row *r = &k_allowed[row];
        enum isaac_vita_settings_kind kind = isaac_vita_settings_key_kind(r->key);
        if (kind != ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT &&
                kind != ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT) {
            continue;
        }
        (void)snprintf(file, sizeof file, "schema=1\n%s=rt\n",
                       isaac_vita_settings_key_name(r->key));
        status = parse_text(file, &parsed, &receipt);
        check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "rt file ok");
        check_u(receipt.rejected,
                kind == ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT ? 1u : 0u, file);
    }

    /* Key length 31 accepted as unknown; 32 rejected. */
    status = parse_text("schema=1\n"
                        "k123456789012345678901234567890=a\n"
                        "k1234567890123456789012345678901=a\n",
                        &parsed, &receipt);
    check_u(receipt.unknown, 1u, "31-char key unknown");
    check_u(receipt.rejected, 1u, "32-char key rejected");
    check_u(receipt.rejected_lines[0], 3u, "32-char key line");
    check_str(receipt.unknown_keys[0], "k12345678901234", "31-char key name");

    /* Value length 15 accepted (preset), 16 rejected. */
    status = parse_text("schema=1\npreset=abcdefghijklmno\n", &parsed, &receipt);
    check_u(receipt.rejected, 0u, "15-char value ok");
    check_str(parsed.preset, "abcdefghijklmno", "15-char preset stored");
    status = parse_text("schema=1\npreset=abcdefghijklmnop\n", &parsed,
                        &receipt);
    check_u(receipt.rejected, 1u, "16-char value rejected");
    check_str(parsed.preset, "", "16-char preset not stored");

    /* Uppercase / invalid characters in keys. */
    status = parse_text("schema=1\nFps_lock=45\nfps-lock=45\nfps lock=45\n"
                        "fps_lock=4 5\n", &parsed, &receipt);
    check_u(receipt.unknown, 0u, "bad keys not unknown");
    check_u(receipt.rejected, 4u, "bad keys rejected");
    check_u(parsed.fps_lock, 60u, "bad keys keep default");

    /* set_text / get_text direct API. */
    {
        struct isaac_vita_settings s;
        isaac_vita_settings_defaults(&s);
        check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
                                           "45", 2u) == 0, "set_text fps 45");
        check_u(s.fps_lock, 45u, "set_text applied");
        check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
                                           "50", 2u) != 0, "set_text fps 50");
        check_u(s.fps_lock, 45u, "set_text rejected leaves value");
        check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_SCHEMA,
                                           "1", 1u) == 0, "set_text schema 1");
        check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_SCHEMA,
                                           "2", 1u) != 0, "set_text schema 2");
        check(isaac_vita_settings_set_text(&s, ISAAC_VITA_SETTINGS_KEY_COUNT,
                                           "1", 1u) != 0, "set_text bad key");
        check(isaac_vita_settings_set_text(
                  &s, ISAAC_VITA_SETTINGS_KEY_CHORD_START_L, "rt", 2u) != 0,
              "set_text rt on chord");
        check(isaac_vita_settings_set_text(
                  &s, ISAAC_VITA_SETTINGS_KEY_BUTTON_L, "rt", 2u) == 0,
              "set_text rt on button");
        check_u(s.button[ISAAC_VITA_SETTINGS_BUTTON_L],
                ISAAC_VITA_SETTINGS_ELEMENT_RT, "set_text rt stored");
        check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_SCHEMA,
                                             got, sizeof got),
                0u, "get_text schema has no text");
        check_str(got, "", "get_text schema empty");
        check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_PRESET,
                                             got, sizeof got),
                0u, "get_text absent preset");
        check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
                                             got, 2u),
                0u, "get_text too small");
        check_str(got, "", "get_text too small terminated");
        check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
                                             got, 3u),
                2u, "get_text exact fit");
        check_str(got, "45", "get_text exact fit text");
        check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
                                             got, 0u),
                0u, "get_text cap 0");
        s.fps_lock = 59u;
        check_u(isaac_vita_settings_get_text(&s, ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
                                             got, sizeof got),
                0u, "get_text out-of-set value");
    }
}

static void test_guard(void)
{
    struct isaac_vita_settings defaults;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;

    isaac_vita_settings_defaults(&defaults);

    /* Everything none: guard fails, values kept by the parser, flag set. */
    status = parse_text("schema=1\nfps_lock=45\n"
                        "button.cross=none\nbutton.circle=none\n"
                        "button.square=none\nbutton.triangle=none\n"
                        "button.l=none\nbutton.r=none\nbutton.select=none\n"
                        "chord.start_l=a\nstick.swap=on\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "guard file ok");
    check_u(receipt.remap_guard, 1u, "guard flag set");
    check_u(receipt.rejected, 0u, "guard rejected none");
    check(!isaac_vita_settings_validate_remap(&parsed), "validate fails");
    check_u(parsed.button[ISAAC_VITA_SETTINGS_BUTTON_CROSS],
            ISAAC_VITA_SETTINGS_ELEMENT_NONE, "guard keeps parsed values");
    check_u(parsed.chord_start_l, ISAAC_VITA_SETTINGS_ELEMENT_A,
            "guard keeps parsed chord");
    /* Loader path: reset the remap section, keep the rest. */
    isaac_vita_settings_reset_remap(&parsed);
    check_u(parsed.fps_lock, 45u, "reset_remap keeps fps_lock");
    check(isaac_vita_settings_validate_remap(&parsed), "reset passes guard");
    parsed.fps_lock = 60u;
    check(isaac_vita_settings_equal(&parsed, &defaults),
          "reset_remap == defaults remap");

    /* Only a, no b. */
    status = parse_text("schema=1\nbutton.circle=x\n", &parsed, &receipt);
    check_u(receipt.remap_guard, 1u, "no b -> guard");
    /* Only b, no a. */
    status = parse_text("schema=1\nbutton.cross=y\n", &parsed, &receipt);
    check_u(receipt.remap_guard, 1u, "no a -> guard");
    /* a and b moved to other buttons: passes. */
    status = parse_text("schema=1\nbutton.cross=none\nbutton.circle=none\n"
                        "button.select=a\nbutton.l=b\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "moved a/b ok");
    check_u(receipt.remap_guard, 0u, "moved a/b passes guard");
    /* Chord or touch targets do not satisfy the guard. */
    status = parse_text("schema=1\nbutton.cross=x\nchord.start_l=a\n"
                        "touch.top_left=a\n", &parsed, &receipt);
    check_u(receipt.remap_guard, 1u, "chord/touch a does not count");
    /* Guard flag is never set when the file is ignored. */
    status = parse_text("button.cross=none\nbutton.circle=none\n", &parsed,
                        &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "ignored file schema");
    check_u(receipt.remap_guard, 0u, "ignored file no guard flag");
    /* Rejected line does not lose the default a. */
    status = parse_text("schema=1\nbutton.cross=start\n", &parsed, &receipt);
    check_u(receipt.rejected, 1u, "start rejected");
    check_u(receipt.remap_guard, 0u, "rejected line keeps default a");
}

static void test_serialize(void)
{
    struct isaac_vita_settings defaults;
    struct isaac_vita_settings custom;
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    char out[1024];
    size_t len;
    size_t i;

    isaac_vita_settings_defaults(&defaults);

    /* Golden bytes of the defaults. */
    memset(out, 'x', sizeof out);
    len = isaac_vita_settings_serialize(&defaults, out, sizeof out);
    check_u(len, strlen(k_defaults_text), "defaults serialize length");
    check_str(out, k_defaults_text, "defaults serialize text");
    check(len > 0u && out[len] == '\0', "serialize terminates");
    for (i = 0u; i < len; ++i) {
        check(out[i] != '\r', "serialize LF only");
        if (out[i] == '\r') {
            break;
        }
    }
    /* Round trip of the defaults. */
    check_u(parse_bytes(out, len, &parsed, &receipt),
            ISAAC_VITA_SETTINGS_STATUS_OK, "defaults round trip status");
    check(isaac_vita_settings_equal(&parsed, &defaults), "defaults round trip");
    check_u(receipt.rejected, 0u, "defaults round trip rejected");
    check_u(receipt.unknown, 0u, "defaults round trip unknown");
    check_u(receipt.dup, 0u, "defaults round trip dup");
    check_u(receipt.lines, 18u, "defaults round trip lines");

    /* Fully customised struct, every field off its default. */
    isaac_vita_settings_defaults(&custom);
    custom.fps_lock = 40u;
    custom.display_raster = 720u;
    custom.render_scale = 2u;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_CROSS] = ISAAC_VITA_SETTINGS_ELEMENT_B;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_CIRCLE] = ISAAC_VITA_SETTINGS_ELEMENT_A;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_SQUARE] = ISAAC_VITA_SETTINGS_ELEMENT_Y;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_TRIANGLE] = ISAAC_VITA_SETTINGS_ELEMENT_X;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_L] = ISAAC_VITA_SETTINGS_ELEMENT_LT;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_R] = ISAAC_VITA_SETTINGS_ELEMENT_RB;
    custom.button[ISAAC_VITA_SETTINGS_BUTTON_SELECT] = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
    custom.chord_start_l = ISAAC_VITA_SETTINGS_ELEMENT_LB;
    custom.chord_start_r = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
    custom.chord_restart_hold = 0u;
    custom.touch[ISAAC_VITA_SETTINGS_TOUCH_TOP_LEFT] = ISAAC_VITA_SETTINGS_ELEMENT_BACK;
    custom.touch[ISAAC_VITA_SETTINGS_TOUCH_TOP_RIGHT] = ISAAC_VITA_SETTINGS_ELEMENT_X;
    custom.touch[ISAAC_VITA_SETTINGS_TOUCH_BOTTOM_RIGHT] = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
    custom.stick_swap = 1u;
    memcpy(custom.preset, "speed-run_2", 12u);
    check(!isaac_vita_settings_equal(&custom, &defaults), "custom differs");
    len = isaac_vita_settings_serialize(&custom, out, sizeof out);
    check_str(out,
              "schema=1\n"
              "preset=speed-run_2\n"
              "fps_lock=40\n"
              "display_raster=720\n"
              "render_scale=2\n"
              "button.cross=b\n"
              "button.circle=a\n"
              "button.square=y\n"
              "button.triangle=x\n"
              "button.l=lt\n"
              "button.r=rb\n"
              "button.select=none\n"
              "chord.start_l=lb\n"
              "chord.start_r=none\n"
              "chord.restart_hold=off\n"
              "touch.top_left=back\n"
              "touch.top_right=x\n"
              "touch.bottom_right=none\n"
              "stick.swap=on\n",
              "custom serialize text");
    check_u(len, strlen(out), "custom serialize length");
    check_u(parse_bytes(out, len, &parsed, &receipt),
            ISAAC_VITA_SETTINGS_STATUS_OK, "custom round trip status");
    check(isaac_vita_settings_equal(&parsed, &custom), "custom round trip");
    check_str(parsed.preset, "speed-run_2", "preset carried through");
    check_u(receipt.remap_guard, 0u, "custom passes guard");
    check_u(parsed.render_scale, 2u, "render_scale carried through");
    check_u(receipt.lines, 19u, "custom lines");

    /* Overflow: exact fit succeeds, one byte short fails and clears out. */
    len = strlen(k_defaults_text);
    memset(out, 'x', sizeof out);
    check_u(isaac_vita_settings_serialize(&defaults, out, len + 1u), len,
            "serialize exact fit");
    check_str(out, k_defaults_text, "serialize exact fit text");
    memset(out, 'x', sizeof out);
    check_u(isaac_vita_settings_serialize(&defaults, out, len), 0u,
            "serialize one short");
    check_str(out, "", "serialize overflow clears out");
    check_u(isaac_vita_settings_serialize(&defaults, out, 1u), 0u,
            "serialize cap 1");
    check_u(isaac_vita_settings_serialize(&defaults, out, 0u), 0u,
            "serialize cap 0");
    check_u(isaac_vita_settings_serialize(&defaults, out, 9u), 0u,
            "serialize cap 9 (schema line without terminator)");

    /* A struct with an out-of-set field is not serializable. */
    custom.fps_lock = 59u;
    check_u(isaac_vita_settings_serialize(&custom, out, sizeof out), 0u,
            "unserializable fps_lock");
    custom.fps_lock = 40u;
    custom.chord_start_l = ISAAC_VITA_SETTINGS_ELEMENT_RT;
    check_u(isaac_vita_settings_serialize(&custom, out, sizeof out), 0u,
            "unserializable rt on chord");
    custom.chord_start_l = ISAAC_VITA_SETTINGS_ELEMENT_LB;
    custom.stick_swap = 2u;
    check_u(isaac_vita_settings_serialize(&custom, out, sizeof out), 0u,
            "unserializable stick_swap 2");
    custom.stick_swap = 1u;
    check(isaac_vita_settings_serialize(&custom, out, sizeof out) > 0u,
          "serializable again");

    /* Serialized output always parses back within the size cap. */
    check(len <= ISAAC_VITA_SETTINGS_MAX_BYTES, "canonical file under cap");

    /* Preset label outside the grammar or not NUL-terminated inside the
     * field: not serializable (serialize 0, get_text 0), never read past the
     * field, and equal() still compares bounded. */
    {
        struct isaac_vita_settings bad;
        char text[32];
        isaac_vita_settings_defaults(&bad);
        memcpy(bad.preset, "Speed Run", 10u);
        memset(out, 'x', sizeof out);
        check_u(isaac_vita_settings_serialize(&bad, out, sizeof out), 0u,
                "preset with blank/uppercase not serializable");
        check_str(out, "", "bad preset clears out");
        check_u(isaac_vita_settings_get_text(&bad, ISAAC_VITA_SETTINGS_KEY_PRESET,
                                             text, sizeof text),
                0u, "get_text bad preset");
        check_str(text, "", "get_text bad preset terminated");
        memset(bad.preset, 'a', sizeof bad.preset);
        check_u(isaac_vita_settings_serialize(&bad, out, sizeof out), 0u,
                "unterminated 16-byte preset not serializable");
        check_u(isaac_vita_settings_get_text(&bad, ISAAC_VITA_SETTINGS_KEY_PRESET,
                                             text, sizeof text),
                0u, "get_text unterminated preset");
        check(!isaac_vita_settings_equal(&bad, &defaults),
              "unterminated preset differs from defaults");
        memcpy(bad.preset, "abcdefghijklmno", 16u);
        check(isaac_vita_settings_serialize(&bad, out, sizeof out) > 0u,
              "15-char preset serializable");
        check_u(isaac_vita_settings_get_text(&bad, ISAAC_VITA_SETTINGS_KEY_PRESET,
                                             text, sizeof text),
                15u, "get_text 15-char preset");
        memcpy(bad.preset, "a\0garbage-tail", 15u);
        check_u(isaac_vita_settings_get_text(&bad, ISAAC_VITA_SETTINGS_KEY_PRESET,
                                             text, sizeof text),
                1u, "get_text stops at the first NUL");
        check_str(text, "a", "get_text one-char preset");
        bad.preset[0] = '\0';
        check(isaac_vita_settings_equal(&bad, &defaults),
              "bytes after the first NUL are not part of the label");
        check_u(isaac_vita_settings_serialize(&bad, out, sizeof out), len,
                "absent preset with tail bytes serializes as absent");
        check_str(out, k_defaults_text, "absent preset with tail bytes text");
    }

    /* SERIALIZED_MAX: the worst-case struct hits it exactly, an independent
     * sum over the longest allowed value of every key agrees, and it is
     * under the file cap. */
    {
        struct isaac_vita_settings worst;
        unsigned row;
        unsigned v;
        size_t expected = strlen("schema=1\n");
        isaac_vita_settings_defaults(&worst);
        memcpy(worst.preset, "abcdefghijklmno", 16u);
        memset(worst.button, ISAAC_VITA_SETTINGS_ELEMENT_NONE, sizeof worst.button);
        worst.chord_start_l = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
        worst.chord_start_r = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
        worst.chord_restart_hold = 0u;
        memset(worst.touch, ISAAC_VITA_SETTINGS_ELEMENT_NONE, sizeof worst.touch);
        worst.stick_swap = 0u;
        memset(out, 'x', sizeof out);
        check_u(isaac_vita_settings_serialize(&worst, out, sizeof out),
                ISAAC_VITA_SETTINGS_SERIALIZED_MAX, "worst-case serialize length");
        check_u(strlen(out), ISAAC_VITA_SETTINGS_SERIALIZED_MAX,
                "worst-case serialize terminated at MAX");
        for (row = 0u; row < sizeof k_allowed / sizeof k_allowed[0]; ++row) {
            size_t longest = 0u;
            for (v = 0u; v < k_allowed[row].count; ++v) {
                size_t n = strlen(k_allowed[row].values[v]);
                longest = n > longest ? n : longest;
            }
            expected += strlen(isaac_vita_settings_key_name(k_allowed[row].key)) +
                        2u + longest;
        }
        check_u(expected, ISAAC_VITA_SETTINGS_SERIALIZED_MAX,
                "SERIALIZED_MAX equals the sum of the longest values");
        check(ISAAC_VITA_SETTINGS_SERIALIZED_MAX + 1u <=
                  ISAAC_VITA_SETTINGS_MAX_BYTES,
              "SERIALIZED_MAX plus terminator under the file cap");
        check_u(isaac_vita_settings_serialize(&worst, out,
                                              ISAAC_VITA_SETTINGS_SERIALIZED_MAX),
                0u, "worst-case needs MAX + 1 bytes");
        check_u(isaac_vita_settings_serialize(
                    &worst, out, ISAAC_VITA_SETTINGS_SERIALIZED_MAX + 1u),
                ISAAC_VITA_SETTINGS_SERIALIZED_MAX, "worst-case fits MAX + 1");
        check_u(parse_bytes(out, ISAAC_VITA_SETTINGS_SERIALIZED_MAX, &parsed,
                            &receipt),
                ISAAC_VITA_SETTINGS_STATUS_OK, "worst-case round trip status");
        check(isaac_vita_settings_equal(&parsed, &worst), "worst-case round trip");
        check_u(receipt.remap_guard, 1u, "worst-case fails the guard (all none)");
    }

    /* equal() ignores nothing that matters: flip each field. */
    {
        struct isaac_vita_settings a;
        struct isaac_vita_settings b;
        isaac_vita_settings_defaults(&a);
        isaac_vita_settings_defaults(&b);
        check(isaac_vita_settings_equal(&a, &b), "equal defaults");
        b.preset[0] = 'p';
        check(!isaac_vita_settings_equal(&a, &b), "equal sees preset");
        isaac_vita_settings_defaults(&b);
        b.touch[2] = ISAAC_VITA_SETTINGS_ELEMENT_NONE;
        check(!isaac_vita_settings_equal(&a, &b), "equal sees touch");
        isaac_vita_settings_defaults(&b);
        b.chord_restart_hold = 0u;
        check(!isaac_vita_settings_equal(&a, &b), "equal sees restart_hold");
        isaac_vita_settings_defaults(&b);
        b.render_scale = 2u;
        check(!isaac_vita_settings_equal(&a, &b), "equal sees render_scale");
    }

    /* receipt_clear golden. */
    memset(&receipt, 0xFF, sizeof receipt);
    isaac_vita_settings_receipt_clear(&receipt);
    check_u(receipt.status, ISAAC_VITA_SETTINGS_STATUS_ABSENT, "clear status");
    check_u(receipt.io_error, 0u, "clear io_error");
    check_u(receipt.bytes, 0u, "clear bytes");
    check_u(receipt.rejected_lines[3], 0u, "clear slots");
    check_str(receipt.unknown_keys[3], "", "clear names");
}

/* Loader model for the display_raster default (design section 1.3: default
 * = build raster).  The shared TU cannot see the build raster, so the loader
 * keeps its own value whenever the key was not assigned. */
static unsigned loader_raster(const struct isaac_vita_settings *parsed,
                              const struct isaac_vita_settings_receipt *receipt,
                              unsigned build_raster)
{
    if (receipt->status != ISAAC_VITA_SETTINGS_STATUS_OK ||
            (receipt->assigned &
             ISAAC_VITA_SETTINGS_ASSIGNED_BIT(
                 ISAAC_VITA_SETTINGS_KEY_DISPLAY_RASTER)) == 0u) {
        return build_raster;
    }
    return parsed->display_raster;
}

static void test_assigned_mask(void)
{
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    enum isaac_vita_settings_status status;
    const uint32_t schema_bit =
        ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_SCHEMA);
    const uint32_t raster_bit =
        ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_DISPLAY_RASTER);
    const uint32_t fps_bit =
        ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_FPS_LOCK);

    /* File without display_raster: the struct shows the TU default 960, the
     * bit is clear, and a 720 build keeps 720. */
    status = parse_text("schema=1\nfps_lock=45\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "assigned: fps only ok");
    check_u(receipt.assigned, schema_bit | fps_bit, "assigned: schema + fps");
    check_u(parsed.display_raster, 960u, "assigned: struct raster stays 960");
    check_u(loader_raster(&parsed, &receipt, 720u), 720u,
            "file without display_raster leaves the caller-supplied raster");
    check_u(loader_raster(&parsed, &receipt, 960u), 960u,
            "file without display_raster on a 960 build");

    /* display_raster=960 is an assignment even though it equals the TU
     * default: a 720 build must switch to 960. */
    status = parse_text("schema=1\ndisplay_raster=960\n", &parsed, &receipt);
    check_u(receipt.assigned, schema_bit | raster_bit, "assigned: raster 960");
    check_u(loader_raster(&parsed, &receipt, 720u), 960u,
            "explicit 960 overrides a 720 build");
    status = parse_text("schema=1\ndisplay_raster=720\n", &parsed, &receipt);
    check_u(receipt.assigned, schema_bit | raster_bit, "assigned: raster 720");
    check_u(loader_raster(&parsed, &receipt, 960u), 720u,
            "explicit 720 overrides a 960 build");

    /* A rejected value does not assign; a duplicate assigns once. */
    status = parse_text("schema=1\ndisplay_raster=600\n", &parsed, &receipt);
    check_u(receipt.rejected, 1u, "assigned: bad raster rejected");
    check_u(receipt.assigned, schema_bit, "assigned: bad raster not assigned");
    check_u(loader_raster(&parsed, &receipt, 720u), 720u,
            "bad raster keeps the build raster");
    status = parse_text("schema=1\nfps_lock=45\nfps_lock=40\n", &parsed,
                        &receipt);
    check_u(receipt.dup, 1u, "assigned: dup counted");
    check_u(receipt.assigned, schema_bit | fps_bit, "assigned: dup one bit");

    /* Unknown keys never set a bit; an ignored file reports nothing
     * assigned even though its lines were valid. */
    status = parse_text("schema=1\ncolour=blue\n", &parsed, &receipt);
    check_u(receipt.assigned, schema_bit, "assigned: unknown key no bit");
    status = parse_text("fps_lock=45\ndisplay_raster=720\n", &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_SCHEMA, "assigned: no schema");
    check_u(receipt.assigned, 0u, "assigned: ignored file reports none");
    check_u(loader_raster(&parsed, &receipt, 720u), 720u,
            "ignored file keeps the build raster");
    status = parse_text("schema=2\ndisplay_raster=720\n", &parsed, &receipt);
    check_u(receipt.assigned, 0u, "assigned: bad schema reports none");

    /* Every key assigned once in the canonical file. */
    status = parse_text(k_defaults_text, &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "assigned: canonical ok");
    check_u(receipt.assigned,
            ((1u << (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT) - 1u) &
                ~ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_PRESET),
            "assigned: canonical file sets every bit but preset");
    status = parse_text("schema=1\npreset=default\n", &parsed, &receipt);
    check_u(receipt.assigned,
            schema_bit |
                ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_PRESET),
            "assigned: preset bit");
}

static void test_receipt_golden(void)
{
    /* One mixed file, every receipt field checked (the section 1.5 text is
     * formatted from these fields by the runtime loader). */
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    const char text[] =
        "# port.ini written by the manager\r\n"   /* 1 */
        "schema=1\r\n"                             /* 2 */
        "fps_lock=45\r\n"                          /* 3 */
        "fps_lock=40\r\n"                          /* 4 dup */
        "colour=blue\r\n"                          /* 5 unknown */
        "button.cross=start\r\n"                   /* 6 rejected */
        "chord.start_l=rt\r\n"                     /* 7 rejected */
        "touch.top_left=\xC3\xA9\r\n"              /* 8 rejected */
        "verylongunknownkeyname=1\r\n"             /* 9 unknown */
        "stick.swap=on\r\n"                        /* 10 */
        "junk line\r\n"                            /* 11 rejected */
        "x=y\r\n"                                  /* 12 unknown */
        "Zed=1\r\n"                                /* 13 rejected */
        "z.z=1";                                   /* 14 unknown */
    enum isaac_vita_settings_status status;

    status = parse_bytes(text, sizeof text - 1u, &parsed, &receipt);
    check_u(status, ISAAC_VITA_SETTINGS_STATUS_OK, "golden status");
    check_u(receipt.status, ISAAC_VITA_SETTINGS_STATUS_OK, "golden receipt status");
    check_u(receipt.bytes, sizeof text - 1u, "golden bytes");
    check_u(receipt.lines, 14u, "golden lines");
    check_u(receipt.rejected, 5u, "golden rejected");
    check_u(receipt.unknown, 4u, "golden unknown");
    check_u(receipt.dup, 1u, "golden dup");
    check_u(receipt.assigned,
            ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_SCHEMA) |
                ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_FPS_LOCK) |
                ISAAC_VITA_SETTINGS_ASSIGNED_BIT(ISAAC_VITA_SETTINGS_KEY_STICK_SWAP),
            "golden assigned");
    check_u(receipt.rejected_lines[0], 6u, "golden rejected line 0");
    check_u(receipt.rejected_lines[1], 7u, "golden rejected line 1");
    check_u(receipt.rejected_lines[2], 8u, "golden rejected line 2");
    check_u(receipt.rejected_lines[3], 11u, "golden rejected line 3");
    check_str(receipt.unknown_keys[0], "colour", "golden unknown 0");
    check_str(receipt.unknown_keys[1], "verylongunknown", "golden unknown 1");
    check_str(receipt.unknown_keys[2], "x", "golden unknown 2");
    check_str(receipt.unknown_keys[3], "z.z", "golden unknown 3");
    check_u(receipt.remap_guard, 0u, "golden guard");
    check_u(receipt.io_error, 0u, "golden io_error untouched by parser");
    check_u(parsed.fps_lock, 40u, "golden fps_lock");
    check_u(parsed.stick_swap, 1u, "golden stick_swap");
    check_u(parsed.button[ISAAC_VITA_SETTINGS_BUTTON_CROSS],
            ISAAC_VITA_SETTINGS_ELEMENT_A, "golden cross default");
    check_u(parsed.chord_start_l, ISAAC_VITA_SETTINGS_ELEMENT_RB,
            "golden chord default");
    check_u(parsed.touch[ISAAC_VITA_SETTINGS_TOUCH_TOP_LEFT],
            ISAAC_VITA_SETTINGS_ELEMENT_LT, "golden touch default");
}

int main(void)
{
    test_defaults_and_empty();
    test_comments_and_layout();
    test_size_and_bytes();
    test_duplicates_unknown_schema();
    test_every_value();
    test_guard();
    test_serialize();
    test_assigned_mask();
    test_receipt_golden();
    if (s_failures != 0u) {
        fprintf(stderr, "host_vita_settings_oracle: %u of %u checks failed\n",
                s_failures, s_checks);
        return 1;
    }
    printf("host_vita_settings_oracle: PASS (%u checks; grammar corpus, "
           "every key value incl. cross-kind, guard, serialize round trip, "
           "label bounds, SERIALIZED_MAX, assigned mask, receipt fields)\n",
           s_checks);
    return 0;
}
