/*
 * Manager UI2 display list and its painter (the UI2 design:
 * "manager_ui.c ... emits a display list (rows, cards, hints,
 * status) - not pixels"; "manager_ui_draw.c: display list -> canvas via
 * manager_canvas + manager_font (theme = palette + draw_background)").
 *
 * The list carries the chrome every page has (title, tab strip, status row,
 * hint bar) plus positioned items in paint order.  Coordinates are the
 * mock C numbers (design mock-up C): text items give the pen position with
 * y on the ascender line (Pillow anchor "la"), boxes are inclusive.  Colours
 * and faces are roles resolved through the theme, so one list paints in any
 * theme.  Strings live in a pool inside the list; a list that ran out of
 * items or pool bytes sets `overflow` (the host tests fail on it) and keeps
 * what fitted.
 *
 * Paint order: theme background, title and tabs, status row, items (a SCRIM
 * item dims everything painted so far above the hint bar, the items after
 * it are the overlay), hint bar labels last.
 */
#ifndef ISAAC_MANAGER_UI_DRAW_H
#define ISAAC_MANAGER_UI_DRAW_H

#if defined(MANAGER_UI2)

#include "manager_theme.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define MANAGER_DL_MAX_ITEMS 224u
#define MANAGER_DL_POOL_BYTES 12288u
#define MANAGER_DL_MAX_TABS 7u
#define MANAGER_DL_MAX_HINTS 6u

/* Chrome geometry (mock C c_frame / c_hints / status row). */
#define MANAGER_DL_TITLE_Y 18
#define MANAGER_DL_TABS_X 350
#define MANAGER_DL_TABS_Y 26
#define MANAGER_DL_TAB_GAP 18
#define MANAGER_DL_TAB_MARK_Y0 44
#define MANAGER_DL_TAB_MARK_Y1 47
#define MANAGER_DL_HINT_Y 510
#define MANAGER_DL_HINT_LEFT_X 32
#define MANAGER_DL_HINT_RIGHT_X 928
#define MANAGER_DL_HINT_GAP 36
#define MANAGER_DL_STATUS_X 32
#define MANAGER_DL_STATUS_Y 470
#define MANAGER_DL_STATUS_RIGHT 928

#if defined(MANAGER_UI2_GAME_SKIN)
/* Game skin layout (owner's device review 2026-10-10): every element sits
 * inside the inner rectangle of the skin's paper, x 30..927 and y 24..463
 * (measured on the composed KeyConfigMenu sheet, which reaches down to the
 * hint bar, see manager_skin_game.c); a tab row across the width under the
 * title, the page info on the title row, the status line on the paper's
 * bottom.  Faces: rows and their values in BODY, secondary lines (sub-lines,
 * headings, page info, status, hint labels) in SMALL.  Used by the painter
 * and manager_ui.c only when the resolved theme is MANAGER_THEME_GAME. */
#define MANAGER_DL_GAME_LAYOUT(theme) ((theme)->id == MANAGER_THEME_GAME)
#define MANAGER_DL_GAME_X0 40      /* title, tabs, rules */
#define MANAGER_DL_GAME_X1 912
/* RU/EN device screenshots 2026-10-10: title ink was y18..41 and crossed
 * the torn paper edge.  Ten pixels lower gives y28..51, still above the
 * tab ink at y55; the font size and the tab/content positions stay intact. */
#define MANAGER_DL_GAME_TITLE_Y 26
#define MANAGER_DL_GAME_TABS_Y 52
#define MANAGER_DL_GAME_TAB_GAP_MIN 16
#define MANAGER_DL_GAME_TAB_MARK_Y0 77
#define MANAGER_DL_GAME_TAB_MARK_Y1 80
#define MANAGER_DL_GAME_RULE_Y 80  /* header rule, 2 px */
#define MANAGER_DL_GAME_TOP 90     /* first content pixel */
#define MANAGER_DL_GAME_BOTTOM 430 /* content ends above this y */
#define MANAGER_DL_GAME_STATUS_RULE_Y 436
#define MANAGER_DL_GAME_STATUS_Y 442
#define MANAGER_DL_GAME_BAND_X0 36 /* selected-row band; the skin's cursor */
#define MANAGER_DL_GAME_BAND_X1 912 /* sits at its left inside it */
#define MANAGER_DL_GAME_TEXT_X 72  /* row labels and section headings */
#define MANAGER_DL_GAME_SUB_X 88   /* sub-lines */
#define MANAGER_DL_GAME_PENDING_X 62
#define MANAGER_DL_GAME_VALUE_R 890 /* right edge of row values */
#define MANAGER_DL_GAME_MORE_X 896  /* "›" and "▸" after the value */
#define MANAGER_DL_GAME_SCROLL_X 918 /* 4 px track, x..x+4 */
#define MANAGER_DL_GAME_ROW_TEXT_DY 5 /* label pen below the band top */
#endif

enum manager_dl_kind {
    MANAGER_DL_TEXT = 0,  /* pool string at the pen */
    MANAGER_DL_GLYPH,     /* theme glyph at the pen */
    MANAGER_DL_HIGHLIGHT, /* selected-row box */
    MANAGER_DL_PANEL,     /* card / box (panel kind in `panel`) */
    MANAGER_DL_SEPARATOR, /* 2 px line x..x1 at y */
    MANAGER_DL_SCROLL,    /* track box, thumb rows a..b */
    MANAGER_DL_SCRIM,     /* dim the box with the theme scrim */
    MANAGER_DL_FILL,      /* plain box in a colour role */
    MANAGER_DL_FRAME      /* outline, radius a, thickness b */
};

