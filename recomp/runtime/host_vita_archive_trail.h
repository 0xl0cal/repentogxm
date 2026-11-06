#ifndef ISAAC_HOST_VITA_ARCHIVE_TRAIL_H
#define ISAAC_HOST_VITA_ARCHIVE_TRAIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ISAAC_VITA_ARCHIVE_READ_TRAIL: the packed-archive read trail.
 *
 * The CRT FILE-token owner (host_vita_crt.c) already classifies every packed
 * archive token by its basename (isaac_vita_archive_cache_file.key) and sees
 * every guest fopen/fseek/fread/fclose on it under its FILE-registry lock.
 * ArchivedFile reads one entry as fopen, File::seek(offset, SEEK_SET) from
 * the pinned constructor seam (ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA),
 * a 4-byte block header plus a <= 0x800-byte payload per block, fclose; a
 * 400 KiB sheet is therefore ~1000 logical freads.  A per-fread log line is
 * out of the question (millions of freads at startup, a 256 KiB async ring),
 * so the trail is recorded per RUN: one record per successful seek (or per
 * first read after open without a seek) that absorbs every fread until the
 * next seek or the fclose.  A run is the exact (archive, offset, bytes)
 * window a read-ahead worker would have to warm.
 *
 * This module is the pure accumulator.  It is not synchronized: the CRT calls
 * it while holding its FILE-registry lock, exactly like the archive cache.
 * The phase reporter (kage_vita_room_profile.h) takes one window's records
 * through the CRT's try-lock wrapper and prints them as ph120.rt lines behind
 * one ph120.rts summary.  The record store is bounded (CAP per window); runs
 * beyond it are counted in `drop`, never blocked on.  The clock is the
 * caller's (sceKernelGetProcessTimeWide on the device, so `at` compares with
 * ph120.s at= and roomlog at=); it is read only when a run opens, never per
 * fread.
 *
 * Record shape printed by the reporter:
 *   [kage-vita] ph120.rts bid= win= loops= valid=1 abi=1 runs= rec= drop= cap=
 *               bytes= freads= seeks= fail= empty= back= opens= closes=
 *               implicit= keys= sat= miss=
 *   [kage-vita] ph120.rt  bid= win= loops= a=<archive basename> o=<hex offset>
 *               n=<bytes read> c=<fread calls> k=<kind><end> at=<hex16 us>
 * kind: S = ArchivedFile constructor seek (the pinned seam), O = another
 * SEEK_SET, C = SEEK_CUR, E = SEEK_END, I = implicit (read after open with
 * no seek); end: c = the run ended by fclose, s = by a further seek.  An
 * unknown offset (SEEK_END, or SEEK_CUR from an unknown cursor) prints as
 * ffffffff.  Non-seam runs with zero freads are counted (`empty`) but not
 * recorded, so the GetSize SEEK_END/SEEK_SET pair does not cost a record.
 * `back` counts run offsets below the previous known offset of the same
 * archive, across the whole process (the layout question). */

#ifndef ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP
#define ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP 64U
#endif
/* Mirrors ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY without including the cache. */
#define ISAAC_VITA_ARCHIVE_TRAIL_KEY_CAPACITY 64U
/* The v1 packed corpus has 18 archives; two spare slots. */
#define ISAAC_VITA_ARCHIVE_TRAIL_KEYS 20U
#define ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE 0xffU
#define ISAAC_VITA_ARCHIVE_TRAIL_OFFSET_UNKNOWN 0xffffffffU
#define ISAAC_VITA_ARCHIVE_TRAIL_ABI 1U

enum {
    ISAAC_VITA_ARCHIVE_TRAIL_KIND_SEAM = 'S',
    ISAAC_VITA_ARCHIVE_TRAIL_KIND_SET = 'O',
    ISAAC_VITA_ARCHIVE_TRAIL_KIND_CUR = 'C',
    ISAAC_VITA_ARCHIVE_TRAIL_KIND_END = 'E',
    ISAAC_VITA_ARCHIVE_TRAIL_KIND_IMPLICIT = 'I'
};

/* Sticky saturation bits reported as sat=. */
enum {
    ISAAC_VITA_ARCHIVE_TRAIL_SAT_KEYS = 1U << 0,
    ISAAC_VITA_ARCHIVE_TRAIL_SAT_BYTES = 1U << 1,
    ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS = 1U << 2
};

typedef struct isaac_vita_archive_trail_run {
    uint64_t at;
    uint32_t offset;
    uint32_t bytes;
    uint32_t freads;
    uint8_t key;
    uint8_t kind;
    uint8_t closed;
    uint8_t reserved;
} isaac_vita_archive_trail_run;

/* Per FILE-token state, embedded in the CRT token table. */
typedef struct isaac_vita_archive_trail_token {
    uint64_t at;
    int64_t cursor;
    int64_t run_offset;
    uint32_t run_bytes;
    uint32_t run_freads;
    uint8_t run_open;
    uint8_t run_kind;
    uint8_t key;
    uint8_t keyed;
} isaac_vita_archive_trail_token;

typedef struct isaac_vita_archive_trail_window {
    uint32_t abi;
    uint32_t valid;
    uint32_t count;
    uint32_t drop;
    uint32_t runs;
    uint32_t bytes;
    uint32_t freads;
    uint32_t seeks;
    uint32_t fails;
    uint32_t empty;
    uint32_t back;
    uint32_t opens;
    uint32_t closes;
    uint32_t implicit;
    uint32_t key_count;
    uint32_t sat;
    isaac_vita_archive_trail_run record[ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP];
    char keys[ISAAC_VITA_ARCHIVE_TRAIL_KEYS]
             [ISAAC_VITA_ARCHIVE_TRAIL_KEY_CAPACITY];
} isaac_vita_archive_trail_window;

/* A published token.  `key` NULL or empty means the token is not a packed
 * archive: the state is cleared and every later call is a no-op. */
void isaac_vita_archive_trail_open(isaac_vita_archive_trail_token *token,
                                   const char *key, uint64_t now);

/* One if the next read on this token would open an implicit run and so
 * needs `now`; the CRT reads its clock only then. */
int isaac_vita_archive_trail_needs_clock(
    const isaac_vita_archive_trail_token *token);

/* `bytes` is size * elements_returned of the completed fread. */
void isaac_vita_archive_trail_read(isaac_vita_archive_trail_token *token,
                                   uint32_t bytes, uint64_t now);

/* After the CRT's fseek dispatch.  `result` is the fseek return, `origin`
 * a stdio SEEK_* value, `known_position` the post-seek position when the CRT
 * already computed it (-1 otherwise), `seam` one for the ArchivedFile
 * constructor parent return. */
void isaac_vita_archive_trail_seek(isaac_vita_archive_trail_token *token,
                                   int32_t offset, int origin, int result,
                                   int64_t known_position, int seam,
                                   uint64_t now);

/* Before the CRT's lane close clears the key. */
void isaac_vita_archive_trail_close(isaac_vita_archive_trail_token *token);

/* Copy and reset the window accumulator.  Keys and per-archive last offsets
 * persist for the process so `a=` names and `back` stay stable. */
void isaac_vita_archive_trail_take(isaac_vita_archive_trail_window *out);

/* Implemented by the CRT: try its FILE-registry lock, take, unlock.  Zero
 * means the lock was busy and nothing was reset (the reporter counts a miss
 * and the records stay for the next window). */
int isaac_vita_crt_archive_trail_take(isaac_vita_archive_trail_window *out);

#ifdef __cplusplus
}
#endif

#endif
