/*
 * metadata.xml name / version scan (see manager_mods_meta.h).
 */
#if defined(MANAGER_UI2)

#include "manager_mods_meta.h"

#include <stdio.h>
#include <string.h>

/* Element text between <tag> (or <tag attributes>) and </tag>; returns 0
 * when the element is absent, empty-tagged or unterminated in the head. */
static int manager_mods_meta_element(const char *xml, size_t length,
                                     const char *tag, const char **begin,
                                     const char **end)
{
    const size_t tag_length = strlen(tag);
    const char *cursor = xml;
    const char *const limit = xml + length;
    while (cursor < limit) {
        const char *open = memchr(cursor, '<', (size_t)(limit - cursor));
        const char *after;
        const char *close;
        if (open == NULL)
            return 0;
        after = open + 1;
        cursor = after;
        if ((size_t)(limit - after) < tag_length + 1u ||
            memcmp(after, tag, tag_length) != 0)
            continue;
        after += tag_length;
        if (*after != '>' && *after != ' ' && *after != '\t' &&
            *after != '\r' && *after != '\n')
            continue;
        after = memchr(after, '>', (size_t)(limit - after));
        if (after == NULL)
            return 0;
        if (after[-1] == '/')
            continue; /* <name/> */
        ++after;
        /* The matching end tag. */
        close = after;
        while (close < limit) {
            const char *candidate =
                memchr(close, '<', (size_t)(limit - close));
            if (candidate == NULL)
                return 0;
            if ((size_t)(limit - candidate) >= tag_length + 3u &&
                candidate[1] == '/' &&
                memcmp(candidate + 2, tag, tag_length) == 0 &&
                candidate[2 + tag_length] == '>') {
                *begin = after;
                *end = candidate;
                return 1;
            }
            close = candidate + 1;
        }
        return 0;
    }
    return 0;
}

/* UTF-8 of a code point; returns the byte count (0 for invalid). */
static size_t manager_mods_meta_utf8(uint32_t codepoint, char out[4])
{
    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1u;
    }
    if (codepoint < 0x800u) {
        out[0] = (char)(0xc0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3fu));
        return 2u;
    }
    if (codepoint >= 0xd800u && codepoint <= 0xdfffu)
        return 0u;
    if (codepoint < 0x10000u) {
        out[0] = (char)(0xe0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (codepoint & 0x3fu));
        return 3u;
    }
    if (codepoint < 0x110000u) {
        out[0] = (char)(0xf0u | (codepoint >> 18));
        out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[3] = (char)(0x80u | (codepoint & 0x3fu));
        return 4u;
    }
    return 0u;
}

/* Entity at *cursor ('&' ... ';'); on success writes the decoded bytes and
 * advances past ';'.  Unknown or malformed entities decode to nothing. */
static size_t manager_mods_meta_entity(const char **cursor, const char *end,
                                       char out[4])
{
    static const struct {
        const char *name;
        char value;
    } named[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'},
                 {"quot", '"'}, {"apos", '\''}};
    const char *start = *cursor + 1;
    const char *semicolon = start;
    size_t length;
    unsigned int index;
    while (semicolon < end && semicolon - start < 12 && *semicolon != ';')
        ++semicolon;
    if (semicolon >= end || *semicolon != ';') {
        *cursor += 1; /* a bare '&': dropped */
        return 0u;
    }
    *cursor = semicolon + 1;
    length = (size_t)(semicolon - start);
    for (index = 0u; index < sizeof(named) / sizeof(named[0]); ++index) {
        if (strlen(named[index].name) == length &&
            memcmp(start, named[index].name, length) == 0) {
            out[0] = named[index].value;
            return 1u;
        }
    }
    if (length >= 2u && start[0] == '#') {
        uint32_t codepoint = 0u;
        const char *digit = start + 1;
        const int hex = *digit == 'x' || *digit == 'X';
        if (hex)
            ++digit;
        if (digit == semicolon)
            return 0u;
        for (; digit < semicolon; ++digit) {
            unsigned int value;
            if (*digit >= '0' && *digit <= '9')
                value = (unsigned int)(*digit - '0');
            else if (hex && *digit >= 'a' && *digit <= 'f')
                value = (unsigned int)(*digit - 'a' + 10);
            else if (hex && *digit >= 'A' && *digit <= 'F')
                value = (unsigned int)(*digit - 'A' + 10);
            else
                return 0u;
            codepoint = codepoint * (hex ? 16u : 10u) + value;
            if (codepoint > 0x10ffffu)
                return 0u;
        }
        return manager_mods_meta_utf8(codepoint, out);
    }
    return 0u;
}

