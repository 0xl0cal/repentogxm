/*
 * Manager UI2 display list and painter (see manager_ui_draw.h).
 */
#if defined(MANAGER_UI2)

#include "manager_ui_draw.h"

#include <stdio.h>
#include <string.h>

/* Hint labels: BODY, in the game layout SMALL with its baseline on the
 * BODY baseline (the button sprites are centred on the BODY capitals). */
#if defined(MANAGER_UI2_GAME_SKIN)
#define MANAGER_DL_HINT_FACE(theme)                                            \
    (MANAGER_DL_GAME_LAYOUT(theme) ? MANAGER_FACE_SMALL : MANAGER_FACE_BODY)
#define MANAGER_DL_HINT_LABEL_Y(theme)                                         \
    (MANAGER_DL_HINT_Y +                                                       \
     (MANAGER_DL_GAME_LAYOUT(theme)                                            \
          ? manager_face_ascent((theme)->face[MANAGER_FACE_BODY]) -            \
                manager_face_ascent((theme)->face[MANAGER_FACE_SMALL])         \
          : 0))
#else
#define MANAGER_DL_HINT_FACE(theme) MANAGER_FACE_BODY
#define MANAGER_DL_HINT_LABEL_Y(theme) MANAGER_DL_HINT_Y
#endif

void manager_dl_reset(struct manager_display_list *dl)
{
    memset(dl, 0, sizeof(*dl));
    dl->active_tab = -1;
    dl->pool[0] = '\0';
    dl->used = 1u; /* offset 0 = "" */
}

uint16_t manager_dl_string(struct manager_display_list *dl, const char *text)
{
    size_t length;
    uint16_t offset;
    if (text == NULL || text[0] == '\0')
        return 0u;
    length = strlen(text);
    if ((size_t)dl->used + length + 1u > sizeof(dl->pool)) {
        dl->overflow = 1u;
        return 0u;
    }
    offset = dl->used;
    memcpy(dl->pool + offset, text, length + 1u);
    dl->used = (uint16_t)(dl->used + length + 1u);
    return offset;
}

uint16_t manager_dl_vformat(struct manager_display_list *dl,
                            const char *format, va_list arguments)
{
    const size_t room = sizeof(dl->pool) - dl->used;
    uint16_t offset;
    int written;
    if (room < 2u) {
        dl->overflow = 1u;
        return 0u;
    }
    offset = dl->used;
    written = vsnprintf(dl->pool + offset, room, format, arguments);
    if (written < 0) {
        dl->pool[offset] = '\0';
        return 0u;
    }
    if ((size_t)written >= room) {
        dl->overflow = 1u;
        dl->pool[offset] = '\0';
        return 0u;
    }
    if (written == 0)
        return 0u;
    dl->used = (uint16_t)(dl->used + (size_t)written + 1u);
    return offset;
}

uint16_t manager_dl_format(struct manager_display_list *dl, const char *format,
                           ...)
{
    va_list arguments;
    uint16_t offset;
    va_start(arguments, format);
    offset = manager_dl_vformat(dl, format, arguments);
    va_end(arguments);
    return offset;
}

const char *manager_dl_text_at(const struct manager_display_list *dl,
                               uint16_t offset)
{
    if (offset >= dl->used)
        return "";
    return dl->pool + offset;
}

static struct manager_dl_item *manager_dl_push(struct manager_display_list *dl,
                                               enum manager_dl_kind kind)
{
    struct manager_dl_item *item;
    if (dl->count >= MANAGER_DL_MAX_ITEMS) {
        dl->overflow = 1u;
        return NULL;
    }
    item = &dl->items[dl->count++];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)kind;
    return item;
}

