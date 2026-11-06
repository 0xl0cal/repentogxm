/*
 * Manager UI2 font files over sceIo (see manager_assets_vita.h).
 */
#if defined(MANAGER_UI2)

#include "manager_assets_vita.h"

#include "manager_font.h"

#include <psp2/io/fcntl.h>

#include <stddef.h>
#include <stdint.h>

static uint8_t manager_assets_pool[MANAGER_ASSETS_POOL_BYTES]
    __attribute__((aligned(16)));
static struct manager_imf manager_assets_imf[MANAGER_FACE_COUNT];
static struct manager_builtin_font manager_assets_builtin[MANAGER_FACE_COUNT] = {
    {3u}, {2u}, {2u}};

/* Reads the whole file into the pool at *used; 0 and the size on success,
 * <0 when it cannot be opened or read, or does not fit what is left. */
static int manager_assets_read(const char *path, size_t *used, size_t *size)
{
    const SceUID file = sceIoOpen(path, SCE_O_RDONLY, 0);
    size_t length = 0u;
    int result = 0;
    if (file < 0)
        return -1;
    for (;;) {
        const size_t room = MANAGER_ASSETS_POOL_BYTES - *used - length;
        SceSSize got;
        if (room == 0u) {
            result = -2; /* larger than the pool */
            break;
        }
        got = sceIoRead(file, manager_assets_pool + *used + length,
                        (SceSize)room);
        if (got < 0) {
            result = -3;
            break;
        }
        if (got == 0)
            break;
        length += (size_t)got;
    }
    (void)sceIoClose(file);
    if (result < 0 || length == 0u)
        return result < 0 ? result : -4;
    *size = length;
    return 0;
}

int manager_assets_load_fonts(struct manager_theme_fonts *fonts)
{
    static const char *const paths[MANAGER_FACE_COUNT] = {
        MANAGER_ASSETS_TITLE_FONT, MANAGER_ASSETS_BODY_FONT,
        MANAGER_ASSETS_SMALL_FONT};
    size_t used = 0u;
    unsigned int index;
    for (index = 0u; index < MANAGER_FACE_COUNT; ++index) {
        size_t size = 0u;
        if (manager_assets_read(paths[index], &used, &size) != 0 ||
            manager_imf_parse(&manager_assets_imf[index],
                              manager_assets_pool + used, size) !=
                MANAGER_IMF_OK)
            break;
        fonts->face[index] = manager_imf_face(&manager_assets_imf[index]);
        /* Keep the next file 16-byte aligned like the first. */
        used += (size + 15u) & ~(size_t)15u;
        if (used > MANAGER_ASSETS_POOL_BYTES)
            used = MANAGER_ASSETS_POOL_BYTES;
    }
    if (index == MANAGER_FACE_COUNT)
        return 0;
    for (index = 0u; index < MANAGER_FACE_COUNT; ++index)
        fonts->face[index] = manager_builtin_face(&manager_assets_builtin[index]);
    return 1;
}

#endif /* MANAGER_UI2 */
