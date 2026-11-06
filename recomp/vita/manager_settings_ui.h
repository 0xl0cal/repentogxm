/*
 * ISAAC SETTINGS page model: the FPS LOCK row, SAVE / DEFAULTS / UNDO
 * over port.ini, and (added 2026-09-16) the ROOM RENDER row for the
 * render_scale key.
 *
 * SDK-free by construction: the model is driven by a button bitmask, calls
 * manager_settings.* through the manager_storage_io vtable it is handed and
 * emits text rows, a legend and a status line; manager_main.c draws them
 * under #if defined(MANAGER_SETTINGS_UI).  Compiled into the manager with
 * MANAGER_SETTINGS_UI=1 and into host_tests/manager_settings_ui_host_test;
 * declares nothing without the knob.
 *
 * Two text filters exist in the manager: rows and the legend go
 * through manager_draw_text and may use A-Z 0-9 space : . - _ > < ? (the
 * "wide" set); the status line goes through manager_display_text, which
 * admits only A-Z 0-9 space . - _ and folds everything else to '?', so every
 * status string here is "narrow" and never carries ':'.  Rows are at most
 * MANAGER_SETTINGS_UI_ROW_MAX glyphs (12 px each at scale 2 from x=88 to the
 * panel edge at x=896); status strings stay under the 58 glyphs the manager
 * shows.  The host test asserts both sets and both limits on every string
 * the model emits in every state it reaches.
 */
#ifndef ISAAC_MANAGER_SETTINGS_UI_H
#define ISAAC_MANAGER_SETTINGS_UI_H

#if defined(MANAGER_SETTINGS_UI)

#include "manager_settings.h"

#include <stddef.h>
#include <stdint.h>

#define MANAGER_SETTINGS_UI_ROWS 12u
#define MANAGER_SETTINGS_UI_ROW_MAX 67u
#define MANAGER_SETTINGS_UI_ROW_CAPACITY (MANAGER_SETTINGS_UI_ROW_MAX + 1u)
#define MANAGER_SETTINGS_UI_STATUS_MAX 57u
#define MANAGER_SETTINGS_UI_STATUS_CAPACITY MANAGER_SETTINGS_TEXT_CAPACITY

/* Button bits: the SCE_CTRL_* values of psp2/ctrl.h copied so this unit has
 * no SDK dependence.  manager_main.c static-asserts the equality and passes
 * its pressed-edge mask through unchanged. */
enum manager_settings_ui_button {
    MANAGER_SETTINGS_UI_BUTTON_SELECT = 0x00000001,
    MANAGER_SETTINGS_UI_BUTTON_START = 0x00000008,
    MANAGER_SETTINGS_UI_BUTTON_UP = 0x00000010,
    MANAGER_SETTINGS_UI_BUTTON_RIGHT = 0x00000020,
    MANAGER_SETTINGS_UI_BUTTON_DOWN = 0x00000040,
    MANAGER_SETTINGS_UI_BUTTON_LEFT = 0x00000080,
    MANAGER_SETTINGS_UI_BUTTON_LTRIGGER = 0x00000100,
    MANAGER_SETTINGS_UI_BUTTON_RTRIGGER = 0x00000200,
    MANAGER_SETTINGS_UI_BUTTON_TRIANGLE = 0x00001000,
    MANAGER_SETTINGS_UI_BUTTON_CIRCLE = 0x00002000,
    MANAGER_SETTINGS_UI_BUTTON_CROSS = 0x00004000,
    MANAGER_SETTINGS_UI_BUTTON_SQUARE = 0x00008000
};

/* Two-press arming.  Exactly one press may be armed; the next press either
 * completes it (the same key) or disarms it (any other key). */
enum manager_settings_ui_arm {
    MANAGER_SETTINGS_UI_ARM_NONE = 0,
    MANAGER_SETTINGS_UI_ARM_SAVE,     /* X once while 40 or 45 is pending */
    MANAGER_SETTINGS_UI_ARM_DEFAULTS, /* TRIANGLE once */
    MANAGER_SETTINGS_UI_ARM_DISCARD   /* O once while edits are pending */
};

