#ifndef ISAAC_MANAGER_ARCH_H
#define ISAAC_MANAGER_ARCH_H

/*
 * Read-only reader for the game's KAGE ARCH000 archives (the ".a" files in
 * ux0:/data/isaacr001/resources/packed), for the manager's game skin
 * (theme=game).  SDK-free; all I/O goes through
 * struct manager_blob_io and all memory is the caller's (no allocation).
 *
 * Every format fact is copied from tools/isaac_vita_repack.py (module
 * docstring, Archive._parse, iter_pieces, IsaacKeystream, mount_checksum)
 * and the path hash from recomp/test_room_entry_corpus.py name_hash():
 *
 *   header   "<7sBIH": "ARCH000", mode u8, index offset u32, count u16
 *   index    count rows "<IIIII": hash_a, hash_b, offset, size, checksum;
 *            it must close exactly at EOF and every offset must lie in
 *            [14, index offset] (Archive._parse)
 *   mode 2   (MiniZ) pieces: u32 header = length | final << 31, length <=
 *            0x7ff; a non-final piece of exactly 0x400 bytes latches the
 *            entry into stored mode for good (keystream XOR, seeded from
 *            hash_b); otherwise raw deflate through one tinfl state per
 *            entry, each non-final piece yielding exactly 0x400 bytes and
 *            matches reaching at most 0x400 bytes back (the reader's ring)
 *   checksum h = 0xababeb98; per dword h = ror32(h, 1) + dword, read in
 *            0x200-byte chunks (the engine drops entries that fail it)
 *
 * Modes 0 (RAW_XOR) and 1 (LZW) are indexed but not decoded here: their
 * entries are reported by manager_arch_read() as MANAGER_ARCH_E_MODE and
 * skipped by manager_arch_set_find() (graphics.a is mode 0
 * and does not decode with the stored-piece rule).
 */

#include <stddef.h>
#include <stdint.h>

#include "host_vita_archive_miniz_native.h"
#include "manager_blob.h"

#define MANAGER_ARCH_HEADER_BYTES 14u
#define MANAGER_ARCH_INDEX_ENTRY_BYTES 20u
#define MANAGER_ARCH_MODE_RAW_XOR 0u
#define MANAGER_ARCH_MODE_LZW 1u
#define MANAGER_ARCH_MODE_MINIZ 2u
#define MANAGER_ARCH_BLOCK 0x400u
#define MANAGER_ARCH_PIECE_MAX 0x7ffu
#define MANAGER_ARCH_CHECKSUM_SEED 0xababeb98u
#define MANAGER_ARCH_CHECKSUM_CHUNK 0x200u
/* Read-ahead window of one entry decode; holds at least one whole piece
 * (4-byte header + 0x7ff payload). */
#define MANAGER_ARCH_WINDOW_BYTES 0x4000u

/* Same root as MANAGER_STORAGE_ROOT (manager_storage.h). */
#define MANAGER_ARCH_PACKED_DIR "ux0:/data/isaacr001/resources/packed"
#define MANAGER_ARCH_SET_MAX 8u

enum manager_arch_status {
    MANAGER_ARCH_OK = 0,
    MANAGER_ARCH_E_ARGUMENT = -1,  /* NULL pointer, bad row */
    MANAGER_ARCH_E_IO = -2,        /* open/pread failed or ended early */
    MANAGER_ARCH_E_HEADER = -3,    /* shorter than 14 bytes / not ARCH000 */
    MANAGER_ARCH_E_INDEX = -4,     /* index not closing at EOF, offset
                                      outside the data area */
    MANAGER_ARCH_E_CAPACITY = -5,  /* caller buffer too small */
    MANAGER_ARCH_E_MODE = -6,      /* archive mode is not MiniZ (2) */
    MANAGER_ARCH_E_PIECE = -7,     /* piece > 0x7ff, crosses the entry
                                      limit, stored piece > 0x400 */
    MANAGER_ARCH_E_INFLATE = -8,   /* tinfl failure, reserved symbol, wrong
                                      piece cadence (status/consumed/size) */
    MANAGER_ARCH_E_SIZE = -9,      /* pieces do not cover the index size */
    MANAGER_ARCH_E_CHECKSUM = -10, /* mount checksum differs from the row */
    MANAGER_ARCH_E_PATH = -11,     /* path is not ASCII */
    MANAGER_ARCH_E_STATE = -12     /* archive not open / index not loaded */
};

struct manager_arch {
    const struct manager_blob_io *io;
    int handle;
    int is_open;
    uint64_t file_size;
    uint32_t mode;
    uint32_t index_offset;
    uint32_t count;
    /* count * 20 raw little-endian bytes in the caller's buffer; NULL until
     * manager_arch_load_index() succeeds. */
    const uint8_t *index;
};

