#include "host_vita_archive_receipt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The translated KAGE target supplies the strong same-context presenter.
 * Receipt host oracles and non-graphics frontiers retain this zero-cost weak
 * edge, so validation never acquires a graphics dependency of its own. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak, noinline))
#endif
void kage_vita_loading_note_verify(void)
{
}

#include "guest_pe.h"
#include "platform.h"

#if !defined(ISAAC_VITA_ARCHIVE_RECEIPT_ORACLE)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif

#define RECEIPT_VERSION             1U
#define RECEIPT_SIZE                512U
#define RECEIPT_DIGEST_OFFSET       480U
#define RECEIPT_ROW_OFFSET          80U
#define RECEIPT_ROW_SIZE            160U
#define RECEIPT_ROW_COUNT           2U
#define RECEIPT_VALID_MASK          3U
#define RECEIPT_INDEX_CAPACITY      52940U
#define RECEIPT_IO_CAPACITY         4096U
#define RECEIPT_SAMPLE_COUNT        64U
#define RECEIPT_RUNTIME_PROVENANCE  1U
#define RECEIPT_PC_PROVENANCE       2U

/* The Vita SELF is UNSAFE, so created files must carry the system read/write
 * permission bits as well as the user bits.  0600 is rejected by sceIoOpen
 * with SCE_ERROR_ERRNO_EINVAL on hardware; 0666 is the established Vita I/O
 * mode used by the adjacent shader-cache publisher. */
#define RECEIPT_CREATE_MODE         0666U

#define RECEIPT_KIND_ANIMATIONS     1U
#define RECEIPT_KIND_AFTERBIRTH     2U

#define RECEIPT_SLOT0 \
    ISAAC_VITA_NATIVE_DATA_ROOT "/archive-validation-receipt.v1.0.bin"
#define RECEIPT_SLOT1 \
    ISAAC_VITA_NATIVE_DATA_ROOT "/archive-validation-receipt.v1.1.bin"
#define RECEIPT_TEMP0 \
    ISAAC_VITA_NATIVE_DATA_ROOT "/archive-validation-receipt.v1.0.tmp"
#define RECEIPT_TEMP1 \
    ISAAC_VITA_NATIVE_DATA_ROOT "/archive-validation-receipt.v1.1.tmp"

typedef struct receipt_archive_spec {
    uint32_t kind;
    const char *name;
    const char *path;
    uint64_t size;
    uint32_t index_offset;
    uint32_t entries;
    uint32_t mode;
    unsigned char full_sha256[32];
    unsigned char index_sha256[32];
} receipt_archive_spec;

typedef struct receipt_row {
    uint32_t provenance;
    isaac_vita_archive_receipt_stat stat;
    unsigned char sample_sha256[32];
} receipt_row;

typedef struct receipt_document {
    uint64_t generation;
    uint32_t valid_mask;
    receipt_row rows[RECEIPT_ROW_COUNT];
} receipt_document;

typedef struct receipt_state {
    int loaded;
    int selected_slot;
    receipt_document document;
} receipt_state;

static const unsigned char s_receipt_magic[16] = {
    'I', 'S', 'A', 'A', 'C', 'A', 'V', 'R',
    'C', 'P', 'T', 'V', '1', 0, 0, 0
};

/* SHA-256 of canonical, sorted, compact JSON for the complete v1 corpus. */
static const unsigned char s_corpus_sha256[32] = {
    0x8e, 0xaf, 0x38, 0x36, 0x02, 0xb9, 0xe5, 0x8a,
    0xd4, 0x25, 0x89, 0xf1, 0x43, 0x6a, 0x91, 0x1b,
    0xae, 0x6f, 0xeb, 0x18, 0x45, 0xb4, 0x2f, 0xb6,
    0xe3, 0x33, 0x98, 0x38, 0x34, 0x90, 0xf0, 0xf6
};

static const receipt_archive_spec s_archive_specs[RECEIPT_ROW_COUNT] = {
    {
        RECEIPT_KIND_ANIMATIONS,
        ISAAC_VITA_ARCHIVE_RECEIPT_ANIMATIONS_NAME,
        ISAAC_VITA_NATIVE_DATA_ROOT "/resources/packed/animations.a",
        UINT64_C(660301), 660281U, 1U, 1U,
        {
            0x18, 0x2e, 0x07, 0x19, 0x34, 0xfc, 0x2d, 0x46,
            0x00, 0x50, 0x6b, 0xb3, 0x26, 0xbb, 0x8d, 0x39,
            0x46, 0x86, 0x1b, 0xfa, 0x7c, 0xe3, 0xc4, 0x40,
            0x4d, 0x52, 0x4d, 0x68, 0xad, 0xa8, 0xab, 0xf5
        },
        {
            0x56, 0x7e, 0xc7, 0xe8, 0x83, 0x48, 0x9c, 0xeb,
            0x94, 0x8a, 0xbd, 0x54, 0xfb, 0xaa, 0xb4, 0x60,
            0x45, 0x00, 0xda, 0x61, 0x3b, 0x15, 0x89, 0x92,
            0x2c, 0xbf, 0x2b, 0xad, 0x86, 0x66, 0x61, 0x0b
        }
    },
    {
        RECEIPT_KIND_AFTERBIRTH,
        ISAAC_VITA_ARCHIVE_RECEIPT_AFTERBIRTH_NAME,
        ISAAC_VITA_NATIVE_DATA_ROOT "/resources/packed/afterbirth.a",
        UINT64_C(145319513), 145266573U, 2647U, 2U,
        {
            0xee, 0x40, 0x0c, 0x96, 0x0d, 0x9b, 0x68, 0x54,
            0xf6, 0x12, 0xf1, 0x5b, 0xae, 0xde, 0xdb, 0xb8,
            0x22, 0x19, 0x20, 0x79, 0x59, 0x61, 0xba, 0x42,
            0x22, 0x53, 0x89, 0x35, 0x57, 0xd6, 0x4d, 0x8c
        },
        {
            0x37, 0xaf, 0x24, 0x88, 0x55, 0xe6, 0x0b, 0x94,
            0x6c, 0xb6, 0xef, 0xe1, 0x98, 0x9a, 0xf5, 0x73,
            0xf0, 0xe7, 0x83, 0x34, 0x22, 0xd7, 0xdd, 0xc7,
            0x5d, 0x87, 0x28, 0x26, 0x85, 0xf6, 0x68, 0x37
        }
    }
};

