/*
 * Manager UI2 flat themes `dark` and `light` and the theme selection (see
 * manager_theme.h).
 *
 * Colour words are copied from the UI2 design and its mock-up renderer
 * (design mock-ups A, B and C; not part of this repository):
 *   light (mock C, render_c_*): C_BG 0xfff4f2ee, C_BAR 0xffe3e0da,
 *     C_TEXT 0xff2b2b2b, C_SEC 0xff6b6b6b, C_HI 0xffffd28a,
 *     C_RED = RGB(160,32,32) = 0xff2020a0; ok/warn/error accents
 *     0xff2f8f2f / 0xff1a8fe0 / 0xff2020c8;
 *   dark (mock A, render_a; the pre-UI2 manager_main.c colours):
 *     bg 0xff171717, panel 0xff252525, accent 0xff3030a0, white
 *     0xffeeeeee, muted 0xffa8a8a8, selected 0xff80e0ff, warning
 *     0xff9090ff; status colours per the design (ok white, warning
 *     yellow = selected, error salmon = warning).
 * Choices made here, not in the mocks (neither mock draws the element):
 * scrim strength under a confirmation box, the light box fill (C_BG with a
 * C_SEC edge) and the dark theme's tab underline (selected yellow: mock C's
 * red underline would vanish on mock A's red bar).
 */
#if defined(MANAGER_UI2)

#include "manager_theme.h"

#include <stddef.h>
#include <string.h>

#define MANAGER_THEME_WIDTH 960
#define MANAGER_THEME_HEIGHT 544
#define MANAGER_THEME_HIGHLIGHT_RADIUS 8 /* mock C rounded_rectangle radius */
#define MANAGER_THEME_PANEL_RADIUS 8

/* mock C */
#define MANAGER_C_BG 0xfff4f2eeu
#define MANAGER_C_BAR 0xffe3e0dau
#define MANAGER_C_TEXT 0xff2b2b2bu
#define MANAGER_C_SEC 0xff6b6b6bu
#define MANAGER_C_HI 0xffffd28au
#define MANAGER_C_RED 0xff2020a0u /* (160, 32, 32) */
#define MANAGER_C_OK 0xff2f8f2fu
#define MANAGER_C_WARN 0xff1a8fe0u
#define MANAGER_C_ERROR 0xff2020c8u

/* mock A */
#define MANAGER_A_BG 0xff171717u
#define MANAGER_A_PANEL 0xff252525u
#define MANAGER_A_ACCENT 0xff3030a0u
#define MANAGER_A_WHITE 0xffeeeeeeu
#define MANAGER_A_MUTED 0xffa8a8a8u
#define MANAGER_A_SELECTED 0xff80e0ffu
#define MANAGER_A_WARNING 0xff9090ffu

static const char *const manager_glyph_texts[MANAGER_GLYPH_COUNT] = {
    "",       /* NONE */
    "×",      /* CROSS */
    "○",      /* CIRCLE */
    "□",      /* SQUARE */
    "△",      /* TRIANGLE */
    "↑↓",     /* UP_DOWN */
    "←→",     /* LEFT_RIGHT */
    "L R",    /* L_R */
    "START",  /* START */
    "☑",      /* CHECK_ON */
    "☐",      /* CHECK_OFF */
    "›",      /* MORE */
    "▸",      /* LAUNCH */
    "•",      /* PENDING */
    "◂",      /* PREV */
    "▸",      /* NEXT */
};

const char *manager_theme_glyph_text(enum manager_glyph glyph)
{
    if ((unsigned int)glyph >= (unsigned int)MANAGER_GLYPH_COUNT)
        return "";
    return manager_glyph_texts[glyph];
}

int manager_theme_flat_glyph(const struct manager_theme *theme,
                             struct manager_canvas *canvas, int x, int y,
                             enum manager_glyph glyph, uint32_t color)
{
    const char *text = manager_theme_glyph_text(glyph);
    if (text[0] == '\0')
        return 0;
    if (canvas == NULL)
        return manager_face_measure(theme->face[MANAGER_FACE_BODY], text);
    return manager_face_draw(theme->face[MANAGER_FACE_BODY], canvas, x, y,
                             color, text);
}

/* ---- shared flat pieces ---------------------------------------------- */

static void manager_flat_bars(const struct manager_theme *theme,
                              struct manager_canvas *canvas)
{
    const uint32_t *c = theme->palette.color;
    manager_canvas_fill_box(canvas, 0, 0, MANAGER_THEME_WIDTH - 1,
                            MANAGER_THEME_HEIGHT - 1,
                            c[MANAGER_COLOR_BACKGROUND]);
    /* mock C c_frame / mock A render_a: (0,0,W,63) and (0,496,W,H). */
    manager_canvas_fill_box(canvas, 0, 0, MANAGER_THEME_WIDTH, 63,
                            c[MANAGER_COLOR_BAR]);
    manager_canvas_fill_box(canvas, 0, 496, MANAGER_THEME_WIDTH,
                            MANAGER_THEME_HEIGHT, c[MANAGER_COLOR_BAR]);
}

