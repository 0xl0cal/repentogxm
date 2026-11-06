/*
 * Host test for the ISAAC SETTINGS page model (manager_settings_ui.c) over
 * the manager settings core and the shared fake filesystem of fake_fs.h
 * (nodes enlarged so the 16 KiB log head fits).
 *
 * Scripted press sequences (the FPS LOCK row, SAVE / DEFAULTS / UNDO and
 * the ROOM RENDER row) with row, legend and status
 * goldens:
 *   - open on an absent file: rows, legend, title, hint status; a zero mask
 *     and an unmapped key change nothing; UP/DOWN walk FPS LOCK / ROOM
 *     RENDER and cannot reach a hidden UNDO; O leaves at once;
 *   - LEFT/RIGHT step 60/45/40/30 with clamping and the PENDING note;
 *   - ROOM RENDER: LEFT/RIGHT step 480X270 FAST / 960X540 SHARP with
 *     clamping, X writes render_scale=2 after display_raster, discard and
 *     DEFAULTS/UNDO restore it, both rows pending at once save together;
 *   - X SAVE: two-press arming while 40/45 is pending, disarm by another
 *     key, canonical bytes written, previous version archived, UNDO row
 *     appears only when an archive differs from the file, UNDO twice = redo;
 *   - O with pending edits arms the discard, a second O discards and leaves;
 *     another key in between disarms and acts;
 *   - TRIANGLE DEFAULTS two-press: archive + remove, NO PORT.INI the second
 *     time, UNDO brings the file back and then hides itself;
 *   - every write refused while FTP listens ("STOP FTP BEFORE LOCAL
 *     CHANGES"), nothing armed, nothing written;
 *   - LAST BOOT row from the log head: receipt values, GAME BUILD IGNORES
 *     PORT.INI (and the SAVE OK suffix), NO LOG;
 *   - PORT.INI row for ok / ignored lines / invalid (SAVE rewrites; the
 *     unparseable archive is offered as UNDO and refused on the press) / io
 *     error (edits, SAVE and DEFAULTS refused, nothing archived); a failing
 *     write keeps the pending edit; a failing exclusive open is an IO ERROR;
 *   - leaving with O hands READY (or CHANGES DISCARDED) to HOME;
 *   - NULL model / NULL io never fault;
 *   - after every open and every press: each emitted row is non-empty, at
 *     most 67 glyphs and in the wide set (A-Z 0-9 space : . - _ > < ?); the
 *     legend and title likewise; the status is non-empty, at most 57 glyphs,
 *     in the narrow set (A-Z 0-9 space . - _) and never carries ':'; exactly
 *     one row is selected; UNDO is selected only while it exists.
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"

#include "manager_settings_ui.h"

#include <stdio.h>
#include <string.h>

#define PORT_INI MANAGER_STORAGE_PORT_INI_PATH
#define PORT_TMP MANAGER_STORAGE_ROOT "/port.ini.tmp"
#define PORT_ROLLBACK MANAGER_STORAGE_ROOT "/port.ini.rollback"
#define ARCHIVE(index) MANAGER_STORAGE_BACKUP_ROOT "/port-" index ".ini"
#define LOG_PATH MANAGER_SETTINGS_LOG_PATH

#define B_UP MANAGER_SETTINGS_UI_BUTTON_UP
#define B_DOWN MANAGER_SETTINGS_UI_BUTTON_DOWN
#define B_LEFT MANAGER_SETTINGS_UI_BUTTON_LEFT
#define B_RIGHT MANAGER_SETTINGS_UI_BUTTON_RIGHT
#define B_CROSS MANAGER_SETTINGS_UI_BUTTON_CROSS
#define B_CIRCLE MANAGER_SETTINGS_UI_BUTTON_CIRCLE
#define B_TRIANGLE MANAGER_SETTINGS_UI_BUTTON_TRIANGLE
#define B_SQUARE MANAGER_SETTINGS_UI_BUTTON_SQUARE

/* Canonical serialization (table order of host_vita_settings_parse.h). */
#define CANON_REST                                                             \
    "button.cross=a\n"                                                         \
    "button.circle=b\n"                                                        \
    "button.square=x\n"                                                        \
    "button.triangle=y\n"                                                      \
    "button.l=lb\n"                                                            \
    "button.r=lt\n"                                                            \
    "button.select=rt\n"                                                       \
    "chord.start_l=rb\n"                                                       \
    "chord.start_r=back\n"                                                     \
    "chord.restart_hold=on\n"                                                  \
    "touch.top_left=lt\n"                                                      \
    "touch.top_right=back\n"                                                   \
    "touch.bottom_right=rb\n"                                                  \
    "stick.swap=off\n"
#define CANON_TAIL "display_raster=960\n" "render_scale=1\n" CANON_REST
#define CANON_60 "schema=1\nfps_lock=60\n" CANON_TAIL
#define CANON_45 "schema=1\nfps_lock=45\n" CANON_TAIL
#define CANON_30 "schema=1\nfps_lock=30\n" CANON_TAIL
#define CANON_SHARP_60 \
    "schema=1\nfps_lock=60\ndisplay_raster=960\nrender_scale=2\n" CANON_REST

#define BUILD_LINE                                                             \
    "[0.123 thr 0x40010003] build 2026-09-10-abc; guest base=0x00400000; "    \
    "softfp ARMv7\n"
#define RECEIPT_LINE_45                                                        \
    "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "                            \
    "path=ux0:/data/isaacr001/port.ini status=ok bytes=32 lines=2 "          \
    "rejected=1 unknown=0 dup=0 fps_lock=45 "                                  \
    "raster=960x544(ignored:not-compiled) remap=default(guard) "              \
    "chords=rb/back restart_hold=1 stick_swap=0\n"
#define RECEIPT_LINE_CUSTOM                                                    \
    "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "                            \
    "path=ux0:/data/isaacr001/port.ini status=absent bytes=0 lines=0 "       \
    "rejected=0 unknown=0 dup=0 fps_lock=60(ignored:no-fullspeed-scheduler) " \
    "raster=960x544(ignored:not-compiled) remap=custom "                      \
    "chords=rb/back restart_hold=1 stick_swap=0\n"
#define RECEIPT_LINE_SCHEMA                                                    \
    "[0.201 thr 0x40010003] ISAAC VITA SETTINGS: "                            \
    "path=ux0:/data/isaacr001/port.ini status=schema bytes=12 lines=1 "      \
    "rejected=0 unknown=0 dup=0 fps_lock=60 "                                  \
    "raster=960x544(ignored:not-compiled) remap=default "                     \
    "chords=rb/back restart_hold=1 stick_swap=0\n"

