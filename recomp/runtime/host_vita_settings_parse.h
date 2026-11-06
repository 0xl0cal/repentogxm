/* Shared port.ini parser/serializer (ISAAC_VITA_RUNTIME_SETTINGS).
 *
 * One pure-C11 translation unit compiled into both SELFs: the game runtime
 * (host_vita_runtime_settings.c reads ux0:/data/isaacr001/port.ini once at
 * boot) and the LiveArea manager (the only writer).  Grammar and key table
 * follow section 1 of the port.ini design:
 *
 *   - ASCII only (0x09, 0x0A, 0x0D, 0x20-0x7E); any other byte rejects that
 *     line; a NUL byte anywhere makes the whole file "garbage".
 *   - Hard cap 4096 bytes; a longer buffer is "too-large" and ignored.
 *   - Lines split on LF, one trailing CR stripped, missing final LF tolerated.
 *   - Leading/trailing blanks (space, tab) trimmed; empty lines, '#' / ';'
 *     comments and "[section]" lines are ignored (not counted).
 *   - key=value with key [a-z0-9_.]{1,31} and value [a-z0-9_-]{1,15};
 *     anything else rejects the line.
 *   - Duplicate known key: last wins, counted.  Unknown key: ignored, counted,
 *     first four names kept (truncated to 15 characters).  Known key with a
 *     value outside its allowed set: line rejected, key keeps its default.
 *   - A rejected line never changes state: a repeat whose value is rejected
 *     keeps the earlier accepted value (or the default), is counted under
 *     "rejected" and not under "dup"; "schema=1" followed by "schema=2"
 *     stays accepted (the bad line is only rejected).  Duplicates are
 *     counted only among accepted assignments.
 *   - "schema=1" is mandatory; absent or another value -> status "schema"
 *     and the whole file is ignored (the struct stays at the defaults).
 *     The byte set applies before comment/blank trimming and exactly one
 *     trailing CR is stripped, so a UTF-8 BOM in front of "schema=1" or a
 *     CR-CR-LF schema line rejects line 1 and thereby the whole file; the
 *     manager's INVALID row must expect that from hand-edited files.
 *
 * The struct is filled from compile-time defaults before parsing, so
 * parse("") == defaults.  The receipt carries an "assigned" bit mask (one
 * bit per key) so a loader can tell "key absent" from "key set to its
 * default": display_raster defaults to 960 here, but the design's default is
 * the build raster (release/v0.1.1-alpha.cmake ISAAC_VITA_DISPLAY_RASTER),
 * so the runtime loader substitutes the build raster whenever the
 * DISPLAY_RASTER bit is clear.  The TU has no SDK, stdio or allocator
 * dependence and no mutable static state; it uses its own ASCII classifiers
 * so no locale can change the accepted set.  The knob that decides where the
 * TU is compiled (ISAAC_VITA_RUNTIME_SETTINGS) is never tested here.
 *
 * One exception, ISAAC_VITA_DEBUG_OVERLAY: its debug_overlay key
 * (off|compact|full, default off) exists only when that macro is defined,
 * because this TU is part of the release eboot and the release build must
 * stay byte-identical with the debug overlay compiled out.  The key adds a
 * struct field, a table row and one KEY_COUNT, so every TU that includes this
 * header must agree on the macro.  The renames below make a disagreement a
 * link error (the layout-dependent entry points get a _dbgov suffix) instead
 * of a silently mismatched struct. */
#ifndef HOST_VITA_SETTINGS_PARSE_H
#define HOST_VITA_SETTINGS_PARSE_H

#include <stddef.h>
#include <stdint.h>

#if defined(ISAAC_VITA_DEBUG_OVERLAY)
#define isaac_vita_settings_defaults isaac_vita_settings_defaults_dbgov
#define isaac_vita_settings_parse isaac_vita_settings_parse_dbgov
#define isaac_vita_settings_validate_remap \
    isaac_vita_settings_validate_remap_dbgov
