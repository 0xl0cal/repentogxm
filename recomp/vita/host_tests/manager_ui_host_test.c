/*
 * Manager UI2 page-controller host test
 * (manager_ui_host_test): scripted presses against the display list for
 * every page and state, over the fake FS and a fake FTP host.
 *
 *   - start-up: manager.ini absent -> theme game; this target is built with
 *     MANAGER_UI2_GAME_SKIN and a stub skin, so both the failing skin
 *     (status line, light theme) and a loading skin are exercised;
 *     lang=auto follows the system language; the 5x7 fallback forces
 *     English and says so;
 *   - key repeat: 24-tick delay, 5-tick interval, release stops it;
 *   - L/R page cycling, O back to HOME;
 *   - HOME: FTP start / failure / server death, launch with and without
 *     FTP (confirmation box), prepare_launch and the failure status;
 *   - FTP read-only mode: MODS, SAVES and SETTINGS refuse every write;
 *   - MODS: display names from metadata.xml, toggle writes / removes
 *     disable.it, detail card, enable all / disable all;
 *   - SAVES: back up, restore through the confirmation box, filter;
 *   - SETTINGS: FPS lock with the uneven-cadence box, port.ini written;
 *     theme and language preview immediately and persist in manager.ini;
 *     leaving with pending edits asks, No stays, Yes discards and restores
 *     the look; defaults box;
 *   - countdown: shown seconds, redraw once per second, launch at the
 *     deadline, cancel by any button, suppressed on a recovery warning and
 *     when the last game build ignored port.ini;
 *   - every display list built fits: no overflow, hint groups do not
 *     overlap, in English and Russian.
 * MANAGER_UI_DUMP_DIR=<dir> writes each checked display list as text.
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"
#include "manager_ui2_test_common.h"
#include "manager_ui2_fixture.h"

#include <stdlib.h>

#define B_UP MANAGER_SETTINGS_UI_BUTTON_UP
#define B_DOWN MANAGER_SETTINGS_UI_BUTTON_DOWN
#define B_LEFT MANAGER_SETTINGS_UI_BUTTON_LEFT
#define B_RIGHT MANAGER_SETTINGS_UI_BUTTON_RIGHT
#define B_L MANAGER_SETTINGS_UI_BUTTON_LTRIGGER
#define B_R MANAGER_SETTINGS_UI_BUTTON_RTRIGGER
#define B_CROSS MANAGER_SETTINGS_UI_BUTTON_CROSS
#define B_CIRCLE MANAGER_SETTINGS_UI_BUTTON_CIRCLE
#define B_SQUARE MANAGER_SETTINGS_UI_BUTTON_SQUARE
#define B_TRIANGLE MANAGER_SETTINGS_UI_BUTTON_TRIANGLE

/* ---- stub game skin (the skin hook contract) -------------------------- */

static int skin_result = -1;
static unsigned int skin_calls;
static const char *skin_marker = "stub skin";

static void skin_background(const struct manager_theme *theme,
                            struct manager_canvas *canvas)
{
    manager_canvas_fill_box(canvas, 0, 0, 959, 543,
                            theme->palette.color[MANAGER_COLOR_BACKGROUND]);
}

static void skin_panel(const struct manager_theme *theme,
                       struct manager_canvas *canvas, int x0, int y0, int x1,
                       int y1, enum manager_panel_kind kind)
{
    (void)kind;
    manager_canvas_fill_box(canvas, x0, y0, x1, y1,
                            theme->palette.color[MANAGER_COLOR_PANEL]);
}

static void skin_highlight(const struct manager_theme *theme,
                           struct manager_canvas *canvas, int x0, int y0,
                           int x1, int y1)
{
    manager_canvas_fill_box(canvas, x0, y0, x1, y1,
                            theme->palette.color[MANAGER_COLOR_HIGHLIGHT]);
}

static const struct manager_theme_ops skin_ops = {
    skin_background, skin_panel, skin_highlight, manager_theme_flat_glyph};