/* Row goldens. */
#define ROW_FPS_60_DEFAULT_SEL "> FPS LOCK            60                DEFAULT"
#define ROW_FPS_60_DEFAULT "  FPS LOCK            60                DEFAULT"
#define ROW_FPS_45_PENDING_SEL "> FPS LOCK            45                PENDING"
#define ROW_FPS_40_PENDING_SEL "> FPS LOCK            40                PENDING"
#define ROW_FPS_30_PENDING_SEL "> FPS LOCK            30                PENDING"
#define ROW_FPS_60_PENDING_SEL "> FPS LOCK            60                PENDING"
#define ROW_FPS_45_SAVED_SEL "> FPS LOCK            45                SAVED"
#define ROW_FPS_30_SAVED_SEL "> FPS LOCK            30                SAVED"
#define ROW_FPS_45_SAVED "  FPS LOCK            45                SAVED"
#define ROW_FPS_60_PENDING "  FPS LOCK            60                PENDING"
#define ROW_UNEVEN "  40 45 - UNEVEN CADENCE 30 UPS"
#define ROW_SCALE_FAST_DEFAULT "  ROOM RENDER         480X270 FAST      DEFAULT"
#define ROW_SCALE_FAST_DEFAULT_SEL "> ROOM RENDER         480X270 FAST      DEFAULT"
#define ROW_SCALE_FAST_PENDING_SEL "> ROOM RENDER         480X270 FAST      PENDING"
#define ROW_SCALE_SHARP_PENDING_SEL "> ROOM RENDER         960X540 SHARP     PENDING"
#define ROW_SCALE_SHARP_SAVED_SEL "> ROOM RENDER         960X540 SHARP     SAVED"
#define ROW_SCALE_SHARP_SAVED "  ROOM RENDER         960X540 SHARP     SAVED"
#define ROW_UNDO(index) "  UNDO                PORT-" index ".INI     X RESTORE"
#define ROW_UNDO_SEL(index) "> UNDO                PORT-" index ".INI     X RESTORE"
#define ROW_ABSENT "  PORT.INI ABSENT"
#define ROW_OK "  PORT.INI OK"
#define ROW_NO_LOG "  LAST BOOT: NO LOG"
#define ROW_PENDING_1 "  PENDING: 1 ROW NOT SAVED"
#define ROW_PENDING_2 "  PENDING: 2 ROWS NOT SAVED"
#define LEGEND "UD ROW  LR VALUE  X SAVE  TRI DEFAULTS  O BACK"

static struct fake_fs g_fs;
static struct manager_settings_ui g_ui;

static struct manager_storage_io ui_fs_init(struct fake_fs *fs, int backup_root)
{
    fake_init(fs);
    if (backup_root)
        fake_add_dir(fs, MANAGER_STORAGE_BACKUP_ROOT);
    return fake_io(fs);
}

static int fake_exists(const struct fake_fs *fs, const char *path)
{
    return fake_find(fs, path) >= 0;
}

static void require_no_inflight(const struct fake_fs *fs, const char *message)
{
    require(!fake_exists(fs, PORT_TMP), message);
    require(!fake_exists(fs, PORT_ROLLBACK), message);
}

static int fake_count_containing_archives(const struct fake_fs *fs)
{
    size_t index;
    int count = 0;
    for (index = 0u; index < fs->node_count; ++index) {
        if (strstr(fs->nodes[index].path,
                   MANAGER_STORAGE_BACKUP_ROOT "/port-") != NULL)
            ++count;
    }
    return count;
}

/* ---- invariants checked after every event ------------------------------- */

static void require_wide(const char *text, size_t max, const char *message)
{
    require(text != NULL && text[0] != '\0', message);
    require(strlen(text) <= max, message);
    require(manager_settings_ui_text_is_wide(text), message);
}

static void require_narrow(const char *text, const char *message)
{
    require(text != NULL && text[0] != '\0', message);
    require(strlen(text) <= MANAGER_SETTINGS_UI_STATUS_MAX, message);
    require(manager_settings_text_is_narrow(text), message);
    require(strchr(text, ':') == NULL, message);
}

static void check_invariants(const struct manager_settings_ui *ui)
{
    unsigned int row;
    unsigned int selected_rows = 0u;

    require(ui->row_count >= 1u && ui->row_count <= MANAGER_SETTINGS_UI_ROWS,
            "row count within the table");
    for (row = 0u; row < MANAGER_SETTINGS_UI_ROWS; ++row) {
        if (row < ui->row_count) {
            require_wide(ui->rows[row], MANAGER_SETTINGS_UI_ROW_MAX,
                         "emitted row glyph set and width");
            require(ui->row_style[row] <=
                        (uint8_t)MANAGER_SETTINGS_UI_STYLE_SELECTED,
                    "row style in range");
            if (ui->row_style[row] == (uint8_t)MANAGER_SETTINGS_UI_STYLE_SELECTED) {
                ++selected_rows;
                require(ui->rows[row][0] == '>', "selected row carries the marker");
            } else {
                require(ui->rows[row][0] == ' ', "unselected row has no marker");
            }
        } else {
            require(ui->rows[row][0] == '\0', "rows past the count are empty");
            require(ui->row_style[row] == 0u, "styles past the count are cleared");
        }
    }
    require(selected_rows == 1u, "exactly one selected row");
    require_wide(manager_settings_ui_legend(), MANAGER_SETTINGS_UI_ROW_MAX,
                 "legend glyph set and width");
    require_wide(manager_settings_ui_title(), 20u, "title glyph set");
    require_narrow(ui->status, "status glyph set, width and no colon");
    require(ui->selected < MANAGER_SETTINGS_UI_ROW_COUNT, "selection in range");
    require(ui->selected != MANAGER_SETTINGS_UI_ROW_UNDO || ui->state.undo_available,
            "UNDO selected only while it exists");
    require(ui->armed <= MANAGER_SETTINGS_UI_ARM_DISCARD, "arm in range");
}

static void ui_open(struct manager_settings_ui *ui,
                    const struct manager_storage_io *io)
{
    manager_settings_ui_open(ui, io);
    check_invariants(ui);
}

static int press(struct manager_settings_ui *ui,
                 const struct manager_storage_io *io, uint32_t buttons,
                 int ftp_listening)
{
    const int leave = manager_settings_ui_handle(ui, io, buttons, ftp_listening);
    check_invariants(ui);
    return leave;
}

static void require_row(const struct manager_settings_ui *ui, unsigned int index,
                        const char *text, const char *message)
{
    require(index < ui->row_count, message);
    if (strcmp(ui->rows[index], text) != 0) {
        fprintf(stderr, "row %u: got %s\n        expected %s\n", index,
                ui->rows[index], text);
        require(0, message);
    }
}

