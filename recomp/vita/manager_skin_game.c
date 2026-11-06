/*
 * Manager UI2 game skin: design variant B on C's page model (see
 * manager_skin_game.h for the contract and the context).
 *
 * What is read (the skin's asset table; paths hashed with
 * the archive prefixes of manager_arch.c, first archive of
 * manager_arch_default_order holding a path wins):
 *
 *   gfx/ui/main menu/EmptyScreen.png    480x270  backdrop of every page,
 *                                                pixel-doubled
 *   gfx/ui/main menu/KeyConfigMenu.png  512x256  the big paper sheet
 *                                                (5,2,404,190) under the page
 *                                                and the strip (4,201,225,52)
 *                                                for the SAVES cards
 *   gfx/ui/GenericPopup.png             290x192  the paper (40,41,231,129) of
 *                                                the confirmation box and the
 *                                                small ink cursor (2,41,12,15)
 *   gfx/ui/Buttons.png                  320x256  the Buttons.anm2 `Vita`
 *                                                frames of the hint bar
 *   font/TeamMeatFont16.fnt + page               body text (rows and values)
 *   font/TeamMeatFont12.fnt + page               small text, the title at 2x
 *
 * Crops: bounding boxes measured on the PC game's sheets and popup, the anm2
 * (frame origins of the `Vita` animation); the alpha boxes inside the
 * 32x16 frames and the cursor box were measured on the PC game's files
 * with a probe script that is not part of this repository.
 * Expected sheet sizes: the same files.  Ink colours: INK and
 * INK_SOFT of the design mock-ups (variant B).
 *
 * The Team Meat faces carry the whole Russian alphabet (251 glyphs, 68 of
 * them in U+0400-04FF), so Russian text renders in
 * them; a code point a face lacks (U+25B8, U+00D7, U+2014, ...) is drawn
 * per glyph by the DejaVu face of the same role with the baselines lined
 * up.
 *
 * The layout is manager_ui.c's game layout (MANAGER_DL_GAME_* in
 * manager_ui_draw.h, owner's device review 2026-10-10): the paper covers
 * the header, the page body and the status line, only the hint bar sits on
 * the backdrop below it; rows, bands and the cursor stay inside the paper's
 * inner rectangle (x 30..927, y 24..463, torn edge excluded).  The
 * smallest face is TeamMeatFont12: TeamMeatFont10 at 1x was unreadable on
 * the device.  The backdrop and the
 * paper are composed once at half resolution (the art is drawn for the
 * game's 480x270 menus and shown pixel-doubled) and expanded 2x per frame.
 */
#if defined(MANAGER_UI2) && defined(MANAGER_UI2_GAME_SKIN)

#include "manager_skin_game.h"

#include "manager_arch.h"
#include "manager_bmfont.h"
#include "manager_canvas.h"
#include "manager_font.h"
#include "manager_png.h"

#include <stdio.h>
#include <string.h>

#define SKIN_W 960
#define SKIN_H 544
#define SKIN_HALF_W (SKIN_W / 2)
#define SKIN_HALF_H (SKIN_H / 2)

/* Geometry of the PC game's files the skin reads. */
#define SKIN_BACKDROP_PATH "gfx/ui/main menu/EmptyScreen.png"
#define SKIN_BACKDROP_W 480u
#define SKIN_BACKDROP_H 270u
#define SKIN_SHEET_PATH "gfx/ui/main menu/KeyConfigMenu.png"
#define SKIN_SHEET_W 512u
#define SKIN_SHEET_H 256u
#define SKIN_POPUP_PATH "gfx/ui/GenericPopup.png"
#define SKIN_POPUP_W 290u
#define SKIN_POPUP_H 192u
#define SKIN_BUTTONS_PATH "gfx/ui/Buttons.png"
#define SKIN_BUTTONS_W 320u
#define SKIN_BUTTONS_H 256u

#define SKIN_FONT_COUNT 2u
#define SKIN_FONT_GLYPHS_MAX 2048u
#define SKIN_FONT_PAGE_TEXELS_MAX (512u * 512u)
#define SKIN_PATH_CAPACITY 192u

/* Paper under the page, half-resolution inclusive box: full resolution
 * x 8..951, y 4..497 - the header, every page body and the status line
 * (MANAGER_DL_GAME_STATUS_Y 442); the torn edge and shadow take about 22
 * px left, 24 right, 20 top and 34 bottom, so the inner rectangle is x
 * 30..927, y 24..463.  The hint bar (y 510) stays on the backdrop. */
#define SKIN_PAPER_X0 4
#define SKIN_PAPER_Y0 2
#define SKIN_PAPER_X1 475
#define SKIN_PAPER_Y1 248
/* Nine-slice corners (source texels kept 1:1; the middle stretches). */
#define SKIN_SHEET_CORNER_X 24
#define SKIN_SHEET_CORNER_Y 20
#define SKIN_STRIP_CORNER_X 12
#define SKIN_STRIP_CORNER_Y 12
#define SKIN_POPUP_CORNER_X 20
#define SKIN_POPUP_CORNER_Y 20

/* The confirmation box paper reaches past the layout box by its own torn
 * edge and shadow (about 3 and 6-8 texels of the crop, doubled), so the
 * text and the ○/× pair at the box's bottom stay on the paper. */
#define SKIN_BOX_PAD_LEFT 8
#define SKIN_BOX_PAD_TOP 8
#define SKIN_BOX_PAD_RIGHT 14
#define SKIN_BOX_PAD_BOTTOM 18
/* Selected row: ink wash over the row box and the cursor inside its left
 * end (the band starts inside the paper, MANAGER_DL_GAME_BAND_X0), pixel-
 * doubled like the game's own menus. */
#define SKIN_WASH_ALPHA 22u
#define SKIN_CURSOR_X 4
#define SKIN_CURSOR_SCALE 2
#define SKIN_ROW_TEXT_DY 5 /* MANAGER_DL_GAME_ROW_TEXT_DY */
/* Selected SAVES card: ink wash over the paper texels. */
#define SKIN_CARD_WASH_ALPHA 40u