struct manager_dl_item *manager_dl_text(struct manager_display_list *dl,
                                        int x, int y,
                                        enum manager_face_role face,
                                        enum manager_color color,
                                        enum manager_dl_align align,
                                        int limit, const char *text)
{
    struct manager_dl_item *item;
    uint16_t offset;
    if (text == NULL || text[0] == '\0')
        return NULL;
    offset = manager_dl_string(dl, text);
    if (offset == 0u)
        return NULL;
    item = manager_dl_push(dl, MANAGER_DL_TEXT);
    if (item == NULL)
        return NULL;
    item->face = (uint8_t)face;
    item->color = (uint8_t)color;
    item->align = (uint8_t)align;
    item->x = (int16_t)x;
    item->y = (int16_t)y;
    item->x1 = (int16_t)limit;
    item->text = offset;
    return item;
}

struct manager_dl_item *manager_dl_glyph(struct manager_display_list *dl,
                                         int x, int y, enum manager_glyph glyph,
                                         enum manager_color color,
                                         enum manager_dl_align align)
{
    struct manager_dl_item *item = manager_dl_push(dl, MANAGER_DL_GLYPH);
    if (item == NULL)
        return NULL;
    item->glyph = (uint8_t)glyph;
    item->color = (uint8_t)color;
    item->align = (uint8_t)align;
    item->face = (uint8_t)MANAGER_FACE_BODY;
    item->x = (int16_t)x;
    item->y = (int16_t)y;
    return item;
}

struct manager_dl_item *manager_dl_box(struct manager_display_list *dl,
                                       enum manager_dl_kind kind, int x0,
                                       int y0, int x1, int y1)
{
    struct manager_dl_item *item = manager_dl_push(dl, kind);
    if (item == NULL)
        return NULL;
    item->x = (int16_t)x0;
    item->y = (int16_t)y0;
    item->x1 = (int16_t)x1;
    item->y1 = (int16_t)y1;
    return item;
}

struct manager_dl_item *manager_dl_panel(struct manager_display_list *dl,
                                         int x0, int y0, int x1, int y1,
                                         enum manager_panel_kind panel)
{
    struct manager_dl_item *item =
        manager_dl_box(dl, MANAGER_DL_PANEL, x0, y0, x1, y1);
    if (item != NULL)
        item->panel = (uint8_t)panel;
    return item;
}

struct manager_dl_item *manager_dl_separator(struct manager_display_list *dl,
                                             int x0, int x1, int y)
{
    struct manager_dl_item *item =
        manager_dl_box(dl, MANAGER_DL_SEPARATOR, x0, y, x1, y + 1);
    if (item != NULL)
        item->color = (uint8_t)MANAGER_COLOR_SEPARATOR;
    return item;
}

struct manager_dl_item *manager_dl_scroll(struct manager_display_list *dl,
                                          int x0, int y0, int x1, int y1,
                                          int thumb_y0, int thumb_y1)
{
    struct manager_dl_item *item =
        manager_dl_box(dl, MANAGER_DL_SCROLL, x0, y0, x1, y1);
    if (item != NULL) {
        item->a = (int16_t)thumb_y0;
        item->b = (int16_t)thumb_y1;
    }
    return item;
}

struct manager_dl_item *manager_dl_fill(struct manager_display_list *dl,
                                        int x0, int y0, int x1, int y1,
                                        enum manager_color color)
{
    struct manager_dl_item *item =
        manager_dl_box(dl, MANAGER_DL_FILL, x0, y0, x1, y1);
    if (item != NULL)
        item->color = (uint8_t)color;
    return item;
}

struct manager_dl_item *manager_dl_frame(struct manager_display_list *dl,
                                         int x0, int y0, int x1, int y1,
                                         int radius, int thickness,
                                         enum manager_color color)
{
    struct manager_dl_item *item =
        manager_dl_box(dl, MANAGER_DL_FRAME, x0, y0, x1, y1);
    if (item != NULL) {
        item->color = (uint8_t)color;
        item->a = (int16_t)radius;
        item->b = (int16_t)(thickness < 1 ? 1 : thickness);
    }
    return item;
}

void manager_dl_set_title(struct manager_display_list *dl, const char *text)
{
    dl->title = manager_dl_string(dl, text);
}

