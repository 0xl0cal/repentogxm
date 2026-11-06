#include "host_vita_archive_sidecar.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "guest_pe.h"
#include "host_vita_archive_receipt.h"
#include "platform.h"

#if !defined(ISAAC_VITA_ARCHIVE_SIDECAR_ORACLE)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

#if !defined(ISAAC_VITA_ARCHIVE_SIDECAR_BUILD_ID)
#define ISAAC_VITA_ARCHIVE_SIDECAR_BUILD_ID "unpinned"
#endif

#define SIDECAR_HEADER_SIZE      14U
#define SIDECAR_ENTRY_SIZE       20U
#define SIDECAR_DIGEST_OFFSET    224U
#define SIDECAR_INDEX_SHA_OFFSET 64U
#define SIDECAR_SAMPLE_SHA_OFFSET 96U
#define SIDECAR_NAME_OFFSET      128U
#define SIDECAR_PATH_CAPACITY    128U
#define SIDECAR_IO_CAPACITY      ISAAC_VITA_ARCHIVE_SIDECAR_SAMPLE_SIZE

static const unsigned char s_sidecar_magic[16] = {
    'I', 'S', 'A', 'A', 'C', '-', 'A', '-',
    'S', 'I', 'D', 'E', 'C', 'A', 'R', '1'
};

static const unsigned char s_sample_domain[] = "isaac-vita-sidecar-sample-v1";

typedef struct sidecar_record {
    uint64_t size;
    uint32_t index_offset;
    uint32_t entries;
    uint32_t mode;
    uint32_t sample_count;
    uint32_t sample_size;
    unsigned char index_sha256[32];
    unsigned char sample_sha256[32];
    char name[ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY];
} sidecar_record;

static isaac_vita_archive_sidecar_diagnostic s_diagnostic;
static unsigned char s_io_buffer[SIDECAR_IO_CAPACITY];

/* ------------------------------------------------------------------------ */
/* I/O                                                                       */
/* ------------------------------------------------------------------------ */

#if defined(ISAAC_VITA_ARCHIVE_SIDECAR_ORACLE)
#define sidecar_io_stat  isaac_vita_archive_sidecar_oracle_stat
#define sidecar_io_fstat isaac_vita_archive_sidecar_oracle_fstat
#define sidecar_io_open  isaac_vita_archive_sidecar_oracle_open
#define sidecar_io_close isaac_vita_archive_sidecar_oracle_close
#define sidecar_io_pread isaac_vita_archive_sidecar_oracle_pread
#else
static void sidecar_copy_stat(const SceIoStat *native, uint64_t *size,
                              int *regular)
{
    *size = native->st_size < 0 ? UINT64_MAX : (uint64_t)native->st_size;
    *regular = SCE_S_ISREG(native->st_mode) ? 1 : 0;
}

static int sidecar_io_stat(const char *path, uint64_t *size, int *regular)
{
    SceIoStat native;
    int status;

    memset(&native, 0, sizeof native);
    status = sceIoGetstat(path, &native);
    if (status < 0)
        return status;
    sidecar_copy_stat(&native, size, regular);
    return 0;
}

static int sidecar_io_fstat(int descriptor, uint64_t *size, int *regular)
{
    SceIoStat native;
    int status;

    memset(&native, 0, sizeof native);
    status = sceIoGetstatByFd(descriptor, &native);
    if (status < 0)
        return status;
    sidecar_copy_stat(&native, size, regular);
    return 0;
}

static int sidecar_io_open(const char *path)
{
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int sidecar_io_close(int descriptor)
{
    int result = sceIoClose(descriptor);
    return result < 0 ? result : 0;
}

static int sidecar_io_pread(int descriptor, void *buffer, uint32_t size,
                            uint64_t offset)
{
    return sceIoPread(descriptor, buffer, size, (SceOff)offset);
}
#endif

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

static uint16_t sidecar_get_u16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t sidecar_get_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static uint64_t sidecar_get_u64(const unsigned char *bytes)
{
    return (uint64_t)sidecar_get_u32(bytes) |
           ((uint64_t)sidecar_get_u32(bytes + 4U) << 32U);
}

static void sidecar_put_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void sidecar_put_u64(unsigned char *bytes, uint64_t value)
{
    sidecar_put_u32(bytes, (uint32_t)value);
    sidecar_put_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static int sidecar_bytes_zero(const unsigned char *bytes, uint32_t size)
{
    uint32_t index;
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U)
            return 0;
    }
    return 1;
}

