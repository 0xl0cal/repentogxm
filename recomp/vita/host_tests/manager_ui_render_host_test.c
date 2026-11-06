/*
 * Manager UI2 render host test (manager_ui_render_host_test):
 * every page and box of the controller painted by manager_ui_draw into a
 * 960x544 canvas with guard words on both sides, in the dark and light
 * themes and in English and Russian, plus the two fallbacks the owner asked
 * for (theme=game with no skin compiled -> light and a status line; font
 * files missing -> built-in 5x7 face, English, status line).
 *
 * Each frame is hashed (FNV-1a over the canvas words) and compared with
 * manager_ui_render_goldens.txt; MANAGER_UI_RENDER_UPDATE=1 rewrites that
 * file instead.  Every frame is painted twice and must hash the same, and
 * the guard words must survive.  MANAGER_UI_FRAMES_DIR=<dir> writes each
 * frame as <label>.ppm (P6) for inspection.
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"
#include "manager_ui2_test_common.h"
#include "manager_ui2_fixture.h"

#include <stdlib.h>

#ifndef MANAGER_UI_RENDER_GOLDENS
#error "MANAGER_UI_RENDER_GOLDENS must name manager_ui_render_goldens.txt"
#endif

#define W 960
#define H 544
#define GUARD 1024u
#define GUARD_WORD 0x5a17c3e1u
#define MAX_FRAMES 96u

#define B_UP MANAGER_SETTINGS_UI_BUTTON_UP
#define B_DOWN MANAGER_SETTINGS_UI_BUTTON_DOWN
#define B_LEFT MANAGER_SETTINGS_UI_BUTTON_LEFT
#define B_RIGHT MANAGER_SETTINGS_UI_BUTTON_RIGHT
#define B_R MANAGER_SETTINGS_UI_BUTTON_RTRIGGER
#define B_CROSS MANAGER_SETTINGS_UI_BUTTON_CROSS
#define B_CIRCLE MANAGER_SETTINGS_UI_BUTTON_CIRCLE
#define B_SQUARE MANAGER_SETTINGS_UI_BUTTON_SQUARE
#define B_TRIANGLE MANAGER_SETTINGS_UI_BUTTON_TRIANGLE

static struct fake_fs fs;
static struct manager_storage_io io;
static struct ui2_ftp ftp;
static struct manager_theme_fonts imf_fonts;
static struct manager_theme_fonts builtin_fonts;
static struct manager_theme_set themes;
static struct manager_ui ui;
static struct manager_display_list dl;
static uint32_t buffer[GUARD + W * H + GUARD];
static uint32_t now = 5000u;

struct frame_hash {
    char label[64];
    uint32_t hash;
};
static struct frame_hash produced[MAX_FRAMES];
static unsigned int produced_count;

static void press(uint32_t buttons)
{
    (void)manager_ui_update(&ui, buttons, ++now);
    (void)manager_ui_update(&ui, 0u, ++now);
}

static void press_n(uint32_t buttons, unsigned int count)
{
    while (count-- != 0u)
        press(buttons);
}

static void goto_page(enum manager_ui_page page)
{
    unsigned int guard = 0u;
    while (ui.page != page && guard++ < 8u)
        press(B_R);
    require(ui.page == page, "reached the page");
}

static void start(const char *ini, const struct manager_theme_fonts *fonts,
                  int fonts_fallback)
{
    struct manager_ui_config config;
    struct manager_ui_host host;
    ui2_populate(&fs, 1);
    if (ini != NULL)
        fake_add_file(&fs, MANAGER_INI_PATH, ini);
    memset(&ftp, 0, sizeof(ftp));
    host = ui2_host(&ftp);
    memset(&config, 0, sizeof(config));
    config.app_version = "01.01";
    config.build_id = "int2l-k2a";
    config.system_lang = MANAGER_LANG_EN;
    config.recovery_result = MANAGER_STORAGE_OK;
    config.fonts_fallback = fonts_fallback;
    io = ui2_io(&fs, 1);
    manager_theme_set_init(&themes, fonts, NULL);
    manager_ui_init(&ui, &io, &host, &config, &themes, now);
}

static uint32_t paint(void)
{
    struct manager_canvas canvas;
    size_t index;
    for (index = 0u; index < GUARD; ++index) {
        buffer[index] = GUARD_WORD;
        buffer[GUARD + W * H + index] = GUARD_WORD;
    }
    for (index = 0u; index < (size_t)W * H; ++index)
        buffer[GUARD + index] = 0x01020304u;
    manager_canvas_init(&canvas, buffer + GUARD, W, H, W);
    manager_ui_draw(&dl, manager_ui_theme(&ui), &canvas);
    for (index = 0u; index < GUARD; ++index) {
        if (buffer[index] != GUARD_WORD ||
            buffer[GUARD + W * H + index] != GUARD_WORD)
            break;
    }
    require(index == GUARD, "guard words on both sides intact");
    for (index = 0u; index < (size_t)W * H; ++index) {
        if (buffer[GUARD + index] == 0x01020304u)
            break;
    }
    require(index == (size_t)W * H, "every pixel painted");
    return ui2_fnv1a(buffer + GUARD, (size_t)W * H);
}

static void write_ppm(const char *label)
{
    const char *directory = getenv("MANAGER_UI_FRAMES_DIR");
    static unsigned char row[W * 3];
    char path[1024];
    FILE *file;
    int y;
    if (directory == NULL || directory[0] == '\0')
        return;
    (void)snprintf(path, sizeof(path), "%s/%s.ppm", directory, label);
    file = fopen(path, "wb");
    require(file != NULL, "frame file opens");
    fprintf(file, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; ++y) {
        int x;
        for (x = 0; x < W; ++x) {
            const uint32_t word = buffer[GUARD + (size_t)y * W + (size_t)x];
            row[x * 3 + 0] = (unsigned char)(word & 0xffu);
            row[x * 3 + 1] = (unsigned char)((word >> 8) & 0xffu);
            row[x * 3 + 2] = (unsigned char)((word >> 16) & 0xffu);
        }
        require(fwrite(row, 1u, sizeof(row), file) == sizeof(row),
                "frame row written");
    }
    fclose(file);
}

static void frame(const char *label)
{
    uint32_t first;
    uint32_t second;
    manager_ui_build(&ui, &dl);
    require(!dl.overflow, "display list fits");
    first = paint();
    second = paint();
    require(first == second, "painting is deterministic");
    write_ppm(label);
    require(produced_count < MAX_FRAMES, "frame table capacity");
    (void)snprintf(produced[produced_count].label,
                   sizeof(produced[produced_count].label), "%s", label);
    produced[produced_count].hash = first;
    ++produced_count;
}

/* ---- the states --------------------------------------------------------- */