void manager_dl_add_tab(struct manager_display_list *dl, const char *text)
{
    if (dl->tab_count >= MANAGER_DL_MAX_TABS) {
        dl->overflow = 1u;
        return;
    }
    dl->tabs[dl->tab_count++] = manager_dl_string(dl, text);
}

void manager_dl_set_status(struct manager_display_list *dl,
                           enum manager_tone tone, const char *text)
{
    dl->status = manager_dl_string(dl, text);
    dl->status_tone = (uint8_t)tone;
}

void manager_dl_add_hint(struct manager_display_list *dl, int right,
                         enum manager_glyph glyph, const char *label)
{
    struct manager_dl_hint *hint;
    if (dl->hint_count >= MANAGER_DL_MAX_HINTS) {
        dl->overflow = 1u;
        return;
    }
    hint = &dl->hints[dl->hint_count++];
    hint->glyph = (uint8_t)glyph;
    hint->right = (uint8_t)(right != 0);
    hint->text = manager_dl_string(dl, label);
}

int manager_dl_measure(const struct manager_theme *theme,
                       enum manager_face_role face, const char *text)
{
    if ((unsigned int)face >= (unsigned int)MANAGER_FACE_COUNT || text == NULL)
        return 0;
    return manager_face_measure(theme->face[face], text);
}

int manager_dl_hint_width(const struct manager_theme *theme,
                          const struct manager_display_list *dl,
                          const struct manager_dl_hint *hint)
{
    const struct manager_face body = theme->face[MANAGER_DL_HINT_FACE(theme)];
    const char *label = manager_dl_text_at(dl, hint->text);
    int width = 0;
    if (hint->glyph != MANAGER_GLYPH_NONE)
        width = theme->ops->draw_hint_glyph(theme, NULL, 0, 0,
                                            (enum manager_glyph)hint->glyph,
                                            0u);
    if (hint->glyph != MANAGER_GLYPH_NONE && label[0] != '\0')
        width += manager_face_measure(body, " ");
    return width + manager_face_measure(body, label);
}

/* ---- painter ---------------------------------------------------------- */

static uint32_t manager_dl_color(const struct manager_theme *theme,
                                 unsigned int role)
{
    if (role >= (unsigned int)MANAGER_COLOR_COUNT)
        role = MANAGER_COLOR_TEXT;
    return theme->palette.color[role];
}

static void manager_dl_paint_text(const struct manager_display_list *dl,
                                  const struct manager_theme *theme,
                                  struct manager_canvas *canvas,
                                  const struct manager_dl_item *item)
{
    const unsigned int role =
        item->face < MANAGER_FACE_COUNT ? item->face : MANAGER_FACE_BODY;
    const struct manager_face face = theme->face[role];
    const char *text = manager_dl_text_at(dl, item->text);
    const uint32_t color = manager_dl_color(theme, item->color);
    int max_width = 0;
    int width;
    int x = item->x;

    if (item->align == MANAGER_DL_LEFT) {
        if (item->x1 > item->x)
            max_width = item->x1 - item->x;
    } else if (item->align == MANAGER_DL_RIGHT) {
        if (item->x1 != 0 && item->x1 < item->x)
            max_width = item->x - item->x1;
    } else if (item->x1 > 0) {
        max_width = item->x1;
    }
    width = manager_face_measure(face, text);
    if (max_width > 0 && width > max_width)
        width = manager_face_draw_clipped(face, NULL, 0, 0, color, text,
                                          max_width);
    if (item->align == MANAGER_DL_RIGHT)
        x -= width;
    else if (item->align == MANAGER_DL_CENTER)
        x -= width / 2;
    if (max_width > 0)
        (void)manager_face_draw_clipped(face, canvas, x, item->y, color, text,
                                        max_width);
    else
        (void)manager_face_draw(face, canvas, x, item->y, color, text);
}

