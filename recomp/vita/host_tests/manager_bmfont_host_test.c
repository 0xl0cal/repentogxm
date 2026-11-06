/*
 * manager_bmfont host test: a binary BMFont v3
 * written by this test (not the game's) - parse goldens, every truncation,
 * one file per refusal, glyph lookup and the '?' fallback, UTF-8 decoding,
 * measure, the blend arithmetic (constants computed independently from the
 * rule in manager_bmfont.h), clipping at all four edges and against a clip
 * rectangle, nearest-neighbour scale 2, pitch padding and guard rows never
 * touched, RGBA -> A8.
 */
#include "gameskin_fixture.h"
#include "manager_bmfont.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_W 32u
#define PAGE_H 16u
#define REPLACEMENT MANAGER_BMFONT_REPLACEMENT

struct row {
    uint32_t id;
    uint16_t x, y, w, h;
    int16_t xoffset, yoffset, xadvance;
    uint8_t page, channel;
};

/* {page alpha, colour, destination, expected}: expected computed with
 * Python from out = (src * a + dst * (255 - a) + 127) / 255, a = (page
 * alpha * colour alpha + 127) / 255, alpha channel with src 255, a >= 255
 * writes the colour opaque, a == 0 leaves the destination. */
static const uint32_t blend_cases[][4] = {
    {128u, 0xFF0000FFu, 0xFF000000u, 0xFF000080u},
    {255u, 0x80FFFFFFu, 0xFF204080u, 0xFF90A0C0u},
    {  1u, 0x01FFFFFFu, 0xFF112233u, 0xFF112233u},
    {254u, 0xFF123456u, 0x00000000u, 0xFE123456u},
    {255u, 0xFF123456u, 0x00ABCDEFu, 0xFF123456u},
    {127u, 0xC0ABCDEFu, 0x7F102030u, 0xAF4A6178u},
    {  0u, 0xFFFFFFFFu, 0x12345678u, 0x12345678u},
    {200u, 0xFF00FF00u, 0xFFFF00FFu, 0xFF37C837u},
    {  1u, 0xFFFFFFFFu, 0xFF000000u, 0xFF010101u},
};
#define BLEND_CASES (sizeof(blend_cases) / sizeof(blend_cases[0]))

/* File order is deliberately unsorted. */
static struct row rows[6 + BLEND_CASES];
static uint32_t row_count;
static uint8_t page[PAGE_W * PAGE_H];

static void build_rows_and_page(void)
{
    static const struct row base[] = {
        {0x1F600u, 10, 0, 1, 1, 0, 0, 9, 0, 15},
        {0x41u, 0, 0, 4, 5, 1, 2, 6, 0, 15},   /* 'A' solid 255 */
        {0x3Fu, 6, 0, 3, 3, 0, 0, 4, 0, 15},   /* '?' solid 255 */
        {0x416u, 9, 0, 1, 1, 0, 0, 7, 0, 15},  /* Cyrillic Zhe, alpha 200 */
        {0x20u, 0, 0, 0, 0, 0, 0, 5, 0, 15},   /* space: no texels */
        {0x42u, 4, 0, 2, 2, 0, 0, 3, 0, 15},   /* 'B' alphas 10 20 / 30 40 */
    };
    uint32_t i, x, y;

    memset(page, 0, sizeof(page));
    for (y = 0u; y < 5u; ++y)
        for (x = 0u; x < 4u; ++x)
            page[y * PAGE_W + x] = 255u;
    page[4] = 10u;
    page[5] = 20u;
    page[PAGE_W + 4] = 30u;
    page[PAGE_W + 5] = 40u;
    for (y = 0u; y < 3u; ++y)
        for (x = 6u; x < 9u; ++x)
            page[y * PAGE_W + x] = 255u;
    page[9] = 200u;
    page[10] = 255u;
    row_count = 0u;
    for (i = 0u; i < sizeof(base) / sizeof(base[0]); ++i)
        rows[row_count++] = base[i];
    for (i = 0u; i < BLEND_CASES; ++i) {
        struct row blend = {0x100u + i, (uint16_t)(16u + i), 0, 1, 1, 0, 0, 1, 0, 15};
        rows[row_count++] = blend;
        page[16u + i] = (uint8_t)blend_cases[i][0];
    }
}

/* ---- BMF writer ------------------------------------------------------- */

