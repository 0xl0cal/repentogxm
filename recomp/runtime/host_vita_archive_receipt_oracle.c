#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "guest_pe.h"
#include "host_vita_archive_receipt.h"

#define ROOT "ux0:/data/isaacr001"
#define ANIMATIONS ROOT "/resources/packed/animations.a"
#define AFTERBIRTH ROOT "/resources/packed/afterbirth.a"
#define SLOT0 ROOT "/archive-validation-receipt.v1.0.bin"
#define SLOT1 ROOT "/archive-validation-receipt.v1.1.bin"
#define TEMP0 ROOT "/archive-validation-receipt.v1.0.tmp"
#define TEMP1 ROOT "/archive-validation-receipt.v1.1.tmp"

#define ROLE_ANIMATIONS (1U << 0U)
#define ROLE_AFTERBIRTH (1U << 1U)
#define ROLE_SLOT0      (1U << 2U)
#define ROLE_SLOT1      (1U << 3U)
#define ROLE_TEMP0      (1U << 4U)
#define ROLE_TEMP1      (1U << 5U)
#define ROLE_DEVICE     (1U << 6U)

/* Unsafe Vita applications must set the low system read/write permission
 * bits when they create a file.  Mirror the hardware rejection so the host
 * oracle cannot silently accept POSIX-only 0600 again. */
#define ORACLE_VITA_SYSTEM_RW 0006U
#define ORACLE_VITA_EINVAL ((int32_t)UINT32_C(0x80010016))

enum oracle_operation {
    OP_NONE,
    OP_STAT,
    OP_FSTAT,
    OP_OPEN,
    OP_CLOSE,
    OP_PREAD,
    OP_WRITE,
    OP_SYNC_FD,
    OP_REMOVE,
    OP_RENAME,
    OP_SYNC_DEVICE
};

enum oracle_mutation {
    MUTATION_NONE,
    MUTATION_SIZE,
    MUTATION_TIME,
    MUTATION_VIEW_TIME,
    MUTATION_VIEW_SIZE,
    MUTATION_FD_LATE_TIME,
    MUTATION_PATH_LATE_TIME,
    MUTATION_INVALID_DATE,
    MUTATION_INDEX,
    MUTATION_SAMPLE,
    MUTATION_CONTENT,
    MUTATION_CONCURRENT_TIME
};

typedef struct fd_role {
    int descriptor;
    uint32_t role;
} fd_role;

static char s_root[PATH_MAX];
static char s_animations[PATH_MAX];
static char s_afterbirth[PATH_MAX];
static fd_role s_fds[32];
static enum oracle_operation s_fail_operation;
static uint32_t s_fail_roles;
static unsigned s_fail_nth;
static unsigned s_fail_seen;
static uint32_t s_mutation_role;
static enum oracle_mutation s_mutation;
static unsigned s_mutation_stat_seen;
static uint32_t s_short_pread;
static uint32_t s_short_write;
static uint64_t s_read_bytes[2];
static unsigned char s_slot0_baseline[512];
static unsigned char s_slot1_baseline[512];

static int fail(unsigned line, const char *expression)
{
    fprintf(stderr, "archive-receipt oracle failed at line %u: %s\n",
            line, expression);
    return 1;
}

