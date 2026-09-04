#include "host_vita_archive_cache.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "platform.h"

#if defined(ISAAC_VITA_ARCHIVE_CACHE_ORACLE)
FILE *isaac_vita_archive_cache_oracle_fopen(const char *path,
                                            const char *mode);
int isaac_vita_archive_cache_oracle_fclose(FILE *stream);
int isaac_vita_archive_cache_oracle_fseek(FILE *stream, long offset,
                                          int origin);
long isaac_vita_archive_cache_oracle_ftell(FILE *stream);
void isaac_vita_archive_cache_oracle_clearerr(FILE *stream);
#define archive_native_fopen    isaac_vita_archive_cache_oracle_fopen
#define archive_native_fclose   isaac_vita_archive_cache_oracle_fclose
#define archive_native_fseek    isaac_vita_archive_cache_oracle_fseek
#define archive_native_ftell    isaac_vita_archive_cache_oracle_ftell
#define archive_native_clearerr isaac_vita_archive_cache_oracle_clearerr
#else
#define archive_native_fopen    fopen
#define archive_native_fclose   fclose
#define archive_native_fseek    fseek
#define archive_native_ftell    ftell
#define archive_native_clearerr clearerr
#endif

#define ISAAC_VITA_ARCHIVE_CACHE_PREFIX \
    ISAAC_VITA_NATIVE_DATA_ROOT "/resources/packed/"

typedef struct isaac_vita_archive_cache_idle {
    FILE *stream;
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY];
} isaac_vita_archive_cache_idle;

static isaac_vita_archive_cache_idle s_idle;
static isaac_vita_archive_diag_event
    s_archive_diag_ring[ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY];
static uint32_t s_archive_diag_sequence;

static const char *archive_diag_kind_name(uint32_t kind)
{
    if (kind == ISAAC_VITA_ARCHIVE_DIAG_OPEN)
        return "open";
    if (kind == ISAAC_VITA_ARCHIVE_DIAG_CACHE_RESET)
        return "cache-reset";
    if (kind == ISAAC_VITA_ARCHIVE_DIAG_FSEEK)
        return "fseek";
    if (kind == ISAAC_VITA_ARCHIVE_DIAG_FREAD)
        return "fread";
    return "unknown";
}

int isaac_vita_archive_diag_format_event(
    char *line, size_t capacity, const char *build_id,
    const char *reason, const isaac_vita_archive_diag_event *event)
{
    if (!line || !capacity || !build_id || !event)
        return -1;
    return snprintf(
        line, capacity,
        "arcdiag v1 tag=archive-cursor-v1 bid_prefix=%.40s why=%.24s "
        "seq=%u op=%.12s key=%.63s "
        "tok=%08x fl=%02x call=%08x off=%08x org=%d rc=%d "
        "pos=%08x>%08x err=%d io=%ux%u/%u word=%08x",
        build_id,
        reason ? reason : "snapshot", (unsigned)event->sequence,
        archive_diag_kind_name(event->kind),
        event->key[0] ? event->key : "-", (unsigned)event->token,
        (unsigned)event->flags, (unsigned)event->caller_return,
        (unsigned)event->requested_offset, event->origin,
        event->operation_result, (unsigned)event->position_before,
        (unsigned)event->position_after, event->operation_errno,
        (unsigned)event->element_size, (unsigned)event->element_count,
        (unsigned)event->elements_returned,
        (unsigned)event->observed_word);
}

uint32_t isaac_vita_archive_diag_record(
    isaac_vita_archive_diag_event *event)
{
    uint32_t sequence;
    uint32_t slot;

    if (!event)
        return 0U;
    sequence = s_archive_diag_sequence + 1U;
    if (!sequence) {
        /* Startup reaches only a few million records.  Still keep the public
         * chronology well-defined if a deliberately long-running oracle
         * crosses UINT32_MAX. */
        memset(s_archive_diag_ring, 0, sizeof s_archive_diag_ring);
        sequence = 1U;
    }
    s_archive_diag_sequence = sequence;
    event->sequence = sequence;
    event->key[sizeof event->key - 1U] = '\0';
    slot = (sequence - 1U) % ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY;
    s_archive_diag_ring[slot] = *event;
    return sequence;
}

uint32_t isaac_vita_archive_diag_snapshot(
    isaac_vita_archive_diag_event *events, uint32_t capacity,
    uint32_t *latest_sequence)
{
    uint32_t available = s_archive_diag_sequence;
    uint32_t first_sequence;
    uint32_t index;

    if (latest_sequence)
        *latest_sequence = s_archive_diag_sequence;
    if (available > ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY)
        available = ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY;
    if (!events || !capacity)
        return 0U;
    if (available > capacity)
        available = capacity;
    first_sequence = s_archive_diag_sequence - available + 1U;
    for (index = 0U; index < available; ++index) {
        uint32_t sequence = first_sequence + index;
        uint32_t slot =
            (sequence - 1U) % ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY;

        events[index] = s_archive_diag_ring[slot];
    }
    return available;
}

static void archive_cache_file_clear(isaac_vita_archive_cache_file *file)
{
    file->stream = NULL;
    file->key[0] = '\0';
}

static int archive_packed_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY])
{
    static const char prefix[] = ISAAC_VITA_ARCHIVE_CACHE_PREFIX;
    const char *name;
    size_t length;

    key[0] = '\0';
    if (!native_path || !mode || strcmp(mode, "rb") != 0)
        return 0;
    if (strncmp(native_path, prefix, sizeof prefix - 1U) != 0)
        return 0;
    name = native_path + sizeof prefix - 1U;
    length = strlen(name);
    if (length <= 2U || length >= ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY ||
        strchr(name, '/') || strchr(name, '\\') ||
        name[length - 2U] != '.' || name[length - 1U] != 'a')
        return 0;
    memcpy(key, name, length + 1U);
    return 1;
}

