/*
 * manager_arch host test: the ARCH000 reader over
 * the in-memory blob fake, driven by the fixtures and expectations that
 * manager_gameskin_fixtures.py writes at test time (archives built with
 * tools/isaac_vita_repack.py; expected bytes decoded by its zlib path).
 *
 *   * name hash, ISAAC keystream and mount checksum against the Python
 *     vectors;
 *   * archive open / index load verdicts, entry reads (bytes, statuses,
 *     piece counts, guard bytes after the output), lookups with both
 *     prefixes, archive-set order;
 *   * blob faults: refused open, failing / early-EOF preads, short reads,
 *     failing close; no handle leaks;
 *   * the tinfl reserved-symbol guard called directly;
 *   * truncation and byte-mutation loops: never a crash, never a write past
 *     output[entry size];
 *   * the archive -> PNG -> BMFont chain: a synthetic font read from an
 *     archive, its page decoded, text drawn and compared with the canvas the
 *     generator rendered independently.
 *
 * usage: manager_arch_host_test FIXTURE_DIR
 */
#include "fake_blob.h"
#include "gameskin_fixture.h"
#include "manager_arch.h"
#include "manager_bmfont.h"
#include "manager_png.h"
#include "manager_tinfl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_BYTES 64u
#define GUARD_VALUE 0xA5u
#define MAX_ARCHIVES 24

static struct gs_manifest manifest;
static struct manager_arch_work work;

struct fixture_archive {
    char name[64];
    char path[160];
    uint8_t *data;
    size_t size;
};

static struct fixture_archive archives[MAX_ARCHIVES];
static int archive_count;

static struct fixture_archive *archive_named(const char *name)
{
    int i;

    for (i = 0; i < archive_count; ++i) {
        if (strcmp(archives[i].name, name) == 0)
            return &archives[i];
    }
    if (archive_count == MAX_ARCHIVES) {
        fprintf(stderr, "FAIL: too many fixture archives\n");
        exit(1);
    }
    snprintf(archives[archive_count].name, sizeof(archives[0].name), "%s", name);
    snprintf(archives[archive_count].path, sizeof(archives[0].path), "%s/%s",
             MANAGER_ARCH_PACKED_DIR, name);
    archives[archive_count].data =
        gs_load(&manifest, name, &archives[archive_count].size);
    return &archives[archive_count++];
}

static void fake_with_all(struct fake_blob *fake)
{
    int i;

    fake_blob_init(fake);
    for (i = 0; i < archive_count; ++i)
        (void)fake_blob_add(fake, archives[i].path, archives[i].data,
                            archives[i].size);
}

/* Opens and loads the index of a fixture archive; the index buffer is
 * malloc'd and returned through *index. */
static int open_loaded(struct manager_arch *arch, const struct manager_blob_io *io,
                       const struct fixture_archive *fixture, uint8_t **index)
{
    int status = manager_arch_open(arch, io, fixture->path);

    *index = NULL;
    if (status != MANAGER_ARCH_OK)
        return status;
    *index = (uint8_t *)malloc(manager_arch_index_bytes(arch) + 1u);
    if (*index == NULL)
        return MANAGER_ARCH_E_CAPACITY;
    status = manager_arch_load_index(arch, *index, manager_arch_index_bytes(arch));
    return status;
}

/* ---- vectors ---------------------------------------------------------- */

static void test_status_texts(void)
{
    static const char *const names[] = {
        "ok", "argument", "io", "header", "index", "capacity", "mode",
        "piece", "inflate", "size", "checksum", "path", "state"};
    int status;

    for (status = 0; status >= -12; --status)
        GS_CHECK(strcmp(manager_arch_status_text(status), names[-status]) == 0,
                 "status %d text %s", status, manager_arch_status_text(status));
    GS_CHECK(strcmp(manager_arch_status_text(-13), "unknown") == 0, "unknown");
    GS_CHECK(strcmp(manager_arch_status_text(1), "unknown") == 0, "unknown +1");
}