/* Design mock-up ink: INK (47,33,33), INK_SOFT (120,100,100). */
#define SKIN_INK_R 47u
#define SKIN_INK_G 33u
#define SKIN_INK_B 33u
#define SKIN_INK_SOFT_R 120u
#define SKIN_INK_SOFT_G 100u
#define SKIN_INK_SOFT_B 100u

struct skin_rect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
};

/* Sprite bounding boxes of KeyConfigMenu.png and GenericPopup.png; cursor:
 * the GenericPopup component of 122
 * texels, the same box as SaveSelectMenu's). */
static const struct skin_rect skin_sheet_crop = {5, 2, 404, 190};
static const struct skin_rect skin_strip_crop = {4, 201, 225, 52};
static const struct skin_rect skin_popup_crop = {40, 41, 231, 129};
static const struct skin_rect skin_cursor_crop = {2, 41, 12, 15};

/* Buttons.png: the `Vita` animation's 32x16 frames (origins:
 * frames 4-7 at (128, 64/80/96/112) from the anm2), cut to the alpha box
 * measured inside each frame. */
enum skin_sprite_id {
    SKIN_SPRITE_TRIANGLE = 0, /* frame 0 at (128,0) */
    SKIN_SPRITE_CROSS,        /* frame 1 at (128,16) */
    SKIN_SPRITE_CIRCLE,       /* frame 2 at (128,32) */
    SKIN_SPRITE_SQUARE,       /* frame 3 at (128,48) */
    SKIN_SPRITE_START,        /* frame 14 at (160,96) */
    SKIN_SPRITE_UP_DOWN,      /* frames 5 (up) over 4 (down) */
    SKIN_SPRITE_LEFT_RIGHT,   /* frames 7 (left) beside 6 (right) */
    SKIN_SPRITE_L_R,          /* frames 10 (L) and 11 (R) */
    SKIN_SPRITE_COUNT
};
static const struct skin_rect skin_button_box[5] = {
    {136, 2, 15, 11},  /* triangle */
    {136, 18, 15, 11}, /* cross */
    {136, 34, 15, 11}, /* circle */
    {136, 50, 15, 11}, /* square */
    {167, 97, 19, 13}, /* START */
};
static const struct skin_rect skin_dpad_down = {137, 65, 14, 14};  /* frame 4 */
static const struct skin_rect skin_dpad_up = {137, 81, 14, 14};    /* frame 5 */
static const struct skin_rect skin_dpad_right = {137, 97, 14, 14}; /* frame 6 */
static const struct skin_rect skin_dpad_left = {137, 113, 14, 14}; /* frame 7 */
static const struct skin_rect skin_button_l = {170, 36, 13, 9};    /* frame 10 */
static const struct skin_rect skin_button_r = {170, 52, 13, 9};    /* frame 11 */
#define SKIN_L_R_GAP 2

/* Team Meat faces: TITLE, BODY, SMALL (enum manager_face_role order).
 * BODY is TeamMeatFont16 at 1x, about the size of the game's own
 * TeamMeatFont10 shown 2x on the Vita, the smallest text the game draws;
 * SMALL is TeamMeatFont12, one step below. */
static const char *const skin_font_paths[SKIN_FONT_COUNT] = {
    "font/TeamMeatFont12.fnt",
    "font/TeamMeatFont16.fnt",
};
struct skin_face_spec {
    uint8_t font;
    uint8_t scale;
    int8_t y_bias;
};
static const struct skin_face_spec skin_face_specs[MANAGER_FACE_COUNT] = {
    {0u, 2u, -4}, /* title: TeamMeatFont12 pixel-doubled like the game */
    {1u, 1u, 0},  /* body: TeamMeatFont16 */
    {0u, 1u, 0},  /* small: TeamMeatFont12 */
};

/* Check boxes (no art for them in the archives the skin reads): 9x9 game
 * pixels drawn 2x, ink outline, the design mock-up's tick. */
static const char *const skin_check_on[9] = {
    "#########", "#.......#", "#......##", "#.....#.#", "#....#..#",
    "#.#.#...#", "#..#....#", "#.......#", "#########",
};
static const char *const skin_check_off[9] = {
    "#########", "#.......#", "#.......#", "#.......#", "#.......#",
    "#.......#", "#.......#", "#.......#", "#########",
};
#define SKIN_CHECK_SIZE 9
#define SKIN_CHECK_SCALE 2

struct skin_sprite {
    int w;
    int h;
    const uint32_t *px; /* straight-alpha 0xAABBGGRR, stride w */
};

struct skin_face {
    const struct manager_bmfont *font;
    int scale;
    int y_bias;
    struct manager_face fallback; /* DejaVu face of the role, may be invalid */
    int fallback_ascent;
};

struct skin {
    uint32_t *frame; /* SKIN_HALF_W x SKIN_HALF_H, opaque */
    struct skin_sprite sheet_strip;
    struct skin_sprite popup;
    struct skin_sprite cursor;
    struct skin_sprite sprite[SKIN_SPRITE_COUNT];
    struct manager_bmfont font[SKIN_FONT_COUNT];
    struct skin_face face[MANAGER_FACE_COUNT];
    int glyph_centre; /* below the body pen y: centre of the capitals */
    uint32_t paper;
    uint32_t ink;
};

/* ---- arena ------------------------------------------------------------ */

/* Transient buffers stack up from `low`, persistent ones come down from
 * `high`; both 16-byte aligned. */
struct skin_arena {
    uintptr_t begin;
    uintptr_t low;
    uintptr_t high;
    uintptr_t end;
    size_t peak;
};

static void skin_arena_init(struct skin_arena *arena, void *block, size_t bytes)
{
    const uintptr_t start = (uintptr_t)block;
    arena->begin = (start + 15u) & ~(uintptr_t)15u;
    arena->end = (start + bytes) & ~(uintptr_t)15u;
    if (arena->end < arena->begin || bytes < 16u)
        arena->end = arena->begin;
    arena->low = arena->begin;
    arena->high = arena->end;
    arena->peak = 0u;
}