static void manager_dl_paint_item(const struct manager_display_list *dl,
                                  const struct manager_theme *theme,
                                  struct manager_canvas *canvas,
                                  const struct manager_dl_item *item)
{
    switch ((enum manager_dl_kind)item->kind) {
    case MANAGER_DL_TEXT:
        manager_dl_paint_text(dl, theme, canvas, item);
        break;
    case MANAGER_DL_GLYPH: {
        const uint32_t color = manager_dl_color(theme, item->color);
        int x = item->x;
        if (item->align != MANAGER_DL_LEFT) {
            const int width = theme->ops->draw_hint_glyph(
                theme, NULL, 0, 0, (enum manager_glyph)item->glyph, color);
            x -= item->align == MANAGER_DL_RIGHT ? width : width / 2;
        }
        (void)theme->ops->draw_hint_glyph(theme, canvas, x, item->y,
                                          (enum manager_glyph)item->glyph,
                                          color);
        break;
    }
    case MANAGER_DL_HIGHLIGHT:
        theme->ops->draw_row_highlight(theme, canvas, item->x, item->y,
                                       item->x1, item->y1);
        break;
    case MANAGER_DL_PANEL:
        theme->ops->draw_panel(
            theme, canvas, item->x, item->y, item->x1, item->y1,
            item->panel < MANAGER_PANEL_COUNT
                ? (enum manager_panel_kind)item->panel
                : MANAGER_PANEL_BOX);
        break;
    case MANAGER_DL_SEPARATOR:
        manager_canvas_hline(canvas, item->x, item->x1, item->y, 2,
                             manager_dl_color(theme, item->color));
        break;
    case MANAGER_DL_SCROLL:
        manager_canvas_fill_box(
            canvas, item->x, item->y, item->x1, item->y1,
            manager_dl_color(theme, MANAGER_COLOR_SEPARATOR));
        manager_canvas_fill_box(
            canvas, item->x, item->a, item->x1, item->b,
            manager_dl_color(theme, MANAGER_COLOR_SCROLL_THUMB));
        break;
    case MANAGER_DL_SCRIM:
        manager_canvas_blend_box(canvas, item->x, item->y, item->x1, item->y1,
                                 manager_dl_color(theme, MANAGER_COLOR_SCRIM),
                                 theme->palette.scrim_alpha);
        break;
    case MANAGER_DL_FILL:
        manager_canvas_fill_box(canvas, item->x, item->y, item->x1, item->y1,
                                manager_dl_color(theme, item->color));
        break;
    case MANAGER_DL_FRAME:
        manager_canvas_round_frame(canvas, item->x, item->y, item->x1,
                                   item->y1, item->a, item->b,
                                   manager_dl_color(theme, item->color));
        break;
    }
}

#if defined(MANAGER_UI2_GAME_SKIN)
/* Game layout: the title alone on its row (the page info the builder puts
 * at its right is an item), the tabs in BODY spread over x0..x1 on the row
 * below with the remainder shared out between the gaps, a rule under them
 * that the active tab's mark sits on.  Tabs too wide for the minimum gap
 * fall back to SMALL. */