#define CHECK(expression) \
    do { if (!(expression)) return fail(__LINE__, #expression); } while (0)

static uint32_t role_for_path(const char *path)
{
    if (strcmp(path, ANIMATIONS) == 0)
        return ROLE_ANIMATIONS;
    if (strcmp(path, AFTERBIRTH) == 0)
        return ROLE_AFTERBIRTH;
    if (strcmp(path, SLOT0) == 0)
        return ROLE_SLOT0;
    if (strcmp(path, SLOT1) == 0)
        return ROLE_SLOT1;
    if (strcmp(path, TEMP0) == 0)
        return ROLE_TEMP0;
    if (strcmp(path, TEMP1) == 0)
        return ROLE_TEMP1;
    return 0U;
}

static int translate_path(const char *path, char output[PATH_MAX])
{
    int length;

    if (strcmp(path, ANIMATIONS) == 0)
        length = snprintf(output, PATH_MAX, "%s", s_animations);
    else if (strcmp(path, AFTERBIRTH) == 0)
        length = snprintf(output, PATH_MAX, "%s", s_afterbirth);
    else if (strncmp(path, ROOT, sizeof ROOT - 1U) == 0)
        length = snprintf(output, PATH_MAX, "%s%s", s_root,
                          path + sizeof ROOT - 1U);
    else
        return 0;
    return length >= 0 && (size_t)length < PATH_MAX;
}

static uint32_t fd_get_role(int descriptor)
{
    size_t index;
    for (index = 0U; index < sizeof s_fds / sizeof s_fds[0]; ++index) {
        if (s_fds[index].descriptor == descriptor)
            return s_fds[index].role;
    }
    return 0U;
}

static void fd_set_role(int descriptor, uint32_t role)
{
    size_t index;
    for (index = 0U; index < sizeof s_fds / sizeof s_fds[0]; ++index) {
        if (s_fds[index].descriptor < 0) {
            s_fds[index].descriptor = descriptor;
            s_fds[index].role = role;
            return;
        }
    }
    abort();
}

static void fd_clear_role(int descriptor)
{
    size_t index;
    for (index = 0U; index < sizeof s_fds / sizeof s_fds[0]; ++index) {
        if (s_fds[index].descriptor == descriptor) {
            s_fds[index].descriptor = -1;
            s_fds[index].role = 0U;
            return;
        }
    }
}

static int should_fail(enum oracle_operation operation, uint32_t role)
{
    if (operation != s_fail_operation || !(role & s_fail_roles))
        return 0;
    ++s_fail_seen;
    return s_fail_seen == s_fail_nth;
}

static void set_failure(enum oracle_operation operation, uint32_t roles,
                        unsigned nth)
{
    s_fail_operation = operation;
    s_fail_roles = roles;
    s_fail_nth = nth;
    s_fail_seen = 0U;
}

static void clear_controls(void)
{
    s_fail_operation = OP_NONE;
    s_fail_roles = 0U;
    s_fail_nth = 0U;
    s_fail_seen = 0U;
    s_mutation = MUTATION_NONE;
    s_mutation_role = 0U;
    s_mutation_stat_seen = 0U;
    s_short_pread = 0U;
    s_short_write = 0U;
}

static int fill_date(isaac_vita_archive_receipt_date *destination,
                     time_t seconds, long nanoseconds)
{
    struct tm broken;
    if (!gmtime_r(&seconds, &broken))
        return 0;
    destination->year = (uint16_t)(broken.tm_year + 1900);
    destination->month = (uint16_t)(broken.tm_mon + 1);
    destination->day = (uint16_t)broken.tm_mday;
    destination->hour = (uint16_t)broken.tm_hour;
    destination->minute = (uint16_t)broken.tm_min;
    destination->second = (uint16_t)broken.tm_sec;
    destination->microsecond = (uint32_t)(nanoseconds / 1000L);
    return 1;
}

static int convert_stat(isaac_vita_archive_receipt_stat *destination,
                        const struct stat *source, uint32_t role,
                        int descriptor_view)
{
    destination->size = (uint64_t)source->st_size;
    destination->regular = S_ISREG(source->st_mode) ? 1U : 0U;
    if (!fill_date(&destination->ctime, source->st_ctim.tv_sec,
                   source->st_ctim.tv_nsec) ||
        !fill_date(&destination->mtime, source->st_mtim.tv_sec,
                   source->st_mtim.tv_nsec))
        return 0;
    if (role & s_mutation_role) {
        ++s_mutation_stat_seen;
        if (s_mutation == MUTATION_SIZE)
            ++destination->size;
        else if (s_mutation == MUTATION_TIME ||
                 (s_mutation == MUTATION_CONCURRENT_TIME &&
                  s_mutation_stat_seen >= 3U))
            destination->mtime.microsecond =
                (destination->mtime.microsecond + 1U) % 1000000U;
        else if (s_mutation == MUTATION_VIEW_TIME && descriptor_view)
            destination->mtime.microsecond =
                (destination->mtime.microsecond + 1U) % 1000000U;
        else if (s_mutation == MUTATION_VIEW_SIZE && descriptor_view)
            ++destination->size;
        else if (s_mutation == MUTATION_FD_LATE_TIME &&
                 descriptor_view && s_mutation_stat_seen >= 3U)
            destination->mtime.microsecond =
                (destination->mtime.microsecond + 1U) % 1000000U;
        else if (s_mutation == MUTATION_PATH_LATE_TIME &&
                 !descriptor_view && s_mutation_stat_seen >= 4U)
            destination->mtime.microsecond =
                (destination->mtime.microsecond + 1U) % 1000000U;
        else if (s_mutation == MUTATION_INVALID_DATE)
            destination->ctime.year = 1U;
    }
    return 1;
}

int isaac_vita_archive_receipt_oracle_stat(
    const char *path, isaac_vita_archive_receipt_stat *result)
{
    char translated[PATH_MAX];
    struct stat native;
    uint32_t role = role_for_path(path);

    if (should_fail(OP_STAT, role) || !translate_path(path, translated) ||
        stat(translated, &native) != 0)
        return -1;
    return convert_stat(result, &native, role, 0) ? 0 : -1;
}

int isaac_vita_archive_receipt_oracle_fstat(
    int descriptor, isaac_vita_archive_receipt_stat *result)
{
    struct stat native;
    uint32_t role = fd_get_role(descriptor);

    if (should_fail(OP_FSTAT, role) || fstat(descriptor, &native) != 0)
        return -1;
    return convert_stat(result, &native, role, 1) ? 0 : -1;
}

int isaac_vita_archive_receipt_oracle_open(
    const char *path, int write_exclusive, uint32_t create_mode)
{
    char translated[PATH_MAX];
    uint32_t role = role_for_path(path);
    int descriptor;
    int flags = write_exclusive
        ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY;

    if (should_fail(OP_OPEN, role) || !translate_path(path, translated))
        return -1;
    if (write_exclusive &&
        (create_mode & ORACLE_VITA_SYSTEM_RW) != ORACLE_VITA_SYSTEM_RW)
        return ORACLE_VITA_EINVAL;
    descriptor = open(translated, flags, (mode_t)create_mode);
    if (descriptor >= 0)
        fd_set_role(descriptor, role);
    return descriptor;
}

int isaac_vita_archive_receipt_oracle_close(int descriptor)
{
    uint32_t role = fd_get_role(descriptor);
    int injected = should_fail(OP_CLOSE, role);
    int status = close(descriptor);
    fd_clear_role(descriptor);
    return injected || status != 0 ? -1 : 0;
}

static void mutate_read(uint32_t role, unsigned char *bytes,
                        uint32_t size, uint64_t offset)
{
    uint64_t target = UINT64_MAX;

    if (!(role & s_mutation_role))
        return;
    if (s_mutation == MUTATION_INDEX) {
        if (role == ROLE_ANIMATIONS)
            target = UINT64_C(660281) + 7U;
        else if (role == ROLE_AFTERBIRTH)
            target = UINT64_C(145266573) + 7U;
    } else if (s_mutation == MUTATION_SAMPLE &&
               role == ROLE_AFTERBIRTH) {
        uint64_t available = UINT64_C(145266573) - 4096U;
        target = (available * 32U) / 63U + 113U;
    } else if (s_mutation == MUTATION_CONTENT &&
               role == ROLE_ANIMATIONS) {
        target = 1000U;
    }
    if (target >= offset && target - offset < size)
        bytes[target - offset] ^= 0x5aU;
}

int isaac_vita_archive_receipt_oracle_pread(
    int descriptor, void *buffer, uint32_t size, uint64_t offset)
{
    uint32_t role = fd_get_role(descriptor);
    ssize_t result;

    if (should_fail(OP_PREAD, role))
        return -1;
    if (s_short_pread && size > s_short_pread)
        size = s_short_pread;
    result = pread(descriptor, buffer, size, (off_t)offset);
    if (result > 0) {
        if (role == ROLE_ANIMATIONS)
            s_read_bytes[0] += (uint64_t)result;
        else if (role == ROLE_AFTERBIRTH)
            s_read_bytes[1] += (uint64_t)result;
        mutate_read(role, (unsigned char *)buffer, (uint32_t)result, offset);
    }
    return result < 0 || result > INT_MAX ? -1 : (int)result;
}

int isaac_vita_archive_receipt_oracle_write(
    int descriptor, const void *buffer, uint32_t size)
{
    uint32_t role = fd_get_role(descriptor);
    ssize_t result;

    if (should_fail(OP_WRITE, role))
        return -1;
    if (s_short_write && size > s_short_write)
        size = s_short_write;
    result = write(descriptor, buffer, size);
    return result < 0 || result > INT_MAX ? -1 : (int)result;
}

int isaac_vita_archive_receipt_oracle_sync_fd(int descriptor)
{
    uint32_t role = fd_get_role(descriptor);
    if (should_fail(OP_SYNC_FD, role))
        return -1;
    return fsync(descriptor) == 0 ? 0 : -1;
}

int isaac_vita_archive_receipt_oracle_remove(const char *path)
{
    char translated[PATH_MAX];
    uint32_t role = role_for_path(path);
    if (should_fail(OP_REMOVE, role) || !translate_path(path, translated))
        return -1;
    return unlink(translated) == 0 ? 0 : -1;
}

int isaac_vita_archive_receipt_oracle_rename(
    const char *source, const char *destination)
{
    char translated_source[PATH_MAX], translated_destination[PATH_MAX];
    uint32_t role = role_for_path(source);
    if (should_fail(OP_RENAME, role) ||
        !translate_path(source, translated_source) ||
        !translate_path(destination, translated_destination))
        return -1;
    return rename(translated_source, translated_destination) == 0 ? 0 : -1;
}

int isaac_vita_archive_receipt_oracle_sync_device(const char *device)
{
    (void)device;
    return should_fail(OP_SYNC_DEVICE, ROLE_DEVICE) ? -1 : 0;
}

static int host_path(const char *vita_path, char output[PATH_MAX])
{
    return translate_path(vita_path, output);
}

static int write_blob(const char *vita_path, const void *bytes, size_t size)
{
    char path[PATH_MAX];
    int descriptor;
    const unsigned char *input = (const unsigned char *)bytes;
    size_t complete = 0U;

    if (!host_path(vita_path, path))
        return 0;
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0)
        return 0;
    while (complete < size) {
        ssize_t result = write(descriptor, input + complete, size - complete);
        if (result <= 0) {
            (void)close(descriptor);
            return 0;
        }
        complete += (size_t)result;
    }
    return close(descriptor) == 0;
}

static int read_blob(const char *vita_path, void *bytes, size_t size)
{
    char path[PATH_MAX];
    int descriptor;
    unsigned char *output = (unsigned char *)bytes;
    size_t complete = 0U;

    if (!host_path(vita_path, path))
        return 0;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
        return 0;
    while (complete < size) {
        ssize_t result = read(descriptor, output + complete, size - complete);
        if (result <= 0) {
            (void)close(descriptor);
            return 0;
        }
        complete += (size_t)result;
    }
    if (close(descriptor) != 0)
        return 0;
    return 1;
}

static int remove_blob(const char *vita_path)
{
    char path[PATH_MAX];
    if (!host_path(vita_path, path))
        return 0;
    return unlink(path) == 0 || errno == ENOENT;
}

static int blob_size(const char *vita_path, uint64_t *size)
{
    char path[PATH_MAX];
    struct stat metadata;
    if (!host_path(vita_path, path) || stat(path, &metadata) != 0)
        return 0;
    *size = (uint64_t)metadata.st_size;
    return 1;
}

static void put_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void put_u64(unsigned char *bytes, uint64_t value)
{
    put_u32(bytes, (uint32_t)value);
    put_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static void rehash_receipt(unsigned char bytes[512])
{
    guest_pe_sha256(bytes, 480U, bytes + 480U);
}

static int cleanup_receipts(void)
{
    return remove_blob(SLOT0) && remove_blob(SLOT1) &&
           remove_blob(TEMP0) && remove_blob(TEMP1);
}

static uint32_t begin_animations(void)
{
    return isaac_vita_archive_receipt_begin(
        ISAAC_VITA_ARCHIVE_RECEIPT_ANIMATIONS_NAME);
}

static uint32_t begin_afterbirth(void)
{
    return isaac_vita_archive_receipt_begin(
        ISAAC_VITA_ARCHIVE_RECEIPT_AFTERBIRTH_NAME);
}

static int finish_animations(uint32_t token)
{
    return isaac_vita_archive_receipt_finish(token, 0U, 0U, 1U, 1U, 0U);
}

static int finish_afterbirth(uint32_t token)
{
    return isaac_vita_archive_receipt_finish(
        token, 0U, 0U, 2647U, 2647U, 0U);
}

static int token_is(uint32_t token, uint32_t kind, int trusted)
{
    return (token & ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_KIND_MASK) == kind &&
           !!(token & ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED) == !!trusted;
}

static isaac_vita_archive_receipt_diagnostic last_diagnostic(void)
{
    isaac_vita_archive_receipt_diagnostic result;
    memset(&result, 0, sizeof result);
    isaac_vita_archive_receipt_get_diagnostic(&result);
    return result;
}

static int test_vita_create_mode_contract(void)
{
    int descriptor;
    uint64_t ignored;

    CHECK(cleanup_receipts());
    clear_controls();
    descriptor = isaac_vita_archive_receipt_oracle_open(TEMP0, 1, 0600U);
    CHECK(descriptor == ORACLE_VITA_EINVAL);
    CHECK(!blob_size(TEMP0, &ignored));
    descriptor = isaac_vita_archive_receipt_oracle_open(TEMP0, 1, 0666U);
    CHECK(descriptor >= 0);
    CHECK(isaac_vita_archive_receipt_oracle_close(descriptor) == 0);
    CHECK(remove_blob(TEMP0));
    return 0;
}

static int create_baselines(void)
{
    uint32_t token;

    CHECK(cleanup_receipts());
    clear_controls();
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(isaac_vita_archive_receipt_begin("resources/packed/not-target.a") == 0U);
    token = begin_animations();
    CHECK(token_is(token, 1U, 0));
    CHECK(!isaac_vita_archive_receipt_finish(token, 0U, 0U, 1U, 0U, 1U));
    CHECK(!isaac_vita_archive_receipt_finish(token, 1U, 0U, 1U, 1U, 0U));
    CHECK(!isaac_vita_archive_receipt_finish(token, 0U, 1U, 0U, 0U, 0U));
    CHECK(!isaac_vita_archive_receipt_finish(token | 0x200U,
                                              0U, 0U, 1U, 1U, 0U));
    CHECK(finish_animations(token));
    CHECK(read_blob(SLOT0, s_slot0_baseline, sizeof s_slot0_baseline));

    token = begin_afterbirth();
    CHECK(token_is(token, 2U, 0));
    s_read_bytes[1] = 0U;
    CHECK(finish_afterbirth(token));
    CHECK(s_read_bytes[1] >= UINT64_C(145319513));
    CHECK(read_blob(SLOT1, s_slot1_baseline, sizeof s_slot1_baseline));
    return 0;
}

static int restore_two_slots(void)
{
    CHECK(cleanup_receipts());
    CHECK(write_blob(SLOT0, s_slot0_baseline, sizeof s_slot0_baseline));
    CHECK(write_blob(SLOT1, s_slot1_baseline, sizeof s_slot1_baseline));
    clear_controls();
    isaac_vita_archive_receipt_oracle_reset();
    return 0;
}

static int restore_one_slot(void)
{
    CHECK(cleanup_receipts());
    CHECK(write_blob(SLOT0, s_slot0_baseline, sizeof s_slot0_baseline));
    clear_controls();
    isaac_vita_archive_receipt_oracle_reset();
    return 0;
}

static int assert_older_fallback(void)
{
    uint32_t animations = begin_animations();
    uint32_t afterbirth = begin_afterbirth();
    CHECK(token_is(animations, 1U, 1));
    CHECK(token_is(afterbirth, 2U, 0));
    return 0;
}

static int test_fast_identity(void)
{
    uint32_t token;

    restore_two_slots();
    s_read_bytes[1] = 0U;
    token = begin_afterbirth();
    CHECK(token_is(token, 2U, 1));
    CHECK(s_read_bytes[1] >= 64U * 4096U + 52940U);
    CHECK(s_read_bytes[1] < 1000000U);
    s_read_bytes[0] = 0U;
    token = begin_animations();
    CHECK(token_is(token, 1U, 1));
    CHECK(s_read_bytes[0] >= 660301U && s_read_bytes[0] < 1000000U);
    return 0;
}

static int test_archive_mutations(void)
{
    static const enum oracle_mutation afterbirth_mutations[] = {
        MUTATION_SIZE, MUTATION_TIME, MUTATION_INDEX,
        MUTATION_SAMPLE, MUTATION_CONCURRENT_TIME
    };
    static const enum oracle_mutation animations_mutations[] = {
        MUTATION_SIZE, MUTATION_TIME, MUTATION_INDEX,
        MUTATION_CONTENT, MUTATION_CONCURRENT_TIME
    };
    size_t index;

    for (index = 0U; index < sizeof afterbirth_mutations /
                             sizeof afterbirth_mutations[0]; ++index) {
        restore_two_slots();
        s_mutation = afterbirth_mutations[index];
        s_mutation_role = ROLE_AFTERBIRTH;
        CHECK(token_is(begin_afterbirth(), 2U, 0));
    }
    for (index = 0U; index < sizeof animations_mutations /
                             sizeof animations_mutations[0]; ++index) {
        restore_two_slots();
        s_mutation = animations_mutations[index];
        s_mutation_role = ROLE_ANIMATIONS;
        CHECK(token_is(begin_animations(), 1U, 0));
    }
    clear_controls();
    return 0;
}

static int test_receipt_hostility(void)
{
    unsigned char altered[513];
    static const size_t semantic_offsets[] = {
        0U, 16U, 32U, 240U, 400U
    };
    size_t index;

    /* A torn/corrupt newer slot cannot displace the valid older slot. */
    restore_two_slots();
    memcpy(altered, s_slot1_baseline, 512U);
    altered[480] ^= 1U;
    CHECK(write_blob(SLOT1, altered, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(assert_older_fallback() == 0);

    /* Even a correctly rehashed semantic mutation is rejected. */
    for (index = 0U; index < sizeof semantic_offsets /
                             sizeof semantic_offsets[0]; ++index) {
        restore_two_slots();
        memcpy(altered, s_slot1_baseline, 512U);
        altered[semantic_offsets[index]] ^= 1U;
        rehash_receipt(altered);
        CHECK(write_blob(SLOT1, altered, 512U));
        isaac_vita_archive_receipt_oracle_reset();
        CHECK(assert_older_fallback() == 0);
    }

    /* A forged selected digest parses but cannot match the live archive. */
    restore_two_slots();
    memcpy(altered, s_slot1_baseline, 512U);
    altered[240U + 128U] ^= 1U;
    rehash_receipt(altered);
    CHECK(write_blob(SLOT1, altered, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(assert_older_fallback() == 0);

    restore_two_slots();
    memcpy(altered, s_slot1_baseline, 512U);
    altered[512] = 0x5aU;
    CHECK(write_blob(SLOT1, altered, sizeof altered));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(assert_older_fallback() == 0);

    /* Equal generations have no unique winner, even if both parse. */
    CHECK(cleanup_receipts());
    CHECK(write_blob(SLOT0, s_slot1_baseline, 512U));
    CHECK(write_blob(SLOT1, s_slot1_baseline, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 0));
    CHECK(token_is(begin_afterbirth(), 2U, 0));

    memcpy(altered, s_slot1_baseline, 512U);
    put_u32(altered + 84U, 2U);
    rehash_receipt(altered);
    CHECK(write_blob(SLOT0, altered, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 0));

    /* Temp files are never candidates. */
    CHECK(cleanup_receipts());
    CHECK(write_blob(TEMP0, s_slot1_baseline, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_afterbirth(), 2U, 0));

    /* PC provenance is admitted only with the same fixed corpus/identity. */
    CHECK(cleanup_receipts());
    memcpy(altered, s_slot1_baseline, 512U);
    put_u32(altered + 84U, 2U);
    put_u32(altered + 244U, 2U);
    rehash_receipt(altered);
    CHECK(write_blob(SLOT1, altered, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 1));
    CHECK(token_is(begin_afterbirth(), 2U, 1));

    /* Generation overflow preserves trust but disables publication. */
    CHECK(cleanup_receipts());
    memcpy(altered, s_slot0_baseline, 512U);
    put_u64(altered + 24U, UINT64_MAX);
    rehash_receipt(altered);
    CHECK(write_blob(SLOT0, altered, 512U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 1));
    CHECK(!finish_animations(1U));
    return 0;
}

static int assert_animation_survives(void)
{
    clear_controls();
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 1));
    return 0;
}

static int atomic_failure(enum oracle_operation operation, uint32_t role,
                          unsigned nth, int seed_target,
                          uint32_t expected_stage)
{
    static const unsigned char junk[23] = { 1U, 2U, 3U };
    isaac_vita_archive_receipt_diagnostic diagnostic;

    restore_one_slot();
    if (seed_target) {
        const char *target = role == ROLE_TEMP1 ? TEMP1 : SLOT1;
        CHECK(write_blob(target, junk, sizeof junk));
    }
    set_failure(operation, role, nth);
    (void)finish_animations(1U);
    diagnostic = last_diagnostic();
    CHECK(diagnostic.version ==
          ISAAC_VITA_ARCHIVE_RECEIPT_DIAGNOSTIC_VERSION);
    CHECK(diagnostic.kind == 1U && !diagnostic.published);
    CHECK(diagnostic.stage == expected_stage);
    CHECK(diagnostic.io_result == -1);
    CHECK(assert_animation_survives() == 0);
    return 0;
}

static int test_atomic_publication(void)
{
    uint64_t ignored;

    CHECK(atomic_failure(
        OP_OPEN, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_OPEN) == 0);
    CHECK(atomic_failure(
        OP_OPEN, ROLE_TEMP1, 2U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_OPEN) == 0);
    CHECK(atomic_failure(
        OP_WRITE, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_WRITE) == 0);
    CHECK(atomic_failure(
        OP_SYNC_FD, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_SYNC) == 0);
    CHECK(atomic_failure(
        OP_CLOSE, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_CLOSE) == 0);
    CHECK(atomic_failure(
        OP_PREAD, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_BYTES) == 0);
    CHECK(atomic_failure(
        OP_FSTAT, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT) == 0);
    CHECK(atomic_failure(
        OP_STAT, ROLE_TEMP1, 2U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT) == 0);
    CHECK(atomic_failure(
        OP_REMOVE, ROLE_TEMP1, 1U, 1,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_REMOVE) == 0);
    CHECK(atomic_failure(
        OP_REMOVE, ROLE_SLOT1, 1U, 1,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SLOT_REMOVE) == 0);
    CHECK(atomic_failure(
        OP_RENAME, ROLE_TEMP1, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RENAME) == 0);
    CHECK(atomic_failure(
        OP_SYNC_DEVICE, ROLE_DEVICE, 1U, 0,
        ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_DEVICE_SYNC) == 0);

    /* Partial I/O loops to exact completion; a later failure leaves no slot. */
    restore_one_slot();
    s_short_write = 37U;
    CHECK(finish_animations(1U));
    CHECK(last_diagnostic().stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISHED);
    CHECK(assert_animation_survives() == 0);
    restore_one_slot();
    s_short_write = 37U;
    set_failure(OP_WRITE, ROLE_TEMP1, 3U);
    CHECK(!finish_animations(1U));
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    s_short_pread = 31U;
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 1));

    /* Deliberately torn higher slot remains unselectable. */
    CHECK(write_blob(SLOT1, s_slot1_baseline, 193U));
    isaac_vita_archive_receipt_oracle_reset();
    CHECK(token_is(begin_animations(), 1U, 1));
    CHECK(blob_size(SLOT1, &ignored) && ignored == 193U);
    return 0;
}

static int test_failure_stages(void)
{
    isaac_vita_archive_receipt_diagnostic diagnostic;

    /* A full-pass data mismatch is distinct from an I/O failure. */
    restore_one_slot();
    s_mutation = MUTATION_CONTENT;
    s_mutation_role = ROLE_ANIMATIONS;
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_HASH);
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    set_failure(OP_PREAD, ROLE_ANIMATIONS, 3U);
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_READ);
    CHECK(assert_animation_survives() == 0);

    /* Post-full-pass metadata and close failures retain their own stage. */
    restore_one_slot();
    set_failure(OP_FSTAT, ROLE_ANIMATIONS, 2U);
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT_AFTER);
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    set_failure(OP_STAT, ROLE_ANIMATIONS, 2U);
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT_AFTER);
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    set_failure(OP_CLOSE, ROLE_ANIMATIONS, 1U);
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CLOSE);
    CHECK(assert_animation_survives() == 0);

    /* Model both leading physical hypotheses without accepting either one:
     * split path/fd time views and a structurally invalid archive date. */
    restore_one_slot();
    s_mutation = MUTATION_VIEW_TIME;
    s_mutation_role = ROLE_TEMP1;
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CROSS_IDENTITY);
    CHECK(diagnostic.detail == 0x08U);
    CHECK(assert_animation_survives() == 0);

    /* Cross-view size/type remain hard identity requirements even if a later
     * physical fix permits representational timestamp differences. */
    restore_one_slot();
    s_mutation = MUTATION_VIEW_SIZE;
    s_mutation_role = ROLE_TEMP1;
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CROSS_IDENTITY);
    CHECK(diagnostic.detail == 0x01U);
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    s_mutation = MUTATION_FD_LATE_TIME;
    s_mutation_role = ROLE_TEMP1;
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FD_CHANGED);
    CHECK(diagnostic.detail == 0x08U);
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    s_mutation = MUTATION_PATH_LATE_TIME;
    s_mutation_role = ROLE_TEMP1;
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_CHANGED);
    CHECK(diagnostic.detail == 0x08U);
    CHECK(assert_animation_survives() == 0);

    restore_one_slot();
    s_mutation = MUTATION_INVALID_DATE;
    s_mutation_role = ROLE_ANIMATIONS;
    CHECK(!finish_animations(1U));
    diagnostic = last_diagnostic();
    CHECK(diagnostic.stage ==
          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_DECODE);
    CHECK((diagnostic.detail & 0xffU) == 6U);
    CHECK((diagnostic.detail & 0x00ff0000U) == 0x00010000U);
    CHECK(assert_animation_survives() == 0);
    return 0;
}