static void skin_arena_note(struct skin_arena *arena)
{
    const size_t used = (size_t)(arena->low - arena->begin) +
                        (size_t)(arena->end - arena->high);
    if (used > arena->peak)
        arena->peak = used;
}

static void *skin_arena_temp(struct skin_arena *arena, size_t bytes)
{
    uintptr_t at;
    if (bytes == 0u)
        bytes = 1u;
    if (bytes > (size_t)(arena->high - arena->low) ||
        ((bytes + 15u) & ~(size_t)15u) > (size_t)(arena->high - arena->low))
        return NULL;
    at = arena->low;
    arena->low += (bytes + 15u) & ~(size_t)15u;
    skin_arena_note(arena);
    return (void *)at;
}

static void *skin_arena_keep(struct skin_arena *arena, size_t bytes)
{
    size_t rounded;
    if (bytes == 0u)
        bytes = 1u;
    if (bytes > (size_t)(arena->high - arena->low))
        return NULL;
    rounded = (bytes + 15u) & ~(size_t)15u;
    if (rounded > (size_t)(arena->high - arena->low))
        return NULL;
    arena->high -= rounded;
    skin_arena_note(arena);
    return (void *)arena->high;
}

/* ---- loader ----------------------------------------------------------- */

struct skin_loader {
    struct manager_skin_game_context *context;
    struct skin_arena arena;
    struct manager_arch arch[3];
    struct manager_arch_set set;
    struct manager_arch_work *work;
    uint8_t *scratch;
    int stage;
    int status;
};

static int skin_fail(struct skin_loader *loader, int stage, int status)
{
    if (loader->stage == MANAGER_SKIN_GAME_OK) {
        loader->stage = stage;
        loader->status = status;
    }
    return -1;
}

static int skin_open_archives(struct skin_loader *loader)
{
    const char *dir = loader->context->packed_dir != NULL
                          ? loader->context->packed_dir
                          : MANAGER_ARCH_PACKED_DIR;
    char path[SKIN_PATH_CAPACITY];
    unsigned int index;
    int last = MANAGER_ARCH_E_IO;
    loader->set.count = 0u;
    for (index = 0u; index < 3u; ++index) {
        struct manager_arch *arch = &loader->arch[index];
        const uintptr_t mark = loader->arena.low;
        uint32_t bytes;
        void *buffer;
        int result;
        int written;
        manager_arch_init(arch);
        written = snprintf(path, sizeof(path), "%s/%s", dir,
                           manager_arch_default_order[index]);
        if (written < 0 || (size_t)written >= sizeof(path))
            return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_ARCHIVE,
                             MANAGER_ARCH_E_PATH);
        result = manager_arch_open(arch, loader->context->io, path);
        if (result != MANAGER_ARCH_OK) {
            last = result;
            continue;
        }
        bytes = manager_arch_index_bytes(arch);
        buffer = skin_arena_temp(&loader->arena, bytes);
        if (buffer == NULL) {
            manager_arch_close(arch);
            return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
        }
        result = manager_arch_load_index(arch, buffer, bytes);
        if (result != MANAGER_ARCH_OK) {
            /* A damaged archive is skipped like a missing one. */
            manager_arch_close(arch);
            loader->arena.low = mark;
            last = result;
            continue;
        }
        loader->set.archives[loader->set.count++] = arch;
    }
    if (loader->set.count == 0u)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_ARCHIVE, last);
    return 0;
}

static void skin_close_archives(struct skin_loader *loader)
{
    unsigned int index;
    for (index = 0u; index < 3u; ++index)
        manager_arch_close(&loader->arch[index]);
    loader->set.count = 0u;
}

/* Reads one file into a transient buffer. */
static int skin_read(struct skin_loader *loader, const char *path,
                     uint8_t **data, uint32_t *size)
{
    struct manager_arch_entry entry;
    uint32_t which = 0u;
    uint8_t *buffer;
    int result = manager_arch_set_find(&loader->set, path, &which, &entry);
    if (result != 1)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_FIND, result);
    buffer = (uint8_t *)skin_arena_temp(&loader->arena, entry.size);
    if (buffer == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    result = manager_arch_read(loader->set.archives[which], &entry, buffer,
                               entry.size, loader->work);
    if (result != MANAGER_ARCH_OK)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_READ, result);
    *data = buffer;
    *size = entry.size;
    return 0;
}

/* Reads and decodes one PNG of the given size (0 x 0: any size up to the
 * font page budget) into transient memory; RGBA words, stride width. */
static int skin_decode(struct skin_loader *loader, const char *path,
                       uint32_t width, uint32_t height, uint32_t **pixels,
                       struct manager_png_info *info)
{
    uint8_t *png = NULL;
    uint32_t size = 0u;
    uint8_t *rgba;
    size_t bytes;
    int result;
    if (skin_read(loader, path, &png, &size) != 0)
        return -1;
    result = manager_png_probe(png, size, info);
    if (result != MANAGER_PNG_OK)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_PNG, result);
    if (width != 0u ? (info->width != width || info->height != height)
                    : (uint64_t)info->width * info->height >
                          SKIN_FONT_PAGE_TEXELS_MAX)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_GEOMETRY, 0);
    bytes = manager_png_buffer_bytes(info);
    rgba = (uint8_t *)skin_arena_temp(&loader->arena, bytes);
    if (rgba == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    result = manager_png_decode(png, size, rgba, bytes, loader->scratch,
                                MANAGER_PNG_SCRATCH_BYTES, info);
    if (result != MANAGER_PNG_OK)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_PNG, result);
    *pixels = (uint32_t *)(void *)rgba;
    return 0;
}