static void manager_dl_paint_game_chrome(const struct manager_display_list *dl,
                                         const struct manager_theme *theme,
                                         struct manager_canvas *canvas)
{
    const uint32_t *c = theme->palette.color;
    const int span = MANAGER_DL_GAME_X1 - MANAGER_DL_GAME_X0;
    struct manager_face face = theme->face[MANAGER_FACE_BODY];
    int widths[MANAGER_DL_MAX_TABS];
    int total = 0;
    int left = 0;
    unsigned int index;
    (void)manager_face_draw_clipped(
        theme->face[MANAGER_FACE_TITLE], canvas, theme->title_x,
        MANAGER_DL_GAME_TITLE_Y, c[MANAGER_COLOR_BAR_TEXT],
        manager_dl_text_at(dl, dl->title), MANAGER_DL_GAME_X1 - theme->title_x);
    manager_canvas_hline(canvas, MANAGER_DL_GAME_X0, MANAGER_DL_GAME_X1,
                         MANAGER_DL_GAME_RULE_Y, 2, c[MANAGER_COLOR_SEPARATOR]);
    if (dl->tab_count == 0u)
        return;
    for (index = 0u; index < dl->tab_count; ++index)
        total += manager_face_measure(face,
                                      manager_dl_text_at(dl, dl->tabs[index]));
    if (span - total <
        MANAGER_DL_GAME_TAB_GAP_MIN * (int)(dl->tab_count - 1u)) {
        face = theme->face[MANAGER_FACE_SMALL];
        total = 0;
        for (index = 0u; index < dl->tab_count; ++index)
            total += manager_face_measure(
                face, manager_dl_text_at(dl, dl->tabs[index]));
    }
    for (index = 0u; index < dl->tab_count; ++index)
        widths[index] =
            manager_face_measure(face, manager_dl_text_at(dl, dl->tabs[index]));
    for (index = 0u; index < dl->tab_count; ++index) {
        const char *tab = manager_dl_text_at(dl, dl->tabs[index]);
        const int active = (int)index == dl->active_tab;
        int x = MANAGER_DL_GAME_X0 + left;
        if (dl->tab_count > 1u)
            x += (span - total) * (int)index / (int)(dl->tab_count - 1u);
        (void)manager_face_draw_clipped(face, canvas, x, MANAGER_DL_GAME_TABS_Y,
                                        active ? c[MANAGER_COLOR_BAR_TEXT]
                                               : c[MANAGER_COLOR_BAR_MUTED],
                                        tab, MANAGER_DL_GAME_X1 - x);
        if (active)
            manager_canvas_fill_box(canvas, x, MANAGER_DL_GAME_TAB_MARK_Y0,
                                    x + widths[index] - 1,
                                    MANAGER_DL_GAME_TAB_MARK_Y1,
                                    c[MANAGER_COLOR_TAB_MARK]);
        left += widths[index];
    }
}
#endif

static void manager_dl_paint_chrome(const struct manager_display_list *dl,
                                    const struct manager_theme *theme,
                                    struct manager_canvas *canvas)
{
    const struct manager_face title = theme->face[MANAGER_FACE_TITLE];
    const struct manager_face small = theme->face[MANAGER_FACE_SMALL];
    const uint32_t *c = theme->palette.color;
    unsigned int index;
    int x = MANAGER_DL_TABS_X;
    int total = 0;

#if defined(MANAGER_UI2_GAME_SKIN)
    if (MANAGER_DL_GAME_LAYOUT(theme)) {
        manager_dl_paint_game_chrome(dl, theme, canvas);
        return;
    }
#endif

    /* Mock C puts the tabs at x=350; a wider face (the 5x7 fallback, a
     * skin's font) moves them left so the last one still ends by 928,
     * taking the room from the title, never from the tabs. */
    for (index = 0u; index < dl->tab_count; ++index)
        total += manager_face_measure(small,
                                      manager_dl_text_at(dl, dl->tabs[index])) +
                 (index != 0u ? MANAGER_DL_TAB_GAP : 0);
    if (x + total > MANAGER_DL_HINT_RIGHT_X) {
        x = MANAGER_DL_HINT_RIGHT_X - total;
        if (x < theme->title_x + 12)
            x = theme->title_x + 12;
    }
    (void)manager_face_draw_clipped(
        title, canvas, theme->title_x, MANAGER_DL_TITLE_Y,
        c[MANAGER_COLOR_BAR_TEXT], manager_dl_text_at(dl, dl->title),
        x - 12 - theme->title_x);
    for (index = 0u; index < dl->tab_count; ++index) {
        const char *tab = manager_dl_text_at(dl, dl->tabs[index]);
        const int active = (int)index == dl->active_tab;
        const int width = manager_face_measure(small, tab);
        (void)manager_face_draw(small, canvas, x, MANAGER_DL_TABS_Y,
                                active ? c[MANAGER_COLOR_BAR_TEXT]
                                       : c[MANAGER_COLOR_BAR_MUTED],
                                tab);
        if (active)
            manager_canvas_fill_box(canvas, x, MANAGER_DL_TAB_MARK_Y0,
                                    x + width, MANAGER_DL_TAB_MARK_Y1,
                                    c[MANAGER_COLOR_TAB_MARK]);
        x += width + MANAGER_DL_TAB_GAP;
    }
}