#define isaac_vita_settings_reset_remap isaac_vita_settings_reset_remap_dbgov
#define isaac_vita_settings_serialize isaac_vita_settings_serialize_dbgov
#define isaac_vita_settings_equal isaac_vita_settings_equal_dbgov
#define isaac_vita_settings_key_name isaac_vita_settings_key_name_dbgov
#define isaac_vita_settings_key_kind isaac_vita_settings_key_kind_dbgov
#define isaac_vita_settings_key_from_text \
    isaac_vita_settings_key_from_text_dbgov
#define isaac_vita_settings_key_accepts_element \
    isaac_vita_settings_key_accepts_element_dbgov
#define isaac_vita_settings_get_text isaac_vita_settings_get_text_dbgov
#define isaac_vita_settings_set_text isaac_vita_settings_set_text_dbgov
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Hard cap of an accepted file (section 1.2); the reader requests one more
 * byte to detect the overflow. */
#define ISAAC_VITA_SETTINGS_MAX_BYTES 4096u
/* Longest accepted key / value (characters, without terminator). */
#define ISAAC_VITA_SETTINGS_KEY_MAX 31u
#define ISAAC_VITA_SETTINGS_VALUE_MAX 15u
/* Slots for the first rejected line numbers / unknown key names carried in
 * the receipt (section 1.5 second line). */
#define ISAAC_VITA_SETTINGS_RECEIPT_SLOTS 4u
/* Longest canonical file isaac_vita_settings_serialize() can produce
 * (without the terminator): "schema=1\n", a 15-character preset, every
 * element key at a 4-character element, both on/off keys at "off", the
 * one-digit render_scale.  The manager's write buffer needs this plus one
 * byte; pinned by the oracle.  With the debug overlay compiled in, one more
 * line "debug_overlay=compact\n" (22 bytes). */
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
#define ISAAC_VITA_SETTINGS_SERIALIZED_MAX 365u
#else
#define ISAAC_VITA_SETTINGS_SERIALIZED_MAX 343u
#endif
/* Bit of one key in isaac_vita_settings_receipt.assigned. */
#define ISAAC_VITA_SETTINGS_ASSIGNED_BIT(key) (1u << (unsigned)(key))

/* Element a physical button, chord or touch zone maps to (section 1.3). */
enum isaac_vita_settings_element {
    ISAAC_VITA_SETTINGS_ELEMENT_NONE = 0,
    ISAAC_VITA_SETTINGS_ELEMENT_A,
    ISAAC_VITA_SETTINGS_ELEMENT_B,
    ISAAC_VITA_SETTINGS_ELEMENT_X,
    ISAAC_VITA_SETTINGS_ELEMENT_Y,
    ISAAC_VITA_SETTINGS_ELEMENT_LB,
    ISAAC_VITA_SETTINGS_ELEMENT_RB,
    ISAAC_VITA_SETTINGS_ELEMENT_LT,
    ISAAC_VITA_SETTINGS_ELEMENT_RT,
    ISAAC_VITA_SETTINGS_ELEMENT_BACK,
    ISAAC_VITA_SETTINGS_ELEMENT_COUNT
};

/* The seven remappable physical inputs (button.* keys), in key-table order. */
enum isaac_vita_settings_button {
    ISAAC_VITA_SETTINGS_BUTTON_CROSS = 0,
    ISAAC_VITA_SETTINGS_BUTTON_CIRCLE,
    ISAAC_VITA_SETTINGS_BUTTON_SQUARE,
    ISAAC_VITA_SETTINGS_BUTTON_TRIANGLE,
    ISAAC_VITA_SETTINGS_BUTTON_L,
    ISAAC_VITA_SETTINGS_BUTTON_R,
    ISAAC_VITA_SETTINGS_BUTTON_SELECT,
    ISAAC_VITA_SETTINGS_BUTTON_COUNT
};

