/*
 * Manager UI2 debug overlay row (knob ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW ->
 * MANAGER_DEBUG_OVERLAY_ROW=1) over the fake FS.  The row reads and writes
 * port.ini debug_overlay = off|compact|full through the shared parser's
 * generic key API, so what this test proves depends on the parser it is
 * linked with, and it says which case it ran:
 *
 *   - parser without the key (host_vita_settings_parse.c before the debug
 *     overlay change): the row is absent from SETTINGS in English and
 *     Russian, no row index reaches it, saving port.ini is unchanged;
 *   - parser with the key: the row sits in the Game section after EID,
 *     shows off by default, ←/→ step off -> compact -> full (clamped), the
 *     pending count and the × save write debug_overlay=<value> into
 *     port.ini, a reopen shows the saved value, DEFAULTS brings it back to
 *     off, and FTP makes it read-only.
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"
#include "manager_ui2_test_common.h"
#include "manager_ui2_fixture.h"

#include <stdlib.h>

static struct fake_fs fs;
static struct manager_storage_io io;
static struct ui2_ftp ftp;
static struct manager_theme_fonts fonts;
static struct manager_theme_set themes;
static struct manager_ui ui;
static struct manager_display_list dl;
static char dump[65536];
static uint32_t now = 1000u;

static const char *build(void)
{
    manager_ui_build(&ui, &dl);
    require(!dl.overflow, "display list fits");
    (void)manager_dl_dump(&dl, dump, sizeof(dump));
    return dump;
}

static int shows(const char *text)
{
    return strstr(dump, text) != NULL;
}

static void press(uint32_t buttons)
{
    (void)manager_ui_update(&ui, buttons, ++now);
    (void)manager_ui_update(&ui, 0u, ++now);
}

static void start(const char *lang)
{
    struct manager_ui_config config;
    struct manager_ui_host host;
    char ini[64];
    ui2_populate(&fs, 1);
    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=light\n", lang);
    fake_add_file(&fs, MANAGER_INI_PATH, ini);
    memset(&ftp, 0, sizeof(ftp));
    host = ui2_host(&ftp);
    memset(&config, 0, sizeof(config));
    config.app_version = "01.01";
    config.build_id = "int2l-k2c";
    config.system_lang = MANAGER_LANG_EN;
    config.recovery_result = MANAGER_STORAGE_OK;
    io = ui2_io(&fs, 0);
    manager_theme_set_init(&themes, &fonts, NULL);
    manager_ui_init(&ui, &io, &host, &config, &themes, now);
}

static void goto_settings(void)
{
    unsigned int guard = 0u;
    while (ui.page != MANAGER_UI_PAGE_SETTINGS && guard++ < 8u)
        press(MANAGER_SETTINGS_UI_BUTTON_RTRIGGER);
    require(ui.page == MANAGER_UI_PAGE_SETTINGS, "reached SETTINGS");
}

static int rows_have_overlay(void)
{
    uint8_t rows[MANAGER_UI_SET_COUNT];
    const unsigned int count = manager_ui_settings_rows(&ui, rows);
    unsigned int index;
    for (index = 0u; index < count; ++index)
        if (rows[index] == MANAGER_UI_SET_DEBUG_OVERLAY)
            return 1;
    return 0;
}

/* The pending value of the key, as the parser spells it. */
static const char *overlay_value(void)
{
    static char text[ISAAC_VITA_SETTINGS_VALUE_MAX + 1u];
    text[0] = '\0';
    if (ui.overlay_key >= 0)
        (void)isaac_vita_settings_get_text(
            &ui.settings.state.pending,
            (enum isaac_vita_settings_key)ui.overlay_key, text, sizeof(text));
    return text;
}

static int port_ini_has(const char *line)
{
    char text[1024];
    size_t length = 0u;
    if (manager_storage_read_text(&io, MANAGER_STORAGE_PORT_INI_PATH, text,
                                  sizeof(text), &length) != MANAGER_STORAGE_OK)
        return 0;
    return strstr(text, line) != NULL;
}

static void test_absent(const char *lang)
{
    unsigned int steps;
    start(lang);
    goto_settings();
    build();
    require(ui.overlay_key < 0, "parser without debug_overlay: no key");
    require(!rows_have_overlay(), "row absent");
    require(!shows("Debug overlay") && !shows("Отладочный оверлей"),
            "no overlay label on the page");
    for (steps = 0u; steps < MANAGER_UI_SET_COUNT + 2u; ++steps) {
        require(ui.settings_row != MANAGER_UI_SET_DEBUG_OVERLAY,
                "the cursor never lands on the absent row");
        press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    }
    /* A save still writes a port.ini without the key (a failing write
     * first: nothing changes). */
    while (ui.settings_row != MANAGER_UI_SET_FPS)
        press(MANAGER_SETTINGS_UI_BUTTON_UP);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    require(ui.settings.state.pending.fps_lock == 30u, "FPS lock 30 pending");
    fake_fail(&fs, FAKE_OP_OPEN_WRITE, 1);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    fake_no_fail(&fs);
    require(ui.status_tone == MANAGER_TONE_ERROR && !port_ini_has("fps_lock=30"),
            "failed save reported, port.ini unchanged");
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require(port_ini_has("fps_lock=30\n"), "port.ini written");
    require(!port_ini_has("debug_overlay"), "no debug_overlay written");
    if (strcmp(lang, "en") == 0)
        require_file(&fs, MANAGER_INI_PATH, "lang=en\ntheme=light\n",
                     "manager.ini untouched");
}