static void test_hash(void)
{
    uint32_t a, b, a2, b2;
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        char name[512];
        const char *slash;
        if (!gs_is(r, "hash", 4))
            continue;
        ++seen;
        gs_hex(r->field[1], name, sizeof(name));
        GS_CHECK(manager_arch_name_hash(NULL, name, &a, &b) == MANAGER_ARCH_OK,
                 "hash %s", name);
        GS_CHECK(a == gs_u32(r->field[2]) && b == gs_u32(r->field[3]),
                 "hash %s = %08x %08x", name, a, b);
        /* prefix + rest folds exactly like the whole path */
        slash = strchr(name, '/');
        if (slash != NULL) {
            char prefix[512];
            size_t length = (size_t)(slash - name) + 1u;
            memcpy(prefix, name, length);
            prefix[length] = '\0';
            GS_CHECK(manager_arch_name_hash(prefix, slash + 1, &a2, &b2) ==
                             MANAGER_ARCH_OK &&
                         a2 == a && b2 == b,
                     "split hash %s", name);
        }
    }
    GS_CHECK(seen >= 5, "hash records: %d", seen);
    a = 1u;
    GS_CHECK(manager_arch_name_hash(NULL, "font/\xd0\x96.fnt", &a, &b) ==
                 MANAGER_ARCH_E_PATH && a == 1u,
             "non-ASCII path refused, outputs untouched");
    GS_CHECK(manager_arch_name_hash("resources/\x80", "x", &a, &b) ==
                 MANAGER_ARCH_E_PATH, "non-ASCII prefix refused");
    GS_CHECK(manager_arch_name_hash(NULL, NULL, &a, &b) == MANAGER_ARCH_E_ARGUMENT,
             "NULL path");
    GS_CHECK(manager_arch_name_hash(NULL, "x", NULL, &b) == MANAGER_ARCH_E_ARGUMENT,
             "NULL hash_a");
}

static void test_keystream(void)
{
    struct manager_arch_keystream keystream;
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        uint8_t *expected;
        uint8_t *bytes;
        size_t size;
        uint32_t words;
        uint32_t w;
        if (!gs_is(r, "keystream", 4))
            continue;
        ++seen;
        words = gs_u32(r->field[2]);
        expected = gs_load(&manifest, r->field[3], &size);
        GS_CHECK(size == (size_t)words * 4u, "keystream file size");
        bytes = (uint8_t *)calloc(size + 1u, 1u);
        /* one word per 4-byte piece, the stock reader's smallest call */
        manager_arch_keystream_init(&keystream, gs_u32(r->field[1]));
        for (w = 0u; w < words; ++w)
            manager_arch_keystream_xor(&keystream, bytes + 4u * w, bytes + 4u * w, 4u);
        GS_CHECK(memcmp(bytes, expected, size) == 0, "keystream seed %s",
                 r->field[1]);
        /* one call over everything consumes the same words */
        memset(bytes, 0, size);
        manager_arch_keystream_init(&keystream, gs_u32(r->field[1]));
        manager_arch_keystream_xor(&keystream, bytes, bytes, (uint32_t)size);
        GS_CHECK(memcmp(bytes, expected, size) == 0, "keystream seed %s (one call)",
                 r->field[1]);
        free(bytes);
        free(expected);
    }
    GS_CHECK(seen >= 5, "keystream records: %d", seen);
}

static void test_xor(void)
{
    struct manager_arch_keystream keystream;
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        uint8_t *plain, *cipher, *work_bytes;
        size_t plain_size, cipher_size, pos = 0u;
        char lengths[256];
        char *token;
        if (!gs_is(r, "xor", 5))
            continue;
        ++seen;
        plain = gs_load(&manifest, r->field[2], &plain_size);
        cipher = gs_load(&manifest, r->field[3], &cipher_size);
        work_bytes = (uint8_t *)malloc(plain_size + 1u);
        memcpy(work_bytes, plain, plain_size);
        snprintf(lengths, sizeof(lengths), "%s", r->field[4]);
        manager_arch_keystream_init(&keystream, gs_u32(r->field[1]));
        /* in place, one call per piece length (ceil(n / 4) words each) */
        for (token = lengths; *token != '\0';) {
            char *comma = strchr(token, ',');
            uint32_t n = (uint32_t)strtoul(token, NULL, 10);
            if (pos + n > plain_size)
                break;
            manager_arch_keystream_xor(&keystream, work_bytes + pos,
                                       work_bytes + pos, n);
            pos += n;
            if (comma == NULL)
                break;
            token = comma + 1;
        }
        GS_CHECK(pos == plain_size && cipher_size == plain_size &&
                     memcmp(work_bytes, cipher, plain_size) == 0,
                 "xor seed %s", r->field[1]);
        free(plain);
        free(cipher);
        free(work_bytes);
    }
    GS_CHECK(seen >= 2, "xor records: %d", seen);
}

static void test_checksum(void)
{
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        uint8_t *data;
        size_t size;
        if (!gs_is(r, "checksum", 3))
            continue;
        ++seen;
        data = gs_load(&manifest, r->field[1], &size);
        GS_CHECK(manager_arch_mount_checksum(data, (uint32_t)size) ==
                     gs_u32(r->field[2]),
                 "checksum %s = %08x", r->field[1],
                 manager_arch_mount_checksum(data, (uint32_t)size));
        free(data);
    }
    GS_CHECK(seen >= 10, "checksum records: %d", seen);
}

