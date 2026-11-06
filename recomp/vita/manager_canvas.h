/*
 * Manager UI2 software canvas (pixels for the display list painter,
 * knob ISAAC_VITA_MANAGER_UI2 -> per-target MANAGER_UI2=1).
 *
 * Plain rectangle / line / alpha-blit primitives over a caller-owned word
 * buffer; owns no memory, calls no SDK.  The pixel word is the manager's
 * framebuffer layout SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 = 0xAABBGGRR
 * (manager_main.c), so a finished canvas is memcpy'd to the display buffer
 * by manager_present() unchanged.  Every primitive clips to the canvas clip
 * rectangle, which never exceeds the buffer: nothing outside
 * [0, width) x [0, height) is ever written (the render host test puts guard
 * words around its buffer and checks them).
 *
 * Box coordinates are inclusive on both ends, like Pillow's
 * ImageDraw.rectangle((x0, y0, x1, y1)) that drew the design mock-ups
 * (a Pillow renderer, not part of this repository), so the mock's numbers are
 * used verbatim.
 */
#ifndef ISAAC_MANAGER_CANVAS_H
#define ISAAC_MANAGER_CANVAS_H

#if defined(MANAGER_UI2)

#include <stdint.h>

struct manager_rect {
    int x;
    int y;
    int w;
    int h;
};

struct manager_canvas {
    uint32_t *pixels; /* 0xAABBGGRR words */
    int width;
    int height;
    int pitch; /* in words */
    struct manager_rect clip;
};

/* 0xAABBGGRR word from an sRGB triple (alpha 255). */
static inline uint32_t __attribute__((unused)) manager_rgb(unsigned int r,
                                                           unsigned int g,
                                                           unsigned int b)
{
    return 0xff000000u | ((uint32_t)(b & 0xffu) << 16) |
           ((uint32_t)(g & 0xffu) << 8) | (uint32_t)(r & 0xffu);
}

void manager_canvas_init(struct manager_canvas *canvas, uint32_t *pixels,
                         int width, int height, int pitch);
/* Clip to the intersection of the rectangle and the buffer. */
void manager_canvas_set_clip(struct manager_canvas *canvas, int x, int y,
                             int w, int h);
void manager_canvas_reset_clip(struct manager_canvas *canvas);

/* dst = lerp(dst, color, alpha / 255) on R, G, B; the result is opaque. */
uint32_t manager_canvas_mix(uint32_t dst, uint32_t color, unsigned int alpha);

/* Inclusive boxes (Pillow rectangle semantics). */
void manager_canvas_fill_box(struct manager_canvas *canvas, int x0, int y0,
                             int x1, int y1, uint32_t color);
void manager_canvas_blend_box(struct manager_canvas *canvas, int x0, int y0,
                              int x1, int y1, uint32_t color,
                              unsigned int alpha);
/* Rounded filled box, corner radius in pixels (0 = square). */
void manager_canvas_round_box(struct manager_canvas *canvas, int x0, int y0,
                              int x1, int y1, int radius, uint32_t color);
/* Rounded outline of the given thickness. */
void manager_canvas_round_frame(struct manager_canvas *canvas, int x0, int y0,
                                int x1, int y1, int radius, int thickness,
                                uint32_t color);
/* Horizontal line x0..x1 (inclusive), rows y .. y+thickness-1 (Pillow
 * ImageDraw.line(..., width=2) on a horizontal segment fills rows y, y+1). */
void manager_canvas_hline(struct manager_canvas *canvas, int x0, int x1,
                          int y, int thickness, uint32_t color);
/* 8-bit coverage mask blended in `color`. */
void manager_canvas_blit_a8(struct manager_canvas *canvas, int x, int y,
                            const uint8_t *alpha, int w, int h, int stride,
                            uint32_t color);
/* Straight-alpha 0xAABBGGRR image blended over the canvas. */
void manager_canvas_blit_rgba(struct manager_canvas *canvas, int x, int y,
                              const uint32_t *pixels, int w, int h,
                              int stride);

#endif /* MANAGER_UI2 */

#endif
