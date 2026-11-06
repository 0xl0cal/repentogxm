/*
 * Manager UI2 text (proportional DejaVu faces with Cyrillic; knob
 * ISAAC_VITA_MANAGER_UI2 -> MANAGER_UI2=1).
 *
 * A face is a tiny vtable (line height, ascent, per-glyph advance and draw)
 * so the page code never knows which rasteriser it talks to:
 *
 *   * IMF1 atlases baked by tools/manager_font_bake.py (DejaVu, shipped as
 *     app0:/manager/font-*.imf): manager_imf_parse() validates the whole
 *     file (sizes, CRC-32, sorted table, glyph boxes inside the atlas, '?'
 *     present) before manager_imf_face() hands out a face;
 *   * the 5x7 upper-case bitmap of the pre-UI2 manager (tables copied from
 *     manager_main.c), wrapped as a degenerate face: the fallback when a
 *     font file is missing or corrupt, so the manager always has text;
 *   * later, a BMFont face (the game skin, knob
 *     ISAAC_VITA_MANAGER_UI2_GAME_SKIN) implementing the same ops.
 *
 * Text is UTF-8; a code point the face lacks draws as its '?' glyph.  The
 * pen position is (x, y) with y on the ascender line (Pillow's "la" anchor,
 * which is what the design mock renderer used for every string), so mock
 * coordinates carry over unchanged.  '\n' starts a new line one line height
 * down.  No kerning.  SDK-free, no allocator.
 */
#ifndef ISAAC_MANAGER_FONT_H
#define ISAAC_MANAGER_FONT_H

#if defined(MANAGER_UI2)

#include "manager_canvas.h"

#include <stddef.h>
#include <stdint.h>

#define MANAGER_IMF_HEADER_BYTES 28u
#define MANAGER_IMF_GLYPH_BYTES 16u
#define MANAGER_UTF8_REPLACEMENT 0xfffdu
#define MANAGER_ELLIPSIS 0x2026u

enum manager_imf_status {
    MANAGER_IMF_OK = 0,
    MANAGER_IMF_NULL = -1,
    MANAGER_IMF_TRUNCATED = -2,
    MANAGER_IMF_BAD_MAGIC = -3,
    MANAGER_IMF_BAD_SIZE = -4,
    MANAGER_IMF_BAD_FLAGS = -5,
    MANAGER_IMF_BAD_CRC = -6,
    MANAGER_IMF_UNSORTED = -7,
    MANAGER_IMF_BAD_GLYPH = -8,
    MANAGER_IMF_NO_QUESTION = -9,
};

/* A validated view into caller-owned IMF1 bytes (nothing is copied). */
struct manager_imf {
    const uint8_t *table;
    const uint8_t *atlas;
    uint32_t atlas_bytes;
    uint16_t px;
    uint16_t line_h;
    uint16_t ascent;
    uint16_t atlas_w;
    uint16_t atlas_h;
    uint16_t glyph_count;
    uint16_t question; /* table index of U+003F */
};

struct manager_face_ops {
    int (*line_height)(const void *impl);
    int (*ascent)(const void *impl);
    int (*has_glyph)(const void *impl, uint32_t codepoint);
    /* Advance of one code point (the '?' advance when missing). */
    int (*advance)(const void *impl, uint32_t codepoint);
    /* Draw one code point with the pen at (x, y = ascender line); returns
     * the advance.  canvas may be NULL (measure only). */
    int (*draw_glyph)(const void *impl, struct manager_canvas *canvas, int x,
                      int y, uint32_t codepoint, uint32_t color);
};

struct manager_face {
    const struct manager_face_ops *ops;
    const void *impl;
};

/* The 5x7 fallback; scale 1..4 (advance 6*scale, line 8*scale). */
struct manager_builtin_font {
    unsigned int scale;
};

uint32_t manager_crc32(const void *data, size_t size);
int manager_imf_parse(struct manager_imf *font, const void *data, size_t size);
/* Table index of the code point, or -1. */
int manager_imf_find(const struct manager_imf *font, uint32_t codepoint);
struct manager_face manager_imf_face(const struct manager_imf *font);
struct manager_face manager_builtin_face(const struct manager_builtin_font *font);
int manager_face_valid(struct manager_face face);

/* Next code point of a NUL-terminated UTF-8 string (0 at the end; the
 * cursor then stays on the NUL).  Malformed, overlong, surrogate or
 * out-of-range sequences yield U+FFFD and consume one byte. */
uint32_t manager_utf8_next(const char **cursor);
/* Code points in the string. */
size_t manager_utf8_length(const char *text);

int manager_face_line_height(struct manager_face face);
int manager_face_ascent(struct manager_face face);
/* Width of the widest line. */
int manager_face_measure(struct manager_face face, const char *text);
/* Draws the text; returns the width of the widest line. */
int manager_face_draw(struct manager_face face, struct manager_canvas *canvas,
                      int x, int y, uint32_t color, const char *text);
/* Single line, cut to max_width with an ellipsis ('…', or "..." when the
 * face lacks it) when it does not fit.  Returns the drawn width. */
int manager_face_draw_clipped(struct manager_face face,
                              struct manager_canvas *canvas, int x, int y,
                              uint32_t color, const char *text,
                              int max_width);
/* 1 when the single-line text fits max_width without an ellipsis. */
int manager_face_fits(struct manager_face face, const char *text,
                      int max_width);

#endif /* MANAGER_UI2 */

#endif