/* ---- archives, entries, lookups -------------------------------------- */

static void test_archives(void)
{
    struct fake_blob fake;
    struct manager_blob_io io;
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct fixture_archive *fixture;
        struct manager_arch arch;
        uint8_t *index;
        int status;
        if (!gs_is(r, "archive", 6))
            continue;
        ++seen;
        fixture = archive_named(r->field[1]);
        fake_with_all(&fake);
        io = fake_blob_io(&fake);
        status = manager_arch_open(&arch, &io, fixture->path);
        GS_CHECK(strcmp(manager_arch_status_text(status), r->field[2]) == 0,
                 "%s open: %s, expected %s", r->field[1],
                 manager_arch_status_text(status), r->field[2]);
        if (status != MANAGER_ARCH_OK) {
            GS_CHECK(!arch.is_open && fake.open_handles == 0,
                     "%s: failed open leaves no handle", r->field[1]);
            GS_CHECK(manager_arch_index_bytes(&arch) == 0u, "closed index bytes");
            continue;
        }
        GS_CHECK(arch.mode == gs_u32(r->field[4]) && arch.count == gs_u32(r->field[5]),
                 "%s mode %u count %u", r->field[1], arch.mode, arch.count);
        GS_CHECK(manager_arch_index_bytes(&arch) == arch.count * 20u, "index bytes");
        index = (uint8_t *)malloc(arch.count * 20u + GUARD_BYTES);
        gs_fill(index, arch.count * 20u + GUARD_BYTES, GUARD_VALUE);
        if (arch.count != 0u)
            GS_CHECK(manager_arch_load_index(&arch, index, arch.count * 20u - 1u) ==
                         MANAGER_ARCH_E_CAPACITY,
                     "%s short index buffer", r->field[1]);
        status = manager_arch_load_index(&arch, index, arch.count * 20u);
        GS_CHECK(strcmp(manager_arch_status_text(status), r->field[3]) == 0,
                 "%s index: %s, expected %s", r->field[1],
                 manager_arch_status_text(status), r->field[3]);
        GS_CHECK(gs_untouched(index + arch.count * 20u, GUARD_BYTES, GUARD_VALUE),
                 "%s index guard", r->field[1]);
        if (status == MANAGER_ARCH_OK && arch.count != 0u) {
            struct manager_arch_entry entry;
            GS_CHECK(manager_arch_entry_at(&arch, arch.count, &entry) ==
                         MANAGER_ARCH_E_ARGUMENT, "row past the end");
            GS_CHECK(manager_arch_entry_at(&arch, arch.count - 1u, &entry) ==
                             MANAGER_ARCH_OK &&
                         entry.limit <= arch.index_offset &&
                         entry.offset <= entry.limit,
                     "last row limit");
        } else if (status != MANAGER_ARCH_OK) {
            struct manager_arch_entry entry;
            GS_CHECK(arch.index == NULL, "failed load leaves no index");
            GS_CHECK(manager_arch_entry_at(&arch, 0u, &entry) == MANAGER_ARCH_E_STATE,
                     "entry_at without index");
        }
        manager_arch_close(&arch);
        GS_CHECK(fake.open_handles == 0 && !arch.is_open, "%s closed", r->field[1]);
        free(index);
    }
    GS_CHECK(seen >= 13, "archive records: %d", seen);
}

/* Reads one row into a guarded buffer; returns the status. */
static int read_row(struct manager_arch *arch, uint32_t row, uint8_t **out,
                    struct manager_arch_entry *entry)
{
    int status = manager_arch_entry_at(arch, row, entry);

    *out = NULL;
    if (status != MANAGER_ARCH_OK)
        return status;
    *out = (uint8_t *)malloc((size_t)entry->size + GUARD_BYTES);
    gs_fill(*out, (size_t)entry->size + GUARD_BYTES, GUARD_VALUE);
    return manager_arch_read(arch, entry, *out, entry->size, &work);
}

