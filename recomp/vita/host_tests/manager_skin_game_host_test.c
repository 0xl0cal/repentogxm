/*
 * Manager UI2 game skin host test (a "synthetic sheets" render test
 * and injected failures -> the flat skin).
 *
 * Runs the real manager_skin_game.c over the synthetic install that
 * manager_gameskin_fixtures.py writes into <fixtures>/skin/<variant>/ (the
 * archives are served by fake_blob.h, the skin's memory by a malloc'd block
 * with guard words on both sides):
 *
 *   - every page and box of manager_ui_render_host_test's script painted
 *     with theme=game in English and Russian, each frame hashed (FNV-1a)
 *     against manager_skin_game_goldens.txt (MANAGER_SKIN_GAME_UPDATE=1
 *     rewrites it, MANAGER_SKIN_FRAMES_DIR=<dir> writes .ppm frames);
 *   - the BMFont face adapter: Cyrillic through the BMFont, a code point the
 *     face lacks drawn by the DejaVu face of the same role on the same
 *     baseline, measure == draw width, nothing drawn outside the clip;
 *   - failure paths, each ending in the light theme with the "could not be
 *     read" status line, the memory block released, no archive handle left
 *     open and the guard words intact: no archives, a missing archive, a
 *     missing row, a corrupt PNG, a sheet of the wrong size, a truncated
 *     .fnt, a font page of the wrong size, acquire() refusing, every arena
 *     size below the high-water mark, every pread failing or hitting EOF in
 *     turn; a damaged archive index is skipped like a missing archive;
 *   - 600 byte-mutated installs: each loads or fails cleanly;
 *   - *theme is left untouched by a failing init.
 *
 * Usage: manager_skin_game_host_test <fixture dir>
 */
#define FAKE_DATA_CAPACITY 17408u
#include "fake_fs.h"
#include "manager_ui2_test_common.h"
#include "manager_ui2_fixture.h"
#include "fake_blob.h"
#include "manager_skin_game_states.h"

#include "manager_skin_game.h"

#include <stdlib.h>

#ifndef MANAGER_SKIN_GAME_GOLDENS
#error "MANAGER_SKIN_GAME_GOLDENS must name manager_skin_game_goldens.txt"
#endif

#define W 960
#define H 544
#define GUARD 1024u
#define GUARD_WORD 0x5a17c3e1u
#define MAX_FRAMES 64u
#define PACKED "host0:/isaacr001/resources/packed"
#define ARENA_GUARD 64u
#define ARENA_GUARD_BYTE 0xc5u

static const char *const archive_names[3] = {"repentance.a", "afterbirthp.a",
                                             "afterbirth.a"};

static const char *fixture_dir;
static struct manager_theme_fonts imf_fonts;
static struct manager_display_list dl;
static uint32_t buffer[GUARD + W * H + GUARD];

/* ---- synthetic install ------------------------------------------------ */

struct install {
    uint8_t *data[3];
    size_t size[3];
};

static struct install installs[16];
static const char *install_names[16];
static unsigned int install_count;
static struct fake_blob blob;
static struct manager_blob_io blob_io;

static const struct install *load_install(const char *variant)
{
    unsigned int index;
    struct install *install;
    for (index = 0u; index < install_count; ++index)
        if (strcmp(install_names[index], variant) == 0)
            return &installs[index];
    require(install_count < 16u, "install table capacity");
    install = &installs[install_count];
    install_names[install_count++] = variant;
    for (index = 0u; index < 3u; ++index) {
        char path[1024];
        (void)snprintf(path, sizeof(path), "%s/skin/%s/%s", fixture_dir,
                       variant, archive_names[index]);
        install->data[index] = fake_blob_load_file(path, &install->size[index]);
    }
    return install;
}

/* Registers the variant's archives under PACKED; faults reset. */
static void use_install(const char *variant)
{
    const struct install *install = load_install(variant);
    unsigned int index;
    fake_blob_init(&blob);
    for (index = 0u; index < 3u; ++index) {
        char path[256];
        if (install->data[index] == NULL)
            continue;
        (void)snprintf(path, sizeof(path), "%s/%s", PACKED,
                       archive_names[index]);
        require(fake_blob_add(&blob, path, install->data[index],
                              install->size[index]) == 0,
                "archive registered");
    }
    blob_io = fake_blob_io(&blob);
}