/* Copies a sub-rectangle of a decoded image into persistent memory. */
static int skin_crop(struct skin_loader *loader, const uint32_t *image,
                     uint32_t stride, const struct skin_rect *rect,
                     struct skin_sprite *sprite)
{
    uint32_t *copy = (uint32_t *)skin_arena_keep(
        &loader->arena, (size_t)rect->w * rect->h * sizeof(uint32_t));
    unsigned int row;
    if (copy == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    for (row = 0u; row < (unsigned int)rect->h; ++row)
        memcpy(copy + (size_t)row * rect->w,
               image + (size_t)(rect->y + row) * stride + rect->x,
               (size_t)rect->w * sizeof(uint32_t));
    sprite->w = rect->w;
    sprite->h = rect->h;
    sprite->px = copy;
    return 0;
}

/* Two same-height crops side by side into one sprite (a gap between),
 * or - split_rows != 0 - two same-size crops, the first giving the rows
 * above split_rows and the second the rest; or the columns left of
 * split_columns from the first and the rest from the second. */
static int skin_join(struct skin_loader *loader, const uint32_t *image,
                     uint32_t stride, const struct skin_rect *first,
                     const struct skin_rect *second, int gap, int split_rows,
                     int split_columns, struct skin_sprite *sprite)
{
    const int w = (split_rows || split_columns) ? first->w
                                                : first->w + gap + second->w;
    const int h = first->h;
    uint32_t *copy =
        (uint32_t *)skin_arena_keep(&loader->arena, (size_t)w * h * 4u);
    int x;
    int y;
    if (copy == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            const struct skin_rect *from = first;
            int sx = x;
            uint32_t word = 0u;
            if (split_rows) {
                from = y < split_rows ? first : second;
            } else if (split_columns) {
                from = x < split_columns ? first : second;
            } else if (x >= first->w) {
                from = second;
                sx = x - first->w - gap;
            }
            if (sx >= 0 && sx < from->w && y < from->h)
                word = image[(size_t)(from->y + y) * stride + from->x +
                             (unsigned int)sx];
            copy[(size_t)y * w + x] = word;
        }
    }
    sprite->w = w;
    sprite->h = h;
    sprite->px = copy;
    return 0;
}

/* ---- drawing helpers ---------------------------------------------------- */

/* Target texel t of `target` -> source texel of `source`, corners 1:1 and
 * the middle stretched (or the whole span scaled when it is too short). */
static int skin_nine_map(int t, int target, int source, int corner)
{
    if (target <= 0 || source <= 0)
        return 0;
    if (target <= 2 * corner || source <= 2 * corner)
        return (int)((int64_t)t * source / target);
    if (t < corner)
        return t;
    if (t >= target - corner)
        return source - (target - t);
    return corner + (int)((int64_t)(t - corner) * (source - 2 * corner) /
                          (target - 2 * corner));
}

/* The sprite stretched nine-slice over the inclusive box at `scale`
 * (every texel a scale x scale block), alpha-blended; tint_alpha != 0
 * lays the tint colour over the opaque texels. */
static void skin_nine(struct manager_canvas *canvas,
                      const struct skin_sprite *sprite, int x0, int y0, int x1,
                      int y1, int corner_x, int corner_y, int scale,
                      uint32_t tint, unsigned int tint_alpha)
{
    int target_w;
    int target_h;
    int cx0;
    int cy0;
    int cx1;
    int cy1;
    int y;
    if (canvas == NULL || canvas->pixels == NULL || sprite->px == NULL ||
        sprite->w <= 0 || sprite->h <= 0 || x1 < x0 || y1 < y0 || scale < 1)
        return;
    target_w = (x1 - x0 + scale) / scale;
    target_h = (y1 - y0 + scale) / scale;
    cx0 = x0 > canvas->clip.x ? x0 : canvas->clip.x;
    cy0 = y0 > canvas->clip.y ? y0 : canvas->clip.y;
    cx1 = canvas->clip.x + canvas->clip.w - 1;
    cy1 = canvas->clip.y + canvas->clip.h - 1;
    if (cx1 > x1)
        cx1 = x1;
    if (cy1 > y1)
        cy1 = y1;
    for (y = cy0; y <= cy1; ++y) {
        const int sy = skin_nine_map((y - y0) / scale, target_h, sprite->h,
                                     corner_y);
        const uint32_t *source = sprite->px + (size_t)sy * (size_t)sprite->w;
        uint32_t *row = canvas->pixels + (size_t)y * (size_t)canvas->pitch;
        int x;
        for (x = cx0; x <= cx1; ++x) {
            const int sx = skin_nine_map((x - x0) / scale, target_w,
                                         sprite->w, corner_x);
            const uint32_t word = source[sx];
            const unsigned int alpha = (unsigned int)(word >> 24);
            if (alpha == 0u)
                continue;
            row[x] = manager_canvas_mix(row[x], word, alpha);
            if (tint_alpha != 0u && alpha == 255u)
                row[x] = manager_canvas_mix(row[x], tint, tint_alpha);
        }
    }
}

static void skin_blit(struct manager_canvas *canvas,
                      const struct skin_sprite *sprite, int x, int y,
                      int scale)
{
    skin_nine(canvas, sprite, x, y, x + sprite->w * scale - 1,
              y + sprite->h * scale - 1, 0, 0, scale, 0u, 0u);
}

/* ---- face adapter (DejaVu fallback per glyph) -------------------------- */

static int skin_face_line_height(const void *impl)
{
    const struct skin_face *face = (const struct skin_face *)impl;
    return (int)face->font->line_height * face->scale;
}

static int skin_face_ascent(const void *impl)
{
    const struct skin_face *face = (const struct skin_face *)impl;
    return (int)face->font->base * face->scale + face->y_bias;
}

static int skin_face_has_glyph(const void *impl, uint32_t codepoint)
{
    const struct skin_face *face = (const struct skin_face *)impl;
    if (manager_bmfont_find(face->font, codepoint) != NULL)
        return 1;
    return manager_face_valid(face->fallback) &&
           face->fallback.ops->has_glyph(face->fallback.impl, codepoint);
}

