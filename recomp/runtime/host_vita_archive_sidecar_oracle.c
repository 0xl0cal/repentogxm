/* Host oracle for host_vita_archive_sidecar.c: the Vita sceIo edge is
 * replaced by POSIX calls on a host directory that stands in for
 * ux0:/data/isaacr001, and the receipt wrap is replaced by explicit tokens.
 *
 * usage: archive-sidecar-host-oracle <host root> <fixture archive base name>
 *
 * The fixture is <host root>/resources/packed/<base> with its sidecar
 * <base>.receipt as written by tools/isaac_vita_halve_sfx.py.  The oracle
 * derives mutated copies of both next to it and checks that only the exact
 * pair is trusted, and that every mutation is refused at the expected stage. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guest_pe.h"
#include "host_vita_archive_receipt.h"
#include "host_vita_archive_sidecar.h"

#define ROOT "ux0:/data/isaacr001"
#define PACKED ROOT "/resources/packed/"

static char s_root[PATH_MAX];
static char s_base[64];
static unsigned char *s_archive;
static size_t s_archive_size;
static unsigned char s_sidecar[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE];
static uint64_t s_read_bytes;
static int s_fail_pread_nth;
static int s_pread_seen;

static int fail(unsigned line, const char *expression)
{
    isaac_vita_archive_sidecar_diagnostic d;
    isaac_vita_archive_sidecar_get_diagnostic(&d);
    fprintf(stderr,
            "archive-sidecar oracle failed at line %u: %s (stage %u:%s io=%d "
            "detail=0x%x value0=0x%x value1=0x%x)\n",
            line, expression, (unsigned)d.stage,
            isaac_vita_archive_sidecar_stage_name(d.stage), (int)d.io_result,
            (unsigned)d.detail, (unsigned)d.value0, (unsigned)d.value1);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

static int translate_path(const char *path, char output[PATH_MAX])
{
    int length;
    if (strncmp(path, ROOT, sizeof ROOT - 1U) != 0)
        return 0;
    length = snprintf(output, PATH_MAX, "%s%s", s_root, path + sizeof ROOT - 1U);
    return length >= 0 && (size_t)length < PATH_MAX;
}

int isaac_vita_archive_sidecar_oracle_stat(const char *path, uint64_t *size,
                                           int *regular)
{
    char translated[PATH_MAX];
    struct stat native;
    if (!translate_path(path, translated) || stat(translated, &native) != 0)
        return -1;
    *size = (uint64_t)native.st_size;
    *regular = S_ISREG(native.st_mode) ? 1 : 0;
    return 0;
}

int isaac_vita_archive_sidecar_oracle_fstat(int descriptor, uint64_t *size,
                                            int *regular)
{
    struct stat native;
    if (fstat(descriptor, &native) != 0)
        return -1;
    *size = (uint64_t)native.st_size;
    *regular = S_ISREG(native.st_mode) ? 1 : 0;
    return 0;
}

int isaac_vita_archive_sidecar_oracle_open(const char *path)
{
    char translated[PATH_MAX];
    if (!translate_path(path, translated))
        return -1;
    return open(translated, O_RDONLY);
}

int isaac_vita_archive_sidecar_oracle_close(int descriptor)
{
    return close(descriptor) == 0 ? 0 : -1;
}

int isaac_vita_archive_sidecar_oracle_pread(int descriptor, void *buffer,
                                            uint32_t size, uint64_t offset)
{
    ssize_t result;
    ++s_pread_seen;
    if (s_fail_pread_nth && s_pread_seen == s_fail_pread_nth)
        return -1;
    result = pread(descriptor, buffer, size, (off_t)offset);
    if (result > 0)
        s_read_bytes += (uint64_t)result;
    return result < 0 || result > INT_MAX ? -1 : (int)result;
}

/* ---- fixture handling -------------------------------------------------- */

static int host_packed(const char *name, char output[PATH_MAX])
{
    int length = snprintf(output, PATH_MAX, "%s/resources/packed/%s", s_root, name);
    return length >= 0 && (size_t)length < PATH_MAX;
}