struct variant {
    uint8_t version;       /* 3 */
    char magic0;           /* 'B' */
    int skip_block;        /* block type left out */
    int repeat_block;      /* block type written twice */
    int extra_type;        /* an extra block of this type (0 = none) */
    uint32_t block1_size;  /* 0 = normal */
    uint32_t block2_size;
    const char *page_name;
    int page_no_nul;
    uint16_t pages;
    uint16_t scale_w;
    int glyph_mod;         /* 1 page 1, 2 channel 4, 3 outside, 4 duplicate */
    int block4_extra;      /* bytes appended to block 4 */
    int trailing;          /* bytes after the last block */
    int zero_glyphs;
    int oversize_last;     /* last block's size field 1 too large */
};

struct bmf {
    uint8_t bytes[4096];
    size_t size;
    size_t block4_end;
};

static void put(struct bmf *out, const void *data, size_t n)
{
    memcpy(out->bytes + out->size, data, n);
    out->size += n;
}

static void put16(struct bmf *out, uint32_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(out, b, 2);
}

static void put32(struct bmf *out, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(out, b, 4);
}

static void block(struct bmf *out, const struct variant *v, int type)
{
    size_t size_at;
    size_t start;
    uint32_t i;

    out->bytes[out->size++] = (uint8_t)type;
    size_at = out->size;
    put32(out, 0u);
    start = out->size;
    switch (type) {
    case 1:
        put16(out, 16u); /* font size */
        if (v->block1_size != 1u) {
            uint8_t rest[12] = {0};
            put(out, rest, sizeof(rest));
            put(out, "hosttest", 9);
        } else {
            out->size = start + 1u;
        }
        break;
    case 2:
        put16(out, 20u);
        put16(out, 16u);
        put16(out, v->scale_w);
        put16(out, PAGE_H);
        put16(out, v->pages);
        if (v->block2_size == 9u)
            out->size = start + 9u;
        else
            put(out, "\0\0\0\0\0", 5);
        break;
    case 3: {
        const char *name = v->page_name;
        put(out, name, strlen(name) + (v->page_no_nul ? 0u : 1u));
        break;
    }
    case 4:
        if (!v->zero_glyphs) {
            for (i = 0u; i < row_count; ++i) {
                struct row r = rows[i];
                if (i == 1u) {
                    if (v->glyph_mod == 1)
                        r.page = 1u;
                    else if (v->glyph_mod == 2)
                        r.channel = 4u;
                    else if (v->glyph_mod == 3)
                        r.x = (uint16_t)(PAGE_W - r.w + 1u);
                    else if (v->glyph_mod == 4)
                        r.id = rows[0].id;
                }
                put32(out, r.id);
                put16(out, r.x);
                put16(out, r.y);
                put16(out, r.w);
                put16(out, r.h);
                put16(out, (uint16_t)r.xoffset);
                put16(out, (uint16_t)r.yoffset);
                put16(out, (uint16_t)r.xadvance);
                out->bytes[out->size++] = r.page;
                out->bytes[out->size++] = r.channel;
            }
        }
        for (i = 0u; i < (uint32_t)v->block4_extra; ++i)
            out->bytes[out->size++] = 0u;
        break;
    default:
        break; /* 5 and unknown types: empty */
    }
    {
        uint32_t size = (uint32_t)(out->size - start);
        out->bytes[size_at] = (uint8_t)size;
        out->bytes[size_at + 1u] = (uint8_t)(size >> 8);
        out->bytes[size_at + 2u] = (uint8_t)(size >> 16);
        out->bytes[size_at + 3u] = (uint8_t)(size >> 24);
    }
    if (type == 4)
        out->block4_end = out->size;
}

static struct variant normal(void)
{
    struct variant v;

    memset(&v, 0, sizeof(v));
    v.version = 3u;
    v.magic0 = 'B';
    v.page_name = "hosttest_0.png";
    v.pages = 1u;
    v.scale_w = PAGE_W;
    return v;
}

static void write_bmf(struct bmf *out, const struct variant *v)
{
    int type;

    memset(out, 0, sizeof(*out));
    out->bytes[0] = (uint8_t)v->magic0;
    out->bytes[1] = 'M';
    out->bytes[2] = 'F';
    out->bytes[3] = v->version;
    out->size = 4u;
    for (type = 1; type <= 5; ++type) {
        if (type == v->skip_block)
            continue;
        block(out, v, type);
        if (type == v->repeat_block)
            block(out, v, type);
    }
    if (v->extra_type != 0)
        block(out, v, v->extra_type);
    if (v->oversize_last) {
        /* block 5 is last and empty: its size field says 1 */
        out->bytes[out->size - 4u] = 1u;
    }
    out->size += (size_t)v->trailing;
}

/* ---- parse ------------------------------------------------------------ */

static struct manager_bmfont_glyph glyphs[64];

