/*
 * Manager UI2 trophy import row (knob ISAAC_VITA_MANAGER_TROPHY_IMPORT ->
 * MANAGER_TROPHY_IMPORT=1) over the fake FS:
 *
 *   - the request parser follows the game runtime's rule (one digit 1..3,
 *     space/tab/CR/LF around it, at most 16 bytes) and the result parser
 *     reads the runtime's "TROPHY IMPORT: slot=..." line, refusing anything
 *     else (see recomp/vita/TROPHIES.md);
 *   - ACHIEVEMENTS: ←/→ pick File 1..3 (clamped), × writes the digit into
 *     ux0:/data/isaacr001/trophy-import.req, again on a repeat; the page shows
 *     the waiting request and the last result (absent, every status the
 *     runtime writes, garbage, too long), never a dialog;
 *   - FTP on refuses the write with the read-only status; a failing write
 *     says so and leaves no request;
 *   - every list fits (no overflow, hint groups apart) in English and
 *     Russian, and the page painted in light/dark x en/ru matches
 *     manager_ui_trophy_goldens.txt (MANAGER_UI_TROPHY_UPDATE=1 rewrites
 *     it, MANAGER_UI_TROPHY_FRAMES_DIR=<dir> writes .ppm frames).
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"
#include "manager_ui2_test_common.h"
#include "manager_ui2_fixture.h"

#include <stdlib.h>

#ifndef MANAGER_UI_TROPHY_GOLDENS
#error "MANAGER_UI_TROPHY_GOLDENS must name manager_ui_trophy_goldens.txt"
#endif

#define W 960
#define H 544
#define MAX_FRAMES 16u

static struct fake_fs fs;
static struct manager_storage_io io;
static struct ui2_ftp ftp;
static struct manager_theme_fonts fonts;
static struct manager_theme_set themes;
static struct manager_ui ui;
static struct manager_display_list dl;
static char dump[65536];
static uint32_t now = 1000u;
static uint32_t canvas_words[W * H];

static const char done_line[] =
    "TROPHY IMPORT: slot=1 status=done saved=28 mapped=27 unlocked=27 "
    "already=1 failed=0 noset=0 request=deleted\n";

/* ---- parsers ------------------------------------------------------------ */

static void test_request_parser(void)
{
    static const struct {
        const char *text;
        size_t length;
        int slot;
    } cases[] = {
        {"1", 1u, 1},           {"3\n", 2u, 3},         {" \t2\r\n", 5u, 2},
        {"", 0u, 0},            {"4", 1u, 0},           {"0", 1u, 0},
        {"12", 2u, 0},          {"1 1", 3u, 0},         {"a", 1u, 0},
        {"1\0", 2u, 0},         {"\n\n\n", 3u, 0},      {"+1", 2u, 0},
        {"               1", 16u, 1},                   /* 16 bytes */
        {"                1", 17u, 0},                  /* 17 bytes */
    };
    unsigned int index;
    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const int slot = manager_ui_trophy_parse_request(cases[index].text,
                                                         cases[index].length);
        if (slot != cases[index].slot)
            fprintf(stderr, "request case %u: %d, expected %d\n", index, slot,
                    cases[index].slot);
        require(slot == cases[index].slot, "request parse");
    }
    require(manager_ui_trophy_parse_request(NULL, 0u) == 0, "NULL request");
}

static void test_result_parser(void)
{
    static const char *const refused[] = {
        "",
        "garbage\n",
        "KAGE VITA TROPHY IMPORT: slot=1 status=done\n", /* the log line */
        "TROPHY IMPORT: status=done unlocked=1\n",       /* no slot */
        "TROPHY IMPORT: slot=1 unlocked=1\n",            /* no status */
        "TROPHY IMPORT: slot=4 status=done\n",
        "TROPHY IMPORT: slot=-1 status=done\n",
        "TROPHY IMPORT: slot=1 status=Done\n",
        "TROPHY IMPORT: slot=1 status=done unlocked=123456\n",
        "TROPHY IMPORT: slot=1 status=done unlocked=\n",
        "TROPHY IMPORT: slot=1 status=done request=maybe\n",
        "TROPHY IMPORT: slot=1 status=done noequals\n",
        "TROPHY IMPORT: slot=1 status=averyveryverylongstatus\n",
    };
    struct manager_trophy_result result;
    unsigned int index;
    require(manager_ui_trophy_parse_result(done_line, &result) == 1 &&
                result.slot == 1u && strcmp(result.status, "done") == 0 &&
                result.saved == 28u && result.mapped == 27u &&
                result.unlocked == 27u && result.already == 1u &&
                result.failed == 0u && result.noset == 0u &&
                result.request_kept == 0,
            "runtime result line parsed");
    require(manager_ui_trophy_parse_result(
                "TROPHY IMPORT: slot=0 status=bad-request saved=0 mapped=0 "
                "unlocked=0 already=0 failed=0 noset=0 request=deleted",
                &result) == 1 &&
                result.slot == 0u && strcmp(result.status, "bad-request") == 0,
            "bad-request line (slot 0, no newline)");
    require(manager_ui_trophy_parse_result(
                "TROPHY IMPORT: slot=2 status=partial saved=30 mapped=29 "
                "unlocked=20 already=6 failed=3 noset=0 request=kept "
                "future=7\r\n",
                &result) == 1 &&
                result.request_kept == 1 && result.failed == 3u,
            "partial line, unknown key ignored, CRLF");
    for (index = 0u; index < sizeof(refused) / sizeof(refused[0]); ++index) {
        const int parsed = manager_ui_trophy_parse_result(refused[index], &result);
        if (parsed)
            fprintf(stderr, "result case %u parsed: %s", index, refused[index]);
        require(!parsed, "malformed result refused");
        require(result.slot == 0u && result.status[0] == '\0',
                "refused result zeroed");
    }
    require(!manager_ui_trophy_parse_result(NULL, &result), "NULL result text");
    require(!manager_ui_trophy_parse_result(done_line, NULL), "NULL result");
}