static void test_entries(uint32_t short_read_cap)
{
    struct fake_blob fake;
    struct manager_blob_io io;
    int i;
    int seen = 0;
    int compared = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct fixture_archive *fixture;
        struct manager_arch arch;
        struct manager_arch_entry entry;
        uint8_t *index;
        uint8_t *out;
        uint32_t row;
        int status;
        if (!gs_is(r, "entry", 7))
            continue;
        ++seen;
        fixture = archive_named(r->field[1]);
        fake_with_all(&fake);
        fake.short_read_cap = short_read_cap;
        io = fake_blob_io(&fake);
        GS_CHECK(open_loaded(&arch, &io, fixture, &index) == MANAGER_ARCH_OK,
                 "%s opens", r->field[1]);
        row = gs_u32(r->field[2]);
        status = read_row(&arch, row, &out, &entry);
        GS_CHECK(strcmp(manager_arch_status_text(status), r->field[3]) == 0,
                 "%s row %u: %s, expected %s (cap %u)", r->field[1], row,
                 manager_arch_status_text(status), r->field[3], short_read_cap);
        if (out != NULL) {
            GS_CHECK(gs_untouched(out + entry.size, GUARD_BYTES, GUARD_VALUE),
                     "%s row %u wrote past the entry size", r->field[1], row);
        }
        if (strcmp(r->field[4], "-") != 0 && out != NULL) {
            size_t size;
            uint8_t *expected = gs_load(&manifest, r->field[4], &size);
            GS_CHECK(size == entry.size && memcmp(out, expected, size) == 0,
                     "%s row %u bytes differ from the Python decode", r->field[1], row);
            ++compared;
            free(expected);
        }
        if (status == MANAGER_ARCH_OK) {
            GS_CHECK(work.deflate_pieces == gs_u32(r->field[5]) &&
                         work.stored_pieces == gs_u32(r->field[6]),
                     "%s row %u pieces %u/%u", r->field[1], row,
                     work.deflate_pieces, work.stored_pieces);
            if (entry.size != 0u)
                GS_CHECK(manager_arch_read(&arch, &entry, out, entry.size - 1u, &work) ==
                             MANAGER_ARCH_E_CAPACITY,
                         "%s row %u capacity", r->field[1], row);
            else
                GS_CHECK(manager_arch_read(&arch, &entry, NULL, 0u, &work) ==
                                 MANAGER_ARCH_OK && work.preads == 0u,
                         "size-0 row reads nothing");
        }
        free(out);
        manager_arch_close(&arch);
        GS_CHECK(fake.open_handles == 0, "no leaked handle");
        free(index);
    }
    GS_CHECK(seen >= 20 && compared >= 14, "entry records: %d, compared %d", seen,
             compared);
}

static void test_find(void)
{
    struct fake_blob fake;
    struct manager_blob_io io;
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct manager_arch arch;
        struct manager_arch_entry entry;
        uint8_t *index;
        char path[256];
        long long row;
        int found;
        if (!gs_is(r, "find", 5))
            continue;
        ++seen;
        fake_with_all(&fake);
        io = fake_blob_io(&fake);
        GS_CHECK(open_loaded(&arch, &io, archive_named(r->field[1]), &index) ==
                     MANAGER_ARCH_OK, "find: open");
        gs_hex(r->field[3], path, sizeof(path));
        row = gs_int(r->field[4]);
        found = manager_arch_find(&arch, strcmp(r->field[2], "-") == 0 ? NULL : r->field[2],
                                  path, &entry);
        GS_CHECK(found == (row >= 0 ? 1 : 0) && (row < 0 || entry.row == (uint32_t)row),
                 "find %s%s -> %d row %u, expected %lld", r->field[2], path, found,
                 entry.row, row);
        manager_arch_close(&arch);
        free(index);
    }
    GS_CHECK(seen >= 8, "find records: %d", seen);
}

static void test_sets(void)
{
    struct fake_blob fake;
    struct manager_blob_io io;
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct manager_arch arch[MANAGER_ARCH_SET_MAX];
        uint8_t *index[MANAGER_ARCH_SET_MAX];
        struct manager_arch_set set;
        int a, j;
        if (strcmp(r->field[0], "set") != 0)
            continue;
        fake_with_all(&fake);
        io = fake_blob_io(&fake);
        memset(&set, 0, sizeof(set));
        for (a = 2; a < r->count && set.count < MANAGER_ARCH_SET_MAX; ++a) {
            GS_CHECK(open_loaded(&arch[set.count], &io, archive_named(r->field[a]),
                                 &index[set.count]) == MANAGER_ARCH_OK,
                     "set %s: %s", r->field[1], r->field[a]);
            set.archives[set.count] = &arch[set.count];
            ++set.count;
        }
        for (j = 0; j < manifest.count; ++j) {
            const struct gs_record *q = &manifest.record[j];
            struct manager_arch_entry entry;
            uint32_t which = 99u;
            char path[256];
            long long expected_archive;
            int found;
            if (!gs_is(q, "setfind", 5) || strcmp(q->field[1], r->field[1]) != 0)
                continue;
            ++seen;
            gs_hex(q->field[2], path, sizeof(path));
            expected_archive = gs_int(q->field[3]);
            found = manager_arch_set_find(&set, path, &which, &entry);
            GS_CHECK(found == (expected_archive >= 0 ? 1 : 0) &&
                         (expected_archive < 0 ||
                          (which == (uint32_t)expected_archive &&
                           entry.row == (uint32_t)gs_int(q->field[4]))),
                     "set %s find %s -> %d archive %u row %u", r->field[1], path,
                     found, which, entry.row);
        }
        {
            struct manager_arch_entry entry;
            uint32_t which;
            GS_CHECK(manager_arch_set_find(&set, "gs/\xff", &which, &entry) ==
                         MANAGER_ARCH_E_PATH, "set find refuses non-ASCII");
        }
        for (a = 0; a < (int)set.count; ++a) {
            manager_arch_close(&arch[a]);
            free(index[a]);
        }
        GS_CHECK(fake.open_handles == 0, "set closed");
    }
    GS_CHECK(seen >= 5, "setfind records: %d", seen);
}