static int parse_variant(const struct variant *v, struct manager_bmfont *font,
                         uint32_t capacity)
{
    struct bmf file;

    write_bmf(&file, v);
    return manager_bmfont_parse(font, file.bytes, file.size, glyphs, capacity);
}

static void test_parse_good(struct manager_bmfont *font)
{
    struct variant v = normal();
    struct bmf file;
    uint32_t i;

    write_bmf(&file, &v);
    GS_CHECK(manager_bmfont_glyph_rows(file.bytes, file.size) == row_count,
             "glyph rows %u", manager_bmfont_glyph_rows(file.bytes, file.size));
    GS_CHECK(manager_bmfont_parse(font, file.bytes, file.size, glyphs, 64u) ==
                 MANAGER_BMFONT_OK, "parse");
    GS_CHECK(font->font_size == 16 && font->line_height == 20u && font->base == 16u &&
                 font->scale_w == PAGE_W && font->scale_h == PAGE_H && font->pages == 1u,
             "common block");
    GS_CHECK(strcmp(font->page_name, "hosttest_0.png") == 0, "page name %s",
             font->page_name);
    GS_CHECK(font->glyph_count == row_count && font->glyphs == glyphs, "glyph count");
    for (i = 1u; i < font->glyph_count; ++i)
        GS_CHECK(glyphs[i - 1u].codepoint < glyphs[i].codepoint, "sorted at %u", i);
    for (i = 0u; i < row_count; ++i) {
        const struct manager_bmfont_glyph *g = manager_bmfont_find(font, rows[i].id);
        GS_CHECK(g != NULL && g->x == rows[i].x && g->y == rows[i].y &&
                     g->width == rows[i].w && g->height == rows[i].h &&
                     g->xoffset == rows[i].xoffset && g->yoffset == rows[i].yoffset &&
                     g->xadvance == rows[i].xadvance && g->page == 0u && g->channel == 15u,
                 "glyph U+%04X", rows[i].id);
    }
    GS_CHECK(font->fallback == manager_bmfont_find(font, '?') && font->fallback != NULL,
             "fallback is '?'");
    GS_CHECK(manager_bmfont_find(font, 'C') == NULL, "C absent");
    GS_CHECK(manager_bmfont_glyph_or_fallback(font, 'C') == font->fallback, "C -> '?'");
    GS_CHECK(manager_bmfont_find(font, 0u) == NULL && manager_bmfont_find(font, 0xFFFFFFFFu) == NULL,
             "extremes absent");
    GS_CHECK(manager_bmfont_glyph_rows(file.bytes, 3u) == 0u, "rows of a stub");
    GS_CHECK(font->page == NULL, "no page before attach");

    /* every strict prefix fails, except the one ending right after block 4
     * (block 5, kerning, is optional) */
    {
        size_t length;
        unsigned unexpected = 0u;
        for (length = 0u; length < file.size; ++length) {
            struct manager_bmfont scratch;
            uint8_t copy[4096];
            int status;
            memcpy(copy, file.bytes, length);
            status = manager_bmfont_parse(&scratch, copy, length, glyphs + 32, 32u);
            if ((status == MANAGER_BMFONT_OK) != (length == file.block4_end))
                ++unexpected;
        }
        GS_CHECK(unexpected == 0u, "%u truncations misjudged", unexpected);
    }
    /* reparse into the real storage for the drawing tests */
    GS_CHECK(manager_bmfont_parse(font, file.bytes, file.size, glyphs, 64u) ==
                 MANAGER_BMFONT_OK, "reparse");
}