static void require_rows(const struct manager_settings_ui *ui,
                         const char *const *rows, unsigned int count,
                         const char *message)
{
    unsigned int index;
    if (ui->row_count != count) {
        fprintf(stderr, "row count %u, expected %u\n", ui->row_count, count);
        for (index = 0u; index < ui->row_count; ++index)
            fprintf(stderr, "  [%u] %s\n", index, ui->rows[index]);
        require(0, message);
    }
    for (index = 0u; index < count; ++index)
        require_row(ui, index, rows[index], message);
}

static void require_status(const struct manager_settings_ui *ui,
                           const char *text, const char *message)
{
    if (strcmp(ui->status, text) != 0) {
        fprintf(stderr, "status: got %s\n        expected %s\n", ui->status, text);
        require(0, message);
    }
}

static int has_row_containing(const struct manager_settings_ui *ui,
                              const char *needle)
{
    unsigned int index;
    for (index = 0u; index < ui->row_count; ++index) {
        if (strstr(ui->rows[index], needle) != NULL)
            return 1;
    }
    return 0;
}

/* ---- cases --------------------------------------------------------------- */

static void test_open_absent(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;
    static const char *const rows[] = {
        ROW_FPS_60_DEFAULT_SEL, ROW_SCALE_FAST_DEFAULT, ROW_ABSENT, ROW_NO_LOG,
    };

    ui_open(ui, &io);
    require_rows(ui, rows, 4u, "absent file rows");
    require(ui->row_style[0] == MANAGER_SETTINGS_UI_STYLE_SELECTED &&
                ui->row_style[1] == MANAGER_SETTINGS_UI_STYLE_ITEM &&
                ui->row_style[2] == MANAGER_SETTINGS_UI_STYLE_INFO &&
                ui->row_style[3] == MANAGER_SETTINGS_UI_STYLE_INFO,
            "absent file row styles");
    require_status(ui, "LR CHANGES VALUE  X SAVES PORT.INI", "open hint");
    require(strcmp(manager_settings_ui_legend(), LEGEND) == 0, "legend text");
    require(strcmp(manager_settings_ui_title(), "ISAAC SETTINGS") == 0, "title");
    require(strstr(LEGEND, "PRESET") == NULL, "no inert legend entry in WP1");
    require(ui->state.status == MANAGER_SETTINGS_LOAD_ABSENT, "load absent");
    require(!ui->state.undo_available, "no undo without archives");

    require(press(ui, &io, 0u, 0) == 0, "zero mask stays");
    require_rows(ui, rows, 4u, "zero mask keeps rows");
    require_status(ui, "LR CHANGES VALUE  X SAVES PORT.INI", "zero mask keeps status");
    require(press(ui, &io, B_SQUARE, 0) == 0, "unmapped key stays");
    require_status(ui, "LR CHANGES VALUE  X SAVES PORT.INI", "unmapped key keeps status");
    require(press(ui, &io, B_UP, 0) == 0, "UP at the top stays");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_FPS_LOCK, "UP clamps at FPS LOCK");
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN to ROOM RENDER");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE, "ROOM RENDER selected");
    require_row(ui, 0u, ROW_FPS_60_DEFAULT, "FPS LOCK unselected");
    require_row(ui, 1u, ROW_SCALE_FAST_DEFAULT_SEL, "ROOM RENDER selected row");
    require(ui->row_style[0] == MANAGER_SETTINGS_UI_STYLE_ITEM &&
                ui->row_style[1] == MANAGER_SETTINGS_UI_STYLE_SELECTED,
            "selection styles moved down");
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN without UNDO stays");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE,
            "DOWN cannot reach a hidden UNDO row");
    require(press(ui, &io, B_UP, 0) == 0, "UP back to FPS LOCK");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_FPS_LOCK, "FPS LOCK selected again");
    require_status(ui, "LR CHANGES VALUE  X SAVES PORT.INI", "navigation keeps the status");
    require_rows(ui, rows, 4u, "navigation keeps rows");
    require(press(ui, &io, B_CIRCLE, 0) == 1, "O leaves without pending edits");
    require_status(ui, "READY", "plain O hands READY to HOME");
    require(!fake_exists(fs, PORT_INI), "viewing writes nothing");
}

