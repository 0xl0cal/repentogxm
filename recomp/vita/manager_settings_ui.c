/*
 * ISAAC SETTINGS page model (the FPS LOCK and ROOM RENDER rows over
 * port.ini).  See manager_settings_ui.h for
 * the contract.  No SDK, no allocator, no VLA; the only libc dependencies
 * are string.h and snprintf.
 */
#include "manager_settings_ui.h"

#if defined(MANAGER_SETTINGS_UI)

#include <stdio.h>
#include <string.h>

/* SQ PRESET joins the legend with the preset work package; a legend entry
 * for an inert key is not shown (test_livearea_package.py bans unfinished
 * labels). */
static const char k_legend[] =
    "UD ROW  LR VALUE  X SAVE  TRI DEFAULTS  O BACK";
static const char k_title[] = "ISAAC SETTINGS";

/* Status strings (narrow set, never ':'). */
static const char k_status_hint[] = "LR CHANGES VALUE  X SAVES PORT.INI";
static const char k_status_edits_refused[] = "PORT.INI IO ERROR - EDITS REFUSED";
static const char k_status_cancelled[] = "CANCELLED - NOTHING WRITTEN";
static const char k_status_discard_arm[] = "UNSAVED - O AGAIN TO DISCARD";
static const char k_status_discarded[] = "CHANGES DISCARDED";
/* HOME's idle status (manager_main.c); the caller mirrors the page status
 * into manager_status when the page is left, so a plain O shows READY and a
 * discarding O shows CHANGES DISCARDED on HOME. */
static const char k_status_ready[] = "READY";
static const char k_status_defaults_arm[] =
    "TRI AGAIN - ARCHIVE PORT.INI AND USE DEFAULTS";
static const char k_status_ignoring_suffix[] = "GAME BUILD IGNORES PORT.INI";

/* Row literals (wide set). */
static const char k_row_uneven[] = "  40 45 - UNEVEN CADENCE 30 UPS";
/* ROOM RENDER values (render_scale 1 / 2): the world raster the game draws
 * its rooms at; the game build writes it into options.ini at its next start. */
static const char k_scale_fast[] = "480X270 FAST";
static const char k_scale_sharp[] = "960X540 SHARP";

const char *manager_settings_ui_title(void)
{
    return k_title;
}

const char *manager_settings_ui_legend(void)
{
    return k_legend;
}

int manager_settings_ui_text_is_wide(const char *text)
{
    if (text == NULL)
        return 0;
    while (*text != '\0') {
        const unsigned char value = (unsigned char)*text++;
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == ' ' ||
              value == ':' || value == '.' || value == '-' || value == '_' ||
              value == '>' || value == '<' || value == '?'))
            return 0;
    }
    return 1;
}

/* ---- helpers ------------------------------------------------------------- */

static void ui_set_status(struct manager_settings_ui *ui, const char *text)
{
    (void)snprintf(ui->status, sizeof(ui->status), "%s", text);
}

static void ui_set_result_status(struct manager_settings_ui *ui,
                                 const struct manager_settings_result *result)
{
    manager_settings_result_text(result, ui->status, sizeof(ui->status));
}

static int ui_edits_refused(const struct manager_settings_ui *ui)
{
    return ui->state.status == MANAGER_SETTINGS_LOAD_IO_ERROR;
}

static int ui_fps_is_risky(uint16_t fps_lock)
{
    return fps_lock == 40u || fps_lock == 45u;
}

static const char *ui_scale_text(uint8_t render_scale)
{
    return render_scale == 2u ? k_scale_sharp : k_scale_fast;
}

/* The row below the selection while it exists: UNDO only with an archive. */
static void ui_clamp_selection(struct manager_settings_ui *ui)
{
    if (ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO &&
        !ui->state.undo_available)
        ui->selected = MANAGER_SETTINGS_UI_ROW_RENDER_SCALE;
    if (ui->selected >= MANAGER_SETTINGS_UI_ROW_COUNT)
        ui->selected = MANAGER_SETTINGS_UI_ROW_FPS_LOCK;
}

