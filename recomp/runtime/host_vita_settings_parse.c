/* Shared port.ini parser/serializer; see host_vita_settings_parse.h.
 *
 * Constraints (allocator poison list recomp/vita/vita_raw_allocator_gate.py,
 * design section 0.10): C11 only, <string.h> for memset/memcpy/memcmp/strlen/
 * strncmp (strlen only on NUL-terminated table strings and on a label whose
 * termination was proven first), no stdio, no allocator, no SDK header, no
 * mutable static state, no VLA. */
#include "host_vita_settings_parse.h"

#include <string.h>

/* The receipt's assigned mask has one bit per key. */
_Static_assert((unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT <= 32u,
               "assigned mask holds at most 32 keys");

/* ---- key table --------------------------------------------------------- */

struct key_row {
    const char *name;
    enum isaac_vita_settings_kind kind;
};

static const struct key_row k_keys[ISAAC_VITA_SETTINGS_KEY_COUNT] = {
    { "schema", ISAAC_VITA_SETTINGS_KIND_SCHEMA },
    { "preset", ISAAC_VITA_SETTINGS_KIND_LABEL },
    { "fps_lock", ISAAC_VITA_SETTINGS_KIND_FPS_LOCK },
    { "display_raster", ISAAC_VITA_SETTINGS_KIND_RASTER },
    { "render_scale", ISAAC_VITA_SETTINGS_KIND_RENDER_SCALE },
    { "button.cross", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "button.circle", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "button.square", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "button.triangle", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "button.l", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "button.r", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "button.select", ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT },
    { "chord.start_l", ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT },
    { "chord.start_r", ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT },
    { "chord.restart_hold", ISAAC_VITA_SETTINGS_KIND_ONOFF },
    { "touch.top_left", ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT },
    { "touch.top_right", ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT },
    { "touch.bottom_right", ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT },
    { "stick.swap", ISAAC_VITA_SETTINGS_KIND_ONOFF },
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    { "debug_overlay", ISAAC_VITA_SETTINGS_KIND_DEBUG_OVERLAY },
#endif
};

static const char *const k_elements[ISAAC_VITA_SETTINGS_ELEMENT_COUNT] = {
    "none", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "back",
};

static const char *const k_status[ISAAC_VITA_SETTINGS_STATUS_COUNT] = {
    "absent", "ok", "schema", "garbage", "too-large", "io-error",
};

/* Value/name pairs; every loop below derives its bound from the array. */
static const uint16_t k_fps_values[] = { 60u, 45u, 40u, 30u };
static const char *const k_fps_names[] = { "60", "45", "40", "30" };
static const uint16_t k_raster_values[] = { 960u, 720u, 480u };
static const char *const k_raster_names[] = { "960", "720", "480" };
static const uint8_t k_scale_values[] = { 1u, 2u };
static const char *const k_scale_names[] = { "1", "2" };
/* Index == stored value (0 off, 1 on). */
static const char *const k_onoff_names[] = { "off", "on" };

#define ISAAC_VITA_SETTINGS_ARRAY_COUNT(a) (sizeof (a) / sizeof (a)[0])

_Static_assert(ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_fps_values) ==
                   ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_fps_names),
               "fps value/name tables pair up");
_Static_assert(ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_raster_values) ==
                   ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_raster_names),
               "raster value/name tables pair up");
_Static_assert(ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_scale_values) ==
                   ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_scale_names),
               "render scale value/name tables pair up");
_Static_assert(ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_onoff_names) == 2u,
               "on/off table is indexed by the stored 0/1");
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
/* Index == stored value (enum isaac_vita_settings_debug_overlay). */
static const char *const k_debug_overlay_names[] = { "off", "compact", "full" };
_Static_assert(ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_debug_overlay_names) ==
                   (unsigned)ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_COUNT,
               "debug_overlay table is indexed by the stored value");
#endif

