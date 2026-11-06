/*
 * Shared scaffolding of the manager UI2 host tests: a counting require(),
 * the committed IMF atlases of recomp/vita/assets/manager loaded into
 * struct manager_theme_fonts, and FNV-1a over canvas words.  Header-only
 * (static functions), included by one test TU each.
 */
#ifndef ISAAC_MANAGER_UI2_TEST_COMMON_H
#define ISAAC_MANAGER_UI2_TEST_COMMON_H

#include "manager_font.h"
#include "manager_theme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MANAGER_UI_ASSET_DIR
#error "MANAGER_UI_ASSET_DIR must name recomp/vita/assets/manager"
#endif

/* fake_fs.h (included first by the tests that need storage) brings the
 * same `checks` counter and require(). */
#if !defined(ISAAC_MANAGER_FAKE_FS_H)
static unsigned int checks;

static void __attribute__((unused)) require(int condition, const char *message)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}
#endif

static unsigned char *__attribute__((unused))
ui2_load_file(const char *name, size_t *size)
{
    char path[1024];
    FILE *file;
    long length;
    unsigned char *data;
    (void)snprintf(path, sizeof(path), "%s/%s", MANAGER_UI_ASSET_DIR, name);
    file = fopen(path, "rb");
    require(file != NULL, "asset opens");
    require(fseek(file, 0, SEEK_END) == 0, "asset seek");
    length = ftell(file);
    require(length > 0, "asset length");
    require(fseek(file, 0, SEEK_SET) == 0, "asset rewind");
    data = (unsigned char *)malloc((size_t)length);
    require(data != NULL, "asset buffer");
    require(fread(data, 1u, (size_t)length, file) == (size_t)length,
            "asset read");
    fclose(file);
    *size = (size_t)length;
    return data;
}

/* Faces in enum manager_face_role order (title 24 bold, body 16, small 12);
 * the file names are the ones the VPK ships under manager/. */
static const char *const ui2_face_files[MANAGER_FACE_COUNT] = {
    "font-title-24.imf", "font-body-16.imf", "font-small-12.imf"};

static struct manager_imf ui2_imf[MANAGER_FACE_COUNT];

static void __attribute__((unused))
ui2_load_fonts(struct manager_theme_fonts *fonts)
{
    unsigned int index;
    for (index = 0u; index < MANAGER_FACE_COUNT; ++index) {
        size_t size = 0u;
        unsigned char *data = ui2_load_file(ui2_face_files[index], &size);
        require(manager_imf_parse(&ui2_imf[index], data, size) ==
                    MANAGER_IMF_OK,
                "committed atlas parses");
        fonts->face[index] = manager_imf_face(&ui2_imf[index]);
    }
}

static uint32_t __attribute__((unused))
ui2_fnv1a(const uint32_t *words, size_t count)
{
    uint32_t hash = 2166136261u;
    size_t index;
    for (index = 0u; index < count; ++index) {
        uint32_t word = words[index];
        unsigned int byte;
        for (byte = 0u; byte < 4u; ++byte) {
            hash ^= word & 0xffu;
            hash *= 16777619u;
            word >>= 8;
        }
    }
    return hash;
}

#endif