/* ---- the skin's memory ------------------------------------------------- */

struct host_arena {
    unsigned int acquires; /* blocks handed out */
    unsigned int releases;
    unsigned int refused;
    int refuse;
    uint8_t *raw;   /* the live block (guards included), NULL when none */
    size_t bytes;
};

static struct host_arena arena;
static struct manager_skin_game_context context;

static void check_guards(const uint8_t *raw, size_t bytes)
{
    size_t index;
    for (index = 0u; index < ARENA_GUARD; ++index) {
        require(raw[index] == ARENA_GUARD_BYTE, "arena guard before");
        require(raw[ARENA_GUARD + bytes + index] == ARENA_GUARD_BYTE,
                "arena guard after");
    }
}

static void *host_acquire(void *opaque, size_t bytes)
{
    struct host_arena *host = (struct host_arena *)opaque;
    require(host == &arena, "acquire gets the opaque pointer");
    require(host->raw == NULL, "one block at a time");
    if (host->refuse) {
        ++host->refused;
        return NULL;
    }
    ++host->acquires;
    host->raw = (uint8_t *)malloc(bytes + 2u * ARENA_GUARD);
    require(host->raw != NULL, "host block");
    memset(host->raw, ARENA_GUARD_BYTE, bytes + 2u * ARENA_GUARD);
    host->bytes = bytes;
    return host->raw + ARENA_GUARD;
}

static void host_release(void *opaque, void *block, size_t bytes)
{
    struct host_arena *host = (struct host_arena *)opaque;
    require(host == &arena && host->raw != NULL, "release of a live block");
    require(block == host->raw + ARENA_GUARD && bytes == host->bytes,
            "release names the acquired block");
    check_guards(host->raw, host->bytes);
    free(host->raw);
    host->raw = NULL;
    ++host->releases;
}

static uint64_t fake_clock_value;
static uint64_t host_clock(void *opaque)
{
    (void)opaque;
    return fake_clock_value += 1000u;
}

/* Drops the block a previous successful init kept (its theme set is about
 * to be replaced). */
static void drop_live_skin(void)
{
    if (arena.raw != NULL) {
        check_guards(arena.raw, arena.bytes);
        free(arena.raw);
        arena.raw = NULL;
        ++arena.releases;
    }
}

static void reset_context(size_t arena_bytes)
{
    memset(&context, 0, sizeof(context));
    context.io = &blob_io;
    context.packed_dir = PACKED;
    context.acquire = host_acquire;
    context.release = host_release;
    context.opaque = &arena;
    context.clock_us = host_clock;
    context.arena_bytes = arena_bytes;
}

/* ---- frames -------------------------------------------------------------- */

struct frame_hash {
    char label[64];
    uint32_t hash;
};
static struct frame_hash produced[MAX_FRAMES];
static unsigned int produced_count;

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
    manager_ui_draw(&dl, manager_ui_theme(&ss_ui), &canvas);
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
    const char *directory = getenv("MANAGER_SKIN_FRAMES_DIR");
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

static uint32_t record_frame(const char *label)
{
    uint32_t first;
    uint32_t second;
    manager_ui_build(&ss_ui, &dl);
    require(!dl.overflow, "display list fits");
    if (manager_ui_theme(&ss_ui)->id == MANAGER_THEME_GAME) {
        unsigned int n;
        for (n = 0u; n < dl.count; ++n) {
            const struct manager_dl_item *item = &dl.items[n];
            if (item->kind == MANAGER_DL_HIGHLIGHT ||
                item->kind == MANAGER_DL_SCROLL) {
                /* Independently measured inner paper bounds, not the layout
                 * constants: moving a band to the torn edge must fail. */
                require(item->x >= 30 && item->x1 < 928 &&
                            item->y >= 90 && item->y1 < 430,
                        "list selection and scrollbar stay inside the paper");
            }
        }
        require(strcmp(manager_str(ss_ui.lang, MSTR_TAB_SAVES),
                       manager_str(ss_ui.lang, MSTR_TITLE_SAVES)) == 0 &&
                    strcmp(manager_str(ss_ui.lang, MSTR_TAB_ACHIEVEMENTS),
                           manager_str(ss_ui.lang,
                                       MSTR_TITLE_ACHIEVEMENTS)) == 0,
                "tabs and page titles use the same names");
    }
    first = paint();
    second = paint();
    require(first == second, "painting is deterministic");
    write_ppm(label);
    require(produced_count < MAX_FRAMES, "frame table capacity");
    (void)snprintf(produced[produced_count].label,
                   sizeof(produced[produced_count].label), "%s", label);
    produced[produced_count].hash = first;
    ++produced_count;
    return first;
}