struct manager_arch_entry {
    uint32_t row;
    uint32_t hash_a;
    uint32_t hash_b;
    uint32_t offset;
    uint32_t size;
    uint32_t checksum;
    /* First byte after the payload: the next higher row offset, else the
     * index offset (Archive.payload_limits). */
    uint32_t limit;
};

/* IsaacKeystream of tools/isaac_vita_repack.py (PCG32 seeding from hash_b,
 * Jenkins randinit(TRUE), isaac()). */
struct manager_arch_keystream {
    uint32_t rsl[256];
    uint32_t mm[256];
    uint32_t aa;
    uint32_t bb;
    uint32_t cc;
    uint32_t index;
};

/* One entry decode's working memory (about 29 KiB); caller-owned, reusable
 * across entries and archives, not shared between concurrent reads. */
struct manager_arch_work {
    uint32_t tinfl_state[ISAAC_VITA_ARCHIVE_MINIZ_STATE_BYTES / 4u];
    struct manager_arch_keystream keystream;
    uint64_t window_offset;
    uint32_t window_bytes;
    uint8_t window[MANAGER_ARCH_WINDOW_BYTES];
    /* Counters of the last manager_arch_read() (diagnostics/tests). */
    uint32_t preads;
    uint32_t deflate_pieces;
    uint32_t stored_pieces;
};

/* An ordered list of opened archives; the first archive holding a path wins
 * (the skin uses repentance.a > afterbirthp.a > afterbirth.a, a cosmetic
 * choice: every candidate copy is the same drawing). */
struct manager_arch_set {
    struct manager_arch *archives[MANAGER_ARCH_SET_MAX];
    uint32_t count;
};

/* Prefixes tried per archive, in this order (as the design's asset dump). */
extern const char *const manager_arch_prefixes[2];
/* The skin's archive order, file names relative to MANAGER_ARCH_PACKED_DIR. */
extern const char *const manager_arch_default_order[3];

void manager_arch_init(struct manager_arch *arch);

/* Opens the file, reads and validates the header (magic, index closing at
 * EOF).  On failure the archive is left closed. */
int manager_arch_open(struct manager_arch *arch,
                      const struct manager_blob_io *io, const char *path);

/* Bytes manager_arch_load_index() needs (count * 20); 0 when not open. */
uint32_t manager_arch_index_bytes(const struct manager_arch *arch);

/* Reads the index into the caller's buffer and validates every offset.
 * The buffer must outlive the archive's use. */
int manager_arch_load_index(struct manager_arch *arch, void *buffer,
                            size_t capacity);

void manager_arch_close(struct manager_arch *arch);

/* hash_a/hash_b of prefix + path (prefix may be NULL), ASCII-lower-cased
 * like name_hash(); MANAGER_ARCH_E_PATH for a byte >= 0x80. */
int manager_arch_name_hash(const char *prefix, const char *path,
                           uint32_t *hash_a, uint32_t *hash_b);

/* Row accessor (row < count). */
int manager_arch_entry_at(const struct manager_arch *arch, uint32_t row,
                          struct manager_arch_entry *entry);

/* 1 = found (first row in index order), 0 = absent, < 0 = error. */
int manager_arch_find(const struct manager_arch *arch, const char *prefix,
                      const char *path, struct manager_arch_entry *entry);

/* Decodes the whole entry (entry->size bytes) into output and checks the
 * mount checksum.  On MANAGER_ARCH_E_CHECKSUM the output holds the decoded
 * bytes (the caller may still reject them); on every other error its
 * content is unspecified.  Never writes past output[entry->size]. */
int manager_arch_read(struct manager_arch *arch,
                      const struct manager_arch_entry *entry, void *output,
                      size_t capacity, struct manager_arch_work *work);

/* Tries every archive in order, both prefixes each, skipping archives
 * without a loaded index or not in MiniZ mode.  1 = found (*archive_index
 * and *entry set), 0 = absent, < 0 = error. */
int manager_arch_set_find(const struct manager_arch_set *set,
                          const char *path, uint32_t *archive_index,
                          struct manager_arch_entry *entry);

void manager_arch_keystream_init(struct manager_arch_keystream *keystream,
                                 uint32_t seed);
/* XORs n bytes exactly as one stored piece: consumes ceil(n / 4) words.
 * dst may equal src. */
void manager_arch_keystream_xor(struct manager_arch_keystream *keystream,
                                uint8_t *dst, const uint8_t *src, uint32_t n);

uint32_t manager_arch_mount_checksum(const uint8_t *data, uint32_t size);

/* Stable short name of a status ("ok", "io", "index", ...). */
const char *manager_arch_status_text(int status);

#endif