static void test_edit_save_undo(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;
    static const char *const pending_45[] = {
        ROW_FPS_45_PENDING_SEL, ROW_UNEVEN, ROW_SCALE_FAST_DEFAULT, ROW_ABSENT,
        ROW_NO_LOG, ROW_PENDING_1,
    };
    static const char *const saved_45[] = {
        ROW_FPS_45_SAVED_SEL, ROW_UNEVEN, ROW_SCALE_FAST_DEFAULT, ROW_OK, ROW_NO_LOG,
    };
    static const char *const saved_30_undo[] = {
        ROW_FPS_30_SAVED_SEL, ROW_SCALE_FAST_DEFAULT, ROW_UNDO("0000"), ROW_OK,
        ROW_NO_LOG,
    };
    static const char *const pending_60_undo_sel[] = {
        ROW_FPS_60_PENDING, ROW_SCALE_FAST_DEFAULT, ROW_UNDO_SEL("0000"), ROW_OK,
        ROW_NO_LOG, ROW_PENDING_1,
    };
    static const char *const undone_45[] = {
        ROW_FPS_45_SAVED, ROW_UNEVEN, ROW_SCALE_FAST_DEFAULT, ROW_UNDO_SEL("0001"),
        ROW_OK, ROW_NO_LOG,
    };

    ui_open(ui, &io);
    /* RIGHT: 60 -> 45 (risky). */
    require(press(ui, &io, B_RIGHT, 0) == 0, "RIGHT stays on the page");
    require_rows(ui, pending_45, 6u, "45 pending rows");
    require_status(ui, "FPS LOCK 45 - UNEVEN CADENCE - X TWICE", "45 status");
    require(ui->state.pending.fps_lock == 45u && ui->state.saved.fps_lock == 60u,
            "pending edited, saved untouched");
    /* X arms; another key disarms; X X saves. */
    require(press(ui, &io, B_CROSS, 0) == 0, "X arms");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_SAVE, "armed for save");
    require_status(ui, "UNEVEN CADENCE - X AGAIN TO SAVE 45", "arm status");
    require(!fake_exists(fs, PORT_INI), "arming writes nothing");
    require(press(ui, &io, B_UP, 0) == 0, "UP disarms");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "disarmed");
    require_status(ui, "CANCELLED - NOTHING WRITTEN", "disarm status");
    require(!fake_exists(fs, PORT_INI), "disarm writes nothing");
    require(press(ui, &io, B_CROSS, 0) == 0, "X arms again");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_SAVE, "armed again");
    require(press(ui, &io, B_CROSS, 0) == 0, "second X saves");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "save disarms");
    require_status(ui, "SAVE OK", "save status");
    require_file(fs, PORT_INI, CANON_45, "canonical 45 written");
    require_no_inflight(fs, "no sidecar after save");
    require(!fake_exists(fs, ARCHIVE("0000")), "nothing to archive on first save");
    require_rows(ui, saved_45, 5u, "saved 45 rows");
    require(!has_row_containing(ui, "UNDO"), "no UNDO row without an archive");
    require(ui->state.pending.fps_lock == 45u && ui->state.saved.fps_lock == 45u,
            "state reloaded after save");

    /* RIGHT RIGHT RIGHT: 45 -> 40 -> 30 -> 30 (clamped). */
    require(press(ui, &io, B_RIGHT, 0) == 0, "to 40");
    require_status(ui, "FPS LOCK 40 - UNEVEN CADENCE - X TWICE", "40 status");
    require_row(ui, 0u, ROW_FPS_40_PENDING_SEL, "40 pending row");
    require(press(ui, &io, B_RIGHT, 0) == 0, "to 30");
    require_status(ui, "FPS LOCK 30 - X SAVES", "30 status");
    require(press(ui, &io, B_RIGHT, 0) == 0, "clamp at 30");
    require_status(ui, "FPS LOCK 30 - X SAVES", "clamped status");
    require_row(ui, 0u, ROW_FPS_30_PENDING_SEL, "30 pending row");
    require(!has_row_containing(ui, "UNEVEN"), "no cadence note at 30");
    require(has_row_containing(ui, ROW_PENDING_1), "pending count row");
    /* X saves 30 directly (not risky); the 45 file becomes archive 0000. */
    require(press(ui, &io, B_CROSS, 0) == 0, "X saves 30 at once");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "30 needs no arming");
    require_status(ui, "SAVE OK", "save 30 status");
    require_file(fs, PORT_INI, CANON_30, "canonical 30 written");
    require_file(fs, ARCHIVE("0000"), CANON_45, "previous file archived");
    require_rows(ui, saved_30_undo, 5u, "saved 30 rows with UNDO");
    require(ui->row_style[2] == MANAGER_SETTINGS_UI_STYLE_ITEM, "UNDO row is an item");
    require(ui->state.undo_available && ui->state.undo_index == 0u, "undo 0000");

    /* LEFT x4: 30 -> 40 -> 45 -> 60 -> 60 (clamped); PENDING vs saved 30. */
    require(press(ui, &io, B_LEFT, 0) == 0, "to 40");
    require(press(ui, &io, B_LEFT, 0) == 0, "to 45");
    require(press(ui, &io, B_LEFT, 0) == 0, "to 60");
    require(press(ui, &io, B_LEFT, 0) == 0, "clamp at 60");
    require_status(ui, "FPS LOCK 60 - X SAVES", "60 status");
    require_row(ui, 0u, ROW_FPS_60_PENDING_SEL, "60 pending against saved 30");

    /* DOWN DOWN selects UNDO; LEFT there only explains; X restores 0000. */
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN to ROOM RENDER");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE, "ROOM RENDER on the way");
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN to UNDO");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO, "UNDO selected");
    require_rows(ui, pending_60_undo_sel, 6u, "UNDO selected rows");
    require(ui->row_style[0] == MANAGER_SETTINGS_UI_STYLE_ITEM &&
                ui->row_style[1] == MANAGER_SETTINGS_UI_STYLE_ITEM &&
                ui->row_style[2] == MANAGER_SETTINGS_UI_STYLE_SELECTED,
            "selection styles moved");
    require(press(ui, &io, B_LEFT, 0) == 0, "LEFT on UNDO");
    require_status(ui, "X RESTORES PORT-0000.INI", "UNDO row hint");
    require(ui->state.pending.fps_lock == 60u, "LEFT on UNDO changes no value");
    require(press(ui, &io, B_CROSS, 0) == 0, "X restores");
    require_status(ui, "UNDO OK", "undo status");
    require_file(fs, PORT_INI, CANON_45, "archive 0000 restored");
    require_file(fs, ARCHIVE("0001"), CANON_30, "replaced file archived as 0001");
    require_no_inflight(fs, "no sidecar after undo");
    require_rows(ui, undone_45, 6u, "rows after undo (redo offered)");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO, "UNDO stays selected");
    require(ui->state.pending.fps_lock == 45u, "pending edit dropped by the reload");
    /* X again = redo. */
    require(press(ui, &io, B_CROSS, 0) == 0, "X redoes");
    require_status(ui, "UNDO OK", "redo status");
    require_file(fs, PORT_INI, CANON_30, "redo restored 30");
    require_file(fs, ARCHIVE("0002"), CANON_45, "redo archived 45 as 0002");
    require_row(ui, 2u, ROW_UNDO_SEL("0002"), "newest archive named");
    /* UP UP back to FPS LOCK leaves the status alone (nothing was armed). */
    require(press(ui, &io, B_UP, 0) == 0, "UP to ROOM RENDER");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE, "ROOM RENDER selected");
    require(press(ui, &io, B_UP, 0) == 0, "UP to FPS LOCK");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_FPS_LOCK, "FPS LOCK selected");
    require_status(ui, "UNDO OK", "navigation keeps the status");
    require_row(ui, 0u, ROW_FPS_30_SAVED_SEL, "30 saved selected");
    require_row(ui, 2u, ROW_UNDO("0002"), "UNDO unselected");
}