static void ui_add_row(struct manager_settings_ui *ui,
                       enum manager_settings_ui_row_style style,
                       const char *text)
{
    if (ui->row_count >= MANAGER_SETTINGS_UI_ROWS)
        return;
    (void)snprintf(ui->rows[ui->row_count], MANAGER_SETTINGS_UI_ROW_CAPACITY,
                   "%s", text);
    ui->row_style[ui->row_count] = (uint8_t)style;
    ++ui->row_count;
}

static enum manager_settings_ui_row_style ui_item_style(
    const struct manager_settings_ui *ui, enum manager_settings_ui_row row)
{
    return ui->selected == row ? MANAGER_SETTINGS_UI_STYLE_SELECTED
                               : MANAGER_SETTINGS_UI_STYLE_ITEM;
}

/* ---- rendering ----------------------------------------------------------- */

/* Row layout: 2-glyph marker column, 20-glyph name column, 18-glyph value
 * column, note.  Precisions bound every field so each row provably fits
 * MANAGER_SETTINGS_UI_ROW_MAX (the core's text helpers stay under 40
 * glyphs; ".54s" / ".65s" are the exact remainders after the prefixes). */
static void ui_render(struct manager_settings_ui *ui)
{
    char line[MANAGER_SETTINGS_UI_ROW_CAPACITY];
    char text[MANAGER_SETTINGS_TEXT_CAPACITY];
    char value[20]; /* "PORT-NNNN.INI" or the FPS value */
    const unsigned int pending = manager_settings_pending_count(&ui->state);
    const uint16_t fps_lock = ui->state.pending.fps_lock;
    const uint8_t render_scale = ui->state.pending.render_scale;
    const char *note;

    ui->row_count = 0u;
    memset(ui->rows, 0, sizeof(ui->rows));
    memset(ui->row_style, 0, sizeof(ui->row_style));
    ui_clamp_selection(ui);

    /* FPS LOCK: name, pending value, then how it relates to the file. */
    if (fps_lock != ui->state.saved.fps_lock)
        note = "PENDING";
    else if (fps_lock == 60u)
        note = "DEFAULT";
    else
        note = "SAVED";
    (void)snprintf(value, sizeof(value), "%u", (unsigned int)fps_lock);
    (void)snprintf(line, sizeof(line), "%c FPS LOCK            %-18.18s%.7s",
                   ui->selected == MANAGER_SETTINGS_UI_ROW_FPS_LOCK ? '>' : ' ',
                   value, note);
    ui_add_row(ui, ui_item_style(ui, MANAGER_SETTINGS_UI_ROW_FPS_LOCK), line);
    if (ui_fps_is_risky(fps_lock))
        ui_add_row(ui, MANAGER_SETTINGS_UI_STYLE_INFO, k_row_uneven);

    /* ROOM RENDER: the render_scale key (port default 1). */
    if (render_scale != ui->state.saved.render_scale)
        note = "PENDING";
    else if (render_scale == 1u)
        note = "DEFAULT";
    else
        note = "SAVED";
    (void)snprintf(line, sizeof(line), "%c ROOM RENDER         %-18.18s%.7s",
                   ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE ? '>' : ' ',
                   ui_scale_text(render_scale), note);
    ui_add_row(ui, ui_item_style(ui, MANAGER_SETTINGS_UI_ROW_RENDER_SCALE), line);

    if (ui->state.undo_available) {
        (void)snprintf(value, sizeof(value), "PORT-%04u.INI",
                       ui->state.undo_index);
        (void)snprintf(line, sizeof(line),
                       "%c UNDO                %-18.18sX RESTORE",
                       ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO ? '>' : ' ',
                       value);
        ui_add_row(ui, ui_item_style(ui, MANAGER_SETTINGS_UI_ROW_UNDO), line);
    }

    manager_settings_port_ini_text(&ui->state, text, sizeof(text));
    (void)snprintf(line, sizeof(line), "  %.65s", text);
    ui_add_row(ui, MANAGER_SETTINGS_UI_STYLE_INFO, line);

    manager_settings_last_boot_text(&ui->last_boot, text, sizeof(text));
    (void)snprintf(line, sizeof(line), "  LAST BOOT: %.54s", text);
    ui_add_row(ui, MANAGER_SETTINGS_UI_STYLE_INFO, line);

    if (pending != 0u) {
        (void)snprintf(line, sizeof(line), "  PENDING: %u ROW%s NOT SAVED",
                       pending, pending == 1u ? "" : "S");
        ui_add_row(ui, MANAGER_SETTINGS_UI_STYLE_INFO, line);
    }
}