/* ---- page ------------------------------------------------------------------ */

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
    manager_ui_build(&ui, &dl);
    (void)manager_dl_dump(&dl, dump, sizeof(dump));
    check_fit(label);
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

static void start(const char *ini)
{
    struct manager_ui_config config;
    struct manager_ui_host host;
    ui2_populate(&fs, 1);
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

static void goto_achievements(void)
{
    unsigned int guard = 0u;
    while (ui.page != MANAGER_UI_PAGE_ACHIEVEMENTS && guard++ < 8u)
        press(MANAGER_SETTINGS_UI_BUTTON_RTRIGGER);
    require(ui.page == MANAGER_UI_PAGE_ACHIEVEMENTS, "reached ACHIEVEMENTS");
}

/* Leave and come back so the page re-reads the files. */
static void reopen(void)
{
    press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    require(ui.page == MANAGER_UI_PAGE_HOME, "O goes home");
    goto_achievements();
}

static void put_file(const char *path, const char *text)
{
    (void)fake_remove(&fs, path);
    fake_add_file(&fs, path, text);
}

static int request_is(const char *expected)
{
    char buffer[64];
    size_t length = 0u;
    if (manager_storage_read_text(&io, MANAGER_TROPHY_REQUEST_PATH, buffer,
                                  sizeof(buffer), &length) != MANAGER_STORAGE_OK)
        return expected == NULL;
    return expected != NULL && length == strlen(expected) &&
           memcmp(buffer, expected, length) == 0;
}

static void test_page(const char *lang)
{
    char ini[64];
    char line[256];
    static const struct {
        const char *file;
        const char *en;
        const char *ru;
    } results[] = {
        {"TROPHY IMPORT: slot=2 status=partial saved=30 mapped=29 unlocked=20 "
         "already=6 failed=3 noset=0 request=kept\n",
         "Last import: File 2 — partial, retried at next start · unlocked 20, "
         "already 6, failed 3",
         "Последний импорт: файл 2 — частично, повтор при запуске · выдано 20, "
         "уже были 6, ошибок 3"},
        {"TROPHY IMPORT: slot=1 status=unavailable saved=28 mapped=27 "
         "unlocked=0 already=0 failed=0 noset=0 request=kept\n",
         "Last import: File 1 — trophies off in that build",
         "Последний импорт: файл 1 — трофеи в той сборке выключены"},
        {"TROPHY IMPORT: slot=3 status=no-save saved=0 mapped=0 unlocked=0 "
         "already=0 failed=0 noset=0 request=deleted\n",
         "Last import: File 3 — no save in that slot",
         "Последний импорт: файл 3 — в слоте нет сейва"},
        {"TROPHY IMPORT: slot=1 status=bad-checksum saved=0 mapped=0 "
         "unlocked=0 already=0 failed=0 noset=0 request=deleted\n",
         "Last import: File 1 — save not readable (bad-checksum)",
         "Последний импорт: файл 1 — сейв не читается (bad-checksum)"},
        {"TROPHY IMPORT: slot=0 status=bad-request saved=0 mapped=0 "
         "unlocked=0 already=0 failed=0 noset=0 request=deleted\n",
         "Last import: File 0 — request file not valid",
         "Последний импорт: файл 0 — файл запроса неверный"},
    };
    const int ru = strcmp(lang, "ru") == 0;
    unsigned int index;
    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=light\n", lang);
    start(ini);
    goto_achievements();
    build("trophy-empty");
    require(ui.trophy_slot == 1 && ui.trophy_pending == 0 &&
                ui.trophy_result_state == 0,
            "fresh page: File 1, nothing waiting, no result");
    require(shows(ru ? "Выдать трофеи из сейва" : "Grant trophies from save") &&
                shows(ru ? "Файл 1" : "File 1") &&
                shows(ru ? "Результата импорта пока нет" : "No import result yet") &&
                shows("hint r × "),
            "row, value, no-result line and × hint shown");
    require(request_is(NULL), "no request yet");

    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
    require(ui.trophy_slot == 3, "→ stops at File 3");
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    require(ui.trophy_slot == 2, "← back to File 2");
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require_file(&fs, MANAGER_TROPHY_REQUEST_PATH, "2",
                 "× writes exactly the digit 2");
    require(request_is("2"), "× writes the digit 2");
    require(ui.status_tone == MANAGER_TONE_OK && ui.trophy_pending == 2,
            "request written, shown as waiting");
    build("trophy-requested");
    (void)snprintf(line, sizeof(line),
                   manager_str(ui.lang, MSTR_TROPHY_PENDING), 2);
    require(shows(line), "waiting line shown");
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    press(MANAGER_SETTINGS_UI_BUTTON_LEFT);
    require(ui.trophy_slot == 1, "← stops at File 1");
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require(request_is("1") && ui.trophy_pending == 1,
            "a repeated request replaces the waiting one");

    /* The game consumed it and left a result. */
    (void)fake_remove(&fs, MANAGER_TROPHY_REQUEST_PATH);
    put_file(MANAGER_TROPHY_RESULT_PATH, done_line);
    reopen();
    build("trophy-done");
    require(ui.trophy_pending == 0 && ui.trophy_result_state == 1,
            "result read, nothing waiting");
    require(shows(ru ? "Последний импорт: файл 1 — готово · выдано 27, уже были "
                       "1, ошибок 0"
                     : "Last import: File 1 — done · unlocked 27, already 1, "
                       "failed 0"),
            "done line shown");
    for (index = 0u; index < sizeof(results) / sizeof(results[0]); ++index) {
        put_file(MANAGER_TROPHY_RESULT_PATH, results[index].file);
        reopen();
        build("trophy-result");
        if (!shows(ru ? results[index].ru : results[index].en))
            fprintf(stderr, "result %u not shown:\n%s", index, dump);
        require(shows(ru ? results[index].ru : results[index].en),
                "every runtime status worded");
    }
    put_file(MANAGER_TROPHY_RESULT_PATH, "not a result line\n");
    reopen();
    build("trophy-garbage");
    require(ui.trophy_result_state == -1 &&
                shows(ru ? "trophy-import.res не читается"
                         : "trophy-import.res could not be read"),
            "garbage result shown as unreadable");
    {
        char big[400];
        memset(big, 'x', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = '\0';
        put_file(MANAGER_TROPHY_RESULT_PATH, big);
        reopen();
        require(ui.trophy_result_state == -1, "oversized result unreadable");
    }
    put_file(MANAGER_TROPHY_REQUEST_PATH, "seven");
    reopen();
    build("trophy-bad-request");
    require(ui.trophy_pending == -1 &&
                shows(ru ? "trophy-import.req ждёт запуска, но это не номер "
                           "слота 1-3"
                         : "trophy-import.req is waiting but is not a slot "
                           "number 1-3"),
            "unreadable request shown");

    /* A failing write leaves the old request and says why. */
    put_file(MANAGER_TROPHY_REQUEST_PATH, "3");
    fake_fail(&fs, FAKE_OP_OPEN_WRITE, 1);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    fake_no_fail(&fs);
    require(ui.status_tone == MANAGER_TONE_ERROR && request_is("3"),
            "failed write reported, request untouched");
    build("trophy-write-failed");

    /* FTP on: read-only. */
    press(MANAGER_SETTINGS_UI_BUTTON_CIRCLE);
    press(MANAGER_SETTINGS_UI_BUTTON_DOWN);
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require(ui.ftp_on, "FTP on");
    goto_achievements();
    require(ui.status_tone == MANAGER_TONE_WARNING &&
                strcmp(ui.status, manager_str(ui.lang, MSTR_STATUS_READONLY)) ==
                    0,
            "the page opens read-only while FTP listens");
    press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
    require(request_is("3") &&
                strcmp(ui.status, manager_str(ui.lang, MSTR_STATUS_READONLY)) ==
                    0,
            "× refused while FTP listens");
}

/* ---- frames ----------------------------------------------------------------- */

struct frame_hash {
    char label[64];
    uint32_t hash;
};
static struct frame_hash produced[MAX_FRAMES];
static unsigned int produced_count;

static void frame(const char *label)
{
    const char *directory = getenv("MANAGER_UI_TROPHY_FRAMES_DIR");
    struct manager_canvas canvas;
    size_t index;
    build(label);
    for (index = 0u; index < (size_t)W * H; ++index)
        canvas_words[index] = 0x01020304u;
    manager_canvas_init(&canvas, canvas_words, W, H, W);
    manager_ui_draw(&dl, manager_ui_theme(&ui), &canvas);
    for (index = 0u; index < (size_t)W * H; ++index)
        if (canvas_words[index] == 0x01020304u)
            break;
    require(index == (size_t)W * H, "every pixel painted");
    require(produced_count < MAX_FRAMES, "frame table capacity");
    (void)snprintf(produced[produced_count].label,
                   sizeof(produced[produced_count].label), "%s", label);
    produced[produced_count].hash = ui2_fnv1a(canvas_words, (size_t)W * H);
    ++produced_count;
    if (directory != NULL && directory[0] != '\0') {
        char path[1024];
        FILE *file;
        int i;
        (void)snprintf(path, sizeof(path), "%s/%s.ppm", directory, label);
        file = fopen(path, "wb");
        require(file != NULL, "frame file opens");
        fprintf(file, "P6\n%d %d\n255\n", W, H);
        for (i = 0; i < W * H; ++i) {
            unsigned char rgb[3];
            rgb[0] = (unsigned char)(canvas_words[i] & 0xffu);
            rgb[1] = (unsigned char)((canvas_words[i] >> 8) & 0xffu);
            rgb[2] = (unsigned char)((canvas_words[i] >> 16) & 0xffu);
            require(fwrite(rgb, 1u, 3u, file) == 3u, "frame pixel written");
        }
        fclose(file);
    }
}

static void frames(void)
{
    static const char *const themes_words[2] = {"light", "dark"};
    static const char *const langs[2] = {"en", "ru"};
    unsigned int theme;
    unsigned int lang;
    for (theme = 0u; theme < 2u; ++theme) {
        for (lang = 0u; lang < 2u; ++lang) {
            char ini[64];
            char label[64];
            (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=%s\n", langs[lang],
                           themes_words[theme]);
            start(ini);
            fake_add_file(&fs, MANAGER_TROPHY_RESULT_PATH, done_line);
            goto_achievements();
            press(MANAGER_SETTINGS_UI_BUTTON_RIGHT);
            press(MANAGER_SETTINGS_UI_BUTTON_CROSS);
            (void)snprintf(label, sizeof(label), "achievements-trophy-%s-%s",
                           themes_words[theme], langs[lang]);
            frame(label);
        }
    }
}

static void check_goldens(void)
{
    const char *update = getenv("MANAGER_UI_TROPHY_UPDATE");
    unsigned int index;
    unsigned int matched = 0u;
    unsigned int listed = 0u;
    char line[160];
    FILE *file;
    if (update != NULL && strcmp(update, "1") == 0) {
        file = fopen(MANAGER_UI_TROPHY_GOLDENS, "wb");
        require(file != NULL, "goldens file writable");
        fprintf(file, "# manager_ui_trophy_host_test frame hashes (FNV-1a of the "
                      "960x544 canvas words);\n# regenerate with "
                      "MANAGER_UI_TROPHY_UPDATE=1 after a deliberate change.\n");
        for (index = 0u; index < produced_count; ++index)
            fprintf(file, "%s %08x\n", produced[index].label,
                    produced[index].hash);
        fclose(file);
        printf("goldens rewritten: %u frames\n", produced_count);
        return;
    }
    file = fopen(MANAGER_UI_TROPHY_GOLDENS, "rb");
    require(file != NULL, "goldens file present");
    while (fgets(line, sizeof(line), file) != NULL) {
        char label[64];
        unsigned int hash;
        if (line[0] == '#' || sscanf(line, "%63s %x", label, &hash) != 2)
            continue;
        ++listed;
        for (index = 0u; index < produced_count; ++index) {
            if (strcmp(produced[index].label, label) != 0)
                continue;
            if (produced[index].hash == (uint32_t)hash)
                ++matched;
            else
                fprintf(stderr, "frame %s: %08x, golden %08x\n", label,
                        produced[index].hash, hash);
            break;
        }
    }
    fclose(file);
    require(listed == produced_count && matched == produced_count,
            "every frame matches its golden");
}

int main(void)
{
    ui2_load_fonts(&fonts);
    test_request_parser();
    test_result_parser();
    test_page("en");
    test_page("ru");
    frames();
    check_goldens();
    printf("manager ui trophy host test: %u frames, %u checks PASS\n",
           produced_count, checks);
    return 0;
}