int manager_skin_game_init(struct manager_theme *theme,
                           const struct manager_theme_fonts *fonts,
                           void *context)
{
    ++skin_calls;
    require(context == (void *)&skin_marker, "skin gets the stored context");
    if (skin_result != 0)
        return skin_result;
    manager_theme_init_light(theme, fonts);
    theme->palette.color[MANAGER_COLOR_BACKGROUND] = 0xff808080u;
    theme->ops = &skin_ops;
    theme->skin = skin_marker;
    return 0;
}

/* ---- harness ---------------------------------------------------------- */

static struct fake_fs fs;
static struct manager_storage_io io;
static struct ui2_ftp ftp;
static struct manager_theme_fonts fonts;
static struct manager_theme_set themes;
static struct manager_ui ui;
static struct manager_display_list dl;
static char dump[65536];
static uint32_t now = 1000u;
static unsigned int dump_count;

static void check_fit(const char *label)
{
    const struct manager_theme *theme = manager_ui_theme(&ui);
    int left = 0;
    int right = 0;
    unsigned int left_items = 0u;
    unsigned int right_items = 0u;
    unsigned int index;
    require(!dl.overflow, label);
    for (index = 0u; index < dl.hint_count; ++index) {
        const int width = manager_dl_hint_width(theme, &dl, &dl.hints[index]);
        if (dl.hints[index].right) {
            right += width;
            ++right_items;
        } else {
            left += width;
            ++left_items;
        }
    }
    if (left_items > 1u)
        left += 36 * (int)(left_items - 1u);
    if (right_items > 1u)
        right += 36 * (int)(right_items - 1u);
    if (32 + left + 36 > 928 - right)
        fprintf(stderr, "hints overlap in %s: left %d right %d\n", label, left,
                right);
    require(32 + left + 36 <= 928 - right, "hint groups do not overlap");
}

static const char *build(const char *label)
{
    const char *directory = getenv("MANAGER_UI_DUMP_DIR");
    manager_ui_build(&ui, &dl);
    (void)manager_dl_dump(&dl, dump, sizeof(dump));
    check_fit(label);
    if (directory != NULL && directory[0] != '\0') {
        char path[1024];
        FILE *file;
        (void)snprintf(path, sizeof(path), "%s/%03u-%s.txt", directory,
                       dump_count, label);
        file = fopen(path, "wb");
        if (file != NULL) {
            fputs(dump, file);
            fclose(file);
        }
    }
    ++dump_count;
    return dump;
}

static int shows(const char *text)
{
    return strstr(dump, text) != NULL;
}

static unsigned int tick(uint32_t held)
{
    return manager_ui_update(&ui, held, ++now);
}

static unsigned int press(uint32_t buttons)
{
    const unsigned int result = tick(buttons);
    (void)tick(0u);
    return result;
}

static void start(int system_ru, int recovery, int fonts_fallback)
{
    struct manager_ui_config config;
    struct manager_ui_host host = ui2_host(&ftp);
    memset(&config, 0, sizeof(config));
    config.app_version = "01.01";
    config.build_id = "int2l-k2a";
    config.system_lang = system_ru ? MANAGER_LANG_RU : MANAGER_LANG_EN;
    config.recovery_result = recovery;
    config.fonts_fallback = fonts_fallback;
    memset(&ftp, 0, sizeof(ftp));
    io = ui2_io(&fs, 0);
    manager_theme_set_init(&themes, &fonts, (void *)&skin_marker);
    manager_ui_init(&ui, &io, &host, &config, &themes, now);
}

static int exists(const char *path)
{
    return fake_find(&fs, path) >= 0;
}

static const char *file_text(const char *path)
{
    static char text[FAKE_DATA_CAPACITY + 1u];
    const int index = fake_find(&fs, path);
    require(index >= 0, "file exists");
    memcpy(text, fs.nodes[index].data, fs.nodes[index].size);
    text[fs.nodes[index].size] = '\0';
    return text;
}

static void goto_page(enum manager_ui_page page)
{
    unsigned int guard = 0u;
    while (ui.page != page && guard++ < 8u)
        (void)press(B_R);
    require(ui.page == page, "reached the page with R");
}

/* ---- tests ------------------------------------------------------------ */