static uint32_t sidecar_fail(uint32_t stage, int io_result, uint32_t detail,
                             uint32_t value0, uint32_t value1)
{
    if (s_diagnostic.stage == ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NONE) {
        s_diagnostic.stage = stage;
        s_diagnostic.io_result = io_result;
        s_diagnostic.detail = detail;
        s_diagnostic.value0 = value0;
        s_diagnostic.value1 = value1;
    }
    return 0U;
}

const char *isaac_vita_archive_sidecar_stage_name(uint32_t stage)
{
    switch (stage) {
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NONE: return "none";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ALREADY_TRUSTED: return "already-trusted";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME: return "name";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_ABSENT: return "sidecar-absent";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_IDENTITY: return "sidecar-identity";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_OPEN: return "sidecar-open";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_READ: return "sidecar-read";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_CLOSE: return "sidecar-close";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE: return "sidecar-decode";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_NAME: return "sidecar-name";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_STAT: return "archive-stat";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_IDENTITY: return "archive-identity";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_OPEN: return "archive-open";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FSTAT: return "archive-fstat";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_CROSS_IDENTITY: return "archive-cross-identity";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER_READ: return "archive-header-read";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER: return "archive-header";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_READ: return "archive-index-read";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_HASH: return "archive-index-hash";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_READ: return "archive-sample-read";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_HASH: return "archive-sample-hash";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FSTAT_AFTER: return "archive-fstat-after";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FD_CHANGED: return "archive-fd-changed";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_CLOSE: return "archive-close";
    case ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_TRUSTED: return "trusted";
    default: return "unknown";
    }
}

void isaac_vita_archive_sidecar_get_diagnostic(
    isaac_vita_archive_sidecar_diagnostic *result)
{
    if (result)
        *result = s_diagnostic;
}

/* The exact guest name is "resources/packed/<base>"; the base must be a
 * plain file name that fits the sidecar's name field.  Returns the base
 * length or zero. */
static uint32_t sidecar_base_name(const char *archive_name, char *base)
{
    const char *prefix = ISAAC_VITA_ARCHIVE_SIDECAR_PACKED_PREFIX;
    size_t prefix_length = strlen(prefix);
    size_t length;
    size_t index;

    if (!archive_name || strncmp(archive_name, prefix, prefix_length) != 0)
        return 0U;
    archive_name += prefix_length;
    length = strlen(archive_name);
    if (length < 3U || length >= ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY)
        return 0U;
    for (index = 0U; index < length; ++index) {
        char c = archive_name[index];
        if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 0x20U)
            return 0U;
    }
    if (archive_name[0] == '.')
        return 0U;
    memcpy(base, archive_name, length + 1U);
    return (uint32_t)length;
}

static int sidecar_build_path(char *path, const char *base,
                              const char *suffix)
{
    const char *root = ISAAC_VITA_NATIVE_DATA_ROOT "/"
                       ISAAC_VITA_ARCHIVE_SIDECAR_PACKED_PREFIX;
    size_t root_length = strlen(root);
    size_t base_length = strlen(base);
    size_t suffix_length = strlen(suffix);

    if (root_length + base_length + suffix_length + 1U >
        SIDECAR_PATH_CAPACITY)
        return 0;
    memcpy(path, root, root_length);
    memcpy(path + root_length, base, base_length);
    memcpy(path + root_length + base_length, suffix, suffix_length + 1U);
    return 1;
}

static int sidecar_read_exact_at(int descriptor, uint64_t offset,
                                 void *buffer, uint32_t size,
                                 uint32_t stage, uint32_t detail)
{
    unsigned char *output = (unsigned char *)buffer;
    uint32_t complete = 0U;

    while (complete < size) {
        int result = sidecar_io_pread(descriptor, output + complete,
                                      size - complete, offset + complete);
        if (result <= 0 || (uint32_t)result > size - complete) {
            sidecar_fail(stage, result, detail, complete, size);
            return 0;
        }
        complete += (uint32_t)result;
    }
    return 1;
}