/* The three front-touch zones (touch.* keys), in key-table order. */
enum isaac_vita_settings_touch {
    ISAAC_VITA_SETTINGS_TOUCH_TOP_LEFT = 0,
    ISAAC_VITA_SETTINGS_TOUCH_TOP_RIGHT,
    ISAAC_VITA_SETTINGS_TOUCH_BOTTOM_RIGHT,
    ISAAC_VITA_SETTINGS_TOUCH_COUNT
};

/* Every key of the table, in canonical (serialization) order. */
enum isaac_vita_settings_key {
    ISAAC_VITA_SETTINGS_KEY_SCHEMA = 0,
    ISAAC_VITA_SETTINGS_KEY_PRESET,
    ISAAC_VITA_SETTINGS_KEY_FPS_LOCK,
    ISAAC_VITA_SETTINGS_KEY_DISPLAY_RASTER,
    ISAAC_VITA_SETTINGS_KEY_RENDER_SCALE,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_CIRCLE,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_SQUARE,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_TRIANGLE,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_L,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_R,
    ISAAC_VITA_SETTINGS_KEY_BUTTON_SELECT,
    ISAAC_VITA_SETTINGS_KEY_CHORD_START_L,
    ISAAC_VITA_SETTINGS_KEY_CHORD_START_R,
    ISAAC_VITA_SETTINGS_KEY_CHORD_RESTART_HOLD,
    ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_LEFT,
    ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_RIGHT,
    ISAAC_VITA_SETTINGS_KEY_TOUCH_BOTTOM_RIGHT,
    ISAAC_VITA_SETTINGS_KEY_STICK_SWAP,
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    ISAAC_VITA_SETTINGS_KEY_DEBUG_OVERLAY,
#endif
    ISAAC_VITA_SETTINGS_KEY_COUNT
};

/* Value class of a key (drives accept/reject and the manager's choosers). */
enum isaac_vita_settings_kind {
    ISAAC_VITA_SETTINGS_KIND_SCHEMA = 0,   /* "1" only                  */
    ISAAC_VITA_SETTINGS_KIND_LABEL,        /* any grammar-valid value   */
    ISAAC_VITA_SETTINGS_KIND_FPS_LOCK,     /* 60 45 40 30               */
    ISAAC_VITA_SETTINGS_KIND_RASTER,       /* 960 720 480               */
    ISAAC_VITA_SETTINGS_KIND_RENDER_SCALE, /* 1 2 (options.ini MaxRenderScale) */
    ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT, /* none..back incl. rt       */
    ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT,/* none..back, rt rejected   */
    ISAAC_VITA_SETTINGS_KIND_ONOFF         /* on off                    */
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    ,
    ISAAC_VITA_SETTINGS_KIND_DEBUG_OVERLAY /* off compact full          */
#endif
};

#if defined(ISAAC_VITA_DEBUG_OVERLAY)
/* Stored value of the debug_overlay key == index of its name. */
enum isaac_vita_settings_debug_overlay {
    ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_OFF = 0,
    ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_COMPACT,
    ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_FULL,
    ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_COUNT
};
#endif

/* Parse outcome of a whole file.  ABSENT and IO_ERROR are never produced by
 * the parser; the loader sets them when the open/read fails. */
enum isaac_vita_settings_status {
    ISAAC_VITA_SETTINGS_STATUS_ABSENT = 0,
    ISAAC_VITA_SETTINGS_STATUS_OK,
    ISAAC_VITA_SETTINGS_STATUS_SCHEMA,
    ISAAC_VITA_SETTINGS_STATUS_GARBAGE,
    ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE,
    ISAAC_VITA_SETTINGS_STATUS_IO_ERROR,
    ISAAC_VITA_SETTINGS_STATUS_COUNT
};

