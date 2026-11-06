#ifndef ISAAC_HOST_VITA_ARCHIVE_SIDECAR_H
#define ISAAC_HOST_VITA_ARCHIVE_SIDECAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Archive validation sidecar (CMake ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR,
 * default OFF; requires ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT).
 *
 * FileManager::LoadArchiveFile (sub_00563880) validates every packed entry
 * at mount by decoding it and folding a checksum.  The Vita port skips that
 * work for entries whose index checksum matches the executable's baked table
 * (ISAAC_VITA_ARCHIVE_VALIDATION_SKIP) and, for animations.a/afterbirth.a,
 * for archives whose exact pinned identity a runtime receipt has already
 * proven (ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT).  An archive rewritten by
 * tools/isaac_vita_halve_sfx.py has new index checksums for its rewritten
 * entries and, for afterbirth.a, a new size and index, so both skips fail
 * and the original validation runs on every boot (measured on the device:
 * afterbirth.a alone 63-66 s).
 *
 * The tool therefore writes, next to each archive it produces, a 256-byte
 * sidecar "<archive>.receipt" after verifying every entry of the written
 * archive against its index checksum.  This module interposes
 * isaac_vita_archive_receipt_begin through the linker's --wrap: when the real
 * receipt does not trust the archive, it looks for the sidecar, checks the
 * archive's identity against it (size, ARCH000 header, SHA-256 of the whole
 * index table, SHA-256 of 64 x 4 KiB samples of the entry region) and, only
 * if everything matches, returns the token with the trusted bit so the
 * generated owner takes the same per-entry skip it takes for a trusted
 * receipt.  A missing or mismatching sidecar returns the real token
 * unchanged, i.e. the original validation loop.
 *
 * Token layout: ISAAC_VITA_ARCHIVE_RECEIPT_TOKEN_TRUSTED (0x100) is the only
 * bit generated code reads; 0x200 marks a sidecar-derived trust in the boot
 * log ("token=768" for a non-pinned archive, 0x302 for afterbirth.a).
 */
#define ISAAC_VITA_ARCHIVE_SIDECAR_TOKEN        0x200U
#define ISAAC_VITA_ARCHIVE_SIDECAR_SIZE         256U
#define ISAAC_VITA_ARCHIVE_SIDECAR_VERSION      1U
#define ISAAC_VITA_ARCHIVE_SIDECAR_SAMPLE_COUNT 64U
#define ISAAC_VITA_ARCHIVE_SIDECAR_SAMPLE_SIZE  4096U
#define ISAAC_VITA_ARCHIVE_SIDECAR_NAME_CAPACITY 40U
#define ISAAC_VITA_ARCHIVE_SIDECAR_SUFFIX       ".receipt"
#define ISAAC_VITA_ARCHIVE_SIDECAR_PACKED_PREFIX "resources/packed/"

/* Sidecar byte layout (little-endian, 256 bytes):
 *   0   16  magic "ISAAC-A-SIDECAR1"
 *   16  4   version (1)
 *   20  4   record size (256)
 *   24  8   archive size in bytes
 *   32  4   index (table) offset
 *   36  4   entry count
 *   40  4   archive mode (ARCH000 version byte)
 *   44  4   sample count (64)
 *   48  4   sample size (4096)
 *   52  12  reserved, zero
 *   64  32  SHA-256 of the index table (entry count x 20 bytes)
 *   96  32  SHA-256 of the sampled entry region (see sample windows)
 *   128 40  archive base name, NUL padded ("afterbirthp.a")
 *   168 56  reserved, zero
 *   224 32  SHA-256 of bytes 0..223
 * Sample windows: when the entry region (bytes 0..index offset) is at most
 * 64 x 4096 bytes it is hashed completely in consecutive 4096-byte windows;
 * otherwise 64 windows of 4096 bytes start at (index_offset - 4096) * i / 63.
 * Each window contributes (u64 offset, u32 length, bytes) after a domain
 * prefix "isaac-vita-sidecar-sample-v1" and the record's numbers. */

typedef enum isaac_vita_archive_sidecar_stage {
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NONE = 0,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ALREADY_TRUSTED = 1,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_NAME = 2,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_ABSENT = 3,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_IDENTITY = 10,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_OPEN = 11,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_READ = 12,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_CLOSE = 13,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_DECODE = 14,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_SIDECAR_NAME = 15,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_STAT = 20,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_IDENTITY = 21,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_OPEN = 22,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FSTAT = 23,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_CROSS_IDENTITY = 24,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER_READ = 25,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_HEADER = 26,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_READ = 27,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_INDEX_HASH = 28,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_READ = 29,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_SAMPLE_HASH = 30,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FSTAT_AFTER = 31,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_FD_CHANGED = 32,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_ARCHIVE_CLOSE = 33,
    ISAAC_VITA_ARCHIVE_SIDECAR_STAGE_TRUSTED = 100
} isaac_vita_archive_sidecar_stage;

typedef struct isaac_vita_archive_sidecar_diagnostic {
    uint32_t stage;
    int32_t io_result;
    uint32_t detail;
    uint32_t value0;
    uint32_t value1;
    uint32_t entries;
    uint64_t size;
} isaac_vita_archive_sidecar_diagnostic;

/*
 * Given the exact guest archive name passed to FileManager::LoadArchiveFile
 * and the token the real receipt returned for it, return the token to hand
 * to generated code.  Returns real_token unchanged unless a valid sidecar
 * proves the archive's identity, in which case the trusted and sidecar bits
 * are added.  Never returns a trusted token without both.
 */
uint32_t isaac_vita_archive_sidecar_trust(const char *archive_name,
                                          uint32_t real_token);

void isaac_vita_archive_sidecar_get_diagnostic(
    isaac_vita_archive_sidecar_diagnostic *result);

const char *isaac_vita_archive_sidecar_stage_name(uint32_t stage);

#if defined(ISAAC_VITA_ARCHIVE_SIDECAR_ORACLE)
/* Host oracle I/O: paths are the Vita spellings; the oracle maps them. */
int isaac_vita_archive_sidecar_oracle_stat(const char *path, uint64_t *size,
                                           int *regular);
int isaac_vita_archive_sidecar_oracle_fstat(int descriptor, uint64_t *size,
                                            int *regular);
int isaac_vita_archive_sidecar_oracle_open(const char *path);
int isaac_vita_archive_sidecar_oracle_close(int descriptor);
int isaac_vita_archive_sidecar_oracle_pread(int descriptor, void *buffer,
                                            uint32_t size, uint64_t offset);
#else
uint32_t __real_isaac_vita_archive_receipt_begin(const char *archive_name);
uint32_t __wrap_isaac_vita_archive_receipt_begin(const char *archive_name);
#endif

#ifdef __cplusplus
}
#endif

#endif