static int sidecar_decode(const unsigned char bytes[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE],
                          sidecar_record *record)
{
    unsigned char digest[32];
    uint32_t mismatch = 0U;
    uint64_t index_end;

    if (memcmp(bytes, s_sidecar_magic, sizeof s_sidecar_magic) != 0)
        mismatch |= 0x01U;
    if (sidecar_get_u32(bytes + 16U) != ISAAC_VITA_ARCHIVE_SIDECAR_VERSION)
        mismatch |= 0x02U;
    if (sidecar_get_u32(bytes + 20U) != ISAAC_VITA_ARCHIVE_SIDECAR_SIZE)
        mismatch |= 0x04U;
    if (!sidecar_bytes_zero(bytes + 52U, 12U) ||
        !sidecar_bytes_zero(bytes + SIDECAR_NAME_OFFSET +
                            ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY,
                            SIDECAR_DIGEST_OFFSET - SIDECAR_NAME_OFFSET -
                            ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY))
        mismatch |= 0x08U;
    if (mismatch != 0U)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE,
                                 0, mismatch, sidecar_get_u32(bytes + 16U),
                                 sidecar_get_u32(bytes + 20U));
    guest_pe_sha256(bytes, SIDECAR_DIGEST_OFFSET, digest);
    if (memcmp(digest, bytes + SIDECAR_DIGEST_OFFSET, sizeof digest) != 0)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE,
                                 0, 0x10U, sidecar_get_u32(digest),
                                 sidecar_get_u32(bytes + SIDECAR_DIGEST_OFFSET));

    memset(record, 0, sizeof *record);
    record->size = sidecar_get_u64(bytes + 24U);
    record->index_offset = sidecar_get_u32(bytes + 32U);
    record->entries = sidecar_get_u32(bytes + 36U);
    record->mode = sidecar_get_u32(bytes + 40U);
    record->sample_count = sidecar_get_u32(bytes + 44U);
    record->sample_size = sidecar_get_u32(bytes + 48U);
    memcpy(record->index_sha256, bytes + SIDECAR_INDEX_SHA_OFFSET, 32U);
    memcpy(record->sample_sha256, bytes + SIDECAR_SAMPLE_SHA_OFFSET, 32U);
    memcpy(record->name, bytes + SIDECAR_NAME_OFFSET,
           ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY);
    if (record->name[ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY - 1U] != '\0')
        mismatch |= 0x20U;
    if (record->sample_count != ISAAC_VITA_ARCHIVE_SIDECAR_SAMPLE_COUNT ||
        record->sample_size != ISAAC_VITA_ARCHIVE_SIDECAR_SAMPLE_SIZE)
        mismatch |= 0x40U;
    if (record->entries == 0U || record->entries > 0xFFFFU ||
        record->index_offset < SIDECAR_HEADER_SIZE || record->mode > 0xFFU)
        mismatch |= 0x80U;
    index_end = (uint64_t)record->index_offset +
                (uint64_t)record->entries * SIDECAR_ENTRY_SIZE;
    if (index_end != record->size)
        mismatch |= 0x100U;
    if (mismatch != 0U)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE,
                                 0, mismatch, record->entries,
                                 record->index_offset);
    return 1;
}

static int sidecar_read_record(const char *path, sidecar_record *record)
{
    unsigned char bytes[ISAAC_VITA_ARCHIVE_SIDECAR_SIZE];
    uint64_t size = 0U;
    int regular = 0;
    int descriptor;
    int result;

    result = sidecar_io_stat(path, &size, &regular);
    if (result < 0)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_ABSENT,
                                 result, 0U, 0U, 0U);
    if (!regular || size != ISAAC_VITA_ARCHIVE_SIDECAR_SIZE)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_IDENTITY,
                                 0, regular ? 0x01U : 0x02U, (uint32_t)size,
                                 ISAAC_VITA_ARCHIVE_SIDECAR_SIZE);
    descriptor = sidecar_io_open(path);
    if (descriptor < 0)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_OPEN,
                                 descriptor, 0U, 0U, 0U);
    if (!sidecar_read_exact_at(descriptor, 0U, bytes, sizeof bytes,
                               ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_READ,
                               0U)) {
        (void)sidecar_io_close(descriptor);
        return 0;
    }
    result = sidecar_io_close(descriptor);
    if (result < 0)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_CLOSE,
                                 result, 0U, 0U, 0U);
    return sidecar_decode(bytes, record);
}

/* Window i of the sampled entry region; returns zero when there is none. */
static int sidecar_window(const sidecar_record *record, uint32_t index,
                          uint64_t *offset, uint32_t *length)
{
    uint64_t region = record->index_offset;
    uint64_t covered = (uint64_t)record->sample_count * record->sample_size;

    if (region <= covered) {
        uint64_t start = (uint64_t)index * record->sample_size;
        if (start >= region)
            return 0;
        *offset = start;
        *length = region - start > record->sample_size
            ? record->sample_size : (uint32_t)(region - start);
        return 1;
    }
    if (index >= record->sample_count)
        return 0;
    *offset = ((region - record->sample_size) * index) /
              (record->sample_count - 1U);
    *length = record->sample_size;
    return 1;
}