static int manager_mods_meta_is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/* Decodes, collapses whitespace, drops control characters and cuts at
 * max_chars code points or the capacity (at a code point boundary). */
static void manager_mods_meta_clean(const char *begin, const char *end,
                                    char *out, size_t capacity,
                                    size_t max_chars)
{
    size_t used = 0u;
    size_t chars = 0u;
    int pending_space = 0;
    const char *cursor = begin;
    out[0] = '\0';
    while (cursor < end && chars < max_chars) {
        char piece[4];
        size_t piece_length;
        unsigned char lead = (unsigned char)*cursor;
        if (lead == '&') {
            piece_length = manager_mods_meta_entity(&cursor, end, piece);
            if (piece_length == 0u)
                continue;
            lead = (unsigned char)piece[0];
        } else if (lead == '<') {
            /* Markup inside the element (a nested tag): skip it. */
            const char *close = memchr(cursor, '>', (size_t)(end - cursor));
            cursor = close != NULL ? close + 1 : end;
            continue;
        } else {
            size_t expected = 1u;
            if (lead >= 0xf0u)
                expected = 4u;
            else if (lead >= 0xe0u)
                expected = 3u;
            else if (lead >= 0xc0u)
                expected = 2u;
            if ((size_t)(end - cursor) < expected)
                break;
            memcpy(piece, cursor, expected);
            piece_length = expected;
            cursor += expected;
        }
        if (piece_length == 1u && manager_mods_meta_is_space(lead)) {
            pending_space = used != 0u;
            continue;
        }
        if (piece_length == 1u && (lead < 0x20u || lead == 0x7fu))
            continue;
        if (pending_space) {
            if (used + 1u >= capacity || chars + 1u >= max_chars)
                break;
            out[used++] = ' ';
            ++chars;
            pending_space = 0;
        }
        if (used + piece_length >= capacity)
            break;
        memcpy(out + used, piece, piece_length);
        used += piece_length;
        ++chars;
    }
    out[used] = '\0';
}

int manager_mods_meta_scan(const char *xml, size_t length,
                           struct manager_mod_meta *meta)
{
    const char *begin;
    const char *end;
    meta->name[0] = '\0';
    meta->version[0] = '\0';
    meta->from_metadata = 0u;
    if (xml == NULL)
        return 0;
    if (manager_mods_meta_element(xml, length, "name", &begin, &end))
        manager_mods_meta_clean(begin, end, meta->name, sizeof(meta->name),
                                MANAGER_MODS_META_NAME_CHARS);
    if (meta->name[0] == '\0' &&
        manager_mods_meta_element(xml, length, "title", &begin, &end))
        manager_mods_meta_clean(begin, end, meta->name, sizeof(meta->name),
                                MANAGER_MODS_META_NAME_CHARS);
    if (manager_mods_meta_element(xml, length, "version", &begin, &end))
        manager_mods_meta_clean(begin, end, meta->version,
                                sizeof(meta->version),
                                MANAGER_MODS_META_VERSION_CHARS);
    meta->from_metadata = meta->name[0] != '\0';
    return meta->from_metadata;
}

int manager_mods_meta_read(const struct manager_storage_io *io,
                           const char *dir, struct manager_mod_meta *meta)
{
    char path[MANAGER_STORAGE_PATH_CAPACITY];
    char head[MANAGER_MODS_META_HEAD_BYTES + 1u];
    size_t length = 0u;
    int truncated = 0;
    int result;
    int written;

    meta->name[0] = '\0';
    meta->version[0] = '\0';
    meta->from_metadata = 0u;
    written = snprintf(path, sizeof(path), "%s/%s/metadata.xml",
                       MANAGER_STORAGE_MOD_ROOT, dir != NULL ? dir : "");
    if (dir == NULL || written < 0 || (size_t)written >= sizeof(path)) {
        result = MANAGER_STORAGE_INVALID;
    } else {
        result = manager_storage_read_head(io, path, head, sizeof(head),
                                           &length, &truncated);
        if (result == MANAGER_STORAGE_OK)
            (void)manager_mods_meta_scan(head, length, meta);
    }
    if (meta->name[0] == '\0' && dir != NULL)
        (void)snprintf(meta->name, sizeof(meta->name), "%s", dir);
    return result;
}

#endif /* MANAGER_UI2 */