static void test_startup_and_themes(void)
{
    ui2_populate(&fs, 1);
    skin_result = -1;
    skin_calls = 0u;
    start(0, MANAGER_STORAGE_OK, 0);
    require(skin_calls == 1u, "skin tried once for theme=game");
    require(manager_ui_theme(&ui)->id == MANAGER_THEME_LIGHT,
            "failing skin -> light");
    require(ui.status_tone == MANAGER_TONE_WARNING &&
                strcmp(ui.status, manager_str(MANAGER_LANG_EN,
                                              MSTR_STATUS_THEME_FAILED)) == 0,
            "failing skin -> status line, not an error dialog");
    require(ui.lang == MANAGER_LANG_EN && !ui.countdown_active, "defaults");
    build("home-start");
    require(shows("title Isaac Manager") && shows("tab* Home") &&
                shows("Launch the game") && shows("8 of 10 enabled") &&
                shows("2 of 3 files · 4 backups") &&
                shows("FPS lock 60 · room render 480x270 fast") &&
                shows("build int2l-k2a · resources ok") &&
                shows("Recovery: ok") &&
                shows("Last game start: port.ini ok · fps_lock 60") &&
                shows("hint r × Launch"),
            "HOME rows, values and hints");

    /* A skin that loads is used; it is tried again only in a new set. */
    skin_result = 0;
    skin_calls = 0u;
    start(0, MANAGER_STORAGE_OK, 0);
    require(skin_calls == 1u && manager_ui_theme(&ui)->id == MANAGER_THEME_GAME &&
                manager_ui_theme(&ui)->skin == skin_marker,
            "loading skin -> game theme");
    require(strcmp(ui.status, manager_str(MANAGER_LANG_EN,
                                          MSTR_STATUS_READY)) == 0,
            "no status line when the skin loads");
    skin_result = -1;

    /* lang=auto follows the system language. */
    start(1, MANAGER_STORAGE_OK, 0);
    require(ui.lang == MANAGER_LANG_RU, "auto + Russian system -> ru");
    build("home-ru");
    require(shows("title Менеджер Isaac") && shows("Запустить игру"),
            "Russian HOME");
    /* The 5x7 fallback has no Cyrillic: English and a status line. */
    start(1, MANAGER_STORAGE_OK, 1);
    require(ui.lang == MANAGER_LANG_EN && ui.status_tone == MANAGER_TONE_WARNING &&
                strstr(ui.status, "built-in font") != NULL,
            "font fallback -> English + status");
    /* Recovery failure is the start-up status. */
    start(0, MANAGER_STORAGE_IO_ERROR, 0);
    require(ui.status_tone == MANAGER_TONE_ERROR &&
                strstr(ui.status, "Recovery: failed") != NULL,
            "recovery failure status");
}