static void manager_flat_highlight(const struct manager_theme *theme,
                                   struct manager_canvas *canvas, int x0,
                                   int y0, int x1, int y1)
{
    manager_canvas_round_box(canvas, x0, y0, x1, y1,
                             MANAGER_THEME_HIGHLIGHT_RADIUS,
                             theme->palette.color[MANAGER_COLOR_HIGHLIGHT]);
}

/* ---- light (mock C) --------------------------------------------------- */

static void manager_light_panel(const struct manager_theme *theme,
                                struct manager_canvas *canvas, int x0,
                                int y0, int x1, int y1,
                                enum manager_panel_kind kind)
{
    const uint32_t *c = theme->palette.color;
    if (kind == MANAGER_PANEL_BOX) {
        manager_canvas_round_box(canvas, x0, y0, x1, y1,
                                 MANAGER_THEME_PANEL_RADIUS,
                                 c[MANAGER_COLOR_BACKGROUND]);
        manager_canvas_round_frame(canvas, x0, y0, x1, y1,
                                   MANAGER_THEME_PANEL_RADIUS, 2,
                                   c[MANAGER_COLOR_PANEL_EDGE]);
        return;
    }
    manager_canvas_round_box(canvas, x0, y0, x1, y1,
                             MANAGER_THEME_PANEL_RADIUS,
                             kind == MANAGER_PANEL_CARD_SELECTED
                                 ? c[MANAGER_COLOR_HIGHLIGHT]
                                 : c[MANAGER_COLOR_PANEL]);
}

static const struct manager_theme_ops manager_light_ops = {
    manager_flat_bars,
    manager_light_panel,
    manager_flat_highlight,
    manager_theme_flat_glyph,
};

void manager_theme_init_light(struct manager_theme *theme,
                              const struct manager_theme_fonts *fonts)
{
    uint32_t *c;
    memset(theme, 0, sizeof(*theme));
    theme->id = MANAGER_THEME_LIGHT;
    c = theme->palette.color;
    c[MANAGER_COLOR_BACKGROUND] = MANAGER_C_BG;
    c[MANAGER_COLOR_BAR] = MANAGER_C_BAR;
    c[MANAGER_COLOR_BAR_TEXT] = MANAGER_C_TEXT;
    c[MANAGER_COLOR_BAR_MUTED] = MANAGER_C_SEC;
    c[MANAGER_COLOR_TAB_MARK] = MANAGER_C_RED;
    c[MANAGER_COLOR_TEXT] = MANAGER_C_TEXT;
    c[MANAGER_COLOR_MUTED] = MANAGER_C_SEC;
    c[MANAGER_COLOR_HIGHLIGHT] = MANAGER_C_HI;
    c[MANAGER_COLOR_HIGHLIGHT_TEXT] = MANAGER_C_TEXT;
    c[MANAGER_COLOR_HIGHLIGHT_MUTED] = MANAGER_C_SEC;
    c[MANAGER_COLOR_SEPARATOR] = MANAGER_C_BAR;
    c[MANAGER_COLOR_SCROLL_THUMB] = MANAGER_C_SEC;
    c[MANAGER_COLOR_PANEL] = MANAGER_C_BAR;
    c[MANAGER_COLOR_PANEL_EDGE] = MANAGER_C_SEC;
    c[MANAGER_COLOR_SCRIM] = MANAGER_C_TEXT;
    c[MANAGER_COLOR_OK] = MANAGER_C_OK;
    c[MANAGER_COLOR_WARNING] = MANAGER_C_WARN;
    c[MANAGER_COLOR_ERROR] = MANAGER_C_ERROR;
    theme->palette.scrim_alpha = 96u;
    if (fonts != NULL)
        memcpy(theme->face, fonts->face, sizeof(theme->face));
    theme->ops = &manager_light_ops;
    theme->title_x = 32;
}

/* ---- dark (mock A palette and bars) ----------------------------------- */

static void manager_dark_panel(const struct manager_theme *theme,
                               struct manager_canvas *canvas, int x0, int y0,
                               int x1, int y1, enum manager_panel_kind kind)
{
    const uint32_t *c = theme->palette.color;
    manager_canvas_round_box(canvas, x0, y0, x1, y1,
                             MANAGER_THEME_PANEL_RADIUS,
                             c[MANAGER_COLOR_PANEL]);
    if (kind == MANAGER_PANEL_BOX)
        manager_canvas_round_frame(canvas, x0, y0, x1, y1,
                                   MANAGER_THEME_PANEL_RADIUS, 2,
                                   c[MANAGER_COLOR_PANEL_EDGE]);
    else if (kind == MANAGER_PANEL_CARD_SELECTED)
        manager_canvas_round_frame(canvas, x0, y0, x1, y1,
                                   MANAGER_THEME_PANEL_RADIUS, 2,
                                   c[MANAGER_COLOR_HIGHLIGHT_TEXT]);
}

