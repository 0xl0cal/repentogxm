/*
 * Manager UI2 font + canvas host test (manager_font_host_test):
 * IMF1 parse goldens (the three committed files, truncated, bad magic, bad
 * CRC, unsorted table, glyph outside the atlas, no '?', A4 flag), widths of
 * fixed strings against Pillow's own getlength() of the same faces, UTF-8
 * decoding, blend math, the rounded-box corner, clipping at all four edges
 * with guard words around the buffer, the ellipsis cut, the 5x7 fallback.
 */
#include "manager_canvas.h"
#include "manager_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MANAGER_UI_ASSET_DIR
#error "MANAGER_UI_ASSET_DIR must name recomp/vita/assets/manager"
#endif

static unsigned int checks;

static void require(int condition, const char *message)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static unsigned char *load_file(const char *name, size_t *size)
{
    char path[1024];
    FILE *file;
    long length;
    unsigned char *data;
    (void)snprintf(path, sizeof(path), "%s/%s", MANAGER_UI_ASSET_DIR, name);
    file = fopen(path, "rb");
    require(file != NULL, "asset opens");
    require(fseek(file, 0, SEEK_END) == 0, "asset seek");
    length = ftell(file);
    require(length > 0, "asset length");
    require(fseek(file, 0, SEEK_SET) == 0, "asset rewind");
    data = (unsigned char *)malloc((size_t)length);
    require(data != NULL, "asset buffer");
    require(fread(data, 1u, (size_t)length, file) == (size_t)length, "asset read");
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void put_u16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    put_u16(p, v & 0xffffu);
    put_u16(p + 2, (v >> 16) & 0xffffu);
}

static void refresh_crc(unsigned char *data, size_t size)
{
    put_u32(data + 24, manager_crc32(data + MANAGER_IMF_HEADER_BYTES,
                                     size - MANAGER_IMF_HEADER_BYTES));
}

/* A 3-glyph font written here: '?', 'A', 'B' on a 4x2 atlas. */
static size_t synthetic_font(unsigned char *out, uint32_t cp0, uint32_t cp1,
                             uint32_t cp2)
{
    const uint32_t cps[3] = {cp0, cp1, cp2};
    size_t size = MANAGER_IMF_HEADER_BYTES + 3u * MANAGER_IMF_GLYPH_BYTES + 8u;
    unsigned int index;
    memset(out, 0, size);
    memcpy(out, "IMF1", 4u);
    put_u16(out + 4, 4u);  /* px */
    put_u16(out + 6, 3u);  /* line_h */
    put_u16(out + 8, 2u);  /* ascent */
    put_u16(out + 10, 4u); /* atlas_w */
    put_u16(out + 12, 2u); /* atlas_h */
    put_u16(out + 14, 3u); /* glyph_count */
    put_u32(out + 20, 8u); /* atlas_bytes */
    for (index = 0u; index < 3u; ++index) {
        unsigned char *g = out + MANAGER_IMF_HEADER_BYTES +
                           index * MANAGER_IMF_GLYPH_BYTES;
        put_u32(g, cps[index]);
        put_u16(g + 4, index); /* x */
        g[8] = 1u;             /* w */
        g[9] = 2u;             /* h */
        g[12] = 2u;            /* advance */
    }
    memset(out + size - 8u, 0xff, 8u);
    refresh_crc(out, size);
    return size;
}