static const char *const s_slot_paths[2] = { RECEIPT_SLOT0, RECEIPT_SLOT1 };
static const char *const s_temp_paths[2] = { RECEIPT_TEMP0, RECEIPT_TEMP1 };

static receipt_state s_state;
static isaac_vita_archive_receipt_diagnostic s_diagnostic;
static unsigned char s_index_buffer[RECEIPT_INDEX_CAPACITY];
static unsigned char s_io_buffer[RECEIPT_IO_CAPACITY];

#if defined(ISAAC_VITA_ARCHIVE_RECEIPT_ORACLE)
#define receipt_io_stat        isaac_vita_archive_receipt_oracle_stat
#define receipt_io_fstat       isaac_vita_archive_receipt_oracle_fstat
#define receipt_io_close       isaac_vita_archive_receipt_oracle_close
#define receipt_io_pread       isaac_vita_archive_receipt_oracle_pread
#define receipt_io_write       isaac_vita_archive_receipt_oracle_write
#define receipt_io_sync_fd     isaac_vita_archive_receipt_oracle_sync_fd
#define receipt_io_remove      isaac_vita_archive_receipt_oracle_remove
#define receipt_io_rename      isaac_vita_archive_receipt_oracle_rename
#define receipt_io_sync_device isaac_vita_archive_receipt_oracle_sync_device

static int receipt_io_open(const char *path, int write_exclusive)
{
    return isaac_vita_archive_receipt_oracle_open(
        path, write_exclusive,
        write_exclusive ? RECEIPT_CREATE_MODE : 0U);
}
#else
static void receipt_copy_date(isaac_vita_archive_receipt_date *destination,
                              const SceDateTime *source)
{
    destination->year = source->year;
    destination->month = source->month;
    destination->day = source->day;
    destination->hour = source->hour;
    destination->minute = source->minute;
    destination->second = source->second;
    destination->microsecond = source->microsecond;
}

static void receipt_copy_stat(isaac_vita_archive_receipt_stat *destination,
                              const SceIoStat *source)
{
    destination->size = source->st_size < 0
        ? UINT64_MAX : (uint64_t)source->st_size;
    receipt_copy_date(&destination->ctime, &source->st_ctime);
    receipt_copy_date(&destination->mtime, &source->st_mtime);
    destination->regular = SCE_S_ISREG(source->st_mode) ? 1U : 0U;
}

static int receipt_io_stat(const char *path,
                           isaac_vita_archive_receipt_stat *result)
{
    SceIoStat native;
    int status;

    memset(&native, 0, sizeof native);
    status = sceIoGetstat(path, &native);
    if (status < 0)
        return status;
    receipt_copy_stat(result, &native);
    return 0;
}

static int receipt_io_fstat(int descriptor,
                            isaac_vita_archive_receipt_stat *result)
{
    SceIoStat native;
    int status;

    memset(&native, 0, sizeof native);
    status = sceIoGetstatByFd(descriptor, &native);
    if (status < 0)
        return status;
    receipt_copy_stat(result, &native);
    return 0;
}

static int receipt_io_open(const char *path, int write_exclusive)
{
    if (write_exclusive)
        return sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL,
                         RECEIPT_CREATE_MODE);
    return sceIoOpen(path, SCE_O_RDONLY, 0);
}

static int receipt_io_close(int descriptor)
{
    int result = sceIoClose(descriptor);
    return result < 0 ? result : 0;
}

static int receipt_io_pread(int descriptor, void *buffer, uint32_t size,
                            uint64_t offset)
{
    int result = sceIoPread(descriptor, buffer, size, (SceOff)offset);
    return result;
}

static int receipt_io_write(int descriptor, const void *buffer,
                            uint32_t size)
{
    int result = sceIoWrite(descriptor, buffer, size);
    return result;
}

static int receipt_io_sync_fd(int descriptor)
{
    int result = sceIoSyncByFd(descriptor, 0);
    return result < 0 ? result : 0;
}

static int receipt_io_remove(const char *path)
{
    int result = sceIoRemove(path);
    return result < 0 ? result : 0;
}

static int receipt_io_rename(const char *source, const char *destination)
{
    int result = sceIoRename(source, destination);
    return result < 0 ? result : 0;
}

static int receipt_io_sync_device(const char *device)
{
    int result = sceIoSync(device, 0);
    return result < 0 ? result : 0;
}
#endif

static uint16_t receipt_get_u16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8U));
}

static uint32_t receipt_get_u32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t receipt_get_u64(const unsigned char *bytes)
{
    return (uint64_t)receipt_get_u32(bytes) |
           ((uint64_t)receipt_get_u32(bytes + 4U) << 32U);
}

static void receipt_put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
}

static void receipt_put_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void receipt_put_u64(unsigned char *bytes, uint64_t value)
{
    receipt_put_u32(bytes, (uint32_t)value);
    receipt_put_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static int receipt_bytes_zero(const unsigned char *bytes, uint32_t size)
{
    uint32_t index;
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U)
            return 0;
    }
    return 1;
}

static uint32_t receipt_date_invalid(
    const isaac_vita_archive_receipt_date *date)
{
    uint32_t invalid = 0U;

    if (!date->year && !date->month && !date->day && !date->hour &&
        !date->minute && !date->second && !date->microsecond)
        return 0U;
    if (date->year < 1980U || date->year > 9999U)
        invalid |= 0x01U;
    if (date->month < 1U || date->month > 12U)
        invalid |= 0x02U;
    if (date->day < 1U || date->day > 31U)
        invalid |= 0x04U;
    if (date->hour > 23U)
        invalid |= 0x08U;
    if (date->minute > 59U)
        invalid |= 0x10U;
    if (date->second > 60U)
        invalid |= 0x20U;
    if (date->microsecond >= 1000000U)
        invalid |= 0x40U;
    return invalid;
}