/* ---- blob faults ------------------------------------------------------ */

static void test_io_faults(void)
{
    struct fixture_archive *good = archive_named("good.a");
    struct fake_blob fake;
    struct manager_blob_io io;
    struct manager_arch arch;
    struct manager_arch_entry entry;
    uint8_t *index;
    uint8_t *out;
    unsigned preads_to_open;
    unsigned n;

    fake_with_all(&fake);
    io = fake_blob_io(&fake);
    fake.refuse_open_path = good->path;
    GS_CHECK(manager_arch_open(&arch, &io, good->path) == MANAGER_ARCH_E_IO &&
                 fake.open_handles == 0, "refused open");
    GS_CHECK(manager_arch_open(&arch, &io, "ux0:/nowhere.a") == MANAGER_ARCH_E_IO,
             "absent file");

    /* the Nth pread fails / hits EOF, for every N an open + load + read of
     * the stored row needs */
    fake_with_all(&fake);
    io = fake_blob_io(&fake);
    GS_CHECK(open_loaded(&arch, &io, good, &index) == MANAGER_ARCH_OK, "baseline");
    GS_CHECK(manager_arch_find(&arch, "resources/", "gs/mixed.bin", &entry) == 1,
             "mixed row");
    out = (uint8_t *)malloc(entry.size + GUARD_BYTES);
    GS_CHECK(manager_arch_read(&arch, &entry, out, entry.size, &work) == MANAGER_ARCH_OK,
             "baseline read");
    preads_to_open = fake.preads;
    manager_arch_close(&arch);
    free(index);
    for (n = 1u; n <= preads_to_open; ++n) {
        int mode;
        for (mode = 0; mode < 2; ++mode) {
            int status;
            fake_with_all(&fake);
            io = fake_blob_io(&fake);
            if (mode == 0)
                fake.fail_pread_at = n;
            else
                fake.eof_pread_at = n;
            status = open_loaded(&arch, &io, good, &index);
            if (status == MANAGER_ARCH_OK) {
                GS_CHECK(manager_arch_find(&arch, "resources/", "gs/mixed.bin", &entry) == 1,
                         "fault find");
                status = manager_arch_read(&arch, &entry, out, entry.size, &work);
            }
            GS_CHECK(status == MANAGER_ARCH_E_IO, "pread %u %s -> %s", n,
                     mode ? "eof" : "error", manager_arch_status_text(status));
            manager_arch_close(&arch);
            free(index);
            GS_CHECK(fake.open_handles == 0, "fault %u leaves no handle", n);
        }
    }

    /* a failing close is ignored and the archive is reset anyway */
    fake_with_all(&fake);
    io = fake_blob_io(&fake);
    fake.fail_close = 1;
    GS_CHECK(open_loaded(&arch, &io, good, &index) == MANAGER_ARCH_OK, "fail_close open");
    manager_arch_close(&arch);
    GS_CHECK(!arch.is_open && arch.handle == -1 && fake.open_handles == 0,
             "failing close still resets the archive");
    free(index);

    /* state errors */
    manager_arch_init(&arch);
    GS_CHECK(manager_arch_load_index(&arch, out, 16u) == MANAGER_ARCH_E_STATE,
             "load before open");
    GS_CHECK(manager_arch_read(&arch, &entry, out, entry.size, &work) ==
                 MANAGER_ARCH_E_STATE, "read before open");
    GS_CHECK(manager_arch_find(&arch, NULL, "x", &entry) == MANAGER_ARCH_E_STATE,
             "find before open");
    GS_CHECK(manager_arch_open(NULL, &io, good->path) == MANAGER_ARCH_E_ARGUMENT,
             "NULL arch");
    GS_CHECK(manager_arch_open(&arch, NULL, good->path) == MANAGER_ARCH_E_ARGUMENT,
             "NULL io");
    manager_arch_close(NULL);
    free(out);
}