static int skin_face_advance(const void *impl, uint32_t codepoint)
{
    const struct skin_face *face = (const struct skin_face *)impl;
    const struct manager_bmfont_glyph *glyph =
        manager_bmfont_find(face->font, codepoint);
    if (glyph == NULL && manager_face_valid(face->fallback))
        return face->fallback.ops->advance(face->fallback.impl, codepoint);
    if (glyph == NULL)
        glyph = face->font->fallback;
    return glyph != NULL ? (int)glyph->xadvance * face->scale : 0;
}

static int skin_face_draw_glyph(const void *impl, struct manager_canvas *canvas,
                                int x, int y, uint32_t codepoint,
                                uint32_t color)
{
    const struct skin_face *face = (const struct skin_face *)impl;
    const struct manager_bmfont_glyph *glyph =
        manager_bmfont_find(face->font, codepoint);
    struct manager_bmfont_surface surface;
    if (glyph == NULL && manager_face_valid(face->fallback)) {
        /* Per-glyph fallback, baselines lined up. */
        if (canvas == NULL)
            return face->fallback.ops->advance(face->fallback.impl, codepoint);
        return face->fallback.ops->draw_glyph(
            face->fallback.impl, canvas, x,
            y + face->y_bias + (int)face->font->base * face->scale -
                face->fallback_ascent,
            codepoint, color);
    }
    if (glyph == NULL)
        glyph = face->font->fallback;
    if (glyph == NULL)
        return 0;
    if (canvas == NULL || canvas->pixels == NULL)
        return (int)glyph->xadvance * face->scale;
    manager_bmfont_surface_init(&surface, canvas->pixels,
                                (uint32_t)canvas->width,
                                (uint32_t)canvas->height,
                                (uint32_t)canvas->pitch);
    surface.clip_x0 = canvas->clip.x;
    surface.clip_y0 = canvas->clip.y;
    surface.clip_x1 = canvas->clip.x + canvas->clip.w;
    surface.clip_y1 = canvas->clip.y + canvas->clip.h;
    return (int)manager_bmfont_draw_glyph(face->font, &surface, x,
                                          y + face->y_bias, glyph, color,
                                          (uint32_t)face->scale);
}

static const struct manager_face_ops skin_face_ops = {
    skin_face_line_height, skin_face_ascent, skin_face_has_glyph,
    skin_face_advance, skin_face_draw_glyph,
};

/* ---- theme ops ------------------------------------------------------------ */

static const struct skin *skin_of(const struct manager_theme *theme)
{
    return (const struct skin *)theme->skin;
}

static void skin_draw_background(const struct manager_theme *theme,
                                 struct manager_canvas *canvas)
{
    const struct skin *skin = skin_of(theme);
    int x0;
    int y0;
    int x1;
    int y1;
    int y;
    if (canvas == NULL || canvas->pixels == NULL)
        return;
    x0 = canvas->clip.x;
    y0 = canvas->clip.y;
    x1 = canvas->clip.x + canvas->clip.w;
    y1 = canvas->clip.y + canvas->clip.h;
    for (y = y0; y < y1; ++y) {
        uint32_t *row = canvas->pixels + (size_t)y * (size_t)canvas->pitch;
        int x;
        if (y >= SKIN_H) {
            for (x = x0; x < x1; ++x)
                row[x] = skin->paper;
            continue;
        }
        {
            const uint32_t *source = skin->frame + (size_t)(y >> 1) * SKIN_HALF_W;
            for (x = x0; x < x1; ++x)
                row[x] = x < SKIN_W ? source[x >> 1] : skin->paper;
        }
    }
}

static void skin_draw_panel(const struct manager_theme *theme,
                            struct manager_canvas *canvas, int x0, int y0,
                            int x1, int y1, enum manager_panel_kind kind)
{
    const struct skin *skin = skin_of(theme);
    if (kind == MANAGER_PANEL_BOX) {
        skin_nine(canvas, &skin->popup, x0 - SKIN_BOX_PAD_LEFT,
                  y0 - SKIN_BOX_PAD_TOP, x1 + SKIN_BOX_PAD_RIGHT,
                  y1 + SKIN_BOX_PAD_BOTTOM, SKIN_POPUP_CORNER_X,
                  SKIN_POPUP_CORNER_Y, 2, 0u, 0u);
        return;
    }
    skin_nine(canvas, &skin->sheet_strip, x0, y0, x1, y1, SKIN_STRIP_CORNER_X,
              SKIN_STRIP_CORNER_Y, 2, skin->ink,
              kind == MANAGER_PANEL_CARD_SELECTED ? SKIN_CARD_WASH_ALPHA : 0u);
}

static void skin_draw_row_highlight(const struct manager_theme *theme,
                                    struct manager_canvas *canvas, int x0,
                                    int y0, int x1, int y1)
{
    const struct skin *skin = skin_of(theme);
    manager_canvas_blend_box(canvas, x0, y0, x1, y1, skin->ink,
                             SKIN_WASH_ALPHA);
    skin_blit(canvas, &skin->cursor, x0 + SKIN_CURSOR_X,
              y0 + SKIN_ROW_TEXT_DY + skin->glyph_centre -
                  skin->cursor.h * SKIN_CURSOR_SCALE / 2,
              SKIN_CURSOR_SCALE);
}

static void skin_draw_check(struct manager_canvas *canvas, int x, int y,
                            const char *const *bitmap, uint32_t color)
{
    int row;
    for (row = 0; row < SKIN_CHECK_SIZE; ++row) {
        int column;
        for (column = 0; column < SKIN_CHECK_SIZE; ++column) {
            if (bitmap[row][column] != '#')
                continue;
            manager_canvas_fill_box(canvas, x + column * SKIN_CHECK_SCALE,
                                    y + row * SKIN_CHECK_SCALE,
                                    x + column * SKIN_CHECK_SCALE +
                                        SKIN_CHECK_SCALE - 1,
                                    y + row * SKIN_CHECK_SCALE +
                                        SKIN_CHECK_SCALE - 1,
                                    color);
        }
    }
}

