/*
 * manager_png host test: every PNG fixture that
 * manager_gameskin_fixtures.py writes with stdlib zlib (all five filters,
 * 1x1, odd widths, multi-IDAT with an empty IDAT, stored and fixed-Huffman
 * zlib, ancillary chunks, and one file per refusal) is probed and decoded;
 * good files must equal the generator's raw pixels exactly, bad files must
 * give the promised status.  Around that: guard bytes after the output and
 * the scratch, capacity refusals, a misaligned scratch, every truncation of
 * every good file and a byte-mutation loop (never a crash, never a write
 * past the capacity passed in).
 *
 * usage: manager_png_host_test FIXTURE_DIR
 */
#include "gameskin_fixture.h"
#include "manager_png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_BYTES 64u
#define GUARD_VALUE 0x5Au
#define MUTATION_CAPACITY (1u << 20)

static struct gs_manifest manifest;
static uint8_t scratch[MANAGER_PNG_SCRATCH_BYTES + GUARD_BYTES + 8u];

static int decode_guarded(const uint8_t *png, size_t size, size_t capacity,
                          struct manager_png_info *info, uint8_t **out)
{
    int status;

    *out = (uint8_t *)malloc(capacity + GUARD_BYTES);
    gs_fill(*out, capacity + GUARD_BYTES, GUARD_VALUE);
    gs_fill(scratch, sizeof(scratch), GUARD_VALUE);
    status = manager_png_decode(png, size, *out, capacity, scratch,
                                MANAGER_PNG_SCRATCH_BYTES, info);
    GS_CHECK(gs_untouched(*out + capacity, GUARD_BYTES, GUARD_VALUE),
             "decode wrote past the output capacity (%u)", (unsigned)capacity);
    GS_CHECK(gs_untouched(scratch + MANAGER_PNG_SCRATCH_BYTES,
                          sizeof(scratch) - MANAGER_PNG_SCRATCH_BYTES, GUARD_VALUE),
             "decode wrote past the scratch");
    return status;
}

static void test_status_texts(void)
{
    static const char *const names[] = {
        "ok", "argument", "signature", "chunk", "unsupported", "dimensions",
        "capacity", "zlib", "data", "filter"};
    int status;

    for (status = 0; status >= -9; --status)
        GS_CHECK(strcmp(manager_png_status_text(status), names[-status]) == 0,
                 "status %d", status);
    GS_CHECK(strcmp(manager_png_status_text(-10), "unknown") == 0, "unknown");
}

static void test_buffer_bytes(void)
{
    struct manager_png_info info;

    memset(&info, 0, sizeof(info));
    GS_CHECK(manager_png_buffer_bytes(&info) == 0u, "unvalidated info");
    GS_CHECK(manager_png_buffer_bytes(NULL) == 0u, "NULL info");
    info.width = 3u;
    info.height = 5u;
    info.channels = 3u;
    GS_CHECK(manager_png_buffer_bytes(&info) == 60u, "RGB 3x5: max(60, 50)");
    info.width = 1u;
    info.height = 1u;
    info.channels = 4u;
    GS_CHECK(manager_png_buffer_bytes(&info) == 5u, "RGBA 1x1: max(4, 5)");
    info.width = 4097u;
    GS_CHECK(manager_png_buffer_bytes(&info) == 0u, "over the dimension cap");
}

static void test_fixtures(void)
{
    int i;
    int seen = 0;
    int good = 0;
    long long promised = -1;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct manager_png_info probe_info, info;
        uint8_t *png, *out;
        size_t size, need;
        int probe, status;
        if (gs_is(r, "pngcount", 2)) {
            promised = gs_int(r->field[1]);
            continue;
        }
        if (!gs_is(r, "png", 7))
            continue;
        ++seen;
        png = gs_load(&manifest, r->field[1], &size);
        probe = manager_png_probe(png, size, &probe_info);
        GS_CHECK(strcmp(manager_png_status_text(probe), r->field[2]) == 0,
                 "%s probe: %s, expected %s", r->field[1],
                 manager_png_status_text(probe), r->field[2]);
        need = probe == MANAGER_PNG_OK ? manager_png_buffer_bytes(&probe_info) : 64u;
        status = decode_guarded(png, size, need, &info, &out);
        GS_CHECK(strcmp(manager_png_status_text(status), r->field[3]) == 0,
                 "%s decode: %s, expected %s", r->field[1],
                 manager_png_status_text(status), r->field[3]);
        if (probe != MANAGER_PNG_OK)
            GS_CHECK(status == probe, "%s: decode and probe disagree", r->field[1]);
        if (status == MANAGER_PNG_OK && strcmp(r->field[6], "-") != 0) {
            size_t expected_size;
            uint8_t *expected = gs_load(&manifest, r->field[6], &expected_size);
            ++good;
            GS_CHECK(info.width == gs_u32(r->field[4]) && info.height == gs_u32(r->field[5]),
                     "%s geometry %ux%u", r->field[1], info.width, info.height);
            GS_CHECK(expected_size == (size_t)info.width * info.height * 4u &&
                         memcmp(out, expected, expected_size) == 0,
                     "%s pixels differ from the generator's raw pixels", r->field[1]);
            free(expected);
            free(out);
            /* one byte short of the documented buffer */
            status = decode_guarded(png, size, need - 1u, &info, &out);
            GS_CHECK(status == MANAGER_PNG_E_CAPACITY, "%s capacity-1: %s", r->field[1],
                     manager_png_status_text(status));
            /* scratch one byte short; a misaligned scratch still works */
            GS_CHECK(manager_png_decode(png, size, out, need - 1u, scratch,
                                        MANAGER_PNG_SCRATCH_BYTES - 16u, NULL) ==
                         MANAGER_PNG_E_CAPACITY, "%s short scratch", r->field[1]);
            free(out);
            out = (uint8_t *)malloc(need);
            GS_CHECK(manager_png_decode(png, size, out, need, scratch + 3,
                                        MANAGER_PNG_SCRATCH_BYTES - 3u, NULL) ==
                         MANAGER_PNG_OK, "%s misaligned scratch", r->field[1]);
        }
        free(out);
        free(png);
    }
    GS_CHECK(promised >= 0 && seen == promised, "png records %d, manifest says %lld",
             seen, promised);
    GS_CHECK(good >= 25, "good png fixtures: %d", good);
}

