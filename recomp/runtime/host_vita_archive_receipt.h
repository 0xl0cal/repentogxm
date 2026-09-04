#ifndef ISAAC_HOST_VITA_ARCHIVE_RECEIPT_H
#define ISAAC_HOST_VITA_ARCHIVE_RECEIPT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tokens are deliberately opaque to generated code except for the trusted
 * bit.  A non-zero, non-trusted token names a frozen archive which must keep
 * the original validation path.  The trusted bit is returned only after the
 * persistent receipt and the current cheap file identity both pass.
 */
#define ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_KIND_MASK 0xffU
#define ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED   0x100U

/* Stable, compact finish diagnostics.  One record is retained in memory and
 * the Vita runtime emits it once for each target finish call. */
#define ISAAC_VITA_ARCHIVE_RECEIPT_DIAGNOSTIC_VERSION 1U

typedef enum isaac_vita_archive_receipt_stage {
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_NONE = 0,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TRUSTED = 1,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_TOKEN = 2,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_FINISH_COUNTERS = 3,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SPEC = 10,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT = 11,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_IDENTITY = 12,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_OPEN = 13,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT = 14,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CROSS_IDENTITY = 15,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_HEADER_READ = 16,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_HEADER = 17,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_INDEX_READ = 18,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_INDEX_HASH = 19,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_SAMPLE_READ = 20,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_READ = 21,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FULL_HASH = 22,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FSTAT_AFTER = 23,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_FD_CHANGED = 24,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_CLOSE = 25,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_STAT_AFTER = 26,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_INSPECT_PATH_CHANGED = 27,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_GENERATION_OVERFLOW = 30,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_REMOVE = 40,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_OPEN = 41,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_WRITE = 42,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_SYNC = 43,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_TEMP_CLOSE = 44,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT = 50,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_IDENTITY = 51,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_OPEN = 52,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT = 53,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CROSS_IDENTITY = 54,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_BYTES = 55,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FSTAT_AFTER = 56,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_FD_CHANGED = 57,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_CLOSE = 58,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_STAT_AFTER = 59,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_PATH_CHANGED = 60,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_READ_DECODE = 61,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RAW_COMPARE = 62,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SEMANTIC_COMPARE = 63,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_SLOT_REMOVE = 64,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_RENAME = 65,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISH_DEVICE_SYNC = 66,
    ISAAC_VITA_ARCHIVE_RECEIPT_STAGE_PUBLISHED = 100
} isaac_vita_archive_receipt_stage;

typedef struct isaac_vita_archive_receipt_diagnostic {
    uint32_t version;
    uint32_t kind;
    uint32_t published;
    uint32_t stage;
    int32_t io_result;
    uint32_t detail;
    uint32_t value0;
    uint32_t value1;
} isaac_vita_archive_receipt_diagnostic;

void isaac_vita_archive_receipt_get_diagnostic(
    isaac_vita_archive_receipt_diagnostic *result);

/* Exact frozen names passed to FileManager::LoadArchiveFile. */
#define ISAAC_VITA_ARCHIVE_RECEIPT_ANIMATIONS_NAME \
    "resources/packed/animations.a"
#define ISAAC_VITA_ARCHIVE_RECEIPT_AFTERBIRTH_NAME \
    "resources/packed/afterbirth.a"

/*
 * Begin is read-only.  Unknown names and every I/O/format/identity failure
 * return zero or an untrusted token, so the caller retains the original
 * validation loop.
 */
uint32_t isaac_vita_archive_receipt_begin(const char *archive_name);

/*
 * Finish is reached only at the frozen normal loop-exit seam.  It publishes a
 * receipt only when the generated counters prove that every target entry took
 * the original path and succeeded.  Publishing also performs one exact full
 * SHA-256 pass over the target archive.  Return values are diagnostic only:
 * 1 means a new generation was durably attempted and selected in memory;
 * zero includes non-target/trusted/incomplete/failing calls.
 */
int isaac_vita_archive_receipt_finish(
    uint32_t token,
    uint32_t baked_skips,
    uint32_t receipt_skips,
    uint32_t original_attempts,
    uint32_t original_successes,
    uint32_t original_failures);

typedef struct isaac_vita_archive_receipt_date {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
    uint32_t microsecond;
} isaac_vita_archive_receipt_date;

typedef struct isaac_vita_archive_receipt_stat {
    uint64_t size;
    isaac_vita_archive_receipt_date ctime;
    isaac_vita_archive_receipt_date mtime;
    uint32_t regular;
} isaac_vita_archive_receipt_stat;

#if defined(ISAAC_VITA_ARCHIVE_RECEIPT_ORACLE)
int isaac_vita_archive_receipt_oracle_stat(
    const char *path, isaac_vita_archive_receipt_stat *result);
int isaac_vita_archive_receipt_oracle_fstat(
    int descriptor, isaac_vita_archive_receipt_stat *result);
int isaac_vita_archive_receipt_oracle_open(
    const char *path, int write_exclusive, uint32_t create_mode);
int isaac_vita_archive_receipt_oracle_close(int descriptor);
int isaac_vita_archive_receipt_oracle_pread(
    int descriptor, void *buffer, uint32_t size, uint64_t offset);
int isaac_vita_archive_receipt_oracle_write(
    int descriptor, const void *buffer, uint32_t size);
int isaac_vita_archive_receipt_oracle_sync_fd(int descriptor);
int isaac_vita_archive_receipt_oracle_remove(const char *path);
int isaac_vita_archive_receipt_oracle_rename(
    const char *source, const char *destination);
int isaac_vita_archive_receipt_oracle_sync_device(const char *device);

/* Host-only reset: production receipt state is process-lifetime state. */
void isaac_vita_archive_receipt_oracle_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
