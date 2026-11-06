/*
 * manager.ini parse / serialize / load / save (see manager_ini.h).
 */
#if defined(MANAGER_UI2)

#include "manager_ini.h"

#include <stdio.h>
#include <string.h>

static const char *const manager_ini_lang_words[MANAGER_INI_LANG_COUNT] = {
    "auto", "en", "ru"};
static const char *const manager_ini_theme_words[MANAGER_THEME_COUNT] = {
    "game", "dark", "light"};
static const uint8_t manager_ini_autolaunch_values[4] = {0u, 5u, 10u, 30u};

void manager_ini_defaults(struct manager_ini *ini)
{
    memset(ini, 0, sizeof(*ini));
    ini->lang = MANAGER_INI_LANG_AUTO;
    ini->autolaunch = 0u;
    ini->theme = MANAGER_THEME_GAME;
}

int manager_ini_equal(const struct manager_ini *a, const struct manager_ini *b)
{
    return a->lang == b->lang && a->autolaunch == b->autolaunch &&
           a->theme == b->theme;
}

static int manager_ini_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* [begin, end) trimmed of spaces, tabs and CR. */
static void manager_ini_trim(const char **begin, const char **end)
{
    while (*begin < *end && manager_ini_is_space(**begin))
        ++*begin;
    while (*end > *begin && manager_ini_is_space((*end)[-1]))
        --*end;
}

static int manager_ini_word_is(const char *begin, const char *end,
                               const char *word)
{
    const size_t length = strlen(word);
    return (size_t)(end - begin) == length &&
           memcmp(begin, word, length) == 0;
}

/* 1 when the key/value pair was applied. */
static int manager_ini_apply(struct manager_ini *ini, const char *key,
                             const char *key_end, const char *value,
                             const char *value_end)
{
    unsigned int index;
    if (manager_ini_word_is(key, key_end, "lang")) {
        for (index = 0u; index < MANAGER_INI_LANG_COUNT; ++index) {
            if (manager_ini_word_is(value, value_end,
                                    manager_ini_lang_words[index])) {
                ini->lang = (uint8_t)index;
                return 1;
            }
        }
        return 0;
    }
    if (manager_ini_word_is(key, key_end, "theme")) {
        for (index = 0u; index < MANAGER_THEME_COUNT; ++index) {
            if (manager_ini_word_is(value, value_end,
                                    manager_ini_theme_words[index])) {
                ini->theme = (uint8_t)index;
                return 1;
            }
        }
        return 0;
    }
    if (manager_ini_word_is(key, key_end, "autolaunch")) {
        if (manager_ini_word_is(value, value_end, "off") ||
            manager_ini_word_is(value, value_end, "0")) {
            ini->autolaunch = 0u;
            return 1;
        }
        for (index = 1u; index < 4u; ++index) {
            char word[4];
            (void)snprintf(word, sizeof(word), "%u",
                           (unsigned int)manager_ini_autolaunch_values[index]);
            if (manager_ini_word_is(value, value_end, word)) {
                ini->autolaunch = manager_ini_autolaunch_values[index];
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

unsigned int manager_ini_parse(const char *text, size_t length,
                               struct manager_ini *ini)
{
    const char *cursor = text;
    const char *const end = text + length;
    unsigned int ignored = 0u;

    manager_ini_defaults(ini);
    if (text == NULL)
        return 0u;
    /* UTF-8 byte order mark written by some PC editors. */
    if (length >= 3u && (unsigned char)text[0] == 0xefu &&
        (unsigned char)text[1] == 0xbbu && (unsigned char)text[2] == 0xbfu)
        cursor += 3;
    while (cursor < end) {
        const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        const char *next;
        const char *begin = cursor;
        const char *equals;
        if (line_end == NULL)
            line_end = end;
        next = line_end < end ? line_end + 1 : end;
        manager_ini_trim(&begin, &line_end);
        cursor = next;
        if (begin == line_end || *begin == '#' || *begin == ';')
            continue;
        equals = memchr(begin, '=', (size_t)(line_end - begin));
        if (equals == NULL) {
            ++ignored;
            continue;
        }
        {
            const char *key = begin;
            const char *key_end = equals;
            const char *value = equals + 1;
            const char *value_end = line_end;
            manager_ini_trim(&key, &key_end);
            manager_ini_trim(&value, &value_end);
            if (!manager_ini_apply(ini, key, key_end, value, value_end))
                ++ignored;
        }
    }
    return ignored;
}

size_t manager_ini_serialize(const struct manager_ini *ini, char *out,
                             size_t capacity)
{
    char autolaunch[8];
    int written;
    const unsigned int lang =
        ini->lang < MANAGER_INI_LANG_COUNT ? ini->lang : MANAGER_INI_LANG_AUTO;
    const unsigned int theme =
        ini->theme < MANAGER_THEME_COUNT ? ini->theme : MANAGER_THEME_GAME;
    if (ini->autolaunch == 0u)
        (void)snprintf(autolaunch, sizeof(autolaunch), "off");
    else
        (void)snprintf(autolaunch, sizeof(autolaunch), "%u",
                       (unsigned int)ini->autolaunch);
    written = snprintf(out, capacity,
                       "# Isaac Vita manager settings (edited on the "
                       "manager's Settings page)\n"
                       "lang=%s\nautolaunch=%s\ntheme=%s\n",
                       manager_ini_lang_words[lang], autolaunch,
                       manager_ini_theme_words[theme]);
    if (written < 0 || (size_t)written >= capacity) {
        if (capacity != 0u)
            out[0] = '\0';
        return 0u;
    }
    return (size_t)written;
}

int manager_ini_load(const struct manager_storage_io *io,
                     struct manager_ini *ini, unsigned int *ignored)
{
    char text[MANAGER_INI_MAX_BYTES + 1u];
    size_t length = 0u;
    unsigned int skipped;
    int result;

    manager_ini_defaults(ini);
    if (ignored != NULL)
        *ignored = 0u;
    result = manager_storage_read_text(io, MANAGER_INI_PATH, text,
                                       sizeof(text), &length);
    if (result == MANAGER_STORAGE_ABSENT)
        return MANAGER_INI_ABSENT;
    if (result != MANAGER_STORAGE_OK)
        return MANAGER_INI_UNREADABLE;
    skipped = manager_ini_parse(text, length, ini);
    if (ignored != NULL)
        *ignored = skipped;
    return skipped != 0u ? MANAGER_INI_OK_WITH_IGNORED : MANAGER_INI_OK;
}

int manager_ini_save(const struct manager_storage_io *io,
                     const struct manager_ini *ini)
{
    char text[256];
    const size_t length = manager_ini_serialize(ini, text, sizeof(text));
    if (length == 0u)
        return MANAGER_STORAGE_INVALID;
    return manager_storage_write_text_atomic(io, MANAGER_INI_NAME, text,
                                             length);
}

uint8_t manager_ini_autolaunch_step(uint8_t current, int direction)
{
    unsigned int index = 0u;
    unsigned int position;
    for (position = 0u; position < 4u; ++position) {
        if (manager_ini_autolaunch_values[position] == current)
            index = position;
    }
    if (direction > 0 && index < 3u)
        ++index;
    else if (direction < 0 && index > 0u)
        --index;
    return manager_ini_autolaunch_values[index];
}

#endif /* MANAGER_UI2 */