static int skin_draw_hint_glyph(const struct manager_theme *theme,
                                struct manager_canvas *canvas, int x, int y,
                                enum manager_glyph glyph, uint32_t color)
{
    const struct skin *skin = skin_of(theme);
    const struct skin_sprite *sprite = NULL;
    switch (glyph) {
    case MANAGER_GLYPH_CROSS:
        sprite = &skin->sprite[SKIN_SPRITE_CROSS];
        break;
    case MANAGER_GLYPH_CIRCLE:
        sprite = &skin->sprite[SKIN_SPRITE_CIRCLE];
        break;
    case MANAGER_GLYPH_SQUARE:
        sprite = &skin->sprite[SKIN_SPRITE_SQUARE];
        break;
    case MANAGER_GLYPH_TRIANGLE:
        sprite = &skin->sprite[SKIN_SPRITE_TRIANGLE];
        break;
    case MANAGER_GLYPH_UP_DOWN:
        sprite = &skin->sprite[SKIN_SPRITE_UP_DOWN];
        break;
    case MANAGER_GLYPH_LEFT_RIGHT:
        sprite = &skin->sprite[SKIN_SPRITE_LEFT_RIGHT];
        break;
    case MANAGER_GLYPH_L_R:
        sprite = &skin->sprite[SKIN_SPRITE_L_R];
        break;
    case MANAGER_GLYPH_START:
        sprite = &skin->sprite[SKIN_SPRITE_START];
        break;
    case MANAGER_GLYPH_CHECK_ON:
    case MANAGER_GLYPH_CHECK_OFF:
        if (canvas != NULL)
            skin_draw_check(canvas, x,
                            y + skin->glyph_centre -
                                SKIN_CHECK_SIZE * SKIN_CHECK_SCALE / 2,
                            glyph == MANAGER_GLYPH_CHECK_ON ? skin_check_on
                                                            : skin_check_off,
                            color);
        return SKIN_CHECK_SIZE * SKIN_CHECK_SCALE;
    case MANAGER_GLYPH_LAUNCH:
        /* The selected row already carries the game's cursor. */
        return 0;
    default:
        return manager_theme_flat_glyph(theme, canvas, x, y, glyph, color);
    }
    if (canvas != NULL)
        skin_blit(canvas, sprite, x, y + skin->glyph_centre - sprite->h, 2);
    return sprite->w * 2;
}

static const struct manager_theme_ops skin_ops = {
    skin_draw_background,
    skin_draw_panel,
    skin_draw_row_highlight,
    skin_draw_hint_glyph,
};

/* ---- loading -------------------------------------------------------------- */