static void test_room_render(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;
    static const char *const after_defaults[] = {
        ROW_FPS_60_DEFAULT_SEL, ROW_SCALE_FAST_DEFAULT, ROW_UNDO("0000"),
        ROW_ABSENT, ROW_NO_LOG,
    };

    ui_open(ui, &io);
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN to ROOM RENDER");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE, "ROOM RENDER selected");
    /* LEFT at the default clamps and explains. */
    require(press(ui, &io, B_LEFT, 0) == 0, "LEFT at 480X270 clamps");
    require_status(ui, "ROOM RENDER 480X270 FAST - X SAVES", "clamped status");
    require(ui->state.pending.render_scale == 1u &&
                manager_settings_pending_count(&ui->state) == 0u,
            "clamp leaves nothing pending");
    require_row(ui, 1u, ROW_SCALE_FAST_DEFAULT_SEL, "still the default");
    /* RIGHT: 1 -> 2 (pending); RIGHT again clamps. */
    require(press(ui, &io, B_RIGHT, 0) == 0, "to 960X540");
    require_status(ui, "ROOM RENDER 960X540 SHARP - X SAVES", "sharp status");
    require(ui->state.pending.render_scale == 2u &&
                ui->state.saved.render_scale == 1u,
            "pending edited, saved untouched");
    require_row(ui, 1u, ROW_SCALE_SHARP_PENDING_SEL, "sharp pending row");
    require(has_row_containing(ui, ROW_PENDING_1), "one pending row");
    require(press(ui, &io, B_RIGHT, 0) == 0, "clamp at 960X540");
    require_status(ui, "ROOM RENDER 960X540 SHARP - X SAVES", "clamped sharp status");
    require(ui->state.pending.render_scale == 2u, "clamp keeps 2");
    /* X saves at once (60 is not risky): render_scale=2 after display_raster. */
    require(press(ui, &io, B_CROSS, 0) == 0, "X saves");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "ROOM RENDER needs no arming");
    require_status(ui, "SAVE OK", "save status");
    require_file(fs, PORT_INI, CANON_SHARP_60, "render_scale=2 written");
    require_no_inflight(fs, "no sidecar after save");
    require_row(ui, 1u, ROW_SCALE_SHARP_SAVED_SEL, "sharp saved row");
    require(ui->state.saved.render_scale == 2u && ui->state.pending.render_scale == 2u,
            "state reloaded after save");
    /* LEFT back to 1 is pending against the saved 2; O O discards it. */
    require(press(ui, &io, B_LEFT, 0) == 0, "back to 480X270");
    require_status(ui, "ROOM RENDER 480X270 FAST - X SAVES", "fast status");
    require_row(ui, 1u, ROW_SCALE_FAST_PENDING_SEL, "fast pending against saved sharp");
    require(press(ui, &io, B_CIRCLE, 0) == 0, "O arms the discard");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_DISCARD, "discard armed");
    require(press(ui, &io, B_CIRCLE, 0) == 1, "second O leaves");
    require_status(ui, "CHANGES DISCARDED", "discard status");
    require(ui->state.pending.render_scale == 2u, "pending equals saved again");
    require_row(ui, 1u, ROW_SCALE_SHARP_SAVED_SEL, "row shows the saved value");
    require_file(fs, PORT_INI, CANON_SHARP_60, "discard writes nothing");
    /* Reopen: the file's 2 is shown as SAVED, not PENDING. */
    ui_open(ui, &io);
    require_row(ui, 1u, ROW_SCALE_SHARP_SAVED, "saved sharp unselected after reopen");
    require(manager_settings_pending_count(&ui->state) == 0u, "nothing pending after reopen");
    /* DEFAULTS archives the file; the row is back at the default. */
    require(press(ui, &io, B_TRIANGLE, 0) == 0 && press(ui, &io, B_TRIANGLE, 0) == 0,
            "TRIANGLE twice");
    require_status(ui, "DEFAULTS OK", "defaults status");
    require_rows(ui, after_defaults, 5u, "rows after defaults");
    require_file(fs, ARCHIVE("0000"), CANON_SHARP_60, "sharp file archived");
    /* UNDO brings 2 back; the hidden UNDO row hands the selection to the
     * row above it. */
    require(press(ui, &io, B_DOWN, 0) == 0 && press(ui, &io, B_DOWN, 0) == 0,
            "DOWN DOWN to UNDO");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO, "UNDO selected");
    require(press(ui, &io, B_CROSS, 0) == 0, "X restores");
    require_status(ui, "UNDO OK", "undo status");
    require_file(fs, PORT_INI, CANON_SHARP_60, "sharp file restored");
    require(!ui->state.undo_available, "restored file equals the newest archive");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE,
            "selection clamps to ROOM RENDER");
    require_row(ui, 1u, ROW_SCALE_SHARP_SAVED_SEL, "sharp saved selected");
    /* Both rows pending: the FPS arming rule still governs X. */
    require(press(ui, &io, B_UP, 0) == 0, "UP to FPS LOCK");
    require(press(ui, &io, B_RIGHT, 0) == 0, "to 45");
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN to ROOM RENDER");
    require(press(ui, &io, B_LEFT, 0) == 0, "to 480X270");
    require(manager_settings_pending_count(&ui->state) == 2u, "two pending rows");
    require(has_row_containing(ui, ROW_PENDING_2), "pending count row for two");
    require_row(ui, 0u, "  FPS LOCK            45                PENDING", "45 pending unselected");
    require_row(ui, 1u, ROW_UNEVEN, "cadence note between the rows");
    require_row(ui, 2u, ROW_SCALE_FAST_PENDING_SEL, "fast pending selected");
    require(press(ui, &io, B_CROSS, 0) == 0, "X arms because 45 is pending");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_SAVE, "armed on the ROOM RENDER row");
    require_status(ui, "UNEVEN CADENCE - X AGAIN TO SAVE 45", "arm status");
    require(press(ui, &io, B_CROSS, 0) == 0, "second X saves both");
    require_status(ui, "SAVE OK", "both saved");
    require_file(fs, PORT_INI, CANON_45, "45 with render_scale=1 written");
    require_file(fs, ARCHIVE("0001"), CANON_SHARP_60, "sharp file archived again");
    require(manager_settings_pending_count(&ui->state) == 0u, "nothing pending");
    /* Edits on the ROOM RENDER row are local while FTP listens; X is not. */
    require(press(ui, &io, B_RIGHT, 1) == 0, "RIGHT while FTP listens");
    require(ui->state.pending.render_scale == 2u, "local edit taken");
    require(press(ui, &io, B_CROSS, 1) == 0, "X while FTP listens");
    require_status(ui, "STOP FTP BEFORE LOCAL CHANGES", "save refused");
    require_file(fs, PORT_INI, CANON_45, "refused save leaves the file");
}