static int sidecar_hash_index(int descriptor, const sidecar_record *record,
                              unsigned char digest[32])
{
    guest_pe_sha256_context context;
    uint64_t offset = record->index_offset;
    uint64_t remaining = (uint64_t)record->entries * SIDECAR_ENTRY_SIZE;

    guest_pe_sha256_init(&context);
    while (remaining > 0U) {
        uint32_t amount = remaining > SIDECAR_IO_CAPACITY
            ? SIDECAR_IO_CAPACITY : (uint32_t)remaining;
        if (!sidecar_read_exact_at(
                descriptor, offset, s_io_buffer, amount,
                ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_READ,
                (uint32_t)(offset - record->index_offset)))
            return 0;
        guest_pe_sha256_update(&context, s_io_buffer, amount);
        offset += amount;
        remaining -= amount;
    }
    guest_pe_sha256_final(&context, digest);
    return 1;
}

static int sidecar_hash_samples(int descriptor, const sidecar_record *record,
                                unsigned char digest[32])
{
    guest_pe_sha256_context context;
    unsigned char numbers[28];
    uint64_t offset;
    uint32_t length;
    uint32_t index;

    guest_pe_sha256_init(&context);
    guest_pe_sha256_update(&context, s_sample_domain,
                           sizeof s_sample_domain - 1U);
    sidecar_put_u32(numbers, record->mode);
    sidecar_put_u64(numbers + 4U, record->size);
    sidecar_put_u32(numbers + 12U, record->index_offset);
    sidecar_put_u32(numbers + 16U, record->entries);
    sidecar_put_u32(numbers + 20U, record->sample_count);
    sidecar_put_u32(numbers + 24U, record->sample_size);
    guest_pe_sha256_update(&context, numbers, sizeof numbers);
    for (index = 0U; sidecar_window(record, index, &offset, &length);
         ++index) {
        if (!sidecar_read_exact_at(
                descriptor, offset, s_io_buffer, length,
                ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_READ, index))
            return 0;
        sidecar_put_u64(numbers, offset);
        sidecar_put_u32(numbers + 8U, length);
        guest_pe_sha256_update(&context, numbers, 12U);
        guest_pe_sha256_update(&context, s_io_buffer, length);
    }
    guest_pe_sha256_final(&context, digest);
    return 1;
}

static int sidecar_inspect_archive(const char *path,
                                   const sidecar_record *record)
{
    unsigned char header[SIDECAR_HEADER_SIZE];
    unsigned char digest[32];
    uint64_t path_size = 0U, descriptor_size = 0U, after_size = 0U;
    int path_regular = 0, descriptor_regular = 0, after_regular = 0;
    uint32_t mismatch;
    int descriptor;
    int result;
    int valid = 0;

    result = sidecar_io_stat(path, &path_size, &path_regular);
    if (result < 0)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_STAT,
                                 result, 0U, 0U, 0U);
    if (!path_regular || path_size != record->size)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_IDENTITY,
                                 0, path_regular ? 0x01U : 0x02U,
                                 (uint32_t)path_size, (uint32_t)record->size);
    descriptor = sidecar_io_open(path);
    if (descriptor < 0)
        return (int)sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_OPEN,
                                 descriptor, 0U, 0U, 0U);
    result = sidecar_io_fstat(descriptor, &descriptor_size,
                              &descriptor_regular);
    if (result < 0) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FSTAT,
                     result, 0U, 0U, 0U);
        goto complete;
    }
    if (descriptor_size != path_size || descriptor_regular != path_regular) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_CROSS_IDENTITY,
                     0, 0U, (uint32_t)path_size, (uint32_t)descriptor_size);
        goto complete;
    }
    if (!sidecar_read_exact_at(
            descriptor, 0U, header, sizeof header,
            ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER_READ, 0U))
        goto complete;
    mismatch = 0U;
    if (memcmp(header, "ARCH000", 7U) != 0)
        mismatch |= 0x01U;
    if (header[7] != record->mode)
        mismatch |= 0x02U;
    if (sidecar_get_u32(header + 8U) != record->index_offset)
        mismatch |= 0x04U;
    if (sidecar_get_u16(header + 12U) != record->entries)
        mismatch |= 0x08U;
    if (mismatch != 0U) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER, 0,
                     mismatch, sidecar_get_u32(header + 8U),
                     sidecar_get_u16(header + 12U));
        goto complete;
    }
    if (!sidecar_hash_index(descriptor, record, digest))
        goto complete;
    if (memcmp(digest, record->index_sha256, sizeof digest) != 0) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_HASH, 0,
                     0U, sidecar_get_u32(digest),
                     sidecar_get_u32(record->index_sha256));
        goto complete;
    }
    if (!sidecar_hash_samples(descriptor, record, digest))
        goto complete;
    if (memcmp(digest, record->sample_sha256, sizeof digest) != 0) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_HASH, 0,
                     0U, sidecar_get_u32(digest),
                     sidecar_get_u32(record->sample_sha256));
        goto complete;
    }
    result = sidecar_io_fstat(descriptor, &after_size, &after_regular);
    if (result < 0) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FSTAT_AFTER,
                     result, 0U, 0U, 0U);
        goto complete;
    }
    if (after_size != descriptor_size || after_regular != descriptor_regular) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FD_CHANGED, 0,
                     0U, (uint32_t)descriptor_size, (uint32_t)after_size);
        goto complete;
    }
    valid = 1;