static void test_parse_goldens(void)
{
    static const struct {
        const char *name;
        unsigned int px;
    } files[] = {{"font-body-16.imf", 16u}, {"font-small-12.imf", 12u},
                 {"font-title-24.imf", 24u}};
    unsigned int index;
    unsigned char synthetic[256];
    size_t size;
    struct manager_imf font;

    for (index = 0u; index < 3u; ++index) {
        unsigned char *data = load_file(files[index].name, &size);
        unsigned char *copy = (unsigned char *)malloc(size);
        require(copy != NULL, "copy buffer");
        require(manager_imf_parse(&font, data, size) == MANAGER_IMF_OK,
                "committed IMF parses");
        require(font.px == files[index].px, "px field");
        require(font.glyph_count == 183u, "183 glyphs");
        require(manager_imf_find(&font, 0x3fu) == (int)font.question, "'?' index");
        require(manager_imf_find(&font, 0x0416u) >= 0, "Cyrillic Zhe baked");
        require(manager_imf_find(&font, 0x2611u) >= 0, "ballot box baked");
        require(manager_imf_find(&font, 0x4e00u) < 0, "CJK not baked");
        require(manager_imf_parse(&font, data, size - 1u) == MANAGER_IMF_BAD_SIZE,
                "one byte short");
        require(manager_imf_parse(&font, data, 20u) == MANAGER_IMF_TRUNCATED,
                "short header");
        memcpy(copy, data, size);
        copy[0] = 'X';
        require(manager_imf_parse(&font, copy, size) == MANAGER_IMF_BAD_MAGIC,
                "bad magic");
        memcpy(copy, data, size);
        copy[size - 1u] ^= 0x5au;
        require(manager_imf_parse(&font, copy, size) == MANAGER_IMF_BAD_CRC,
                "bad crc");
        memcpy(copy, data, size);
        put_u16(copy + 16, 1u);
        require(manager_imf_parse(&font, copy, size) == MANAGER_IMF_BAD_FLAGS,
                "A4 flag refused");
        /* Swap glyph 1 and 2: unsorted. */
        memcpy(copy, data, size);
        {
            unsigned char temp[16];
            unsigned char *a = copy + MANAGER_IMF_HEADER_BYTES + 16u;
            memcpy(temp, a, 16u);
            memcpy(a, a + 16u, 16u);
            memcpy(a + 16u, temp, 16u);
        }
        refresh_crc(copy, size);
        require(manager_imf_parse(&font, copy, size) == MANAGER_IMF_UNSORTED,
                "unsorted table");
        /* Glyph 5 widened past the atlas. */
        memcpy(copy, data, size);
        copy[MANAGER_IMF_HEADER_BYTES + 5u * 16u + 8u] = 255u;
        put_u16(copy + MANAGER_IMF_HEADER_BYTES + 5u * 16u + 4u, 200u);
        refresh_crc(copy, size);
        require(manager_imf_parse(&font, copy, size) == MANAGER_IMF_BAD_GLYPH,
                "glyph outside atlas");
        free(copy);
        free(data);
    }
    size = synthetic_font(synthetic, 0x3fu, 'A', 'B');
    require(manager_imf_parse(&font, synthetic, size) == MANAGER_IMF_OK,
            "synthetic font parses");
    size = synthetic_font(synthetic, 0x3eu, 'A', 'B');
    require(manager_imf_parse(&font, synthetic, size) == MANAGER_IMF_NO_QUESTION,
            "font without '?' refused");
    require(manager_imf_parse(NULL, synthetic, size) == MANAGER_IMF_NULL, "NULL");
}

static void test_widths(void)
{
    size_t sizes[3];
    unsigned char *body = load_file("font-body-16.imf", &sizes[0]);
    unsigned char *small = load_file("font-small-12.imf", &sizes[1]);
    unsigned char *title = load_file("font-title-24.imf", &sizes[2]);
    struct manager_imf fonts[3];
    struct manager_face face;
    require(manager_imf_parse(&fonts[0], body, sizes[0]) == 0, "body parse");
    require(manager_imf_parse(&fonts[1], small, sizes[1]) == 0, "small parse");
    require(manager_imf_parse(&fonts[2], title, sizes[2]) == 0, "title parse");
    /* Pillow 12.3 / FreeType 2.14.3 getlength() of the same strings with
     * the same TTFs (the baker's toolchain): 140, 107, 230. */
    face = manager_imf_face(&fonts[0]);
    require(manager_face_measure(face, "Launch the game") == 140, "body width");
    require(manager_face_line_height(face) == 19 && manager_face_ascent(face) == 15,
            "body metrics (Pillow getmetrics 15+4)");
    face = manager_imf_face(&fonts[1]);
    require(manager_face_measure(face, "Launch the game") == 107, "small width");
    require(manager_face_line_height(face) == 15, "small line height");
    face = manager_imf_face(&fonts[2]);
    require(manager_face_measure(face, "Launch the game") == 230, "title width");
    require(manager_face_line_height(face) == 29, "title line height");
    /* Missing glyph = '?' advance; two lines measure the widest. */
    face = manager_imf_face(&fonts[0]);
    require(manager_face_measure(face, "\xe4\xb8\x80") == manager_face_measure(face, "?"),
            "missing glyph measures as '?'");
    require(manager_face_measure(face, "ab\nabcd") == manager_face_measure(face, "abcd"),
            "multi-line measure");
    require(manager_face_measure(face, "") == 0, "empty");
    free(body);
    free(small);
    free(title);
}