enum manager_dl_align {
    MANAGER_DL_LEFT = 0,
    MANAGER_DL_RIGHT, /* x is the right edge */
    MANAGER_DL_CENTER /* x is the centre */
};

enum manager_tone {
    MANAGER_TONE_NORMAL = 0,
    MANAGER_TONE_OK,
    MANAGER_TONE_WARNING,
    MANAGER_TONE_ERROR
};

struct manager_dl_item {
    uint8_t kind;  /* enum manager_dl_kind */
    uint8_t face;  /* enum manager_face_role */
    uint8_t color; /* enum manager_color */
    uint8_t align; /* enum manager_dl_align */
    uint8_t glyph; /* enum manager_glyph */
    uint8_t panel; /* enum manager_panel_kind */
    int16_t x;
    int16_t y;
    int16_t x1; /* boxes: right edge; text: the limit (0 = none): right
                 * edge for LEFT, left edge for RIGHT, width for CENTER */
    int16_t y1;
    int16_t a;
    int16_t b;
    uint16_t text; /* pool offset */
};

struct manager_dl_hint {
    uint8_t glyph; /* enum manager_glyph */
    uint8_t right; /* 0 left group, 1 right group */
    uint16_t text; /* pool offset of the label */
};

struct manager_display_list {
    uint16_t title;
    uint16_t tabs[MANAGER_DL_MAX_TABS];
    uint8_t tab_count;
    int8_t active_tab; /* -1 none */
    uint16_t status;
    uint8_t status_tone; /* enum manager_tone */
    uint8_t hint_count;
    struct manager_dl_hint hints[MANAGER_DL_MAX_HINTS];
    uint16_t count;
    uint8_t overflow;
    struct manager_dl_item items[MANAGER_DL_MAX_ITEMS];
    uint16_t used;
    char pool[MANAGER_DL_POOL_BYTES];
};

void manager_dl_reset(struct manager_display_list *dl);
/* Pool string (offset 0 is the empty string). */
uint16_t manager_dl_string(struct manager_display_list *dl, const char *text);
uint16_t manager_dl_format(struct manager_display_list *dl, const char *format,
                           ...) __attribute__((format(printf, 2, 3)));
uint16_t manager_dl_vformat(struct manager_display_list *dl,
                            const char *format, va_list arguments);
const char *manager_dl_text_at(const struct manager_display_list *dl,
                               uint16_t offset);

/* Item appenders; each returns the item or NULL when the list is full. */
struct manager_dl_item *manager_dl_text(struct manager_display_list *dl,
                                        int x, int y,
                                        enum manager_face_role face,
                                        enum manager_color color,
                                        enum manager_dl_align align,
                                        int limit, const char *text);
struct manager_dl_item *manager_dl_glyph(struct manager_display_list *dl,
                                         int x, int y, enum manager_glyph glyph,
                                         enum manager_color color,
                                         enum manager_dl_align align);
struct manager_dl_item *manager_dl_box(struct manager_display_list *dl,
                                       enum manager_dl_kind kind, int x0,
                                       int y0, int x1, int y1);
struct manager_dl_item *manager_dl_panel(struct manager_display_list *dl,
                                         int x0, int y0, int x1, int y1,
                                         enum manager_panel_kind panel);
struct manager_dl_item *manager_dl_separator(struct manager_display_list *dl,
                                             int x0, int x1, int y);
struct manager_dl_item *manager_dl_scroll(struct manager_display_list *dl,
                                          int x0, int y0, int x1, int y1,
                                          int thumb_y0, int thumb_y1);
struct manager_dl_item *manager_dl_fill(struct manager_display_list *dl,
                                        int x0, int y0, int x1, int y1,
                                        enum manager_color color);
struct manager_dl_item *manager_dl_frame(struct manager_display_list *dl,
                                         int x0, int y0, int x1, int y1,
                                         int radius, int thickness,
                                         enum manager_color color);

void manager_dl_set_title(struct manager_display_list *dl, const char *text);
void manager_dl_add_tab(struct manager_display_list *dl, const char *text);
void manager_dl_set_status(struct manager_display_list *dl,
                           enum manager_tone tone, const char *text);
void manager_dl_add_hint(struct manager_display_list *dl, int right,
                         enum manager_glyph glyph, const char *label);

/* Width of the text in the theme's face (layout helper for the builder). */
int manager_dl_measure(const struct manager_theme *theme,
                       enum manager_face_role face, const char *text);
/* Width of one hint (glyph, a space, label) in the body face. */
int manager_dl_hint_width(const struct manager_theme *theme,
                          const struct manager_display_list *dl,
                          const struct manager_dl_hint *hint);

/* Paints the whole frame (every pixel of the 960x544 canvas). */
void manager_ui_draw(const struct manager_display_list *dl,
                     const struct manager_theme *theme,
                     struct manager_canvas *canvas);

/* Plain-text dump of the list (one line per element) for host-test
 * goldens and diagnostics; returns the bytes written (NUL excluded). */
size_t manager_dl_dump(const struct manager_display_list *dl, char *out,
                       size_t capacity);

#endif /* MANAGER_UI2 */

#endif
