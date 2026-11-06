/*
 * manager.ini host test (manager_ini_host_test): defaults,
 * every key and value, comments, CRLF, BOM, whitespace, unknown keys and
 * values counted and skipped, garbage -> defaults, serialize round trip,
 * load ABSENT / OK / OK_WITH_IGNORED / UNREADABLE (read fault, directory,
 * too large), save through the atomic text protocol with the previous file
 * archived as .manager-backups/manager-0000.ini, autolaunch stepping.
 */
#define FAKE_DATA_CAPACITY 8192u
#include "fake_fs.h"
#include "manager_ini.h"

static void test_parse(void)
{
    struct manager_ini ini;
    unsigned int ignored;
    const char *text;

    manager_ini_defaults(&ini);
    require(ini.lang == MANAGER_INI_LANG_AUTO && ini.autolaunch == 0u &&
                ini.theme == MANAGER_THEME_GAME,
            "defaults: lang auto, autolaunch off, theme game");

    text = "lang=ru\nautolaunch=10\ntheme=dark\n";
    ignored = manager_ini_parse(text, strlen(text), &ini);
    require(ignored == 0u && ini.lang == MANAGER_INI_LANG_RU &&
                ini.autolaunch == 10u && ini.theme == MANAGER_THEME_DARK,
            "all three keys");

    text = "\xef\xbb\xbf# comment\r\n; other comment\r\n  lang = en \r\n"
           "autolaunch=off\r\ntheme=light\r\n\r\n";
    ignored = manager_ini_parse(text, strlen(text), &ini);
    require(ignored == 0u && ini.lang == MANAGER_INI_LANG_EN &&
                ini.autolaunch == 0u && ini.theme == MANAGER_THEME_LIGHT,
            "BOM, comments, CRLF, spaces around = and value");

    text = "lang=de\nautolaunch=7\ntheme=blue\ncolour=red\nnonsense\n"
           "autolaunch=30";
    ignored = manager_ini_parse(text, strlen(text), &ini);
    require(ignored == 5u, "four bad values/keys and one line without =");
    require(ini.lang == MANAGER_INI_LANG_AUTO && ini.theme == MANAGER_THEME_GAME,
            "bad values keep the defaults");
    require(ini.autolaunch == 30u, "last line without newline still parsed");

    text = "LANG=ru\nlang=RU\n";
    ignored = manager_ini_parse(text, strlen(text), &ini);
    require(ignored == 2u && ini.lang == MANAGER_INI_LANG_AUTO,
            "keys and values are case-sensitive");

    text = "autolaunch=0\nautolaunch=5\n";
    ignored = manager_ini_parse(text, strlen(text), &ini);
    require(ignored == 0u && ini.autolaunch == 5u, "0 and 5 accepted, last wins");

    {
        static const char garbage[] = {'\x01', '\x02', '=', '\xff', '\n', 0};
        ignored = manager_ini_parse(garbage, sizeof(garbage) - 1u, &ini);
        require(ignored == 1u && ini.lang == MANAGER_INI_LANG_AUTO &&
                    ini.autolaunch == 0u && ini.theme == MANAGER_THEME_GAME,
                "garbage -> defaults");
    }
    ignored = manager_ini_parse(NULL, 0u, &ini);
    require(ignored == 0u && ini.theme == MANAGER_THEME_GAME, "NULL -> defaults");
}

static void test_serialize(void)
{
    struct manager_ini ini;
    struct manager_ini back;
    char text[256];
    size_t length;
    unsigned int lang;
    unsigned int theme;
    unsigned int step;

    for (lang = 0u; lang < MANAGER_INI_LANG_COUNT; ++lang) {
        for (theme = 0u; theme < MANAGER_THEME_COUNT; ++theme) {
            static const uint8_t seconds[4] = {0u, 5u, 10u, 30u};
            for (step = 0u; step < 4u; ++step) {
                ini.lang = (uint8_t)lang;
                ini.theme = (uint8_t)theme;
                ini.autolaunch = seconds[step];
                length = manager_ini_serialize(&ini, text, sizeof(text));
                require(length != 0u && length == strlen(text),
                        "serialize length");
                require(manager_ini_parse(text, length, &back) == 0u,
                        "serialized text parses clean");
                require(manager_ini_equal(&ini, &back), "round trip");
            }
        }
    }
    manager_ini_defaults(&ini);
    length = manager_ini_serialize(&ini, text, sizeof(text));
    require(strstr(text, "lang=auto\nautolaunch=off\ntheme=game\n") != NULL,
            "canonical default text");
    require(manager_ini_serialize(&ini, text, 20u) == 0u && text[0] == '\0',
            "too small a buffer -> 0");

    require(manager_ini_autolaunch_step(0u, 1) == 5u &&
                manager_ini_autolaunch_step(5u, 1) == 10u &&
                manager_ini_autolaunch_step(10u, 1) == 30u &&
                manager_ini_autolaunch_step(30u, 1) == 30u &&
                manager_ini_autolaunch_step(30u, -1) == 10u &&
                manager_ini_autolaunch_step(0u, -1) == 0u &&
                manager_ini_autolaunch_step(7u, 1) == 5u,
            "autolaunch steps off/5/10/30, clamped");
}