static void test_repeat_and_pages(void)
{
    unsigned int step;
    ui2_populate(&fs, 1);
    start(0, MANAGER_STORAGE_OK, 0);
    require(ui.home_row == MANAGER_UI_HOME_LAUNCH, "starts on Launch");
    (void)tick(B_DOWN);
    require(ui.home_row == 1u, "edge moves once");
    for (step = 1u; step < MANAGER_UI_REPEAT_DELAY; ++step)
        (void)tick(B_DOWN);
    require(ui.home_row == 1u, "no repeat before 24 ticks");
    (void)tick(B_DOWN);
    require(ui.home_row == 2u, "first repeat at 24 ticks");
    for (step = 1u; step < MANAGER_UI_REPEAT_INTERVAL; ++step)
        (void)tick(B_DOWN);
    require(ui.home_row == 2u, "no repeat inside the interval");
    (void)tick(B_DOWN);
    require(ui.home_row == 3u, "repeat every 5 ticks");
    (void)tick(0u);
    for (step = 0u; step < 40u; ++step)
        (void)tick(0u);
    require(ui.home_row == 3u && ui.repeat_button == 0u, "release stops repeat");
    (void)press(B_UP);
    (void)press(B_UP);
    (void)press(B_UP);
    (void)press(B_UP);
    require(ui.home_row == MANAGER_UI_HOME_LAUNCH, "clamped at the top");

    {
        static const char *const titles[MANAGER_UI_PAGE_COUNT] = {
            "title Isaac Manager", "title Mods",
#if defined(MANAGER_UI2_GAME_SKIN)
            "title Saves",
#else
            "title Saves and backups",
#endif
            "title Settings", "title Achievements", "title Controls",
            "title About"};
        static const char *const labels[MANAGER_UI_PAGE_COUNT] = {
            "page-home", "page-mods", "page-saves", "page-settings",
            "page-achievements", "page-controls", "page-about"};
        unsigned int lang;
        for (lang = 0u; lang < 2u; ++lang) {
            unsigned int page;
            start(lang == 1u, MANAGER_STORAGE_OK, 0);
            for (page = 0u; page < MANAGER_UI_PAGE_COUNT; ++page) {
                require(ui.page == page, "R walks the tab order");
                build(labels[page]);
                if (lang == 0u)
                    require(shows(titles[page]), "page title");
                (void)press(B_R);
            }
            require(ui.page == MANAGER_UI_PAGE_HOME, "R wraps to HOME");
            (void)press(B_L);
            require(ui.page == MANAGER_UI_PAGE_ABOUT, "L wraps to ABOUT");
            (void)press(B_CIRCLE);
            require(ui.page == MANAGER_UI_PAGE_HOME, "O returns to HOME");
        }
    }
    start(0, MANAGER_STORAGE_OK, 0);
    goto_page(MANAGER_UI_PAGE_ACHIEVEMENTS);
    build("achievements");
    require(shows("No achievement data yet.") &&
                shows("Last game start: port.ini ok"),
            "ACHIEVEMENTS empty state");
    goto_page(MANAGER_UI_PAGE_CONTROLS);
    build("controls");
    require(shows("R · top-left touch") && shows("use active item") &&
                shows("START + R") && shows("The game decides which slot"),
            "CONTROLS facts");
    goto_page(MANAGER_UI_PAGE_ABOUT);
    build("about");
    require(shows("resources/packed/animations.a") &&
                shows("Version 01.01 · build int2l-k2a · manager ui2") &&
                shows("mods: 10 directories · saves: 2 of 3 files present") &&
                shows("port.ini ok · fps_lock 60") &&
                !shows("Last game start"),
            "ABOUT facts; the last start under its heading, unlabelled");
}

static void test_ftp_and_launch(void)
{
    unsigned int result;
    ui2_populate(&fs, 1);
    start(0, MANAGER_STORAGE_OK, 0);
    (void)press(B_DOWN);
    require(ui.home_row == MANAGER_UI_HOME_FTP, "FTP row");
    build("home-ftp-row");
    require(shows("hint r × Start FTP") && shows("off"), "FTP off");
    (void)press(B_CROSS);
    require(ui.ftp_on && ftp.starts == 1u &&
                ui.status_tone == MANAGER_TONE_WARNING,
            "FTP started, warning status");
    build("home-ftp-on");
    require(shows("on · 192.168.1.20:1337") && shows(" · read-only") &&
                shows("hint r × Stop FTP"),
            "FTP on: endpoint, read-only values");

    /* Read-only pages: nothing is written. */
    goto_page(MANAGER_UI_PAGE_MODS);
    require(strstr(ui.status, "Read-only") != NULL, "MODS opens read-only");
    build("mods-readonly");
    require(!shows("Toggle") && !shows("Enable all"), "no write hints");
    (void)press(B_CROSS);
    (void)press(B_SQUARE);
    require(!exists(MANAGER_STORAGE_MOD_ROOT "/836319872/disable.it") &&
                ui.confirm == MANAGER_UI_CONFIRM_NONE,
            "MODS writes refused");
    goto_page(MANAGER_UI_PAGE_SAVES);
    {
        const size_t before = fs.node_count;
        (void)press(B_SQUARE);
        (void)press(B_CROSS);
        require(fs.node_count == before && ui.confirm == MANAGER_UI_CONFIRM_NONE,
                "SAVES writes refused");
    }
    goto_page(MANAGER_UI_PAGE_SETTINGS);
    (void)press(B_RIGHT);
    require(manager_ui_pending_count(&ui) == 0u &&
                strstr(ui.status, "Read-only") != NULL,
            "SETTINGS edits refused");
    goto_page(MANAGER_UI_PAGE_HOME);

    /* Launch while FTP listens asks first. */
    (void)press(B_UP);
    result = press(B_CROSS);
    require((result & MANAGER_UI_LAUNCH) == 0u &&
                ui.confirm == MANAGER_UI_CONFIRM_LAUNCH_FTP,
            "launch with FTP -> box");
    build("home-launch-ftp-box");
    require(shows("FTP is on — stop it and launch?") && shows("hint r × Yes") &&
                shows("hint l ○ No") && shows("scrim"),
            "launch box");
    (void)press(B_CIRCLE);
    require(ui.confirm == MANAGER_UI_CONFIRM_NONE && ui.ftp_on, "No keeps FTP");
    (void)press(B_CROSS);
    result = press(B_CROSS);
    require((result & MANAGER_UI_LAUNCH) != 0u, "Yes -> launch");
    require(manager_ui_prepare_launch(&ui) == 0 && !ui.ftp_on &&
                ftp.stops != 0u && strstr(ui.status, "Returning") != NULL,
            "prepare: FTP stopped, returning status");
    manager_ui_launch_failed(&ui);
    require(ui.status_tone == MANAGER_TONE_ERROR &&
                strstr(ui.status, "did not start") != NULL,
            "LoadExec failure status");
    result = press(B_CROSS);
    require((result & MANAGER_UI_LAUNCH) != 0u, "launch without FTP: no box");

    /* Start failure, then a server that dies. */
    (void)press(B_DOWN);
    ftp.start_result = -5;
    (void)press(B_CROSS);
    require(!ui.ftp_on && ui.ftp_failed && ui.status_tone == MANAGER_TONE_ERROR,
            "start failure");
    build("home-ftp-failed");
    require(shows("did not start"), "failure value");
    ftp.start_result = 0;
    (void)press(B_CROSS);
    require(ui.ftp_on, "restarted");
    ftp.listening = 0;
    result = tick(0u);
    require((result & MANAGER_UI_REDRAW) != 0u && !ui.ftp_on &&
                strstr(ui.status, "after an error") != NULL,
            "server death noticed");
}

