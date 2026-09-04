#ifndef ISAAC_VITA_ARCHIVE_CACHE_H
#define ISAAC_VITA_ARCHIVE_CACHE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only the archive basename is retained with an active logical FILE.  Names
 * which do not fit this bound remain correct and simply bypass the cache. */
#define ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY 64U

typedef struct isaac_vita_archive_cache_file {
    FILE *stream;
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY];
} isaac_vita_archive_cache_file;

#define ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER { NULL, { 0 } }

/* The cache and CRT share this bounded in-memory history.  It is deliberately
 * not a per-call durable logger: packed-resource startup performs millions of
 * reads.  A fatal boundary can snapshot the newest tail, while only an actual
 * cursor anomaly is written immediately. */
#define ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY 32U
#define ISAAC_VITA_ARCHIVE_DIAG_LOG_CAPACITY 384U

typedef enum isaac_vita_archive_diag_kind {
    ISAAC_VITA_ARCHIVE_DIAG_OPEN = 1,
    ISAAC_VITA_ARCHIVE_DIAG_CACHE_RESET = 2,
    ISAAC_VITA_ARCHIVE_DIAG_FSEEK = 3,
    ISAAC_VITA_ARCHIVE_DIAG_FREAD = 4
} isaac_vita_archive_diag_kind;

enum {
    ISAAC_VITA_ARCHIVE_DIAG_CACHE_HIT = 1U << 0,
    ISAAC_VITA_ARCHIVE_DIAG_EOF = 1U << 1,
    ISAAC_VITA_ARCHIVE_DIAG_STREAM_ERROR = 1U << 2,
    ISAAC_VITA_ARCHIVE_DIAG_RELEVANT = 1U << 3,
    ISAAC_VITA_ARCHIVE_DIAG_FTELL_FAILED = 1U << 4,
    ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT = 1U << 5
};

typedef struct isaac_vita_archive_diag_event {
    uint32_t sequence;
    uint32_t kind;
    uint32_t token;
    uint32_t flags;
    int32_t requested_offset;
    int32_t origin;
    int32_t operation_result;
    int32_t position_before;
    int32_t position_after;
    int32_t operation_errno;
    uint32_t element_size;
    uint32_t element_count;
    uint32_t elements_returned;
    uint32_t caller_return;
    uint32_t observed_word;
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY];
} isaac_vita_archive_diag_event;

/* Callers serialize these with the Vita CRT FILE-registry lock.  record()
 * assigns a monotonically increasing sequence to both the caller's event and
 * the retained ring copy.  snapshot() returns the newest chronological tail
 * and reports the last assigned sequence through `latest_sequence`. */
uint32_t isaac_vita_archive_diag_record(
    isaac_vita_archive_diag_event *event);
uint32_t isaac_vita_archive_diag_snapshot(
    isaac_vita_archive_diag_event *events, uint32_t capacity,
    uint32_t *latest_sequence);
/* Pure formatter shared with the host oracle.  A 96-character build identity
 * is named and retained as a prefix so the fixed event tail fits one durable
 * logger body without truncation. */
int isaac_vita_archive_diag_format_event(
    char *line, size_t capacity, const char *build_id,
    const char *reason, const isaac_vita_archive_diag_event *event);

/* Classify exact read-only archives directly below the immutable packed-data
 * root.  On success, copy the bounded basename to `key`; on failure, clear
 * `key`.  The allocation-free raw backend accepts music.a because it owns a
 * distinct SceIo descriptor and does not consume or retain a newlib FILE. */
int isaac_vita_archive_raw_fallback_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY]);

/* The one-idle FILE cache has a deliberately narrower policy than the raw
 * backend: music.a remains a normal non-cached open.  Keep this decision
 * separate so changing descriptor reuse cannot disable a safe raw fallback. */
int isaac_vita_archive_cache_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY]);

/* These calls are intentionally not internally synchronized.  The Vita CRT
 * calls them while holding its existing FILE-registry lock, which keeps cache
 * state and token ownership on one boundary.
 *
 * A cache hit is returned only for exact "rb" opens directly below
 * ISAAC_VITA_NATIVE_DATA_ROOT "/resources/packed/", with a non-empty `.a`
 * basename other than music.a.  Every returned file owns a distinct active
 * FILE; an idle stream is detached before it can be returned.  `cache_hit` is
 * optional and is set to one only when an idle stream was reset and reused.
 * This lets the CRT avoid calling setvbuf again after the stream has already
 * performed I/O. */
FILE *isaac_vita_archive_cache_open(isaac_vita_archive_cache_file *file,
                                    const char *native_path,
                                    const char *mode,
                                    int *cache_hit);

/* Logical close.  An eligible stream becomes the sole idle stream and returns
 * success without a native fclose.  Bypassed streams retain native fclose
 * behavior.  Packed archives are therefore required to be immutable for the
 * process lifetime: retaining a descriptor cannot preserve replacement and
 * delayed-close-error semantics of an arbitrary mutable filesystem object. */
int isaac_vita_archive_cache_close(isaac_vita_archive_cache_file *file);

/* Unconditionally close an active native stream.  Use this when an fopen
 * succeeded but the guest token cannot be published (pointer conversion or a
 * full token table); such a failed logical open must never populate the idle
 * cache. */
int isaac_vita_archive_cache_force_discard(
    isaac_vita_archive_cache_file *file);

/* Close and forget the idle stream, if any.  This is also the shutdown edge.
 * Calling it more than once is harmless. */
int isaac_vita_archive_cache_drop(void);

#ifdef __cplusplus
}
#endif

#endif
