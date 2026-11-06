/*
 * BMFont v3 reader and A8 renderer for the manager's game skin; see
 * manager_bmfont.h.  Block and row layouts are copied from
 * the design's asset probe and mock-up renderer (Python, not shipped)
 * (BMFont: "<IHHHHhhhBB" rows of 20 bytes, "<HH" line height/base).
 */
#include "manager_bmfont.h"

#include <stdlib.h>
#include <string.h>

#define BMFONT_ROW_BYTES 20u
#define BMFONT_ALL_CHANNELS 15u

static uint32_t bmfont_le16(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

static uint32_t bmfont_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int bmfont_signature_ok(const uint8_t *fnt, size_t size)
{
    return size >= 4u && fnt[0] == 'B' && fnt[1] == 'M' && fnt[2] == 'F' &&
           fnt[3] == 3u;
}

static int bmfont_compare(const void *left, const void *right)
{
    uint32_t a = ((const struct manager_bmfont_glyph *)left)->codepoint;
    uint32_t b = ((const struct manager_bmfont_glyph *)right)->codepoint;
    return a < b ? -1 : (a > b ? 1 : 0);
}

uint32_t manager_bmfont_glyph_rows(const uint8_t *fnt, size_t size)
{
    size_t pos = 4u;

    if (fnt == NULL || !bmfont_signature_ok(fnt, size))
        return 0u;
    while (pos < size) {
        uint32_t block_size;
        if (size - pos < 5u)
            return 0u;
        block_size = bmfont_le32(fnt + pos + 1);
        if (block_size > size - pos - 5u)
            return 0u;
        if (fnt[pos] == 4u)
            return block_size % BMFONT_ROW_BYTES == 0u
                       ? block_size / BMFONT_ROW_BYTES : 0u;
        pos += 5u + (size_t)block_size;
    }
    return 0u;
}

int manager_bmfont_parse(struct manager_bmfont *font, const uint8_t *fnt,
                         size_t size, struct manager_bmfont_glyph *glyphs,
                         uint32_t capacity)
{
    size_t pos = 4u;
    uint32_t seen = 0u;
    uint32_t count = 0u;
    uint32_t i;

    if (font == NULL || fnt == NULL || (glyphs == NULL && capacity != 0u))
        return MANAGER_BMFONT_E_ARGUMENT;
    memset(font, 0, sizeof(*font));
    if (!bmfont_signature_ok(fnt, size))
        return MANAGER_BMFONT_E_SIGNATURE;

    while (pos < size) {
        const uint8_t *block;
        uint32_t type;
        uint32_t block_size;

        if (size - pos < 5u)
            return MANAGER_BMFONT_E_BLOCK;
        type = fnt[pos];
        block_size = bmfont_le32(fnt + pos + 1);
        if (block_size > size - pos - 5u)
            return MANAGER_BMFONT_E_BLOCK;
        block = fnt + pos + 5;
        if (type < 1u || type > 5u || (seen & (1u << type)) != 0u)
            return MANAGER_BMFONT_E_BLOCK;
        seen |= 1u << type;
        switch (type) {
        case 1u:
            if (block_size < 2u)
                return MANAGER_BMFONT_E_BLOCK;
            font->font_size = (int16_t)bmfont_le16(block);
            break;
        case 2u:
            if (block_size < 10u)
                return MANAGER_BMFONT_E_BLOCK;
            font->line_height = (uint16_t)bmfont_le16(block);
            font->base = (uint16_t)bmfont_le16(block + 2);
            font->scale_w = (uint16_t)bmfont_le16(block + 4);
            font->scale_h = (uint16_t)bmfont_le16(block + 6);
            font->pages = (uint16_t)bmfont_le16(block + 8);
            break;
        case 3u: {
            const uint8_t *nul = (const uint8_t *)memchr(block, 0, block_size);
            size_t length = nul == NULL ? 0u : (size_t)(nul - block);
            if (nul == NULL || length == 0u ||
                length >= MANAGER_BMFONT_PAGE_NAME_CAPACITY)
                return MANAGER_BMFONT_E_BLOCK;
            memcpy(font->page_name, block, length);
            font->page_name[length] = '\0';
            break;
        }
        case 4u:
            if (block_size % BMFONT_ROW_BYTES != 0u)
                return MANAGER_BMFONT_E_BLOCK;
            count = block_size / BMFONT_ROW_BYTES;
            if (count > capacity)
                return MANAGER_BMFONT_E_CAPACITY;
            for (i = 0u; i < count; ++i) {
                const uint8_t *row = block + (size_t)i * BMFONT_ROW_BYTES;
                struct manager_bmfont_glyph *glyph = &glyphs[i];
                glyph->codepoint = bmfont_le32(row);
                glyph->x = (uint16_t)bmfont_le16(row + 4);
                glyph->y = (uint16_t)bmfont_le16(row + 6);
                glyph->width = (uint16_t)bmfont_le16(row + 8);
                glyph->height = (uint16_t)bmfont_le16(row + 10);
                glyph->xoffset = (int16_t)bmfont_le16(row + 12);
                glyph->yoffset = (int16_t)bmfont_le16(row + 14);
                glyph->xadvance = (int16_t)bmfont_le16(row + 16);
                glyph->page = row[18];
                glyph->channel = row[19];
            }
            break;
        default:
            /* 5: kerning pairs, not used by the manager. */
            break;
        }
        pos += 5u + (size_t)block_size;
    }
    if ((seen & 0x1Eu) != 0x1Eu)
        return MANAGER_BMFONT_E_BLOCK;
    if (font->pages != 1u)
        return MANAGER_BMFONT_E_UNSUPPORTED;
    if (font->scale_w == 0u || font->scale_h == 0u)
        return MANAGER_BMFONT_E_GLYPH;
    for (i = 0u; i < count; ++i) {
        const struct manager_bmfont_glyph *glyph = &glyphs[i];
        if (glyph->page != 0u || glyph->channel != BMFONT_ALL_CHANNELS)
            return MANAGER_BMFONT_E_UNSUPPORTED;
        if ((uint32_t)glyph->x + glyph->width > font->scale_w ||
            (uint32_t)glyph->y + glyph->height > font->scale_h)
            return MANAGER_BMFONT_E_GLYPH;
    }
    if (count > 1u)
        qsort(glyphs, count, sizeof(glyphs[0]), bmfont_compare);
    for (i = 1u; i < count; ++i) {
        if (glyphs[i].codepoint == glyphs[i - 1u].codepoint)
            return MANAGER_BMFONT_E_GLYPH;
    }
    font->glyphs = glyphs;
    font->glyph_count = count;
    font->fallback = manager_bmfont_find(font, '?');
    return MANAGER_BMFONT_OK;
}

void manager_bmfont_rgba_to_a8(uint8_t *a8, const uint8_t *rgba,
                               uint32_t pixels, uint32_t *non_white)
{
    uint32_t lost = 0u;
    uint32_t i;

    if (a8 == NULL || rgba == NULL)
        return;
    /* In place is safe: a8[i] is written after rgba[4i .. 4i+3] was read
     * and 4i >= i. */
    for (i = 0u; i < pixels; ++i) {
        const uint8_t *texel = rgba + (size_t)i * 4u;
        uint8_t alpha = texel[3];
        if (alpha != 0u &&
            (texel[0] != 0xFFu || texel[1] != 0xFFu || texel[2] != 0xFFu))
            ++lost;
        a8[i] = alpha;
    }
    if (non_white != NULL)
        *non_white = lost;
}

int manager_bmfont_attach_page(struct manager_bmfont *font, const uint8_t *a8,
                               uint32_t width, uint32_t height)
{
    if (font == NULL || a8 == NULL || font->glyphs == NULL)
        return MANAGER_BMFONT_E_ARGUMENT;
    if (width != font->scale_w || height != font->scale_h)
        return MANAGER_BMFONT_E_PAGE;
    font->page = a8;
    return MANAGER_BMFONT_OK;
}

uint32_t manager_bmfont_utf8_next(const char **cursor, const char *end)
{
    const unsigned char *bytes;
    uint32_t lead;
    uint32_t codepoint;
    uint32_t minimum;
    uint32_t need;
    uint32_t i;

    if (cursor == NULL || *cursor == NULL)
        return 0u;
    bytes = (const unsigned char *)*cursor;
    if (end != NULL ? *cursor >= end : bytes[0] == 0u)
        return 0u;
    lead = bytes[0];
    if (lead < 0x80u) {
        *cursor += 1;
        return lead;
    }
    if (lead >= 0xC2u && lead <= 0xDFu) {
        need = 1u;
        codepoint = lead & 0x1Fu;
        minimum = 0x80u;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        need = 2u;
        codepoint = lead & 0x0Fu;
        minimum = 0x800u;
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        need = 3u;
        codepoint = lead & 0x07u;
        minimum = 0x10000u;
    } else {
        goto malformed;
    }
    for (i = 1u; i <= need; ++i) {
        uint32_t byte;
        /* A NUL (or the end) is never a continuation byte, so the loop
         * never reads past either. */
        if (end != NULL && (size_t)(end - *cursor) <= i)
            goto malformed;
        byte = bytes[i];
        if ((byte & 0xC0u) != 0x80u)
            goto malformed;
        codepoint = (codepoint << 6) | (byte & 0x3Fu);
    }
    if (codepoint < minimum || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
        goto malformed;
    *cursor += need + 1u;
    return codepoint;

malformed:
    *cursor += 1;
    return MANAGER_BMFONT_REPLACEMENT;
}

const struct manager_bmfont_glyph *
manager_bmfont_find(const struct manager_bmfont *font, uint32_t codepoint)
{
    uint32_t low = 0u;
    uint32_t high;

    if (font == NULL || font->glyphs == NULL)
        return NULL;
    high = font->glyph_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint32_t value = font->glyphs[middle].codepoint;
        if (value == codepoint)
            return &font->glyphs[middle];
        if (value < codepoint)
            low = middle + 1u;
        else
            high = middle;
    }
    return NULL;
}

const struct manager_bmfont_glyph *
manager_bmfont_glyph_or_fallback(const struct manager_bmfont *font,
                                 uint32_t codepoint)
{
    const struct manager_bmfont_glyph *glyph = manager_bmfont_find(font, codepoint);

    if (glyph != NULL || font == NULL)
        return glyph;
    return font->fallback;
}

uint32_t manager_bmfont_line_height(const struct manager_bmfont *font,
                                    uint32_t scale)
{
    if (font == NULL || scale == 0u || scale > MANAGER_BMFONT_MAX_SCALE)
        return 0u;
    return (uint32_t)font->line_height * scale;
}

static int bmfont_text_done(const char *cursor, const char *end)
{
    return end != NULL ? cursor >= end : *cursor == '\0';
}

int32_t manager_bmfont_measure(const struct manager_bmfont *font,
                               const char *text, size_t length,
                               uint32_t scale)
{
    const char *cursor = text;
    const char *end;
    int32_t width = 0;

    if (font == NULL || text == NULL || scale == 0u ||
        scale > MANAGER_BMFONT_MAX_SCALE)
        return 0;
    end = length == MANAGER_BMFONT_NUL_TERMINATED ? NULL : text + length;
    while (!bmfont_text_done(cursor, end)) {
        uint32_t codepoint = manager_bmfont_utf8_next(&cursor, end);
        const struct manager_bmfont_glyph *glyph =
            manager_bmfont_glyph_or_fallback(font, codepoint);
        if (glyph != NULL)
            width += (int32_t)glyph->xadvance * (int32_t)scale;
    }
    return width;
}

void manager_bmfont_surface_init(struct manager_bmfont_surface *surface,
                                 uint32_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t pitch)
{
    if (surface == NULL)
        return;
    surface->pixels = pixels;
    surface->width = width;
    surface->height = height;
    surface->pitch = pitch;
    surface->clip_x0 = 0;
    surface->clip_y0 = 0;
    surface->clip_x1 = width > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)width;
    surface->clip_y1 = height > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)height;
}