static void test_mods(void)
{
    ui2_populate(&fs, 1);
    start(0, MANAGER_STORAGE_OK, 0);
    goto_page(MANAGER_UI_PAGE_MODS);
    build("mods");
    require(shows("External Item Descriptions") &&
                shows("836319872 · v4.92") &&
                shows("Описания предметов (русский)") && !shows("vita-perf") &&
                shows("8 of 10 enabled") && shows("1 / 10") && shows("scroll"),
            "MODS rows with metadata names, scroll bar");
    require(ui.mods[0].meta.from_metadata, "names read on entry");
    {
        size_t index;
        size_t eid = UI2_MOD_COUNT;
        for (index = 0u; index < ui.mod_count; ++index)
            if (strcmp(ui.mods[index].entry.name, "836319872") == 0)
                eid = index;
        require(eid < ui.mod_count, "EID listed");
        while (ui.mod_selection < eid)
            (void)press(B_DOWN);
        while (ui.mod_selection > eid)
            (void)press(B_UP);
    }
    (void)press(B_CROSS);
    require(exists(MANAGER_STORAGE_MOD_ROOT "/836319872/disable.it") &&
                strcmp(ui.status, "External Item Descriptions disabled — "
                                  "disable.it written") == 0 &&
                ui.status_tone == MANAGER_TONE_OK,
            "toggle off writes disable.it");
    (void)press(B_CROSS);
    require(!exists(MANAGER_STORAGE_MOD_ROOT "/836319872/disable.it") &&
                strstr(ui.status, "enabled — disable.it removed") != NULL,
            "toggle on removes it");
    /* A write that fails leaves the mod as it was and says why (enabling
     * parked disable.it as .isaac-manager-disable-marker, so disabling
     * again is a rename). */
    fake_fail(&fs, FAKE_OP_RENAME, 1);
    (void)press(B_CROSS);
    fake_no_fail(&fs);
    require(!exists(MANAGER_STORAGE_MOD_ROOT "/836319872/disable.it") &&
                ui.status_tone == MANAGER_TONE_ERROR &&
                strstr(ui.status, "External Item Descriptions not changed: ") ==
                    ui.status,
            "failed toggle: error status, nothing written");
    (void)press(B_TRIANGLE);
    require(ui.detail_open, "detail card");
    build("mods-detail");
    require(shows("Directory: mods/836319872") && shows("Version: 4.92") &&
                shows("State: enabled (no disable.it)") &&
                shows("hint r ○ Close"),
            "detail lines");
    (void)press(B_CIRCLE);
    require(!ui.detail_open && ui.page == MANAGER_UI_PAGE_MODS,
            "O closes the card only");
    (void)press(B_SQUARE);
    require(ui.confirm == MANAGER_UI_CONFIRM_MODS_ALL, "all -> box");
    build("mods-all-box");
    require(shows("Enable all mods (2)?"), "enable-all question counts");
    (void)press(B_CROSS);
    require(!exists(MANAGER_STORAGE_MOD_ROOT "/2491006386/disable.it") &&
                !exists(MANAGER_STORAGE_MOD_ROOT "/1348031964/disable.it") &&
                strstr(ui.status, "Mods changed: 2") != NULL,
            "enable all");
    (void)press(B_SQUARE);
    build("mods-all-box-disable");
    require(shows("Disable all mods (10)?"), "disable-all question");
    (void)press(B_CIRCLE);
    require(exists(MANAGER_STORAGE_MOD_ROOT "/836319872") &&
                !exists(MANAGER_STORAGE_MOD_ROOT "/836319872/disable.it"),
            "No writes nothing");
    while (ui.mod_selection + 1u < ui.mod_count)
        (void)press(B_DOWN);
    build("mods-scrolled");
    require(shows("10 / 10") && shows("vita-perf") &&
                !shows("Описания предметов"),
            "scrolled to the end: the last row, no metadata, dir name");
}