static int read_file(const char *name, unsigned char **bytes, size_t *size)
{
    char path[PATH_MAX];
    struct stat metadata;
    FILE *file;
    if (!host_packed(name, path) || stat(path, &metadata) != 0)
        return 0;
    *size = (size_t)metadata.st_size;
    *bytes = (unsigned char *)malloc(*size + 1U);
    if (!*bytes)
        return 0;
    file = fopen(path, "rb");
    if (!file)
        return 0;
    if (fread(*bytes, 1U, *size, file) != *size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int write_file(const char *name, const unsigned char *bytes, size_t size)
{
    char path[PATH_MAX];
    FILE *file;
    if (!host_packed(name, path))
        return 0;
    file = fopen(path, "wb");
    if (!file)
        return 0;
    if (size && fwrite(bytes, 1U, size, file) != size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int remove_file(const char *name)
{
    char path[PATH_MAX];
    if (!host_packed(name, path))
        return 0;
    return unlink(path) == 0 || errno == ENOENT;
}

static void rehash_sidecar(unsigned char bytes[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE])
{
    guest_pe_sha256(bytes, 224U, bytes + 224U);
}

/* Publish <variant>/<variant>.receipt from the given images (the sidecar's
 * name field must already say `variant` when it should match). */
static int publish(const char *variant, const unsigned char *archive,
                   size_t archive_size, const unsigned char *sidecar)
{
    char sidecar_name[96];
    snprintf(sidecar_name, sizeof sidecar_name, "%s%s", variant,
             ISAAC_VITA_ARCHIVE_SIDECAR_SUFFIX);
    return write_file(variant, archive, archive_size) &&
           (sidecar ? write_file(sidecar_name, sidecar,
                                 ISAAC_VITA_ARCHIVE_SIDECAR_SIZE)
                    : remove_file(sidecar_name));
}

static void sidecar_for_name(unsigned char out[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE],
                             const char *variant)
{
    memcpy(out, s_sidecar, ISAAC_VITA_ARCHIVE_SIDECAR_SIZE);
    memset(out + 128U, 0, 40U);
    memcpy(out + 128U, variant, strlen(variant));
    rehash_sidecar(out);
}

static uint32_t trust(const char *variant, uint32_t real_token)
{
    char guest_name[96];
    snprintf(guest_name, sizeof guest_name, "%s%s",
             ISAAC_VITA_ARCHIVE_SIDECAR_PACKED_PREFIX, variant);
    s_pread_seen = 0;
    return isaac_vita_archive_sidecar_trust(guest_name, real_token);
}

static uint32_t stage(void)
{
    isaac_vita_archive_sidecar_diagnostic d;
    isaac_vita_archive_sidecar_get_diagnostic(&d);
    return d.stage;
}

static int test_exact_pair(void)
{
    uint32_t token;
    uint64_t region = (uint64_t)((uint32_t)s_archive[8] | ((uint32_t)s_archive[9] << 8U) |
                                 ((uint32_t)s_archive[10] << 16U) | ((uint32_t)s_archive[11] << 24U));
    uint32_t entries = (uint32_t)s_archive[12] | ((uint32_t)s_archive[13] << 8U);

    s_read_bytes = 0U;
    token = trust(s_base, 0U);
    CHECK(token == (ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED |
                    ISAAC_VITA_ARCHIVE_SIDECAR_TOKEN));
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_TRUSTED);
    /* sidecar + header + index + samples, never the entry bodies in full */
    CHECK(s_read_bytes >= ISAAC_VITA_ARCHIVE_SIDECAR_SIZE + 14U + entries * 20U);
    if (region > 64U * 4096U)
        CHECK(s_read_bytes <= ISAAC_VITA_ARCHIVE_SIDECAR_SIZE + 14U +
                                  entries * 20U + 64U * 4096U);
    else
        CHECK(s_read_bytes == ISAAC_VITA_ARCHIVE_SIDECAR_SIZE + 14U +
                                  entries * 20U + region);
    /* the pinned kinds keep their kind byte */
    token = trust(s_base, 2U);
    CHECK(token == (2U | ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED |
                    ISAAC_VITA_ARCHIVE_SIDECAR_TOKEN));
    /* an already trusted token is returned untouched without I/O */
    s_read_bytes = 0U;
    token = trust(s_base, 0x101U);
    CHECK(token == 0x101U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ALREADY_TRUSTED);
    CHECK(s_read_bytes == 0U);
    return 0;
}

static int test_names(void)
{
    CHECK(trust("../" "x.a", 7U) == 7U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME);
    CHECK(isaac_vita_archive_sidecar_trust("resources/other/x.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME);
    CHECK(isaac_vita_archive_sidecar_trust(
              "resources/packed/0123456789012345678901234567890123456789.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME);
    CHECK(isaac_vita_archive_sidecar_trust(NULL, 3U) == 3U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME);
    CHECK(isaac_vita_archive_sidecar_trust("resources/packed/", 0U) == 0U);
    /* an archive without a sidecar keeps the real token */
    CHECK(publish("plain.a", s_archive, s_archive_size, NULL));
    CHECK(trust("plain.a", 2U) == 2U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_ABSENT);
    return 0;
}

static int test_sidecar_mutations(void)
{
    unsigned char altered[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE + 1U];
    uint32_t sizes[2] = { 255U, 257U };
    size_t index;

    /* wrong length */
    for (index = 0U; index < 2U; ++index) {
        sidecar_for_name(altered, "m.a");
        altered[256] = 0U;
        CHECK(write_file("m.a", s_archive, s_archive_size));
        CHECK(write_file("m.a.receipt", altered, sizes[index]));
        CHECK(trust("m.a", 0U) == 0U);
        CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_IDENTITY);
    }
    /* digest flip, magic flip, reserved byte, version, sample scheme, geometry */
    sidecar_for_name(altered, "m.a");
    altered[240] ^= 1U;
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE);

    sidecar_for_name(altered, "m.a");
    altered[3] ^= 1U;
    rehash_sidecar(altered);
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE);

    sidecar_for_name(altered, "m.a");
    altered[60] = 1U;
    rehash_sidecar(altered);
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE);

    sidecar_for_name(altered, "m.a");
    altered[16] = 2U;
    rehash_sidecar(altered);
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE);

    sidecar_for_name(altered, "m.a");
    altered[44] = 32U;
    rehash_sidecar(altered);
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE);

    sidecar_for_name(altered, "m.a");
    altered[36] ^= 1U;   /* entries no longer match the size */
    rehash_sidecar(altered);
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE);

    /* a sidecar written for another name (copied or renamed) */
    sidecar_for_name(altered, "other.a");
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_NAME);

    /* the exact sidecar under a new name is trusted again */
    sidecar_for_name(altered, "m.a");
    CHECK(publish("m.a", s_archive, s_archive_size, altered));
    CHECK(trust("m.a", 0U) == (ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED |
                               ISAAC_VITA_ARCHIVE_SIDECAR_TOKEN));
    return 0;
}