static int skin_load_backdrop_and_sheet(struct skin_loader *loader,
                                        struct skin *skin)
{
    const uintptr_t mark = loader->arena.low;
    struct manager_png_info info;
    struct manager_canvas half;
    struct skin_sprite sheet;
    uint32_t *pixels = NULL;
    unsigned int row;
    skin->frame = (uint32_t *)skin_arena_keep(
        &loader->arena, (size_t)SKIN_HALF_W * SKIN_HALF_H * sizeof(uint32_t));
    if (skin->frame == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    if (skin_decode(loader, SKIN_BACKDROP_PATH, SKIN_BACKDROP_W,
                    SKIN_BACKDROP_H, &pixels, &info) != 0)
        return -1;
    /* 270 rows; the last two repeat for 272 (the design mock-up renderer
     * pastes rows 536..539 of the 2x image again at 540). */
    for (row = 0u; row < (unsigned int)SKIN_HALF_H; ++row) {
        const unsigned int from =
            row < SKIN_BACKDROP_H ? row : row - 2u;
        unsigned int column;
        for (column = 0u; column < (unsigned int)SKIN_HALF_W; ++column)
            skin->frame[(size_t)row * SKIN_HALF_W + column] =
                pixels[(size_t)from * SKIN_BACKDROP_W + column] | 0xff000000u;
    }
    loader->arena.low = mark;

    if (skin_decode(loader, SKIN_SHEET_PATH, SKIN_SHEET_W, SKIN_SHEET_H,
                    &pixels, &info) != 0)
        return -1;
    {
        const uint32_t centre =
            pixels[(size_t)(skin_sheet_crop.y + skin_sheet_crop.h / 2u) *
                       SKIN_SHEET_W +
                   skin_sheet_crop.x + skin_sheet_crop.w / 2u];
        skin->paper = centre | 0xff000000u;
    }
    sheet.w = skin_sheet_crop.w;
    sheet.h = skin_sheet_crop.h;
    sheet.px = NULL;
    {
        /* A view into the decoded sheet: stride differs, so the paper is
         * stretched row by row through a temporary copy of the crop. */
        uint32_t *copy = (uint32_t *)skin_arena_temp(
            &loader->arena, (size_t)sheet.w * sheet.h * sizeof(uint32_t));
        if (copy == NULL)
            return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
        for (row = 0u; row < (unsigned int)skin_sheet_crop.h; ++row)
            memcpy(copy + (size_t)row * sheet.w,
                   pixels + (size_t)(skin_sheet_crop.y + row) * SKIN_SHEET_W +
                       skin_sheet_crop.x,
                   (size_t)sheet.w * sizeof(uint32_t));
        sheet.px = copy;
    }
    manager_canvas_init(&half, skin->frame, SKIN_HALF_W, SKIN_HALF_H,
                        SKIN_HALF_W);
    skin_nine(&half, &sheet, SKIN_PAPER_X0, SKIN_PAPER_Y0, SKIN_PAPER_X1,
              SKIN_PAPER_Y1, SKIN_SHEET_CORNER_X, SKIN_SHEET_CORNER_Y, 1, 0u,
              0u);
    if (skin_crop(loader, pixels, SKIN_SHEET_W, &skin_strip_crop,
                  &skin->sheet_strip) != 0)
        return -1;
    loader->arena.low = mark;
    return 0;
}

static int skin_load_popup(struct skin_loader *loader, struct skin *skin)
{
    const uintptr_t mark = loader->arena.low;
    struct manager_png_info info;
    uint32_t *pixels = NULL;
    if (skin_decode(loader, SKIN_POPUP_PATH, SKIN_POPUP_W, SKIN_POPUP_H,
                    &pixels, &info) != 0 ||
        skin_crop(loader, pixels, SKIN_POPUP_W, &skin_popup_crop,
                  &skin->popup) != 0 ||
        skin_crop(loader, pixels, SKIN_POPUP_W, &skin_cursor_crop,
                  &skin->cursor) != 0)
        return -1;
    loader->arena.low = mark;
    return 0;
}

static int skin_load_buttons(struct skin_loader *loader, struct skin *skin)
{
    const uintptr_t mark = loader->arena.low;
    struct manager_png_info info;
    uint32_t *pixels = NULL;
    unsigned int index;
    if (skin_decode(loader, SKIN_BUTTONS_PATH, SKIN_BUTTONS_W, SKIN_BUTTONS_H,
                    &pixels, &info) != 0)
        return -1;
    for (index = 0u; index < 5u; ++index) {
        if (skin_crop(loader, pixels, SKIN_BUTTONS_W, &skin_button_box[index],
                      &skin->sprite[index]) != 0)
            return -1;
    }
    /* Up arm from frame 5 above the middle row, down arm from frame 4
     * below it (both 14x14, the white arm 4 texels deep); left/right the
     * same with columns. */
    if (skin_join(loader, pixels, SKIN_BUTTONS_W, &skin_dpad_up,
                  &skin_dpad_down, 0, 7, 0,
                  &skin->sprite[SKIN_SPRITE_UP_DOWN]) != 0 ||
        skin_join(loader, pixels, SKIN_BUTTONS_W, &skin_dpad_left,
                  &skin_dpad_right, 0, 0, 7,
                  &skin->sprite[SKIN_SPRITE_LEFT_RIGHT]) != 0 ||
        skin_join(loader, pixels, SKIN_BUTTONS_W, &skin_button_l,
                  &skin_button_r, SKIN_L_R_GAP, 0, 0,
                  &skin->sprite[SKIN_SPRITE_L_R]) != 0)
        return -1;
    loader->arena.low = mark;
    return 0;
}

static int skin_load_font(struct skin_loader *loader, const char *fnt_path,
                          struct manager_bmfont *font)
{
    const uintptr_t mark = loader->arena.low;
    struct manager_png_info info;
    struct manager_bmfont_glyph *glyphs;
    char page_path[SKIN_PATH_CAPACITY];
    const char *slash = strrchr(fnt_path, '/');
    const size_t dir_length = slash != NULL ? (size_t)(slash - fnt_path) + 1u : 0u;
    uint8_t *fnt = NULL;
    uint32_t size = 0u;
    uint32_t rows;
    uint32_t *pixels = NULL;
    uint8_t *page;
    uint32_t non_white = 0u;
    int written;
    int result;
    if (skin_read(loader, fnt_path, &fnt, &size) != 0)
        return -1;
    rows = manager_bmfont_glyph_rows(fnt, size);
    if (rows == 0u || rows > SKIN_FONT_GLYPHS_MAX)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_FONT,
                         MANAGER_BMFONT_E_BLOCK);
    glyphs = (struct manager_bmfont_glyph *)skin_arena_keep(
        &loader->arena, (size_t)rows * sizeof(*glyphs));
    if (glyphs == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    result = manager_bmfont_parse(font, fnt, size, glyphs, rows);
    if (result != MANAGER_BMFONT_OK)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_FONT, result);
    loader->arena.low = mark; /* the .fnt bytes are no longer needed */
    written = snprintf(page_path, sizeof(page_path), "%.*s%s", (int)dir_length,
                       fnt_path, font->page_name);
    if (written < 0 || (size_t)written >= sizeof(page_path))
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_FONT,
                         MANAGER_BMFONT_E_PAGE);
    if (skin_decode(loader, page_path, 0u, 0u, &pixels, &info) != 0)
        return -1;
    if (info.width != font->scale_w || info.height != font->scale_h)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_GEOMETRY,
                         MANAGER_BMFONT_E_PAGE);
    page = (uint8_t *)skin_arena_keep(&loader->arena,
                                      (size_t)info.width * info.height);
    if (page == NULL)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
    manager_bmfont_rgba_to_a8(page, (const uint8_t *)pixels,
                              info.width * info.height, &non_white);
    result = manager_bmfont_attach_page(font, page, info.width, info.height);
    if (result != MANAGER_BMFONT_OK)
        return skin_fail(loader, MANAGER_SKIN_GAME_STAGE_FONT, result);
    loader->arena.low = mark;
    return 0;
}

static void skin_palette(struct manager_palette *palette, const struct skin *skin)
{
    const uint32_t ink = manager_rgb(SKIN_INK_R, SKIN_INK_G, SKIN_INK_B);
    const uint32_t soft =
        manager_rgb(SKIN_INK_SOFT_R, SKIN_INK_SOFT_G, SKIN_INK_SOFT_B);
    uint32_t *c = palette->color;
    c[MANAGER_COLOR_BACKGROUND] = skin->paper;
    c[MANAGER_COLOR_BAR] = skin->paper;
    c[MANAGER_COLOR_BAR_TEXT] = ink;
    c[MANAGER_COLOR_BAR_MUTED] = soft;
    c[MANAGER_COLOR_TAB_MARK] = ink;
    c[MANAGER_COLOR_TEXT] = ink;
    c[MANAGER_COLOR_MUTED] = soft;
    c[MANAGER_COLOR_HIGHLIGHT] = ink;
    c[MANAGER_COLOR_HIGHLIGHT_TEXT] = ink;
    c[MANAGER_COLOR_HIGHLIGHT_MUTED] = soft;
    c[MANAGER_COLOR_SEPARATOR] = manager_canvas_mix(skin->paper, ink, 64u);
    c[MANAGER_COLOR_SCROLL_THUMB] = soft;
    c[MANAGER_COLOR_PANEL] = skin->paper;
    c[MANAGER_COLOR_PANEL_EDGE] = soft;
    c[MANAGER_COLOR_SCRIM] = ink;
    /* Status tones on the backdrop: darker than mock C's accents so they
     * read on the grey-beige of EmptyScreen. */
    c[MANAGER_COLOR_OK] = manager_rgb(40u, 110u, 40u);
    c[MANAGER_COLOR_WARNING] = manager_rgb(150u, 85u, 10u);
    c[MANAGER_COLOR_ERROR] = manager_rgb(160u, 32u, 32u);
    palette->scrim_alpha = 110u;
}