static void test_saves(void)
{
    ui2_populate(&fs, 1);
    start(0, MANAGER_STORAGE_OK, 0);
    goto_page(MANAGER_UI_PAGE_SAVES);
    build("saves");
    require(shows("File 1") && shows("File 3") && shows("no save") &&
                shows("Backups of File 1 (3)") && shows("slot1-0003.dat") &&
                shows("20260821.persistentgamedata1.dat") &&
                shows("hint r □ Back up File 1"),
            "SAVES cards, filtered list, hints");
    (void)press(B_SQUARE);
    require(strstr(ui.status, "Backup of File 1 written — ok") != NULL,
            "backup written");
    build("saves-after-backup");
    require(shows("Backups of File 1 (4)"), "new backup listed");
    (void)press(B_CROSS);
    require(ui.confirm == MANAGER_UI_CONFIRM_RESTORE, "restore -> box");
    build("saves-restore-box");
    require(shows("Restore into File 1?") &&
                shows("The current File 1 is kept as a new backup."),
            "restore box");
    (void)press(B_CROSS);
    require(strstr(ui.status, "File 1 restored from ") != NULL &&
                ui.status_tone == MANAGER_TONE_OK,
            "restored");
    (void)press(B_RIGHT);
    (void)press(B_RIGHT);
    require(ui.save_slot == 3, "slot 3");
    (void)press(B_SQUARE);
    require(strstr(ui.status, "File 3 has no save") != NULL, "missing slot");
    (void)press(B_TRIANGLE);
    build("saves-all");
    require(shows("All backups (") && shows("hint r △ File 3 only") &&
                shows(" File 2 · ") && shows(" File 1 · "),
            "all slots view names each copy's file");
    (void)press(B_CIRCLE);
    require(ui.page == MANAGER_UI_PAGE_HOME, "O back");
}