static const uint8_t k_default_buttons[ISAAC_VITA_SETTINGS_BUTTON_COUNT] = {
    ISAAC_VITA_SETTINGS_ELEMENT_A,    /* cross    */
    ISAAC_VITA_SETTINGS_ELEMENT_B,    /* circle   */
    ISAAC_VITA_SETTINGS_ELEMENT_X,    /* square   */
    ISAAC_VITA_SETTINGS_ELEMENT_Y,    /* triangle */
    ISAAC_VITA_SETTINGS_ELEMENT_LB,   /* l        */
    ISAAC_VITA_SETTINGS_ELEMENT_LT,   /* r        */
    ISAAC_VITA_SETTINGS_ELEMENT_RT,   /* select   */
};

static const uint8_t k_default_touch[ISAAC_VITA_SETTINGS_TOUCH_COUNT] = {
    ISAAC_VITA_SETTINGS_ELEMENT_LT,   /* top_left     */
    ISAAC_VITA_SETTINGS_ELEMENT_BACK, /* top_right    */
    ISAAC_VITA_SETTINGS_ELEMENT_RB,   /* bottom_right */
};

/* ---- locale-free ASCII classifiers ------------------------------------- */

static int is_lower(unsigned char c)
{
    return c >= (unsigned char)'a' && c <= (unsigned char)'z';
}