static void manager_dl_paint_status(const struct manager_display_list *dl,
                                    const struct manager_theme *theme,
                                    struct manager_canvas *canvas)
{
    static const unsigned char tone_color[4] = {
        MANAGER_COLOR_TEXT, MANAGER_COLOR_OK, MANAGER_COLOR_WARNING,
        MANAGER_COLOR_ERROR};
    const unsigned int tone = dl->status_tone < 4u ? dl->status_tone : 0u;
#if defined(MANAGER_UI2_GAME_SKIN)
    if (MANAGER_DL_GAME_LAYOUT(theme)) {
        /* On the paper's bottom under a rule, the same place on every page. */
        manager_canvas_hline(canvas, MANAGER_DL_GAME_X0, MANAGER_DL_GAME_X1,
                             MANAGER_DL_GAME_STATUS_RULE_Y, 2,
                             manager_dl_color(theme, MANAGER_COLOR_SEPARATOR));
        (void)manager_face_draw_clipped(
            theme->face[MANAGER_FACE_SMALL], canvas, MANAGER_DL_GAME_X0,
            MANAGER_DL_GAME_STATUS_Y, manager_dl_color(theme, tone_color[tone]),
            manager_dl_text_at(dl, dl->status),
            MANAGER_DL_GAME_X1 - MANAGER_DL_GAME_X0);
        return;
    }
#endif
    (void)manager_face_draw_clipped(
        theme->face[MANAGER_FACE_SMALL], canvas, MANAGER_DL_STATUS_X,
        MANAGER_DL_STATUS_Y, manager_dl_color(theme, tone_color[tone]),
        manager_dl_text_at(dl, dl->status),
        MANAGER_DL_STATUS_RIGHT - MANAGER_DL_STATUS_X);
}

static void manager_dl_paint_hint(const struct manager_display_list *dl,
                                  const struct manager_theme *theme,
                                  struct manager_canvas *canvas,
                                  const struct manager_dl_hint *hint, int x)
{
    const struct manager_face body = theme->face[MANAGER_DL_HINT_FACE(theme)];
    const uint32_t color = theme->palette.color[MANAGER_COLOR_BAR_TEXT];
    const char *label = manager_dl_text_at(dl, hint->text);
    if (hint->glyph != MANAGER_GLYPH_NONE) {
        x += theme->ops->draw_hint_glyph(theme, canvas, x, MANAGER_DL_HINT_Y,
                                         (enum manager_glyph)hint->glyph,
                                         color);
        if (label[0] != '\0')
            x += manager_face_measure(body, " ");
    }
    (void)manager_face_draw(body, canvas, x, MANAGER_DL_HINT_LABEL_Y(theme),
                            color, label);
}

static void manager_dl_paint_hints(const struct manager_display_list *dl,
                                   const struct manager_theme *theme,
                                   struct manager_canvas *canvas)
{
    unsigned int index;
    int x = MANAGER_DL_HINT_LEFT_X;
    for (index = 0u; index < dl->hint_count; ++index) {
        const struct manager_dl_hint *hint = &dl->hints[index];
        if (hint->right)
            continue;
        manager_dl_paint_hint(dl, theme, canvas, hint, x);
        x += manager_dl_hint_width(theme, dl, hint) + MANAGER_DL_HINT_GAP;
    }
    /* Right group: right-aligned at 928, the last item rightmost. */
    x = MANAGER_DL_HINT_RIGHT_X;
    for (index = dl->hint_count; index > 0u; --index) {
        const struct manager_dl_hint *hint = &dl->hints[index - 1u];
        int width;
        if (!hint->right)
            continue;
        width = manager_dl_hint_width(theme, dl, hint);
        manager_dl_paint_hint(dl, theme, canvas, hint, x - width);
        x -= width + MANAGER_DL_HINT_GAP;
    }
}