static void test_settings(void)
{
    unsigned int guard;
    ui2_populate(&fs, 1);
    fake_add_file(&fs, MANAGER_INI_PATH, "theme=light\n");
    start(0, MANAGER_STORAGE_OK, 0);
    require(manager_ui_theme(&ui)->id == MANAGER_THEME_LIGHT, "theme=light");
    goto_page(MANAGER_UI_PAGE_SETTINGS);
    build("settings");
    require(shows("GAME") && shows("MANAGER") && shows("FILES") &&
                shows("FPS lock") && shows("EID mod") && shows("Theme") &&
                shows("Light") && shows("Auto (English)") &&
                shows("port.ini ok") && shows("hint r × Save"),
            "SETTINGS rows");
    /* FPS lock to an uneven value: the box, then port.ini. */
    guard = 0u;
    while (ui.settings.state.pending.fps_lock != 45u && guard++ < 6u)
        (void)press(B_LEFT);
    if (ui.settings.state.pending.fps_lock != 45u) {
        guard = 0u;
        while (ui.settings.state.pending.fps_lock != 45u && guard++ < 6u)
            (void)press(B_RIGHT);
    }
    require(ui.settings.state.pending.fps_lock == 45u &&
                manager_ui_pending_count(&ui) == 1u,
            "FPS pending 45");
    build("settings-fps45");
    require(shows("Save (1)") && shows("45 and 40 alternate"), "pending + help");
    (void)press(B_CROSS);
    require(ui.confirm == MANAGER_UI_CONFIRM_SETTINGS &&
                ui.settings.armed == MANAGER_SETTINGS_UI_ARM_SAVE,
            "uneven -> box");
    build("settings-uneven-box");
    require(shows("Save FPS lock 45?"), "uneven question");
    (void)press(B_CROSS);
    require(strstr(file_text(MANAGER_STORAGE_PORT_INI_PATH), "fps_lock=45") !=
                    NULL &&
                ui.status_tone == MANAGER_TONE_OK,
            "port.ini saved");
    require(ui.settings.state.undo_available, "undo offered after a save");

    /* Theme previews at once, persists with the save. */
    while (ui.settings_row != MANAGER_UI_SET_THEME)
        (void)press(B_DOWN);
    (void)press(B_LEFT);
    require(ui.ini_pending.theme == MANAGER_THEME_DARK &&
                manager_ui_theme(&ui)->id == MANAGER_THEME_DARK,
            "dark previews immediately");
    (void)press(B_CIRCLE);
    require(ui.confirm == MANAGER_UI_CONFIRM_SETTINGS &&
                ui.settings.armed == MANAGER_SETTINGS_UI_ARM_DISCARD,
            "leaving with edits asks");
    build("settings-discard-box");
    require(shows("Discard unsaved changes (1)?"), "discard question");
    (void)press(B_CIRCLE);
    require(ui.page == MANAGER_UI_PAGE_SETTINGS &&
                manager_ui_theme(&ui)->id == MANAGER_THEME_DARK,
            "No stays with the edit");
    (void)press(B_R);
    require(ui.confirm == MANAGER_UI_CONFIRM_SETTINGS, "R with edits asks too");
    (void)press(B_CROSS);
    require(ui.page == MANAGER_UI_PAGE_ACHIEVEMENTS &&
                manager_ui_theme(&ui)->id == MANAGER_THEME_LIGHT &&
                strstr(ui.status, "Changes discarded") != NULL,
            "Yes discards, restores the look, goes on");
    goto_page(MANAGER_UI_PAGE_SETTINGS);
    while (ui.settings_row != MANAGER_UI_SET_THEME)
        (void)press(B_DOWN);
    (void)press(B_LEFT);
    (void)press(B_LEFT);
    require(ui.ini_pending.theme == MANAGER_THEME_GAME &&
                manager_ui_theme(&ui)->id == MANAGER_THEME_LIGHT &&
                strstr(ui.status, "Game theme could not be read") != NULL,
            "game with a failing skin: preview falls back, says why");
    (void)press(B_RIGHT);
    require(ui.ini_pending.theme == MANAGER_THEME_DARK, "back to dark");
    /* Language previews too. */
    while (ui.settings_row != MANAGER_UI_SET_LANG)
        (void)press(B_UP);
    (void)press(B_RIGHT);
    (void)press(B_RIGHT);
    require(ui.ini_pending.lang == MANAGER_INI_LANG_RU &&
                ui.lang == MANAGER_LANG_RU,
            "Russian previews immediately");
    build("settings-ru-pending");
    require(shows("title Настройки") && shows("Сохранить (2)"),
            "Russian page, two pending");
    (void)press(B_CROSS);
    require_file(&fs, MANAGER_INI_PATH,
                 "# Isaac Vita manager settings (edited on the manager's "
                 "Settings page)\nlang=ru\nautolaunch=off\ntheme=dark\n",
                 "manager.ini written");
    require(exists(MANAGER_STORAGE_BACKUP_ROOT "/manager-0000.ini"),
            "previous manager.ini archived");
    require(ui.ini_saved.theme == MANAGER_THEME_DARK &&
                manager_ui_pending_count(&ui) == 0u &&
                ui.status_tone == MANAGER_TONE_OK,
            "saved");

    /* EID row: a pending toggle applied on save. */
    while (ui.settings_row != MANAGER_UI_SET_EID)
        (void)press(B_UP);
    (void)press(B_LEFT);
    require(ui.eid_pending == 0 && manager_ui_pending_count(&ui) == 1u,
            "EID pending off");
    (void)press(B_CROSS);
    require(exists(MANAGER_STORAGE_MOD_ROOT "/836319872/disable.it"),
            "EID disabled on save");

    /* Defaults through the box. */
    while (ui.settings_row != MANAGER_UI_SET_DEFAULTS)
        (void)press(B_DOWN);
    build("settings-defaults-row");
    require(shows("hint r × Выполнить"), "Run hint on FILES rows");
    (void)press(B_CROSS);
    require(ui.settings.armed == MANAGER_SETTINGS_UI_ARM_DEFAULTS, "defaults box");
    (void)press(B_CROSS);
    require(!exists(MANAGER_STORAGE_PORT_INI_PATH), "port.ini removed");
    (void)press(B_CROSS);
    (void)press(B_CROSS);
    require(strstr(ui.status, manager_str(MANAGER_LANG_RU,
                                          MSTR_SET_DEFAULTS_NOFILE)) != NULL,
            "defaults again: nothing to remove");
}