static void test_parse_refusals(void)
{
    struct manager_bmfont font;
    struct variant v;
    char long_name[80];
    int status;

#define EXPECT(label, expected)                                               \
    do {                                                                      \
        status = parse_variant(&v, &font, 64u);                               \
        GS_CHECK(status == (expected), "%s: %s", label,                       \
                 manager_bmfont_status_text(status));                         \
        v = normal();                                                         \
    } while (0)

    v = normal();
    v.version = 2u;             EXPECT("version 2", MANAGER_BMFONT_E_SIGNATURE);
    v.magic0 = 'X';             EXPECT("magic", MANAGER_BMFONT_E_SIGNATURE);
    v.skip_block = 3;           EXPECT("no block 3", MANAGER_BMFONT_E_BLOCK);
    v.skip_block = 4;           EXPECT("no block 4", MANAGER_BMFONT_E_BLOCK);
    v.skip_block = 1;           EXPECT("no block 1", MANAGER_BMFONT_E_BLOCK);
    v.skip_block = 5;           EXPECT("no block 5 is fine", MANAGER_BMFONT_OK);
    v.repeat_block = 2;         EXPECT("block 2 twice", MANAGER_BMFONT_E_BLOCK);
    v.extra_type = 6;           EXPECT("block type 6", MANAGER_BMFONT_E_BLOCK);
    v.extra_type = 0x100;       /* written as type 0 */
    EXPECT("block type 0", MANAGER_BMFONT_E_BLOCK);
    v.block1_size = 1u;         EXPECT("block 1 of 1 byte", MANAGER_BMFONT_E_BLOCK);
    v.block2_size = 9u;         EXPECT("block 2 of 9 bytes", MANAGER_BMFONT_E_BLOCK);
    v.page_no_nul = 1;          EXPECT("page name without NUL", MANAGER_BMFONT_E_BLOCK);
    v.page_name = "";           EXPECT("empty page name", MANAGER_BMFONT_E_BLOCK);
    memset(long_name, 'n', sizeof(long_name));
    long_name[64] = '\0';
    v.page_name = long_name;    EXPECT("64-char page name", MANAGER_BMFONT_E_BLOCK);
    long_name[63] = '\0';
    v.page_name = long_name;    EXPECT("63-char page name", MANAGER_BMFONT_OK);
    v.block4_extra = 1;         EXPECT("block 4 not rows of 20", MANAGER_BMFONT_E_BLOCK);
    v.trailing = 3;             EXPECT("trailing bytes", MANAGER_BMFONT_E_BLOCK);
    v.oversize_last = 1;        EXPECT("block past the end", MANAGER_BMFONT_E_BLOCK);
    v.pages = 2u;               EXPECT("two pages", MANAGER_BMFONT_E_UNSUPPORTED);
    v.glyph_mod = 1;            EXPECT("glyph on page 1", MANAGER_BMFONT_E_UNSUPPORTED);
    v.glyph_mod = 2;            EXPECT("glyph channel 4", MANAGER_BMFONT_E_UNSUPPORTED);
    v.glyph_mod = 3;            EXPECT("glyph outside the page", MANAGER_BMFONT_E_GLYPH);
    v.glyph_mod = 4;            EXPECT("duplicate codepoint", MANAGER_BMFONT_E_GLYPH);
    v.scale_w = 0u;             EXPECT("zero-width page", MANAGER_BMFONT_E_GLYPH);
#undef EXPECT

    v = normal();
    GS_CHECK(parse_variant(&v, &font, row_count - 1u) == MANAGER_BMFONT_E_CAPACITY,
             "capacity");
    {
        struct bmf file;
        write_bmf(&file, &v);
        GS_CHECK(manager_bmfont_parse(&font, file.bytes, file.size, NULL, 0u) ==
                     MANAGER_BMFONT_E_CAPACITY, "no storage");
        GS_CHECK(manager_bmfont_parse(&font, file.bytes, file.size, NULL, 4u) ==
                     MANAGER_BMFONT_E_ARGUMENT, "NULL storage with capacity");
        GS_CHECK(manager_bmfont_parse(NULL, file.bytes, file.size, glyphs, 64u) ==
                     MANAGER_BMFONT_E_ARGUMENT, "NULL font");
    }
    v.zero_glyphs = 1;
    GS_CHECK(parse_variant(&v, &font, 0u) == MANAGER_BMFONT_OK && font.glyph_count == 0u &&
                 font.fallback == NULL, "font without glyphs");
    GS_CHECK(manager_bmfont_measure(&font, "abc", MANAGER_BMFONT_NUL_TERMINATED, 1u) == 0,
             "nothing to measure");
}

/* ---- UTF-8 ------------------------------------------------------------ */

struct utf8_case {
    const char *bytes;
    size_t length; /* MANAGER_BMFONT_NUL_TERMINATED = use NUL */
    uint32_t cps[6];
    unsigned n;
};