static uint32_t bmfont_blend(uint32_t destination, uint32_t color, uint32_t alpha)
{
    uint32_t inverse;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t coverage;

    if (alpha >= 255u)
        return color | 0xFF000000u;
    inverse = 255u - alpha;
    red = ((color & 0xFFu) * alpha + (destination & 0xFFu) * inverse + 127u) /
          255u;
    green = (((color >> 8) & 0xFFu) * alpha +
             ((destination >> 8) & 0xFFu) * inverse + 127u) / 255u;
    blue = (((color >> 16) & 0xFFu) * alpha +
            ((destination >> 16) & 0xFFu) * inverse + 127u) / 255u;
    coverage = (255u * alpha + (destination >> 24) * inverse + 127u) / 255u;
    return red | (green << 8) | (blue << 16) | (coverage << 24);
}

static int64_t bmfont_max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t bmfont_min64(int64_t a, int64_t b) { return a < b ? a : b; }

int32_t manager_bmfont_draw_glyph(const struct manager_bmfont *font,
                                  const struct manager_bmfont_surface *surface,
                                  int32_t x, int32_t y,
                                  const struct manager_bmfont_glyph *glyph,
                                  uint32_t color, uint32_t scale)
{
    int32_t advance;
    uint32_t color_alpha;
    int64_t clip_x0, clip_y0, clip_x1, clip_y1;
    int64_t origin_x, origin_y;
    int64_t x0, y0, x1, y1;
    int64_t row;

    if (font == NULL || glyph == NULL || scale == 0u ||
        scale > MANAGER_BMFONT_MAX_SCALE)
        return 0;
    advance = (int32_t)glyph->xadvance * (int32_t)scale;
    color_alpha = color >> 24;
    if (surface == NULL || surface->pixels == NULL || font->page == NULL ||
        color_alpha == 0u || glyph->width == 0u || glyph->height == 0u)
        return advance;

    clip_x0 = bmfont_max64(surface->clip_x0, 0);
    clip_y0 = bmfont_max64(surface->clip_y0, 0);
    clip_x1 = bmfont_min64(surface->clip_x1, (int64_t)surface->width);
    clip_y1 = bmfont_min64(surface->clip_y1, (int64_t)surface->height);
    origin_x = (int64_t)x + (int64_t)glyph->xoffset * scale;
    origin_y = (int64_t)y + (int64_t)glyph->yoffset * scale;
    x0 = bmfont_max64(origin_x, clip_x0);
    y0 = bmfont_max64(origin_y, clip_y0);
    x1 = bmfont_min64(origin_x + (int64_t)glyph->width * scale, clip_x1);
    y1 = bmfont_min64(origin_y + (int64_t)glyph->height * scale, clip_y1);
    for (row = y0; row < y1; ++row) {
        uint32_t glyph_row = (uint32_t)((row - origin_y) / scale);
        const uint8_t *source = font->page +
                                (size_t)(glyph->y + glyph_row) * font->scale_w +
                                glyph->x;
        uint32_t *target = surface->pixels + (size_t)row * surface->pitch;
        int64_t column;
        for (column = x0; column < x1; ++column) {
            uint32_t alpha = source[(size_t)((column - origin_x) / scale)];
            if (alpha == 0u)
                continue;
            alpha = (alpha * color_alpha + 127u) / 255u;
            if (alpha == 0u)
                continue;
            target[column] = bmfont_blend(target[column], color, alpha);
        }
    }
    return advance;
}