/* An entry that does not describe the archive is refused before any read. */
static void test_foreign_entry(void)
{
    struct fake_blob fake;
    struct manager_blob_io io;
    struct manager_arch arch;
    struct manager_arch_entry entry;
    uint8_t *index;
    uint8_t out[16];

    fake_with_all(&fake);
    io = fake_blob_io(&fake);
    GS_CHECK(open_loaded(&arch, &io, archive_named("good.a"), &index) == MANAGER_ARCH_OK,
             "open");
    GS_CHECK(manager_arch_entry_at(&arch, 0u, &entry) == MANAGER_ARCH_OK, "row 0");
    entry.size = 8u;
    entry.limit = arch.index_offset + 1u;
    GS_CHECK(manager_arch_read(&arch, &entry, out, sizeof(out), &work) ==
                 MANAGER_ARCH_E_ARGUMENT, "limit past the index");
    entry.offset = 3u;
    entry.limit = 20u;
    GS_CHECK(manager_arch_read(&arch, &entry, out, sizeof(out), &work) ==
                 MANAGER_ARCH_E_ARGUMENT, "offset inside the header");
    manager_arch_close(&arch);
    free(index);
}

/* ---- the tinfl guard -------------------------------------------------- */

static void test_tinfl_reserved(void)
{
    static const char *const cases[][2] = {
        {"gs/reserved-length.bin", "4"},
        {"gs/reserved-distance.bin", "7"},
        {"gs/tiny.txt", "300"},
    };
    struct fixture_archive *good = archive_named("good.a");
    struct fake_blob fake;
    struct manager_blob_io io;
    struct manager_arch arch;
    uint8_t *index;
    size_t c;

    fake_with_all(&fake);
    io = fake_blob_io(&fake);
    GS_CHECK(open_loaded(&arch, &io, good, &index) == MANAGER_ARCH_OK, "open");
    for (c = 0u; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        struct manager_arch_entry entry;
        uint32_t header, length, in_size, out_size, reserved = 0u;
        uint32_t want = (uint32_t)atoi(cases[c][1]);
        uint8_t output[300 + GUARD_BYTES];
        int status;
        GS_CHECK(manager_arch_find(&arch, "resources/", cases[c][0], &entry) == 1,
                 "find %s", cases[c][0]);
        header = (uint32_t)good->data[entry.offset] |
                 ((uint32_t)good->data[entry.offset + 1u] << 8) |
                 ((uint32_t)good->data[entry.offset + 2u] << 16) |
                 ((uint32_t)good->data[entry.offset + 3u] << 24);
        length = header & 0x7FFFFFFFu;
        in_size = length;
        out_size = want;
        gs_fill(output, sizeof(output), GUARD_VALUE);
        memset(work.tinfl_state, 0, sizeof(work.tinfl_state));
        /* the output ends exactly where the reserved match sits */
        status = manager_tinfl_decompress(work.tinfl_state,
                                          good->data + entry.offset + 4u, &in_size,
                                          output, output, &out_size,
                                          MANAGER_TINFL_NON_WRAPPING_OUTPUT, &reserved);
        GS_CHECK(gs_untouched(output + want, GUARD_BYTES, GUARD_VALUE),
                 "%s: tinfl wrote past the output", cases[c][0]);
        if (c < 2u)
            GS_CHECK(reserved == 1u, "%s: reserved symbol flagged (status %d)",
                     cases[c][0], status);
        else
            GS_CHECK(reserved == 0u && status == ISAAC_VITA_ARCHIVE_MINIZ_DONE &&
                         out_size == want,
                     "normal stream not flagged");
    }
    manager_arch_close(&arch);
    free(index);
}

/* ---- robustness loops ------------------------------------------------- */

static void read_all_rows(struct manager_arch *arch, uint32_t max_size,
                          const char *label, unsigned iteration)
{
    uint32_t row;

    for (row = 0u; row < arch->count; ++row) {
        struct manager_arch_entry entry;
        uint8_t *out;
        int status;
        if (manager_arch_entry_at(arch, row, &entry) != MANAGER_ARCH_OK)
            continue;
        if (entry.size > max_size) {
            uint8_t small[16];
            GS_CHECK(manager_arch_read(arch, &entry, small, sizeof(small), &work) ==
                         MANAGER_ARCH_E_CAPACITY, "%s: capacity", label);
            continue;
        }
        out = (uint8_t *)malloc((size_t)entry.size + GUARD_BYTES);
        gs_fill(out, (size_t)entry.size + GUARD_BYTES, GUARD_VALUE);
        status = manager_arch_read(arch, &entry, out, entry.size, &work);
        GS_CHECK(status <= 0 && status >= -12, "%s: status %d", label, status);
        GS_CHECK(gs_untouched(out + entry.size, GUARD_BYTES, GUARD_VALUE),
                 "%s %u row %u: write past the output", label, iteration, row);
        free(out);
    }
}