static int receipt_date_equal(const isaac_vita_archive_receipt_date *left,
                              const isaac_vita_archive_receipt_date *right)
{
    return left->year == right->year && left->month == right->month &&
           left->day == right->day && left->hour == right->hour &&
           left->minute == right->minute &&
           left->second == right->second &&
           left->microsecond == right->microsecond;
}

static int receipt_stat_equal(
    const isaac_vita_archive_receipt_stat *left,
    const isaac_vita_archive_receipt_stat *right)
{
    return left->size == right->size && left->regular == right->regular &&
           receipt_date_equal(&left->ctime, &right->ctime) &&
           receipt_date_equal(&left->mtime, &right->mtime);
}

#define RECEIPT_STAT_MISMATCH_SIZE    0x01U
#define RECEIPT_STAT_MISMATCH_REGULAR 0x02U
#define RECEIPT_STAT_MISMATCH_CTIME   0x04U
#define RECEIPT_STAT_MISMATCH_MTIME   0x08U

static uint32_t receipt_stat_mismatch(
    const isaac_vita_archive_receipt_stat *left,
    const isaac_vita_archive_receipt_stat *right)
{
    uint32_t mismatch = 0U;

    if (left->size != right->size)
        mismatch |= RECEIPT_STAT_MISMATCH_SIZE;
    if (left->regular != right->regular)
        mismatch |= RECEIPT_STAT_MISMATCH_REGULAR;
    if (!receipt_date_equal(&left->ctime, &right->ctime))
        mismatch |= RECEIPT_STAT_MISMATCH_CTIME;
    if (!receipt_date_equal(&left->mtime, &right->mtime))
        mismatch |= RECEIPT_STAT_MISMATCH_MTIME;
    return mismatch;
}

static int receipt_diag_fail(
    isaac_vita_archive_receipt_diagnostic *diagnostic,
    uint32_t stage, int io_result, uint32_t detail,
    uint32_t value0, uint32_t value1)
{
    if (diagnostic && diagnostic->stage ==
                          ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_NONE) {
        diagnostic->stage = stage;
        diagnostic->io_result = io_result;
        diagnostic->detail = detail;
        diagnostic->value0 = value0;
        diagnostic->value1 = value1;
    }
    return 0;
}

#if !defined(ISAAC_VITA_ARCHIVE_RECEIPT_ORACLE)
static const char *receipt_stage_name(uint32_t stage)
{
    switch (stage) {
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_NONE: return "none";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TRUSTED: return "finish-trusted";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TOKEN: return "finish-token";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_COUNTERS: return "finish-counters";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SPEC: return "inspect-spec";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT: return "inspect-path-stat";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_IDENTITY: return "inspect-path-identity";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_OPEN: return "inspect-open";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT: return "inspect-fstat";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CROSS_IDENTITY: return "inspect-cross-identity";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_HEADER_READ: return "inspect-header-read";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_HEADER: return "inspect-header";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_INDEX_READ: return "inspect-index-read";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_INDEX_HASH: return "inspect-index-hash";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SAMPLE_READ: return "inspect-sample-read";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_READ: return "inspect-full-read";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_HASH: return "inspect-full-hash";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT_AFTER: return "inspect-fstat-after";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FD_CHANGED: return "inspect-fd-changed";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CLOSE: return "inspect-close";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT_AFTER: return "inspect-path-stat-after";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_CHANGED: return "inspect-path-changed";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_GENERATION_OVERFLOW: return "generation-overflow";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_REMOVE: return "publish-temp-remove";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_OPEN: return "publish-temp-open";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_WRITE: return "publish-temp-write";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_SYNC: return "publish-temp-sync";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_CLOSE: return "publish-temp-close";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT: return "publish-read-path-stat";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_IDENTITY: return "publish-read-path-identity";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_OPEN: return "publish-read-open";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT: return "publish-read-fstat";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CROSS_IDENTITY: return "publish-read-cross-identity";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_BYTES: return "publish-read-bytes";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT_AFTER: return "publish-read-fstat-after";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FD_CHANGED: return "publish-read-fd-changed";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CLOSE: return "publish-read-close";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT_AFTER: return "publish-read-path-stat-after";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_CHANGED: return "publish-read-path-changed";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_DECODE: return "publish-read-decode";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RAW_COMPARE: return "publish-raw-compare";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SEMANTIC_COMPARE: return "publish-semantic-compare";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SLOT_REMOVE: return "publish-slot-remove";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RENAME: return "publish-rename";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_DEVICE_SYNC: return "publish-device-sync";
    case ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISHED: return "published";
    default: return "unknown";
    }
}
#endif

static void receipt_decode_date(isaac_vita_archive_receipt_date *date,
                                const unsigned char *bytes)
{
    date->year = receipt_get_u16(bytes);
    date->month = receipt_get_u16(bytes + 2U);
    date->day = receipt_get_u16(bytes + 4U);
    date->hour = receipt_get_u16(bytes + 6U);
    date->minute = receipt_get_u16(bytes + 8U);
    date->second = receipt_get_u16(bytes + 10U);
    date->microsecond = receipt_get_u32(bytes + 12U);
}

static void receipt_encode_date(unsigned char *bytes,
                                const isaac_vita_archive_receipt_date *date)
{
    receipt_put_u16(bytes, date->year);
    receipt_put_u16(bytes + 2U, date->month);
    receipt_put_u16(bytes + 4U, date->day);
    receipt_put_u16(bytes + 6U, date->hour);
    receipt_put_u16(bytes + 8U, date->minute);
    receipt_put_u16(bytes + 10U, date->second);
    receipt_put_u32(bytes + 12U, date->microsecond);
}

static const receipt_archive_spec *receipt_spec_for_kind(uint32_t kind)
{
    if (kind < RECEIPT_KIND_ANIMATIONS || kind > RECEIPT_KIND_AFTERBIRTH)
        return NULL;
    return &s_archive_specs[kind - 1U];
}

