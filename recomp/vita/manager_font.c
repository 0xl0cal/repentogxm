/*
 * Manager UI2 text.  See manager_font.h for the contract.  No SDK, no
 * allocator, no floating point; every multi-byte field of an IMF1 file is
 * read byte by byte (little-endian), so no alignment is assumed.
 */
#include "manager_font.h"

#if defined(MANAGER_UI2)

#include <string.h>

/* ---- CRC-32 (IEEE 802.3, the zlib.crc32 the baker writes) --------------- */

uint32_t manager_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    size_t index;
    unsigned int bit;
    if (bytes == NULL)
        return 0u;
    for (index = 0u; index < size; ++index) {
        crc ^= bytes[index];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xffffffffu;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- IMF1 ------------------------------------------------------------------ */

int manager_imf_parse(struct manager_imf *font, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t table_bytes;
    uint32_t previous = 0u;
    uint16_t flags;
    uint16_t reserved;
    unsigned int index;
    int question = -1;

    if (font == NULL || bytes == NULL)
        return MANAGER_IMF_NULL;
    memset(font, 0, sizeof(*font));
    if (size < MANAGER_IMF_HEADER_BYTES)
        return MANAGER_IMF_TRUNCATED;
    if (memcmp(bytes, "IMF1", 4u) != 0)
        return MANAGER_IMF_BAD_MAGIC;
    font->px = read_u16(bytes + 4);
    font->line_h = read_u16(bytes + 6);
    font->ascent = read_u16(bytes + 8);
    font->atlas_w = read_u16(bytes + 10);
    font->atlas_h = read_u16(bytes + 12);
    font->glyph_count = read_u16(bytes + 14);
    flags = read_u16(bytes + 16);
    reserved = read_u16(bytes + 18);
    font->atlas_bytes = read_u32(bytes + 20);
    if (flags != 0u || reserved != 0u)
        return MANAGER_IMF_BAD_FLAGS; /* A4 is not produced by the baker */
    table_bytes = (uint32_t)font->glyph_count * MANAGER_IMF_GLYPH_BYTES;
    if (font->glyph_count == 0u || font->line_h == 0u ||
        font->ascent > font->line_h ||
        font->atlas_bytes != (uint32_t)font->atlas_w * font->atlas_h ||
        size != (size_t)MANAGER_IMF_HEADER_BYTES + table_bytes +
                    font->atlas_bytes)
        return MANAGER_IMF_BAD_SIZE;
    if (manager_crc32(bytes + MANAGER_IMF_HEADER_BYTES,
                      table_bytes + font->atlas_bytes) != read_u32(bytes + 24))
        return MANAGER_IMF_BAD_CRC;
    font->table = bytes + MANAGER_IMF_HEADER_BYTES;
    font->atlas = font->table + table_bytes;
    for (index = 0u; index < font->glyph_count; ++index) {
        const uint8_t *g = font->table + index * MANAGER_IMF_GLYPH_BYTES;
        const uint32_t codepoint = read_u32(g);
        const unsigned int x = read_u16(g + 4);
        const unsigned int y = read_u16(g + 6);
        const unsigned int w = g[8];
        const unsigned int h = g[9];
        if (index != 0u && codepoint <= previous)
            return MANAGER_IMF_UNSORTED;
        previous = codepoint;
        if (w != 0u && h != 0u &&
            (x + w > font->atlas_w || y + h > font->atlas_h))
            return MANAGER_IMF_BAD_GLYPH;
        if (codepoint == 0x3fu)
            question = (int)index;
    }
    if (question < 0)
        return MANAGER_IMF_NO_QUESTION;
    font->question = (uint16_t)question;
    return MANAGER_IMF_OK;
}

int manager_imf_find(const struct manager_imf *font, uint32_t codepoint)
{
    int low = 0;
    int high;
    if (font == NULL || font->table == NULL)
        return -1;
    high = (int)font->glyph_count - 1;
    while (low <= high) {
        const int middle = low + (high - low) / 2;
        const uint32_t value =
            read_u32(font->table + (unsigned int)middle * MANAGER_IMF_GLYPH_BYTES);
        if (value == codepoint)
            return middle;
        if (value < codepoint)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return -1;
}

static const uint8_t *imf_glyph(const struct manager_imf *font,
                                uint32_t codepoint)
{
    int index = manager_imf_find(font, codepoint);
    if (index < 0)
        index = font->question;
    return font->table + (unsigned int)index * MANAGER_IMF_GLYPH_BYTES;
}

static int imf_line_height(const void *impl)
{
    return ((const struct manager_imf *)impl)->line_h;
}

static int imf_ascent(const void *impl)
{
    return ((const struct manager_imf *)impl)->ascent;
}

static int imf_has_glyph(const void *impl, uint32_t codepoint)
{
    return manager_imf_find((const struct manager_imf *)impl, codepoint) >= 0;
}

static int imf_advance(const void *impl, uint32_t codepoint)
{
    return imf_glyph((const struct manager_imf *)impl, codepoint)[12];
}

static int imf_draw_glyph(const void *impl, struct manager_canvas *canvas,
                          int x, int y, uint32_t codepoint, uint32_t color)
{
    const struct manager_imf *font = (const struct manager_imf *)impl;
    const uint8_t *g = imf_glyph(font, codepoint);
    const int w = g[8];
    const int h = g[9];
    if (canvas != NULL && w != 0 && h != 0) {
        const unsigned int gx = read_u16(g + 4);
        const unsigned int gy = read_u16(g + 6);
        manager_canvas_blit_a8(canvas, x + (int8_t)g[10], y + (int8_t)g[11],
                               font->atlas + gy * font->atlas_w + gx, w, h,
                               font->atlas_w, color);
    }
    return g[12];
}

static const struct manager_face_ops k_imf_ops = {
    imf_line_height, imf_ascent, imf_has_glyph, imf_advance, imf_draw_glyph,
};

struct manager_face manager_imf_face(const struct manager_imf *font)
{
    struct manager_face face;
    face.ops = &k_imf_ops;
    face.impl = font;
    return face;
}

/* ---- 5x7 fallback (tables copied verbatim from manager_main.c) ----------- */

static const uint8_t manager_letters[26][7] = {
    {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* A */
    {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, /* B */
    {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, /* C */
    {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, /* D */
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, /* E */
    {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, /* F */
    {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, /* G */
    {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* H */
    {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, /* L */
    {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
    {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* O */
    {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, /* P */
    {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, /* Q */
    {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, /* R */
    {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, /* S */
    {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, /* W */
    {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, /* Z */
};

static const uint8_t manager_digits[10][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, /* 0 */
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}, /* 1 */
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, /* 2 */
    {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}, /* 3 */
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, /* 4 */
    {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}, /* 5 */
    {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}, /* 6 */
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, /* 8 */
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}, /* 9 */
};

static uint8_t manager_glyph_row(char character, unsigned int row)
{
    if (character >= 'A' && character <= 'Z')
        return manager_letters[(unsigned int)(character - 'A')][row];
    if (character >= '0' && character <= '9')
        return manager_digits[(unsigned int)(character - '0')][row];
    if (character == ':') {
        static const uint8_t colon[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        return colon[row];
    }
    if (character == '.') {
        static const uint8_t period[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};
        return period[row];
    }
    if (character == '-') {
        static const uint8_t dash[7] = {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
        return dash[row];
    }
    if (character == '_') {
        static const uint8_t underscore[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f};
        return underscore[row];
    }
    if (character == '>') {
        static const uint8_t arrow[7] = {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10};
        return arrow[row];
    }
    if (character == '<') {
        static const uint8_t arrow[7] = {0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01};
        return arrow[row];
    }
    if (character == '?') {
        static const uint8_t question[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
        return question[row];
    }
    return 0;
}

static unsigned int builtin_scale(const void *impl)
{
    const unsigned int scale = ((const struct manager_builtin_font *)impl)->scale;
    return scale == 0u ? 1u : (scale > 4u ? 4u : scale);
}

/* Lower case folds to upper case; the rest of the table's set draws
 * itself; the UI2 separators and button glyphs map to the nearest
 * character the table has (the table itself stays verbatim); anything else
 * is '?' (space is blank).  U+2026 is left unmapped so clipped text keeps
 * its three-dot ellipsis. */
static char builtin_char(uint32_t codepoint)
{
    if (codepoint >= 'a' && codepoint <= 'z')
        return (char)(codepoint - 'a' + 'A');
    switch (codepoint) {
    case 0x00b7u: /* · */
    case 0x2013u: /* – */
    case 0x2014u: /* — */
        return '-';
    case 0x00d7u: /* × */
        return 'X';
    case 0x25cbu: /* ○ */
        return 'O';
    case 0x203au: /* › */
    case 0x25b8u: /* ▸ */
    case 0x2192u: /* → */
    case 0x00bbu: /* » */
        return '>';
    case 0x25c2u: /* ◂ */
    case 0x2190u: /* ← */
    case 0x00abu: /* « */
        return '<';
    default:
        break;
    }
    if ((codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= '0' && codepoint <= '9') || codepoint == ' ' ||
        codepoint == ':' || codepoint == '.' || codepoint == '-' ||
        codepoint == '_' || codepoint == '>' || codepoint == '<' ||
        codepoint == '?')
        return (char)codepoint;
    return '?';
}

static int builtin_line_height(const void *impl)
{
    return (int)(8u * builtin_scale(impl));
}

static int builtin_ascent(const void *impl)
{
    return (int)(7u * builtin_scale(impl));
}

static int builtin_has_glyph(const void *impl, uint32_t codepoint)
{
    (void)impl;
    return codepoint == '?' || builtin_char(codepoint) != '?';
}

static int builtin_advance(const void *impl, uint32_t codepoint)
{
    (void)codepoint;
    return (int)(6u * builtin_scale(impl));
}

static int builtin_draw_glyph(const void *impl, struct manager_canvas *canvas,
                              int x, int y, uint32_t codepoint, uint32_t color)
{
    const unsigned int scale = builtin_scale(impl);
    const char character = builtin_char(codepoint);
    unsigned int row;
    unsigned int column;
    if (canvas != NULL && character != ' ') {
        for (row = 0u; row < 7u; ++row) {
            const uint8_t bits = manager_glyph_row(character, row);
            for (column = 0u; column < 5u; ++column) {
                if ((bits & (uint8_t)(0x10u >> column)) != 0u) {
                    const int px = x + (int)(column * scale);
                    const int py = y + (int)(row * scale);
                    manager_canvas_fill_box(canvas, px, py,
                                            px + (int)scale - 1,
                                            py + (int)scale - 1, color);
                }
            }
        }
    }
    return (int)(6u * scale);
}

static const struct manager_face_ops k_builtin_ops = {
    builtin_line_height, builtin_ascent, builtin_has_glyph, builtin_advance,
    builtin_draw_glyph,
};

struct manager_face manager_builtin_face(const struct manager_builtin_font *font)
{
    struct manager_face face;
    face.ops = &k_builtin_ops;
    face.impl = font;
    return face;
}

int manager_face_valid(struct manager_face face)
{
    return face.ops != NULL && face.impl != NULL;
}

/* ---- UTF-8 ------------------------------------------------------------------ */

uint32_t manager_utf8_next(const char **cursor)
{
    const unsigned char *p;
    uint32_t value;
    unsigned int extra;
    unsigned int index;
    uint32_t minimum;
    if (cursor == NULL || *cursor == NULL)
        return 0u;
    p = (const unsigned char *)*cursor;
    if (p[0] == 0u)
        return 0u;
    if (p[0] < 0x80u) {
        *cursor += 1;
        return p[0];
    }
    if ((p[0] & 0xe0u) == 0xc0u) {
        value = p[0] & 0x1fu;
        extra = 1u;
        minimum = 0x80u;
    } else if ((p[0] & 0xf0u) == 0xe0u) {
        value = p[0] & 0x0fu;
        extra = 2u;
        minimum = 0x800u;
    } else if ((p[0] & 0xf8u) == 0xf0u) {
        value = p[0] & 0x07u;
        extra = 3u;
        minimum = 0x10000u;
    } else {
        *cursor += 1;
        return MANAGER_UTF8_REPLACEMENT;
    }
    for (index = 1u; index <= extra; ++index) {
        if ((p[index] & 0xc0u) != 0x80u) {
            *cursor += 1;
            return MANAGER_UTF8_REPLACEMENT;
        }
        value = (value << 6) | (p[index] & 0x3fu);
    }
    if (value < minimum || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu)) {
        *cursor += 1;
        return MANAGER_UTF8_REPLACEMENT;
    }
    *cursor += 1 + extra;
    return value;
}

size_t manager_utf8_length(const char *text)
{
    size_t count = 0u;
    while (text != NULL && manager_utf8_next(&text) != 0u)
        ++count;
    return count;
}

/* ---- strings ------------------------------------------------------------- */

int manager_face_line_height(struct manager_face face)
{
    return manager_face_valid(face) ? face.ops->line_height(face.impl) : 0;
}

int manager_face_ascent(struct manager_face face)
{
    return manager_face_valid(face) ? face.ops->ascent(face.impl) : 0;
}

int manager_face_measure(struct manager_face face, const char *text)
{
    int widest = 0;
    int width = 0;
    uint32_t codepoint;
    if (!manager_face_valid(face) || text == NULL)
        return 0;
    while ((codepoint = manager_utf8_next(&text)) != 0u) {
        if (codepoint == '\n') {
            if (width > widest)
                widest = width;
            width = 0;
            continue;
        }
        width += face.ops->advance(face.impl, codepoint);
    }
    return width > widest ? width : widest;
}

int manager_face_draw(struct manager_face face, struct manager_canvas *canvas,
                      int x, int y, uint32_t color, const char *text)
{
    const int origin = x;
    int widest = 0;
    uint32_t codepoint;
    if (!manager_face_valid(face) || text == NULL)
        return 0;
    while ((codepoint = manager_utf8_next(&text)) != 0u) {
        if (codepoint == '\n') {
            if (x - origin > widest)
                widest = x - origin;
            x = origin;
            y += face.ops->line_height(face.impl);
            continue;
        }
        x += face.ops->draw_glyph(face.impl, canvas, x, y, codepoint, color);
    }
    return x - origin > widest ? x - origin : widest;
}

int manager_face_fits(struct manager_face face, const char *text,
                      int max_width)
{
    return manager_face_measure(face, text) <= max_width;
}

int manager_face_draw_clipped(struct manager_face face,
                              struct manager_canvas *canvas, int x, int y,
                              uint32_t color, const char *text,
                              int max_width)
{
    const char *cursor = text;
    int ellipsis_width;
    int width = 0;
    uint32_t codepoint;
    int has_ellipsis;
    if (!manager_face_valid(face) || text == NULL)
        return 0;
    if (max_width <= 0 || manager_face_fits(face, text, max_width))
        return manager_face_draw(face, canvas, x, y, color, text);
    has_ellipsis = face.ops->has_glyph(face.impl, MANAGER_ELLIPSIS);
    ellipsis_width = has_ellipsis
                         ? face.ops->advance(face.impl, MANAGER_ELLIPSIS)
                         : 3 * face.ops->advance(face.impl, '.');
    /* Draw whole code points while they and the ellipsis still fit. */
    for (;;) {
        const char *before = cursor;
        int advance;
        codepoint = manager_utf8_next(&cursor);
        if (codepoint == 0u || codepoint == '\n')
            break;
        advance = face.ops->advance(face.impl, codepoint);
        if (width + advance + ellipsis_width > max_width) {
            cursor = before;
            break;
        }
        width += face.ops->draw_glyph(face.impl, canvas, x + width, y,
                                      codepoint, color);
    }
    if (has_ellipsis) {
        width += face.ops->draw_glyph(face.impl, canvas, x + width, y,
                                      MANAGER_ELLIPSIS, color);
    } else {
        unsigned int dot;
        for (dot = 0u; dot < 3u; ++dot)
            width += face.ops->draw_glyph(face.impl, canvas, x + width, y,
                                          '.', color);
    }
    return width;
}

#endif /* MANAGER_UI2 */