static void test_truncation(void)
{
    struct fixture_archive *good = archive_named("good.a");
    struct fake_blob fake;
    struct manager_blob_io io;
    size_t length;
    int opened = 0;

    for (length = 0u; length < good->size; ++length) {
        struct manager_arch arch;
        fake_blob_init(&fake);
        (void)fake_blob_add(&fake, "cut.a", good->data, length);
        io = fake_blob_io(&fake);
        if (manager_arch_open(&arch, &io, "cut.a") == MANAGER_ARCH_OK)
            ++opened;
        manager_arch_close(&arch);
        GS_CHECK(fake.open_handles == 0, "truncation %u leaks", (unsigned)length);
    }
    GS_CHECK(opened == 0, "%d truncated archives opened", opened);
}

static void test_mutations(void)
{
    struct fixture_archive *good = archive_named("good.a");
    uint8_t *copy = (uint8_t *)malloc(good->size);
    uint32_t index_offset = (uint32_t)good->data[8] | ((uint32_t)good->data[9] << 8) |
                            ((uint32_t)good->data[10] << 16) |
                            ((uint32_t)good->data[11] << 24);
    uint32_t state = 0x1234567u;
    unsigned iteration;

    for (iteration = 0u; iteration < 600u; ++iteration) {
        struct fake_blob fake;
        struct manager_blob_io io;
        struct manager_arch arch;
        uint8_t *index;
        unsigned flips = 1u + gs_random(&state) % 4u;
        unsigned f;
        memcpy(copy, good->data, good->size);
        for (f = 0u; f < flips; ++f) {
            /* 3 of 4 iterations hit the payload area, the rest the index */
            size_t at = iteration % 4u != 3u
                            ? 14u + gs_random(&state) % (index_offset - 14u)
                            : index_offset + gs_random(&state) % (good->size - index_offset);
            copy[at] ^= (uint8_t)(1u + gs_random(&state) % 255u);
        }
        fake_blob_init(&fake);
        (void)fake_blob_add(&fake, "m.a", copy, good->size);
        fake.short_read_cap = iteration % 5u == 0u ? 13u : 0u;
        io = fake_blob_io(&fake);
        if (open_loaded(&arch, &io, &(struct fixture_archive){"m", "m.a", copy, 0},
                        &index) == MANAGER_ARCH_OK)
            read_all_rows(&arch, 1u << 20, "mutation", iteration);
        manager_arch_close(&arch);
        free(index);
        GS_CHECK(fake.open_handles == 0, "mutation %u leaks", iteration);
    }
    free(copy);
}

/* ---- archive -> PNG -> BMFont ----------------------------------------- */

