#ifndef ISAAC_MANAGER_BMFONT_H
#define ISAAC_MANAGER_BMFONT_H

/*
 * Binary BMFont v3 reader and A8 text renderer for the manager's game skin
 * (theme=game): the game's
 * TeamMeatFont10/12/16/20Bold and Upheaval faces, read from the user's
 * archives at run time (never shipped).  SDK-free, no allocation.
 *
 * File layout (copied from the design's asset probe and mock-up renderer,
 * both of which decoded the real files): "BMF" + version
 * byte 3, then blocks of u8 type, u32 size, payload:
 *   1 info    int16 font size at 0 (face name at 14, not needed)
 *   2 common  "<HHHHH": line height, base, scale width, scale height, pages
 *   3 pages   NUL-terminated page file names
 *   4 chars   20-byte rows "<IHHHHhhhBB": id, x, y, width, height,
 *             xoffset, yoffset, xadvance, page, channel
 *   5 kerning pairs: skipped (the Team Meat files carry an empty block 5,
 *             Upheaval none)
 * Supported subset: one page, every glyph on page 0 with channel 15 (all
 * channels) - what every font the skin reads uses.  The game's pages are
 * white-only (measured: 23,787 of 23,787 opaque texels of
 * TeamMeatFont16_0.png are 255,255,255), so the page is kept as A8.
 *
 * Drawing: glyph alpha scaled by the colour's alpha, blended over 32-bit
 * canvas words 0xAABBGGRR (SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) as
 * out = (src * a + dst * (255 - a) + 127) / 255 per channel (the alpha
 * channel treats the source as 255), integer nearest-neighbour scale
 * (every page texel becomes a scale x scale block, like the game's 2x
 * menus), clipped to the surface's clip rectangle.  Text is UTF-8; a
 * codepoint without a glyph draws the font's '?' glyph (nothing when the
 * font has none).  Pen positions are the top of the line: a glyph is drawn
 * at (x + xoffset * scale, y + yoffset * scale); the baseline is at
 * y + base * scale; the pen advances by xadvance * scale.
 */

#include <stddef.h>
#include <stdint.h>

#define MANAGER_BMFONT_PAGE_NAME_CAPACITY 64u
#define MANAGER_BMFONT_MAX_SCALE 8u
/* length argument meaning "up to the terminating NUL". */
#define MANAGER_BMFONT_NUL_TERMINATED ((size_t)-1)
#define MANAGER_BMFONT_REPLACEMENT 0xFFFDu

enum manager_bmfont_status {
    MANAGER_BMFONT_OK = 0,
    MANAGER_BMFONT_E_ARGUMENT = -1,
    MANAGER_BMFONT_E_SIGNATURE = -2,   /* not "BMF" version 3 */
    MANAGER_BMFONT_E_BLOCK = -3,       /* block crosses the end, too short,
                                          repeated, unknown type, missing
                                          block 1-4, trailing bytes */
    MANAGER_BMFONT_E_UNSUPPORTED = -4, /* pages != 1, glyph page != 0,
                                          channel != 15 */
    MANAGER_BMFONT_E_GLYPH = -5,       /* glyph outside the page, duplicate
                                          codepoint, zero-sized page */
    MANAGER_BMFONT_E_CAPACITY = -6,    /* glyph storage too small */
    MANAGER_BMFONT_E_PAGE = -7         /* page size differs from block 2 */
};

struct manager_bmfont_glyph {
    uint32_t codepoint;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    int16_t xoffset;
    int16_t yoffset;
    int16_t xadvance;
    uint8_t page;
    uint8_t channel;
};

struct manager_bmfont {
    int16_t font_size;
    uint16_t line_height;
    uint16_t base;
    uint16_t scale_w;
    uint16_t scale_h;
    uint16_t pages;
    char page_name[MANAGER_BMFONT_PAGE_NAME_CAPACITY];
    struct manager_bmfont_glyph *glyphs; /* caller storage, sorted */
    uint32_t glyph_count;
    const struct manager_bmfont_glyph *fallback; /* '?' or NULL */
    const uint8_t *page; /* A8, scale_w * scale_h, NULL until attached */
};