static void test_utf8(void)
{
    static const struct utf8_case cases[] = {
        {"A", 1, {0x41}, 1},
        {"\xC3\xA9", 2, {0xE9}, 1},
        {"\xD0\x96", 2, {0x416}, 1},
        {"\xE2\x82\xAC", 3, {0x20AC}, 1},
        {"\xF0\x9F\x98\x80", 4, {0x1F600}, 1},
        {"\xF4\x8F\xBF\xBF", 4, {0x10FFFF}, 1},
        {"\xC0\x80", 2, {REPLACEMENT, REPLACEMENT}, 2},
        {"\xC1\xBF", 2, {REPLACEMENT, REPLACEMENT}, 2},
        {"\xE0\x80\x80", 3, {REPLACEMENT, REPLACEMENT, REPLACEMENT}, 3},
        {"\xED\xA0\x80", 3, {REPLACEMENT, REPLACEMENT, REPLACEMENT}, 3},
        {"\xED\xBF\xBF", 3, {REPLACEMENT, REPLACEMENT, REPLACEMENT}, 3},
        {"\xED\x9F\xBF", 3, {0xD7FF}, 1},
        {"\xEE\x80\x80", 3, {0xE000}, 1},
        {"\xF4\x90\x80\x80", 4, {REPLACEMENT, REPLACEMENT, REPLACEMENT, REPLACEMENT}, 4},
        {"\xF5\x80", 2, {REPLACEMENT, REPLACEMENT}, 2},
        {"\x80", 1, {REPLACEMENT}, 1},
        {"\xFF" "A", 2, {REPLACEMENT, 0x41}, 2},
        {"\xE2\x82", 2, {REPLACEMENT, REPLACEMENT}, 2},
        {"A\xC3", 2, {0x41, REPLACEMENT}, 2},
        {"A\0B", 3, {0x41, 0, 0x42}, 3},
        {"\xE2\x82", MANAGER_BMFONT_NUL_TERMINATED, {REPLACEMENT, REPLACEMENT}, 2},
        {"A\0B", MANAGER_BMFONT_NUL_TERMINATED, {0x41}, 1},
        {"\xC3\xA9x", MANAGER_BMFONT_NUL_TERMINATED, {0xE9, 0x78}, 2},
    };
    size_t c;
    const char *cursor = NULL;

    for (c = 0u; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const char *end = cases[c].length == MANAGER_BMFONT_NUL_TERMINATED
                              ? NULL : cases[c].bytes + cases[c].length;
        unsigned got = 0u;
        int same = 1;
        cursor = cases[c].bytes;
        for (;;) {
            uint32_t cp;
            if (end != NULL ? cursor >= end : *cursor == '\0')
                break;
            cp = manager_bmfont_utf8_next(&cursor, end);
            if (got >= 6u || cp != cases[c].cps[got])
                same = 0;
            ++got;
        }
        GS_CHECK(same && got == cases[c].n, "utf8 case %u: %u codepoints", (unsigned)c, got);
        if (end == NULL) {
            const char *at_nul = cursor;
            GS_CHECK(manager_bmfont_utf8_next(&cursor, NULL) == 0u && cursor == at_nul,
                     "utf8 case %u: NUL does not advance", (unsigned)c);
        }
    }
    cursor = NULL;
    GS_CHECK(manager_bmfont_utf8_next(&cursor, NULL) == 0u, "NULL text");
    GS_CHECK(manager_bmfont_utf8_next(NULL, NULL) == 0u, "NULL cursor");
}

/* ---- measure ---------------------------------------------------------- */

static void test_measure(const struct manager_bmfont *font)
{
    /* advances: A 6, B 3, '?' 4, space 5, Zhe 7, U+1F600 9 */
    GS_CHECK(manager_bmfont_measure(font, "AB", MANAGER_BMFONT_NUL_TERMINATED, 1u) == 9,
             "AB");
    GS_CHECK(manager_bmfont_measure(font, "AB", 2u, 2u) == 18, "AB x2");
    GS_CHECK(manager_bmfont_measure(font, "AB", 1u, 1u) == 6, "length 1");
    GS_CHECK(manager_bmfont_measure(font, "A?C", MANAGER_BMFONT_NUL_TERMINATED, 1u) == 14,
             "missing C measures as '?'");
    GS_CHECK(manager_bmfont_measure(font, "\xD0\x96\xF0\x9F\x98\x80 ",
                                    MANAGER_BMFONT_NUL_TERMINATED, 1u) == 21,
             "Zhe + emoji + space");
    GS_CHECK(manager_bmfont_measure(font, "\xFF", 1u, 3u) == 12, "invalid byte -> '?' x3");
    GS_CHECK(manager_bmfont_measure(font, "", MANAGER_BMFONT_NUL_TERMINATED, 1u) == 0,
             "empty");
    GS_CHECK(manager_bmfont_measure(font, "AB", 2u, 0u) == 0, "scale 0");
    GS_CHECK(manager_bmfont_measure(font, "AB", 2u, MANAGER_BMFONT_MAX_SCALE + 1u) == 0,
             "scale 9");
    GS_CHECK(manager_bmfont_measure(font, "AB", 2u, MANAGER_BMFONT_MAX_SCALE) == 72,
             "scale 8");
    GS_CHECK(manager_bmfont_measure(NULL, "AB", 2u, 1u) == 0, "NULL font");
    GS_CHECK(manager_bmfont_measure(font, NULL, 2u, 1u) == 0, "NULL text");
    GS_CHECK(manager_bmfont_line_height(font, 1u) == 20u &&
                 manager_bmfont_line_height(font, 2u) == 40u &&
                 manager_bmfont_line_height(font, 0u) == 0u &&
                 manager_bmfont_line_height(font, 9u) == 0u &&
                 manager_bmfont_line_height(NULL, 1u) == 0u,
             "line height");
}

