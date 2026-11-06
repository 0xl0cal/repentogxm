/*
 * Manager UI2 software canvas.  See manager_canvas.h for the contract.
 * No SDK, no allocator, no floating point.
 */
#include "manager_canvas.h"

#if defined(MANAGER_UI2)

#include <stddef.h>

void manager_canvas_init(struct manager_canvas *canvas, uint32_t *pixels,
                         int width, int height, int pitch)
{
    if (canvas == NULL)
        return;
    canvas->pixels = pixels;
    canvas->width = width > 0 ? width : 0;
    canvas->height = height > 0 ? height : 0;
    canvas->pitch = pitch >= canvas->width ? pitch : canvas->width;
    manager_canvas_reset_clip(canvas);
}

void manager_canvas_reset_clip(struct manager_canvas *canvas)
{
    canvas->clip.x = 0;
    canvas->clip.y = 0;
    canvas->clip.w = canvas->width;
    canvas->clip.h = canvas->height;
}

void manager_canvas_set_clip(struct manager_canvas *canvas, int x, int y,
                             int w, int h)
{
    int x1 = x + w;
    int y1 = y + h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > canvas->width)
        x1 = canvas->width;
    if (y1 > canvas->height)
        y1 = canvas->height;
    canvas->clip.x = x;
    canvas->clip.y = y;
    canvas->clip.w = x1 > x ? x1 - x : 0;
    canvas->clip.h = y1 > y ? y1 - y : 0;
}

uint32_t manager_canvas_mix(uint32_t dst, uint32_t color, unsigned int alpha)
{
    uint32_t out = 0xff000000u;
    unsigned int shift;
    if (alpha >= 255u)
        return color | 0xff000000u;
    if (alpha == 0u)
        return dst | 0xff000000u;
    for (shift = 0u; shift < 24u; shift += 8u) {
        const unsigned int d = (unsigned int)(dst >> shift) & 0xffu;
        const unsigned int c = (unsigned int)(color >> shift) & 0xffu;
        const unsigned int v = (d * (255u - alpha) + c * alpha + 127u) / 255u;
        out |= (uint32_t)v << shift;
    }
    return out;
}

/* Clip an inclusive span to the clip rectangle; 0 when empty. */
static int clip_span(const struct manager_canvas *canvas, int *x0, int *x1,
                     int y)
{
    const int left = canvas->clip.x;
    const int right = canvas->clip.x + canvas->clip.w - 1;
    if (canvas->pixels == NULL || y < canvas->clip.y ||
        y >= canvas->clip.y + canvas->clip.h)
        return 0;
    if (*x0 < left)
        *x0 = left;
    if (*x1 > right)
        *x1 = right;
    return *x0 <= *x1;
}

static void span_fill(struct manager_canvas *canvas, int x0, int x1, int y,
                      uint32_t color)
{
    uint32_t *row;
    int x;
    if (!clip_span(canvas, &x0, &x1, y))
        return;
    row = canvas->pixels + (size_t)y * (size_t)canvas->pitch;
    for (x = x0; x <= x1; ++x)
        row[x] = color;
}

static void span_blend(struct manager_canvas *canvas, int x0, int x1, int y,
                       uint32_t color, unsigned int alpha)
{
    uint32_t *row;
    int x;
    if (!clip_span(canvas, &x0, &x1, y))
        return;
    row = canvas->pixels + (size_t)y * (size_t)canvas->pitch;
    for (x = x0; x <= x1; ++x)
        row[x] = manager_canvas_mix(row[x], color, alpha);
}

void manager_canvas_fill_box(struct manager_canvas *canvas, int x0, int y0,
                             int x1, int y1, uint32_t color)
{
    int y;
    if (canvas == NULL)
        return;
    for (y = y0; y <= y1; ++y)
        span_fill(canvas, x0, x1, y, color);
}

void manager_canvas_blend_box(struct manager_canvas *canvas, int x0, int y0,
                              int x1, int y1, uint32_t color,
                              unsigned int alpha)
{
    int y;
    if (canvas == NULL)
        return;
    for (y = y0; y <= y1; ++y)
        span_blend(canvas, x0, x1, y, color, alpha);
}

/* Horizontal inset of corner row i (0 = the box edge) for radius r: the
 * smallest d with (2r-2d-1)^2 + (2r-2i-1)^2 <= 4r^2 - 2r - 2.  The bound
 * was fitted so r = 8 reproduces the corner Pillow's rounded_rectangle
 * drew for the design mock-ups (insets 6 4 3 2 1 1 0, measured with Pillow
 * 12.3 on a 36x19 box). */