static void test_utf8(void)
{
    const char *cursor = "\xd0\x9c\xd0\xbe\xd0\xb4\xd1\x8b"; /* Моды */
    require(manager_utf8_next(&cursor) == 0x041cu, "M");
    require(manager_utf8_next(&cursor) == 0x043eu, "o");
    require(manager_utf8_next(&cursor) == 0x0434u, "d");
    require(manager_utf8_next(&cursor) == 0x044bu, "y");
    require(manager_utf8_next(&cursor) == 0u && *cursor == '\0', "end");
    cursor = "\xc0\xafx"; /* overlong '/' */
    require(manager_utf8_next(&cursor) == MANAGER_UTF8_REPLACEMENT, "overlong");
    require(manager_utf8_next(&cursor) == MANAGER_UTF8_REPLACEMENT, "stray continuation");
    require(manager_utf8_next(&cursor) == 'x', "resync");
    cursor = "\xed\xa0\x80"; /* surrogate */
    require(manager_utf8_next(&cursor) == MANAGER_UTF8_REPLACEMENT, "surrogate");
    cursor = "\xe2\x96";      /* truncated at NUL */
    require(manager_utf8_next(&cursor) == MANAGER_UTF8_REPLACEMENT, "truncated");
    require(manager_utf8_length("\xe2\x96\xb8 go") == 4u, "length");
}

#define GUARD 64
#define CW 96
#define CH 40
static uint32_t buffer[GUARD + CW * CH + GUARD];

static void reset_buffer(uint32_t fill)
{
    size_t index;
    for (index = 0u; index < sizeof(buffer) / sizeof(buffer[0]); ++index)
        buffer[index] = 0xdeadbeefu;
    for (index = 0u; index < (size_t)(CW * CH); ++index)
        buffer[GUARD + index] = fill;
}

static void require_guards(const char *message)
{
    size_t index;
    for (index = 0u; index < GUARD; ++index) {
        require(buffer[index] == 0xdeadbeefu, message);
        require(buffer[GUARD + CW * CH + index] == 0xdeadbeefu, message);
    }
}

/* FNV-1a over the canvas words (guards excluded). */
static uint32_t checksum_canvas(void)
{
    uint32_t hash = 2166136261u;
    size_t index;
    for (index = 0u; index < (size_t)(CW * CH); ++index) {
        hash ^= buffer[GUARD + index];
        hash *= 16777619u;
    }
    return hash;
}