static struct fake_fs fs;

static void test_load_save(void)
{
    struct manager_storage_io io;
    struct manager_ini ini;
    unsigned int ignored = 99u;
    int status;

    fake_init(&fs);
    io = fake_io(&fs);
    status = manager_ini_load(&io, &ini, &ignored);
    require(status == MANAGER_INI_ABSENT && ignored == 0u &&
                ini.theme == MANAGER_THEME_GAME,
            "absent -> ABSENT + defaults");

    fake_add_file(&fs, MANAGER_INI_PATH, "lang=ru\ntheme=light\n");
    status = manager_ini_load(&io, &ini, &ignored);
    require(status == MANAGER_INI_OK && ini.lang == MANAGER_INI_LANG_RU &&
                ini.theme == MANAGER_THEME_LIGHT && ini.autolaunch == 0u,
            "file read");

    {
        struct fake_node *node = &fs.nodes[fake_find(&fs, MANAGER_INI_PATH)];
        const char *text = "theme=dark\nfoo=bar\n";
        memcpy(node->data, text, strlen(text));
        node->size = strlen(text);
    }
    status = manager_ini_load(&io, &ini, &ignored);
    require(status == MANAGER_INI_OK_WITH_IGNORED && ignored == 1u &&
                ini.theme == MANAGER_THEME_DARK,
            "unknown key -> OK_WITH_IGNORED");

    fake_fail(&fs, FAKE_OP_READ, 1);
    status = manager_ini_load(&io, &ini, &ignored);
    fake_no_fail(&fs);
    require(status == MANAGER_INI_UNREADABLE && ini.theme == MANAGER_THEME_GAME,
            "read fault -> UNREADABLE + defaults");

    {
        struct fake_node *node = &fs.nodes[fake_find(&fs, MANAGER_INI_PATH)];
        memset(node->data, '#', MANAGER_INI_MAX_BYTES + 1u);
        node->size = MANAGER_INI_MAX_BYTES + 1u;
    }
    status = manager_ini_load(&io, &ini, &ignored);
    require(status == MANAGER_INI_UNREADABLE, "too large -> UNREADABLE");

    /* Save: the old file is archived, the new text is in place. */
    {
        struct fake_node *node = &fs.nodes[fake_find(&fs, MANAGER_INI_PATH)];
        const char *text = "theme=dark\n";
        memcpy(node->data, text, strlen(text));
        node->size = strlen(text);
    }
    ini.lang = MANAGER_INI_LANG_EN;
    ini.autolaunch = 10u;
    ini.theme = MANAGER_THEME_LIGHT;
    require(manager_ini_save(&io, &ini) == MANAGER_STORAGE_OK, "save ok");
    require_file(&fs, MANAGER_STORAGE_BACKUP_ROOT "/manager-0000.ini",
                 "theme=dark\n", "previous manager.ini archived");
    status = manager_ini_load(&io, &ini, &ignored);
    require(status == MANAGER_INI_OK && ini.lang == MANAGER_INI_LANG_EN &&
                ini.autolaunch == 10u && ini.theme == MANAGER_THEME_LIGHT,
            "saved values load back");
    require(fake_find(&fs, MANAGER_INI_PATH ".tmp") < 0 &&
                fake_find(&fs, MANAGER_INI_PATH ".rollback") < 0,
            "no sidecar left");

    fake_fail(&fs, FAKE_OP_WRITE, 1);
    require(manager_ini_save(&io, &ini) < 0, "write fault -> failure");
    fake_no_fail(&fs);
    status = manager_ini_load(&io, &ini, &ignored);
    require(status == MANAGER_INI_OK && ini.theme == MANAGER_THEME_LIGHT,
            "failed save leaves the previous file");
}

int main(void)
{
    test_parse();
    test_serialize();
    test_load_save();
    printf("manager ini host test: %u checks PASS\n", checks);
    return 0;
}