/* A 32-bit canvas (pitch in pixels) and the clip rectangle drawing honours,
 * [clip_x0, clip_x1) x [clip_y0, clip_y1), always intersected with the
 * surface.  manager_bmfont_surface_init() clips to the whole surface. */
struct manager_bmfont_surface {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    int32_t clip_x0;
    int32_t clip_y0;
    int32_t clip_x1;
    int32_t clip_y1;
};

/* Parses the .fnt bytes; glyphs are copied into the caller's storage and
 * sorted by codepoint, so the .fnt buffer may be released afterwards. */
int manager_bmfont_parse(struct manager_bmfont *font, const uint8_t *fnt,
                         size_t size, struct manager_bmfont_glyph *glyphs,
                         uint32_t capacity);

/* Glyph rows in a .fnt (block 4 size / 20) without parsing the rest; 0 on
 * any error.  Lets the caller size the glyph storage. */
uint32_t manager_bmfont_glyph_rows(const uint8_t *fnt, size_t size);

/* RGBA8 page (manager_png_decode output) -> A8, alpha channel only; a8 may
 * be the rgba buffer itself (in place).  *non_white (optional) counts texels
 * with alpha > 0 whose colour is not 255,255,255, i.e. what A8 loses. */
void manager_bmfont_rgba_to_a8(uint8_t *a8, const uint8_t *rgba,
                               uint32_t pixels, uint32_t *non_white);

/* The A8 page must be scale_w x scale_h and outlive the font's use. */
int manager_bmfont_attach_page(struct manager_bmfont *font, const uint8_t *a8,
                               uint32_t width, uint32_t height);

/* Next codepoint of UTF-8 text, advancing *cursor; end == NULL means
 * NUL-terminated (returns 0 at the NUL without advancing).  Malformed,
 * overlong, surrogate or > U+10FFFF sequences yield U+FFFD and consume one
 * byte. */
uint32_t manager_bmfont_utf8_next(const char **cursor, const char *end);

const struct manager_bmfont_glyph *
manager_bmfont_find(const struct manager_bmfont *font, uint32_t codepoint);

/* manager_bmfont_find() or the '?' fallback (may still be NULL). */
const struct manager_bmfont_glyph *
manager_bmfont_glyph_or_fallback(const struct manager_bmfont *font,
                                 uint32_t codepoint);

uint32_t manager_bmfont_line_height(const struct manager_bmfont *font,
                                    uint32_t scale);

/* Sum of the advances (fallback glyph for missing codepoints) at `scale`;
 * length in bytes or MANAGER_BMFONT_NUL_TERMINATED.  scale 0 or above
 * MANAGER_BMFONT_MAX_SCALE measures 0. */
int32_t manager_bmfont_measure(const struct manager_bmfont *font,
                               const char *text, size_t length,
                               uint32_t scale);

void manager_bmfont_surface_init(struct manager_bmfont_surface *surface,
                                 uint32_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t pitch);

/* One glyph at pen (x, y); returns its advance (xadvance * scale).  A NULL
 * glyph draws nothing and advances 0. */
int32_t manager_bmfont_draw_glyph(const struct manager_bmfont *font,
                                  const struct manager_bmfont_surface *surface,
                                  int32_t x, int32_t y,
                                  const struct manager_bmfont_glyph *glyph,
                                  uint32_t color, uint32_t scale);

/* Draws UTF-8 text starting at pen (x, y); returns the pen x after the
 * text.  Needs an attached page (otherwise only advances). */
int32_t manager_bmfont_draw(const struct manager_bmfont *font,
                            const struct manager_bmfont_surface *surface,
                            int32_t x, int32_t y, const char *text,
                            size_t length, uint32_t color, uint32_t scale);

/* Stable short name of a status ("ok", "signature", "block", ...). */
const char *manager_bmfont_status_text(int status);

#endif