/* ---- events -------------------------------------------------------------- */

void manager_settings_ui_open(struct manager_settings_ui *ui,
                              const struct manager_storage_io *io)
{
    if (ui == NULL)
        return;
    memset(ui, 0, sizeof(*ui));
    (void)manager_settings_load(io, &ui->state);
    (void)manager_settings_read_last_boot(io, NULL, &ui->last_boot);
    ui->selected = MANAGER_SETTINGS_UI_ROW_FPS_LOCK;
    ui->armed = MANAGER_SETTINGS_UI_ARM_NONE;
    ui_set_status(ui, ui_edits_refused(ui) ? k_status_edits_refused
                                           : k_status_hint);
    ui_render(ui);
}

static void ui_move(struct manager_settings_ui *ui, int direction)
{
    if (direction < 0) {
        if (ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO)
            ui->selected = MANAGER_SETTINGS_UI_ROW_RENDER_SCALE;
        else
            ui->selected = MANAGER_SETTINGS_UI_ROW_FPS_LOCK;
    } else if (ui->selected == MANAGER_SETTINGS_UI_ROW_FPS_LOCK) {
        ui->selected = MANAGER_SETTINGS_UI_ROW_RENDER_SCALE;
    } else if (ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE &&
               ui->state.undo_available) {
        ui->selected = MANAGER_SETTINGS_UI_ROW_UNDO;
    }
}

static void ui_step_value(struct manager_settings_ui *ui, int direction)
{
    char text[MANAGER_SETTINGS_TEXT_CAPACITY];
    uint16_t next;
    uint8_t next_scale;

    if (ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO) {
        (void)snprintf(text, sizeof(text), "X RESTORES PORT-%04u.INI",
                       ui->state.undo_index);
        ui_set_status(ui, text);
        return;
    }
    if (ui_edits_refused(ui)) {
        ui_set_status(ui, k_status_edits_refused);
        return;
    }
    if (ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE) {
        next_scale = manager_settings_render_scale_step(
            ui->state.pending.render_scale, direction);
        (void)manager_settings_set_render_scale(&ui->state, next_scale);
        (void)snprintf(text, sizeof(text), "ROOM RENDER %s - X SAVES",
                       ui_scale_text(next_scale));
        ui_set_status(ui, text);
        return;
    }
    next = manager_settings_fps_lock_step(ui->state.pending.fps_lock, direction);
    (void)manager_settings_set_fps_lock(&ui->state, next);
    if (ui_fps_is_risky(next))
        (void)snprintf(text, sizeof(text),
                       "FPS LOCK %u - UNEVEN CADENCE - X TWICE",
                       (unsigned int)next);
    else
        (void)snprintf(text, sizeof(text), "FPS LOCK %u - X SAVES",
                       (unsigned int)next);
    ui_set_status(ui, text);
}

static void ui_cross(struct manager_settings_ui *ui,
                     const struct manager_storage_io *io, int ftp_listening,
                     enum manager_settings_ui_arm previous)
{
    struct manager_settings_result result;
    char text[MANAGER_SETTINGS_TEXT_CAPACITY];