static void test_canvas_and_clipping(void)
{
    size_t size;
    unsigned char *body = load_file("font-body-16.imf", &size);
    struct manager_imf font;
    struct manager_face face;
    struct manager_canvas canvas;
    struct manager_builtin_font builtin = {2u};
    int width;
    unsigned int changed = 0u;
    size_t index;

    require(manager_canvas_mix(0xff000000u, 0xffffffffu, 128u) == 0xff808080u,
            "mix half");
    require(manager_canvas_mix(0xff102030u, 0xffffffffu, 0u) == 0xff102030u, "mix 0");
    require(manager_canvas_mix(0xff102030u, 0xff405060u, 255u) == 0xff405060u, "mix 255");
    require(manager_rgb(160u, 32u, 32u) == 0xff2020a0u, "rgb word order 0xAABBGGRR");

    require(manager_imf_parse(&font, body, size) == 0, "parse");
    face = manager_imf_face(&font);
    reset_buffer(0xff000000u);
    manager_canvas_init(&canvas, buffer + GUARD, CW, CH, CW);
    /* Text hanging over every edge. */
    manager_face_draw(face, &canvas, -7, -9, 0xffffffffu, "Wg\xd0\x96");
    manager_face_draw(face, &canvas, CW - 10, CH - 8, 0xffffffffu, "Wg\xd0\x96");
    manager_face_draw(face, &canvas, -30, CH - 6, 0xffffffffu, "Wide text");
    manager_face_draw(face, &canvas, CW - 4, -12, 0xffffffffu, "Wide text");
    manager_canvas_round_box(&canvas, -20, -20, CW + 20, 3, 8, 0xff00ff00u);
    manager_canvas_fill_box(&canvas, CW - 2, CH - 2, CW + 50, CH + 50, 0xff0000ffu);
    require_guards("guard words intact after edge draws");
    for (index = 0u; index < (size_t)(CW * CH); ++index)
        changed += buffer[GUARD + index] != 0xff000000u;
    require(changed > 0u, "something was drawn");

    /* Clip rectangle respected. */
    reset_buffer(0xff000000u);
    manager_canvas_set_clip(&canvas, 10, 10, 20, 10);
    manager_canvas_fill_box(&canvas, 0, 0, CW - 1, CH - 1, 0xffffffffu);
    require(buffer[GUARD + 9 * CW + 15] == 0xff000000u, "above clip untouched");
    require(buffer[GUARD + 10 * CW + 10] == 0xffffffffu, "clip top-left filled");
    require(buffer[GUARD + 19 * CW + 29] == 0xffffffffu, "clip bottom-right filled");
    require(buffer[GUARD + 20 * CW + 29] == 0xff000000u, "below clip untouched");
    require(buffer[GUARD + 19 * CW + 30] == 0xff000000u, "right of clip untouched");
    manager_canvas_reset_clip(&canvas);

    /* Rounded corner r = 8: first row inset 6 (Pillow's rounded_rectangle). */
    reset_buffer(0xff000000u);
    manager_canvas_round_box(&canvas, 2, 2, 37, 20, 8, 0xffffffffu);
    require(buffer[GUARD + 2 * CW + 7] == 0xff000000u &&
                buffer[GUARD + 2 * CW + 8] == 0xffffffffu,
            "corner row 0 inset 6");
    require(buffer[GUARD + 8 * CW + 2] == 0xffffffffu, "corner row 6 inset 0");
    require_guards("guards after round box");

    /* Ellipsis: fits in max width, ends with the '…' glyph. */
    reset_buffer(0xff000000u);
    width = manager_face_draw_clipped(face, &canvas, 0, 0, 0xffffffffu,
                                      "External Item Descriptions", 80);
    require(width <= 80 && width > 40, "clipped width within limit");
    require(manager_face_draw_clipped(face, NULL, 0, 0, 0u, "ab", 80) ==
                manager_face_measure(face, "ab"),
            "short text unclipped");
    require(manager_face_fits(face, "ab", 80) && !manager_face_fits(face, "External Item Descriptions", 80),
            "fits");

    /* 5x7 fallback: 12 px advance at scale 2, lower case folded. */
    face = manager_builtin_face(&builtin);
    require(manager_face_measure(face, "Abc") == 36, "builtin width");
    require(manager_face_line_height(face) == 16, "builtin line");
    require(face.ops->has_glyph(face.impl, 'q') && !face.ops->has_glyph(face.impl, 0x416u),
            "builtin coverage");
    reset_buffer(0xff000000u);
    manager_face_draw(face, &canvas, 0, 0, 0xffffffffu, "A");
    require(buffer[GUARD + 0 * CW + 2] == 0xffffffffu, "builtin 'A' top row pixel (0x0e)");
    require(buffer[GUARD + 0 * CW + 0] == 0xff000000u, "builtin 'A' top-left blank");
    /* UI2 separators and button glyphs map onto the verbatim table; the
     * ellipsis stays uncovered so clipping falls back to "...". */
    require(face.ops->has_glyph(face.impl, 0xd7u) &&
                face.ops->has_glyph(face.impl, 0xb7u) &&
                face.ops->has_glyph(face.impl, 0x25cbu) &&
                !face.ops->has_glyph(face.impl, 0x2026u),
            "builtin substitutions");
    {
        uint32_t plain;
        reset_buffer(0xff000000u);
        manager_face_draw(face, &canvas, 0, 0, 0xffffffffu, "X-O>");
        plain = checksum_canvas();
        reset_buffer(0xff000000u);
        manager_face_draw(face, &canvas, 0, 0, 0xffffffffu,
                          "\xc3\x97\xc2\xb7\xe2\x97\x8b\xe2\x80\xba");
        require(checksum_canvas() == plain, "builtin '×·○›' draws as 'X-O>'");
    }
    require(manager_face_draw_clipped(face, &canvas, 0, 20, 0xffffffffu,
                                      "LONG TEXT HERE", 60) <= 60,
            "builtin ellipsis uses dots");
    require_guards("guards after builtin");
    free(body);
}

int main(void)
{
    test_parse_goldens();
    test_widths();
    test_utf8();
    test_canvas_and_clipping();
    printf("manager font host tests: PASS (%u checks)\n", checks);
    return 0;
}