static void test_present(const char *lang)
{
    const int ru = strcmp(lang, "ru") == 0;
    uint8_t rows[MANAGER_UI_SET_COUNT];
    unsigned int count;
    unsigned int index;
    start(lang);
    goto_settings();
    require(ui.overlay_key >= 0, "parser knows debug_overlay");
    count = manager_ui_settings_rows(&ui, rows);
    for (index = 0u; index < count; ++index)
        if (rows[index] == MANAGER_UI_SET_DEBUG_OVERLAY)
            break;
    require(index < count && index > 0u &&
                rows[index - 1u] == MANAGER_UI_SET_EID &&
                index + 1u < count && rows[index + 1u] == MANAGER_UI_SET_LANG,
            "row in the Game section after EID");
    while (ui.settings_row != MANAGER_UI_SET_DEBUG_OVERLAY)
        press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    build();
    require(shows(ru ? "Отладочный оверлей" : "Debug overlay") &&
                strcmp(overlay_value(), "off") == 0,
            "row shown, off by default");
    require(manager_ui_pending_count(&ui) == 0u, "nothing pending");
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    build();
    require(strcmp(overlay_value(), "compact") == 0 &&
                shows(ru ? "компактный" : "compact") &&
                manager_ui_pending_count(&ui) == 1u,
            "→ compact, one pending edit");
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    build();
    require(strcmp(overlay_value(), "full") == 0 &&
                shows(ru ? "полный" : "full"),
            "→ full, clamped");
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    require(strcmp(overlay_value(), "off") == 0 &&
                manager_ui_pending_count(&ui) == 0u,
            "← back to off, clamped");
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require(ui.status_tone == MANAGER_TONE_OK &&
                port_ini_has("debug_overlay=compact\n"),
            "× saves debug_overlay=compact into port.ini");
    press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    goto_settings();
    while (ui.settings_row != MANAGER_UI_SET_DEBUG_OVERLAY)
        press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    build();
    require(strcmp(overlay_value(), "compact") == 0 &&
                shows(ru ? "компактный" : "compact") &&
                manager_ui_pending_count(&ui) == 0u,
            "reopened page shows the saved value");
    /* FTP on: read-only. */
    press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    while (ui.home_row != MANAGER_UI_HOME_FTP)
        press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require(ui.ftp_on, "FTP on");
    goto_settings();
    while (ui.settings_row != MANAGER_UI_SET_DEBUG_OVERLAY)
        press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    require(strcmp(ui.status, manager_str(ui.lang, MSTR_STATUS_READONLY)) == 0 &&
                manager_ui_pending_count(&ui) == 0u,
            "→ refused while FTP listens");
    press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS); /* FTP off */
    require(!ui.ftp_on, "FTP off");
    /* DEFAULTS archives port.ini: the row shows off again. */
    goto_settings();
    while (ui.settings_row != MANAGER_UI_SET_DEFAULTS)
        press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS); /* confirm */
    while (ui.settings_row != MANAGER_UI_SET_DEBUG_OVERLAY)
        press(MANAGER_SETTINGS_UI_BUTTON_UP);
    build();
    require(strcmp(overlay_value(), "off") == 0 &&
                !port_ini_has("debug_overlay"),
            "defaults bring the row back to off");
}

int main(void)
{
    enum isaac_vita_settings_key key;
    const int known = isaac_vita_settings_key_from_text(
                          "debug_overlay", sizeof("debug_overlay") - 1u,
                          &key) == 0;
    ui2_load_fonts(&fonts);
    if (known) {
        test_present("en");
        test_present("ru");
        printf("manager ui overlay host test: parser knows debug_overlay; "
               "row, steps, save, reopen, defaults and FTP checked; %u checks "
               "PASS\n",
               checks);
    } else {
        test_absent("en");
        test_absent("ru");
        printf("manager ui overlay host test: PARSER WITHOUT debug_overlay - "
               "row absent as designed; the row's own path runs once "
               "host_vita_settings_parse.c knows the key; %u checks PASS\n",
               checks);
    }
    return 0;
}