static void test_discard_flow(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 1);
    struct manager_settings_ui *ui = &g_ui;

    fake_add_file(fs, PORT_INI, CANON_45);
    ui_open(ui, &io);
    require_row(ui, 0u, ROW_FPS_45_SAVED_SEL, "45 saved");
    require(press(ui, &io, B_RIGHT, 0) == 0, "to 40");
    require(press(ui, &io, B_CIRCLE, 0) == 0, "O with pending stays");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_DISCARD, "discard armed");
    require_status(ui, "UNSAVED - O AGAIN TO DISCARD", "discard arm status");
    require(press(ui, &io, B_RIGHT, 0) == 0, "RIGHT disarms the discard");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "discard disarmed");
    require_status(ui, "FPS LOCK 30 - X SAVES", "RIGHT acted after disarming");
    require(press(ui, &io, B_CIRCLE, 0) == 0, "O arms again");
    require(press(ui, &io, B_CIRCLE, 0) == 1, "second O leaves");
    require_status(ui, "CHANGES DISCARDED", "discard status");
    require(manager_settings_pending_count(&ui->state) == 0u, "pending dropped");
    require(ui->state.pending.fps_lock == 45u, "pending equals saved");
    require_row(ui, 0u, ROW_FPS_45_SAVED_SEL, "row shows the saved value again");
    require_file(fs, PORT_INI, CANON_45, "discard writes nothing");

    /* O armed, then X: the discard is dropped and X saves (30 not risky). */
    ui_open(ui, &io);
    require(press(ui, &io, B_RIGHT, 0) == 0 && press(ui, &io, B_RIGHT, 0) == 0,
            "to 30");
    require(press(ui, &io, B_CIRCLE, 0) == 0, "O arms discard");
    require(press(ui, &io, B_CROSS, 0) == 0, "X saves instead");
    require_status(ui, "SAVE OK", "save after a dropped discard");
    require_file(fs, PORT_INI, CANON_30, "30 written");
    require_file(fs, ARCHIVE("0000"), CANON_45, "45 archived");
    /* O armed, then TRIANGLE arms DEFAULTS instead. */
    require(press(ui, &io, B_LEFT, 0) == 0, "to 40");
    require(press(ui, &io, B_CIRCLE, 0) == 0, "O arms discard");
    require(press(ui, &io, B_TRIANGLE, 0) == 0, "TRIANGLE re-arms");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_DEFAULTS, "defaults armed");
    require_file(fs, PORT_INI, CANON_30, "nothing written by arming");
    require(press(ui, &io, B_CIRCLE, 0) == 0, "O after TRIANGLE arms discard");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_DISCARD, "discard armed anew");
    require_file(fs, PORT_INI, CANON_30, "still nothing written");
}

static void test_defaults(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;
    static const char *const after_defaults[] = {
        ROW_FPS_60_DEFAULT_SEL, ROW_SCALE_FAST_DEFAULT, ROW_UNDO("0000"),
        ROW_ABSENT, ROW_NO_LOG,
    };

    fake_add_file(fs, PORT_INI, CANON_45);
    ui_open(ui, &io);
    require(press(ui, &io, B_TRIANGLE, 0) == 0, "TRIANGLE arms");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_DEFAULTS, "defaults armed");
    require_status(ui, "TRI AGAIN - ARCHIVE PORT.INI AND USE DEFAULTS",
                   "defaults arm status");
    require_file(fs, PORT_INI, CANON_45, "arming keeps the file");
    require(press(ui, &io, B_RIGHT, 0) == 0, "RIGHT disarms");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "defaults disarmed");
    require_status(ui, "FPS LOCK 40 - UNEVEN CADENCE - X TWICE", "RIGHT acted");
    require(press(ui, &io, B_TRIANGLE, 0) == 0, "TRIANGLE arms again");
    require(press(ui, &io, B_TRIANGLE, 0) == 0, "second TRIANGLE applies");
    require_status(ui, "DEFAULTS OK", "defaults status");
    require(!fake_exists(fs, PORT_INI), "port.ini removed");
    require_file(fs, ARCHIVE("0000"), CANON_45, "port.ini archived");
    require_no_inflight(fs, "defaults leaves no sidecar");
    require_rows(ui, after_defaults, 5u, "rows after defaults");
    require(ui->state.pending.fps_lock == 60u, "pending edit dropped with the file");
    require(manager_settings_pending_count(&ui->state) == 0u, "nothing pending");
    /* Recovery at the next manager start must not undo DEFAULTS. */
    require(manager_storage_recover(&io) == MANAGER_STORAGE_OK, "recover");
    require(!fake_exists(fs, PORT_INI), "recover keeps defaults");
    require(press(ui, &io, B_TRIANGLE, 0) == 0 && press(ui, &io, B_TRIANGLE, 0) == 0,
            "TRIANGLE twice without a file");
    require_status(ui, "DEFAULTS OK - NO PORT.INI", "no-file defaults status");
    require(fake_count_containing_archives(fs) == 1, "no second archive");
    /* UNDO brings the file back and hides itself (equal hashes). */
    require(press(ui, &io, B_DOWN, 0) == 0 && press(ui, &io, B_DOWN, 0) == 0,
            "DOWN DOWN to UNDO");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_UNDO, "UNDO selected");
    require(press(ui, &io, B_CROSS, 0) == 0, "X restores");
    require_status(ui, "UNDO OK", "undo after defaults");
    require_file(fs, PORT_INI, CANON_45, "file restored");
    require(!ui->state.undo_available, "restored file equals the newest archive");
    require(!has_row_containing(ui, "UNDO"), "UNDO row hidden");
    require(ui->selected == MANAGER_SETTINGS_UI_ROW_RENDER_SCALE,
            "selection clamped to the row above");
    require_row(ui, 0u, "  FPS LOCK            45                SAVED", "45 back");
    require_row(ui, 2u, ROW_SCALE_FAST_DEFAULT_SEL, "ROOM RENDER selected after the clamp");
}

static void test_ftp_refusal(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 1);
    struct manager_settings_ui *ui = &g_ui;

    fake_add_file(fs, PORT_INI, CANON_45);
    fake_add_file(fs, ARCHIVE("0000"), CANON_30);
    ui_open(ui, &io);
    require(ui->state.undo_available, "archive differs: UNDO shown");
    require(press(ui, &io, B_RIGHT, 1) == 0, "local edit while FTP listens");
    require(ui->state.pending.fps_lock == 40u, "pending edit is local");
    require(press(ui, &io, B_CROSS, 1) == 0, "X while FTP listens");
    require_status(ui, "STOP FTP BEFORE LOCAL CHANGES", "save refused");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "refusal never arms");
    require_file(fs, PORT_INI, CANON_45, "save refused: file untouched");
    require(press(ui, &io, B_TRIANGLE, 1) == 0, "TRIANGLE while FTP listens");
    require_status(ui, "STOP FTP BEFORE LOCAL CHANGES", "defaults refused");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "defaults never armed");
    require_file(fs, PORT_INI, CANON_45, "defaults refused: file untouched");
    require(press(ui, &io, B_DOWN, 1) == 0 && press(ui, &io, B_DOWN, 1) == 0,
            "DOWN DOWN to UNDO");
    require(press(ui, &io, B_CROSS, 1) == 0, "X on UNDO while FTP listens");
    require_status(ui, "STOP FTP BEFORE LOCAL CHANGES", "undo refused");
    require_file(fs, PORT_INI, CANON_45, "undo refused: file untouched");
    require(!fake_exists(fs, ARCHIVE("0001")), "nothing archived while refused");
    require(ui->state.pending.fps_lock == 40u, "pending edit survives refusals");
    /* FTP stopped: the same press works. */
    require(press(ui, &io, B_CROSS, 0) == 0, "X on UNDO with FTP stopped");
    require_status(ui, "UNDO OK", "undo after FTP stop");
    require_file(fs, PORT_INI, CANON_30, "archive restored");
    require_file(fs, ARCHIVE("0001"), CANON_45, "replaced file archived");
}