static const struct manager_theme_ops manager_dark_ops = {
    manager_flat_bars,
    manager_dark_panel,
    manager_flat_highlight,
    manager_theme_flat_glyph,
};

void manager_theme_init_dark(struct manager_theme *theme,
                             const struct manager_theme_fonts *fonts)
{
    uint32_t *c;
    memset(theme, 0, sizeof(*theme));
    theme->id = MANAGER_THEME_DARK;
    c = theme->palette.color;
    c[MANAGER_COLOR_BACKGROUND] = MANAGER_A_BG;
    c[MANAGER_COLOR_BAR] = MANAGER_A_ACCENT;
    c[MANAGER_COLOR_BAR_TEXT] = MANAGER_A_WHITE;
    c[MANAGER_COLOR_BAR_MUTED] = MANAGER_A_MUTED;
    c[MANAGER_COLOR_TAB_MARK] = MANAGER_A_SELECTED;
    c[MANAGER_COLOR_TEXT] = MANAGER_A_WHITE;
    c[MANAGER_COLOR_MUTED] = MANAGER_A_MUTED;
    c[MANAGER_COLOR_HIGHLIGHT] = MANAGER_A_PANEL;
    c[MANAGER_COLOR_HIGHLIGHT_TEXT] = MANAGER_A_SELECTED;
    c[MANAGER_COLOR_HIGHLIGHT_MUTED] = MANAGER_A_MUTED;
    c[MANAGER_COLOR_SEPARATOR] = MANAGER_A_PANEL;
    c[MANAGER_COLOR_SCROLL_THUMB] = MANAGER_A_MUTED;
    c[MANAGER_COLOR_PANEL] = MANAGER_A_PANEL;
    c[MANAGER_COLOR_PANEL_EDGE] = MANAGER_A_ACCENT;
    c[MANAGER_COLOR_SCRIM] = MANAGER_A_BG;
    c[MANAGER_COLOR_OK] = MANAGER_A_WHITE;
    c[MANAGER_COLOR_WARNING] = MANAGER_A_SELECTED;
    c[MANAGER_COLOR_ERROR] = MANAGER_A_WARNING;
    theme->palette.scrim_alpha = 160u;
    if (fonts != NULL)
        memcpy(theme->face, fonts->face, sizeof(theme->face));
    theme->ops = &manager_dark_ops;
    theme->title_x = 32;
}

/* ---- selection -------------------------------------------------------- */

void manager_theme_set_init(struct manager_theme_set *set,
                            const struct manager_theme_fonts *fonts,
                            void *game_context)
{
    memset(set, 0, sizeof(*set));
    if (fonts != NULL)
        set->fonts = *fonts;
    manager_theme_init_dark(&set->dark, &set->fonts);
    manager_theme_init_light(&set->light, &set->fonts);
    set->game_context = game_context;
    set->game_state = 0;
}

const struct manager_theme *manager_theme_select(
    struct manager_theme_set *set, enum manager_theme_id wanted,
    enum manager_theme_fallback *fallback)
{
    enum manager_theme_fallback why = MANAGER_THEME_FALLBACK_NONE;
    const struct manager_theme *chosen = &set->light;

    if (wanted == MANAGER_THEME_DARK) {
        chosen = &set->dark;
    } else if (wanted == MANAGER_THEME_GAME) {
#if defined(MANAGER_UI2_GAME_SKIN)
        if (set->game_state == 0) {
            memset(&set->game, 0, sizeof(set->game));
            if (manager_skin_game_init(&set->game, &set->fonts,
                                       set->game_context) == 0 &&
                set->game.ops != NULL && set->game.ops->draw_background &&
                set->game.ops->draw_panel && set->game.ops->draw_row_highlight &&
                set->game.ops->draw_hint_glyph) {
                set->game.id = MANAGER_THEME_GAME;
                set->game_state = 1;
            } else {
                memset(&set->game, 0, sizeof(set->game));
                set->game_state = -1;
            }
        }
        if (set->game_state == 1)
            chosen = &set->game;
        else
            why = MANAGER_THEME_FALLBACK_FAILED;
#else
        set->game_state = -1;
        why = MANAGER_THEME_FALLBACK_NOT_BUILT;
#endif
    }
    if (fallback != NULL)
        *fallback = why;
    return chosen;
}

#endif /* MANAGER_UI2 */