static const receipt_archive_spec *receipt_spec_for_name(const char *name)
{
    uint32_t index;

    if (!name)
        return NULL;
    for (index = 0U; index < RECEIPT_ROW_COUNT; ++index) {
        if (strcmp(name, s_archive_specs[index].name) == 0)
            return &s_archive_specs[index];
    }
    return NULL;
}

static int receipt_decode(const unsigned char bytes[RECEIPT_SIZE],
                          receipt_document *document,
                          uint32_t *failure_detail,
                          uint32_t *failure_value0,
                          uint32_t *failure_value1)
{
    unsigned char digest[32];
    uint32_t valid_mask;
    uint32_t index;

    if (failure_detail)
        *failure_detail = 0U;
    if (failure_value0)
        *failure_value0 = 0U;
    if (failure_value1)
        *failure_value1 = 0U;
    if (memcmp(bytes, s_receipt_magic, sizeof s_receipt_magic) != 0 ||
        receipt_get_u32(bytes + 16U) != RECEIPT_VERSION ||
        receipt_get_u32(bytes + 20U) != RECEIPT_SIZE ||
        receipt_get_u64(bytes + 24U) == 0U ||
        memcmp(bytes + 32U, s_corpus_sha256, sizeof s_corpus_sha256) != 0 ||
        receipt_get_u32(bytes + 68U) != RECEIPT_ROW_COUNT ||
        receipt_get_u32(bytes + 72U) != 0U ||
        receipt_get_u32(bytes + 76U) != 0U ||
        !receipt_bytes_zero(bytes + 400U, 80U)) {
        if (failure_detail)
            *failure_detail = 1U;
        return 0;
    }

    valid_mask = receipt_get_u32(bytes + 64U);
    if (!valid_mask || (valid_mask & ~RECEIPT_VALID_MASK) != 0U) {
        if (failure_detail)
            *failure_detail = 2U;
        if (failure_value0)
            *failure_value0 = valid_mask;
        return 0;
    }
    guest_pe_sha256(bytes, RECEIPT_DIGEST_OFFSET, digest);
    if (memcmp(digest, bytes + RECEIPT_DIGEST_OFFSET, sizeof digest) != 0) {
        if (failure_detail)
            *failure_detail = 3U;
        if (failure_value0)
            *failure_value0 = receipt_get_u32(digest);
        if (failure_value1)
            *failure_value1 = receipt_get_u32(
                bytes + RECEIPT_DIGEST_OFFSET);
        return 0;
    }

    memset(document, 0, sizeof *document);
    document->generation = receipt_get_u64(bytes + 24U);
    document->valid_mask = valid_mask;
    for (index = 0U; index < RECEIPT_ROW_COUNT; ++index) {
        const receipt_archive_spec *spec = &s_archive_specs[index];
        const unsigned char *row = bytes + RECEIPT_ROW_OFFSET +
                                   index * RECEIPT_ROW_SIZE;
        receipt_row *decoded = &document->rows[index];
        uint32_t provenance;

        if ((valid_mask & (1U << index)) == 0U) {
            if (!receipt_bytes_zero(row, RECEIPT_ROW_SIZE)) {
                if (failure_detail)
                    *failure_detail = 4U | (index << 8U);
                return 0;
            }
            continue;
        }
        provenance = receipt_get_u32(row + 4U);
        if (receipt_get_u32(row) != spec->kind ||
            (provenance != RECEIPT_RUNTIME_PROVENANCE &&
             provenance != RECEIPT_PC_PROVENANCE) ||
            receipt_get_u64(row + 8U) != spec->size ||
            receipt_get_u64(row + 16U) != spec->index_offset ||
            receipt_get_u32(row + 24U) != spec->entries ||
            receipt_get_u32(row + 28U) != spec->mode ||
            memcmp(row + 64U, spec->full_sha256, 32U) != 0 ||
            memcmp(row + 96U, spec->index_sha256, 32U) != 0) {
            if (failure_detail)
                *failure_detail = 5U | (index << 8U);
            return 0;
        }
        decoded->provenance = provenance;
        decoded->stat.size = receipt_get_u64(row + 8U);
        decoded->stat.regular = 1U;
        receipt_decode_date(&decoded->stat.ctime, row + 32U);
        receipt_decode_date(&decoded->stat.mtime, row + 48U);
        if (receipt_date_invalid(&decoded->stat.ctime)) {
            if (failure_detail)
                *failure_detail = 6U | (index << 8U) |
                    (receipt_date_invalid(&decoded->stat.ctime) << 16U);
            if (failure_value0)
                *failure_value0 = ((uint32_t)decoded->stat.ctime.year << 16U) |
                    decoded->stat.ctime.month;
            if (failure_value1)
                *failure_value1 = decoded->stat.ctime.microsecond;
            return 0;
        }
        if (receipt_date_invalid(&decoded->stat.mtime)) {
            if (failure_detail)
                *failure_detail = 7U | (index << 8U) |
                    (receipt_date_invalid(&decoded->stat.mtime) << 16U);
            if (failure_value0)
                *failure_value0 = ((uint32_t)decoded->stat.mtime.year << 16U) |
                    decoded->stat.mtime.month;
            if (failure_value1)
                *failure_value1 = decoded->stat.mtime.microsecond;
            return 0;
        }
        memcpy(decoded->sample_sha256, row + 128U, 32U);
        if ((spec->kind == RECEIPT_KIND_ANIMATIONS &&
             !receipt_bytes_zero(decoded->sample_sha256, 32U)) ||
            (spec->kind == RECEIPT_KIND_AFTERBIRTH &&
             receipt_bytes_zero(decoded->sample_sha256, 32U))) {
            if (failure_detail)
                *failure_detail = 8U | (index << 8U);
            return 0;
        }
    }
    return 1;
}