static const char *const theme_words[2] = {"dark", "light"};
static const char *const lang_words[2] = {"en", "ru"};

static void states(unsigned int theme, unsigned int lang)
{
    char ini[128];
    char label[64];
    const char *suffix;
    char tail[16];
    (void)snprintf(tail, sizeof(tail), "%s-%s", theme_words[theme],
                   lang_words[lang]);
    suffix = tail;
#define FRAME(name)                                                            \
    do {                                                                       \
        (void)snprintf(label, sizeof(label), "%s-%s", name, suffix);           \
        frame(label);                                                          \
    } while (0)

    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=%s\n", lang_words[lang],
                   theme_words[theme]);
    start(ini, &imf_fonts, 0);
    require(manager_ui_theme(&ui)->id ==
                (theme == 0u ? MANAGER_THEME_DARK : MANAGER_THEME_LIGHT),
            "theme from manager.ini");
    require(ui.lang == (lang == 0u ? MANAGER_LANG_EN : MANAGER_LANG_RU),
            "language from manager.ini");
    FRAME("01-home");
    press(B_DOWN);
    press(B_CROSS);
    FRAME("02-home-ftp-on");
    press(B_UP);
    press(B_CROSS);
    FRAME("03-box-launch-ftp");
    press(B_CIRCLE);
    press(B_DOWN);
    press(B_CROSS); /* FTP off again */
    goto_page(MANAGER_UI_PAGE_MODS);
    press_n(B_DOWN, 2u);
    FRAME("04-mods");
    press(B_TRIANGLE);
    FRAME("05-mods-detail");
    press(B_CIRCLE);
    press(B_SQUARE);
    FRAME("06-box-mods-all");
    press(B_CIRCLE);
    press_n(B_DOWN, 7u);
    FRAME("07-mods-scrolled");
    goto_page(MANAGER_UI_PAGE_SAVES);
    press(B_DOWN);
    FRAME("08-saves");
    press(B_CROSS);
    FRAME("09-box-restore");
    press(B_CIRCLE);
    press(B_TRIANGLE);
    FRAME("10-saves-all");
    goto_page(MANAGER_UI_PAGE_SETTINGS);
    FRAME("11-settings");
    while (ui.settings_row != MANAGER_UI_SET_AUTOLAUNCH)
        press(B_DOWN);
    press(B_RIGHT);
    FRAME("12-settings-pending");
    press(B_CIRCLE);
    FRAME("13-box-discard");
    press(B_CROSS);
    goto_page(MANAGER_UI_PAGE_ACHIEVEMENTS);
    FRAME("14-achievements");
    goto_page(MANAGER_UI_PAGE_CONTROLS);
    FRAME("15-controls");
    goto_page(MANAGER_UI_PAGE_ABOUT);
    FRAME("16-about");

    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=%s\nautolaunch=5\n",
                   lang_words[lang], theme_words[theme]);
    start(ini, &imf_fonts, 0);
    require(ui.countdown_active, "countdown armed");
    FRAME("17-home-countdown");
#undef FRAME
}