static void test_countdown(void)
{
    unsigned int result;
    uint32_t base;
    ui2_populate(&fs, 1);
    fake_add_file(&fs, MANAGER_INI_PATH, "autolaunch=5\ntheme=dark\n");
    start(0, MANAGER_STORAGE_OK, 0);
    base = now;
    require(ui.countdown_active && ui.countdown_shown == 5u, "countdown armed");
    build("home-countdown");
    require(shows("Launching the game in 5 s") &&
                shows("press any button to stay") && shows("hint r ○ Stay here"),
            "countdown row");
    now = base + 59u;
    result = manager_ui_update(&ui, 0u, now);
    require(result == 0u, "no redraw inside a second");
    now = base + 60u;
    result = manager_ui_update(&ui, 0u, now);
    require(result == MANAGER_UI_REDRAW && ui.countdown_shown == 4u,
            "redraw once per second");
    now = base + 299u;
    result = manager_ui_update(&ui, 0u, now);
    require((result & MANAGER_UI_LAUNCH) == 0u, "not yet");
    now = base + 300u;
    result = manager_ui_update(&ui, 0u, now);
    require((result & MANAGER_UI_LAUNCH) != 0u && !ui.countdown_active,
            "launch at the deadline");

    start(0, MANAGER_STORAGE_OK, 0);
    result = press(B_CIRCLE);
    require((result & MANAGER_UI_LAUNCH) == 0u && !ui.countdown_active &&
                strstr(ui.status, "cancelled") != NULL &&
                ui.page == MANAGER_UI_PAGE_HOME,
            "any button cancels, the press does nothing else");
    now += 1000u;
    result = tick(0u);
    require((result & MANAGER_UI_LAUNCH) == 0u, "never re-arms");

    start(0, MANAGER_STORAGE_SYNC_WARNING, 0);
    require(!ui.countdown_active, "suppressed after a recovery warning");
    fs.nodes[fake_find(&fs, UI2_LOG_PATH)].size = strlen(UI2_BUILD_LINE);
    start(0, MANAGER_STORAGE_OK, 0);
    require(ui.settings.last_boot.kind == MANAGER_SETTINGS_LAST_BOOT_IGNORES &&
                !ui.countdown_active,
            "suppressed when the game build ignored port.ini");
}

int main(void)
{
    ui2_load_fonts(&fonts);
    test_startup_and_themes();
    test_repeat_and_pages();
    test_ftp_and_launch();
    test_mods();
    test_saves();
    test_settings();
    test_countdown();
    printf("manager ui host test: %u display lists, %u checks PASS\n",
           dump_count, checks);
    return 0;
}