static void receipt_encode(const receipt_document *document,
                           unsigned char bytes[RECEIPT_SIZE])
{
    uint32_t index;

    memset(bytes, 0, RECEIPT_SIZE);
    memcpy(bytes, s_receipt_magic, sizeof s_receipt_magic);
    receipt_put_u32(bytes + 16U, RECEIPT_VERSION);
    receipt_put_u32(bytes + 20U, RECEIPT_SIZE);
    receipt_put_u64(bytes + 24U, document->generation);
    memcpy(bytes + 32U, s_corpus_sha256, sizeof s_corpus_sha256);
    receipt_put_u32(bytes + 64U, document->valid_mask);
    receipt_put_u32(bytes + 68U, RECEIPT_ROW_COUNT);

    for (index = 0U; index < RECEIPT_ROW_COUNT; ++index) {
        const receipt_archive_spec *spec = &s_archive_specs[index];
        const receipt_row *source = &document->rows[index];
        unsigned char *row = bytes + RECEIPT_ROW_OFFSET +
                             index * RECEIPT_ROW_SIZE;

        if ((document->valid_mask & (1U << index)) == 0U)
            continue;
        receipt_put_u32(row, spec->kind);
        receipt_put_u32(row + 4U, source->provenance);
        receipt_put_u64(row + 8U, spec->size);
        receipt_put_u64(row + 16U, spec->index_offset);
        receipt_put_u32(row + 24U, spec->entries);
        receipt_put_u32(row + 28U, spec->mode);
        receipt_encode_date(row + 32U, &source->stat.ctime);
        receipt_encode_date(row + 48U, &source->stat.mtime);
        memcpy(row + 64U, spec->full_sha256, 32U);
        memcpy(row + 96U, spec->index_sha256, 32U);
        memcpy(row + 128U, source->sample_sha256, 32U);
    }
    guest_pe_sha256(bytes, RECEIPT_DIGEST_OFFSET,
                    bytes + RECEIPT_DIGEST_OFFSET);
}

static int receipt_read_exact_at(int descriptor, uint64_t offset,
                                 void *buffer, uint32_t size,
                                 isaac_vita_archive_receipt_diagnostic *diagnostic,
                                 uint32_t stage, uint32_t detail)
{
    unsigned char *output = (unsigned char *)buffer;
    uint32_t complete = 0U;

    while (complete < size) {
        int result = receipt_io_pread(descriptor, output + complete,
                                      size - complete, offset + complete);
        if (result <= 0 || (uint32_t)result > size - complete) {
            receipt_diag_fail(diagnostic, stage, result, detail,
                              complete, size);
            return 0;
        }
        complete += (uint32_t)result;
    }
    return 1;
}

static int receipt_write_exact(int descriptor, const void *buffer,
                               uint32_t size,
                               isaac_vita_archive_receipt_diagnostic *diagnostic)
{
    const unsigned char *input = (const unsigned char *)buffer;
    uint32_t complete = 0U;

    while (complete < size) {
        int result = receipt_io_write(descriptor, input + complete,
                                      size - complete);
        if (result <= 0 || (uint32_t)result > size - complete) {
            receipt_diag_fail(
                diagnostic,
                ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_WRITE,
                result, 0U, complete, size);
            return 0;
        }
        complete += (uint32_t)result;
    }
    return 1;
}

static int receipt_read_path(
    const char *path, receipt_document *document,
    unsigned char raw[RECEIPT_SIZE],
    isaac_vita_archive_receipt_diagnostic *diagnostic)
{
    isaac_vita_archive_receipt_stat path_before, descriptor_before;
    isaac_vita_archive_receipt_stat descriptor_after, path_after;
    unsigned char local[RECEIPT_SIZE];
    unsigned char *bytes = raw ? raw : local;
    int descriptor;
    int close_attempted = 0;
    int result;
    int valid = 0;
    uint32_t decode_detail = 0U;
    uint32_t decode_value0 = 0U;
    uint32_t decode_value1 = 0U;

    result = receipt_io_stat(path, &path_before);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT,
            result, 0U, 0U, 0U);
        return 0;
    }
    if (!path_before.regular || path_before.size != RECEIPT_SIZE) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_IDENTITY,
            0, (!path_before.regular ? RECEIPT_STAT_MISMATCH_REGULAR : 0U) |
               (path_before.size != RECEIPT_SIZE
                    ? RECEIPT_STAT_MISMATCH_SIZE : 0U),
            (uint32_t)path_before.size, RECEIPT_SIZE);
        return 0;
    }
    descriptor = receipt_io_open(path, 0);
    if (descriptor < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_OPEN,
            descriptor, 0U, 0U, 0U);
        return 0;
    }
    result = receipt_io_fstat(descriptor, &descriptor_before);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT,
            result, 0U, 0U, 0U);
        goto complete;
    }
    if (!receipt_stat_equal(&path_before, &descriptor_before)) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CROSS_IDENTITY,
            0, receipt_stat_mismatch(&path_before, &descriptor_before),
            (uint32_t)path_before.size, (uint32_t)descriptor_before.size);
        goto complete;
    }
    if (!receipt_read_exact_at(
            descriptor, 0U, bytes, RECEIPT_SIZE, diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_BYTES, 0U))
        goto complete;
    result = receipt_io_fstat(descriptor, &descriptor_after);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT_AFTER,
            result, 0U, 0U, 0U);
        goto complete;
    }
    if (!receipt_stat_equal(&descriptor_before, &descriptor_after)) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FD_CHANGED,
            0, receipt_stat_mismatch(&descriptor_before, &descriptor_after),
            (uint32_t)descriptor_before.size,
            (uint32_t)descriptor_after.size);
        goto complete;
    }
    close_attempted = 1;
    result = receipt_io_close(descriptor);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CLOSE,
            result, 0U, 0U, 0U);
        return 0;
    }
    result = receipt_io_stat(path, &path_after);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT_AFTER,
            result, 0U, 0U, 0U);
        return 0;
    }
    if (!receipt_stat_equal(&path_before, &path_after)) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_CHANGED,
            0, receipt_stat_mismatch(&path_before, &path_after),
            (uint32_t)path_before.size, (uint32_t)path_after.size);
        return 0;
    }
    if (!receipt_decode(bytes, document, &decode_detail,
                        &decode_value0, &decode_value1)) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_DECODE,
            0, decode_detail, decode_value0, decode_value1);
        return 0;
    }
    valid = 1;