static int test_archive_mutations(void)
{
    unsigned char sidecar[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE];
    unsigned char *copy = (unsigned char *)malloc(s_archive_size + 1U);
    uint32_t index_offset = (uint32_t)s_archive[8] | ((uint32_t)s_archive[8 + 1] << 8U) |
                            ((uint32_t)s_archive[8 + 2] << 16U) | ((uint32_t)s_archive[8 + 3] << 24U);

    CHECK(copy != NULL);
    sidecar_for_name(sidecar, "a.a");

    /* truncated / extended copy */
    memcpy(copy, s_archive, s_archive_size);
    CHECK(publish("a.a", copy, s_archive_size - 1U, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_IDENTITY);
    copy[s_archive_size] = 0U;
    CHECK(publish("a.a", copy, s_archive_size + 1U, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_IDENTITY);

    /* header: mode byte, entry count */
    memcpy(copy, s_archive, s_archive_size);
    copy[7] ^= 1U;
    CHECK(publish("a.a", copy, s_archive_size, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER);
    memcpy(copy, s_archive, s_archive_size);
    copy[12] ^= 1U;
    CHECK(publish("a.a", copy, s_archive_size, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER);

    /* one bit in an index checksum */
    memcpy(copy, s_archive, s_archive_size);
    copy[index_offset + 16U] ^= 1U;
    CHECK(publish("a.a", copy, s_archive_size, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_HASH);

    /* one bit in the entry region (first and last window) */
    memcpy(copy, s_archive, s_archive_size);
    copy[14U + 5U] ^= 1U;
    CHECK(publish("a.a", copy, s_archive_size, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_HASH);
    memcpy(copy, s_archive, s_archive_size);
    copy[index_offset - 1U] ^= 1U;
    CHECK(publish("a.a", copy, s_archive_size, sidecar));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_HASH);

    /* missing archive, sidecar present */
    CHECK(remove_file("a.a"));
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_STAT);

    /* I/O failure inside the archive read falls back untrusted */
    memcpy(copy, s_archive, s_archive_size);
    CHECK(publish("a.a", copy, s_archive_size, sidecar));
    s_fail_pread_nth = 2;   /* the archive header read */
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER_READ);
    s_fail_pread_nth = 1;   /* the sidecar read */
    CHECK(trust("a.a", 0U) == 0U);
    CHECK(stage() == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_READ);
    s_fail_pread_nth = 0;
    CHECK(trust("a.a", 0U) == (ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED |
                               ISAAC_VITA_ARCHIVE_SIDECAR_TOKEN));
    free(copy);
    return 0;
}

int main(int argc, char **argv)
{
    char sidecar_name[96];
    unsigned char *sidecar_bytes;
    size_t sidecar_size;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <host root> <fixture archive base name>\n",
                argv[0]);
        return 2;
    }
    snprintf(s_root, sizeof s_root, "%s", argv[1]);
    snprintf(s_base, sizeof s_base, "%s", argv[2]);
    snprintf(sidecar_name, sizeof sidecar_name, "%s%s", s_base,
             ISAAC_VITA_ARCHIVE_SIDECAR_SUFFIX);
    if (!read_file(s_base, &s_archive, &s_archive_size) ||
        !read_file(sidecar_name, &sidecar_bytes, &sidecar_size) ||
        sidecar_size != ISAAC_VITA_ARCHIVE_SIDECAR_SIZE) {
        fprintf(stderr, "cannot read fixture %s / %s\n", s_base, sidecar_name);
        return 2;
    }
    memcpy(s_sidecar, sidecar_bytes, sizeof s_sidecar);
    free(sidecar_bytes);

    if (test_exact_pair() || test_names() || test_sidecar_mutations() ||
        test_archive_mutations())
        return 1;
    printf("archive-sidecar oracle: PASS (%s, %zu bytes)\n", s_base,
           s_archive_size);
    return 0;
}