/* Selectable rows (UP/DOWN).  UNDO exists only while an archive newer than
 * the live file exists (manager_settings_state.undo_available). */
enum manager_settings_ui_row {
    MANAGER_SETTINGS_UI_ROW_FPS_LOCK = 0,
    MANAGER_SETTINGS_UI_ROW_RENDER_SCALE, /* ROOM RENDER: 480X270 FAST | 960X540 SHARP */
    MANAGER_SETTINGS_UI_ROW_UNDO,
    MANAGER_SETTINGS_UI_ROW_COUNT
};

/* Colour class of an emitted row. */
enum manager_settings_ui_row_style {
    MANAGER_SETTINGS_UI_STYLE_INFO = 0, /* status rows: muted */
    MANAGER_SETTINGS_UI_STYLE_ITEM,     /* selectable, not selected: white */
    MANAGER_SETTINGS_UI_STYLE_SELECTED  /* the '>' row: selected colour */
};

struct manager_settings_ui {
    struct manager_settings_state state;
    struct manager_settings_last_boot last_boot;
    enum manager_settings_ui_row selected;
    enum manager_settings_ui_arm armed;
    /* Narrow set, never ':', at most MANAGER_SETTINGS_UI_STATUS_MAX glyphs;
     * manager_main.c copies it into manager_status after every event. */
    char status[MANAGER_SETTINGS_UI_STATUS_CAPACITY];
    unsigned int row_count;
    char rows[MANAGER_SETTINGS_UI_ROWS][MANAGER_SETTINGS_UI_ROW_CAPACITY];
    uint8_t row_style[MANAGER_SETTINGS_UI_ROWS]; /* enum ..._row_style */
};

/* Page title and legend (wide set, at most MANAGER_SETTINGS_UI_ROW_MAX). */
const char *manager_settings_ui_title(void);
const char *manager_settings_ui_legend(void);

/* Load port.ini and the per-boot log head, select FPS LOCK, disarm, render.
 * A NULL io yields the IO ERROR state (edits refused). */
void manager_settings_ui_open(struct manager_settings_ui *ui,
                              const struct manager_storage_io *io);

/* One pressed-edge mask (manager_main.c: pad.buttons & ~previous).  Zero
 * changes nothing.  UP/DOWN move the selection one row (FPS LOCK, ROOM
 * RENDER, UNDO while it exists); LEFT/RIGHT step the selected row's pending
 * value (FPS LOCK 60/45/40/30; ROOM RENDER 480X270 FAST / 960X540 SHARP,
 * the render_scale key the game build writes into options.ini at its next
 * start); X saves (two presses while 40/45 is pending), or
 * restores the newest archive on the UNDO row; TRIANGLE twice archives
 * port.ini and removes it (DEFAULTS); O leaves, or with pending edits asks
 * "UNSAVED - O AGAIN TO DISCARD" first.  Every write is refused while
 * ftp_listening != 0 ("STOP FTP BEFORE LOCAL CHANGES") and while the load
 * failed with an I/O error (SAVE, DEFAULTS and UNDO alike: "PORT.INI IO
 * ERROR - EDITS REFUSED").  Any key other than the completing one disarms
 * a two-press action.  Re-renders; returns 1 when the caller should return
 * to HOME, 0 otherwise.  On return 1 the status holds the text HOME should
 * show ("READY", or "CHANGES DISCARDED" after a discarding O). */
int manager_settings_ui_handle(struct manager_settings_ui *ui,
                               const struct manager_storage_io *io,
                               uint32_t pressed, int ftp_listening);

/* 1 when text uses only A-Z 0-9 space : . - _ > < ? (manager_glyph_row). */
int manager_settings_ui_text_is_wide(const char *text);

#endif /* MANAGER_SETTINGS_UI */

#endif