static int corner_inset(int r, int i)
{
    const long limit = 4L * r * r - 2L * r - 2L;
    const long dy = 2L * r - 2L * i - 1L;
    int d;
    if (r <= 0 || i >= r)
        return 0;
    for (d = 0; d < r; ++d) {
        const long dx = 2L * r - 2L * d - 1L;
        if (dx * dx + dy * dy <= limit)
            return d;
    }
    return r;
}

void manager_canvas_round_box(struct manager_canvas *canvas, int x0, int y0,
                              int x1, int y1, int radius, uint32_t color)
{
    int y;
    int height;
    if (canvas == NULL || x1 < x0 || y1 < y0)
        return;
    height = y1 - y0 + 1;
    if (radius * 2 > height)
        radius = height / 2;
    if (radius * 2 > x1 - x0 + 1)
        radius = (x1 - x0 + 1) / 2;
    for (y = y0; y <= y1; ++y) {
        const int from_top = y - y0;
        const int from_bottom = y1 - y;
        const int i = from_top < from_bottom ? from_top : from_bottom;
        const int d = corner_inset(radius, i);
        span_fill(canvas, x0 + d, x1 - d, y, color);
    }
}

void manager_canvas_round_frame(struct manager_canvas *canvas, int x0, int y0,
                                int x1, int y1, int radius, int thickness,
                                uint32_t color)
{
    int y;
    int inner_radius;
    if (canvas == NULL || x1 < x0 || y1 < y0 || thickness <= 0)
        return;
    if (radius * 2 > y1 - y0 + 1)
        radius = (y1 - y0 + 1) / 2;
    inner_radius = radius > thickness ? radius - thickness : 0;
    for (y = y0; y <= y1; ++y) {
        const int from_top = y - y0;
        const int from_bottom = y1 - y;
        const int i = from_top < from_bottom ? from_top : from_bottom;
        const int d = corner_inset(radius, i);
        if (i < thickness) {
            span_fill(canvas, x0 + d, x1 - d, y, color);
        } else {
            const int di = corner_inset(inner_radius, i - thickness);
            span_fill(canvas, x0 + d, x0 + thickness + di - 1, y, color);
            span_fill(canvas, x1 - thickness - di + 1, x1 - d, y, color);
        }
    }
}

void manager_canvas_hline(struct manager_canvas *canvas, int x0, int x1,
                          int y, int thickness, uint32_t color)
{
    int row;
    if (canvas == NULL)
        return;
    for (row = 0; row < thickness; ++row)
        span_fill(canvas, x0, x1, y + row, color);
}

void manager_canvas_blit_a8(struct manager_canvas *canvas, int x, int y,
                            const uint8_t *alpha, int w, int h, int stride,
                            uint32_t color)
{
    int row;
    if (canvas == NULL || canvas->pixels == NULL || alpha == NULL)
        return;
    for (row = 0; row < h; ++row) {
        const int yy = y + row;
        int x0 = x;
        int x1 = x + w - 1;
        const uint8_t *src;
        uint32_t *dst;
        int xx;
        if (!clip_span(canvas, &x0, &x1, yy))
            continue;
        src = alpha + (size_t)row * (size_t)stride;
        dst = canvas->pixels + (size_t)yy * (size_t)canvas->pitch;
        for (xx = x0; xx <= x1; ++xx) {
            const unsigned int a = src[xx - x];
            if (a != 0u)
                dst[xx] = manager_canvas_mix(dst[xx], color, a);
        }
    }
}

void manager_canvas_blit_rgba(struct manager_canvas *canvas, int x, int y,
                              const uint32_t *pixels, int w, int h,
                              int stride)
{
    int row;
    if (canvas == NULL || canvas->pixels == NULL || pixels == NULL)
        return;
    for (row = 0; row < h; ++row) {
        const int yy = y + row;
        int x0 = x;
        int x1 = x + w - 1;
        const uint32_t *src;
        uint32_t *dst;
        int xx;
        if (!clip_span(canvas, &x0, &x1, yy))
            continue;
        src = pixels + (size_t)row * (size_t)stride;
        dst = canvas->pixels + (size_t)yy * (size_t)canvas->pitch;
        for (xx = x0; xx <= x1; ++xx) {
            const uint32_t word = src[xx - x];
            const unsigned int a = (unsigned int)(word >> 24);
            if (a != 0u)
                dst[xx] = manager_canvas_mix(dst[xx], word, a);
        }
    }
}

#endif /* MANAGER_UI2 */
