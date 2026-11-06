/*
 * Shared helpers of the game-skin host tests (manager_arch_host_test,
 * manager_png_host_test): failure counting and the manifest written by
 * manager_gameskin_fixtures.py (one record per line, space-separated
 * fields, last line "end <record count>").  A missing, truncated or
 * miscounted manifest is a failure, never a skip.
 */
#ifndef ISAAC_MANAGER_GAMESKIN_FIXTURE_H
#define ISAAC_MANAGER_GAMESKIN_FIXTURE_H

#include "fake_blob.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GS_MAX_RECORDS 512
#define GS_MAX_FIELDS 20

static int gs_failures;
static int gs_checks;

#define GS_CHECK(condition, ...)                                              \
    do {                                                                      \
        ++gs_checks;                                                          \
        if (!(condition)) {                                                   \
            ++gs_failures;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

struct gs_record {
    int count;
    char *field[GS_MAX_FIELDS];
};

struct gs_manifest {
    char dir[512];
    char *text;
    int count;
    struct gs_record record[GS_MAX_RECORDS];
};

/* dir/name into a malloc'd buffer; aborts the test (counted failure) when
 * the file is missing. */
static inline uint8_t *gs_load(const struct gs_manifest *manifest, const char *name,
                        size_t *size)
{
    char path[1024];
    uint8_t *data;

    snprintf(path, sizeof(path), "%s/%s", manifest->dir, name);
    data = fake_blob_load_file(path, size);
    if (data == NULL) {
        fprintf(stderr, "FAIL: cannot read fixture %s\n", path);
        exit(1);
    }
    return data;
}

static inline int gs_manifest_load(struct gs_manifest *manifest, const char *dir)
{
    size_t size = 0u;
    char *cursor;
    int ended = 0;
    int end_count = -1;

    memset(manifest, 0, sizeof(*manifest));
    snprintf(manifest->dir, sizeof(manifest->dir), "%s", dir);
    manifest->text = (char *)gs_load(manifest, "manifest.txt", &size);
    manifest->text[size] = '\0';
    cursor = manifest->text;
    while (*cursor != '\0') {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        struct gs_record *record;
        char *token;
        if (newline == NULL) {
            fprintf(stderr, "FAIL: manifest line without newline\n");
            return -1;
        }
        *newline = '\0';
        cursor = newline + 1;
        if (ended) {
            fprintf(stderr, "FAIL: manifest continues after its end line\n");
            return -1;
        }
        if (strncmp(line, "end ", 4) == 0) {
            end_count = atoi(line + 4);
            ended = 1;
            continue;
        }
        if (manifest->count >= GS_MAX_RECORDS) {
            fprintf(stderr, "FAIL: manifest has too many records\n");
            return -1;
        }
        record = &manifest->record[manifest->count++];
        for (token = strtok(line, " "); token != NULL; token = strtok(NULL, " ")) {
            if (record->count >= GS_MAX_FIELDS) {
                fprintf(stderr, "FAIL: manifest record with too many fields\n");
                return -1;
            }
            record->field[record->count++] = token;
        }
        if (record->count == 0) {
            fprintf(stderr, "FAIL: empty manifest record\n");
            return -1;
        }
    }
    if (!ended || end_count != manifest->count) {
        fprintf(stderr, "FAIL: manifest end line %d, %d records read\n",
                end_count, manifest->count);
        return -1;
    }
    return 0;
}

static inline int gs_is(const struct gs_record *record, const char *kind, int fields)
{
    if (strcmp(record->field[0], kind) != 0)
        return 0;
    if (record->count != fields) {
        GS_CHECK(0, "manifest record %s has %d fields, expected %d", kind,
                 record->count, fields);
        return 0;
    }
    return 1;
}

static inline long long gs_int(const char *text)
{
    return strtoll(text, NULL, 0);
}

static inline uint32_t gs_u32(const char *text)
{
    return (uint32_t)strtoull(text, NULL, 0);
}

/* Hex text ("-" = empty) into a NUL-terminated byte string. */
static inline size_t gs_hex(const char *hex, char *out, size_t capacity)
{
    size_t length = 0u;

    if (strcmp(hex, "-") == 0) {
        out[0] = '\0';
        return 0u;
    }
    while (hex[0] != '\0' && hex[1] != '\0' && length + 1u < capacity) {
        unsigned value;
        if (sscanf(hex, "%2x", &value) != 1)
            break;
        out[length++] = (char)value;
        hex += 2;
    }
    out[length] = '\0';
    return length;
}

/* Deterministic generator for the mutation loops. */
static inline uint32_t gs_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static inline void gs_fill(uint8_t *bytes, size_t size, uint8_t value)
{
    memset(bytes, value, size);
}

static inline int gs_untouched(const uint8_t *bytes, size_t size, uint8_t value)
{
    size_t i;

    for (i = 0u; i < size; ++i) {
        if (bytes[i] != value)
            return 0;
    }
    return 1;
}

static inline int gs_report(const char *name)
{
    if (gs_failures != 0) {
        fprintf(stderr, "%s: %d of %d checks FAILED\n", name, gs_failures,
                gs_checks);
        return 1;
    }
    printf("%s: PASS (%d checks)\n", name, gs_checks);
    return 0;
}

#endif