int isaac_vita_archive_raw_fallback_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY])
{
    return archive_packed_key(native_path, mode, key);
}

int isaac_vita_archive_cache_key(
    const char *native_path, const char *mode,
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY])
{
    if (!archive_packed_key(native_path, mode, key))
        return 0;
    if (strcmp(key, "music.a") == 0) {
        key[0] = '\0';
        return 0;
    }
    return 1;
}

static int archive_cache_drop_idle(void)
{
    FILE *stream = s_idle.stream;

    s_idle.stream = NULL;
    s_idle.key[0] = '\0';
    if (!stream)
        return 0;
    return archive_native_fclose(stream);
}

/* Eviction is not part of the fopen being requested.  In particular, a stale
 * idle close error must not become the errno reported for a later open. */
static void archive_cache_evict_before_open(void)
{
    int saved_errno = errno;

    (void)archive_cache_drop_idle();
    errno = saved_errno;
}

FILE *isaac_vita_archive_cache_open(isaac_vita_archive_cache_file *file,
                                    const char *native_path,
                                    const char *mode,
                                    int *cache_hit)
{
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY];
    FILE *stream;
    int cacheable;

    if (cache_hit)
        *cache_hit = 0;
    if (!file) {
        errno = EINVAL;
        return NULL;
    }
    if (file->stream) {
        errno = EBUSY;
        return NULL;
    }
    archive_cache_file_clear(file);
    cacheable = isaac_vita_archive_cache_key(native_path, mode, key);

    if (cacheable && s_idle.stream && strcmp(s_idle.key, key) == 0) {
        isaac_vita_archive_diag_event event;
        long reset_position = -1L;
        int reset_result;
        int reset_errno;

        stream = s_idle.stream;
        s_idle.stream = NULL;
        s_idle.key[0] = '\0';
        errno = 0;
        reset_result = archive_native_fseek(stream, 0L, SEEK_SET);
        reset_errno = errno;
        if (reset_result == 0) {
            errno = 0;
            reset_position = archive_native_ftell(stream);
            if (reset_position < 0L && !reset_errno)
                reset_errno = errno ? errno : EIO;
        }
        memset(&event, 0, sizeof event);
        event.kind = ISAAC_VITA_ARCHIVE_DIAG_CACHE_RESET;
        event.flags = ISAAC_VITA_ARCHIVE_DIAG_CACHE_HIT;
        if (reset_position < 0L)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_FTELL_FAILED;
        event.requested_offset = 0;
        event.origin = SEEK_SET;
        event.operation_result = reset_result;
        event.position_before = -1;
        event.position_after = (int32_t)reset_position;
        event.operation_errno = reset_errno;
        memcpy(event.key, key, strlen(key) + 1U);
        (void)isaac_vita_archive_diag_record(&event);
        if (reset_result == 0 && reset_position == 0L) {
            archive_native_clearerr(stream);
            file->stream = stream;
            memcpy(file->key, key, strlen(key) + 1U);
            if (cache_hit)
                *cache_hit = 1;
            return stream;
        }
        isaac_vita_log(
            "archive cursor diag: version=1 anomaly=cache-reset "
            "seq=%u key=%s token=0x00000000 requested=0x00000000 "
            "origin=%d result=%d actual=0x%08x errno=%d",
            (unsigned)event.sequence, event.key, event.origin,
            event.operation_result, (unsigned)event.position_after,
            event.operation_errno);
        /* A failed reset makes this descriptor unfit for reuse.  Discard it
         * and execute an ordinary fopen rather than returning a bad cursor.
         * A successful fseek with a mismatching ftell is equally unfit: the
         * exact hardware failure under diagnosis returned archive-index bytes
         * after a data-offset seek. */
        {
            int saved_errno = reset_errno ? reset_errno : EIO;
            (void)archive_native_fclose(stream);
            errno = saved_errno;
        }
    } else if (s_idle.stream) {
        /* This includes non-cacheable opens.  The idle descriptor is gone
         * before native fopen, so the cache can never cause a +2 FILE peak. */
        archive_cache_evict_before_open();
    }

    stream = archive_native_fopen(native_path, mode);
    if (!stream)
        return NULL;
    file->stream = stream;
    if (cacheable)
        memcpy(file->key, key, strlen(key) + 1U);
    return stream;
}

int isaac_vita_archive_cache_close(isaac_vita_archive_cache_file *file)
{
    FILE *stream;

    if (!file || !file->stream) {
        errno = EBADF;
        return EOF;
    }
    stream = file->stream;
    file->stream = NULL;
    if (!file->key[0])
        return archive_native_fclose(stream);

    if (s_idle.stream) {
        int saved_errno = errno;
        (void)archive_cache_drop_idle();
        errno = saved_errno;
    }
    s_idle.stream = stream;
    memcpy(s_idle.key, file->key, strlen(file->key) + 1U);
    file->key[0] = '\0';
    return 0;
}

int isaac_vita_archive_cache_force_discard(
    isaac_vita_archive_cache_file *file)
{
    FILE *stream;

    if (!file || !file->stream) {
        errno = EBADF;
        return EOF;
    }
    stream = file->stream;
    archive_cache_file_clear(file);
    return archive_native_fclose(stream);
}

int isaac_vita_archive_cache_drop(void)
{
    return archive_cache_drop_idle();
}