static void test_chain(void)
{
    int i;
    int seen = 0;

    for (i = 0; i < manifest.count; ++i) {
        const struct gs_record *r = &manifest.record[i];
        struct fake_blob fake;
        struct manager_blob_io io;
        struct manager_arch arch;
        struct manager_arch_set set;
        struct manager_arch_entry entry;
        struct manager_png_info info;
        struct manager_bmfont font;
        struct manager_bmfont_glyph *glyphs;
        struct manager_bmfont_surface surface;
        uint8_t *index, *fnt, *png, *rgba, *expected_a8, *expected_canvas;
        uint32_t *canvas;
        uint8_t scratch[MANAGER_PNG_SCRATCH_BYTES];
        char fnt_path[256], page_path[320], text[256];
        const char *slash;
        size_t expected_size, text_length;
        uint32_t which, rows, non_white = 99u, x, y, width, height, pitch;
        int32_t pen;
        if (!gs_is(r, "chain", 16))
            continue;
        ++seen;
        fake_with_all(&fake);
        io = fake_blob_io(&fake);
        GS_CHECK(open_loaded(&arch, &io, archive_named(r->field[1]), &index) ==
                     MANAGER_ARCH_OK, "chain open");
        memset(&set, 0, sizeof(set));
        set.archives[0] = &arch;
        set.count = 1u;
        gs_hex(r->field[2], fnt_path, sizeof(fnt_path));
        text_length = gs_hex(r->field[3], text, sizeof(text));

        GS_CHECK(manager_arch_set_find(&set, fnt_path, &which, &entry) == 1, "fnt found");
        fnt = (uint8_t *)malloc(entry.size + 1u);
        GS_CHECK(manager_arch_read(&arch, &entry, fnt, entry.size, &work) ==
                     MANAGER_ARCH_OK, "fnt read");
        rows = manager_bmfont_glyph_rows(fnt, entry.size);
        GS_CHECK(rows == gs_u32(r->field[14]), "glyph rows %u", rows);
        glyphs = (struct manager_bmfont_glyph *)calloc(rows + 1u, sizeof(*glyphs));
        GS_CHECK(manager_bmfont_parse(&font, fnt, entry.size, glyphs, rows) ==
                     MANAGER_BMFONT_OK, "fnt parse");
        free(fnt); /* glyphs are copied */

        /* the page name is relative to the .fnt's directory */
        slash = strrchr(fnt_path, '/');
        snprintf(page_path, sizeof(page_path), "%.*s%s",
                 slash ? (int)(slash - fnt_path + 1) : 0, fnt_path, font.page_name);
        GS_CHECK(manager_arch_set_find(&set, page_path, &which, &entry) == 1,
                 "page %s found", page_path);
        png = (uint8_t *)malloc(entry.size + 1u);
        GS_CHECK(manager_arch_read(&arch, &entry, png, entry.size, &work) ==
                     MANAGER_ARCH_OK, "page read");
        GS_CHECK(manager_png_probe(png, entry.size, &info) == MANAGER_PNG_OK, "page probe");
        rgba = (uint8_t *)malloc(manager_png_buffer_bytes(&info) + 1u);
        GS_CHECK(manager_png_decode(png, entry.size, rgba, manager_png_buffer_bytes(&info),
                                    scratch, sizeof(scratch), &info) == MANAGER_PNG_OK,
                 "page decode");
        manager_bmfont_rgba_to_a8(rgba, rgba, info.width * info.height, &non_white);
        GS_CHECK(non_white == 0u, "page is white-only (%u)", non_white);
        expected_a8 = gs_load(&manifest, r->field[15], &expected_size);
        GS_CHECK(expected_size == (size_t)info.width * info.height &&
                     memcmp(rgba, expected_a8, expected_size) == 0, "A8 page");
        GS_CHECK(manager_bmfont_attach_page(&font, rgba, info.width, info.height) ==
                     MANAGER_BMFONT_OK, "attach");

        width = gs_u32(r->field[9]);
        height = gs_u32(r->field[10]);
        pitch = width + 5u; /* the 5 extra columns must stay untouched */
        canvas = (uint32_t *)malloc(((size_t)pitch * (height + 2u)) * 4u);
        for (y = 0u; y < height + 2u; ++y)
            for (x = 0u; x < pitch; ++x)
                canvas[(size_t)y * pitch + x] =
                    (y < height && x < width) ? gs_u32(r->field[8]) : 0xDEADBEEFu;
        manager_bmfont_surface_init(&surface, canvas, width, height, pitch);
        GS_CHECK(manager_bmfont_measure(&font, text, text_length,
                                        gs_u32(r->field[6])) ==
                     (int32_t)gs_int(r->field[12]), "measure");
        pen = manager_bmfont_draw(&font, &surface, (int32_t)gs_int(r->field[4]),
                                  (int32_t)gs_int(r->field[5]), text, text_length,
                                  gs_u32(r->field[7]), gs_u32(r->field[6]));
        GS_CHECK(pen == (int32_t)gs_int(r->field[13]), "pen %d", pen);
        expected_canvas = gs_load(&manifest, r->field[11], &expected_size);
        GS_CHECK(expected_size == (size_t)width * height * 4u, "canvas size");
        {
            uint32_t differing = 0u, pad = 0u;
            for (y = 0u; y < height + 2u; ++y) {
                for (x = 0u; x < pitch; ++x) {
                    uint32_t got = canvas[(size_t)y * pitch + x];
                    if (y < height && x < width) {
                        const uint8_t *e = expected_canvas + ((size_t)y * width + x) * 4u;
                        uint32_t want = (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
                                        ((uint32_t)e[2] << 16) | ((uint32_t)e[3] << 24);
                        differing += got != want;
                    } else {
                        pad += got != 0xDEADBEEFu;
                    }
                }
            }
            GS_CHECK(differing == 0u && pad == 0u,
                     "chain %s: %u pixels differ from the generator's render, %u pad "
                     "words touched", r->field[11], differing, pad);
        }
        free(expected_canvas);
        free(canvas);
        free(expected_a8);
        free(rgba);
        free(png);
        free(glyphs);
        manager_arch_close(&arch);
        free(index);
    }
    GS_CHECK(seen >= 3, "chain records: %d", seen);
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
    test_hash();
    test_keystream();
    test_xor();
    test_checksum();
    test_archives();
    test_entries(0u);
    test_entries(7u);
    test_find();
    test_sets();
    test_io_faults();
    test_foreign_entry();
    test_tinfl_reserved();
    test_truncation();
    test_mutations();
    test_chain();
    return gs_report("manager_arch_host_test");
}