int main(int argc, char **argv)
{
    struct stat metadata;
    size_t index;

    if (argc != 4) {
        fprintf(stderr, "usage: %s ROOT animations.a afterbirth.a\n", argv[0]);
        return 2;
    }
    if (snprintf(s_root, sizeof s_root, "%s", argv[1]) < 0 ||
        snprintf(s_animations, sizeof s_animations, "%s", argv[2]) < 0 ||
        snprintf(s_afterbirth, sizeof s_afterbirth, "%s", argv[3]) < 0 ||
        stat(s_root, &metadata) != 0 || !S_ISDIR(metadata.st_mode)) {
        fprintf(stderr, "invalid oracle fixture paths\n");
        return 2;
    }
    for (index = 0U; index < sizeof s_fds / sizeof s_fds[0]; ++index)
        s_fds[index].descriptor = -1;

    if (test_vita_create_mode_contract() || create_baselines() ||
        test_fast_identity() ||
        test_archive_mutations() || test_receipt_hostility() ||
        test_atomic_publication() || test_failure_stages())
        return 1;
    if (!cleanup_receipts())
        return fail(__LINE__, "cleanup_receipts()");
    printf("archive validation receipt oracle: PASS; full=2; "
           "fast-afterbirth<1MiB; mutations=10; hostile-receipts=13; "
           "atomic-failures=12; precise-stages=22; unsafe-create-mode=1\n");
    return 0;
}