complete:
    result = sidecar_io_close(descriptor);
    if (result < 0) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_CLOSE, result,
                     0U, 0U, 0U);
        valid = 0;
    }
    return valid;
}

uint32_t isaac_vita_archive_sidecar_trust(const char *archive_name,
                                          uint32_t real_token)
{
    char base[ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY];
    char sidecar_path[SIDECAR_PATH_CAPACITY];
    char archive_path[SIDECAR_PATH_CAPACITY];
    sidecar_record record;
    uint32_t token = real_token;

    memset(&s_diagnostic, 0, sizeof s_diagnostic);
    if ((real_token & ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED) != 0U) {
        s_diagnostic.stage = ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ALREADY_TRUSTED;
        return real_token;
    }
    if (!sidecar_base_name(archive_name, base) ||
        !sidecar_build_path(sidecar_path, base,
                            ISAAC_VITA_ARCHIVE_SIDECAR_SUFFIX) ||
        !sidecar_build_path(archive_path, base, "")) {
        s_diagnostic.stage = ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME;
        return real_token;
    }
    if (!sidecar_read_record(sidecar_path, &record))
        goto complete;
    s_diagnostic.entries = record.entries;
    s_diagnostic.size = record.size;
    if (strcmp(record.name, base) != 0) {
        sidecar_fail(ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_NAME, 0, 0U,
                     (uint32_t)(unsigned char)record.name[0],
                     (uint32_t)(unsigned char)base[0]);
        goto complete;
    }
    if (!sidecar_inspect_archive(archive_path, &record))
        goto complete;
    s_diagnostic.stage = ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_TRUSTED;
    token = real_token | ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED |
            ISAAC_VITA_ARCHIVE_SIDECAR_TOKEN;

complete:
#if !defined(ISAAC_VITA_ARCHIVE_SIDECAR_ORACLE)
    /* Archives without a sidecar are the common case; keep the boot log to
     * the archives a sidecar was found for (any outcome). */
    if (s_diagnostic.stage != ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_ABSENT) {
        isaac_vita_log(
            "[archive-validation-sidecar] build=%s archive=%s trusted=%u "
            "stage=%u:%s io=0x%08x detail=0x%08x value0=0x%08x "
            "value1=0x%08x entries=%u size=%u token=%u\n",
            ISAAC_VITA_ARCHIVE_SIDECAR_BUILD_ID, base,
            (unsigned)(s_diagnostic.stage ==
                       ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_TRUSTED),
            (unsigned)s_diagnostic.stage,
            isaac_vita_archive_sidecar_stage_name(s_diagnostic.stage),
            (unsigned)(uint32_t)s_diagnostic.io_result,
            (unsigned)s_diagnostic.detail,
            (unsigned)s_diagnostic.value0,
            (unsigned)s_diagnostic.value1,
            (unsigned)s_diagnostic.entries,
            (unsigned)(uint32_t)s_diagnostic.size,
            (unsigned)token);
    }
#endif
    return token;
}

#if !defined(ISAAC_VITA_ARCHIVE_SIDECAR_ORACLE)
/* Linker --wrap edge: the generated FileManager::LoadArchiveFile owner calls
 * isaac_vita_archive_receipt_begin; the real receipt decides first and the
 * sidecar is consulted only when it did not trust the archive. */
uint32_t __wrap_isaac_vita_archive_receipt_begin(const char *archive_name)
{
    return isaac_vita_archive_sidecar_trust(
        archive_name, __real_isaac_vita_archive_receipt_begin(archive_name));
}
#endif