void manager_ui_draw(const struct manager_display_list *dl,
                     const struct manager_theme *theme,
                     struct manager_canvas *canvas)
{
    unsigned int index;
    manager_canvas_reset_clip(canvas);
    theme->ops->draw_background(theme, canvas);
    manager_dl_paint_chrome(dl, theme, canvas);
    manager_dl_paint_status(dl, theme, canvas);
    for (index = 0u; index < dl->count; ++index)
        manager_dl_paint_item(dl, theme, canvas, &dl->items[index]);
    manager_dl_paint_hints(dl, theme, canvas);
}

/* ---- dump ------------------------------------------------------------- */

struct manager_dl_writer {
    char *out;
    size_t capacity;
    size_t used;
};

static void manager_dl_put(struct manager_dl_writer *writer,
                           const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void manager_dl_put(struct manager_dl_writer *writer,
                           const char *format, ...)
{
    va_list arguments;
    int written;
    if (writer->capacity == 0u || writer->used >= writer->capacity - 1u)
        return;
    va_start(arguments, format);
    written = vsnprintf(writer->out + writer->used,
                        writer->capacity - writer->used, format, arguments);
    va_end(arguments);
    if (written < 0)
        return;
    if ((size_t)written >= writer->capacity - writer->used)
        writer->used = writer->capacity - 1u;
    else
        writer->used += (size_t)written;
}

size_t manager_dl_dump(const struct manager_display_list *dl, char *out,
                       size_t capacity)
{
    static const char *const kinds[] = {"text",  "glyph", "highlight",
                                        "panel", "sep",   "scroll",
                                        "scrim", "fill",  "frame"};
    static const char aligns[] = {'l', 'r', 'c'};
    struct manager_dl_writer writer;
    unsigned int index;
    writer.out = out;
    writer.capacity = capacity;
    writer.used = 0u;
    if (capacity != 0u)
        out[0] = '\0';
    manager_dl_put(&writer, "title %s\n", manager_dl_text_at(dl, dl->title));
    for (index = 0u; index < dl->tab_count; ++index)
        manager_dl_put(&writer, "tab%s %s\n",
                       (int)index == dl->active_tab ? "*" : "",
                       manager_dl_text_at(dl, dl->tabs[index]));
    manager_dl_put(&writer, "status %u %s\n", dl->status_tone,
                   manager_dl_text_at(dl, dl->status));
    for (index = 0u; index < dl->hint_count; ++index)
        manager_dl_put(&writer, "hint %s %s %s\n",
                       dl->hints[index].right ? "r" : "l",
                       manager_theme_glyph_text(
                           (enum manager_glyph)dl->hints[index].glyph),
                       manager_dl_text_at(dl, dl->hints[index].text));
    for (index = 0u; index < dl->count; ++index) {
        const struct manager_dl_item *item = &dl->items[index];
        const char *kind =
            item->kind < sizeof(kinds) / sizeof(kinds[0]) ? kinds[item->kind]
                                                          : "?";
        if (item->kind == MANAGER_DL_TEXT)
            manager_dl_put(&writer, "%s %d,%d f%u c%u %c %d %s\n", kind,
                           item->x, item->y, item->face, item->color,
                           item->align < 3u ? aligns[item->align] : '?',
                           item->x1, manager_dl_text_at(dl, item->text));
        else if (item->kind == MANAGER_DL_GLYPH)
            manager_dl_put(&writer, "%s %d,%d c%u %c %s\n", kind, item->x,
                           item->y, item->color,
                           item->align < 3u ? aligns[item->align] : '?',
                           manager_theme_glyph_text(
                               (enum manager_glyph)item->glyph));
        else
            manager_dl_put(&writer, "%s %d,%d,%d,%d c%u p%u %d %d\n", kind,
                           item->x, item->y, item->x1, item->y1, item->color,
                           item->panel, item->a, item->b);
    }
    if (dl->overflow)
        manager_dl_put(&writer, "OVERFLOW\n");
    return writer.used;
}

#endif /* MANAGER_UI2 */