static void test_last_boot(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;

    fake_add_file(fs, LOG_PATH, BUILD_LINE RECEIPT_LINE_45);
    ui_open(ui, &io);
    require_row(ui, 3u, "  LAST BOOT: FPS 45 REMAP DEFAULT GUARD", "receipt row");
    require(ui->last_boot.kind == MANAGER_SETTINGS_LAST_BOOT_RECEIPT, "receipt kind");

    /* fps_lock=60(ignored:no-fullspeed-scheduler): the game build could not
     * apply the lock, and the row says so (design 9). */
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, LOG_PATH, BUILD_LINE RECEIPT_LINE_CUSTOM);
    ui_open(ui, &io);
    require_row(ui, 3u, "  LAST BOOT: FPS 60 IGNORED REMAP CUSTOM",
                "custom receipt row");

    /* status=schema: that boot ran on the defaults; the row names the
     * verdict instead of echoing FPS 60. */
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, LOG_PATH, BUILD_LINE RECEIPT_LINE_SCHEMA);
    ui_open(ui, &io);
    require_row(ui, 3u, "  LAST BOOT: FILE SCHEMA - DEFAULTS USED",
                "schema verdict row");

    io = ui_fs_init(fs, 0);
    fake_add_file(fs, LOG_PATH, BUILD_LINE);
    ui_open(ui, &io);
    require_row(ui, 3u, "  LAST BOOT: GAME BUILD IGNORES PORT.INI", "ignoring eboot row");
    require(press(ui, &io, B_RIGHT, 0) == 0 && press(ui, &io, B_RIGHT, 0) == 0 &&
                press(ui, &io, B_RIGHT, 0) == 0,
            "to 30");
    require(press(ui, &io, B_CROSS, 0) == 0, "X saves");
    require_status(ui, "SAVE OK - GAME BUILD IGNORES PORT.INI",
                   "save warns about the ignoring eboot");
    require_file(fs, PORT_INI, CANON_30, "save still allowed");
    require(press(ui, &io, B_LEFT, 0) == 0, "to 40");
    require(press(ui, &io, B_CROSS, 0) == 0, "X arms (40)");
    require_status(ui, "UNEVEN CADENCE - X AGAIN TO SAVE 40", "arming has no suffix");
    require(press(ui, &io, B_CROSS, 0) == 0, "X saves 40");
    require_status(ui, "SAVE OK - GAME BUILD IGNORES PORT.INI", "suffix on the risky save");

    io = ui_fs_init(fs, 0);
    fake_add_file(fs, LOG_PATH, "no build line here\n");
    ui_open(ui, &io);
    require_row(ui, 3u, ROW_NO_LOG, "log without build line");
}

