/*
 * Manager UI2 themes (the UI2 design's three visual variants; owner
 * decision 2026-09-27: three themes, `theme = game | dark | light` in
 * ux0:data/isaacr001/manager.ini).
 *
 * A theme is everything that decides how the page model looks and nothing
 * that decides what is on a page: manager_ui.c builds the same display list
 * (rows, rectangles, text, glyph ids) for every theme and manager_ui_draw.c
 * paints it through the theme.  A theme therefore provides
 *
 *   palette   colour words for the roles of enum manager_color (0xAABBGGRR,
 *             the manager framebuffer layout);
 *   faces     three struct manager_face (manager_font.h: a vtable with
 *             line_height / ascent / has_glyph / advance / draw_glyph, on
 *             top of which manager_face_measure / _draw / _draw_clipped
 *             work) for the title, body and small text roles;
 *   ops       draw_background (the whole frame under the page: body, header
 *             bar, hint bar), draw_panel (cards and the confirmation box),
 *             draw_row_highlight (the selected row) and draw_hint_glyph
 *             (button and marker glyphs of enum manager_glyph).
 *
 * dark  = design mock-up A's palette and bars (red header and hint bars on
 *         a dark body, render_a) on C's page model;
 * light = design mock-up C exactly (render_c_home / render_c_mods_ru);
 * game  = mock-up B, the game's own menu art; written by the game skin
 *         (knob ISAAC_VITA_MANAGER_UI2_GAME_SKIN -> MANAGER_UI2_GAME_SKIN=1,
 *         manager_skin_game.c), which implements
 *
 *             int manager_skin_game_init(struct manager_theme *theme,
 *                                        const struct manager_theme_fonts *fonts,
 *                                        void *context);
 *
 *         filling *theme completely (palette, faces - it may keep any of the
 *         DejaVu faces handed in `fonts` - ops, skin) and returning 0, or
 *         returning nonzero and leaving nothing allocated.  It is called at
 *         most once per process, the first time `game` is selected, with the
 *         context pointer the caller stored in manager_theme_set.game_context
 *         (NULL unless main or a host test set one).
 *
 * Selection never fails: `game` without the skin compiled in, or with a skin
 * that did not load, resolves to `light` and reports why, so the caller
 * shows a status-row message instead of an error dialog.
 *
 * SDK-free; owns no memory beyond the structs.
 */
#ifndef ISAAC_MANAGER_THEME_H
#define ISAAC_MANAGER_THEME_H

#if defined(MANAGER_UI2)

#include "manager_canvas.h"
#include "manager_font.h"

#include <stdint.h>

enum manager_theme_id {
    MANAGER_THEME_GAME = 0, /* manager.ini default (theme key absent) */
    MANAGER_THEME_DARK,
    MANAGER_THEME_LIGHT,
    MANAGER_THEME_COUNT
};

enum manager_face_role {
    MANAGER_FACE_TITLE = 0, /* 24 px bold: page title */
    MANAGER_FACE_BODY,      /* 16 px: rows, hints, glyphs */
    MANAGER_FACE_SMALL,     /* 12 px: tabs, values, sub-lines, status */
    MANAGER_FACE_COUNT
};

enum manager_color {
    MANAGER_COLOR_BACKGROUND = 0, /* body fill */
    MANAGER_COLOR_BAR,            /* header and hint bar fill */
    MANAGER_COLOR_BAR_TEXT,       /* title, active tab, hint labels */
    MANAGER_COLOR_BAR_MUTED,      /* inactive tabs */
    MANAGER_COLOR_TAB_MARK,       /* underline of the active tab */
    MANAGER_COLOR_TEXT,           /* body text */
    MANAGER_COLOR_MUTED,          /* values, sub-lines, disabled rows */
    MANAGER_COLOR_HIGHLIGHT,      /* selected row fill */
    MANAGER_COLOR_HIGHLIGHT_TEXT, /* body text on the selected row */
    MANAGER_COLOR_HIGHLIGHT_MUTED, /* secondary text on the selected row */
    MANAGER_COLOR_SEPARATOR,      /* separators, scroll track */
    MANAGER_COLOR_SCROLL_THUMB,
    MANAGER_COLOR_PANEL,          /* card and box fill */
    MANAGER_COLOR_PANEL_EDGE,     /* card and box outline */
    MANAGER_COLOR_SCRIM,          /* colour laid over the page under a box */
    MANAGER_COLOR_OK,
    MANAGER_COLOR_WARNING,
    MANAGER_COLOR_ERROR,
    MANAGER_COLOR_COUNT
};

struct manager_palette {
    uint32_t color[MANAGER_COLOR_COUNT];
    unsigned int scrim_alpha; /* 0..255, MANAGER_COLOR_SCRIM strength */
};

enum manager_panel_kind {
    MANAGER_PANEL_CARD = 0,      /* SAVES slot card */
    MANAGER_PANEL_CARD_SELECTED, /* the selected slot card */
    MANAGER_PANEL_BOX,           /* confirmation box, detail card */
    MANAGER_PANEL_COUNT
};