/* ---- goldens -------------------------------------------------------------- */

static void check_goldens(void)
{
    const char *update = getenv("MANAGER_UI_RENDER_UPDATE");
    unsigned int index;
    FILE *file;
    if (update != NULL && strcmp(update, "1") == 0) {
        file = fopen(MANAGER_UI_RENDER_GOLDENS, "wb");
        require(file != NULL, "goldens file writable");
        fprintf(file, "# manager_ui_render_host_test frame hashes (FNV-1a of "
                      "the 960x544 canvas words);\n# regenerate with "
                      "MANAGER_UI_RENDER_UPDATE=1 after a deliberate layout "
                      "change.\n");
        for (index = 0u; index < produced_count; ++index)
            fprintf(file, "%s %08x\n", produced[index].label,
                    produced[index].hash);
        fclose(file);
        printf("goldens rewritten: %u frames\n", produced_count);
        return;
    }
    file = fopen(MANAGER_UI_RENDER_GOLDENS, "rb");
    if (file == NULL) {
        fprintf(stderr, "no goldens at %s; run with MANAGER_UI_RENDER_UPDATE=1\n",
                MANAGER_UI_RENDER_GOLDENS);
        require(0, "goldens file present");
    }
    {
        char line[160];
        unsigned int matched = 0u;
        unsigned int mismatched = 0u;
        unsigned int listed = 0u;
        while (fgets(line, sizeof(line), file) != NULL) {
            char label[64];
            unsigned int hash;
            if (line[0] == '#' || line[0] == '\n')
                continue;
            if (sscanf(line, "%63s %x", label, &hash) != 2)
                continue;
            ++listed;
            for (index = 0u; index < produced_count; ++index) {
                if (strcmp(produced[index].label, label) != 0)
                    continue;
                if (produced[index].hash == (uint32_t)hash) {
                    ++matched;
                } else {
                    ++mismatched;
                    fprintf(stderr, "frame %s: %08x, golden %08x\n", label,
                            produced[index].hash, hash);
                }
                break;
            }
            if (index == produced_count) {
                ++mismatched;
                fprintf(stderr, "golden %s: frame not produced\n", label);
            }
        }
        fclose(file);
        require(mismatched == 0u, "every frame matches its golden");
        require(listed == produced_count && matched == produced_count,
                "every frame has a golden");
    }
}

int main(void)
{
    static struct manager_builtin_font builtin[MANAGER_FACE_COUNT] = {
        {3u}, {2u}, {2u}};
    unsigned int theme;
    unsigned int lang;
    unsigned int index;

    ui2_load_fonts(&imf_fonts);
    for (index = 0u; index < MANAGER_FACE_COUNT; ++index)
        builtin_fonts.face[index] = manager_builtin_face(&builtin[index]);

    for (theme = 0u; theme < 2u; ++theme)
        for (lang = 0u; lang < 2u; ++lang)
            states(theme, lang);

    /* theme=game with no skin in this build: light and a status line. */
    for (lang = 0u; lang < 2u; ++lang) {
        char ini[64];
        char label[64];
        (void)snprintf(ini, sizeof(ini), "lang=%s\n", lang_words[lang]);
        start(ini, &imf_fonts, 0);
        require(ui.ini_saved.theme == MANAGER_THEME_GAME, "game is the default");
        require(manager_ui_theme(&ui)->id == MANAGER_THEME_LIGHT &&
                    ui.status_tone == MANAGER_TONE_WARNING &&
                    strcmp(ui.status,
                           manager_str(ui.lang, MSTR_STATUS_THEME_NOT_BUILT)) ==
                        0,
                "game theme not built -> light + status");
        (void)snprintf(label, sizeof(label), "18-game-not-built-%s",
                       lang_words[lang]);
        frame(label);
    }
    /* The mods directory cannot be listed: error status, empty page. */
    start("lang=en\ntheme=light\n", &imf_fonts, 0);
    fake_fail(&fs, FAKE_OP_OPEN_DIR, 1);
    goto_page(MANAGER_UI_PAGE_MODS);
    fake_no_fail(&fs);
    require(ui.status_tone == MANAGER_TONE_ERROR && ui.mod_count == 0u,
            "listing failure shown");
    frame("21-mods-scan-failed-light-en");
    require_file(&fs, MANAGER_INI_PATH, "lang=en\ntheme=light\n",
                 "rendering never rewrites manager.ini");
    /* Font files missing: the built-in face, English whatever the ini says. */
    start("lang=ru\ntheme=light\n", &builtin_fonts, 1);
    require(ui.lang == MANAGER_LANG_EN, "fallback font forces English");
    frame("19-font-fallback-light-en");
    goto_page(MANAGER_UI_PAGE_SETTINGS);
    frame("20-font-fallback-settings-light-en");

    check_goldens();
    printf("manager ui render host test: %u frames, %u checks PASS\n",
           produced_count, checks);
    return 0;
}