/* Every strict prefix of a good file must be refused (the IEND chunk is the
 * last thing in it), without touching memory it was not given. */
static void test_truncations(void)
{
    int i;
    unsigned files = 0u;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct manager_png_info info;
        uint8_t *png, *out;
        size_t size, need, length;
        unsigned accepted = 0u;
        if (!gs_is(r, "png", 7) || strcmp(r->field[3], "ok") != 0 ||
            strstr(r->field[1], "after-iend") != NULL)
            continue;
        ++files;
        png = gs_load(&manifest, r->field[1], &size);
        GS_CHECK(manager_png_probe(png, size, &info) == MANAGER_PNG_OK, "probe");
        need = manager_png_buffer_bytes(&info);
        out = (uint8_t *)malloc(need + GUARD_BYTES);
        /* every length for files up to 16 KiB; above that ~2000 evenly
         * spread cuts plus the last 64 */
        for (length = 0u; length < size;
             length += (size <= 16384u || length + 64u >= size) ? 1u : size / 2000u) {
            /* exactly `length` bytes, so a sanitizer build sees over-reads */
            uint8_t *copy = (uint8_t *)malloc(length ? length : 1u);
            memcpy(copy, png, length);
            gs_fill(out + need, GUARD_BYTES, GUARD_VALUE);
            if (manager_png_decode(copy, length, out, need, scratch,
                                   MANAGER_PNG_SCRATCH_BYTES, NULL) == MANAGER_PNG_OK)
                ++accepted;
            GS_CHECK(gs_untouched(out + need, GUARD_BYTES, GUARD_VALUE),
                     "%s cut at %u: write past the output", r->field[1],
                     (unsigned)length);
            free(copy);
        }
        GS_CHECK(accepted == 0u, "%s: %u truncations accepted", r->field[1], accepted);
        free(out);
        free(png);
    }
    GS_CHECK(files >= 24u, "truncated files: %u", files);
}

static void test_mutations(void)
{
    uint32_t state = 0xC0FFEEu;
    int i;
    unsigned runs = 0u;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        uint8_t *png, *copy, *out;
        size_t size;
        unsigned iteration;
        if (!gs_is(r, "png", 7) || strcmp(r->field[3], "ok") != 0)
            continue;
        png = gs_load(&manifest, r->field[1], &size);
        copy = (uint8_t *)malloc(size);
        out = (uint8_t *)malloc(MUTATION_CAPACITY + GUARD_BYTES);
        for (iteration = 0u; iteration < 300u; ++iteration) {
            struct manager_png_info info;
            unsigned flips = 1u + gs_random(&state) % 3u;
            unsigned f;
            size_t capacity;
            int probe, status;
            memcpy(copy, png, size);
            for (f = 0u; f < flips; ++f)
                copy[8u + gs_random(&state) % (size - 8u)] ^=
                    (uint8_t)(1u + gs_random(&state) % 255u);
            probe = manager_png_probe(copy, size, &info);
            capacity = probe == MANAGER_PNG_OK ? manager_png_buffer_bytes(&info) : 64u;
            if (capacity > MUTATION_CAPACITY)
                capacity = MUTATION_CAPACITY;
            gs_fill(out + capacity, GUARD_BYTES, GUARD_VALUE);
            status = manager_png_decode(copy, size, out, capacity, scratch,
                                        MANAGER_PNG_SCRATCH_BYTES, NULL);
            ++runs;
            GS_CHECK(status <= 0 && status >= -9, "status %d", status);
            if (probe != MANAGER_PNG_OK)
                GS_CHECK(status == probe, "%s mutation %u: decode %d, probe %d",
                         r->field[1], iteration, status, probe);
            GS_CHECK(gs_untouched(out + capacity, GUARD_BYTES, GUARD_VALUE),
                     "%s mutation %u: write past the output", r->field[1], iteration);
        }
        free(out);
        free(copy);
        free(png);
    }
    GS_CHECK(runs >= 7500u, "mutation runs: %u", runs);
}

static void test_arguments(void)
{
    uint8_t byte = 0u;
    struct manager_png_info info;

    GS_CHECK(manager_png_probe(NULL, 8u, &info) == MANAGER_PNG_E_ARGUMENT, "probe NULL");
    GS_CHECK(manager_png_decode(NULL, 8u, &byte, 1u, scratch, sizeof(scratch), NULL) ==
                 MANAGER_PNG_E_ARGUMENT, "decode NULL png");
    GS_CHECK(manager_png_decode(&byte, 1u, NULL, 1u, scratch, sizeof(scratch), NULL) ==
                 MANAGER_PNG_E_ARGUMENT, "decode NULL output");
    GS_CHECK(manager_png_decode(&byte, 1u, &byte, 1u, NULL, 0u, NULL) ==
                 MANAGER_PNG_E_ARGUMENT, "decode NULL scratch");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURE_DIR\n", argv[0]);
        return 2;
    }
    if (gs_manifest_load(&manifest, argv[1]) != 0)
        return 1;
    test_status_texts();
    test_buffer_bytes();
    test_arguments();
    test_fixtures();
    test_truncations();
    test_mutations();
    return gs_report("manager_png_host_test");
}