/* Symbolic glyphs: controller buttons on the hint bar and the row markers.
 * The flat themes draw them as DejaVu text (the design mock-ups' GLYPHS
 * set); the game skin may draw its Buttons.anm2 frames instead. */
enum manager_glyph {
    MANAGER_GLYPH_NONE = 0,
    MANAGER_GLYPH_CROSS,      /* × */
    MANAGER_GLYPH_CIRCLE,     /* ○ */
    MANAGER_GLYPH_SQUARE,     /* □ */
    MANAGER_GLYPH_TRIANGLE,   /* △ */
    MANAGER_GLYPH_UP_DOWN,    /* ↑↓ */
    MANAGER_GLYPH_LEFT_RIGHT, /* ←→ */
    MANAGER_GLYPH_L_R,        /* L R */
    MANAGER_GLYPH_START,      /* START */
    MANAGER_GLYPH_CHECK_ON,   /* ☑ */
    MANAGER_GLYPH_CHECK_OFF,  /* ☐ */
    MANAGER_GLYPH_MORE,       /* › (row opens a page) */
    MANAGER_GLYPH_LAUNCH,     /* ▸ (primary row) */
    MANAGER_GLYPH_PENDING,    /* • (unsaved edit) */
    MANAGER_GLYPH_PREV,       /* ◂ (value can step left) */
    MANAGER_GLYPH_NEXT,       /* ▸ (value can step right) */
    MANAGER_GLYPH_COUNT
};

struct manager_theme;

struct manager_theme_ops {
    /* The whole frame under the page: body fill, header bar (y 0..63) and
     * hint bar (y 496..543). */
    void (*draw_background)(const struct manager_theme *theme,
                            struct manager_canvas *canvas);
    /* Inclusive box x0..x1, y0..y1. */
    void (*draw_panel)(const struct manager_theme *theme,
                       struct manager_canvas *canvas, int x0, int y0, int x1,
                       int y1, enum manager_panel_kind kind);
    void (*draw_row_highlight)(const struct manager_theme *theme,
                               struct manager_canvas *canvas, int x0, int y0,
                               int x1, int y1);
    /* Pen at (x, y = ascender line of the body face); returns the advance.
     * canvas NULL = measure only.  color is the role colour the text next
     * to the glyph uses (a skin drawing a sprite may ignore it). */
    int (*draw_hint_glyph)(const struct manager_theme *theme,
                           struct manager_canvas *canvas, int x, int y,
                           enum manager_glyph glyph, uint32_t color);
};

struct manager_theme {
    enum manager_theme_id id;
    struct manager_palette palette;
    struct manager_face face[MANAGER_FACE_COUNT];
    const struct manager_theme_ops *ops;
    /* x of the page title (32; a theme that draws an icon in the header
     * moves it right, mock C: icon 40x40 at (16,12), title at 68). */
    int title_x;
    const void *skin; /* implementation data (game skin), else NULL */
};

/* The faces a theme may use: DejaVu IMF faces when the atlases loaded, the
 * 5x7 fallback otherwise (manager_assets_vita.c / the host tests). */
struct manager_theme_fonts {
    struct manager_face face[MANAGER_FACE_COUNT];
};

enum manager_theme_fallback {
    MANAGER_THEME_FALLBACK_NONE = 0,
    MANAGER_THEME_FALLBACK_NOT_BUILT, /* game skin not compiled in */
    MANAGER_THEME_FALLBACK_FAILED     /* game skin did not load */
};

struct manager_theme_set {
    struct manager_theme_fonts fonts;
    struct manager_theme dark;
    struct manager_theme light;
    struct manager_theme game;
    int game_state;     /* 0 untried, 1 loaded, -1 failed or not built */
    void *game_context; /* handed to manager_skin_game_init */
};

void manager_theme_init_dark(struct manager_theme *theme,
                             const struct manager_theme_fonts *fonts);
void manager_theme_init_light(struct manager_theme *theme,
                              const struct manager_theme_fonts *fonts);
/* Fills dark and light from the fonts; the game skin stays untried. */
void manager_theme_set_init(struct manager_theme_set *set,
                            const struct manager_theme_fonts *fonts,
                            void *game_context);
/* The theme to draw with for the wanted id (never NULL); *fallback says
 * why a `game` request resolved to `light`. */
const struct manager_theme *manager_theme_select(
    struct manager_theme_set *set, enum manager_theme_id wanted,
    enum manager_theme_fallback *fallback);

/* Text of a flat glyph ("×", "↑↓", ...); "" for NONE or out of range. */
const char *manager_theme_glyph_text(enum manager_glyph glyph);
/* The flat draw_hint_glyph (the glyph text in the theme's body face); a
 * skin may call it for the glyphs it has no art for. */
int manager_theme_flat_glyph(const struct manager_theme *theme,
                             struct manager_canvas *canvas, int x, int y,
                             enum manager_glyph glyph, uint32_t color);

#if defined(MANAGER_UI2_GAME_SKIN)
/* Implemented by manager_skin_game.c; see the header comment. */
int manager_skin_game_init(struct manager_theme *theme,
                           const struct manager_theme_fonts *fonts,
                           void *context);
#endif

#endif /* MANAGER_UI2 */

#endif