static void game_frame(const char *label)
{
    require(manager_ui_theme(&ss_ui)->id == MANAGER_THEME_GAME,
            "the game skin is in use");
    (void)record_frame(label);
}

static void test_layout_rows(void)
{
    uint8_t rows[MANAGER_UI_SET_COUNT];
    unsigned int count;
    unsigned int n;
    ss_goto(MANAGER_UI_PAGE_SETTINGS);
    /* The undo row makes the page taller, as it is on the owner's Vita. */
    ss_ui.settings.state.undo_available = 1;
    count = manager_ui_settings_rows(&ss_ui, rows);
    for (n = 0u; n < count; ++n) {
        unsigned int i;
        unsigned int selected = 0u;
        ss_ui.settings_row = rows[n];
        manager_ui_build(&ss_ui, &dl);
        for (i = 0u; i < dl.count; ++i) {
            const struct manager_dl_item *item = &dl.items[i];
            if (item->kind == MANAGER_DL_HIGHLIGHT) {
                ++selected;
                require(item->y >= 90 && item->y1 < 430,
                        "every settings row scrolls fully into view");
            }
        }
        require(selected == 1u && !dl.overflow,
                "selected settings row is present, including undo/defaults");
        (void)paint();
    }
    ss_goto(MANAGER_UI_PAGE_MODS);
    for (n = 0u; n < ss_ui.mod_count; ++n) {
        if (strcmp(ss_ui.mods[n].entry.name, MANAGER_UI_EID_DIRECTORY) == 0) {
            unsigned int i;
            unsigned int found = 0u;
            /* The device has a numeric <name>, not the Workshop title. */
            (void)snprintf(ss_ui.mods[n].meta.name,
                           sizeof ss_ui.mods[n].meta.name, "%s",
                           MANAGER_UI_EID_DIRECTORY);
            ss_ui.mod_selection = n;
            manager_ui_build(&ss_ui, &dl);
            for (i = 0u; i < dl.count; ++i) {
                const struct manager_dl_item *item = &dl.items[i];
                if (item->kind == MANAGER_DL_TEXT &&
                    strcmp(manager_dl_text_at(&dl, item->text),
                           "External Item Descriptions") == 0) {
                    ++found;
                    require(item->face == MANAGER_FACE_BODY,
                            "mod names use the readable row font");
                }
            }
            require(found == 1u, "numeric EID metadata shows a real name once");
            return;
        }
    }
    require(0, "EID fixture is present");
}

