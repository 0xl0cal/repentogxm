/*
 * Manager UI2 string table host test (by design, "Strings: a test walks
 * both language tables through measure() and fails on any string wider than
 * its slot").  For every id, in English and Russian:
 *   - the text is non-empty, valid UTF-8, and every code point is in the
 *     committed atlases (the design mock-ups' GLYPHS set);
 *   - both columns carry the same printf directives in the same order;
 *   - with each directive replaced by a typical value the text fits its
 *     slot's width in the slot's face.
 * Plus: the seven tabs with their 18 px gaps fit between x=350 and 928.
 * Every failure is printed before the test fails, with the measured width.
 */
#include "manager_strings.h"
#include "manager_ui2_test_common.h"

#define DIRECTIVES_MAX 8u

/* Directive list of a format ("%u %s %04u" -> "u,s,04u"); 0 on a directive
 * the tables must not use. */
static int directives(const char *format, char *out, size_t capacity)
{
    size_t used = 0u;
    out[0] = '\0';
    while (*format != '\0') {
        const char *start;
        if (*format != '%') {
            ++format;
            continue;
        }
        ++format;
        if (*format == '%') {
            ++format;
            continue;
        }
        start = format;
        while (*format == '0' || (*format >= '1' && *format <= '9') ||
               *format == '-' || *format == '.')
            ++format;
        if (*format != 'u' && *format != 'd' && *format != 's')
            return 0;
        ++format;
        if (used + (size_t)(format - start) + 2u > capacity)
            return 0;
        if (used != 0u)
            out[used++] = ',';
        memcpy(out + used, start, (size_t)(format - start));
        used += (size_t)(format - start);
        out[used] = '\0';
    }
    return 1;
}

/* The format with each directive replaced by a typical value: %u 88, %d 3,
 * %04u 0003, %s a 12-glyph word (names are clipped with an ellipsis where
 * they are drawn; the typical value checks the fixed text around them). */
static void typical(const char *format, char *out, size_t capacity)
{
    size_t used = 0u;
    while (*format != '\0' && used + 16u < capacity) {
        if (format[0] == '%' && format[1] == '%') {
            out[used++] = '%';
            format += 2;
            continue;
        }
        if (format[0] == '%') {
            const char *value;
            ++format;
            if (strncmp(format, "04u", 3u) == 0) {
                value = "0003";
                format += 3;
            } else if (*format == 'u') {
                value = "88";
                ++format;
            } else if (*format == 'd') {
                value = "3";
                ++format;
            } else {
                value = "mmmmmmmmmmmm";
                ++format;
            }
            memcpy(out + used, value, strlen(value));
            used += strlen(value);
            continue;
        }
        out[used++] = *format++;
    }
    out[used] = '\0';
}

int main(void)
{
    static const char *const lang_names[MANAGER_LANG_COUNT] = {"en", "ru"};
    struct manager_theme_fonts fonts;
    unsigned int failures = 0u;
    int widest[MSLOT_COUNT];
    unsigned int id;
    unsigned int lang;
    memset(widest, 0, sizeof(widest));
    ui2_load_fonts(&fonts);

    for (id = 0u; id < (unsigned int)MSTR_COUNT; ++id) {
        const enum manager_string_slot slot =
            manager_str_slot((enum manager_string_id)id);
        const int face = manager_str_slot_face(slot);
        const int limit = manager_str_slot_width(slot);
        char first[64];
        require((unsigned int)slot < (unsigned int)MSLOT_COUNT, "slot valid");
        require(face >= 0 && face < (int)MANAGER_FACE_COUNT, "face valid");
        require(limit > 0, "slot width");
        for (lang = 0u; lang < MANAGER_LANG_COUNT; ++lang) {
            const char *text =
                manager_str((enum manager_lang)lang, (enum manager_string_id)id);
            const char *cursor = text;
            char list[64];
            char sample[512];
            uint32_t codepoint;
            int width;
            require(text != NULL && text[0] != '\0', "string present");
            while ((codepoint = manager_utf8_next(&cursor)) != 0u) {
                unsigned int f;
                if (codepoint == MANAGER_UTF8_REPLACEMENT) {
                    fprintf(stderr, "bad UTF-8: id %u %s: %s\n", id,
                            lang_names[lang], text);
                    ++failures;
                    break;
                }
                for (f = 0u; f < MANAGER_FACE_COUNT; ++f) {
                    if (!fonts.face[f].ops->has_glyph(fonts.face[f].impl,
                                                      codepoint)) {
                        fprintf(stderr,
                                "U+%04X not in %s: id %u %s: %s\n",
                                (unsigned int)codepoint, ui2_face_files[f], id,
                                lang_names[lang], text);
                        ++failures;
                    }
                }
            }
            if (!directives(text, list, sizeof(list))) {
                fprintf(stderr, "unsupported directive: id %u %s: %s\n", id,
                        lang_names[lang], text);
                ++failures;
                continue;
            }
            if (lang == 0u) {
                memcpy(first, list, sizeof(first));
            } else if (strcmp(first, list) != 0) {
                fprintf(stderr, "directives differ: id %u en [%s] ru [%s]: %s\n",
                        id, first, list, text);
                ++failures;
            }
            typical(text, sample, sizeof(sample));
            width = manager_face_measure(fonts.face[face], sample);
            if (width > widest[slot])
                widest[slot] = width;
            if (width > limit) {
                fprintf(stderr, "too wide: id %u %s slot %d: %d > %d px: %s\n",
                        id, lang_names[lang], (int)slot, width, limit, sample);
                ++failures;
            }
        }
    }

    for (lang = 0u; lang < MANAGER_LANG_COUNT; ++lang) {
        static const enum manager_string_id tabs[7] = {
            MSTR_TAB_HOME,         MSTR_TAB_MODS,     MSTR_TAB_SAVES,
            MSTR_TAB_SETTINGS,     MSTR_TAB_ACHIEVEMENTS,
            MSTR_TAB_CONTROLS,     MSTR_TAB_ABOUT};
        unsigned int index;
        int x = 350;
        for (index = 0u; index < 7u; ++index)
            x += manager_face_measure(
                     fonts.face[MANAGER_FACE_SMALL],
                     manager_str((enum manager_lang)lang, tabs[index])) +
                 (index + 1u < 7u ? 18 : 0);
        printf("tabs %s end at x=%d\n", lang_names[lang], x);
        if (x > 928) {
            fprintf(stderr, "tabs %s end at %d > 928\n", lang_names[lang], x);
            ++failures;
        }
    }

    for (id = 0u; id < (unsigned int)MSLOT_COUNT; ++id)
        printf("slot %u widest %d of %d px\n", id, widest[id],
               manager_str_slot_width((enum manager_string_slot)id));
    require(failures == 0u, "every string fits, shares directives, has glyphs");
    printf("manager strings host test: %u strings x %u languages, %u checks PASS\n",
           (unsigned int)MSTR_COUNT, (unsigned int)MANAGER_LANG_COUNT,
           checks);
    return 0;
}