static uint64_t skin_clock(const struct manager_skin_game_context *context)
{
    return context->clock_us != NULL ? context->clock_us(context->opaque) : 0u;
}

int manager_skin_game_init(struct manager_theme *theme,
                           const struct manager_theme_fonts *fonts,
                           void *context_pointer)
{
    struct manager_skin_game_context *context =
        (struct manager_skin_game_context *)context_pointer;
    struct skin_loader loader;
    struct skin *skin;
    size_t bytes;
    void *block;
    uint64_t started;
    unsigned int index;
    int result = -1;

    if (context == NULL)
        return -1;
    context->stage = MANAGER_SKIN_GAME_OK;
    context->status = 0;
    context->decode_us = 0u;
    context->arena_peak = 0u;
    context->arena_resident = 0u;
    if (theme == NULL || context->io == NULL || context->acquire == NULL ||
        context->release == NULL) {
        context->stage = MANAGER_SKIN_GAME_STAGE_CONTEXT;
        return -1;
    }
    started = skin_clock(context);
    bytes = context->arena_bytes != 0u ? context->arena_bytes
                                       : MANAGER_SKIN_GAME_ARENA_BYTES;
    block = context->acquire(context->opaque, bytes);
    if (block == NULL) {
        context->stage = MANAGER_SKIN_GAME_STAGE_MEMORY;
        return -1;
    }
    memset(&loader, 0, sizeof(loader));
    loader.context = context;
    loader.stage = MANAGER_SKIN_GAME_OK;
    skin_arena_init(&loader.arena, block, bytes);
    for (index = 0u; index < 3u; ++index)
        manager_arch_init(&loader.arch[index]);

    skin = (struct skin *)skin_arena_keep(&loader.arena, sizeof(*skin));
    if (skin == NULL) {
        (void)skin_fail(&loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
        goto done;
    }
    memset(skin, 0, sizeof(*skin));
    skin->ink = manager_rgb(SKIN_INK_R, SKIN_INK_G, SKIN_INK_B);
    if (skin_open_archives(&loader) != 0)
        goto done;
    loader.work = (struct manager_arch_work *)skin_arena_temp(
        &loader.arena, sizeof(*loader.work));
    loader.scratch =
        (uint8_t *)skin_arena_temp(&loader.arena, MANAGER_PNG_SCRATCH_BYTES);
    if (loader.work == NULL || loader.scratch == NULL) {
        (void)skin_fail(&loader, MANAGER_SKIN_GAME_STAGE_MEMORY, 0);
        goto done;
    }
    if (skin_load_backdrop_and_sheet(&loader, skin) != 0 ||
        skin_load_popup(&loader, skin) != 0 ||
        skin_load_buttons(&loader, skin) != 0)
        goto done;
    for (index = 0u; index < SKIN_FONT_COUNT; ++index) {
        if (skin_load_font(&loader, skin_font_paths[index],
                           &skin->font[index]) != 0)
            goto done;
    }
    for (index = 0u; index < (unsigned int)MANAGER_FACE_COUNT; ++index) {
        struct skin_face *face = &skin->face[index];
        face->font = &skin->font[skin_face_specs[index].font];
        face->scale = skin_face_specs[index].scale;
        face->y_bias = skin_face_specs[index].y_bias;
        if (fonts != NULL && manager_face_valid(fonts->face[index])) {
            face->fallback = fonts->face[index];
            face->fallback_ascent = manager_face_ascent(fonts->face[index]);
        }
    }
    {
        /* Centre of the capitals under the body pen: 'H' when the face has
         * it, else half the baseline. */
        const struct skin_face *body = &skin->face[MANAGER_FACE_BODY];
        const struct manager_bmfont_glyph *h = manager_bmfont_find(body->font, 'H');
        const int base = (int)body->font->base * body->scale;
        const int top = h != NULL ? (int)h->yoffset * body->scale : 0;
        skin->glyph_centre = body->y_bias + (top + base) / 2;
    }
    result = 0;

done:
    skin_close_archives(&loader);
    context->arena_peak = loader.arena.peak;
    context->stage = loader.stage;
    context->status = loader.status;
    if (result != 0) {
        context->release(context->opaque, block, bytes);
        if (context->stage == MANAGER_SKIN_GAME_OK)
            context->stage = MANAGER_SKIN_GAME_STAGE_MEMORY;
        return -1;
    }
    context->arena_resident = (size_t)(loader.arena.end - loader.arena.high);
    memset(theme, 0, sizeof(*theme));
    theme->id = MANAGER_THEME_GAME;
    skin_palette(&theme->palette, skin);
    for (index = 0u; index < (unsigned int)MANAGER_FACE_COUNT; ++index) {
        theme->face[index].ops = &skin_face_ops;
        theme->face[index].impl = &skin->face[index];
    }
    theme->ops = &skin_ops;
    theme->title_x = 40; /* MANAGER_DL_GAME_X0 */
    theme->skin = skin;
    if (context->clock_us != NULL) {
        const uint64_t elapsed = skin_clock(context) - started;
        context->decode_us = elapsed > 0xffffffffu ? 0xffffffffu
                                                   : (uint32_t)elapsed;
    }
    return 0;
}

const char *manager_skin_game_stage_text(int stage)
{
    static const char *const names[] = {
        "ok",   "context", "memory",   "archive", "find",
        "read", "png",     "geometry", "font",
    };
    if (stage < 0 || (size_t)stage >= sizeof(names) / sizeof(names[0]))
        return "unknown";
    return names[stage];
}

#endif /* MANAGER_UI2 && MANAGER_UI2_GAME_SKIN */