static void check_goldens(void)
{
    const char *update = getenv("MANAGER_SKIN_GAME_UPDATE");
    unsigned int index;
    FILE *file;
    char line[160];
    unsigned int matched = 0u;
    unsigned int mismatched = 0u;
    unsigned int listed = 0u;
    if (update != NULL && strcmp(update, "1") == 0) {
        file = fopen(MANAGER_SKIN_GAME_GOLDENS, "wb");
        require(file != NULL, "goldens file writable");
        fprintf(file, "# manager_skin_game_host_test frame hashes (FNV-1a of "
                      "the 960x544 canvas words)\n# over the synthetic "
                      "install of manager_gameskin_fixtures.py; regenerate "
                      "with\n# MANAGER_SKIN_GAME_UPDATE=1 after a deliberate "
                      "skin or layout change.\n");
        for (index = 0u; index < produced_count; ++index)
            fprintf(file, "%s %08x\n", produced[index].label,
                    produced[index].hash);
        fclose(file);
        printf("goldens rewritten: %u frames\n", produced_count);
        return;
    }
    file = fopen(MANAGER_SKIN_GAME_GOLDENS, "rb");
    if (file == NULL) {
        fprintf(stderr, "no goldens at %s; run with MANAGER_SKIN_GAME_UPDATE=1\n",
                MANAGER_SKIN_GAME_GOLDENS);
        require(0, "goldens file present");
    }
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

/* ---- the skin through the theme set ---------------------------------------- */

static void render_variant(const char *lang)
{
    use_install("good");
    reset_context(0u);
    ss_states(lang, &imf_fonts, &context, game_frame);
    require(context.stage == MANAGER_SKIN_GAME_OK, "skin loaded");
    require(blob.open_handles == 0, "archives closed after the init");
}

/* A start with theme=game over a broken install: light + status line. */
static void expect_fallback(const char *variant, int stage, const char *lang)
{
    char ini[64];
    use_install(variant);
    reset_context(0u);
    (void)snprintf(ini, sizeof(ini), "lang=%s\ntheme=game\n", lang);
    ss_start(ini, &imf_fonts, &context);
    if (context.stage != stage)
        fprintf(stderr, "variant %s: stage %s, expected %s\n", variant,
                manager_skin_game_stage_text(context.stage),
                manager_skin_game_stage_text(stage));
    require(context.stage == stage, "failure stage");
    require(manager_ui_theme(&ss_ui)->id == MANAGER_THEME_LIGHT &&
                ss_themes.game_state == -1,
            "failed skin -> light theme");
    require(ss_ui.status_tone == MANAGER_TONE_WARNING &&
                strcmp(ss_ui.status,
                       manager_str(ss_ui.lang, MSTR_STATUS_THEME_FAILED)) == 0,
            "failed skin -> status line");
    require(arena.raw == NULL && arena.acquires == arena.releases,
            "failed skin released its memory");
    require(blob.open_handles == 0, "no archive handle left open");
}

/* ---- direct init ---------------------------------------------------------- */

struct init_result {
    int result;
    int stage;
    size_t peak;
};

/* manager_skin_game_init on its own; a successful skin is dropped again. */
static struct init_result direct_init(size_t arena_bytes)
{
    struct manager_theme theme;
    struct manager_theme pattern;
    struct init_result outcome;
    memset(&theme, 0xa5, sizeof(theme));
    memset(&pattern, 0xa5, sizeof(pattern));
    reset_context(arena_bytes);
    outcome.result = manager_skin_game_init(&theme, &imf_fonts, &context);
    outcome.stage = context.stage;
    outcome.peak = context.arena_peak;
    require(blob.open_handles == 0, "no archive handle left open");
    if (outcome.result != 0) {
        require(memcmp(&theme, &pattern, sizeof(theme)) == 0,
                "a failing init leaves *theme untouched");
        require(arena.raw == NULL && arena.acquires == arena.releases,
                "a failing init releases its memory");
        require(outcome.stage != MANAGER_SKIN_GAME_OK, "a failure has a stage");
    } else {
        require(theme.id == MANAGER_THEME_GAME && theme.ops != NULL &&
                    theme.skin != NULL,
                "a loaded skin fills the theme");
        require(arena.raw != NULL, "a loaded skin keeps its block");
        drop_live_skin();
    }
    return outcome;
}

static void test_direct_failures(void)
{
    struct manager_theme theme;
    struct init_result outcome;
    size_t peak;
    size_t size;
    unsigned int preads;
    unsigned int n;
    unsigned int succeeded;
    unsigned int failed;

    require(manager_skin_game_init(&theme, &imf_fonts, NULL) != 0,
            "NULL context refused");
    use_install("good");
    reset_context(0u);
    context.io = NULL;
    memset(&theme, 0, sizeof(theme));
    require(manager_skin_game_init(&theme, &imf_fonts, &context) != 0 &&
                context.stage == MANAGER_SKIN_GAME_STAGE_CONTEXT &&
                arena.acquires == arena.releases,
            "context without io refused before acquiring");

    arena.refuse = 1;
    outcome = direct_init(0u);
    require(outcome.result != 0 &&
                outcome.stage == MANAGER_SKIN_GAME_STAGE_MEMORY &&
                arena.refused == 1u,
            "acquire refusing -> memory stage");
    arena.refuse = 0;

    /* The arena: success at the high-water mark, a clean failure below. */
    outcome = direct_init(0u);
    require(outcome.result == 0, "good install loads");
    peak = outcome.peak;
    require(peak > 0u && peak <= MANAGER_SKIN_GAME_ARENA_BYTES,
            "peak within the default arena");
    outcome = direct_init(peak);
    require(outcome.result == 0 && outcome.peak == peak,
            "exactly the high-water mark is enough");
    succeeded = 0u;
    /* arena_bytes 0 means the default, so the sweep starts at 1. */
    for (size = 1u; size < peak; size += size < 4096u ? 511u : 12288u) {
        outcome = direct_init(size);
        if (outcome.result == 0 ||
            outcome.stage != MANAGER_SKIN_GAME_STAGE_MEMORY)
            fprintf(stderr, "arena %zu: result %d stage %s\n", size,
                    outcome.result, manager_skin_game_stage_text(outcome.stage));
        require(outcome.result != 0 &&
                    outcome.stage == MANAGER_SKIN_GAME_STAGE_MEMORY,
                "a short arena fails at the memory stage");
        ++succeeded;
    }
    outcome = direct_init(peak - 16u);
    require(outcome.result != 0 && outcome.stage == MANAGER_SKIN_GAME_STAGE_MEMORY,
            "16 bytes short fails");
    printf("arena: peak %zu bytes, %u short sizes refused cleanly\n", peak,
           succeeded + 1u);

    /* Every pread failing, then every pread hitting EOF, in turn. */
    blob.preads = 0u;
    outcome = direct_init(0u);
    preads = blob.preads;
    require(outcome.result == 0 && preads > 0u, "pread count of a good init");
    for (n = 1u, succeeded = 0u, failed = 0u; n <= preads; ++n) {
        use_install("good");
        blob.fail_pread_at = n;
        outcome = direct_init(0u);
        if (outcome.result == 0)
            ++succeeded;
        else
            ++failed;
        use_install("good");
        blob.eof_pread_at = n;
        outcome = direct_init(0u);
        if (outcome.result == 0)
            ++succeeded;
        else
            ++failed;
    }
    require(failed > 0u, "injected read faults fail the init");
    printf("pread faults: %u preads, %u inits failed cleanly, %u still loaded "
           "(a skipped archive)\n",
           preads, failed, succeeded);
    use_install("good");
}

/* Byte mutations of the good install (deterministic LCG): every init
 * either loads or fails cleanly - direct_init checks the handles, the
 * guard words, the released block and the untouched theme. */
static void test_mutations(void)
{
    const struct install *good = load_install("good");
    static uint8_t *copies[3];
    uint32_t seed = 0x2c1b3a5du;
    unsigned int round;
    unsigned int loaded = 0u;
    unsigned int refused = 0u;
    unsigned int index;
    for (index = 0u; index < 3u; ++index) {
        copies[index] = (uint8_t *)malloc(good->size[index]);
        require(copies[index] != NULL, "mutation buffer");
    }
    for (round = 0u; round < 600u; ++round) {
        unsigned int flips;
        fake_blob_init(&blob);
        for (index = 0u; index < 3u; ++index) {
            char path[256];
            memcpy(copies[index], good->data[index], good->size[index]);
            (void)snprintf(path, sizeof(path), "%s/%s", PACKED,
                           archive_names[index]);
            require(fake_blob_add(&blob, path, copies[index],
                                  good->size[index]) == 0,
                    "archive registered");
        }
        for (flips = 0u; flips < 1u + round % 4u; ++flips) {
            size_t offset;
            seed = seed * 1664525u + 1013904223u;
            index = (seed >> 8) % 3u;
            seed = seed * 1664525u + 1013904223u;
            offset = (size_t)(seed >> 4) % good->size[index];
            seed = seed * 1664525u + 1013904223u;
            copies[index][offset] ^= (uint8_t)(1u + (seed >> 24) % 255u);
        }
        blob_io = fake_blob_io(&blob);
        if (direct_init(0u).result == 0)
            ++loaded;
        else
            ++refused;
    }
    for (index = 0u; index < 3u; ++index)
        free(copies[index]);
    printf("mutations: 600 mutated installs, %u loaded, %u refused cleanly\n",
           loaded, refused);
    require(refused > 0u, "mutations reach the refusal paths");
    use_install("good");
}

/* ---- the face adapter ----------------------------------------------------- */

static void clear(uint32_t *words, size_t count, uint32_t value)
{
    size_t index;
    for (index = 0u; index < count; ++index)
        words[index] = value;
}

static void test_adapter(const struct manager_theme *game)
{
    static uint32_t a[200 * 60];
    static uint32_t b[200 * 60];
    struct manager_canvas ca;
    struct manager_canvas cb;
    const struct manager_face body = game->face[MANAGER_FACE_BODY];
    const struct manager_face dejavu = imf_fonts.face[MANAGER_FACE_BODY];
    const uint32_t ink = game->palette.color[MANAGER_COLOR_TEXT];
    const char *mixed = "Ж×A—я";
    int width;
    size_t index;
    unsigned int changed;

    require(body.ops->has_glyph(body.impl, 0x416u), "the face has Cyrillic");
    require(body.ops->has_glyph(body.impl, 0xd7u),
            "U+00D7 is drawable (DejaVu fallback)");

    /* A code point the face lacks: the DejaVu glyph, baselines lined up. */
    clear(a, 200u * 60u, 0xffe0e0e0u);
    clear(b, 200u * 60u, 0xffe0e0e0u);
    manager_canvas_init(&ca, a, 200, 60, 200);
    manager_canvas_init(&cb, b, 200, 60, 200);
    width = body.ops->draw_glyph(body.impl, &ca, 20, 10, 0xd7u, ink);
    require(width == dejavu.ops->advance(dejavu.impl, 0xd7u),
            "fallback advance is DejaVu's");
    (void)dejavu.ops->draw_glyph(
        dejavu.impl, &cb, 20,
        10 + manager_face_ascent(body) - manager_face_ascent(dejavu), 0xd7u,
        ink);
    require(memcmp(a, b, sizeof(a)) == 0,
            "fallback glyph sits on the BMFont baseline");

    /* A Cyrillic letter goes through the BMFont, not DejaVu. */
    clear(a, 200u * 60u, 0xffe0e0e0u);
    clear(b, 200u * 60u, 0xffe0e0e0u);
    (void)body.ops->draw_glyph(body.impl, &ca, 20, 10, 0x416u, ink);
    (void)dejavu.ops->draw_glyph(
        dejavu.impl, &cb, 20,
        10 + manager_face_ascent(body) - manager_face_ascent(dejavu), 0x416u,
        ink);
    require(memcmp(a, b, sizeof(a)) != 0, "Cyrillic drawn by the BMFont");
    for (index = 0u, changed = 0u; index < 200u * 60u; ++index)
        changed += a[index] != 0xffe0e0e0u;
    require(changed > 0u, "the BMFont glyph has ink");

    /* measure == draw, and nothing outside the clip. */
    clear(a, 200u * 60u, 0xffe0e0e0u);
    require(manager_face_measure(body, mixed) ==
                manager_face_draw(body, &ca, 4, 4, ink, mixed),
            "measure equals the drawn width");
    clear(a, 200u * 60u, 0xffe0e0e0u);
    manager_canvas_set_clip(&ca, 30, 12, 25, 9);
    (void)manager_face_draw(body, &ca, 10, 4, ink, mixed);
    (void)manager_face_draw(game->face[MANAGER_FACE_TITLE], &ca, -8, -6, ink,
                            mixed);
    for (index = 0u, changed = 0u; index < 200u * 60u; ++index) {
        const int x = (int)(index % 200u);
        const int y = (int)(index / 200u);
        const int inside = x >= 30 && x < 55 && y >= 12 && y < 21;
        if (!inside)
            require(a[index] == 0xffe0e0e0u, "nothing drawn outside the clip");
        else
            changed += a[index] != 0xffe0e0e0u;
    }
    require(changed > 0u, "text inside the clip drawn");
    /* Measure-only glyph ops. */
    require(game->ops->draw_hint_glyph(game, NULL, 0, 0, MANAGER_GLYPH_CROSS,
                                       0u) > 0 &&
                game->ops->draw_hint_glyph(game, NULL, 0, 0,
                                           MANAGER_GLYPH_LAUNCH, 0u) == 0,
            "hint glyph widths");
}

/* ---- main ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    static const char *const langs[2] = {"en", "ru"};
    static const struct {
        const char *variant;
        int stage;
    } broken[] = {
        {"empty", MANAGER_SKIN_GAME_STAGE_ARCHIVE},
        {"no-afterbirthp", MANAGER_SKIN_GAME_STAGE_FIND},
        {"no-popup", MANAGER_SKIN_GAME_STAGE_FIND},
        {"bad-png", MANAGER_SKIN_GAME_STAGE_PNG},
        {"bad-size", MANAGER_SKIN_GAME_STAGE_GEOMETRY},
        {"bad-font", MANAGER_SKIN_GAME_STAGE_FONT},
        {"bad-page", MANAGER_SKIN_GAME_STAGE_GEOMETRY},
    };
    unsigned int index;
    uint32_t good_home = 0u;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixture dir>\n", argv[0]);
        return 2;
    }
    fixture_dir = argv[1];
    ui2_load_fonts(&imf_fonts);
    ss_before_start = drop_live_skin;

    for (index = 0u; index < 2u; ++index)
        render_variant(langs[index]);
    good_home = produced[0].hash;
    printf("skin: arena peak %zu bytes, resident %zu bytes\n",
           context.arena_peak, context.arena_resident);

    /* The theme as the UI uses it, for the adapter checks. */
    use_install("good");
    reset_context(0u);
    ss_start("lang=en\ntheme=game\n", &imf_fonts, &context);
    require(manager_ui_theme(&ss_ui)->id == MANAGER_THEME_GAME, "game theme");
    test_adapter(manager_ui_theme(&ss_ui));
    test_layout_rows();

    /* Short reads (7 bytes per pread) give the same picture. */
    use_install("good");
    blob.short_read_cap = 7u;
    reset_context(0u);
    ss_start("lang=en\ntheme=game\n", &imf_fonts, &context);
    manager_ui_build(&ss_ui, &dl);
    require(paint() == good_home, "short reads give the same frame");

    /* A damaged archive index is skipped like a missing archive. */
    use_install("damaged-index");
    reset_context(0u);
    ss_start("lang=en\ntheme=game\n", &imf_fonts, &context);
    require(manager_ui_theme(&ss_ui)->id == MANAGER_THEME_GAME &&
                context.stage == MANAGER_SKIN_GAME_OK,
            "damaged repentance.a index skipped, the skin still loads");
    (void)record_frame("18-damaged-index-game-en");
    /* The mods directory cannot be listed: the error tone on the skin. */
    fake_fail(&ss_fs, FAKE_OP_OPEN_DIR, 1);
    ss_goto(MANAGER_UI_PAGE_MODS);
    fake_no_fail(&ss_fs);
    require(ss_ui.status_tone == MANAGER_TONE_ERROR && ss_ui.mod_count == 0u,
            "listing failure shown");
    (void)record_frame("20-mods-scan-failed-game-en");
    require_file(&ss_fs, MANAGER_INI_PATH, "lang=en\ntheme=game\n",
                 "rendering never rewrites manager.ini");
    drop_live_skin();

    for (index = 0u; index < sizeof(broken) / sizeof(broken[0]); ++index)
        expect_fallback(broken[index].variant, broken[index].stage, "en");
    for (index = 0u; index < 2u; ++index) {
        char label[64];
        expect_fallback("no-popup", MANAGER_SKIN_GAME_STAGE_FIND, langs[index]);
        (void)snprintf(label, sizeof(label), "19-game-failed-%s", langs[index]);
        (void)record_frame(label);
    }

    test_direct_failures();
    test_mutations();
    drop_live_skin();
    require(arena.acquires == arena.releases, "every block released");

    check_goldens();
    printf("manager skin game host test: %u frames, %u checks PASS\n",
           produced_count, checks);
    return 0;
}
