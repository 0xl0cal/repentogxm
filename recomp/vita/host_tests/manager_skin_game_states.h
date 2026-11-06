/*
 * The page states of manager_ui_render_host_test (the same press script,
 * frame names 01..17) driven with a game-skin context, for
 * manager_skin_game_host_test.  Header-only; include after fake_fs.h,
 * manager_ui2_test_common.h and manager_ui2_fixture.h.  The includer
 * provides the frame callback and, optionally, a hook run before every
 * start (the skin test releases the previous skin's memory there).
 */
#ifndef ISAAC_MANAGER_SKIN_GAME_STATES_H
#define ISAAC_MANAGER_SKIN_GAME_STATES_H

#include "manager_ui.h"

#include <stdio.h>
#include <string.h>

typedef void (*ss_frame_fn)(const char *label);

static struct fake_fs ss_fs;
static struct manager_storage_io ss_io;
static struct ui2_ftp ss_ftp;
static struct manager_theme_set ss_themes;
static struct manager_ui ss_ui;
static uint32_t ss_now = 5000u;
static void (*ss_before_start)(void);

static void ss_press(uint32_t buttons)
{
    (void)manager_ui_update(&ss_ui, buttons, ++ss_now);
    (void)manager_ui_update(&ss_ui, 0u, ++ss_now);
}

static void ss_press_n(uint32_t buttons, unsigned int count)
{
    while (count-- != 0u)
        ss_press(buttons);
}

static void ss_goto(enum manager_ui_page page)
{
    unsigned int guard = 0u;
    while (ss_ui.page != page && guard++ < 8u)
        ss_press(MANAGER_SETTINGS_UI_BUTTON_RTRIGGER);
    require(ss_ui.page == page, "reached the page");
}

static void ss_start(const char *ini, const struct manager_theme_fonts *fonts,
                     void *game_context)
{
    struct manager_ui_config config;
    struct manager_ui_host host;
    if (ss_before_start != NULL)
        ss_before_start();
    ui2_populate(&ss_fs, 1);
    if (ini != NULL)
        fake_add_file(&ss_fs, MANAGER_INI_PATH, ini);
    memset(&ss_ftp, 0, sizeof(ss_ftp));
    host = ui2_host(&ss_ftp);
    memset(&config, 0, sizeof(config));
    config.app_version = "01.01";
    config.build_id = "int2l-k2c";
    config.system_lang = MANAGER_LANG_EN;
    config.recovery_result = MANAGER_STORAGE_OK;
    config.fonts_fallback = 0;
    ss_io = ui2_io(&ss_fs, 1);
    manager_theme_set_init(&ss_themes, fonts, game_context);
    manager_ui_init(&ss_ui, &ss_io, &host, &config, &ss_themes, ss_now);
}

/* Every page and box with theme=game in one language; frame labels are
 * "<state>-game-<lang>". */
static void ss_states(const char *lang, const struct manager_theme_fonts *fonts,
                      void *game_context, ss_frame_fn frame)
{
    char ini[128];
    char label[64];
#define SS_FRAME(name)                                                         \
    do {                                                                       \
        (void)snprintf(label, sizeof(label), "%s-game-%s", name, lang);        \
        frame(label);                                                          \
    } while (0)
    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=game\n", lang);
    ss_start(ini, fonts, game_context);
    SS_FRAME("01-home");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    SS_FRAME("02-home-ftp-on");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_UP);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    SS_FRAME("03-box-launch-ftp");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CROSS); /* FTP off again */
    ss_goto(MANAGER_UI_PAGE_MODS);
    ss_press_n(MANAGER_SETTINGS_UI_BUTTON_DOWN, 2u);
    SS_FRAME("04-mods");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_TRIANGLE);
    SS_FRAME("05-mods-detail");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_SQUARE);
    SS_FRAME("06-box-mods-all");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    ss_press_n(MANAGER_SETTINGS_UI_BUTTON_DOWN, 7u);
    SS_FRAME("07-mods-scrolled");
    ss_goto(MANAGER_UI_PAGE_SAVES);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    SS_FRAME("08-saves");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    SS_FRAME("09-box-restore");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_TRIANGLE);
    SS_FRAME("10-saves-all");
    ss_goto(MANAGER_UI_PAGE_SETTINGS);
    SS_FRAME("11-settings");
    while (ss_ui.settings_row != MANAGER_UI_SET_AUTOLAUNCH)
        ss_press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    ss_press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    SS_FRAME("12-settings-pending");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    SS_FRAME("13-box-discard");
    ss_press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    ss_goto(MANAGER_UI_PAGE_ACHIEVEMENTS);
    SS_FRAME("14-achievements");
    ss_goto(MANAGER_UI_PAGE_CONTROLS);
    SS_FRAME("15-controls");
    ss_goto(MANAGER_UI_PAGE_ABOUT);
    SS_FRAME("16-about");
    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=game\nautolaunch=5\n",
                   lang);
    ss_start(ini, fonts, game_context);
    require(ss_ui.countdown_active, "countdown armed");
    SS_FRAME("17-home-countdown");
#undef SS_FRAME
}

#endif