complete:
    if (!close_attempted) {
        result = receipt_io_close(descriptor);
        if (result < 0)
            receipt_diag_fail(
                diagnostic,
                ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CLOSE,
                result, 0U, 0U, 0U);
    }
    return valid;
}

static void receipt_load_state(void)
{
    receipt_document slot[2];
    int valid[2];

    if (s_state.loaded)
        return;
    memset(&s_state, 0, sizeof s_state);
    s_state.loaded = 1;
    s_state.selected_slot = -1;
    valid[0] = receipt_read_path(s_slot_paths[0], &slot[0], NULL, NULL);
    valid[1] = receipt_read_path(s_slot_paths[1], &slot[1], NULL, NULL);
    if (valid[0] && valid[1]) {
        /* Equal generations have no unique newest record: reject both. */
        if (slot[0].generation == slot[1].generation) {
            /* Retain only the generation floor so a later full validation can
             * repair the journal at generation+1 without trusting either. */
            s_state.document.generation = slot[0].generation;
            return;
        }
        s_state.selected_slot = slot[0].generation > slot[1].generation
            ? 0 : 1;
        s_state.document = slot[s_state.selected_slot];
    } else if (valid[0] || valid[1]) {
        s_state.selected_slot = valid[0] ? 0 : 1;
        s_state.document = slot[s_state.selected_slot];
    }
}

static int receipt_hash_full_file(int descriptor, uint64_t size,
                                  unsigned char digest[32],
                                  isaac_vita_archive_receipt_diagnostic *diagnostic)
{
    guest_pe_sha256_context context;
    uint64_t offset = 0U;

    guest_pe_sha256_init(&context);
    while (offset < size) {
        uint32_t amount = size - offset > RECEIPT_INDEX_CAPACITY
            ? RECEIPT_INDEX_CAPACITY : (uint32_t)(size - offset);
        /* The index has already been authenticated before the full pass, so
         * its fixed buffer can double as a larger sequential hash window. */
        if (!receipt_read_exact_at(
                descriptor, offset, s_index_buffer, amount, diagnostic,
                ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_READ,
                (uint32_t)offset))
            return 0;
        guest_pe_sha256_update(&context, s_index_buffer, amount);
        offset += amount;
    }
    guest_pe_sha256_final(&context, digest);
    return 1;
}

static int receipt_hash_afterbirth_samples(
    int descriptor, const receipt_archive_spec *spec,
    unsigned char digest[32],
    isaac_vita_archive_receipt_diagnostic *diagnostic)
{
    static const unsigned char domain[] = "isaac-vita-sample-v1";
    guest_pe_sha256_context context;
    unsigned char numbers[28];
    uint64_t available;
    uint32_t index;

    if (spec->index_offset < RECEIPT_IO_CAPACITY) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SPEC,
            0, 0x10U, spec->index_offset, RECEIPT_IO_CAPACITY);
        return 0;
    }
    available = (uint64_t)spec->index_offset - RECEIPT_IO_CAPACITY;
    guest_pe_sha256_init(&context);
    guest_pe_sha256_update(&context, domain, sizeof domain - 1U);
    receipt_put_u32(numbers, spec->kind);
    receipt_put_u64(numbers + 4U, spec->size);
    receipt_put_u64(numbers + 12U, spec->index_offset);
    receipt_put_u32(numbers + 20U, RECEIPT_SAMPLE_COUNT);
    receipt_put_u32(numbers + 24U, RECEIPT_IO_CAPACITY);
    guest_pe_sha256_update(&context, numbers, sizeof numbers);
    for (index = 0U; index < RECEIPT_SAMPLE_COUNT; ++index) {
        uint64_t offset = (available * index) /
                          (RECEIPT_SAMPLE_COUNT - 1U);
        if (!receipt_read_exact_at(
                descriptor, offset, s_io_buffer, RECEIPT_IO_CAPACITY,
                diagnostic,
                ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SAMPLE_READ,
                index))
            return 0;
        receipt_put_u64(numbers, offset);
        receipt_put_u32(numbers + 8U, RECEIPT_IO_CAPACITY);
        guest_pe_sha256_update(&context, numbers, 12U);
        guest_pe_sha256_update(&context, s_io_buffer,
                               RECEIPT_IO_CAPACITY);
    }
    guest_pe_sha256_final(&context, digest);
    return 1;
}