    if (ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO) {
        /* The row is hidden after a failed read (refresh_undo returns
         * early), so this refusal is only a belt for the same rule. */
        if (!ftp_listening && ui_edits_refused(ui)) {
            ui_set_status(ui, k_status_edits_refused);
            return;
        }
        result = manager_settings_undo(io, &ui->state, ftp_listening);
        ui_set_result_status(ui, &result);
        return;
    }
    if (ftp_listening) {
        /* Refused before storage is touched; the core owns the text. */
        result = manager_settings_save(io, &ui->state, 1);
        ui_set_result_status(ui, &result);
        return;
    }
    if (ui_edits_refused(ui)) {
        ui_set_status(ui, k_status_edits_refused);
        return;
    }
    if (ui_fps_is_risky(ui->state.pending.fps_lock) &&
        previous != MANAGER_SETTINGS_UI_ARM_SAVE) {
        ui->armed = MANAGER_SETTINGS_UI_ARM_SAVE;
        (void)snprintf(text, sizeof(text), "UNEVEN CADENCE - X AGAIN TO SAVE %u",
                       (unsigned int)ui->state.pending.fps_lock);
        ui_set_status(ui, text);
        return;
    }
    result = manager_settings_save(io, &ui->state, 0);
    ui_set_result_status(ui, &result);
    if (manager_settings_result_ok(&result) &&
        ui->last_boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES) {
        /* SAVE is allowed; the status warns. */
        const size_t used = strlen(ui->status);
        if (used < sizeof(ui->status))
            (void)snprintf(ui->status + used, sizeof(ui->status) - used,
                           " - %s", k_status_ignoring_suffix);
    }
}

static void ui_triangle(struct manager_settings_ui *ui,
                        const struct manager_storage_io *io, int ftp_listening,
                        enum manager_settings_ui_arm previous)
{
    struct manager_settings_result result;

    if (ftp_listening) {
        result = manager_settings_defaults(io, &ui->state, 1);
        ui_set_result_status(ui, &result);
        return;
    }
    if (ui_edits_refused(ui)) {
        /* Section 9: after a failed read every write is refused, DEFAULTS
         * included; the page shows values it never read. */
        ui_set_status(ui, k_status_edits_refused);
        return;
    }
    if (previous != MANAGER_SETTINGS_UI_ARM_DEFAULTS) {
        ui->armed = MANAGER_SETTINGS_UI_ARM_DEFAULTS;
        ui_set_status(ui, k_status_defaults_arm);
        return;
    }
    result = manager_settings_defaults(io, &ui->state, 0);
    ui_set_result_status(ui, &result);
}

int manager_settings_ui_handle(struct manager_settings_ui *ui,
                               const struct manager_storage_io *io,
                               uint32_t pressed, int ftp_listening)
{
    enum manager_settings_ui_arm previous;
    int leave = 0;
    int status_set = 1;

    if (ui == NULL || pressed == 0u)
        return 0;
    previous = ui->armed;
    ui->armed = MANAGER_SETTINGS_UI_ARM_NONE;
    if ((pressed & MANAGER_SETTINGS_UI_BUTTON_CIRCLE) != 0u) {
        if (previous == MANAGER_SETTINGS_UI_ARM_DISCARD) {
            manager_settings_discard(&ui->state);
            ui_set_status(ui, k_status_discarded);
            leave = 1;
        } else if (manager_settings_pending_count(&ui->state) != 0u) {
            ui->armed = MANAGER_SETTINGS_UI_ARM_DISCARD;
            ui_set_status(ui, k_status_discard_arm);
        } else {
            ui_set_status(ui, k_status_ready);
            leave = 1;
        }
    } else if ((pressed & MANAGER_SETTINGS_UI_BUTTON_UP) != 0u) {
        ui_move(ui, -1);
        status_set = 0;
    } else if ((pressed & MANAGER_SETTINGS_UI_BUTTON_DOWN) != 0u) {
        ui_move(ui, 1);
        status_set = 0;
    } else if ((pressed & (MANAGER_SETTINGS_UI_BUTTON_LEFT |
                           MANAGER_SETTINGS_UI_BUTTON_RIGHT)) != 0u) {
        ui_step_value(ui, (pressed & MANAGER_SETTINGS_UI_BUTTON_RIGHT) != 0u
                              ? 1 : -1);
    } else if ((pressed & MANAGER_SETTINGS_UI_BUTTON_CROSS) != 0u) {
        ui_cross(ui, io, ftp_listening, previous);
    } else if ((pressed & MANAGER_SETTINGS_UI_BUTTON_TRIANGLE) != 0u) {
        ui_triangle(ui, io, ftp_listening, previous);
    } else {
        status_set = 0;
    }
    if (!status_set && previous != MANAGER_SETTINGS_UI_ARM_NONE)
        ui_set_status(ui, k_status_cancelled);
    ui_render(ui);
    return leave;
}

#endif /* MANAGER_SETTINGS_UI */