/* ---- drawing ---------------------------------------------------------- */

static void test_blend(const struct manager_bmfont *font)
{
    size_t c;

    for (c = 0u; c < BLEND_CASES; ++c) {
        struct manager_bmfont_surface surface;
        uint32_t pixel[2] = {blend_cases[c][2], 0x13572468u};
        int32_t advance;
        manager_bmfont_surface_init(&surface, pixel, 1u, 1u, 1u);
        advance = manager_bmfont_draw_glyph(font, &surface, 0, 0,
                                            manager_bmfont_find(font, 0x100u + (uint32_t)c),
                                            blend_cases[c][1], 1u);
        GS_CHECK(advance == 1 && pixel[0] == blend_cases[c][3] && pixel[1] == 0x13572468u,
                 "blend case %u: %08x, expected %08x", (unsigned)c, pixel[0],
                 blend_cases[c][3]);
    }
}

#define CANVAS_W 12u
#define CANVAS_H 10u
#define CANVAS_PITCH 16u
#define BACKGROUND 0xFF000000u
#define PAD 0xDEADBEEFu

static uint32_t canvas[CANVAS_PITCH * (CANVAS_H + 1u)];

static void canvas_reset(void)
{
    uint32_t x, y;

    for (y = 0u; y < CANVAS_H + 1u; ++y)
        for (x = 0u; x < CANVAS_PITCH; ++x)
            canvas[y * CANVAS_PITCH + x] = (y < CANVAS_H && x < CANVAS_W) ? BACKGROUND : PAD;
}

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

/* 'A' (4x5 solid, offset 1,2) drawn opaque white at pen (px, py) inside
 * clip; every pixel checked against the rectangle intersection. */