static int receipt_inspect_archive(const receipt_archive_spec *spec,
                                   int require_full_hash,
                                   receipt_row *identity,
                                   isaac_vita_archive_receipt_diagnostic *diagnostic)
{
    isaac_vita_archive_receipt_stat path_before, descriptor_before;
    isaac_vita_archive_receipt_stat descriptor_after, path_after;
    unsigned char header[14];
    unsigned char digest[32];
    uint32_t index_size;
    uint32_t mismatch;
    int descriptor;
    int result;
    int valid = 0;

    index_size = spec->entries * 20U;
    if (index_size > RECEIPT_INDEX_CAPACITY ||
        spec->index_offset > spec->size ||
        index_size != spec->size - spec->index_offset) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SPEC,
            0, (index_size > RECEIPT_INDEX_CAPACITY ? 0x01U : 0U) |
               (spec->index_offset > spec->size ? 0x02U : 0U) |
               (index_size != spec->size - spec->index_offset ? 0x04U : 0U),
            index_size, spec->index_offset);
        return 0;
    }
    result = receipt_io_stat(spec->path, &path_before);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT,
            result, 0U, 0U, 0U);
        return 0;
    }
    if (!path_before.regular || path_before.size != spec->size) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_IDENTITY,
            0, (!path_before.regular ? RECEIPT_STAT_MISMATCH_REGULAR : 0U) |
               (path_before.size != spec->size
                    ? RECEIPT_STAT_MISMATCH_SIZE : 0U),
            (uint32_t)path_before.size, (uint32_t)spec->size);
        return 0;
    }
    descriptor = receipt_io_open(spec->path, 0);
    if (descriptor < 0) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_OPEN,
            descriptor, 0U, 0U, 0U);
        return 0;
    }
    result = receipt_io_fstat(descriptor, &descriptor_before);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT,
            result, 0U, 0U, 0U);
        goto complete;
    }
    mismatch = receipt_stat_mismatch(&path_before, &descriptor_before);
    if (mismatch != 0U) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CROSS_IDENTITY,
            0, mismatch, (uint32_t)path_before.size,
            (uint32_t)descriptor_before.size);
        goto complete;
    }
    if (!receipt_read_exact_at(
            descriptor, 0U, header, sizeof header, diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_HEADER_READ, 0U))
        goto complete;
    mismatch = 0U;
    if (memcmp(header, "ARCH000", 7U) != 0)
        mismatch |= 0x01U;
    if (header[7] != spec->mode)
        mismatch |= 0x02U;
    if (receipt_get_u32(header + 8U) != spec->index_offset)
        mismatch |= 0x04U;
    if (receipt_get_u16(header + 12U) != spec->entries)
        mismatch |= 0x08U;
    if (mismatch != 0U) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_HEADER,
            0, mismatch, receipt_get_u32(header + 8U),
            receipt_get_u16(header + 12U));
        goto complete;
    }
    if (!receipt_read_exact_at(
            descriptor, spec->index_offset, s_index_buffer, index_size,
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_INDEX_READ, 0U))
        goto complete;
    guest_pe_sha256(s_index_buffer, index_size, digest);
    if (memcmp(digest, spec->index_sha256, sizeof digest) != 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_INDEX_HASH,
            0, 0U, receipt_get_u32(digest),
            receipt_get_u32(spec->index_sha256));
        goto complete;
    }
    memset(identity, 0, sizeof *identity);
    identity->stat = path_before;
    if (spec->kind == RECEIPT_KIND_AFTERBIRTH) {
        if (!receipt_hash_afterbirth_samples(
                descriptor, spec, identity->sample_sha256, diagnostic))
            goto complete;
    } else if (!require_full_hash) {
        /* animations.a is small enough that every trusted start hashes it. */
        require_full_hash = 1;
    }
    if (require_full_hash) {
        if (!receipt_hash_full_file(
                descriptor, spec->size, digest, diagnostic))
            goto complete;
        if (memcmp(digest, spec->full_sha256, sizeof digest) != 0) {
            receipt_diag_fail(
                diagnostic,
                ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_HASH,
                0, 0U, receipt_get_u32(digest),
                receipt_get_u32(spec->full_sha256));
            goto complete;
        }
    }
    result = receipt_io_fstat(descriptor, &descriptor_after);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT_AFTER,
            result, 0U, 0U, 0U);
        goto complete;
    }
    mismatch = receipt_stat_mismatch(&descriptor_before, &descriptor_after);
    if (mismatch != 0U) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FD_CHANGED,
            0, mismatch, (uint32_t)descriptor_before.size,
            (uint32_t)descriptor_after.size);
        goto complete;
    }
    valid = 1;

complete:
    result = receipt_io_close(descriptor);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CLOSE,
            result, 0U, 0U, 0U);
        valid = 0;
    }
    result = receipt_io_stat(spec->path, &path_after);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT_AFTER,
            result, 0U, 0U, 0U);
        valid = 0;
    } else {
        mismatch = receipt_stat_mismatch(&path_before, &path_after);
        if (mismatch != 0U) {
            receipt_diag_fail(
                diagnostic,
                ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_CHANGED,
                0, mismatch, (uint32_t)path_before.size,
                (uint32_t)path_after.size);
            valid = 0;
        }
    }
    return valid;
}

static int receipt_identity_equal(const receipt_row *stored,
                                  const receipt_row *current)
{
    return receipt_stat_equal(&stored->stat, &current->stat) &&
           memcmp(stored->sample_sha256, current->sample_sha256,
                  sizeof stored->sample_sha256) == 0;
}

static int receipt_remove_if_present(
    const char *path, isaac_vita_archive_receipt_diagnostic *diagnostic,
    uint32_t stage)
{
    isaac_vita_archive_receipt_stat ignored;
    int remove_result = receipt_io_remove(path);

    if (remove_result == 0)
        return 1;
    /* A failed remove is harmless only when the path is now absent. */
    if (receipt_io_stat(path, &ignored) < 0)
        return 1;
    return receipt_diag_fail(diagnostic, stage, remove_result, 0U,
                             (uint32_t)ignored.size, 0U);
}

static int receipt_publish(
    const receipt_document *candidate,
    isaac_vita_archive_receipt_diagnostic *diagnostic)
{
    receipt_document verified;
    unsigned char bytes[RECEIPT_SIZE];
    unsigned char reread[RECEIPT_SIZE];
    int target = s_state.selected_slot == 0 ? 1 : 0;
    int descriptor;
    int result;
    int status = 0;
    uint32_t mismatch;

    receipt_encode(candidate, bytes);
    if (!receipt_remove_if_present(
            s_temp_paths[target], diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_REMOVE))
        return 0;
    descriptor = receipt_io_open(s_temp_paths[target], 1);
    if (descriptor < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_OPEN,
            descriptor, 0U, 0U, 0U);
        return 0;
    }
    if (!receipt_write_exact(descriptor, bytes, RECEIPT_SIZE, diagnostic)) {
        (void)receipt_io_close(descriptor);
        (void)receipt_remove_if_present(s_temp_paths[target], NULL, 0U);
        return 0;
    }
    result = receipt_io_sync_fd(descriptor);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_SYNC,
            result, 0U, 0U, 0U);
        (void)receipt_io_close(descriptor);
        (void)receipt_remove_if_present(s_temp_paths[target], NULL, 0U);
        return 0;
    }
    result = receipt_io_close(descriptor);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_CLOSE,
            result, 0U, 0U, 0U);
        (void)receipt_remove_if_present(s_temp_paths[target], NULL, 0U);
        return 0;
    }
    if (!receipt_read_path(
            s_temp_paths[target], &verified, reread, diagnostic))
        goto cleanup;
    if (memcmp(bytes, reread, RECEIPT_SIZE) != 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RAW_COMPARE,
            0, 0U, receipt_get_u32(bytes), receipt_get_u32(reread));
        goto cleanup;
    }
    mismatch = 0U;
    if (verified.generation != candidate->generation)
        mismatch |= 0x01U;
    if (verified.valid_mask != candidate->valid_mask)
        mismatch |= 0x02U;
    if (mismatch != 0U) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SEMANTIC_COMPARE,
            0, mismatch, (uint32_t)verified.generation,
            (uint32_t)candidate->generation);
        goto cleanup;
    }
    if (!receipt_remove_if_present(
            s_slot_paths[target], diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SLOT_REMOVE))
        goto cleanup;
    result = receipt_io_rename(s_temp_paths[target], s_slot_paths[target]);
    if (result < 0) {
        receipt_diag_fail(
            diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RENAME,
            result, 0U, 0U, 0U);
        goto cleanup;
    }
    result = receipt_io_sync_device("ux0:");
    if (result < 0) {
        receipt_diag_fail(
            diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_DEVICE_SYNC,
            result, 0U, 0U, 0U);
        goto cleanup;
    }
    status = 1;