/* The runtime object of section 1.4 before the consumers translate it. */
struct isaac_vita_settings {
    uint16_t fps_lock;       /* 60 45 40 30; default 60                  */
    uint16_t display_raster; /* 960 720 480; default 960                 */
    uint8_t render_scale;    /* 1 2: the game's MaxRenderScale the runtime
                              * writes into options.ini at boot (1 = 480x270
                              * world, the port default; 2 = the game's stock
                              * 960x540); default 1                       */
    uint8_t button[ISAAC_VITA_SETTINGS_BUTTON_COUNT]; /* element codes    */
    uint8_t chord_start_l;   /* element, never RT; default RB            */
    uint8_t chord_start_r;   /* element, never RT; default BACK          */
    uint8_t chord_restart_hold; /* 0 / 1; default 1                      */
    uint8_t touch[ISAAC_VITA_SETTINGS_TOUCH_COUNT]; /* element, never RT  */
    uint8_t stick_swap;      /* 0 / 1; default 0                         */
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    uint8_t debug_overlay;   /* enum isaac_vita_settings_debug_overlay;
                              * default off                            */
#endif
    /* Manager-only preset label; "" = absent (not serialized).  The
     * runtime ignores it.  Invariant for a serializable struct: NUL-
     * terminated inside the field and within the value grammar
     * [a-z0-9_-]{1,15}; the parser and set_text always store it that way,
     * and serialize/get_text treat any other content as out of set (no
     * byte past the field is ever read). */
    char preset[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
};

/* Counters and samples for the section 1.5 receipt lines.  The parser owns
 * status (OK/SCHEMA/GARBAGE/TOO_LARGE), bytes, lines, rejected, unknown, dup,
 * assigned, rejected_lines, unknown_keys and remap_guard; the loader owns
 * io_error and the ABSENT/IO_ERROR statuses. */
struct isaac_vita_settings_receipt {
    uint8_t status;          /* enum isaac_vita_settings_status          */
    uint8_t remap_guard;     /* 1 = parsed remap failed the a/b guard    */
    uint32_t io_error;       /* SCE error code for STATUS_IO_ERROR       */
    /* ISAAC_VITA_SETTINGS_ASSIGNED_BIT(key) set for every key whose value
     * in *settings came from an accepted line; 0 unless status is OK.  The
     * loader substitutes the build raster when the DISPLAY_RASTER bit is
     * clear (design section 1.3: default = build raster). */
    uint32_t assigned;
    uint32_t bytes;          /* buffer length handed to the parser       */
    uint32_t lines;          /* physical lines seen                      */
    uint32_t rejected;       /* malformed / bad-value / non-ASCII lines  */
    uint32_t unknown;        /* unknown-key assignments                  */
    uint32_t dup;            /* known key assigned more than once        */
    uint32_t rejected_lines[ISAAC_VITA_SETTINGS_RECEIPT_SLOTS]; /* 1-based */
    char unknown_keys[ISAAC_VITA_SETTINGS_RECEIPT_SLOTS]
                     [ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
};

/* Compile-time defaults (section 1.3): fps_lock 60, display_raster 960 (the
 * TU is shared with the manager and cannot see the build raster; see the
 * receipt's assigned mask), render_scale 1 (owner decision 2026-09-16),
 * buttons a b x y lb lt rt, chords rb / back, restart_hold on, touch zones
 * lt back rb, stick_swap off, preset absent; debug_overlay off when
 * ISAAC_VITA_DEBUG_OVERLAY is compiled in.
 * Zero-fills the struct first so padding bytes are deterministic. */
void isaac_vita_settings_defaults(struct isaac_vita_settings *settings);

/* Zero every receipt field (status ABSENT, io_error 0). */
void isaac_vita_settings_receipt_clear(
    struct isaac_vita_settings_receipt *receipt);

/* Parse buf[0..len) (no NUL terminator required or expected) into
 * *settings, which is reset to the defaults first.  On any status other than
 * OK the struct is left at the defaults.  The receipt (may be NULL) gets
 * status/bytes/lines/rejected/unknown/dup/assigned/rejected_lines/
 * unknown_keys and remap_guard = !isaac_vita_settings_validate_remap() of
 * the parsed result.
 * The parser does not reset the remap section itself: the runtime loader
 * calls isaac_vita_settings_reset_remap() when remap_guard is set, the
 * manager keeps the values and blocks SAVE.  Returns the status. */
enum isaac_vita_settings_status isaac_vita_settings_parse(
    const char *buf, size_t len, struct isaac_vita_settings *settings,
    struct isaac_vita_settings_receipt *receipt);

/* Section 0.9 guard: at least one physical button maps to A and at least one
 * to B.  Returns 1 when satisfied. */
int isaac_vita_settings_validate_remap(
    const struct isaac_vita_settings *settings);

/* Restore button.*, chord.*, touch.* and stick.swap to the defaults; fps_lock,
 * display_raster, render_scale and preset are untouched. */
void isaac_vita_settings_reset_remap(struct isaac_vita_settings *settings);

/* Canonical text: "schema=1\n" then every key in table order as
 * "key=value\n" (LF only); "preset=<label>\n" only when the label is
 * non-empty.  NUL-terminates.  Returns the length without the terminator
 * (at most ISAAC_VITA_SETTINGS_SERIALIZED_MAX), or 0 when cap cannot hold
 * text plus terminator or when any field (including a non-empty preset
 * label) is outside its allowed set.  parse(serialize(s)) == s for every
 * struct whose fields are in their allowed sets. */
size_t isaac_vita_settings_serialize(const struct isaac_vita_settings *settings,
                                     char *out, size_t cap);

/* Field-wise equality (padding ignored). */
int isaac_vita_settings_equal(const struct isaac_vita_settings *a,
                              const struct isaac_vita_settings *b);

/* Section 2: fps_lock -> admission duty k for the fullspeed scheduler.
 * 60 -> 1 (admit every non-full phase, today's behaviour), 45 -> 2,
 * 40 -> 3, 30 -> 0 (admit none).  Any other value -> 1. */
uint8_t isaac_vita_settings_fps_lock_duty_k(uint16_t fps_lock);

/* Name tables and lookups.  *_name returns "" for an out-of-range code. */
const char *isaac_vita_settings_key_name(enum isaac_vita_settings_key key);
enum isaac_vita_settings_kind isaac_vita_settings_key_kind(
    enum isaac_vita_settings_key key);
/* 0 on success, -1 when text[0..len) names no key. */
int isaac_vita_settings_key_from_text(const char *text, size_t len,
                                      enum isaac_vita_settings_key *key);
const char *isaac_vita_settings_element_name(
    enum isaac_vita_settings_element element);
/* 0 on success, -1 when text[0..len) names no element. */
int isaac_vita_settings_element_from_text(
    const char *text, size_t len, enum isaac_vita_settings_element *element);
/* 1 when the key takes elements and accepts this one (RT only on button.*). */
int isaac_vita_settings_key_accepts_element(
    enum isaac_vita_settings_key key, enum isaac_vita_settings_element element);
const char *isaac_vita_settings_status_name(
    enum isaac_vita_settings_status status);

/* Value of one key as canonical text.  Returns the length written (without
 * the terminator) or 0 when the key is SCHEMA, the PRESET label is absent or
 * out of set, the field is outside its allowed set, or cap is too small; out
 * is always NUL-terminated when cap > 0. */
size_t isaac_vita_settings_get_text(const struct isaac_vita_settings *settings,
                                    enum isaac_vita_settings_key key,
                                    char *out, size_t cap);

/* Assign one key from text[0..len) with the same acceptance rules as the
 * parser (value grammar plus the key's allowed set).  SCHEMA accepts only
 * "1" and changes nothing.  Returns 0 on success, -1 when rejected (struct
 * untouched). */
int isaac_vita_settings_set_text(struct isaac_vita_settings *settings,
                                 enum isaac_vita_settings_key key,
                                 const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HOST_VITA_SETTINGS_PARSE_H */