int32_t manager_bmfont_draw(const struct manager_bmfont *font,
                            const struct manager_bmfont_surface *surface,
                            int32_t x, int32_t y, const char *text,
                            size_t length, uint32_t color, uint32_t scale)
{
    const char *cursor = text;
    const char *end;
    int32_t pen = x;

    if (font == NULL || text == NULL || scale == 0u ||
        scale > MANAGER_BMFONT_MAX_SCALE)
        return x;
    end = length == MANAGER_BMFONT_NUL_TERMINATED ? NULL : text + length;
    while (!bmfont_text_done(cursor, end)) {
        uint32_t codepoint = manager_bmfont_utf8_next(&cursor, end);
        pen += manager_bmfont_draw_glyph(
            font, surface, pen, y,
            manager_bmfont_glyph_or_fallback(font, codepoint), color, scale);
    }
    return pen;
}

const char *manager_bmfont_status_text(int status)
{
    switch (status) {
    case MANAGER_BMFONT_OK: return "ok";
    case MANAGER_BMFONT_E_ARGUMENT: return "argument";
    case MANAGER_BMFONT_E_SIGNATURE: return "signature";
    case MANAGER_BMFONT_E_BLOCK: return "block";
    case MANAGER_BMFONT_E_UNSUPPORTED: return "unsupported";
    case MANAGER_BMFONT_E_GLYPH: return "glyph";
    case MANAGER_BMFONT_E_CAPACITY: return "capacity";
    case MANAGER_BMFONT_E_PAGE: return "page";
    default: return "unknown";
    }
}