cleanup:
    if (!status)
        (void)receipt_remove_if_present(s_temp_paths[target], NULL, 0U);
    return status;
}

uint32_t isaac_vita_archive_receipt_begin(const char *archive_name)
{
    const receipt_archive_spec *spec = receipt_spec_for_name(archive_name);
    receipt_row current;
    uint32_t index;
    uint32_t token;

    if (!spec)
        return 0U;
    kage_vita_loading_note_verify();
    token = spec->kind;
    index = spec->kind - 1U;
    receipt_load_state();
    if ((s_state.document.valid_mask & (1U << index)) == 0U ||
        !receipt_inspect_archive(spec, 0, &current, NULL) ||
        !receipt_identity_equal(&s_state.document.rows[index], &current))
        return token;
    return token | ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED;
}

int isaac_vita_archive_receipt_finish(
    uint32_t token,
    uint32_t baked_skips,
    uint32_t receipt_skips,
    uint32_t original_attempts,
    uint32_t original_successes,
    uint32_t original_failures)
{
    const receipt_archive_spec *spec;
    receipt_document candidate;
    receipt_row current;
    uint32_t kind = token & ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_KIND_MASK;
    uint32_t index;
    uint32_t counter_mismatch = 0U;
    int published = 0;

    memset(&s_diagnostic, 0, sizeof s_diagnostic);
    s_diagnostic.version = ISAAC_VITA_ARCHIVE_RECEIPT_DIAGNOSTIC_VERSION;
    s_diagnostic.kind = kind;

    if ((token & ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED) != 0U) {
        receipt_diag_fail(
            &s_diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TRUSTED,
            0, token, 0U, 0U);
        goto complete;
    }
    if (token != kind) {
        receipt_diag_fail(
            &s_diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TOKEN,
            0, token, kind, 0U);
        goto complete;
    }
    spec = receipt_spec_for_kind(kind);
    if (!spec) {
        receipt_diag_fail(
            &s_diagnostic, ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TOKEN,
            0, token, kind, 0U);
        goto complete;
    }
    if (baked_skips != 0U)
        counter_mismatch |= 0x01U;
    if (receipt_skips != 0U)
        counter_mismatch |= 0x02U;
    if (original_attempts != spec->entries)
        counter_mismatch |= 0x04U;
    if (original_successes != spec->entries)
        counter_mismatch |= 0x08U;
    if (original_failures != 0U)
        counter_mismatch |= 0x10U;
    if (counter_mismatch != 0U) {
        receipt_diag_fail(
            &s_diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_COUNTERS,
            0, counter_mismatch, original_attempts, original_successes);
        goto complete;
    }
    if (!receipt_inspect_archive(spec, 1, &current, &s_diagnostic))
        goto complete;

    receipt_load_state();
    if (s_state.document.generation == UINT64_MAX) {
        receipt_diag_fail(
            &s_diagnostic,
            ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_GENERATION_OVERFLOW,
            0, 0U, UINT32_MAX, UINT32_MAX);
        goto complete;
    }
    candidate = s_state.document;
    candidate.generation = s_state.document.generation + 1U;
    candidate.valid_mask |= 1U << (kind - 1U);
    index = kind - 1U;
    candidate.rows[index] = current;
    candidate.rows[index].provenance = RECEIPT_RUNTIME_PROVENANCE;
    if (!receipt_publish(&candidate, &s_diagnostic))
        goto complete;
    s_state.document = candidate;
    s_state.selected_slot = s_state.selected_slot == 0 ? 1 : 0;
    published = 1;
    s_diagnostic.published = 1U;
    s_diagnostic.stage = ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISHED;

complete:
#if !defined(ISAAC_VITA_ARCHIVE_RECEIPT_ORACLE)
    if (kind >= RECEIPT_KIND_ANIMATIONS &&
        kind <= RECEIPT_KIND_AFTERBIRTH) {
        isaac_vita_log(
            "[archive-validation-receipt-stage] v=%u kind=%u "
            "published=%u stage=%u:%s io=0x%08x detail=0x%08x "
            "value0=0x%08x value1=0x%08x",
            (unsigned)s_diagnostic.version,
            (unsigned)s_diagnostic.kind,
            (unsigned)s_diagnostic.published,
            (unsigned)s_diagnostic.stage,
            receipt_stage_name(s_diagnostic.stage),
            (unsigned)(uint32_t)s_diagnostic.io_result,
            (unsigned)s_diagnostic.detail,
            (unsigned)s_diagnostic.value0,
            (unsigned)s_diagnostic.value1);
    }
#endif
    return published;
}

void isaac_vita_archive_receipt_get_diagnostic(
    isaac_vita_archive_receipt_diagnostic *result)
{
    if (result)
        *result = s_diagnostic;
}

#if defined(ISAAC_VITA_ARCHIVE_RECEIPT_ORACLE)
void isaac_vita_archive_receipt_oracle_reset(void)
{
    memset(&s_state, 0, sizeof s_state);
    s_state.selected_slot = -1;
    memset(&s_diagnostic, 0, sizeof s_diagnostic);
}
#endif