static void test_load_states(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;

    /* Invalid (no schema): defaults shown, SAVE rewrites, original archived. */
    fake_add_file(fs, PORT_INI, "fps_lock=45\n");
    ui_open(ui, &io);
    require_row(ui, 0u, ROW_FPS_60_DEFAULT_SEL, "invalid shows defaults");
    require_row(ui, 1u, ROW_SCALE_FAST_DEFAULT, "invalid shows the scale default");
    require_row(ui, 2u, "  PORT.INI INVALID - DEFAULTS SHOWN", "invalid row");
    require(press(ui, &io, B_CROSS, 0) == 0, "X rewrites");
    require_status(ui, "SAVE OK", "rewrite status");
    require_file(fs, PORT_INI, CANON_60, "canonical defaults written");
    require_file(fs, ARCHIVE("0000"), "fps_lock=45\n", "invalid original archived verbatim");
    /* The archive differs from the live file, so UNDO is offered (section
     * 1.3 detail hides it only for none / equal hashes); it does not parse,
     * so the press refuses and nothing is written. */
    require(ui->state.undo_available && ui->state.undo_index == 0u,
            "unparseable archive is offered");
    require_row(ui, 2u, ROW_UNDO("0000"), "UNDO row for the bad archive");
    require_row(ui, 3u, ROW_OK, "file now ok");
    require(press(ui, &io, B_DOWN, 0) == 0 && press(ui, &io, B_DOWN, 0) == 0,
            "DOWN DOWN to the bad archive");
    require(press(ui, &io, B_CROSS, 0) == 0, "X on the bad archive");
    require_status(ui, "UNDO REFUSED - BAD ARCHIVE", "bad archive refused on the press");
    require_file(fs, PORT_INI, CANON_60, "refused undo leaves the file");
    require_file(fs, ARCHIVE("0000"), "fps_lock=45\n", "refused undo keeps the archive");
    require(fake_count_containing_archives(fs) == 1, "refused undo archives nothing");
    require_no_inflight(fs, "refused undo leaves no sidecar");
    require(ui->state.undo_available, "bad archive stays offered after the refusal");

    /* Ignored lines. */
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, PORT_INI, "schema=1\nfps_lock=45\nbogus=1\n");
    ui_open(ui, &io);
    require_row(ui, 0u, ROW_FPS_45_SAVED_SEL, "45 from file");
    require_row(ui, 3u, "  PORT.INI OK - 1 LINE IGNORED", "ignored row");
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, PORT_INI, "schema=1\nfps_lock=45\nbogus=1\nfps_lock=99\n");
    ui_open(ui, &io);
    require_row(ui, 3u, "  PORT.INI OK - 2 LINES IGNORED", "ignored rows plural");
    /* render_scale from the file; an out-of-set value is an ignored line. */
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, PORT_INI, "schema=1\nrender_scale=2\n");
    ui_open(ui, &io);
    require_row(ui, 1u, ROW_SCALE_SHARP_SAVED, "render_scale=2 from file");
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, PORT_INI, "schema=1\nrender_scale=3\n");
    ui_open(ui, &io);
    require_row(ui, 1u, ROW_SCALE_FAST_DEFAULT, "render_scale=3 falls back to 1");
    require_row(ui, 2u, "  PORT.INI OK - 1 LINE IGNORED", "render_scale=3 ignored");

    /* I/O error at open: edits, SAVE and DEFAULTS refused; the
     * page shows defaults it never read and must not archive the real file. */
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, PORT_INI, CANON_45);
    fake_fail(fs, FAKE_OP_OPEN_READ, 1);
    ui_open(ui, &io);
    fake_no_fail(fs);
    require(ui->state.status == MANAGER_SETTINGS_LOAD_IO_ERROR, "io error state");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "io error hint");
    require_row(ui, 0u, ROW_FPS_60_DEFAULT_SEL, "defaults shown on io error");
    require_row(ui, 2u, "  PORT.INI IO ERROR", "io error row");
    require(press(ui, &io, B_RIGHT, 0) == 0, "RIGHT on io error");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "edit refused");
    require(ui->state.pending.fps_lock == 60u, "no pending edit");
    require(press(ui, &io, B_DOWN, 0) == 0, "DOWN to ROOM RENDER on io error");
    require(press(ui, &io, B_RIGHT, 0) == 0, "RIGHT on ROOM RENDER on io error");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "scale edit refused");
    require(ui->state.pending.render_scale == 1u, "no pending scale edit");
    require(press(ui, &io, B_CROSS, 0) == 0, "X on io error");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "save refused");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "nothing armed");
    require_file(fs, PORT_INI, CANON_45, "file untouched");
    require(press(ui, &io, B_TRIANGLE, 0) == 0, "TRIANGLE on io error");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "defaults refused");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "defaults never armed on io error");
    require(press(ui, &io, B_TRIANGLE, 0) == 0, "second TRIANGLE on io error");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "defaults refused again");
    require_file(fs, PORT_INI, CANON_45, "file untouched by TRIANGLE twice");
    require(!fake_exists(fs, MANAGER_STORAGE_BACKUP_ROOT), "nothing archived on io error");
    require(press(ui, &io, B_CIRCLE, 0) == 1, "O leaves (nothing pending)");
    require_status(ui, "READY", "leaving hands READY to HOME");

    /* Write failure keeps the pending edit; a retry succeeds. */
    io = ui_fs_init(fs, 0);
    fake_add_file(fs, PORT_INI, CANON_45);
    ui_open(ui, &io);
    require(press(ui, &io, B_RIGHT, 0) == 0 && press(ui, &io, B_RIGHT, 0) == 0,
            "to 30");
    fake_fail(fs, FAKE_OP_WRITE, 1);
    require(press(ui, &io, B_CROSS, 0) == 0, "X with a failing write");
    fake_no_fail(fs);
    require_status(ui, "SAVE FAILED - IO ERROR", "write failure status");
    require_file(fs, PORT_INI, CANON_45, "old file intact");
    require_no_inflight(fs, "failure leaves no sidecar");
    require(ui->state.pending.fps_lock == 30u, "pending edit kept");
    require_row(ui, 0u, ROW_FPS_30_PENDING_SEL, "still pending");
    require(has_row_containing(ui, ROW_PENDING_1), "pending row kept");
    fake_fail(fs, FAKE_OP_OPEN_WRITE, 1);
    require(press(ui, &io, B_CROSS, 0) == 0, "X with a failing exclusive open");
    fake_no_fail(fs);
    /* No temp is in the way (step 1 removed any), so the failure is an I/O
     * error (ENOSPC/EIO on the device), not a FILE CONFLICT. */
    require_status(ui, "SAVE FAILED - IO ERROR", "exclusive-open failure status");
    require_file(fs, PORT_INI, CANON_45, "old file intact after the open failure");
    require(ui->state.pending.fps_lock == 30u, "pending edit kept after the open failure");
    require(press(ui, &io, B_CROSS, 0) == 0, "retry");
    require_status(ui, "SAVE OK", "retry status");
    require_file(fs, PORT_INI, CANON_30, "retry wrote");

    /* Save failure with a free-slot shortage. */
    io = ui_fs_init(fs, 1);
    fake_add_file(fs, PORT_INI, CANON_45);
    ui_open(ui, &io);
    require(press(ui, &io, B_RIGHT, 0) == 0 && press(ui, &io, B_RIGHT, 0) == 0,
            "to 30");
    fs->pretend_exists_prefix = MANAGER_STORAGE_BACKUP_ROOT "/port-";
    require(press(ui, &io, B_CROSS, 0) == 0, "X without a free archive index");
    fs->pretend_exists_prefix = NULL;
    require_status(ui, "SAVE FAILED - NO FREE BACKUP SLOT", "capacity status");
    require_file(fs, PORT_INI, CANON_45, "old file intact");
}

static void test_null_and_predicates(void)
{
    struct fake_fs *fs = &g_fs;
    struct manager_storage_io io = ui_fs_init(fs, 0);
    struct manager_settings_ui *ui = &g_ui;

    manager_settings_ui_open(NULL, &io);
    require(manager_settings_ui_handle(NULL, &io, B_CROSS, 0) == 0, "NULL model");
    ui_open(ui, NULL);
    require(ui->state.status == MANAGER_SETTINGS_LOAD_IO_ERROR, "NULL io is an io error");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "NULL io hint");
    require_row(ui, 3u, ROW_NO_LOG, "NULL io has no log");
    require(press(ui, NULL, B_RIGHT, 0) == 0, "RIGHT with NULL io");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "edit refused with NULL io");
    require(press(ui, NULL, B_CROSS, 0) == 0, "X with NULL io");
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "save refused with NULL io");
    require(press(ui, NULL, B_TRIANGLE, 0) == 0 && press(ui, NULL, B_TRIANGLE, 0) == 0,
            "TRIANGLE twice with NULL io");
    /* NULL io is the IO ERROR state, so DEFAULTS is refused before arming
     * (the core's own "DEFAULTS FAILED - IO ERROR" is reached only through
     * the API, manager_settings_host_test). */
    require_status(ui, "PORT.INI IO ERROR - EDITS REFUSED", "defaults refused with NULL io");
    require(ui->armed == MANAGER_SETTINGS_UI_ARM_NONE, "nothing armed with NULL io");
    require(press(ui, NULL, B_CIRCLE, 0) == 1, "O leaves");

    require(manager_settings_ui_text_is_wide("A-Z 0-9 :.-_><?") &&
                !manager_settings_ui_text_is_wide("a") &&
                !manager_settings_ui_text_is_wide("A*B") &&
                !manager_settings_ui_text_is_wide("(") &&
                !manager_settings_ui_text_is_wide(NULL) &&
                manager_settings_ui_text_is_wide(""),
            "wide predicate");
    require(MANAGER_SETTINGS_UI_ROW_CAPACITY == 68u &&
                MANAGER_SETTINGS_UI_ROWS == 12u &&
                MANAGER_SETTINGS_UI_STATUS_MAX < 58u &&
                sizeof(ui->status) <= sizeof(char[96]),
            "capacities match the manager's draw and status buffers");
}

int main(void)
{
    test_open_absent();
    test_edit_save_undo();
    test_room_render();
    test_discard_flow();
    test_defaults();
    test_ftp_refusal();
    test_last_boot();
    test_load_states();
    test_null_and_predicates();
    printf("manager settings ui host tests: PASS (%u checks)\n", checks);
    return 0;
}