static int is_digit(unsigned char c)
{
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int is_blank(unsigned char c)
{
    return c == (unsigned char)' ' || c == (unsigned char)'\t';
}

static int is_key_char(unsigned char c)
{
    return is_lower(c) || is_digit(c) || c == (unsigned char)'_' ||
           c == (unsigned char)'.';
}

static int is_value_char(unsigned char c)
{
    return is_lower(c) || is_digit(c) || c == (unsigned char)'_' ||
           c == (unsigned char)'-';
}

/* Section 1.2 byte set: 0x09, 0x0A, 0x0D, 0x20-0x7E. */
static int is_allowed_byte(unsigned char c)
{
    return c == 0x09u || c == 0x0Au || c == 0x0Du ||
           (c >= 0x20u && c <= 0x7Eu);
}

static int text_equals(const char *text, size_t len, const char *name)
{
    size_t name_len = strlen(name);
    return len == name_len && memcmp(text, name, len) == 0;
}

static int valid_key_text(const char *text, size_t len)
{
    size_t i;
    if (len == 0u || len > ISAAC_VITA_SETTINGS_KEY_MAX) {
        return 0;
    }
    for (i = 0u; i < len; ++i) {
        if (!is_key_char((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

static int valid_value_text(const char *text, size_t len)
{
    size_t i;
    if (len == 0u || len > ISAAC_VITA_SETTINGS_VALUE_MAX) {
        return 0;
    }
    for (i = 0u; i < len; ++i) {
        if (!is_value_char((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

/* ---- public tables ----------------------------------------------------- */

const char *isaac_vita_settings_key_name(enum isaac_vita_settings_key key)
{
    if ((unsigned)key >= (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT) {
        return "";
    }
    return k_keys[key].name;
}

enum isaac_vita_settings_kind isaac_vita_settings_key_kind(
    enum isaac_vita_settings_key key)
{
    if ((unsigned)key >= (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT) {
        return ISAAC_VITA_SETTINGS_KIND_SCHEMA;
    }
    return k_keys[key].kind;
}

int isaac_vita_settings_key_from_text(const char *text, size_t len,
                                      enum isaac_vita_settings_key *key)
{
    unsigned i;
    for (i = 0u; i < (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT; ++i) {
        if (text_equals(text, len, k_keys[i].name)) {
            *key = (enum isaac_vita_settings_key)i;
            return 0;
        }
    }
    return -1;
}

const char *isaac_vita_settings_element_name(
    enum isaac_vita_settings_element element)
{
    if ((unsigned)element >= (unsigned)ISAAC_VITA_SETTINGS_ELEMENT_COUNT) {
        return "";
    }
    return k_elements[element];
}

int isaac_vita_settings_element_from_text(
    const char *text, size_t len, enum isaac_vita_settings_element *element)
{
    unsigned i;
    for (i = 0u; i < (unsigned)ISAAC_VITA_SETTINGS_ELEMENT_COUNT; ++i) {
        if (text_equals(text, len, k_elements[i])) {
            *element = (enum isaac_vita_settings_element)i;
            return 0;
        }
    }
    return -1;
}

int isaac_vita_settings_key_accepts_element(
    enum isaac_vita_settings_key key, enum isaac_vita_settings_element element)
{
    if ((unsigned)element >= (unsigned)ISAAC_VITA_SETTINGS_ELEMENT_COUNT) {
        return 0;
    }
    switch (isaac_vita_settings_key_kind(key)) {
    case ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT:
        return 1;
    case ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT:
        return element != ISAAC_VITA_SETTINGS_ELEMENT_RT;
    case ISAAC_VITA_SETTINGS_KIND_SCHEMA:
    case ISAAC_VITA_SETTINGS_KIND_LABEL:
    case ISAAC_VITA_SETTINGS_KIND_FPS_LOCK:
    case ISAAC_VITA_SETTINGS_KIND_RASTER:
    case ISAAC_VITA_SETTINGS_KIND_RENDER_SCALE:
    case ISAAC_VITA_SETTINGS_KIND_ONOFF:
    default:
        return 0;
    }
}

const char *isaac_vita_settings_status_name(
    enum isaac_vita_settings_status status)
{
    if ((unsigned)status >= (unsigned)ISAAC_VITA_SETTINGS_STATUS_COUNT) {
        return "";
    }
    return k_status[status];
}

uint8_t isaac_vita_settings_fps_lock_duty_k(uint16_t fps_lock)
{
    switch (fps_lock) {
    case 45u:
        return 2u;
    case 40u:
        return 3u;
    case 30u:
        return 0u;
    case 60u:
    default:
        return 1u;
    }
}

/* ---- struct helpers ---------------------------------------------------- */

void isaac_vita_settings_reset_remap(struct isaac_vita_settings *settings)
{
    memcpy(settings->button, k_default_buttons, sizeof settings->button);
    settings->chord_start_l = ISAAC_VITA_SETTINGS_ELEMENT_RB;
    settings->chord_start_r = ISAAC_VITA_SETTINGS_ELEMENT_BACK;
    settings->chord_restart_hold = 1u;
    memcpy(settings->touch, k_default_touch, sizeof settings->touch);
    settings->stick_swap = 0u;
}

void isaac_vita_settings_defaults(struct isaac_vita_settings *settings)
{
    memset(settings, 0, sizeof *settings);
    settings->fps_lock = 60u;
    settings->display_raster = 960u;
    settings->render_scale = 1u;
    isaac_vita_settings_reset_remap(settings);
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    settings->debug_overlay = (uint8_t)ISAAC_VITA_SETTINGS_DEBUG_OVERLAY_OFF;
#endif
    settings->preset[0] = '\0';
}

void isaac_vita_settings_receipt_clear(
    struct isaac_vita_settings_receipt *receipt)
{
    memset(receipt, 0, sizeof *receipt);
    receipt->status = (uint8_t)ISAAC_VITA_SETTINGS_STATUS_ABSENT;
}

int isaac_vita_settings_validate_remap(
    const struct isaac_vita_settings *settings)
{
    int has_a = 0;
    int has_b = 0;
    unsigned i;
    for (i = 0u; i < (unsigned)ISAAC_VITA_SETTINGS_BUTTON_COUNT; ++i) {
        if (settings->button[i] == (uint8_t)ISAAC_VITA_SETTINGS_ELEMENT_A) {
            has_a = 1;
        }
        if (settings->button[i] == (uint8_t)ISAAC_VITA_SETTINGS_ELEMENT_B) {
            has_b = 1;
        }
    }
    return has_a && has_b;
}

int isaac_vita_settings_equal(const struct isaac_vita_settings *a,
                              const struct isaac_vita_settings *b)
{
    return a->fps_lock == b->fps_lock &&
           a->display_raster == b->display_raster &&
           a->render_scale == b->render_scale &&
           memcmp(a->button, b->button, sizeof a->button) == 0 &&
           a->chord_start_l == b->chord_start_l &&
           a->chord_start_r == b->chord_start_r &&
           a->chord_restart_hold == b->chord_restart_hold &&
           memcmp(a->touch, b->touch, sizeof a->touch) == 0 &&
           a->stick_swap == b->stick_swap &&
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
           a->debug_overlay == b->debug_overlay &&
#endif
           strncmp(a->preset, b->preset, sizeof a->preset) == 0;
}

/* Byte offset inside struct isaac_vita_settings of the uint8_t a key owns
 * (element or on/off keys); 0 with *found = 0 for every other key. */
static size_t slot_offset(enum isaac_vita_settings_key key, int *found)
{
    *found = 1;
    switch (key) {
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS:
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_CIRCLE:
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_SQUARE:
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_TRIANGLE:
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_L:
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_R:
    case ISAAC_VITA_SETTINGS_KEY_BUTTON_SELECT:
        return offsetof(struct isaac_vita_settings, button) +
               ((size_t)key - (size_t)ISAAC_VITA_SETTINGS_KEY_BUTTON_CROSS);
    case ISAAC_VITA_SETTINGS_KEY_CHORD_START_L:
        return offsetof(struct isaac_vita_settings, chord_start_l);
    case ISAAC_VITA_SETTINGS_KEY_CHORD_START_R:
        return offsetof(struct isaac_vita_settings, chord_start_r);
    case ISAAC_VITA_SETTINGS_KEY_CHORD_RESTART_HOLD:
        return offsetof(struct isaac_vita_settings, chord_restart_hold);
    case ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_LEFT:
    case ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_RIGHT:
    case ISAAC_VITA_SETTINGS_KEY_TOUCH_BOTTOM_RIGHT:
        return offsetof(struct isaac_vita_settings, touch) +
               ((size_t)key - (size_t)ISAAC_VITA_SETTINGS_KEY_TOUCH_TOP_LEFT);
    case ISAAC_VITA_SETTINGS_KEY_STICK_SWAP:
        return offsetof(struct isaac_vita_settings, stick_swap);
    default:
        *found = 0;
        return 0u;
    }
}

static uint8_t *slot_mut(struct isaac_vita_settings *settings,
                         enum isaac_vita_settings_key key)
{
    int found;
    size_t offset = slot_offset(key, &found);
    return found ? (uint8_t *)settings + offset : NULL;
}

static const uint8_t *slot_const(const struct isaac_vita_settings *settings,
                                 enum isaac_vita_settings_key key)
{
    int found;
    size_t offset = slot_offset(key, &found);
    return found ? (const uint8_t *)settings + offset : NULL;
}

int isaac_vita_settings_set_text(struct isaac_vita_settings *settings,
                                 enum isaac_vita_settings_key key,
                                 const char *text, size_t len)
{
    unsigned i;
    uint8_t *slot;
    enum isaac_vita_settings_element element;

    if ((unsigned)key >= (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT ||
            !valid_value_text(text, len)) {
        return -1;
    }
    switch (k_keys[key].kind) {
    case ISAAC_VITA_SETTINGS_KIND_SCHEMA:
        return text_equals(text, len, "1") ? 0 : -1;
    case ISAAC_VITA_SETTINGS_KIND_LABEL:
        memcpy(settings->preset, text, len);
        settings->preset[len] = '\0';
        return 0;
    case ISAAC_VITA_SETTINGS_KIND_FPS_LOCK:
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_fps_names); ++i) {
            if (text_equals(text, len, k_fps_names[i])) {
                settings->fps_lock = k_fps_values[i];
                return 0;
            }
        }
        return -1;
    case ISAAC_VITA_SETTINGS_KIND_RASTER:
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_raster_names); ++i) {
            if (text_equals(text, len, k_raster_names[i])) {
                settings->display_raster = k_raster_values[i];
                return 0;
            }
        }
        return -1;
    case ISAAC_VITA_SETTINGS_KIND_RENDER_SCALE:
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_scale_names); ++i) {
            if (text_equals(text, len, k_scale_names[i])) {
                settings->render_scale = k_scale_values[i];
                return 0;
            }
        }
        return -1;
    case ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT:
    case ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT:
        if (isaac_vita_settings_element_from_text(text, len, &element) != 0 ||
                !isaac_vita_settings_key_accepts_element(key, element)) {
            return -1;
        }
        slot = slot_mut(settings, key);
        if (slot == NULL) {
            return -1;
        }
        *slot = (uint8_t)element;
        return 0;
    case ISAAC_VITA_SETTINGS_KIND_ONOFF:
        slot = slot_mut(settings, key);
        if (slot == NULL) {
            return -1;
        }
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_onoff_names); ++i) {
            if (text_equals(text, len, k_onoff_names[i])) {
                *slot = (uint8_t)i;
                return 0;
            }
        }
        return -1;
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    case ISAAC_VITA_SETTINGS_KIND_DEBUG_OVERLAY:
        for (i = 0u;
             i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_debug_overlay_names); ++i) {
            if (text_equals(text, len, k_debug_overlay_names[i])) {
                settings->debug_overlay = (uint8_t)i;
                return 0;
            }
        }
        return -1;
#endif
    default:
        return -1;
    }
}

/* Length of the preset label without touching any byte past the field:
 * 0 = absent (preset[0] == NUL), 1..VALUE_MAX = valid label, LABEL_INVALID
 * = no NUL inside the field or a label outside the value grammar.  The
 * struct is filled directly by callers (memcpy in the oracle, the manager's
 * editor), so the terminator is checked here rather than assumed. */
#define LABEL_INVALID ((size_t)-1)

static size_t label_len(const struct isaac_vita_settings *settings)
{
    size_t n = 0u;
    while (n < sizeof settings->preset && settings->preset[n] != '\0') {
        ++n;
    }
    if (n == 0u) {
        return 0u;
    }
    if (n >= sizeof settings->preset ||
            !valid_value_text(settings->preset, n)) {
        return LABEL_INVALID;
    }
    return n;
}

/* Canonical text of a key; NULL when the key has no text (schema, absent or
 * invalid preset, out-of-range codes). */
static const char *field_text(const struct isaac_vita_settings *settings,
                              enum isaac_vita_settings_key key)
{
    unsigned i;
    size_t n;
    const uint8_t *slot;

    if ((unsigned)key >= (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT) {
        return NULL;
    }
    switch (k_keys[key].kind) {
    case ISAAC_VITA_SETTINGS_KIND_SCHEMA:
        return NULL;
    case ISAAC_VITA_SETTINGS_KIND_LABEL:
        n = label_len(settings);
        return (n != 0u && n != LABEL_INVALID) ? settings->preset : NULL;
    case ISAAC_VITA_SETTINGS_KIND_FPS_LOCK:
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_fps_values); ++i) {
            if (settings->fps_lock == k_fps_values[i]) {
                return k_fps_names[i];
            }
        }
        return NULL;
    case ISAAC_VITA_SETTINGS_KIND_RASTER:
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_raster_values); ++i) {
            if (settings->display_raster == k_raster_values[i]) {
                return k_raster_names[i];
            }
        }
        return NULL;
    case ISAAC_VITA_SETTINGS_KIND_RENDER_SCALE:
        for (i = 0u; i < ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_scale_values); ++i) {
            if (settings->render_scale == k_scale_values[i]) {
                return k_scale_names[i];
            }
        }
        return NULL;
    case ISAAC_VITA_SETTINGS_KIND_PHYS_ELEMENT:
    case ISAAC_VITA_SETTINGS_KIND_SYNTH_ELEMENT:
        slot = slot_const(settings, key);
        if (slot == NULL ||
                *slot >= (uint8_t)ISAAC_VITA_SETTINGS_ELEMENT_COUNT ||
                !isaac_vita_settings_key_accepts_element(
                    key, (enum isaac_vita_settings_element)*slot)) {
            return NULL;
        }
        return k_elements[*slot];
    case ISAAC_VITA_SETTINGS_KIND_ONOFF:
        slot = slot_const(settings, key);
        if (slot == NULL || *slot > 1u) {
            return NULL;
        }
        return k_onoff_names[*slot];
#if defined(ISAAC_VITA_DEBUG_OVERLAY)
    case ISAAC_VITA_SETTINGS_KIND_DEBUG_OVERLAY:
        if (settings->debug_overlay >=
                ISAAC_VITA_SETTINGS_ARRAY_COUNT(k_debug_overlay_names)) {
            return NULL;
        }
        return k_debug_overlay_names[settings->debug_overlay];
#endif
    default:
        return NULL;
    }
}

size_t isaac_vita_settings_get_text(const struct isaac_vita_settings *settings,
                                    enum isaac_vita_settings_key key,
                                    char *out, size_t cap)
{
    const char *text;
    size_t len;

    if (cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    text = field_text(settings, key);
    if (text == NULL) {
        return 0u;
    }
    len = strlen(text);
    if (len + 1u > cap) {
        return 0u;
    }
    memcpy(out, text, len + 1u);
    return len;
}

/* Append text to out[*pos..cap); returns 0 on overflow. */
static int emit(char *out, size_t cap, size_t *pos, const char *text,
                size_t len)
{
    if (len > cap || *pos > cap - len) {
        return 0;
    }
    memcpy(out + *pos, text, len);
    *pos += len;
    return 1;
}

size_t isaac_vita_settings_serialize(const struct isaac_vita_settings *settings,
                                     char *out, size_t cap)
{
    size_t pos = 0u;
    unsigned i;

    if (cap == 0u) {
        return 0u;
    }
    if (!emit(out, cap, &pos, "schema=1\n", 9u)) {
        out[0] = '\0';
        return 0u;
    }
    for (i = 1u; i < (unsigned)ISAAC_VITA_SETTINGS_KEY_COUNT; ++i) {
        enum isaac_vita_settings_key key = (enum isaac_vita_settings_key)i;
        const char *value = field_text(settings, key);
        if (value == NULL) {
            /* Absent preset is not written; any other NULL (including an
             * unterminated or ungrammatical label) means a field outside its
             * allowed set and the struct is not serializable. */
            if (k_keys[i].kind == ISAAC_VITA_SETTINGS_KIND_LABEL &&
                    label_len(settings) == 0u) {
                continue;
            }
            out[0] = '\0';
            return 0u;
        }
        if (!emit(out, cap, &pos, k_keys[i].name, strlen(k_keys[i].name)) ||
                !emit(out, cap, &pos, "=", 1u) ||
                !emit(out, cap, &pos, value, strlen(value)) ||
                !emit(out, cap, &pos, "\n", 1u)) {
            out[0] = '\0';
            return 0u;
        }
    }
    if (pos >= cap) {
        out[0] = '\0';
        return 0u;
    }
    out[pos] = '\0';
    return pos;
}

/* ---- parser ------------------------------------------------------------ */

struct parse_state {
    struct isaac_vita_settings parsed;
    struct isaac_vita_settings_receipt receipt;
    uint8_t seen[ISAAC_VITA_SETTINGS_KEY_COUNT];
    int schema_ok;
};

static void note_rejected(struct parse_state *st, uint32_t line_no)
{
    if (st->receipt.rejected < ISAAC_VITA_SETTINGS_RECEIPT_SLOTS) {
        st->receipt.rejected_lines[st->receipt.rejected] = line_no;
    }
    ++st->receipt.rejected;
}

static void note_unknown(struct parse_state *st, const char *key, size_t len)
{
    if (st->receipt.unknown < ISAAC_VITA_SETTINGS_RECEIPT_SLOTS) {
        char *slot = st->receipt.unknown_keys[st->receipt.unknown];
        size_t copy = len < ISAAC_VITA_SETTINGS_VALUE_MAX
                          ? len : ISAAC_VITA_SETTINGS_VALUE_MAX;
        memcpy(slot, key, copy);
        slot[copy] = '\0';
    }
    ++st->receipt.unknown;
}

/* One physical line, already split on LF; line[0..len) may hold a trailing
 * CR.  Applies the section 1.2 rules and updates the state. */
static void parse_line(struct parse_state *st, const char *line, size_t len,
                       uint32_t line_no)
{
    size_t i;
    size_t eq = 0u;
    int has_eq = 0;
    const char *key;
    size_t key_len;
    const char *value;
    size_t value_len;
    enum isaac_vita_settings_key code;

    /* Byte set first: any byte outside it rejects the whole line. */
    for (i = 0u; i < len; ++i) {
        if (!is_allowed_byte((unsigned char)line[i])) {
            note_rejected(st, line_no);
            return;
        }
    }
    /* One trailing CR (CRLF files). */
    if (len > 0u && line[len - 1u] == '\r') {
        --len;
    }
    /* Trim blanks. */
    while (len > 0u && is_blank((unsigned char)line[0])) {
        ++line;
        --len;
    }
    while (len > 0u && is_blank((unsigned char)line[len - 1u])) {
        --len;
    }
    if (len == 0u || line[0] == '#' || line[0] == ';') {
        return;
    }
    if (line[0] == '[') {
        if (line[len - 1u] == ']') {
            return;
        }
        note_rejected(st, line_no);
        return;
    }
    for (i = 0u; i < len; ++i) {
        if (line[i] == '=') {
            eq = i;
            has_eq = 1;
            break;
        }
    }
    if (!has_eq) {
        note_rejected(st, line_no);
        return;
    }
    key = line;
    key_len = eq;
    value = line + eq + 1u;
    value_len = len - eq - 1u;
    if (!valid_key_text(key, key_len) || !valid_value_text(value, value_len)) {
        note_rejected(st, line_no);
        return;
    }
    if (isaac_vita_settings_key_from_text(key, key_len, &code) != 0) {
        note_unknown(st, key, key_len);
        return;
    }
    /* A rejected assignment never changes state: the earlier value (or the
     * default) stands, the line is counted as rejected only, and a later
     * "schema=2" does not undo an accepted "schema=1". */
    if (isaac_vita_settings_set_text(&st->parsed, code, value, value_len) != 0) {
        note_rejected(st, line_no);
        return;
    }
    if (code == ISAAC_VITA_SETTINGS_KEY_SCHEMA) {
        st->schema_ok = 1;
    }
    if (st->seen[code]) {
        ++st->receipt.dup;
    }
    st->seen[code] = 1u;
    st->receipt.assigned |= 1u << (unsigned)code;
}

/* size_t -> uint32_t saturating; written without a constant comparison so a
 * 32-bit size_t (arm-vita-eabi) raises no -Wtype-limits under -Werror. */
static uint32_t clamp_u32(size_t value)
{
    uint32_t narrow = (uint32_t)value;
    if ((size_t)narrow != value) {
        narrow = 0xFFFFFFFFu;
    }
    return narrow;
}

enum isaac_vita_settings_status isaac_vita_settings_parse(
    const char *buf, size_t len, struct isaac_vita_settings *settings,
    struct isaac_vita_settings_receipt *receipt)
{
    struct parse_state st;
    size_t start = 0u;
    size_t i;
    uint32_t line_no = 0u;
    enum isaac_vita_settings_status status;

    isaac_vita_settings_defaults(settings);
    memset(&st, 0, sizeof st);
    isaac_vita_settings_defaults(&st.parsed);
    isaac_vita_settings_receipt_clear(&st.receipt);
    st.receipt.bytes = clamp_u32(len);

    if (len > (size_t)ISAAC_VITA_SETTINGS_MAX_BYTES) {
        status = ISAAC_VITA_SETTINGS_STATUS_TOO_LARGE;
    } else {
        status = ISAAC_VITA_SETTINGS_STATUS_SCHEMA;
        for (i = 0u; i < len; ++i) {
            if (buf[i] == '\0') {
                status = ISAAC_VITA_SETTINGS_STATUS_GARBAGE;
                break;
            }
        }
    }
    if (status == ISAAC_VITA_SETTINGS_STATUS_SCHEMA) {
        for (i = 0u; i < len; ++i) {
            if (buf[i] == '\n') {
                ++line_no;
                parse_line(&st, buf + start, i - start, line_no);
                start = i + 1u;
            }
        }
        if (start < len) {
            ++line_no;
            parse_line(&st, buf + start, len - start, line_no);
        }
        st.receipt.lines = line_no;
        if (st.schema_ok) {
            status = ISAAC_VITA_SETTINGS_STATUS_OK;
            *settings = st.parsed;
            st.receipt.remap_guard = (uint8_t)(
                isaac_vita_settings_validate_remap(settings) ? 0 : 1);
        } else {
            /* The file is ignored: nothing in *settings came from it. */
            st.receipt.assigned = 0u;
        }
    }
    st.receipt.status = (uint8_t)status;
    if (receipt != NULL) {
        *receipt = st.receipt;
    }
    return status;
}