static void check_clip(const struct manager_bmfont *font, int32_t px, int32_t py,
                       int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1,
                       uint32_t scale, const char *label)
{
    struct manager_bmfont_surface surface;
    int64_t x0, y0, x1, y1;
    uint32_t x, y;
    unsigned wrong = 0u, painted = 0u;
    int64_t expected_painted;

    canvas_reset();
    manager_bmfont_surface_init(&surface, canvas, CANVAS_W, CANVAS_H, CANVAS_PITCH);
    surface.clip_x0 = cx0;
    surface.clip_y0 = cy0;
    surface.clip_x1 = cx1;
    surface.clip_y1 = cy1;
    GS_CHECK(manager_bmfont_draw_glyph(font, &surface, px, py, manager_bmfont_find(font, 'A'),
                                       0xFFFFFFFFu, scale) == 6 * (int32_t)scale,
             "%s advance", label);
    x0 = max64(max64((int64_t)px + 1 * (int64_t)scale, cx0), 0);
    y0 = max64(max64((int64_t)py + 2 * (int64_t)scale, cy0), 0);
    x1 = min64(min64((int64_t)px + 5 * (int64_t)scale, cx1), CANVAS_W);
    y1 = min64(min64((int64_t)py + 7 * (int64_t)scale, cy1), CANVAS_H);
    expected_painted = (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
    for (y = 0u; y < CANVAS_H + 1u; ++y) {
        for (x = 0u; x < CANVAS_PITCH; ++x) {
            uint32_t got = canvas[y * CANVAS_PITCH + x];
            uint32_t want;
            if (y >= CANVAS_H || x >= CANVAS_W)
                want = PAD;
            else if ((int64_t)x >= x0 && (int64_t)x < x1 && (int64_t)y >= y0 && (int64_t)y < y1)
                want = 0xFFFFFFFFu;
            else
                want = BACKGROUND;
            wrong += got != want;
            painted += got == 0xFFFFFFFFu;
        }
    }
    GS_CHECK(wrong == 0u && (int64_t)painted == expected_painted,
             "%s: %u wrong pixels, %u painted, expected %lld", label, wrong, painted,
             (long long)expected_painted);
}

static void test_clipping(const struct manager_bmfont *font)
{
    check_clip(font, 3, 1, 0, 0, CANVAS_W, CANVAS_H, 1u, "inside");
    check_clip(font, -3, 1, 0, 0, CANVAS_W, CANVAS_H, 1u, "left edge");
    check_clip(font, 9, 1, 0, 0, CANVAS_W, CANVAS_H, 1u, "right edge");
    check_clip(font, 2, -4, 0, 0, CANVAS_W, CANVAS_H, 1u, "top edge");
    check_clip(font, 3, 7, 0, 0, CANVAS_W, CANVAS_H, 1u, "bottom edge");
    check_clip(font, -2, -5, 0, 0, CANVAS_W, CANVAS_H, 1u, "top-left corner");
    check_clip(font, 10, 8, 0, 0, CANVAS_W, CANVAS_H, 1u, "bottom-right corner");
    check_clip(font, -20, -20, 0, 0, CANVAS_W, CANVAS_H, 1u, "fully outside");
    check_clip(font, 2000000000, 5, 0, 0, CANVAS_W, CANVAS_H, 8u, "far right, scale 8");
    check_clip(font, -2000000000, -2000000000, 0, 0, CANVAS_W, CANVAS_H, 8u, "far negative");
    check_clip(font, 1, 0, 3, 3, 7, 6, 1u, "clip rectangle");
    check_clip(font, 1, 0, 4, 0, 5, CANVAS_H, 1u, "one-column clip");
    check_clip(font, 1, 0, 6, 6, 3, 3, 1u, "inverted clip");
    check_clip(font, 1, 0, -50, -50, 500, 500, 1u, "clip larger than the surface");
    check_clip(font, 9, 1, -50, -50, 500, 500, 1u, "large clip, right edge");
    check_clip(font, 3, 7, -50, -50, 500, 500, 1u, "large clip, bottom edge");
    check_clip(font, -3, -4, -50, -50, 500, 500, 1u, "large clip, top-left");
    check_clip(font, 0, -3, 0, 0, CANVAS_W, CANVAS_H, 2u, "scale 2 at the top");
}

/* 'B' (2x2 texels of alpha 10 20 / 30 40, white over opaque black: the
 * blend gives 0xFF0A0A0A ... 0xFF282828, computed with Python) at scales 1,
 * 2 and 3: every page texel becomes a scale x scale block. */
static void test_scales(const struct manager_bmfont *font)
{
    static const uint32_t texel[4] = {0xFF0A0A0Au, 0xFF141414u, 0xFF1E1E1Eu, 0xFF282828u};
    uint32_t scale;

    for (scale = 1u; scale <= 3u; ++scale) {
        struct manager_bmfont_surface surface;
        uint32_t x, y;
        unsigned wrong = 0u;
        canvas_reset();
        manager_bmfont_surface_init(&surface, canvas, CANVAS_W, CANVAS_H, CANVAS_PITCH);
        GS_CHECK(manager_bmfont_draw_glyph(font, &surface, 1, 1,
                                           manager_bmfont_find(font, 'B'), 0xFFFFFFFFu,
                                           scale) == 3 * (int32_t)scale,
                 "B advance x%u", scale);
        for (y = 0u; y < CANVAS_H; ++y) {
            for (x = 0u; x < CANVAS_W; ++x) {
                uint32_t want = BACKGROUND;
                if (x >= 1u && x < 1u + 2u * scale && y >= 1u && y < 1u + 2u * scale)
                    want = texel[((y - 1u) / scale) * 2u + (x - 1u) / scale];
                wrong += canvas[y * CANVAS_PITCH + x] != want;
            }
        }
        GS_CHECK(wrong == 0u, "scale %u: %u wrong pixels", scale, wrong);
    }
}

static void test_draw_misc(struct manager_bmfont *font)
{
    struct manager_bmfont_surface surface;
    const uint8_t *attached = font->page;
    unsigned touched = 0u;
    uint32_t i;
    int32_t pen;

    canvas_reset();
    manager_bmfont_surface_init(&surface, canvas, CANVAS_W, CANVAS_H, CANVAS_PITCH);
    GS_CHECK(manager_bmfont_draw_glyph(font, &surface, 1, 1, manager_bmfont_find(font, 'A'),
                                       0x00FFFFFFu, 1u) == 6, "colour alpha 0 advances");
    GS_CHECK(manager_bmfont_draw_glyph(font, &surface, 1, 1, manager_bmfont_find(font, ' '),
                                       0xFFFFFFFFu, 1u) == 5, "space advances");
    GS_CHECK(manager_bmfont_draw_glyph(font, &surface, 1, 1, NULL, 0xFFFFFFFFu, 1u) == 0,
             "NULL glyph");
    GS_CHECK(manager_bmfont_draw_glyph(font, &surface, 1, 1, manager_bmfont_find(font, 'A'),
                                       0xFFFFFFFFu, 9u) == 0, "scale 9 refused");
    GS_CHECK(manager_bmfont_draw_glyph(font, NULL, 1, 1, manager_bmfont_find(font, 'A'),
                                       0xFFFFFFFFu, 1u) == 6, "NULL surface advances");
    font->page = NULL;
    pen = manager_bmfont_draw(font, &surface, 2, 0, "AB?", MANAGER_BMFONT_NUL_TERMINATED,
                              0xFFFFFFFFu, 1u);
    GS_CHECK(pen == 2 + 13, "no page: pen %d", pen);
    font->page = attached;
    for (i = 0u; i < CANVAS_PITCH * (CANVAS_H + 1u); ++i)
        touched += canvas[i] != ((i / CANVAS_PITCH < CANVAS_H && i % CANVAS_PITCH < CANVAS_W)
                                     ? BACKGROUND : PAD);
    GS_CHECK(touched == 0u, "nothing drawn: %u pixels touched", touched);

    pen = manager_bmfont_draw(font, &surface, -4, 2, "AB?C\xD0\x96",
                              MANAGER_BMFONT_NUL_TERMINATED, 0xFFFFFFFFu, 1u);
    GS_CHECK(pen == -4 + manager_bmfont_measure(font, "AB?C\xD0\x96",
                                                 MANAGER_BMFONT_NUL_TERMINATED, 1u),
             "draw returns x + measure (%d)", pen);
    GS_CHECK(canvas[CANVAS_H * CANVAS_PITCH] == PAD && canvas[CANVAS_W] == PAD,
             "pad words after drawing text");
    GS_CHECK(manager_bmfont_draw(font, &surface, 7, 0, NULL, 3u, 0xFFFFFFFFu, 1u) == 7,
             "NULL text");
}

static void test_a8_and_attach(struct manager_bmfont *font)
{
    uint8_t rgba[16] = {255, 255, 255, 0,  10, 20, 30, 0,
                        255, 255, 255, 128, 255, 0, 255, 7};
    uint32_t lost = 99u;
    struct manager_bmfont empty;

    manager_bmfont_rgba_to_a8(rgba, rgba, 4u, &lost);
    GS_CHECK(rgba[0] == 0u && rgba[1] == 0u && rgba[2] == 128u && rgba[3] == 7u,
             "A8 in place");
    GS_CHECK(lost == 1u, "one non-white texel with alpha (%u)", lost);
    manager_bmfont_rgba_to_a8(NULL, rgba, 4u, &lost);
    manager_bmfont_rgba_to_a8(rgba, rgba, 0u, NULL);

    GS_CHECK(manager_bmfont_attach_page(font, page, PAGE_W + 1u, PAGE_H) ==
                 MANAGER_BMFONT_E_PAGE, "page width");
    GS_CHECK(manager_bmfont_attach_page(font, page, PAGE_W, PAGE_H - 1u) ==
                 MANAGER_BMFONT_E_PAGE, "page height");
    GS_CHECK(manager_bmfont_attach_page(font, NULL, PAGE_W, PAGE_H) ==
                 MANAGER_BMFONT_E_ARGUMENT, "NULL page");
    memset(&empty, 0, sizeof(empty));
    GS_CHECK(manager_bmfont_attach_page(&empty, page, PAGE_W, PAGE_H) ==
                 MANAGER_BMFONT_E_ARGUMENT, "unparsed font");
    GS_CHECK(manager_bmfont_attach_page(font, page, PAGE_W, PAGE_H) == MANAGER_BMFONT_OK &&
                 font->page == page, "attach");
}

static void test_status_texts(void)
{
    static const char *const names[] = {
        "ok", "argument", "signature", "block", "unsupported", "glyph", "capacity", "page"};
    int status;

    for (status = 0; status >= -7; --status)
        GS_CHECK(strcmp(manager_bmfont_status_text(status), names[-status]) == 0,
                 "status %d", status);
    GS_CHECK(strcmp(manager_bmfont_status_text(-8), "unknown") == 0, "unknown");
}

int main(void)
{
    struct manager_bmfont font;

    build_rows_and_page();
    test_status_texts();
    test_parse_refusals();
    test_parse_good(&font);
    test_utf8();
    test_measure(&font);
    test_a8_and_attach(&font);
    test_blend(&font);
    test_clipping(&font);
    test_scales(&font);
    test_draw_misc(&font);
    return gs_report("manager_bmfont_host_test");
}
