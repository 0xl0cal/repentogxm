/* Vita implementations of the measured CRT startup batch.
 *
 * Most handlers operate only on the emulated x86 CPU and identity-mapped
 * 32-bit guest storage.  The time family additionally uses SceRtc for the
 * console's configured UTC-to-local conversion; all UTC calendar arithmetic
 * remains 64-bit here because Vita newlib's time_t is only 32-bit.
 */
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/rtc.h>
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE) && \
    !defined(ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
#include <sys/stat.h>
#endif

#include "host_vita_crt.h"
#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT) && ISAAC_VITA_CRT_ATOF_SMALLINT
#include "host_vita_atof_smallint.h"
#endif
#include "host_vita_import_id.h"
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
#if !defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#error ISAAC_VITA_ASYNC_SAVE_WRITE requires the ARCHIVE_FILE_CACHE token layout
#endif
#include "host_vita_async_write.h"
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#include "host_vita_archive_cache.h"
#ifndef ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID
#define ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID "link:unstamped"
#endif
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
#if !defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#error ISAAC_VITA_CRT_SEEK_SHADOW requires the ARCHIVE_FILE_CACHE token layout
#endif
/* The host oracle counts the real newlib-lane seek family through these
 * three seams; production resolves them to libc without indirection. */
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW_ORACLE)
int isaac_vita_crt_seek_shadow_oracle_fseek(FILE *stream, long offset,
                                            int origin);
long isaac_vita_crt_seek_shadow_oracle_ftell(FILE *stream);
int isaac_vita_crt_seek_shadow_oracle_fstat(int descriptor,
                                            struct stat *status);
#define vita_crt_shadow_native_fseek isaac_vita_crt_seek_shadow_oracle_fseek
#define vita_crt_shadow_native_ftell isaac_vita_crt_seek_shadow_oracle_ftell
#define vita_crt_shadow_native_fstat isaac_vita_crt_seek_shadow_oracle_fstat
#else
#define vita_crt_shadow_native_fseek fseek
#define vita_crt_shadow_native_ftell ftell
#define vita_crt_shadow_native_fstat fstat
#endif
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
#if !defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#error ISAAC_VITA_CRT_DESCRIPTOR_RECOVER requires the ARCHIVE_FILE_CACHE token layout
#endif
/* The fresh handle is positioned through the seek shadow's counted seams
 * when that oracle is linked, so its pinned syscall counts see the one real
 * seek; production resolves both to libc. */
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
#define vita_crt_recover_native_fseek vita_crt_shadow_native_fseek
#define vita_crt_recover_native_ftell vita_crt_shadow_native_ftell
#else
#define vita_crt_recover_native_fseek fseek
#define vita_crt_recover_native_ftell ftell
#endif
#endif
#include "host_vita_heap.h"
#include "host_vita_startup.h"
#include "third_party/musl_fmt_fp/musl_fmt_fp.h"
#if defined(ISAAC_VITA_EXIT_MENU_PROFILE)
#include "kage_vita_exit_menu_profile.h"
#endif
#include "platform.h"
#include "vita_host_services.h"
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
#include "kage_vita_phase_profile.h"
#endif

#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#ifndef ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX
#define ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX LONG_MAX
#endif
#if defined(ISAAC_VITA_CRT_RAW_ARCHIVE_ORACLE)
int32_t isaac_vita_crt_raw_archive_oracle_open(const char *path);
int32_t isaac_vita_crt_raw_archive_oracle_pread(
    int32_t descriptor, void *buffer, uint32_t size, uint64_t offset);
int32_t isaac_vita_crt_raw_archive_oracle_get_size(
    int32_t descriptor, int64_t *size);
int32_t isaac_vita_crt_raw_archive_oracle_close(int32_t descriptor);
#define vita_crt_raw_sce_open(path) \
    isaac_vita_crt_raw_archive_oracle_open(path)
#define vita_crt_raw_sce_pread(descriptor, buffer, size, offset) \
    isaac_vita_crt_raw_archive_oracle_pread(                     \
        (descriptor), (buffer), (size), (offset))
#define vita_crt_raw_sce_get_size(descriptor, size) \
    isaac_vita_crt_raw_archive_oracle_get_size((descriptor), (size))
#define vita_crt_raw_sce_close(descriptor) \
    isaac_vita_crt_raw_archive_oracle_close(descriptor)
#else
#define vita_crt_raw_sce_open(path) sceIoOpen((path), SCE_O_RDONLY, 0)
#define vita_crt_raw_sce_pread(descriptor, buffer, size, offset) \
    sceIoPread((descriptor), (buffer), (size), (SceOff)(offset))
#define vita_crt_raw_sce_close(descriptor) sceIoClose(descriptor)

static int vita_crt_raw_sce_get_size(int32_t descriptor, int64_t *size)
{
    SceIoStat status;
    int result;

    memset(&status, 0, sizeof status);
    result = sceIoGetstatByFd(descriptor, &status);
    if (result < 0)
        return result;
    if (status.st_size < 0)
        return -1;
    *size = (int64_t)status.st_size;
    return 0;
}
#endif
#endif

#if defined(ISAAC_VITA_CRT_FREAD_ORACLE)
size_t isaac_vita_crt_oracle_fread(void *buffer, size_t size,
                                   size_t count, FILE *stream);
#define vita_crt_native_fread isaac_vita_crt_oracle_fread
#else
#define vita_crt_native_fread fread
#endif

typedef void (*vita_crt_import_fn)(CPU *__restrict);

typedef struct vita_crt_import_entry {
    const char *name;
    vita_crt_import_fn fn;
} vita_crt_import_entry;

isaac_vita_crt_state g_isaac_vita_crt = {
    0U, 0U, 0U, 0U,
    0x00010000U,                 /* _PC_53, copied from host_win32.c */
    0, 0U
};
int32_t g_isaac_vita_crt_errno;
static uint32_t s_time_tm[ISAAC_VITA_CRT_TM_DWORD_COUNT];

/* Kept externally observable for the later exit boundary, exactly as the
 * count is.  A write-only TU-local table is dead-store-eliminated before that
 * consumer exists, which would make today's successful registration a lie. */
uint32_t g_isaac_vita_crt_atexit[ISAAC_VITA_CRT_ATEXIT_MAX];
static volatile uint32_t s_atexit_lock;
static volatile uint32_t s_exit_running;
static uint32_t s_tls_atexit;
static uint32_t s_tls_atexit_registered;
static uint32_t s_empty_environment[1];
static uint32_t s_empty_environment_pointer;
static char s_argv0[] = "isaac-ng.exe";
static uint32_t s_argv[2];
static uint32_t s_argv_pointer;
static int32_t s_argc = 1;
static int s_environment_ready;
static char s_user_profile[] = ISAAC_VITA_DATA_ROOT;

#if defined(ISAAC_VITA_IO_PROFILE)
enum {
    VITA_CRT_IO_SHADOW_SEEK_BLOCK_SIZE = 1024U
};

typedef struct vita_crt_io_shadow_file {
    uint64_t position;
    uint64_t buffer_begin[ISAAC_VITA_CRT_IO_SHADOW_VARIANT_COUNT];
    uint64_t buffer_end[ISAAC_VITA_CRT_IO_SHADOW_VARIANT_COUNT];
    uint32_t buffer_valid_mask;
    uint32_t selected;
    uint32_t model_valid;
    uint32_t seek_optimized;
} vita_crt_io_shadow_file;
#endif

typedef struct vita_crt_file_token {
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    isaac_vita_archive_cache_file file;
    struct {
        int64_t position;
        int64_t size;
        int32_t descriptor;
        uint8_t live;
        uint8_t eof;
        uint8_t error;
        uint8_t reserved;
    } raw_archive;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* A "wb" *.dat save below Documents held as a host memory image until
     * fclose hands it to the writer thread.  file.stream stays NULL. */
    isaac_vita_async_write_image async_write;
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    /* Read-only newlib-lane SEEK_END elision.  pending_end means the FILE's
     * real cursor still sits where the guest last left it while the logical
     * cursor is size + pending_offset; every consumer other than ftell and
     * fseek applies the deferred real seek first. */
    struct {
        int64_t size;
        int32_t pending_offset;
        uint32_t write_gen;
        uint8_t read_only;
        uint8_t size_valid;
        uint8_t pending_end;
        uint8_t reserved;
        char name[ISAAC_VITA_CRT_SEEK_SHADOW_NAME_CAPACITY];
    } seek_shadow;
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    /* Read-only tokens retain the resolved native path and mode: a
     * descriptor SceIofilemgr invalidated (ENODEV after suspend/resume) is
     * reopened there at the logical cursor.  path[0] == 0 means the token is
     * not recoverable (write side, or a path over the bound). */
    struct {
        char path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
        char mode[4];
    } recover;
#endif
#else
    FILE *stream;
#endif
    uint32_t token;
#if defined(ISAAC_VITA_IO_PROFILE) && \
    defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    vita_crt_io_shadow_file io_shadow;
#endif
} vita_crt_file_token;

static vita_crt_file_token s_file_tokens[ISAAC_VITA_CRT_FILE_TOKEN_COUNT];
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
/* All access is under s_file_lock.  This stores no FILE pointer.  Invalidate
 * at both token-publication sites: the remembered linear-search first match
 * cannot then be superseded by a newly published, lower-index duplicate. */
static uint32_t s_file_lookup_hint = ISAAC_VITA_CRT_FILE_TOKEN_COUNT;
static uint32_t s_file_lookup_hits;
static uint32_t s_file_lookup_misses;
#endif

static FILE *vita_crt_dynamic_stream(const vita_crt_file_token *entry)
{
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    return entry->file.stream;
#else
    return entry->stream;
#endif
}

static int vita_crt_dynamic_file_is_live(const vita_crt_file_token *entry)
{
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    return entry->file.stream != NULL || entry->raw_archive.live != 0U
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
        || entry->async_write.live != 0U
#endif
        ;
#else
    return entry->stream != NULL;
#endif
}

#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
/* SceIofilemgr returns encoded 0x800100XX errno-family values directly. */
static int vita_crt_raw_archive_errno(int32_t result)
{
    uint32_t value = (uint32_t)result;
    uint32_t low = value & UINT32_C(0xff);

    if ((value & UINT32_C(0xffffff00)) == UINT32_C(0x80010000) && low)
        return (int)low;
    return EIO;
}

static void vita_crt_raw_archive_clear(vita_crt_file_token *entry)
{
    entry->raw_archive.position = 0;
    entry->raw_archive.size = 0;
    entry->raw_archive.descriptor = -1;
    entry->raw_archive.live = 0U;
    entry->raw_archive.eof = 0U;
    entry->raw_archive.error = 0U;
    entry->raw_archive.reserved = 0U;
}

static int vita_crt_raw_archive_try_open(vita_crt_file_token *entry,
                                         const char *native_path,
                                         const char *mode,
                                         int fopen_errno)
{
    int32_t descriptor;
    int32_t size_result;
    int close_result;
    int64_t size = 0;

    vita_crt_raw_archive_clear(entry);
    if (fopen_errno != ENOMEM ||
        !isaac_vita_archive_raw_fallback_key(
            native_path, mode, entry->file.key)) {
        errno = fopen_errno;
        return 0;
    }
    descriptor = vita_crt_raw_sce_open(native_path);
    if (descriptor < 0) {
        errno = vita_crt_raw_archive_errno(descriptor);
        entry->file.key[0] = '\0';
        return 0;
    }
    size_result = vita_crt_raw_sce_get_size(descriptor, &size);
    if (size_result < 0 || size < 0) {
        int open_error = size_result < 0
            ? vita_crt_raw_archive_errno(size_result) : EIO;

        close_result = vita_crt_raw_sce_close(descriptor);
        (void)close_result;
        errno = open_error;
        entry->file.key[0] = '\0';
        return 0;
    }
    entry->raw_archive.size = size;
    entry->raw_archive.descriptor = descriptor;
    entry->raw_archive.live = 1U;
    errno = 0;
    return 1;
}

static int vita_crt_raw_archive_close(vita_crt_file_token *entry)
{
    int32_t descriptor;
    int result;

    if (!entry->raw_archive.live) {
        errno = EBADF;
        return EOF;
    }
    descriptor = entry->raw_archive.descriptor;
    vita_crt_raw_archive_clear(entry);
    entry->file.key[0] = '\0';
    result = vita_crt_raw_sce_close(descriptor);
    if (result < 0) {
        errno = vita_crt_raw_archive_errno(result);
        return EOF;
    }
    return 0;
}

static size_t vita_crt_raw_archive_fread(vita_crt_file_token *entry,
                                         void *buffer, size_t size,
                                         size_t count)
{
    unsigned char *output = (unsigned char *)buffer;
    uint32_t requested;
    uint32_t complete = 0U;

    if (!entry->raw_archive.live) {
        errno = EBADF;
        return 0U;
    }
    if (!size || !count)
        return 0U;
    if (count > (size_t)UINT32_MAX / size ||
        entry->raw_archive.position < 0 ||
        (uint64_t)entry->raw_archive.position >
            (uint64_t)INT64_MAX - (uint64_t)(size * count)) {
        entry->raw_archive.error = 1U;
        errno = EOVERFLOW;
        return 0U;
    }
    /* Match newlib __srefill_r: sticky EOF does not issue another read.
     * A successful raw fseek already clears EOF (but not the error flag).
     * Retain the validation/zero-size ordering above and leave errno alone. */
    if (entry->raw_archive.eof)
        return 0U;
    requested = (uint32_t)(size * count);
    while (complete < requested) {
        uint32_t remaining = requested - complete;
        uint32_t chunk = remaining > (uint32_t)INT_MAX
            ? (uint32_t)INT_MAX : remaining;
        int result = vita_crt_raw_sce_pread(
            entry->raw_archive.descriptor, output + complete,
            chunk,
            (uint64_t)entry->raw_archive.position + complete);

        if (result < 0) {
            entry->raw_archive.error = 1U;
            errno = vita_crt_raw_archive_errno(result);
            break;
        }
        if (result == 0) {
            entry->raw_archive.eof = 1U;
            break;
        }
        if ((uint32_t)result > chunk) {
            entry->raw_archive.error = 1U;
            errno = EIO;
            break;
        }
        complete += (uint32_t)result;
    }
    entry->raw_archive.position += (int64_t)complete;
    return (size_t)complete / size;
}

static size_t vita_crt_file_fwrite(const void *buffer, size_t size,
                                   size_t count, FILE *stream,
                                   int dynamic_index)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return isaac_vita_async_write_image_write(
            &s_file_tokens[dynamic_index].async_write, buffer, size, count);
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live) {
        (void)buffer;
        if (!size || !count)
            return 0U;
        s_file_tokens[dynamic_index].raw_archive.error = 1U;
        errno = EBADF;
        return 0U;
    }
    return fwrite(buffer, size, count, stream);
}

static int vita_crt_raw_archive_fseek(vita_crt_file_token *entry,
                                      long offset, int origin)
{
    int64_t base;
    int64_t delta = (int64_t)offset;
    int64_t target;

    if (!entry->raw_archive.live) {
        errno = EBADF;
        return -1;
    }
    if (origin == SEEK_SET) {
        base = 0;
    } else if (origin == SEEK_CUR) {
        base = entry->raw_archive.position;
    } else if (origin == SEEK_END) {
        base = entry->raw_archive.size;
    } else {
        errno = EINVAL;
        return -1;
    }
    if (delta < 0) {
        uint64_t magnitude = (uint64_t)(-(delta + 1)) + 1U;
        if ((uint64_t)base < magnitude) {
            errno = EINVAL;
            return -1;
        }
        target = base - (int64_t)magnitude;
    } else {
        if (base > INT64_MAX - delta) {
            errno = EOVERFLOW;
            return -1;
        }
        target = base + delta;
    }
    entry->raw_archive.position = target;
    entry->raw_archive.eof = 0U;
    return 0;
}

static long vita_crt_raw_archive_ftell(vita_crt_file_token *entry)
{
    if (!entry->raw_archive.live) {
        errno = EBADF;
        return -1L;
    }
    if ((uint64_t)entry->raw_archive.position >
        (uint64_t)ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX) {
        errno = EOVERFLOW;
        return -1L;
    }
    return (long)entry->raw_archive.position;
}

#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
/* File::GetSize (guest sub_005964a0) is `ftell; fseek(0, SEEK_END); ftell;
 * fseek(pos, SEEK_SET)` and File::IsEOF is `Tell() >= GetSize()`.  On Vita
 * newlib every SEEK_END costs sceIoGetstatByFd + sceIoLseek32 + one 16 KiB
 * tail refill, and the back-seek then re-reads the block it discarded: about
 * 3 ms per GetSize, 5.65 s of one measured 9.9 s floor load.  A read-only
 * stream cannot change its own size, so the SEEK_END is recorded instead of
 * performed: ftell answers size + offset from a cached fstat, the following
 * SEEK_SET/SEEK_CUR is issued for real against the still-live buffer (newlib
 * pointer-fixes an in-buffer target without a syscall) and fread, fwrite,
 * fflush and vfprintf apply the deferred seek first, so bytes, cursors, EOF
 * and error flags are exactly newlib's.  Every write-side operation (a
 * non-read-only fopen/fseek/ftell/fread/fwrite/fflush/vfprintf/fclose, or
 * fflush(NULL)) bumps one generation so the next SEEK_END re-fstats.  Raw
 * SceIo and async-image tokens already seek arithmetically and never enter
 * this path. */
enum {
    VITA_CRT_SEEK_SHADOW_TOP = 8U,
    VITA_CRT_SEEK_SHADOW_REPORT_PERIOD = 512U
};

typedef struct vita_crt_seek_shadow_stats {
    uint32_t seek_end;       /* SEEK_END requests on eligible tokens */
    uint32_t elided;         /* ... answered without a newlib fseek */
    uint32_t real_end;       /* ... handed to newlib (fstat/range) */
    uint32_t fstat_calls;
    uint32_t fstat_fail;
    uint32_t restat;         /* fstat repeated after a write generation */
    uint32_t applied;        /* deferred seeks performed before another op */
    uint32_t apply_fail;
    uint32_t ftell_pending;  /* ftell answered from the shadow */
    uint32_t set_pending;    /* SEEK_SET issued while a seek was pending */
    uint32_t cur_pending;    /* SEEK_CUR rewritten to SEEK_SET */
    uint32_t rearmed;        /* real seek failed; logical cursor kept */
    uint32_t other_end;      /* SEEK_END on write/raw/async/standard */
    uint32_t reports;
    struct {
        uint32_t key;
        uint32_t count;
    } top_ret[VITA_CRT_SEEK_SHADOW_TOP];
    struct {
        char name[ISAAC_VITA_CRT_SEEK_SHADOW_NAME_CAPACITY];
        uint32_t count;
    } top_name[VITA_CRT_SEEK_SHADOW_TOP];
} vita_crt_seek_shadow_stats;

static vita_crt_seek_shadow_stats s_seek_shadow;
static uint32_t s_seek_shadow_write_gen;
static uint32_t s_seek_shadow_since_report;

static int vita_crt_seek_shadow_mode_read_only(const char *mode)
{
    return mode[0] == 'r' && strchr(mode, '+') == NULL;
}

/* Any write-side activity invalidates every cached size; a reader that
 * repeats SEEK_END afterwards pays exactly newlib's fstat once more. */
static void vita_crt_seek_shadow_note_write(void)
{
    ++s_seek_shadow_write_gen;
}

/* newlib also flushes a write or update stream's buffer inside fseek (any
 * origin), ftell and the fread that switches an update stream to input, so
 * a cursor operation on anything but a read-only token is write-side
 * activity for the cached sizes. */
static void vita_crt_seek_shadow_note_cursor_op(int dynamic_index)
{
    if (dynamic_index < 0 ||
        !s_file_tokens[dynamic_index].seek_shadow.read_only)
        vita_crt_seek_shadow_note_write();
}

static int vita_crt_seek_shadow_eligible(const vita_crt_file_token *entry,
                                         FILE *stream)
{
    return stream != NULL && entry->seek_shadow.read_only &&
        !entry->raw_archive.live
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
        && !entry->async_write.live
#endif
        ;
}

static void vita_crt_seek_shadow_clear(vita_crt_file_token *entry)
{
    memset(&entry->seek_shadow, 0, sizeof entry->seek_shadow);
}

static void vita_crt_seek_shadow_publish(vita_crt_file_token *entry,
                                         const char *mode,
                                         const char *native_path)
{
    const char *base = strrchr(native_path, '/');
    size_t length;

    vita_crt_seek_shadow_clear(entry);
    entry->seek_shadow.read_only =
        vita_crt_seek_shadow_mode_read_only(mode) ? 1U : 0U;
    base = base ? base + 1 : native_path;
    length = strlen(base);
    if (length >= sizeof entry->seek_shadow.name)
        length = sizeof entry->seek_shadow.name - 1U;
    memcpy(entry->seek_shadow.name, base, length);
    entry->seek_shadow.name[length] = '\0';
}

/* Caller owns s_file_lock.  Make the FILE's real cursor equal its logical
 * cursor before any operation that consumes or moves it. */
static void vita_crt_seek_shadow_apply(FILE *stream, int dynamic_index)
{
    vita_crt_file_token *entry;
    int saved_errno;

    if (dynamic_index < 0 || !stream)
        return;
    entry = &s_file_tokens[dynamic_index];
    if (!entry->seek_shadow.pending_end)
        return;
    saved_errno = errno;
    entry->seek_shadow.pending_end = 0U;
    ++s_seek_shadow.applied;
    if (vita_crt_shadow_native_fseek(
            stream, (long)entry->seek_shadow.pending_offset, SEEK_END) != 0)
        ++s_seek_shadow.apply_fail;
    errno = saved_errno;
}

static int vita_crt_seek_shadow_fseek(FILE *stream, int dynamic_index,
                                      long offset, int origin)
{
    vita_crt_file_token *entry;
    int result;

    if (dynamic_index < 0 ||
        !vita_crt_seek_shadow_eligible(&s_file_tokens[dynamic_index],
                                       stream)) {
        if (origin == SEEK_END)
            ++s_seek_shadow.other_end;
        vita_crt_seek_shadow_note_cursor_op(dynamic_index);
        return vita_crt_shadow_native_fseek(stream, offset, origin);
    }
    entry = &s_file_tokens[dynamic_index];
    if (origin == SEEK_END) {
        int64_t target;

        ++s_seek_shadow.seek_end;
        if (!entry->seek_shadow.size_valid ||
            entry->seek_shadow.write_gen != s_seek_shadow_write_gen) {
            struct stat status;
            int descriptor = fileno(stream);

            if (entry->seek_shadow.size_valid)
                ++s_seek_shadow.restat;
            ++s_seek_shadow.fstat_calls;
            memset(&status, 0, sizeof status);
            if (descriptor < 0 ||
                vita_crt_shadow_native_fstat(descriptor, &status) != 0 ||
                !S_ISREG(status.st_mode) || status.st_size < 0) {
                ++s_seek_shadow.fstat_fail;
                ++s_seek_shadow.real_end;
                entry->seek_shadow.size_valid = 0U;
                result = vita_crt_shadow_native_fseek(stream, offset, origin);
                if (result == 0)
                    entry->seek_shadow.pending_end = 0U;
                return result;
            }
            entry->seek_shadow.size = (int64_t)status.st_size;
            entry->seek_shadow.size_valid = 1U;
            entry->seek_shadow.write_gen = s_seek_shadow_write_gen;
        }
        target = entry->seek_shadow.size + (int64_t)offset;
        if (target < 0 ||
            target > (int64_t)ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX) {
            /* newlib reports EINVAL/EOVERFLOW here and leaves the cursor. */
            ++s_seek_shadow.real_end;
            result = vita_crt_shadow_native_fseek(stream, offset, origin);
            if (result == 0)
                entry->seek_shadow.pending_end = 0U;
            return result;
        }
        entry->seek_shadow.pending_end = 1U;
        entry->seek_shadow.pending_offset = (int32_t)offset;
        ++s_seek_shadow.elided;
        return 0;
    }
    if (!entry->seek_shadow.pending_end)
        return vita_crt_shadow_native_fseek(stream, offset, origin);
    if (origin == SEEK_SET) {
        ++s_seek_shadow.set_pending;
    } else if (origin == SEEK_CUR) {
        int64_t target = entry->seek_shadow.size +
            (int64_t)entry->seek_shadow.pending_offset + (int64_t)offset;

        ++s_seek_shadow.cur_pending;
        if (target > (int64_t)ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        if (target < 0) {
            errno = EINVAL;
            return -1;
        }
        offset = (long)target;
        origin = SEEK_SET;
    } else {
        /* Invalid origin: newlib fails with EINVAL and moves nothing. */
        return vita_crt_shadow_native_fseek(stream, offset, origin);
    }
    entry->seek_shadow.pending_end = 0U;
    result = vita_crt_shadow_native_fseek(stream, offset, origin);
    if (result != 0) {
        /* A failed seek leaves the logical cursor at the deferred end. */
        entry->seek_shadow.pending_end = 1U;
        ++s_seek_shadow.rearmed;
    }
    return result;
}

static long vita_crt_seek_shadow_ftell(FILE *stream, int dynamic_index)
{
    if (dynamic_index >= 0) {
        vita_crt_file_token *entry = &s_file_tokens[dynamic_index];

        if (entry->seek_shadow.pending_end &&
            vita_crt_seek_shadow_eligible(entry, stream)) {
            ++s_seek_shadow.ftell_pending;
            return (long)(entry->seek_shadow.size +
                          (int64_t)entry->seek_shadow.pending_offset);
        }
    }
    vita_crt_seek_shadow_note_cursor_op(dynamic_index);
    return vita_crt_shadow_native_ftell(stream);
}
#endif

static int vita_crt_file_fseek(FILE *stream, int dynamic_index,
                               long offset, int origin)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return isaac_vita_async_write_image_seek(
            &s_file_tokens[dynamic_index].async_write, offset, origin);
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live)
        return vita_crt_raw_archive_fseek(
            &s_file_tokens[dynamic_index], offset, origin);
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    return vita_crt_seek_shadow_fseek(stream, dynamic_index, offset, origin);
#else
    return fseek(stream, offset, origin);
#endif
}

static long vita_crt_file_ftell(FILE *stream, int dynamic_index)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return isaac_vita_async_write_image_tell(
            &s_file_tokens[dynamic_index].async_write);
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live)
        return vita_crt_raw_archive_ftell(&s_file_tokens[dynamic_index]);
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    return vita_crt_seek_shadow_ftell(stream, dynamic_index);
#else
    return ftell(stream);
#endif
}

static int vita_crt_file_fileno(FILE *stream, int dynamic_index)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* The frozen File wrapper converts every FILE through _fileno and
     * _get_osfhandle before LockFileEx.  A memory image has no newlib
     * descriptor, so publish a slot-derived pseudo descriptor far above the
     * newlib fd range; only the two lock bridges ever consume it and neither
     * passes it to native I/O. */
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return (int)(ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE +
                     (uint32_t)dynamic_index);
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live) {
        /* A SceUID is not a newlib fd-map index.  Do not publish a handle
         * which later newlib descriptor calls would misinterpret. */
        errno = EBADF;
        return -1;
    }
    return fileno(stream);
}

static int vita_crt_file_fflush(FILE *stream, int dynamic_index)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* The image reaches disk at fclose; an explicit flush has nothing to
     * do and newlib would also report success for a fully buffered stream. */
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return 0;
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live)
        return 0;
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    vita_crt_seek_shadow_apply(stream, dynamic_index);
#endif
    return fflush(stream);
}

static int vita_crt_file_feof(FILE *stream, int dynamic_index)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return 0;
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live)
        return s_file_tokens[dynamic_index].raw_archive.eof != 0U;
    return feof(stream);
}

static int vita_crt_file_ferror(FILE *stream, int dynamic_index)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live)
        return s_file_tokens[dynamic_index].async_write.error != 0U;
#endif
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live)
        return s_file_tokens[dynamic_index].raw_archive.error != 0U;
    return ferror(stream);
}
#endif

typedef struct vita_crt_format_output {
    uint32_t buffer;
    uint32_t limit;
    uint32_t written;
    FILE *stream;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    isaac_vita_async_write_image *async_image;
#endif
    uint8_t *stream_buffer;
    uint32_t stream_capacity;
    uint32_t stream_used;
    int stream_failed;
} vita_crt_format_output;

enum {
    VITA_CRT_VFPRINTF_CHUNK_BYTES = 512U
};

/* The executable host oracle uses this seam to model the only state change
 * which can occur between the faulting validation pass and FILE-token
 * revalidation.  It compiles to nothing in production. */
#ifndef ISAAC_VITA_CRT_VFPRINTF_AFTER_FIRST_PASS
#define ISAAC_VITA_CRT_VFPRINTF_AFTER_FIRST_PASS(                    \
    cpu, token, format, arguments, length)                            \
    ((void)(cpu), (void)(token), (void)(format), (void)(arguments),  \
     (void)(length))
#endif

static volatile uint32_t s_file_lock;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
static isaac_vita_crt_log_batch_snapshot s_log_batch = {
    ISAAC_VITA_CRT_LOG_BATCH_ABI, 1U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
};
static uint32_t s_log_batch_token;
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
static uint32_t s_archive_diag_logged_first_relevant;
static uint32_t s_archive_diag_logged_first_direct;
#endif

#if defined(ISAAC_VITA_IO_PROFILE)
static isaac_vita_crt_io_profile_snapshot s_io_profile;
static uint32_t s_io_profile_active;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
static vita_crt_io_shadow_file s_io_shadow_idle;
#endif

static void vita_crt_profile_add(uint32_t *value, uint32_t addend)
{
    if (*value > UINT32_MAX - addend)
        *value = UINT32_MAX;
    else
        *value += addend;
}

static void vita_crt_profile_add_product(uint32_t *value,
                                         uint32_t left, uint32_t right)
{
    uint64_t product = (uint64_t)left * (uint64_t)right;

    vita_crt_profile_add(
        value, product > UINT32_MAX ? UINT32_MAX : (uint32_t)product);
}

#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
static const uint32_t s_vita_crt_io_shadow_buffer_sizes[
    ISAAC_VITA_CRT_IO_SHADOW_VARIANT_COUNT] = {
        8U * 1024U, 16U * 1024U, 32U * 1024U, 64U * 1024U
    };

static void vita_crt_io_shadow_clear(vita_crt_io_shadow_file *file)
{
    memset(file, 0, sizeof *file);
}

static void vita_crt_io_shadow_invalidate_buffers(
    vita_crt_io_shadow_file *file)
{
    file->buffer_valid_mask = 0U;
}

static void vita_crt_io_shadow_refill(vita_crt_io_shadow_file *file,
                                      uint32_t variant, uint64_t begin)
{
    uint32_t size = s_vita_crt_io_shadow_buffer_sizes[variant];

    vita_crt_profile_add(&s_io_profile.shadow_read_calls[variant], 1U);
    vita_crt_profile_add(
        &s_io_profile.shadow_read_requested_bytes[variant], size);
    file->buffer_begin[variant] = begin;
    file->buffer_end[variant] = begin + size;
    file->buffer_valid_mask |= 1U << variant;
}

static void vita_crt_io_shadow_invalidate(vita_crt_io_shadow_file *file)
{
    file->model_valid = 0U;
    vita_crt_io_shadow_invalidate_buffers(file);
}

static void vita_crt_io_shadow_select_unknown(
    vita_crt_io_shadow_file *file)
{
    if (!file->selected) {
        vita_crt_io_shadow_clear(file);
        file->selected = 1U;
    }
    vita_crt_io_shadow_invalidate(file);
}

/* Vita newlib's installed _fseeko_r aligns an out-of-buffer SEEK_SET target
 * to a hard-coded 1 KiB block.  A non-aligned target immediately refills the
 * FILE buffer; an aligned target leaves it empty until the next fread.  The
 * first seek after our 16 KiB setvbuf is deliberately different: setvbuf set
 * __SNPT because st_blksize is 1 KiB, so it performs one exact seek without a
 * refill and only then enables this optimization. */
static void vita_crt_io_shadow_seek_target(vita_crt_io_shadow_file *file,
                                           uint64_t target)
{
    uint64_t aligned = target &
        ~((uint64_t)VITA_CRT_IO_SHADOW_SEEK_BLOCK_SIZE - 1U);
    uint32_t variant;

    /* setvbuf marks the stream __SNPT because every tested lane differs from
     * Vita newlib's 1 KiB st_blksize.  The first successful SEEK_SET is a
     * direct exact seek with no refill; it clears __SNPT for later seeks. */
    if (!file->seek_optimized) {
        vita_crt_io_shadow_invalidate_buffers(file);
        file->position = target;
        file->seek_optimized = 1U;
        return;
    }
    for (variant = 0U;
         variant < ISAAC_VITA_CRT_IO_SHADOW_VARIANT_COUNT;
         ++variant) {
        uint32_t mask = 1U << variant;

        if ((file->buffer_valid_mask & mask) &&
            target >= file->buffer_begin[variant] &&
            target < file->buffer_end[variant])
            continue;
        file->buffer_valid_mask &= ~mask;
        if (target != aligned)
            vita_crt_io_shadow_refill(file, variant, aligned);
    }
    file->position = target;
}

static void vita_crt_io_shadow_open(vita_crt_io_shadow_file *file,
                                    const vita_crt_io_shadow_file *retained,
                                    int cache_hit, int setvbuf_succeeded)
{
    vita_crt_profile_add(&s_io_profile.shadow_cacheable_fopen_calls, 1U);
    vita_crt_io_shadow_clear(file);
    file->selected = 1U;
    if (cache_hit) {
        if (retained && retained->selected) {
            *file = *retained;
            file->selected = 1U;
            if (file->model_valid) {
                /* The cache already performed this successful rewind.  It is
                 * not a guest fseek and therefore has no origin counter. */
                vita_crt_io_shadow_seek_target(file, 0U);
            } else {
                vita_crt_profile_add(
                    &s_io_profile.shadow_invalid_cache_hits, 1U);
            }
        } else {
            /* profile_begin may have started after an idle FILE was created;
             * never pretend that its buffer and __SNPT state are fresh. */
            vita_crt_profile_add(
                &s_io_profile.shadow_invalid_cache_hits, 1U);
        }
    } else {
        file->model_valid = setvbuf_succeeded ? 1U : 0U;
        if (!setvbuf_succeeded)
            vita_crt_profile_add(
                &s_io_profile.shadow_setvbuf_failures, 1U);
    }
}

static void vita_crt_io_shadow_close(vita_crt_io_shadow_file *file)
{
    (void)file;
    vita_crt_profile_add(
        &s_io_profile.shadow_cacheable_fclose_calls, 1U);
}

static void vita_crt_io_shadow_note_seek_origin(int origin)
{
    if (origin == SEEK_SET) {
        vita_crt_profile_add(&s_io_profile.shadow_fseek_set_calls, 1U);
    } else if (origin == SEEK_CUR) {
        vita_crt_profile_add(&s_io_profile.shadow_fseek_cur_calls, 1U);
    } else if (origin == SEEK_END) {
        vita_crt_profile_add(&s_io_profile.shadow_fseek_end_calls, 1U);
    } else {
        vita_crt_profile_add(&s_io_profile.shadow_fseek_other_calls, 1U);
    }
}

static void vita_crt_io_shadow_seek_failed(vita_crt_io_shadow_file *file,
                                           int origin)
{
    vita_crt_io_shadow_note_seek_origin(origin);
    if (!file->selected)
        vita_crt_io_shadow_select_unknown(file);
    vita_crt_profile_add(&s_io_profile.shadow_unmodelled_seeks, 1U);
    vita_crt_io_shadow_invalidate(file);
}

static void vita_crt_io_shadow_seek(vita_crt_io_shadow_file *file,
                                    int32_t offset, int origin)
{
    vita_crt_io_shadow_note_seek_origin(origin);

    if (!file->selected)
        vita_crt_io_shadow_select_unknown(file);
    /* The replay is intentionally narrow.  SEEK_CUR first flushes unread
     * bytes and makes __SNPT/optimization state lane-dependent; SEEK_END needs
     * the archive length.  One such event permanently invalidates this FILE's
     * lanes rather than manufacturing a precise-looking estimate. */
    if (!file->model_valid || origin != SEEK_SET || offset < 0) {
        vita_crt_profile_add(&s_io_profile.shadow_unmodelled_seeks, 1U);
        vita_crt_io_shadow_invalidate(file);
        return;
    }
    vita_crt_io_shadow_seek_target(file, (uint32_t)offset);
}

static void vita_crt_io_shadow_read(vita_crt_io_shadow_file *file,
                                    uint32_t requested, uint32_t returned)
{
    uint32_t variant;

    if (!file->selected)
        vita_crt_io_shadow_select_unknown(file);
    vita_crt_profile_add(&s_io_profile.shadow_fread_calls, 1U);
    vita_crt_profile_add(
        &s_io_profile.shadow_fread_requested_bytes, requested);
    vita_crt_profile_add(
        &s_io_profile.shadow_fread_returned_bytes, returned);
    if (!file->model_valid || requested != returned) {
        vita_crt_profile_add(
            &s_io_profile.shadow_unmodelled_fread_calls, 1U);
        vita_crt_profile_add(
            &s_io_profile.shadow_unmodelled_fread_bytes, requested);
        if (requested != returned) {
            vita_crt_profile_add(
                &s_io_profile.shadow_partial_fread_calls, 1U);
            vita_crt_profile_add(
                &s_io_profile.shadow_partial_fread_requested_bytes,
                requested);
            vita_crt_profile_add(
                &s_io_profile.shadow_partial_fread_returned_bytes,
                returned);
        }
        vita_crt_io_shadow_invalidate(file);
        return;
    }
    if (!returned)
        return;
    for (variant = 0U;
         variant < ISAAC_VITA_CRT_IO_SHADOW_VARIANT_COUNT;
         ++variant) {
        uint64_t position = file->position;
        uint64_t remaining = returned;
        uint32_t mask = 1U << variant;

        while (remaining) {
            uint64_t available;
            uint64_t consumed;

            if (!(file->buffer_valid_mask & mask) ||
                position < file->buffer_begin[variant] ||
                position >= file->buffer_end[variant])
                vita_crt_io_shadow_refill(file, variant, position);
            available = file->buffer_end[variant] - position;
            consumed = remaining < available ? remaining : available;
            position += consumed;
            remaining -= consumed;
        }
    }
    file->position += returned;
}

static void vita_crt_io_shadow_fflush(vita_crt_io_shadow_file *file)
{
    if (!file->selected)
        vita_crt_io_shadow_select_unknown(file);
    vita_crt_profile_add(&s_io_profile.shadow_fflush_calls, 1U);
    vita_crt_profile_add(
        &s_io_profile.shadow_unmodelled_fflush_calls, 1U);
    vita_crt_io_shadow_invalidate(file);
}

static void vita_crt_io_shadow_fflush_all(void)
{
    uint32_t index;

    vita_crt_profile_add(&s_io_profile.shadow_fflush_all_calls, 1U);
    vita_crt_profile_add(
        &s_io_profile.shadow_unmodelled_fflush_calls, 1U);
    if (s_io_shadow_idle.selected)
        vita_crt_io_shadow_invalidate(&s_io_shadow_idle);
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        if (s_file_tokens[index].file.key[0] &&
            !s_file_tokens[index].raw_archive.live) {
            if (!s_file_tokens[index].io_shadow.selected)
                vita_crt_io_shadow_select_unknown(
                    &s_file_tokens[index].io_shadow);
            else
                vita_crt_io_shadow_invalidate(
                    &s_file_tokens[index].io_shadow);
        }
    }
}

_Static_assert(ISAAC_VITA_CRT_IO_SHADOW_VARIANT_COUNT == 4U &&
               ISAAC_VITA_CRT_IO_SHADOW_64K_INDEX == 3U &&
               ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE == 16U * 1024U &&
               VITA_CRT_IO_SHADOW_SEEK_BLOCK_SIZE == 1024U,
               "Vita CRT I/O shadow model drifted");
#endif
#endif

#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE) && \
    !defined(ISAAC_VITA_CRT_QSORT_HOST_ORACLE)
/* Vita newlib exposes FOPEN_MAX=20.  One retained archive may coexist with
 * the three standard streams and all sixteen measured guest FILE tokens, but
 * a second idle slot would silently reduce the guest-visible open capacity.
 * Raw SceIo tokens have no newlib FILE and therefore consume none of these
 * twenty slots; the hostile oracle fills all sixteen raw-token entries. */
_Static_assert(ISAAC_VITA_CRT_STANDARD_STREAM_COUNT +
               ISAAC_VITA_CRT_FILE_TOKEN_COUNT + 1U <= FOPEN_MAX,
               "Vita archive cache exceeds newlib FILE capacity");
#endif

#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
/* SceIofilemgr drops every open ux0: descriptor of the process across an
 * application suspend/resume or PS-button interception.  The next
 * sceIoRead/sceIoLseek on such a descriptor fails with
 * SCE_ERROR_ERRNO_ENODEV, which both lanes surface as errno ENODEV: newlib
 * through fread -> __srefill_r -> __sread -> _read_r, the raw lane through
 * vita_crt_raw_archive_errno.  The idle archive stream already survives this
 * (a failed cache reset falls back to a fresh fopen); a live token did not,
 * and ArchivedFile then faulted on "block header is invalid".  Every
 * read-only token retains its native path and mode at fopen.  A failed
 * fread/fseek on such a token is recovered exactly once: fresh handle first,
 * positioned at the logical cursor recorded before the call (no syscall:
 * raw position, deferred SEEK_END, or newlib's _offset - _r), then the dead
 * handle is closed, the fresh one swapped into the slot (the published token
 * value is the slot address and does not change) and the whole call redone.
 * Anything that cannot be done exactly leaves the token untouched and lets
 * the ordinary failure path run. */
typedef struct vita_crt_descriptor_recover_stats {
    uint32_t enodev;        /* ENODEV failures on tokens with a retained path */
    uint32_t recovered;     /* ... redone on a fresh handle (one receipt each) */
    uint32_t unknown_pos;   /* ... abandoned: logical cursor unknown */
    uint32_t reopen_fail;   /* ... abandoned: fresh open, size or seek failed */
    uint32_t redo_fail;     /* redone calls that failed again */
} vita_crt_descriptor_recover_stats;

typedef struct vita_crt_descriptor_recover_receipt {
    char key[ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY];
    const char *lane;
    const char *op;
    long position;
} vita_crt_descriptor_recover_receipt;

static vita_crt_descriptor_recover_stats s_descriptor_recover;

static void vita_crt_recover_clear(vita_crt_file_token *entry)
{
    entry->recover.path[0] = '\0';
    entry->recover.mode[0] = '\0';
}

/* Caller owns s_file_lock.  Only "r"/"rb" tokens are recoverable: a write
 * or update stream has buffered state no reopen can reproduce.  A path over
 * the bound leaves the token unrecoverable rather than truncated. */
static void vita_crt_recover_publish(vita_crt_file_token *entry,
                                     const char *mode,
                                     const char *native_path)
{
    size_t path_length = strlen(native_path);
    size_t mode_length = strlen(mode);

    vita_crt_recover_clear(entry);
    if (mode[0] != 'r' || strchr(mode, '+') != NULL ||
        path_length >= sizeof entry->recover.path ||
        mode_length >= sizeof entry->recover.mode)
        return;
    memcpy(entry->recover.path, native_path, path_length + 1U);
    memcpy(entry->recover.mode, mode, mode_length + 1U);
}

/* Caller owns s_file_lock.  The logical cursor of a live token without a
 * syscall, or -1 when it is not known exactly.  A raw token keeps it
 * arithmetically.  A newlib FILE with a deferred SEEK_END sits at
 * size + pending_offset; otherwise newlib's own ftell arithmetic applies,
 * _offset - _r, which is exact only while __SOFF says _offset is (a failed
 * read or seek clears it, so this must run before the native call). */
static long vita_crt_recover_position(const vita_crt_file_token *entry,
                                      FILE *stream)
{
    if (entry->raw_archive.live) {
        if ((uint64_t)entry->raw_archive.position >
            (uint64_t)ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX)
            return -1L;
        return (long)entry->raw_archive.position;
    }
    if (!stream)
        return -1L;
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    if (entry->seek_shadow.pending_end && entry->seek_shadow.size_valid) {
        int64_t target = entry->seek_shadow.size +
            (int64_t)entry->seek_shadow.pending_offset;

        if (target < 0 ||
            target > (int64_t)ISAAC_VITA_CRT_RAW_ARCHIVE_LONG_MAX)
            return -1L;
        return (long)target;
    }
#endif
#if defined(__NEWLIB__)
    if ((stream->_flags & (__SOFF | __SRD)) == (__SOFF | __SRD) &&
        !(stream->_flags & __SWR) && stream->_ub._base == NULL)
        return (long)stream->_offset - (long)stream->_r;
    return -1L;
#else
    /* Host oracles: libc exposes no cursor field, so one real ftell. */
    return ftell(stream);
#endif
}

/* Caller owns s_file_lock.  A fresh SceIo descriptor on the retained path;
 * the archive must still have the size recorded at open.  The dead
 * descriptor goes only once the fresh one is ready; the kernel already
 * dropped it, so its close result carries nothing. */
static int vita_crt_recover_raw(vita_crt_file_token *entry)
{
    int32_t descriptor = vita_crt_raw_sce_open(entry->recover.path);
    int64_t size = 0;

    if (descriptor < 0) {
        ++s_descriptor_recover.reopen_fail;
        return 0;
    }
    if (vita_crt_raw_sce_get_size(descriptor, &size) < 0 ||
        size != entry->raw_archive.size) {
        (void)vita_crt_raw_sce_close(descriptor);
        ++s_descriptor_recover.reopen_fail;
        return 0;
    }
    (void)vita_crt_raw_sce_close(entry->raw_archive.descriptor);
    entry->raw_archive.descriptor = descriptor;
    return 1;
}

/* Caller owns s_file_lock.  A fresh newlib FILE on the retained path with
 * the 16 KiB buffer the open path installs, seeked to `position` when the
 * redo depends on the cursor (a SEEK_SET whose ftell must agree, as the
 * cache reset demands).  The dead FILE is discarded afterwards through the
 * cache module's close seam and never enters the idle slot. */
static int vita_crt_recover_stream(vita_crt_file_token *entry,
                                   long position, int need_position)
{
    isaac_vita_archive_cache_file discard =
        ISAAC_VITA_ARCHIVE_CACHE_FILE_INITIALIZER;
    FILE *fresh;

    if (need_position && position < 0L) {
        ++s_descriptor_recover.unknown_pos;
        return 0;
    }
    fresh = isaac_vita_archive_cache_reopen_native(
        entry->recover.path, entry->recover.mode);
    if (!fresh) {
        ++s_descriptor_recover.reopen_fail;
        return 0;
    }
    (void)setvbuf(fresh, NULL, _IOFBF, ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
    if (need_position &&
        (vita_crt_recover_native_fseek(fresh, position, SEEK_SET) != 0 ||
         vita_crt_recover_native_ftell(fresh) != position)) {
        discard.stream = fresh;
        (void)isaac_vita_archive_cache_force_discard(&discard);
        ++s_descriptor_recover.reopen_fail;
        return 0;
    }
    discard.stream = entry->file.stream;
    (void)isaac_vita_archive_cache_force_discard(&discard);
    entry->file.stream = fresh;
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    /* The fresh handle's real cursor is the logical one: nothing deferred. */
    if (need_position)
        entry->seek_shadow.pending_end = 0U;
#endif
    return 1;
}

#if defined(ISAAC_VITA_IO_PROFILE)
/* The fresh FILE starts with an empty buffer the replay model never saw. */
static void vita_crt_recover_note_io_shadow(vita_crt_file_token *entry)
{
    if (!s_io_profile_active || !entry->file.key[0] ||
        entry->raw_archive.live)
        return;
    if (!entry->io_shadow.selected)
        vita_crt_io_shadow_select_unknown(&entry->io_shadow);
    else
        vita_crt_io_shadow_invalidate(&entry->io_shadow);
}
#endif

/* Caller owns s_file_lock.  One recovery attempt for a call that failed
 * with ENODEV on a token with a retained path.  Returns 1 when the token
 * owns a fresh handle at the recorded cursor and the caller must redo the
 * call from its start; 0 leaves the token untouched.  The receipt is filled
 * for the caller to log once the lock is released. */
static int vita_crt_recover_token(vita_crt_file_token *entry, long position,
                                  int need_position, const char *op,
                                  vita_crt_descriptor_recover_receipt *receipt)
{
    const char *name;
    size_t length;
    int saved_errno = errno;
    int recovered;

    ++s_descriptor_recover.enodev;
    if (entry->raw_archive.live) {
        if (position < 0L) {
            ++s_descriptor_recover.unknown_pos;
            recovered = 0;
        } else {
            recovered = vita_crt_recover_raw(entry);
            if (recovered)
                entry->raw_archive.position = position;
        }
    } else {
        recovered = vita_crt_recover_stream(entry, position, need_position);
    }
    errno = saved_errno;
    if (!recovered)
        return 0;
    ++s_descriptor_recover.recovered;
#if defined(ISAAC_VITA_IO_PROFILE)
    vita_crt_recover_note_io_shadow(entry);
#endif
    name = strrchr(entry->recover.path, '/');
    name = entry->file.key[0] ? entry->file.key
        : name ? name + 1 : entry->recover.path;
    length = strlen(name);
    if (length >= sizeof receipt->key)
        length = sizeof receipt->key - 1U;
    memcpy(receipt->key, name, length);
    receipt->key[length] = '\0';
    receipt->lane = entry->raw_archive.live ? "raw" : "newlib";
    receipt->op = op;
    receipt->position = position;
    return 1;
}

/* Never under s_file_lock. */
static void vita_crt_recover_log_receipt(
    const vita_crt_descriptor_recover_receipt *receipt)
{
    isaac_vita_log(
        "arcdiag v1 tag=descriptor-recover-v1 key=%s lane=%s pos=%ld op=%s",
        receipt->key, receipt->lane, receipt->position, receipt->op);
}
#endif

/* The focused qsort host oracle compiles this complete translation unit on
 * x86-64 and garbage-collects the file family.  Keep its unused spin hint
 * assemblable without changing the production ARM instruction. */
#if defined(ISAAC_VITA_CRT_QSORT_HOST_ORACLE)
#define VITA_CRT_SPIN_HINT() __asm__ volatile("" ::: "memory")
#else
#define VITA_CRT_SPIN_HINT() __asm__ volatile("yield" ::: "memory")
#endif

static uint32_t vita_crt_arg(CPU *__restrict c, uint32_t index)
{
    /* ESP points at the x86 return address while an import is active. */
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

/* Only inspect EBP after the import return proves which measured adapter owns
 * the frame.  Generic CRT host oracles call the import handler directly and
 * therefore never expose an adapter frame here. */
static uint32_t vita_crt_adapter_parent_return(
    CPU *__restrict c, uint32_t adapter_return)
{
    uint32_t return_address;
    uint32_t parent_address;

    return_address = guest_stack_address(c, c->esp, 4U, 0U);
    if (!return_address || ld32(return_address) != adapter_return)
        return 0U;
    if (c->ebp > UINT32_MAX - 4U)
        return 0U;
    parent_address = guest_stack_address(c, c->ebp + 4U, 4U, 0U);
    if (!parent_address)
        return 0U;
    return ld32(parent_address);
}

static void vita_crt_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);               /* the translated caller removes arguments */
}

static void vita_crt_stdcall_return(CPU *__restrict c,
                                    uint32_t argument_bytes)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, argument_bytes, 0U);
}

static int vita_crt_guest_pointer(CPU *__restrict c, const void *pointer,
                                  uint32_t *result)
{
    uintptr_t address = (uintptr_t)pointer;
#if UINTPTR_MAX > UINT32_MAX
    if (address > UINT32_MAX) {
        guest_fault(c, 0U, "Vita CRT storage is not a 32-bit guest pointer");
        return 0;
    }
#else
    (void)c;
#endif
    *result = (uint32_t)address;
    return 1;
}

#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
static void vita_crt_seek_shadow_bump_ret(uint32_t key)
{
    uint32_t index;
    uint32_t victim = 0U;

    for (index = 0U; index < VITA_CRT_SEEK_SHADOW_TOP; ++index) {
        if (s_seek_shadow.top_ret[index].count &&
            s_seek_shadow.top_ret[index].key == key) {
            ++s_seek_shadow.top_ret[index].count;
            return;
        }
        if (s_seek_shadow.top_ret[index].count <
            s_seek_shadow.top_ret[victim].count)
            victim = index;
    }
    s_seek_shadow.top_ret[victim].key = key;
    s_seek_shadow.top_ret[victim].count = 1U;
}

static void vita_crt_seek_shadow_bump_name(const char *name)
{
    uint32_t index;
    uint32_t victim = 0U;

    for (index = 0U; index < VITA_CRT_SEEK_SHADOW_TOP; ++index) {
        if (s_seek_shadow.top_name[index].count &&
            strcmp(s_seek_shadow.top_name[index].name, name) == 0) {
            ++s_seek_shadow.top_name[index].count;
            return;
        }
        if (s_seek_shadow.top_name[index].count <
            s_seek_shadow.top_name[victim].count)
            victim = index;
    }
    memcpy(s_seek_shadow.top_name[victim].name, name,
           sizeof s_seek_shadow.top_name[victim].name);
    s_seek_shadow.top_name[victim].count = 1U;
}

/* Caller owns s_file_lock.  Attribute one eligible SEEK_END to the guest
 * loop that issued it: the import return word, or for File::GetSize its
 * caller ([EBP+4] behind GetSize's `push ebp; mov ebp, esp`), or for
 * File::IsEOF (frameless `push esi; push edi`) that caller's return at
 * [EBP+16].  Returns nonzero when a periodic report is due. */
static int vita_crt_seek_shadow_attribute(CPU *__restrict c,
                                          const vita_crt_file_token *entry)
{
    uint32_t address = guest_stack_address(c, c->esp, 4U, 0U);
    uint32_t key = address ? ld32(address) : 0U;

    if (key == ISAAC_VITA_CRT_FSEEK_FIRST_RETURN_RVA) {
        uint32_t parent = vita_crt_adapter_parent_return(c, key);

        if (parent) {
            key = parent;
            if (parent == ISAAC_VITA_CRT_SEEK_SHADOW_ISEOF_RETURN_RVA &&
                c->ebp <= UINT32_MAX - 16U) {
                address = guest_stack_address(c, c->ebp + 16U, 4U, 0U);
                if (address)
                    key = ld32(address);
            }
        }
    }
    vita_crt_seek_shadow_bump_ret(key);
    vita_crt_seek_shadow_bump_name(entry->seek_shadow.name);
    if (++s_seek_shadow_since_report >= VITA_CRT_SEEK_SHADOW_REPORT_PERIOD) {
        s_seek_shadow_since_report = 0U;
        return 1;
    }
    return 0;
}
#endif

static int vita_crt_copy_ascii(CPU *__restrict c, uint32_t address,
                               char *out, uint32_t maximum,
                               const char *null_fault,
                               const char *long_fault)
{
    uint32_t i;

    if (!address) {
        guest_fault(c, address, null_fault);
        return 0;
    }
    for (i = 0U; i <= maximum; ++i) {
        uint8_t value;
        if (address > UINT32_MAX - i) {
            guest_fault(c, address, long_fault);
            return 0;
        }
        value = ld8(address + i);
        if (!value) {
            out[i] = '\0';
            return 1;
        }
        if (i == maximum) {
            guest_fault(c, address, long_fault);
            return 0;
        }
        out[i] = (char)value;
    }
    return 0;
}

static int vita_crt_call_initializer(CPU *__restrict c, uint32_t target)
{
    uint32_t saved = c->esp;

    /* A translated RET still needs an x86 return address even though the
     * native C activation returns directly to this shim. */
    gpush(c, 0xFFF1A11EU);
    guest_call(c, target);
    if (c->fault)
        return 0;
    if (c->esp != saved) {
        guest_fault(c, target, "CRT callback did not restore its cdecl stack");
        return 0;
    }
    return 1;
}

static int vita_crt_call_stdcall3(CPU *__restrict c, uint32_t target,
                                  uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t saved = c->esp;

    gpush(c, a2);
    gpush(c, a1);
    gpush(c, a0);
    gpush(c, 0xFFF17311U);
    guest_call(c, target);
    if (c->fault)
        return 0;
    if (c->esp != saved) {
        guest_fault(c, target,
                    "TLS exit callback did not clean its stdcall arguments");
        return 0;
    }
    return 1;
}

static void vita_crt_initterm_e(CPU *__restrict c)
{
    uint32_t current = vita_crt_arg(c, 0U);
    uint32_t end = vita_crt_arg(c, 1U);
    uint32_t result = 0U;

    if (current > end || ((end - current) & 3U)) {
        guest_fault(c, current, "_initterm_e invalid guest range");
        return;
    }
    while (current < end) {
        uint32_t callback = ld32(current);
        current += 4U;
        if (!callback)
            continue;
        if (!vita_crt_call_initializer(c, callback))
            return;
        result = c->eax;
        if (result)
            break;
    }
    c->eax = result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_initterm(CPU *__restrict c)
{
    uint32_t current = vita_crt_arg(c, 0U);
    uint32_t end = vita_crt_arg(c, 1U);

    if (current > end || ((end - current) & 3U)) {
        guest_fault(c, current, "_initterm invalid guest range");
        return;
    }
    while (current < end) {
        uint32_t callback = ld32(current);
        current += 4U;
        if (callback && !vita_crt_call_initializer(c, callback))
            return;
    }
    /* void (__cdecl): callback EAX is deliberately not a result channel. */
    vita_crt_cdecl_return(c);
}

static void vita_crt_set_app_type(CPU *__restrict c)
{
    g_isaac_vita_crt.app_type = vita_crt_arg(c, 0U);
    vita_crt_cdecl_return(c);
}

static void vita_crt_set_fmode(CPU *__restrict c)
{
    g_isaac_vita_crt.fmode = vita_crt_arg(c, 0U);
    c->eax = 0U;                 /* errno_t success */
    vita_crt_cdecl_return(c);
}

static void vita_crt_p_commode(CPU *__restrict c)
{
    uint32_t pointer;
    if (!vita_crt_guest_pointer(c, &g_isaac_vita_crt.commode, &pointer))
        return;
    c->eax = pointer;
    vita_crt_cdecl_return(c);
}

/* UCRT exposes errno as writable CRT-owned storage.  Both imported entry
 * points address the same cell: `_errno(void)` returns its stable pointer,
 * while `_set_errno(int)` stores the caller's complete 32-bit value and
 * returns errno_t success.  Neither operation touches newlib's host errno. */
static void vita_crt_errno(CPU *__restrict c)
{
    uint32_t pointer;

    if (!vita_crt_guest_pointer(c, &g_isaac_vita_crt_errno, &pointer))
        return;
    c->eax = pointer;
    vita_crt_cdecl_return(c);
}

static void vita_crt_set_errno(CPU *__restrict c)
{
    g_isaac_vita_crt_errno = (int32_t)vita_crt_arg(c, 0U);
    c->eax = 0U;
    vita_crt_cdecl_return(c);
}

/* Microsoft x86 struct tm is exactly nine consecutive signed dwords.  Keep
 * the boundary explicit instead of depending on newlib's native extension
 * fields or layout. */
static void vita_crt_load_tm(uint32_t address, struct tm *value)
{
    memset(value, 0, sizeof *value);
    value->tm_sec   = (int32_t)ld32(address + 0U);
    value->tm_min   = (int32_t)ld32(address + 4U);
    value->tm_hour  = (int32_t)ld32(address + 8U);
    value->tm_mday  = (int32_t)ld32(address + 12U);
    value->tm_mon   = (int32_t)ld32(address + 16U);
    value->tm_year  = (int32_t)ld32(address + 20U);
    value->tm_wday  = (int32_t)ld32(address + 24U);
    value->tm_yday  = (int32_t)ld32(address + 28U);
    value->tm_isdst = (int32_t)ld32(address + 32U);
}

static void vita_crt_store_tm(uint32_t address, const struct tm *value)
{
    st32(address + 0U,  (uint32_t)value->tm_sec);
    st32(address + 4U,  (uint32_t)value->tm_min);
    st32(address + 8U,  (uint32_t)value->tm_hour);
    st32(address + 12U, (uint32_t)value->tm_mday);
    st32(address + 16U, (uint32_t)value->tm_mon);
    st32(address + 20U, (uint32_t)value->tm_year);
    st32(address + 24U, (uint32_t)value->tm_wday);
    st32(address + 28U, (uint32_t)value->tm_yday);
    st32(address + 32U, (uint32_t)value->tm_isdst);
}

static int64_t vita_crt_load_time64(uint32_t address)
{
    uint64_t bits = (uint64_t)ld32(address) |
                    ((uint64_t)ld32(address + 4U) << 32U);
    return (int64_t)bits;
}

static void vita_crt_store_time64(uint32_t address, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    st32(address, (uint32_t)bits);
    st32(address + 4U, (uint32_t)(bits >> 32U));
}

static void vita_crt_return_time64(CPU *__restrict c, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    c->eax = (uint32_t)bits;
    c->edx = (uint32_t)(bits >> 32U);
}

static int64_t vita_crt_floor_div(int64_t numerator, int64_t denominator)
{
    int64_t quotient = numerator / denominator;
    int64_t remainder = numerator % denominator;

    if (remainder < 0)
        --quotient;
    return quotient;
}

/* Howard Hinnant's proleptic-Gregorian mapping, expressed with signed 64-bit
 * intermediates so even every possible nine-dword tm input is normalised
 * without overflowing before the documented 1970..3000 range check. */
static int64_t vita_crt_days_from_civil(int64_t year, unsigned month,
                                        unsigned day)
{
    int64_t era;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned day_of_era;
    unsigned shifted_month;

    year -= month <= 2U;
    era = vita_crt_floor_div(year, 400);
    year_of_era = (unsigned)(year - era * 400);
    shifted_month = month > 2U ? month - 3U : month + 9U;
    day_of_year = (153U * shifted_month + 2U) / 5U + day - 1U;
    day_of_era = year_of_era * 365U + year_of_era / 4U -
                  year_of_era / 100U + day_of_year;
    return era * INT64_C(146097) + (int64_t)day_of_era -
           INT64_C(719468);
}

static void vita_crt_civil_from_days(int64_t days, int *year,
                                     unsigned *month, unsigned *day)
{
    int64_t era;
    int64_t civil_year;
    unsigned day_of_era;
    unsigned year_of_era;
    unsigned day_of_year;
    unsigned month_prime;

    days += INT64_C(719468);
    era = vita_crt_floor_div(days, INT64_C(146097));
    day_of_era = (unsigned)(days - era * INT64_C(146097));
    year_of_era = (day_of_era - day_of_era / 1460U +
                   day_of_era / 36524U - day_of_era / 146096U) / 365U;
    civil_year = (int64_t)year_of_era + era * 400;
    day_of_year = day_of_era -
                  (365U * year_of_era + year_of_era / 4U -
                   year_of_era / 100U);
    month_prime = (5U * day_of_year + 2U) / 153U;
    *day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    *month = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    civil_year += *month <= 2U;
    *year = (int)civil_year;
}

static int vita_crt_seconds_to_tm(int64_t seconds, struct tm *value)
{
    int64_t days;
    int64_t remainder;
    int64_t weekday;
    int year;
    unsigned month;
    unsigned day;

    if (seconds < 0 || seconds > ISAAC_VITA_CRT_TIME64_MAX)
        return 0;
    days = seconds / INT64_C(86400);
    remainder = seconds % INT64_C(86400);
    vita_crt_civil_from_days(days, &year, &month, &day);
    weekday = (days + 4) % 7;
    if (weekday < 0)
        weekday += 7;

    memset(value, 0, sizeof *value);
    value->tm_sec = (int)(remainder % 60);
    remainder /= 60;
    value->tm_min = (int)(remainder % 60);
    value->tm_hour = (int)(remainder / 60);
    value->tm_mday = (int)day;
    value->tm_mon = (int)month - 1;
    value->tm_year = year - 1900;
    value->tm_wday = (int)weekday;
    value->tm_yday = (int)(days - vita_crt_days_from_civil(year, 1U, 1U));
    value->tm_isdst = 0;
    return 1;
}

static int vita_crt_normalize_utc_tm(const struct tm *source,
                                     int64_t *seconds, struct tm *normal)
{
    int64_t year = (int64_t)source->tm_year + 1900;
    int64_t month = source->tm_mon;
    int64_t month_quotient = vita_crt_floor_div(month, 12);
    int64_t days;
    int64_t result;

    year += month_quotient;
    month -= month_quotient * 12;
    days = vita_crt_days_from_civil(year, (unsigned)month + 1U, 1U) +
           (int64_t)source->tm_mday - 1;
    result = days * INT64_C(86400) +
             (int64_t)source->tm_hour * INT64_C(3600) +
             (int64_t)source->tm_min * 60 + source->tm_sec;
    if (!vita_crt_seconds_to_tm(result, normal))
        return 0;
    *seconds = result;
    return 1;
}

static int vita_crt_rtc_tick_offset(const SceRtcTick *local_tick,
                                    const SceRtcTick *utc_tick,
                                    int64_t *seconds)
{
    uint64_t difference;
    unsigned resolution = sceRtcGetTickResolution();

    if (!resolution)
        return 0;
    if (local_tick->tick >= utc_tick->tick) {
        difference = local_tick->tick - utc_tick->tick;
        if (difference > INT64_MAX)
            return 0;
        *seconds = (int64_t)(difference / resolution);
    } else {
        difference = utc_tick->tick - local_tick->tick;
        if (difference > INT64_MAX)
            return 0;
        *seconds = -(int64_t)(difference / resolution);
    }
    return 1;
}

static int vita_crt_rtc_convert_local(const SceDateTime *utc,
                                      SceDateTime *local,
                                      int64_t *offset_seconds)
{
    SceRtcTick utc_tick;
    SceRtcTick local_tick;

    if (sceRtcGetTick(utc, &utc_tick) < 0 ||
        sceRtcConvertUtcToLocalTime(&utc_tick, &local_tick) < 0 ||
        sceRtcSetTick(local, &local_tick) < 0)
        return 0;
    return !offset_seconds ||
           vita_crt_rtc_tick_offset(&local_tick, &utc_tick, offset_seconds);
}

static int vita_crt_rtc_month_offset(int year, unsigned month,
                                     int64_t *offset_seconds)
{
    SceDateTime utc;
    SceDateTime local;

    memset(&utc, 0, sizeof utc);
    utc.year = (uint16_t)year;
    utc.month = (uint16_t)month;
    utc.day = 15U;
    utc.hour = 12U;
    return vita_crt_rtc_convert_local(&utc, &local, offset_seconds);
}

static int vita_crt_rtc_local_tm(int64_t seconds, struct tm *value)
{
    SceDateTime utc;
    SceDateTime local;
    int64_t days;
    int64_t weekday;
    int64_t current_offset;
    int64_t standard_offset = INT64_MAX;
    unsigned month;

    if (sceRtcSetTime64_t(&utc, (SceUInt64)seconds) < 0 ||
        !vita_crt_rtc_convert_local(&utc, &local, &current_offset))
        return 0;

    memset(value, 0, sizeof *value);
    value->tm_sec = local.second;
    value->tm_min = local.minute;
    value->tm_hour = local.hour;
    value->tm_mday = local.day;
    value->tm_mon = local.month - 1;
    value->tm_year = local.year - 1900;
    days = vita_crt_days_from_civil(local.year, local.month, local.day);
    weekday = (days + 4) % 7;
    if (weekday < 0)
        weekday += 7;
    value->tm_wday = (int)weekday;
    value->tm_yday = (int)(days -
        vita_crt_days_from_civil(local.year, 1U, 1U));

    /* SceRtc owns the platform timezone conversion but exposes no separate
     * DST bit.  Infer it from that same conversion: standard time is the
     * smallest UTC offset observed across the target calendar year. */
    for (month = 1U; month <= 12U; ++month) {
        int64_t offset;
        if (vita_crt_rtc_month_offset(local.year, month, &offset) &&
            offset < standard_offset)
            standard_offset = offset;
    }
    value->tm_isdst = standard_offset != INT64_MAX &&
                      current_offset > standard_offset;
    return 1;
}

/* __time64_t __cdecl _time64(__time64_t *): return EDX:EAX and, when the
 * pointer is non-NULL, write the identical eight-byte value. */
static void vita_crt_time64(CPU *__restrict c)
{
    uint32_t output = vita_crt_arg(c, 0U);
    uint64_t filetime = 0U;
    int64_t value = -1;
    int saved_errno = errno;

    if (output && output > UINT32_MAX - 7U) {
        guest_fault(c, output, "_time64 output range overflow");
        return;
    }
    if (isaac_vita_get_win32_filetime(&filetime) == 0 &&
        filetime >= ISAAC_VITA_CRT_FILETIME_UNIX_EPOCH) {
        uint64_t seconds =
            (filetime - ISAAC_VITA_CRT_FILETIME_UNIX_EPOCH) /
            ISAAC_VITA_CRT_FILETIME_TICKS_PER_SECOND;
        if (seconds <= (uint64_t)ISAAC_VITA_CRT_TIME64_MAX)
            value = (int64_t)seconds;
    }
    if (value < 0)
        g_isaac_vita_crt_errno = EINVAL;
    if (output)
        vita_crt_store_time64(output, value);
    vita_crt_return_time64(c, value);
    errno = saved_errno;
    vita_crt_cdecl_return(c);
}

/* _gmtime64 and _localtime64 share this one stable CRT-owned tm cell, as
 * UCRT does.  Every measured caller copies it before the next time call. */
static void vita_crt_gmtime64(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    uint32_t result_pointer;
    struct tm value;
    int saved_errno = errno;

    if (!source || source > UINT32_MAX - 7U ||
        !vita_crt_seconds_to_tm(source ? vita_crt_load_time64(source) : -1,
                                &value)) {
        g_isaac_vita_crt_errno = EINVAL;
        c->eax = 0U;
    } else if (!vita_crt_guest_pointer(c, s_time_tm, &result_pointer)) {
        errno = saved_errno;
        return;
    } else {
        vita_crt_store_tm(result_pointer, &value);
        c->eax = result_pointer;
    }
    errno = saved_errno;
    vita_crt_cdecl_return(c);
}

static void vita_crt_localtime64(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    uint32_t result_pointer;
    struct tm value;
    int64_t seconds = -1;
    int saved_errno = errno;

    if (source && source <= UINT32_MAX - 7U)
        seconds = vita_crt_load_time64(source);
    if (seconds < 0 || seconds > ISAAC_VITA_CRT_TIME64_MAX ||
        !vita_crt_rtc_local_tm(seconds, &value)) {
        g_isaac_vita_crt_errno = EINVAL;
        c->eax = 0U;
    } else if (!vita_crt_guest_pointer(c, s_time_tm, &result_pointer)) {
        errno = saved_errno;
        return;
    } else {
        vita_crt_store_tm(result_pointer, &value);
        c->eax = result_pointer;
    }
    errno = saved_errno;
    vita_crt_cdecl_return(c);
}

/* _mkgmtime64 accepts deliberately out-of-range fields, normalises all nine
 * dwords in place on success, and interprets the result as UTC. */
static void vita_crt_mkgmtime64(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    struct tm input;
    struct tm normal;
    int64_t result = -1;
    int saved_errno = errno;

    if (source && source <= UINT32_MAX - 35U) {
        vita_crt_load_tm(source, &input);
        if (vita_crt_normalize_utc_tm(&input, &result, &normal))
            vita_crt_store_tm(source, &normal);
    }
    if (result < 0)
        g_isaac_vita_crt_errno = EINVAL;
    vita_crt_return_time64(c, result);
    errno = saved_errno;
    vita_crt_cdecl_return(c);
}

/* The guest has no locale-changing import, so newlib remains in its initial
 * C locale.  Decode the guest tm layout, then let its complete strftime
 * implementation handle the format language. */
static void vita_crt_strftime(CPU *__restrict c)
{
    uint32_t destination = vita_crt_arg(c, 0U);
    uint32_t capacity = vita_crt_arg(c, 1U);
    uint32_t format = vita_crt_arg(c, 2U);
    uint32_t time_value = vita_crt_arg(c, 3U);
    struct tm value;
    size_t result = 0U;
    int saved_errno = errno;

    if (!destination || !format || !time_value) {
        g_isaac_vita_crt_errno = EINVAL;
    } else if (capacity) {
        vita_crt_load_tm(time_value, &value);
        result = strftime((char *)(uintptr_t)destination, (size_t)capacity,
                          (const char *)(uintptr_t)format, &value);
    }
    c->eax = (uint32_t)result;
    errno = saved_errno;
    vita_crt_cdecl_return(c);
}

static void vita_crt_atexit(CPU *__restrict c)
{
    uint32_t callback = vita_crt_arg(c, 0U);

    while (__sync_lock_test_and_set(&s_atexit_lock, 1U))
        VITA_CRT_SPIN_HINT();
    if (g_isaac_vita_crt.atexit_count >= ISAAC_VITA_CRT_ATEXIT_MAX) {
        __sync_lock_release(&s_atexit_lock);
        guest_fault(c, callback, "_crt_atexit table full");
        return;
    }
    g_isaac_vita_crt_atexit[g_isaac_vita_crt.atexit_count++] = callback;
    __sync_lock_release(&s_atexit_lock);
    c->eax = 0U;
    vita_crt_cdecl_return(c);
}

static void vita_crt_register_tls_atexit(CPU *__restrict c)
{
    uint32_t callback = vita_crt_arg(c, 0U);

    while (__sync_lock_test_and_set(&s_atexit_lock, 1U))
        VITA_CRT_SPIN_HINT();
    if (s_tls_atexit_registered) {
        __sync_lock_release(&s_atexit_lock);
        guest_fault(c, callback, "TLS exit callback registered twice");
        return;
    }
    s_tls_atexit = callback;
    s_tls_atexit_registered = 1U;
    __sync_lock_release(&s_atexit_lock);
    vita_crt_cdecl_return(c);
}

static int vita_crt_run_exit_cleanup(CPU *__restrict c)
{
    uint32_t callback;

    if (!__sync_bool_compare_and_swap(&s_exit_running, 0U, 1U)) {
        guest_fault(c, 0U, "recursive or concurrent guest exit cleanup");
        return 0;
    }

    while (__sync_lock_test_and_set(&s_atexit_lock, 1U))
        VITA_CRT_SPIN_HINT();
    callback = s_tls_atexit;
    s_tls_atexit = 0U;
    s_tls_atexit_registered = 0U;
    __sync_lock_release(&s_atexit_lock);
    if (callback && !vita_crt_call_stdcall3(c, callback, 0U, 0U, 0U))
        goto failed;

    for (;;) {
        while (__sync_lock_test_and_set(&s_atexit_lock, 1U))
            VITA_CRT_SPIN_HINT();
        if (!g_isaac_vita_crt.atexit_count) {
            __sync_lock_release(&s_atexit_lock);
            break;
        }
        callback = g_isaac_vita_crt_atexit[
            --g_isaac_vita_crt.atexit_count];
        g_isaac_vita_crt_atexit[
            g_isaac_vita_crt.atexit_count] = 0U;
        __sync_lock_release(&s_atexit_lock);
        if (callback && !vita_crt_call_initializer(c, callback))
            goto failed;
    }
    if (isaac_vita_crt_flush_all() == EOF)
        isaac_vita_log("guest exit stream flush failed: errno=%d",
                       g_isaac_vita_crt_errno);
    /* Successful `exit` is terminal.  Keep the state RUNNING until
     * guest_exit unwinds the translated call tree; only returning cleanup
     * APIs such as `_cexit` would be allowed to reset it. */
    return 1;

failed:
    __sync_lock_release(&s_exit_running);
    return 0;
}

static void vita_crt__exit(CPU *__restrict c)
{
    /* Microsoft `_exit` is deliberately quick: no TLS callbacks, atexit or
     * stream flush.  The native handoff must not silently strengthen it. */
    guest_exit(c, (int32_t)vita_crt_arg(c, 0U), "_exit");
}

static void vita_crt_exit(CPU *__restrict c)
{
    int32_t code = (int32_t)vita_crt_arg(c, 0U);

    if (!vita_crt_run_exit_cleanup(c))
        return;
    guest_exit(c, code, "exit");
}

static void vita_crt_configure_narrow_argv(CPU *__restrict c)
{
    g_isaac_vita_crt.argv_mode = vita_crt_arg(c, 0U);
    c->eax = 0U;                 /* errno_t success */
    vita_crt_cdecl_return(c);
}

static void vita_crt_InitializeSListHead(CPU *__restrict c)
{
    uint32_t header = vita_crt_arg(c, 0U);

    /* The x86 SLIST_HEADER is one aligned 64-bit word.  Vita has no Win32
     * interlocked list to delegate to; its required initialized value is
     * exactly all zeroes, written in the guest's own eight-byte layout. */
    st32(header, 0U);
    st32(header + 4U, 0U);
    vita_crt_stdcall_return(c, 4U);
}

static void vita_crt_controlfp_s(CPU *__restrict c)
{
    uint32_t output = vita_crt_arg(c, 0U);
    uint32_t value = vita_crt_arg(c, 1U);
    uint32_t mask = vita_crt_arg(c, 2U);

    g_isaac_vita_crt.fp_control =
        (g_isaac_vita_crt.fp_control & ~mask) | (value & mask);
    if (output)
        st32(output, g_isaac_vita_crt.fp_control);
    c->eax = 0U;                 /* errno_t success */
    vita_crt_cdecl_return(c);
}

static void vita_crt_configthreadlocale(CPU *__restrict c)
{
    int32_t requested = (int32_t)vita_crt_arg(c, 0U);
    int32_t previous = g_isaac_vita_crt.thread_locale;

    /* Exact current host_win32.c contract: 0 disables, 1 enables, and every
     * other value (including -1) is a query. */
    if (requested == 0 || requested == 1)
        g_isaac_vita_crt.thread_locale = requested;
    c->eax = (uint32_t)previous;
    vita_crt_cdecl_return(c);
}

static int vita_crt_initialize_environment_storage(CPU *__restrict c)
{
    uint32_t argv0_pointer;

    if (s_environment_ready)
        return 1;
    s_empty_environment[0] = 0U;
    if (!vita_crt_guest_pointer(c, s_empty_environment,
                                &s_empty_environment_pointer))
        return 0;
    if (!vita_crt_guest_pointer(c, s_argv0, &argv0_pointer))
        return 0;
    s_argv[0] = argv0_pointer;
    s_argv[1] = 0U;
    if (!vita_crt_guest_pointer(c, s_argv, &s_argv_pointer))
        return 0;
    s_argc = 1;
    s_environment_ready = 1;
    return 1;
}

static void vita_crt_initialize_narrow_environment(CPU *__restrict c)
{
    if (!vita_crt_initialize_environment_storage(c))
        return;
    c->eax = 0U;
    vita_crt_cdecl_return(c);
}

static void vita_crt_get_initial_narrow_environment(CPU *__restrict c)
{
    if (!vita_crt_initialize_environment_storage(c))
        return;
    c->eax = s_empty_environment_pointer;
    vita_crt_cdecl_return(c);
}

static void vita_crt_p_argv(CPU *__restrict c)
{
    uint32_t pointer;

    if (!vita_crt_initialize_environment_storage(c) ||
        !vita_crt_guest_pointer(c, &s_argv_pointer, &pointer))
        return;
    c->eax = pointer;
    vita_crt_cdecl_return(c);
}

static void vita_crt_p_argc(CPU *__restrict c)
{
    uint32_t pointer;

    if (!vita_crt_initialize_environment_storage(c) ||
        !vita_crt_guest_pointer(c, &s_argc, &pointer))
        return;
    c->eax = pointer;
    vita_crt_cdecl_return(c);
}

static void vita_crt_getenv(CPU *__restrict c)
{
    uint32_t name = vita_crt_arg(c, 0U);
    char measured[ISAAC_VITA_CRT_GETENV_NAME_MAX + 1U];
    uint32_t pointer;

    if (!vita_crt_copy_ascii(
            c, name, measured, ISAAC_VITA_CRT_GETENV_NAME_MAX,
            "getenv received a null name",
            "getenv name exceeds measured bound"))
        return;
    if (strcmp(measured, ISAAC_VITA_CRT_GETENV_USERPROFILE_NAME) == 0) {
        if (!vita_crt_guest_pointer(c, s_user_profile, &pointer))
            return;
        c->eax = pointer;
    } else if (strcmp(measured, ISAAC_VITA_CRT_GETENV_HOMEDRIVE_NAME) == 0 ||
               strcmp(measured, ISAAC_VITA_CRT_GETENV_HOMEPATH_NAME) == 0) {
        c->eax = 0U;
    } else {
        guest_fault(c, name, "getenv received an unmeasured variable name");
        return;
    }
    vita_crt_cdecl_return(c);
}

/* Copied from the exercised host_win32 guest-va_list formatter.  Native
 * varargs are never used: every argument remains an x86 guest dword. */
static void vita_crt_stdio_flush(vita_crt_format_output *out)
{
    size_t written;

#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (out->async_image) {
        if (out->stream_failed || !out->stream_used)
            return;
        errno = 0;
        written = isaac_vita_async_write_image_write(
            out->async_image, out->stream_buffer, 1U, out->stream_used);
        if (written != out->stream_used)
            out->stream_failed = 1;
        out->stream_used = 0U;
        return;
    }
#endif
    if (!out->stream || out->stream_failed || !out->stream_used)
        return;
    /* Each chunk is a separate native transaction.  Do not let an errno
     * value left by a successful earlier fwrite masquerade as the reason for
     * a later short write which did not set errno. */
    errno = 0;
    written = fwrite(out->stream_buffer, 1U, out->stream_used, out->stream);
    if (written != out->stream_used)
        out->stream_failed = 1;
    out->stream_used = 0U;
}

static void vita_crt_stdio_put(vita_crt_format_output *out, uint8_t value)
{
    if (out->stream
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
        || out->async_image
#endif
        ) {
        if (!out->stream_failed) {
            out->stream_buffer[out->stream_used++] = value;
            if (out->stream_used == out->stream_capacity)
                vita_crt_stdio_flush(out);
        }
    } else if (out->written < out->limit) {
        st8(out->buffer + out->written, value);
    }
    ++out->written;
}

static void vita_crt_stdio_put_string_limited(vita_crt_format_output *out,
                                              uint32_t source,
                                              uint32_t limit)
{
    static const char null_string[] = "(null)";
    if (!source) {
        const char *p = null_string;
        while (*p && limit) {
            vita_crt_stdio_put(out, (uint8_t)*p++);
            --limit;
        }
        return;
    }
    while (limit) {
        uint8_t value = ld8(source++);
        if (!value)
            return;
        vita_crt_stdio_put(out, value);
        --limit;
    }
}

static void vita_crt_stdio_put_string(vita_crt_format_output *out,
                                      uint32_t source)
{
    vita_crt_stdio_put_string_limited(out, source, UINT32_MAX);
}

static void vita_crt_stdio_put_repeat(vita_crt_format_output *out,
                                      uint8_t value, unsigned count)
{
    while (count--)
        vita_crt_stdio_put(out, value);
}

static void vita_crt_stdio_put_i32_width(vita_crt_format_output *out,
                                         uint32_t raw, unsigned width,
                                         uint8_t padding, int space_sign)
{
    char digits[10];
    uint32_t magnitude = raw;
    unsigned used = 0U;
    unsigned prefix;
    uint8_t sign = 0U;

    if ((int32_t)raw < 0) {
        sign = '-';
        magnitude = 0U - raw;
    } else if (space_sign) {
        sign = ' ';
    }
    do {
        digits[used++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    prefix = sign != 0U;
    if (padding != '0' && width > used + prefix)
        vita_crt_stdio_put_repeat(out, ' ', width - used - prefix);
    if (sign)
        vita_crt_stdio_put(out, sign);
    if (padding == '0' && width > used + prefix)
        vita_crt_stdio_put_repeat(out, '0', width - used - prefix);
    while (used)
        vita_crt_stdio_put(out, (uint8_t)digits[--used]);
}

static void vita_crt_stdio_put_u32_width(vita_crt_format_output *out,
                                         uint32_t raw, unsigned width,
                                         uint8_t padding)
{
    char digits[10];
    unsigned used = 0U;
    do {
        digits[used++] = (char)('0' + raw % 10U);
        raw /= 10U;
    } while (raw);
    if (width > used)
        vita_crt_stdio_put_repeat(out, padding, width - used);
    while (used)
        vita_crt_stdio_put(out, (uint8_t)digits[--used]);
}

static void vita_crt_stdio_put_u64_hex_width(vita_crt_format_output *out,
                                             uint64_t raw, int uppercase,
                                             unsigned width)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    const char *alphabet = uppercase ? upper : lower;
    char digits[16];
    unsigned used = 0U;
    do {
        digits[used++] = alphabet[(unsigned)(raw & 15U)];
        raw >>= 4;
    } while (raw);
    while (used < width)
        digits[used++] = '0';
    while (used)
        vita_crt_stdio_put(out, (uint8_t)digits[--used]);
}

static void vita_crt_stdio_put_u64(vita_crt_format_output *out, uint64_t raw)
{
    char digits[20];
    unsigned used = 0U;
    do {
        digits[used++] = (char)('0' + raw % 10U);
        raw /= 10U;
    } while (raw);
    while (used)
        vita_crt_stdio_put(out, (uint8_t)digits[--used]);
}

static int vita_crt_stdio_put_double(CPU *__restrict c,
                                     vita_crt_format_output *out,
                                     uint64_t raw,
                                     char style,
                                     unsigned precision,
                                     uint32_t guest_format,
                                     int fault_on_error)
{
    char text[384];
    double value;
    int length;
    int i;

    memcpy(&value, &raw, sizeof value);
    length = isaac_vita_musl_format_double(
        text, sizeof text, value, style, precision);
    if (length < 0 || (unsigned)length >= sizeof text) {
        if (fault_on_error)
            guest_fault(c, guest_format,
                        "guest stdio double conversion overflow");
        return 0;
    }
    for (i = 0; i < length; ++i)
        vita_crt_stdio_put(out, (uint8_t)text[i]);
    return 1;
}

static int vita_crt_stdio_format(CPU *__restrict c, uint32_t format,
                                 uint32_t arguments,
                                 vita_crt_format_output *out,
                                 int fault_on_error)
{
    for (;;) {
        uint8_t value = ld8(format++);
        uint32_t guest_format;
        if (!value)
            return 1;
        if (value != '%') {
            vita_crt_stdio_put(out, value);
            continue;
        }
        guest_format = format - 1U;
        value = ld8(format++);
        if (value == '%') {
            vita_crt_stdio_put(out, '%');
        } else if (value == 's') {
            vita_crt_stdio_put_string(out, ld32(arguments));
            arguments += 4U;
        } else if (value == 'd') {
            vita_crt_stdio_put_i32_width(
                out, ld32(arguments), 0U, ' ', 0);
            arguments += 4U;
        } else if (value == 'X' || value == 'x') {
            vita_crt_stdio_put_u64_hex_width(
                out, ld32(arguments), value == 'X', 0U);
            arguments += 4U;
        } else if (value == 'u') {
            vita_crt_stdio_put_u32_width(out, ld32(arguments), 0U, ' ');
            arguments += 4U;
        } else if (value == 'l' && ld8(format) == 'd') {
            ++format;
            vita_crt_stdio_put_i32_width(
                out, ld32(arguments), 0U, ' ', 0);
            arguments += 4U;
        } else if (value == '0' && ld8(format) == '8' &&
                   ld8(format + 1U) == 'x') {
            format += 2U;
            vita_crt_stdio_put_u64_hex_width(
                out, ld32(arguments), 0, 8U);
            arguments += 4U;
        } else if (value == '0' &&
                   (ld8(format) == '2' || ld8(format) == '3' ||
                    ld8(format) == '4') && ld8(format + 1U) == 'u') {
            unsigned width = (unsigned)(ld8(format) - '0');
            format += 2U;
            vita_crt_stdio_put_u32_width(
                out, ld32(arguments), width, '0');
            arguments += 4U;
        } else if ((value == '2' || value == '4') &&
                   ld8(format) == 'd') {
            unsigned width = (unsigned)(value - '0');
            ++format;
            vita_crt_stdio_put_i32_width(
                out, ld32(arguments), width, ' ', 0);
            arguments += 4U;
        } else if (value == '0' &&
                   (ld8(format) == '2' || ld8(format) == '3') &&
                   ld8(format + 1U) == 'd') {
            unsigned width = (unsigned)(ld8(format) - '0');
            format += 2U;
            vita_crt_stdio_put_i32_width(
                out, ld32(arguments), width, '0', 0);
            arguments += 4U;
        } else if (value == '.' && ld8(format) == '1' &&
                   ld8(format + 1U) == '6' &&
                   ld8(format + 2U) == 's') {
            format += 3U;
            vita_crt_stdio_put_string_limited(
                out, ld32(arguments), 16U);
            arguments += 4U;
        } else if (value == ' ' && ld8(format) == 'd') {
            ++format;
            vita_crt_stdio_put_i32_width(
                out, ld32(arguments), 0U, ' ', 1);
            arguments += 4U;
        } else if (value == 'f' || value == 'g' || value == '.') {
            uint64_t raw;
            char style = '\0';
            unsigned precision = 0U;
            if (value == 'f') {
                style = 'f';
                precision = 6U;
            } else if (value == 'g') {
                style = 'g';
                precision = 6U;
            } else if (ld8(format) == 'f') {
                ++format;
                style = 'f';
                precision = 0U;
            } else if (ld8(format) == '1' && ld8(format + 1U) == 'f') {
                format += 2U;
                style = 'f';
                precision = 1U;
            } else if (ld8(format) == '2' && ld8(format + 1U) == 'f') {
                format += 2U;
                style = 'f';
                precision = 2U;
            } else if (ld8(format) == '4' && ld8(format + 1U) == 'f') {
                format += 2U;
                style = 'f';
                precision = 4U;
            }
            if (!style) {
                if (fault_on_error)
                    guest_fault(c, guest_format,
                                "guest stdio unsupported format");
                return 0;
            }
            raw = (uint64_t)ld32(arguments) |
                  ((uint64_t)ld32(arguments + 4U) << 32);
            arguments += 8U;
            if (!vita_crt_stdio_put_double(
                    c, out, raw, style, precision, guest_format,
                    fault_on_error))
                return 0;
        } else if (value == 'l' && ld8(format) == 'l' &&
                   ld8(format + 1U) == 'u') {
            uint64_t integer = (uint64_t)ld32(arguments) |
                               ((uint64_t)ld32(arguments + 4U) << 32);
            format += 2U;
            arguments += 8U;
            vita_crt_stdio_put_u64(out, integer);
        } else if (value == '0' && ld8(format) == '1' &&
                   ld8(format + 1U) == '6' &&
                   ld8(format + 2U) == 'l' &&
                   ld8(format + 3U) == 'l' &&
                   ld8(format + 4U) == 'x') {
            uint64_t integer = (uint64_t)ld32(arguments) |
                               ((uint64_t)ld32(arguments + 4U) << 32);
            format += 5U;
            arguments += 8U;
            vita_crt_stdio_put_u64_hex_width(out, integer, 0, 16U);
        } else if (value == '0' && ld8(format) == '8' &&
                   ld8(format + 1U) == 'l' &&
                   ld8(format + 2U) == 'l' &&
                   ld8(format + 3U) == 'x') {
            uint64_t integer = (uint64_t)ld32(arguments) |
                               ((uint64_t)ld32(arguments + 4U) << 32);
            format += 4U;
            arguments += 8U;
            vita_crt_stdio_put_u64_hex_width(out, integer, 0, 8U);
        } else {
            if (fault_on_error)
                guest_fault(c, guest_format,
                            "guest stdio unsupported format");
            return 0;
        }
    }
}

#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
/* Observe only the completed, non-truncated frozen logger invocation.  These
 * are pure checks: unlike guest_stack_address they cannot fault or change the
 * guest stack low-water mark.  The ordinary formatter has already consumed
 * both %d arguments.  Its fixed output capacity is disjoint from the checked
 * stack ranges, so rereading those two words cannot see an output overwrite.
 * Do not read the descriptor, Room object, name string or formatted output. */
static void vita_crt_room_log_completed(
    CPU *__restrict c, uint32_t buffer, uint32_t count, uint32_t format,
    uint32_t arguments, uint32_t written, int standard)
{
    uint32_t frame;
    int saved_errno;

    if (format != ISAAC_VITA_CRT_ROOM_LOG_FORMAT_VA || !standard ||
        buffer < ISAAC_VITA_CRT_ROOM_LOG_BUFFER_VA ||
        buffer >= ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END ||
        count != ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END - buffer ||
        written >= count || !guest_stack_contains(c, c->esp, 4U) ||
        ld32(c->esp) != ISAAC_VITA_CRT_VSPRINTF_TIMER_RETURN_RVA ||
        c->ebp > UINT32_MAX - 24U ||
        !guest_stack_contains(c, c->ebp, 24U))
        return;
    frame = c->ebp;
    if (arguments != frame + 16U ||
        !(frame + 24U <= buffer || frame >= ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END) ||
        !(c->esp + 4U <= buffer || c->esp >= ISAAC_VITA_CRT_ROOM_LOG_BUFFER_END) ||
        ld32(frame + 4U) != ISAAC_VITA_CRT_ROOM_LOG_ORIGIN_RETURN_RVA ||
        ld32(frame + 8U) != 0U || ld32(frame + 12U) != format ||
        memcmp((const void *)(uintptr_t)format, ISAAC_VITA_CRT_ROOM_LOG_FORMAT,
               sizeof ISAAC_VITA_CRT_ROOM_LOG_FORMAT) != 0)
        return;
    saved_errno = errno;
    kage_vita_phase_profile_room_log(ld32(arguments), ld32(arguments + 4U));
    errno = saved_errno;
}
#endif

static void vita_crt_stdio_common_vsprintf(CPU *__restrict c)
{
    uint32_t buffer = vita_crt_arg(c, 2U);
    uint32_t count = vita_crt_arg(c, 3U);
    uint32_t format = vita_crt_arg(c, 4U);
    uint32_t arguments = vita_crt_arg(c, 6U);
    uint32_t options = vita_crt_arg(c, 0U);
    int standard = (options & 2U) != 0;
    uint32_t limit = standard ? (count ? count - 1U : 0U) : count;
    vita_crt_format_output out;

    (void)vita_crt_arg(c, 1U);
    (void)vita_crt_arg(c, 5U);
    if (!format || (!buffer && count)) {
        c->eax = UINT32_MAX;
        vita_crt_cdecl_return(c);
        return;
    }
    out.buffer = buffer;
    out.limit = limit;
    out.written = 0U;
    out.stream = NULL;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    out.async_image = NULL;
#endif
    if (!vita_crt_stdio_format(c, format, arguments, &out, 1))
        return;
    if (!buffer) {
        c->eax = out.written;
    } else if (standard) {
        if (count)
            st8(buffer + (out.written < limit ? out.written : limit), 0U);
        c->eax = out.written;
    } else {
        if (out.written < count)
            st8(buffer + out.written, 0U);
        c->eax = out.written <= count ? out.written : UINT32_MAX;
    }
#if defined(ISAAC_VITA_ROOM_LOG_MARKERS) && ISAAC_VITA_ROOM_LOG_MARKERS
    vita_crt_room_log_completed(
        c, buffer, count, format, arguments, out.written, standard);
#endif
    vita_crt_cdecl_return(c);
}

/* Secure sibling copied from the exercised host_win32 implementation.  It
 * shares the seven-dword x86 ABI and guest-va_list formatter above, requires
 * a real destination, and clears that destination instead of exposing a
 * partial string when the formatted output does not fit. */
static void vita_crt_stdio_common_vsprintf_s(CPU *__restrict c)
{
    uint32_t buffer = vita_crt_arg(c, 2U);
    uint32_t count = vita_crt_arg(c, 3U);
    uint32_t format = vita_crt_arg(c, 4U);
    uint32_t arguments = vita_crt_arg(c, 6U);
    vita_crt_format_output out;

    (void)vita_crt_arg(c, 0U);
    (void)vita_crt_arg(c, 1U);
    (void)vita_crt_arg(c, 5U);
    if (!buffer || !count || !format) {
        if (buffer && count)
            st8(buffer, 0U);
        c->eax = UINT32_MAX;
        vita_crt_cdecl_return(c);
        return;
    }
    out.buffer = buffer;
    out.limit = count - 1U;
    out.written = 0U;
    out.stream = NULL;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    out.async_image = NULL;
#endif
    if (!vita_crt_stdio_format(c, format, arguments, &out, 1))
        return;
    if (out.written >= count) {
        st8(buffer, 0U);
        c->eax = UINT32_MAX;
    } else {
        st8(buffer + out.written, 0U);
        c->eax = out.written;
    }
    vita_crt_cdecl_return(c);
}

/*
 * int __cdecl __stdio_common_vsscanf(
 *     uint64_t options, const char *buffer, size_t buffer_count,
 *     const char *format, _locale_t locale, va_list arguments);
 *
 * This is the complete scanf-family imported by the frozen PE: its sole IAT
 * slot is reached through the ordinary sscanf adapter at RVA 0x003f0ef0.
 * Four statically measured callers use the three admitted narrow formats:
 *
 *     ISAACNG_GSR%u       two shader-register call sites
 *     %i.%i               OpenGL version parsing
 *     %d.%d.%d            GLFW/WGL version parsing
 *
 * Keep the x86 va_list guest-addressed.  Passing it to a native ARM variadic
 * function would be an ABI bug.  A new format, finite input count, locale or
 * option bit remains a loud frontier instead of silently changing meaning. */
static int vita_crt_scan_guest_string_equals(uint32_t guest,
                                              const char *text)
{
    if (!guest || !text)
        return 0;
    for (;;) {
        uint8_t actual = ld8(guest++);
        uint8_t expected = (uint8_t)*text++;
        if (actual != expected)
            return 0;
        if (!actual)
            return 1;
    }
}

static int vita_crt_scan_space(uint8_t value)
{
    return value == ' ' || value == '\t' || value == '\n' ||
           value == '\r' || value == '\v' || value == '\f';
}

static int vita_crt_scan_digit(uint8_t value)
{
    if (value >= '0' && value <= '9')
        return (int)(value - '0');
    if (value >= 'a' && value <= 'f')
        return (int)(value - 'a') + 10;
    if (value >= 'A' && value <= 'F')
        return (int)(value - 'A') + 10;
    return -1;
}

/* Return 1 for a conversion, 0 for a matching failure, and -1 after a loud
 * range fault.  Every admitted conversion stores one 32-bit guest value. */
static int vita_crt_scan_integer(CPU *__restrict c, uint32_t *cursor,
                                 int requested_base, int signed_result,
                                 uint32_t *result)
{
    uint32_t p = *cursor;
    uint32_t value = 0U;
    uint32_t limit;
    unsigned digits = 0U;
    int negative = 0;
    int base = requested_base;

    while (vita_crt_scan_space(ld8(p)))
        ++p;
    if (ld8(p) == '+' || ld8(p) == '-') {
        negative = ld8(p) == '-';
        ++p;
    }
    if (base == 0) {
        if (ld8(p) == '0') {
            int after_prefix = vita_crt_scan_digit(ld8(p + 2U));
            if ((ld8(p + 1U) == 'x' || ld8(p + 1U) == 'X') &&
                after_prefix >= 0 && after_prefix < 16) {
                base = 16;
                p += 2U;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    }
    limit = signed_result
        ? (negative ? UINT32_C(0x80000000) : UINT32_C(0x7fffffff))
        : UINT32_MAX;
    for (;;) {
        int digit = vita_crt_scan_digit(ld8(p));
        if (digit < 0 || digit >= base)
            break;
        if (value > (limit - (uint32_t)digit) / (uint32_t)base) {
            guest_fault(c, p,
                        "guest stdio scanned integer outside 32-bit range");
            return -1;
        }
        value = value * (uint32_t)base + (uint32_t)digit;
        ++p;
        ++digits;
    }
    if (!digits)
        return 0;
    *cursor = p;
    *result = negative ? 0U - value : value;
    return 1;
}

static int vita_crt_scan_store_integer(CPU *__restrict c,
                                       uint32_t *cursor,
                                       uint32_t *arguments, int base,
                                       int signed_result)
{
    uint32_t value;
    uint32_t destination;
    int converted = vita_crt_scan_integer(c, cursor, base, signed_result,
                                          &value);

    if (converted <= 0)
        return converted;
    destination = ld32(*arguments);
    if (!destination) {
        guest_fault(c, *arguments, "guest stdio scan has a null destination");
        return -1;
    }
    *arguments += 4U;
    st32(destination, value);
    return 1;
}

static int vita_crt_scan_literal(uint32_t *cursor, const char *literal)
{
    uint32_t p = *cursor;

    while (*literal) {
        if (ld8(p) != (uint8_t)*literal++)
            return 0;
        ++p;
    }
    *cursor = p;
    return 1;
}

static void vita_crt_stdio_common_vsscanf(CPU *__restrict c)
{
    const uint32_t legacy_wide_specifiers = 2U;
    uint32_t options_low = vita_crt_arg(c, 0U);
    uint32_t options_high = vita_crt_arg(c, 1U);
    uint32_t cursor = vita_crt_arg(c, 2U);
    uint32_t buffer_count = vita_crt_arg(c, 3U);
    uint32_t format = vita_crt_arg(c, 4U);
    uint32_t locale = vita_crt_arg(c, 5U);
    uint32_t arguments = vita_crt_arg(c, 6U);
    uint32_t assignments = 0U;
    int converted;

    if ((options_low & ~legacy_wide_specifiers) || options_high) {
        guest_fault(c, options_low ? options_low : options_high,
                    "guest stdio unsupported vsscanf option bits");
        return;
    }
    if (buffer_count != UINT32_MAX) {
        guest_fault(c, buffer_count,
                    "guest stdio unsupported finite scan count");
        return;
    }
    if (locale) {
        guest_fault(c, locale, "guest stdio unsupported scan locale");
        return;
    }
    if (!cursor || !format || !arguments) {
        guest_fault(c, format,
                    "guest stdio vsscanf received a null pointer");
        return;
    }

    if (vita_crt_scan_guest_string_equals(format, "ISAACNG_GSR%u")) {
        if (vita_crt_scan_literal(&cursor, "ISAACNG_GSR")) {
            converted = vita_crt_scan_store_integer(c, &cursor, &arguments,
                                                    10, 0);
            if (converted < 0)
                return;
            assignments += (uint32_t)converted;
        }
    } else if (vita_crt_scan_guest_string_equals(format, "%i.%i")) {
        converted = vita_crt_scan_store_integer(c, &cursor, &arguments,
                                                0, 1);
        if (converted < 0)
            return;
        assignments += (uint32_t)converted;
        if (converted && vita_crt_scan_literal(&cursor, ".")) {
            converted = vita_crt_scan_store_integer(c, &cursor, &arguments,
                                                    0, 1);
            if (converted < 0)
                return;
            assignments += (uint32_t)converted;
        }
    } else if (vita_crt_scan_guest_string_equals(format, "%d.%d.%d")) {
        converted = vita_crt_scan_store_integer(c, &cursor, &arguments,
                                                10, 1);
        if (converted < 0)
            return;
        assignments += (uint32_t)converted;
        if (converted && vita_crt_scan_literal(&cursor, ".")) {
            converted = vita_crt_scan_store_integer(c, &cursor, &arguments,
                                                    10, 1);
            if (converted < 0)
                return;
            assignments += (uint32_t)converted;
            if (converted && vita_crt_scan_literal(&cursor, ".")) {
                converted = vita_crt_scan_store_integer(
                    c, &cursor, &arguments, 10, 1);
                if (converted < 0)
                    return;
                assignments += (uint32_t)converted;
            }
        }
    } else {
        guest_fault(c, format, "guest stdio unsupported scan format");
        return;
    }

    c->eax = assignments;
    vita_crt_cdecl_return(c);
}

static void vita_crt_file_lock(void)
{
    while (__sync_lock_test_and_set(&s_file_lock, 1U)) {
        while (s_file_lock)
            VITA_CRT_SPIN_HINT();
    }
    __sync_synchronize();
}

static void vita_crt_file_unlock(void)
{
    __sync_synchronize();
    __sync_lock_release(&s_file_lock);
}

#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
int isaac_vita_crt_file_lookup_hint_get(uint32_t out[2])
{
    if (!out || __sync_lock_test_and_set(&s_file_lock, 1U))
        return 0;
    __sync_synchronize();
    out[0] = s_file_lookup_hits;
    out[1] = s_file_lookup_misses;
    vita_crt_file_unlock();
    return 1;
}
#endif

#undef VITA_CRT_SPIN_HINT

#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
static void vita_crt_seek_shadow_order(uint32_t *order, const uint32_t *count)
{
    uint32_t i;
    uint32_t j;

    for (i = 0U; i < VITA_CRT_SEEK_SHADOW_TOP; ++i)
        order[i] = i;
    for (i = 0U; i < VITA_CRT_SEEK_SHADOW_TOP; ++i) {
        for (j = i + 1U; j < VITA_CRT_SEEK_SHADOW_TOP; ++j) {
            if (count[order[j]] > count[order[i]]) {
                uint32_t swap = order[i];
                order[i] = order[j];
                order[j] = swap;
            }
        }
    }
}

/* Two bounded lines (the durable logger keeps 383 body bytes): counters,
 * then the four hottest attribution keys and file names.  Never under
 * s_file_lock while logging. */
void isaac_vita_crt_seek_shadow_report(const char *why)
{
    vita_crt_seek_shadow_stats snapshot;
    uint32_t ret_count[VITA_CRT_SEEK_SHADOW_TOP];
    uint32_t name_count[VITA_CRT_SEEK_SHADOW_TOP];
    uint32_t ret_order[VITA_CRT_SEEK_SHADOW_TOP];
    uint32_t name_order[VITA_CRT_SEEK_SHADOW_TOP];
    uint32_t live = 0U;
    uint32_t generation;
    uint32_t index;
    int saved_errno = errno;

    vita_crt_file_lock();
    ++s_seek_shadow.reports;
    s_seek_shadow_since_report = 0U;
    snapshot = s_seek_shadow;
    generation = s_seek_shadow_write_gen;
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        if (vita_crt_dynamic_file_is_live(&s_file_tokens[index]) &&
            vita_crt_seek_shadow_eligible(
                &s_file_tokens[index],
                vita_crt_dynamic_stream(&s_file_tokens[index])))
            ++live;
    }
    vita_crt_file_unlock();
    for (index = 0U; index < VITA_CRT_SEEK_SHADOW_TOP; ++index) {
        ret_count[index] = snapshot.top_ret[index].count;
        name_count[index] = snapshot.top_name[index].count;
        snapshot.top_name[index].name[
            sizeof snapshot.top_name[index].name - 1U] = '\0';
    }
    vita_crt_seek_shadow_order(ret_order, ret_count);
    vita_crt_seek_shadow_order(name_order, name_count);
    isaac_vita_log(
        "KAGE VITA CRT SEEK SHADOW: why=%s n=%u seek_end=%u elided=%u "
        "real_end=%u fstat=%u fstat_fail=%u restat=%u applied=%u "
        "apply_fail=%u ftell_pending=%u set_pending=%u cur_pending=%u "
        "rearmed=%u other_end=%u gen=%u live=%u",
        why, (unsigned)snapshot.reports, (unsigned)snapshot.seek_end,
        (unsigned)snapshot.elided, (unsigned)snapshot.real_end,
        (unsigned)snapshot.fstat_calls, (unsigned)snapshot.fstat_fail,
        (unsigned)snapshot.restat, (unsigned)snapshot.applied,
        (unsigned)snapshot.apply_fail, (unsigned)snapshot.ftell_pending,
        (unsigned)snapshot.set_pending, (unsigned)snapshot.cur_pending,
        (unsigned)snapshot.rearmed, (unsigned)snapshot.other_end,
        (unsigned)generation, (unsigned)live);
    isaac_vita_log(
        "KAGE VITA CRT SEEK SHADOW TOP: why=%s "
        "ret=%08x:%u,%08x:%u,%08x:%u,%08x:%u "
        "name=%s:%u,%s:%u,%s:%u,%s:%u",
        why,
        (unsigned)snapshot.top_ret[ret_order[0]].key,
        (unsigned)snapshot.top_ret[ret_order[0]].count,
        (unsigned)snapshot.top_ret[ret_order[1]].key,
        (unsigned)snapshot.top_ret[ret_order[1]].count,
        (unsigned)snapshot.top_ret[ret_order[2]].key,
        (unsigned)snapshot.top_ret[ret_order[2]].count,
        (unsigned)snapshot.top_ret[ret_order[3]].key,
        (unsigned)snapshot.top_ret[ret_order[3]].count,
        snapshot.top_name[name_order[0]].name,
        (unsigned)snapshot.top_name[name_order[0]].count,
        snapshot.top_name[name_order[1]].name,
        (unsigned)snapshot.top_name[name_order[1]].count,
        snapshot.top_name[name_order[2]].name,
        (unsigned)snapshot.top_name[name_order[2]].count,
        snapshot.top_name[name_order[3]].name,
        (unsigned)snapshot.top_name[name_order[3]].count);
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    isaac_vita_crt_descriptor_recover_report(why);
#endif
    errno = saved_errno;
}
#endif

#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
/* One bounded counter line.  Never under s_file_lock while logging. */
void isaac_vita_crt_descriptor_recover_report(const char *why)
{
    vita_crt_descriptor_recover_stats snapshot;
    uint32_t retained = 0U;
    uint32_t index;
    int saved_errno = errno;

    vita_crt_file_lock();
    snapshot = s_descriptor_recover;
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        if (vita_crt_dynamic_file_is_live(&s_file_tokens[index]) &&
            s_file_tokens[index].recover.path[0])
            ++retained;
    }
    vita_crt_file_unlock();
    isaac_vita_log(
        "KAGE VITA CRT DESCRIPTOR RECOVER: why=%s enodev=%u recovered=%u "
        "unknown_pos=%u reopen_fail=%u redo_fail=%u retained=%u",
        why, (unsigned)snapshot.enodev, (unsigned)snapshot.recovered,
        (unsigned)snapshot.unknown_pos, (unsigned)snapshot.reopen_fail,
        (unsigned)snapshot.redo_fail, (unsigned)retained);
    errno = saved_errno;
}
#endif

#if defined(ISAAC_VITA_GAME_LOG_BATCH)
enum vita_crt_log_batch_decision {
    VITA_CRT_LOG_BATCH_NATIVE = 0,
    VITA_CRT_LOG_BATCH_DEFER = 1
};

static void vita_crt_log_batch_increment(uint32_t *value)
{
    if (*value != UINT32_MAX)
        ++*value;
}

/* Caller owns s_file_lock.  The measured adapter has no EBP prologue:
 * imported fflush therefore sees its own return at ESP, the logger return at
 * ESP+8, and the logger's severity argument at EBP+8.  Inspect those values
 * only after the ordinary FILE-token boundary has accepted the stream. */
static enum vita_crt_log_batch_decision
vita_crt_log_batch_classify_locked(CPU *__restrict c, uint32_t token,
                                   int *exact_logger)
{
    uint32_t severity_address;
    uint32_t severity;

    *exact_logger = 0;
    if (!guest_stack_contains(c, c->esp, 12U) ||
        ld32(c->esp) != ISAAC_VITA_CRT_FFLUSH_FIRST_RETURN_RVA)
        return VITA_CRT_LOG_BATCH_NATIVE;
    if (!token || ld32(c->esp + 8U) !=
            ISAAC_VITA_CRT_FFLUSH_FIRST_ORIGIN_RETURN_RVA ||
        c->ebp > UINT32_MAX - 8U) {
        vita_crt_log_batch_increment(&s_log_batch.chain_rejects);
        return VITA_CRT_LOG_BATCH_NATIVE;
    }
    severity_address = c->ebp + 8U;
    if (!guest_stack_contains(c, severity_address, 4U)) {
        vita_crt_log_batch_increment(&s_log_batch.chain_rejects);
        return VITA_CRT_LOG_BATCH_NATIVE;
    }
    if (!s_log_batch_token)
        s_log_batch_token = token;
    else if (s_log_batch_token != token) {
        vita_crt_log_batch_increment(&s_log_batch.chain_rejects);
        return VITA_CRT_LOG_BATCH_NATIVE;
    }

    *exact_logger = 1;
    severity = ld32(severity_address) & 3U;
    if (severity == 0U) {
        vita_crt_log_batch_increment(&s_log_batch.info_seen);
        if (s_log_batch.sticky_fail_open)
            return VITA_CRT_LOG_BATCH_NATIVE;
        if (s_log_batch.pending_info <
                ISAAC_VITA_CRT_LOG_BATCH_SIZE - 1U) {
            ++s_log_batch.pending_info;
            vita_crt_log_batch_increment(&s_log_batch.info_deferred);
            if (s_log_batch.max_deferred < s_log_batch.pending_info)
                s_log_batch.max_deferred = s_log_batch.pending_info;
            return VITA_CRT_LOG_BATCH_DEFER;
        }
        ++s_log_batch.pending_info;
        vita_crt_log_batch_increment(&s_log_batch.info_batch_flushes);
    } else if (severity == 1U) {
        vita_crt_log_batch_increment(&s_log_batch.warn_forwarded);
    } else if (severity == 2U) {
        vita_crt_log_batch_increment(&s_log_batch.error_forwarded);
    } else {
        vita_crt_log_batch_increment(&s_log_batch.assert_forwarded);
    }
    return VITA_CRT_LOG_BATCH_NATIVE;
}

static int vita_crt_log_batch_tracks_flush_locked(uint32_t token,
                                                   int exact_logger)
{
    return exact_logger || (!token && s_log_batch_token) ||
           (token && token == s_log_batch_token);
}

static void vita_crt_log_batch_native_result_locked(int tracked, int result)
{
    if (!tracked)
        return;
    vita_crt_log_batch_increment(&s_log_batch.native_flush_calls);
    if (result == EOF) {
        vita_crt_log_batch_increment(&s_log_batch.native_failures);
        s_log_batch.sticky_fail_open = 1U;
    } else {
        s_log_batch.pending_info = 0U;
    }
}

_Static_assert(ISAAC_VITA_CRT_LOG_BATCH_SIZE == 8U &&
               sizeof(isaac_vita_crt_log_batch_snapshot) == 14U * 4U,
               "Vita logger batch ABI/policy drifted");
#endif

#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
static int vita_crt_archive_diag_relevant_key(const char *key)
{
    return key && (strcmp(key, "afterbirth.a") == 0 ||
                   strcmp(key, "afterbirthp.a") == 0);
}

static void vita_crt_archive_diag_log_event(
    const char *reason, const isaac_vita_archive_diag_event *event)
{
    char line[ISAAC_VITA_ARCHIVE_DIAG_LOG_CAPACITY];
    int length;

    /* platform.c retains at most 383 body bytes.  Bound every string field
     * here so the fixed numeric tail, including the observed header word,
     * can never be silently truncated by the durable logger. */
    length = isaac_vita_archive_diag_format_event(
        line, sizeof line, ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID,
        reason, event);
    if (length < 0 || (unsigned)length >= sizeof line) {
        isaac_vita_log(
            "arcdiag v1 tag=archive-cursor-v1 bid_prefix=%.40s "
            "why=format-overflow seq=%u",
            ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID, (unsigned)event->sequence);
        return;
    }
    isaac_vita_log("%s", line);
}
#endif

void isaac_vita_crt_archive_diag_dump(void)
{
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    isaac_vita_archive_diag_event
        events[ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY];
    uint32_t latest_sequence = 0U;
    uint32_t count;
    uint32_t index;
    int saved_errno = errno;

    vita_crt_file_lock();
    count = isaac_vita_archive_diag_snapshot(
        events, ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY, &latest_sequence);
    vita_crt_file_unlock();
    isaac_vita_log(
        "arcdiag v1 tag=archive-cursor-v1 bid_prefix=%.40s "
        "snapshot=%u latest=%u cap=%u",
        ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID,
        (unsigned)count, (unsigned)latest_sequence,
        (unsigned)ISAAC_VITA_ARCHIVE_DIAG_RING_CAPACITY);
    for (index = 0U; index < count; ++index)
        vita_crt_archive_diag_log_event("snapshot", &events[index]);
    errno = saved_errno;
#endif
}

int isaac_vita_crt_flush_all(void)
{
    int result;
    int flush_errno;
    int saved_errno = errno;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    int tracked;
#endif

    vita_crt_file_lock();
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    tracked = vita_crt_log_batch_tracks_flush_locked(0U, 0);
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    vita_crt_seek_shadow_note_write();
#endif
    errno = 0;
    result = fflush(NULL);
    flush_errno = errno;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    vita_crt_log_batch_native_result_locked(tracked, result);
#endif
    vita_crt_file_unlock();
    if (result == EOF)
        g_isaac_vita_crt_errno = flush_errno ? flush_errno : EIO;
    errno = saved_errno;
    return result;
}

int isaac_vita_crt_log_batch_get_snapshot(
    isaac_vita_crt_log_batch_snapshot *snapshot)
{
    if (!snapshot)
        return 0;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    vita_crt_file_lock();
    *snapshot = s_log_batch;
    vita_crt_file_unlock();
    return 1;
#else
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->abi_version = ISAAC_VITA_CRT_LOG_BATCH_ABI;
    return 0;
#endif
}

#if defined(ISAAC_VITA_IO_PROFILE)
static void vita_crt_io_profile_snapshot_locked(
    isaac_vita_crt_io_profile_snapshot *snapshot)
{
    uint32_t index;

    *snapshot = s_io_profile;
    snapshot->live_files = 0U;
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index) {
        if (vita_crt_dynamic_file_is_live(&s_file_tokens[index]))
            ++snapshot->live_files;
    }
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    {
        isaac_vita_archive_diag_event event;

        _Static_assert(
            ISAAC_VITA_CRT_IO_ARCHIVE_KEY_CAPACITY ==
                ISAAC_VITA_ARCHIVE_CACHE_KEY_CAPACITY,
            "profile/archive diagnostic key capacities must match");
        if (isaac_vita_archive_diag_snapshot(&event, 1U, NULL) == 1U) {
            snapshot->archive_sequence = event.sequence;
            snapshot->archive_kind = event.kind;
            snapshot->archive_flags = event.flags;
            snapshot->archive_position_before = event.position_before;
            snapshot->archive_position_after = event.position_after;
            memcpy(snapshot->archive_key, event.key,
                   sizeof snapshot->archive_key);
            snapshot->archive_key[sizeof snapshot->archive_key - 1U] = '\0';
        }
    }
#endif
}

int isaac_vita_crt_io_profile_get_snapshot(
    isaac_vita_crt_io_profile_snapshot *snapshot)
{
    int active;

    if (!snapshot)
        return 0;
    vita_crt_file_lock();
    active = s_io_profile_active ? 1 : 0;
    vita_crt_io_profile_snapshot_locked(snapshot);
    vita_crt_file_unlock();
    return active;
}

int isaac_vita_crt_io_profile_stop_and_snapshot(
    isaac_vita_crt_io_profile_snapshot *snapshot)
{
    int active;

    if (!snapshot)
        return 0;
    vita_crt_file_lock();
    active = s_io_profile_active ? 1 : 0;
    s_io_profile_active = 0U;
    vita_crt_io_profile_snapshot_locked(snapshot);
    vita_crt_file_unlock();
    return active;
}

void isaac_vita_crt_io_profile_begin(void)
{
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    uint32_t index;
#endif

    vita_crt_file_lock();
    memset(&s_io_profile, 0, sizeof s_io_profile);
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    vita_crt_io_shadow_clear(&s_io_shadow_idle);
    for (index = 0U; index < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++index)
        vita_crt_io_shadow_clear(&s_file_tokens[index].io_shadow);
#endif
    s_io_profile_active = 1U;
    vita_crt_file_unlock();
}
#endif

void isaac_vita_crt_async_write_shutdown(void)
{
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    int saved_errno = errno;

    /* Never under s_file_lock: the drain may block on the writer thread and
     * the guest thread is the only lock owner anyway. */
    isaac_vita_async_write_shutdown();
    errno = saved_errno;
#endif
}

void isaac_vita_crt_archive_cache_shutdown(void)
{
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    int saved_errno = errno;

    vita_crt_file_lock();
    (void)isaac_vita_archive_cache_drop();
#if defined(ISAAC_VITA_IO_PROFILE)
    vita_crt_io_shadow_clear(&s_io_shadow_idle);
#endif
    vita_crt_file_unlock();
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    /* The seek shadow report also emits the descriptor-recover line. */
    isaac_vita_crt_seek_shadow_report("shutdown");
#elif defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    isaac_vita_crt_descriptor_recover_report("shutdown");
#endif
    errno = saved_errno;
#endif
}

static FILE *vita_crt_standard_stream(uint32_t index)
{
    if (index == 0U)
        return stdin;
    if (index == 1U)
        return stdout;
    if (index == 2U)
        return stderr;
    return NULL;
}

/* Caller owns s_file_lock.  Standard streams live outside the bounded
 * dynamic fopen ledger, so exposing stderr cannot consume a game-file slot. */
static int vita_crt_find_file_token_unlocked(uint32_t token, FILE **stream,
                                             int *dynamic_index,
                                             int *standard_index)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_STANDARD_STREAM_COUNT; ++i) {
        FILE *standard = vita_crt_standard_stream(i);
        if ((uint32_t)(uintptr_t)standard == token) {
            if (stream)
                *stream = standard;
            if (dynamic_index)
                *dynamic_index = -1;
            if (standard_index)
                *standard_index = (int)i;
            return 1;
        }
    }
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
    i = s_file_lookup_hint;
    if (i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT &&
        vita_crt_dynamic_file_is_live(&s_file_tokens[i]) &&
        s_file_tokens[i].token == token) {
        if (stream)
            *stream = vita_crt_dynamic_stream(&s_file_tokens[i]);
        if (dynamic_index)
            *dynamic_index = (int)i;
        if (standard_index)
            *standard_index = -1;
        if (s_file_lookup_hits != UINT32_MAX)
            ++s_file_lookup_hits;
        return 1;
    }
    if (s_file_lookup_misses != UINT32_MAX)
        ++s_file_lookup_misses;
#endif
    for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i) {
        FILE *dynamic = vita_crt_dynamic_stream(&s_file_tokens[i]);
        if (vita_crt_dynamic_file_is_live(&s_file_tokens[i]) &&
            s_file_tokens[i].token == token) {
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
            s_file_lookup_hint = i;
#endif
            if (stream)
                *stream = dynamic;
            if (dynamic_index)
                *dynamic_index = (int)i;
            if (standard_index)
                *standard_index = -1;
            return 1;
        }
    }
    return 0;
}

static int vita_crt_fopen_mode_valid(const char *mode)
{
    int saw_binary = 0;
    int saw_update = 0;
    uint32_t i;

    if (!mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a'))
        return 0;
    for (i = 1U; mode[i]; ++i) {
        if (mode[i] == 'b' && !saw_binary)
            saw_binary = 1;
        else if (mode[i] == '+' && !saw_update)
            saw_update = 1;
        else
            return 0;
    }
    return i <= 3U;
}

static void vita_crt_file_errno_restore(int saved_errno, int failed,
                                        int fallback)
{
    if (failed)
        g_isaac_vita_crt_errno = errno ? errno : fallback;
    errno = saved_errno;
}

#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
/* Caller owns s_file_lock.  Returns 1 when the open was completed (token
 * published or fault raised) on the memory-image lane; 0 hands the path to
 * the ordinary native fopen with the lock still held. */
static int vita_crt_fopen_async_write_locked(CPU *__restrict c,
                                             const char *native_path,
                                             const char *mode,
                                             int saved_errno)
{
    uint32_t token = 0U;
    uint32_t i;

    if (!isaac_vita_async_write_eligible(native_path, mode))
        return 0;
    for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i) {
        if (!vita_crt_dynamic_file_is_live(&s_file_tokens[i]))
            break;
    }
    /* A full table keeps the exact native "table full" fault path. */
    if (i == ISAAC_VITA_CRT_FILE_TOKEN_COUNT)
        return 0;
    if (!isaac_vita_async_write_image_open(
            &s_file_tokens[i].async_write, native_path))
        return 0;
    if (!vita_crt_guest_pointer(c, &s_file_tokens[i], &token)) {
        /* The image opened but its slot has no guest-visible address.  Surface
         * this exactly like a native fopen failure -- a NULL FILE with a CRT
         * errno -- and release the lane, instead of returning to the guest
         * with a stale EAX and an unbalanced stack. */
        s_file_tokens[i].async_write.error = 1U;
        (void)isaac_vita_async_write_image_close(
            &s_file_tokens[i].async_write);
        vita_crt_file_unlock();
        vita_crt_file_errno_restore(saved_errno, 1, EIO);
        c->eax = 0U;
        vita_crt_cdecl_return(c);
        return 1;
    }
    s_file_tokens[i].token = token;
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    vita_crt_recover_clear(&s_file_tokens[i]);
#endif
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
    s_file_lookup_hint = ISAAC_VITA_CRT_FILE_TOKEN_COUNT;
#endif
    c->eax = token;
    vita_crt_file_unlock();
    errno = saved_errno;
    vita_crt_cdecl_return(c);
    return 1;
}
#endif

static void vita_crt_fopen(CPU *__restrict c)
{
    uint32_t guest_path = vita_crt_arg(c, 0U);
    uint32_t guest_mode = vita_crt_arg(c, 1U);
    char native_path[ISAAC_VITA_STARTUP_PATH_MAX + 1U];
    char mode[4];
    FILE *stream;
    uint32_t token = 0U;
    uint32_t i;
    int saved_errno = errno;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    vita_crt_file_token opened_entry = { 0 };
    isaac_vita_archive_diag_event open_event;
    const char *open_log_reason = NULL;
    long archive_open_position = -1L;
    int archive_open_errno = 0;
    int native_fopen_errno = 0;
    int cache_hit = 0;
    int direct = 0;
#if defined(ISAAC_VITA_IO_PROFILE)
    vita_crt_io_shadow_file retained_shadow;
    vita_crt_io_shadow_file opened_shadow;
    int shadow_active = 0;
    int shadow_setvbuf_succeeded = 0;
#endif
#endif

    if (!isaac_vita_startup_map_path(
            c, guest_path, native_path, sizeof native_path) ||
        !vita_crt_copy_ascii(
            c, guest_mode, mode, sizeof mode - 1U,
            "fopen received a null mode",
            "fopen mode exceeds measured bound"))
        return;
    if (!vita_crt_fopen_mode_valid(mode)) {
        guest_fault(c, guest_mode, "fopen received an invalid mode");
        return;
    }
    vita_crt_file_lock();
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active)
        vita_crt_profile_add(&s_io_profile.fopen_calls, 1U);
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    if (!vita_crt_seek_shadow_mode_read_only(mode))
        vita_crt_seek_shadow_note_write();
#endif
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    /* Read-your-writes: a native open (any mode) of a path whose image is
     * still queued must observe the completed file.  Then a fresh binary
     * save becomes a memory image instead of a 1 KiB-buffered newlib FILE. */
    isaac_vita_async_write_sync_path(native_path);
    if (vita_crt_fopen_async_write_locked(c, native_path, mode, saved_errno))
        return;
#endif
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#if defined(ISAAC_VITA_IO_PROFILE)
    vita_crt_io_shadow_clear(&retained_shadow);
    vita_crt_io_shadow_clear(&opened_shadow);
    shadow_active = s_io_profile_active ? 1 : 0;
    if (shadow_active) {
        retained_shadow = s_io_shadow_idle;
        vita_crt_io_shadow_clear(&s_io_shadow_idle);
    }
#endif
    stream = isaac_vita_archive_cache_open(
        &opened_entry.file, native_path, mode, &cache_hit);
    if (!stream) {
        native_fopen_errno = errno;
        direct = vita_crt_raw_archive_try_open(
            &opened_entry, native_path, mode, native_fopen_errno);
    }
#else
    stream = fopen(native_path, mode);
#endif
    if (!stream
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        && !direct
#endif
    ) {
#if defined(ISAAC_VITA_IO_PROFILE)
        if (s_io_profile_active)
            vita_crt_profile_add(&s_io_profile.fopen_failures, 1U);
#endif
        vita_crt_file_unlock();
        vita_crt_file_errno_restore(saved_errno, 1, EIO);
        c->eax = 0U;
        vita_crt_cdecl_return(c);
        return;
    }
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (opened_entry.file.key[0]) {
        errno = 0;
        if (direct) {
            archive_open_position =
                vita_crt_raw_archive_ftell(&opened_entry);
            archive_open_errno = native_fopen_errno;
        } else if (stream) {
            archive_open_position = ftell(stream);
            archive_open_errno = errno;
        }
    }
#endif
#if defined(ISAAC_VITA_IO_PROFILE) && defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (s_io_profile_active && cache_hit)
        vita_crt_profile_add(&s_io_profile.fopen_cache_hits, 1U);
#endif
    /* Vita newlib defaults to a 1 KiB FILE buffer.  Hardware replay of the
     * packed-archive access stream selected 16 KiB as the measured knee:
     * 43.4% fewer requested native bytes than 64 KiB without the 8 KiB call
     * explosion.  Allocation failure keeps the valid stream and default. */
    if (stream && mode[0] == 'r'
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        && !cache_hit
#endif
    ) {
        int setvbuf_result = setvbuf(
            stream, NULL, _IOFBF, ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE);
#if defined(ISAAC_VITA_IO_PROFILE) && \
    defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        shadow_setvbuf_succeeded = setvbuf_result == 0;
#else
        (void)setvbuf_result;
#endif
    }
#if defined(ISAAC_VITA_IO_PROFILE) && \
    defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (shadow_active && opened_entry.file.key[0] && !direct)
        vita_crt_io_shadow_open(
            &opened_shadow, &retained_shadow, cache_hit,
            shadow_setvbuf_succeeded);
#endif
    if (stream && !vita_crt_guest_pointer(c, stream, &token)) {
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        (void)isaac_vita_archive_cache_force_discard(&opened_entry.file);
#else
        (void)fclose(stream);
#endif
        vita_crt_file_unlock();
        errno = saved_errno;
        return;
    }
    for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT; ++i) {
        if (!vita_crt_dynamic_file_is_live(&s_file_tokens[i]))
            break;
    }
    if (i == ISAAC_VITA_CRT_FILE_TOKEN_COUNT) {
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        if (direct)
            (void)vita_crt_raw_archive_close(&opened_entry);
        else
            (void)isaac_vita_archive_cache_force_discard(
                &opened_entry.file);
#else
        (void)fclose(stream);
#endif
        vita_crt_file_unlock();
        errno = saved_errno;
        guest_fault(c, stream ? token : 0U,
                    "Vita CRT FILE token table full");
        return;
    }
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (direct &&
        !vita_crt_guest_pointer(c, &s_file_tokens[i], &token)) {
        (void)vita_crt_raw_archive_close(&opened_entry);
        vita_crt_file_unlock();
        errno = saved_errno;
        return;
    }
    s_file_tokens[i].file = opened_entry.file;
    s_file_tokens[i].raw_archive = opened_entry.raw_archive;
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    vita_crt_seek_shadow_publish(&s_file_tokens[i], mode, native_path);
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    vita_crt_recover_publish(&s_file_tokens[i], mode, native_path);
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    s_file_tokens[i].io_shadow = opened_shadow;
#endif
#else
    s_file_tokens[i].stream = stream;
#endif
    s_file_tokens[i].token = token;
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT) && ISAAC_VITA_CRT_FILE_LOOKUP_HINT
    s_file_lookup_hint = ISAAC_VITA_CRT_FILE_TOKEN_COUNT;
#endif
    c->eax = token;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (opened_entry.file.key[0]) {
        memset(&open_event, 0, sizeof open_event);
        open_event.kind = ISAAC_VITA_ARCHIVE_DIAG_OPEN;
        open_event.token = token;
        open_event.flags = cache_hit ?
            ISAAC_VITA_ARCHIVE_DIAG_CACHE_HIT : 0U;
        if (direct)
            open_event.flags |= ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT;
        if (vita_crt_archive_diag_relevant_key(opened_entry.file.key))
            open_event.flags |= ISAAC_VITA_ARCHIVE_DIAG_RELEVANT;
        if (archive_open_position < 0L)
            open_event.flags |= ISAAC_VITA_ARCHIVE_DIAG_FTELL_FAILED;
        open_event.operation_result = 0;
        open_event.position_before = -1;
        open_event.position_after = (int32_t)archive_open_position;
        open_event.operation_errno = archive_open_errno;
        memcpy(open_event.key, opened_entry.file.key,
               strlen(opened_entry.file.key) + 1U);
        (void)isaac_vita_archive_diag_record(&open_event);
        if (archive_open_position != 0L) {
            open_log_reason = "open-position-anomaly";
        } else if (direct && !s_archive_diag_logged_first_direct) {
            s_archive_diag_logged_first_direct = 1U;
            if (open_event.flags & ISAAC_VITA_ARCHIVE_DIAG_RELEVANT)
                s_archive_diag_logged_first_relevant = 1U;
            open_log_reason = "first-direct-open";
        } else if ((open_event.flags & ISAAC_VITA_ARCHIVE_DIAG_RELEVANT) &&
                   !s_archive_diag_logged_first_relevant) {
            s_archive_diag_logged_first_relevant = 1U;
            open_log_reason = "first-relevant-open";
        }
    }
#endif
    vita_crt_file_unlock();
    errno = saved_errno;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (open_log_reason) {
        vita_crt_archive_diag_log_event(open_log_reason, &open_event);
        errno = saved_errno;
    }
#endif
    vita_crt_cdecl_return(c);
}

static void vita_crt_stdio_common_vfprintf(CPU *__restrict c)
{
    uint32_t token = vita_crt_arg(c, 2U);
    uint32_t format = vita_crt_arg(c, 3U);
    uint32_t arguments = vita_crt_arg(c, 5U);
    FILE *stream;
    vita_crt_format_output out;
    uint8_t chunk[VITA_CRT_VFPRINTF_CHUNK_BYTES];
    uint32_t length;
    int file_errno;
    int failed;
    int dynamic_index;
    int saved_errno = errno;

    (void)vita_crt_arg(c, 0U);
    (void)vita_crt_arg(c, 1U);
    (void)vita_crt_arg(c, 4U);

    /* Validate ownership without retaining a pointer across formatting.  A
     * formatter guest_fault nonlocally unwinds in production, so no registry
     * lock may be held while it reads guest strings or rejects a specifier. */
    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, NULL, &dynamic_index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "vfprintf received an unknown FILE token");
        return;
    }
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live) {
        s_file_tokens[dynamic_index].raw_archive.error = 1U;
        vita_crt_file_unlock();
        errno = EBADF;
        c->eax = UINT32_MAX;
        vita_crt_file_errno_restore(saved_errno, 1, EBADF);
        vita_crt_cdecl_return(c);
        return;
    }
#endif
    vita_crt_file_unlock();
    if (!format) {
        g_isaac_vita_crt_errno = EINVAL;
        errno = saved_errno;
        c->eax = UINT32_MAX;
        vita_crt_cdecl_return(c);
        return;
    }
    out.buffer = 0U;
    out.limit = 0U;
    out.written = 0U;
    out.stream = NULL;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    out.async_image = NULL;
#endif
    if (!vita_crt_stdio_format(c, format, arguments, &out, 1))
        return;
    length = out.written;

    ISAAC_VITA_CRT_VFPRINTF_AFTER_FIRST_PASS(
        c, token, format, arguments, length);

    /* A concurrent close can invalidate the earlier observation.  Revalidate
     * under the same lock and keep the whole native write transaction inside
     * it.  The frozen guest executes this handler on one guest-code thread;
     * native audio and input never enter guest code.  Therefore the validated
     * format cannot legitimately mutate during the second pass.  Keep that
     * pass non-faulting anyway: an unsupported external mutation may leave an
     * already-flushed prefix, but it must never leak the registry lock. */
    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &dynamic_index, NULL)) {
        vita_crt_file_unlock();
        errno = saved_errno;
        guest_fault(c, token, "vfprintf FILE token closed while formatting");
        return;
    }
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].raw_archive.live) {
        s_file_tokens[dynamic_index].raw_archive.error = 1U;
        vita_crt_file_unlock();
        errno = EBADF;
        c->eax = UINT32_MAX;
        vita_crt_file_errno_restore(saved_errno, 1, EBADF);
        vita_crt_cdecl_return(c);
        return;
    }
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    vita_crt_seek_shadow_apply(stream, dynamic_index);
    vita_crt_seek_shadow_note_write();
#endif
    errno = 0;
    out.buffer = 0U;
    out.limit = 0U;
    out.written = 0U;
    out.stream = stream;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    out.async_image = dynamic_index >= 0 &&
        s_file_tokens[dynamic_index].async_write.live
        ? &s_file_tokens[dynamic_index].async_write : NULL;
#endif
    out.stream_buffer = chunk;
    out.stream_capacity = sizeof chunk;
    out.stream_used = 0U;
    out.stream_failed = 0;
    if (length &&
        !vita_crt_stdio_format(c, format, arguments, &out, 0)) {
        vita_crt_file_unlock();
        g_isaac_vita_crt_errno = EINVAL;
        errno = saved_errno;
        c->eax = UINT32_MAX;
        vita_crt_cdecl_return(c);
        return;
    }
    if (out.written != length) {
        vita_crt_file_unlock();
        g_isaac_vita_crt_errno = EINVAL;
        errno = saved_errno;
        c->eax = UINT32_MAX;
        vita_crt_cdecl_return(c);
        return;
    }
    vita_crt_stdio_flush(&out);
    failed = out.stream_failed;
    file_errno = errno;
    vita_crt_file_unlock();
    errno = file_errno;
    c->eax = failed ? UINT32_MAX : length;
    vita_crt_file_errno_restore(saved_errno, failed, EIO);
    vita_crt_cdecl_return(c);
}

static void vita_crt_fclose(CPU *__restrict c)
{
    uint32_t token = vita_crt_arg(c, 0U);
    FILE *stream;
    int index;
    int standard;
    int result;
    int saved_errno = errno;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    int tracked;
#endif
#if defined(ISAAC_VITA_IO_PROFILE) && \
    defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    vita_crt_io_shadow_file closing_shadow;
    int was_cacheable;
    int was_raw;
#endif

    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &index, &standard)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "fclose received an unknown FILE token");
        return;
    }
    if (standard >= 0) {
        vita_crt_file_unlock();
        guest_fault(c, token, "fclose received a standard FILE token");
        return;
    }
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    tracked = token == s_log_batch_token;
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active)
        vita_crt_profile_add(&s_io_profile.fclose_calls, 1U);
#endif
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
#if defined(ISAAC_VITA_IO_PROFILE)
    closing_shadow = s_file_tokens[index].io_shadow;
    was_cacheable = s_file_tokens[index].file.key[0] != '\0';
    was_raw = s_file_tokens[index].raw_archive.live != 0U;
#endif
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (s_file_tokens[index].async_write.live)
        result = isaac_vita_async_write_image_close(
            &s_file_tokens[index].async_write);
    else
#endif
    if (s_file_tokens[index].raw_archive.live)
        result = vita_crt_raw_archive_close(&s_file_tokens[index]);
    else
        result = isaac_vita_archive_cache_close(&s_file_tokens[index].file);
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active && result == 0 && was_cacheable && !was_raw) {
        vita_crt_io_shadow_clear(&s_io_shadow_idle);
        if (!closing_shadow.selected)
            vita_crt_io_shadow_select_unknown(&closing_shadow);
        vita_crt_io_shadow_close(&closing_shadow);
        s_io_shadow_idle = closing_shadow;
    }
    vita_crt_io_shadow_clear(&s_file_tokens[index].io_shadow);
#endif
#else
    result = fclose(stream);
    s_file_tokens[index].stream = NULL;
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    if (!s_file_tokens[index].seek_shadow.read_only)
        vita_crt_seek_shadow_note_write();
    vita_crt_seek_shadow_clear(&s_file_tokens[index]);
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    vita_crt_recover_clear(&s_file_tokens[index]);
#endif
    s_file_tokens[index].token = 0U;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    if (tracked) {
        if (result == EOF) {
            vita_crt_log_batch_increment(&s_log_batch.native_failures);
            s_log_batch.sticky_fail_open = 1U;
        } else {
            s_log_batch.pending_info = 0U;
        }
        s_log_batch_token = 0U;
    }
#endif
    c->eax = (uint32_t)result;
    vita_crt_file_unlock();
    vita_crt_file_errno_restore(saved_errno, result == EOF, EIO);
    vita_crt_cdecl_return(c);
}

static int vita_crt_file_buffer_range(CPU *__restrict c, uint32_t buffer,
                                      uint32_t size, uint32_t count,
                                      const char *what, uint32_t *bytes)
{
    uint32_t total;

    if (size && count > UINT32_MAX / size) {
        guest_fault(c, buffer, what);
        return 0;
    }
    total = size * count;
    if (total && (!buffer || buffer > UINT32_MAX - (total - 1U))) {
        guest_fault(c, buffer, what);
        return 0;
    }
    *bytes = total;
    return 1;
}

static void vita_crt_fileno(CPU *__restrict c)
{
    uint32_t token = vita_crt_arg(c, 0U);
    FILE *stream;
    int result;
    int index;
    int saved_errno = errno;

    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "_fileno received an unknown FILE token");
        return;
    }
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    result = vita_crt_file_fileno(stream, index);
#else
    result = fileno(stream);
#endif
    vita_crt_file_unlock();
    vita_crt_file_errno_restore(saved_errno, result < 0, EBADF);
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_fread(CPU *__restrict c)
{
    uint32_t buffer = vita_crt_arg(c, 0U);
    uint32_t size = vita_crt_arg(c, 1U);
    uint32_t count = vita_crt_arg(c, 2U);
    uint32_t token = vita_crt_arg(c, 3U);
    uint32_t bytes;
    FILE *stream;
    size_t result;
    int failed;
    int index;
    int saved_errno = errno;
    uint32_t parent_return = vita_crt_adapter_parent_return(
        c, ISAAC_VITA_CRT_FREAD_RETURN_RVA);
    int archive_header =
        parent_return == ISAAC_VITA_CRT_ARCHIVE_TYPE1_HEADER_RETURN_RVA ||
        parent_return == ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA;
    int archive_payload =
        parent_return == ISAAC_VITA_CRT_ARCHIVE_TYPE1_PAYLOAD_RETURN_RVA ||
        parent_return == ISAAC_VITA_CRT_ARCHIVE_TYPE2_PAYLOAD_RETURN_RVA;
    int archive_header_invalid = 0;
    int archive_fatal = 0;
    int log_anomaly = 0;
    const char *fault_message = NULL;
    int read_errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    isaac_vita_archive_diag_event event;
    const char *archive_key = NULL;
    int32_t raw_position_before = -1;
    int raw_position_valid = 0;
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    vita_crt_descriptor_recover_receipt recover_receipt =
        { { 0 }, NULL, NULL, -1L };
    long recover_position = -1L;
    int recover_armed = 0;
    int recovered = 0;
    uint8_t recover_raw_error = 0U;
#endif

    if (!vita_crt_file_buffer_range(
            c, buffer, size, count, "fread guest range overflow", &bytes))
        return;
#if !defined(ISAAC_VITA_IO_PROFILE) && \
    !defined(ISAAC_VITA_EXIT_MENU_PROFILE)
    (void)bytes;
#endif
    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "fread received an unknown FILE token");
        return;
    }
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    /* Before the deferred SEEK_END is applied: that real seek fails on a
     * dead descriptor too and would leave nothing to recover the cursor
     * from.  One branch on the hot path for tokens without a path. */
    if (index >= 0 && s_file_tokens[index].recover.path[0]) {
        recover_armed = 1;
        recover_position =
            vita_crt_recover_position(&s_file_tokens[index], stream);
        recover_raw_error = s_file_tokens[index].raw_archive.error;
    }
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    vita_crt_seek_shadow_apply(stream, index);
    vita_crt_seek_shadow_note_cursor_op(index);
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active) {
        vita_crt_profile_add(&s_io_profile.fread_calls, 1U);
        vita_crt_profile_add(&s_io_profile.fread_requested_bytes, bytes);
    }
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (index >= 0 && s_file_tokens[index].file.key[0]) {
        archive_key = s_file_tokens[index].file.key;
        if (s_file_tokens[index].raw_archive.live) {
            raw_position_before =
                (int32_t)s_file_tokens[index].raw_archive.position;
            raw_position_valid = 1;
        }
    }
#endif
    /* The two measured ArchivedFile adapters always request exactly one
     * four-byte block header.  Reject a distorted shape before native fread:
     * the post-read decoder uses ld32 and therefore must never rely on a
     * smaller range having been validated above. */
    if (archive_header &&
        (size != sizeof(uint32_t) || count != 1U ||
         bytes != sizeof(uint32_t))) {
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        memset(&event, 0, sizeof event);
        event.kind = ISAAC_VITA_ARCHIVE_DIAG_FREAD;
        event.token = token;
        event.flags = ISAAC_VITA_ARCHIVE_DIAG_RELEVANT;
        if (index >= 0 && s_file_tokens[index].raw_archive.live)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT;
        event.position_before =
            (int32_t)vita_crt_file_ftell(stream, index);
        event.position_after = event.position_before;
        if (event.position_before < 0)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_FTELL_FAILED;
        event.operation_result = -1;
        event.operation_errno = EINVAL;
        event.element_size = size;
        event.element_count = count;
        event.caller_return = parent_return;
        if (archive_key)
            memcpy(event.key, archive_key, strlen(archive_key) + 1U);
        (void)isaac_vita_archive_diag_record(&event);
#endif
        vita_crt_file_unlock();
        errno = saved_errno;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        vita_crt_archive_diag_log_event("header-shape", &event);
        isaac_vita_crt_archive_diag_dump();
        errno = saved_errno;
#endif
        guest_fault(c, buffer,
                    "ArchivedFile block header read shape is invalid");
        return;
    }
    /* Both ArchivedFile modes trust the block length after this imported
     * read.  Reject an oversized payload before native fread can overwrite
     * the 0x800-byte destination; the observed hardware word would otherwise
     * request roughly 52 KiB here before the later giant memcpy fault. */
    if (archive_payload && bytes > ISAAC_VITA_CRT_ARCHIVE_BLOCK_MAX_BYTES) {
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        memset(&event, 0, sizeof event);
        event.kind = ISAAC_VITA_ARCHIVE_DIAG_FREAD;
        event.token = token;
        event.flags = ISAAC_VITA_ARCHIVE_DIAG_RELEVANT;
        if (index >= 0 && s_file_tokens[index].raw_archive.live)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT;
        event.position_before =
            (int32_t)vita_crt_file_ftell(stream, index);
        event.position_after = event.position_before;
        event.operation_result = -1;
        event.operation_errno = EOVERFLOW;
        event.element_size = size;
        event.element_count = count;
        event.caller_return = parent_return;
        if (archive_key)
            memcpy(event.key, archive_key, strlen(archive_key) + 1U);
        (void)isaac_vita_archive_diag_record(&event);
#endif
        vita_crt_file_unlock();
        errno = saved_errno;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        vita_crt_archive_diag_log_event("payload-bound", &event);
        isaac_vita_crt_archive_diag_dump();
        errno = saved_errno;
#endif
        guest_fault(c, buffer,
                    "ArchivedFile payload exceeds 0x800-byte block");
        return;
    }
#if defined(ISAAC_VITA_EXIT_MENU_PROFILE)
    /* Keep the profiler outside fread's errno contract.  The measured span
     * intentionally includes the errno clear/read pair so callbacks cannot
     * perturb the value observed by the existing CRT path. */
    isaac_vita_exit_menu_profile_fread_begin(bytes);
#endif
    errno = 0;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
    if (index >= 0 && s_file_tokens[index].async_write.live) {
        /* newlib returns 0 with the error flag for a read on a "wb" stream. */
        s_file_tokens[index].async_write.error = 1U;
        errno = EBADF;
        result = 0U;
    } else
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (index >= 0 && s_file_tokens[index].raw_archive.live)
        result = vita_crt_raw_archive_fread(
            &s_file_tokens[index], (void *)(uintptr_t)buffer, size, count);
    else
#endif
        result = vita_crt_native_fread(
            (void *)(uintptr_t)buffer, size, count, stream);
    read_errno = errno;
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    if (recover_armed && result < count && read_errno == ENODEV &&
        vita_crt_recover_token(&s_file_tokens[index], recover_position, 1,
                               "fread", &recover_receipt)) {
        /* The whole request again from the recorded cursor: a partial
         * element newlib already copied is read again, not skipped. */
        stream = vita_crt_dynamic_stream(&s_file_tokens[index]);
        recovered = 1;
        errno = 0;
        if (s_file_tokens[index].raw_archive.live) {
            s_file_tokens[index].raw_archive.error = recover_raw_error;
            result = vita_crt_raw_archive_fread(
                &s_file_tokens[index], (void *)(uintptr_t)buffer, size,
                count);
        } else {
            result = vita_crt_native_fread(
                (void *)(uintptr_t)buffer, size, count, stream);
        }
        read_errno = errno;
    }
#endif
#if defined(ISAAC_VITA_EXIT_MENU_PROFILE)
    isaac_vita_exit_menu_profile_fread_end(
        (uint32_t)((uint64_t)size * (uint64_t)result));
#endif
    failed = result < count &&
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        vita_crt_file_ferror(stream, index);
#else
        ferror(stream);
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    if (recovered && failed)
        ++s_descriptor_recover.redo_fail;
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active) {
        uint64_t returned = (uint64_t)size * (uint64_t)result;

        vita_crt_profile_add_product(
            &s_io_profile.fread_returned_bytes, size, (uint32_t)result);
        if (failed)
            vita_crt_profile_add(&s_io_profile.fread_failures, 1U);
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        if (index >= 0 && s_file_tokens[index].file.key[0] &&
            !s_file_tokens[index].raw_archive.live)
            vita_crt_io_shadow_read(
                &s_file_tokens[index].io_shadow,
                bytes,
                returned > UINT32_MAX ? UINT32_MAX : (uint32_t)returned);
#else
        (void)returned;
#endif
    }
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (archive_header) {
        uint32_t raw = result == 1U ? ld32(buffer) : 0U;

        archive_header_invalid =
            result != 1U ||
            (raw & 0x7fffffffU) >
                ISAAC_VITA_CRT_ARCHIVE_BLOCK_MAX_BYTES;
        if (archive_header_invalid) {
            memset(&event, 0, sizeof event);
            event.kind = ISAAC_VITA_ARCHIVE_DIAG_FREAD;
            event.token = token;
            event.flags = ISAAC_VITA_ARCHIVE_DIAG_RELEVANT;
            if (index >= 0 && s_file_tokens[index].raw_archive.live)
                event.flags |= ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT;
            if (vita_crt_file_feof(stream, index))
                event.flags |= ISAAC_VITA_ARCHIVE_DIAG_EOF;
            if (vita_crt_file_ferror(stream, index))
                event.flags |= ISAAC_VITA_ARCHIVE_DIAG_STREAM_ERROR;
            event.position_after =
                (int32_t)vita_crt_file_ftell(stream, index);
            event.position_before = raw_position_valid
                ? raw_position_before
                : event.position_after -
                    (int32_t)((uint32_t)size * (uint32_t)result);
            event.operation_result = (int32_t)result;
            event.operation_errno = read_errno;
            event.element_size = size;
            event.element_count = count;
            event.elements_returned = (uint32_t)result;
            event.caller_return = parent_return;
            event.observed_word = raw;
            if (archive_key)
                memcpy(event.key, archive_key, strlen(archive_key) + 1U);
            (void)isaac_vita_archive_diag_record(&event);
            archive_fatal = 1;
            log_anomaly = 1;
            fault_message = "ArchivedFile block header is invalid";
        }
    } else if (archive_key &&
               vita_crt_archive_diag_relevant_key(archive_key) &&
               result < count) {
        memset(&event, 0, sizeof event);
        event.kind = ISAAC_VITA_ARCHIVE_DIAG_FREAD;
        event.token = token;
        event.flags = ISAAC_VITA_ARCHIVE_DIAG_RELEVANT;
        if (index >= 0 && s_file_tokens[index].raw_archive.live)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT;
        if (vita_crt_file_feof(stream, index))
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_EOF;
        if (vita_crt_file_ferror(stream, index))
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_STREAM_ERROR;
        event.position_after =
            (int32_t)vita_crt_file_ftell(stream, index);
        event.position_before = raw_position_valid
            ? raw_position_before
            : event.position_after -
                (int32_t)((uint32_t)size * (uint32_t)result);
        event.operation_result = (int32_t)result;
        event.operation_errno = read_errno;
        event.element_size = size;
        event.element_count = count;
        event.elements_returned = (uint32_t)result;
        event.caller_return = parent_return;
        memcpy(event.key, archive_key, strlen(archive_key) + 1U);
        (void)isaac_vita_archive_diag_record(&event);
        log_anomaly = 1;
    }
#else
    if (archive_header) {
        uint32_t raw = result == 1U ? ld32(buffer) : 0U;
        archive_header_invalid =
            result != 1U ||
            (raw & 0x7fffffffU) >
                ISAAC_VITA_CRT_ARCHIVE_BLOCK_MAX_BYTES;
        if (archive_header_invalid) {
            archive_fatal = 1;
            fault_message = "ArchivedFile block header is invalid";
        }
    }
#endif
    vita_crt_file_unlock();
    errno = read_errno;
    vita_crt_file_errno_restore(saved_errno, failed, EIO);
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    if (recovered) {
        vita_crt_recover_log_receipt(&recover_receipt);
        errno = saved_errno;
    }
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (log_anomaly) {
        vita_crt_archive_diag_log_event(
            archive_fatal ? "header-bound" : "short-read", &event);
        if (archive_fatal)
            isaac_vita_crt_archive_diag_dump();
        errno = saved_errno;
    }
#else
    (void)log_anomaly;
#endif
    if (archive_fatal) {
        guest_fault(c, buffer, fault_message);
        return;
    }
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_fwrite(CPU *__restrict c)
{
    uint32_t buffer = vita_crt_arg(c, 0U);
    uint32_t size = vita_crt_arg(c, 1U);
    uint32_t count = vita_crt_arg(c, 2U);
    uint32_t token = vita_crt_arg(c, 3U);
    uint32_t bytes;
    FILE *stream;
    size_t result;
    int failed;
    int index;
    int saved_errno = errno;

    if (!vita_crt_file_buffer_range(
            c, buffer, size, count, "fwrite guest range overflow", &bytes))
        return;
    (void)bytes;
    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "fwrite received an unknown FILE token");
        return;
    }
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    vita_crt_seek_shadow_apply(stream, index);
    vita_crt_seek_shadow_note_write();
#endif
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    result = vita_crt_file_fwrite(
        (const void *)(uintptr_t)buffer, size, count, stream, index);
    failed = result < count && vita_crt_file_ferror(stream, index);
#else
    result = fwrite((const void *)(uintptr_t)buffer, size, count, stream);
    failed = result < count && ferror(stream);
#endif
    vita_crt_file_unlock();
    vita_crt_file_errno_restore(saved_errno, failed, EIO);
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_fseek(CPU *__restrict c)
{
    uint32_t token = vita_crt_arg(c, 0U);
    int32_t offset = (int32_t)vita_crt_arg(c, 1U);
    int origin = (int)vita_crt_arg(c, 2U);
    FILE *stream;
    int result;
    int index;
    int saved_errno = errno;
    int seek_errno;
    uint32_t parent_return = vita_crt_adapter_parent_return(
        c, ISAAC_VITA_CRT_ARCHIVE_FSEEK_RETURN_RVA);
#if !defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    (void)parent_return;
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    isaac_vita_archive_diag_event event;
    long position_before = -1L;
    long position_after = -1L;
    int track_archive_seek = 0;
    int seek_anomaly = 0;
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    int report_due = 0;
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    vita_crt_descriptor_recover_receipt recover_receipt =
        { { 0 }, NULL, NULL, -1L };
    long recover_position = -1L;
    int recover_armed = 0;
    int recovered = 0;
#endif

    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "fseek received an unknown FILE token");
        return;
    }
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active)
        vita_crt_profile_add(&s_io_profile.fseek_calls, 1U);
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    if (origin == SEEK_END && index >= 0 &&
        vita_crt_seek_shadow_eligible(&s_file_tokens[index], stream))
        report_due = vita_crt_seek_shadow_attribute(c, &s_file_tokens[index]);
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    track_archive_seek =
        index >= 0 && s_file_tokens[index].file.key[0] &&
        parent_return == ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA;
    if (track_archive_seek)
        position_before = vita_crt_file_ftell(stream, index);
#endif
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    /* Raw tokens seek arithmetically and never reach the kernel here.  Only
     * a SEEK_CUR redo depends on the cursor; SEEK_SET and SEEK_END are redone
     * on the fresh handle as they are. */
    if (index >= 0 && s_file_tokens[index].recover.path[0] &&
        !s_file_tokens[index].raw_archive.live) {
        recover_armed = 1;
        if (origin == SEEK_CUR)
            recover_position =
                vita_crt_recover_position(&s_file_tokens[index], stream);
    }
#endif
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    result = vita_crt_file_fseek(stream, index, (long)offset, origin);
#else
    result = fseek(stream, (long)offset, origin);
#endif
    seek_errno = errno;
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    if (recover_armed && result != 0 && seek_errno == ENODEV &&
        vita_crt_recover_token(&s_file_tokens[index], recover_position,
                               origin == SEEK_CUR, "fseek",
                               &recover_receipt)) {
        /* Through the lane dispatcher again, so the seek shadow's deferred
         * state is exactly what a first-time success would have left. */
        stream = vita_crt_dynamic_stream(&s_file_tokens[index]);
        recovered = 1;
        errno = 0;
        result = vita_crt_file_fseek(stream, index, (long)offset, origin);
        seek_errno = errno;
        if (result != 0)
            ++s_descriptor_recover.redo_fail;
    }
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (track_archive_seek ||
        (result != 0 && index >= 0 && s_file_tokens[index].file.key[0])) {
        position_after = vita_crt_file_ftell(stream, index);
        memset(&event, 0, sizeof event);
        event.kind = ISAAC_VITA_ARCHIVE_DIAG_FSEEK;
        event.token = token;
        event.requested_offset = offset;
        event.origin = origin;
        event.operation_result = result;
        event.position_before = (int32_t)position_before;
        event.position_after = (int32_t)position_after;
        event.operation_errno = seek_errno;
        event.caller_return = parent_return;
        if (position_after < 0L)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_FTELL_FAILED;
        if (vita_crt_archive_diag_relevant_key(
                s_file_tokens[index].file.key))
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_RELEVANT;
        if (s_file_tokens[index].raw_archive.live)
            event.flags |= ISAAC_VITA_ARCHIVE_DIAG_SCEIO_DIRECT;
        memcpy(event.key, s_file_tokens[index].file.key,
               strlen(s_file_tokens[index].file.key) + 1U);
        (void)isaac_vita_archive_diag_record(&event);
        seek_anomaly = result != 0 || position_after < 0L ||
            (track_archive_seek && origin == SEEK_SET &&
             position_after != (long)offset);
    }
#endif
#if defined(ISAAC_VITA_IO_PROFILE)
    if (s_io_profile_active) {
        if (result != 0) {
            vita_crt_profile_add(&s_io_profile.fseek_failures, 1U);
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
            if (index >= 0 && s_file_tokens[index].file.key[0] &&
                !s_file_tokens[index].raw_archive.live)
                vita_crt_io_shadow_seek_failed(
                    &s_file_tokens[index].io_shadow, origin);
#endif
        }
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
        else if (index >= 0 && s_file_tokens[index].file.key[0] &&
                 !s_file_tokens[index].raw_archive.live) {
            vita_crt_io_shadow_seek(
                &s_file_tokens[index].io_shadow, offset, origin);
        }
#endif
    }
#endif
    vita_crt_file_unlock();
    errno = seek_errno;
    vita_crt_file_errno_restore(saved_errno, result != 0, EIO);
#if defined(ISAAC_VITA_CRT_DESCRIPTOR_RECOVER)
    if (recovered) {
        vita_crt_recover_log_receipt(&recover_receipt);
        errno = saved_errno;
    }
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    if (report_due) {
        isaac_vita_crt_seek_shadow_report("periodic");
        errno = saved_errno;
    }
#endif
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (seek_anomaly) {
        vita_crt_archive_diag_log_event("seek-cursor-anomaly", &event);
        if (track_archive_seek)
            isaac_vita_crt_archive_diag_dump();
        errno = saved_errno;
        if (track_archive_seek) {
            guest_fault(
                c, token,
                "ArchivedFile seek failed or landed at wrong cursor");
            return;
        }
    }
#endif
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_ftell(CPU *__restrict c)
{
    uint32_t token = vita_crt_arg(c, 0U);
    FILE *stream;
    long result;
    int index;
    int saved_errno = errno;

    vita_crt_file_lock();
    if (!vita_crt_find_file_token_unlocked(
            token, &stream, &index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "ftell received an unknown FILE token");
        return;
    }
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    result = vita_crt_file_ftell(stream, index);
#else
    result = ftell(stream);
#endif
    vita_crt_file_unlock();
    vita_crt_file_errno_restore(saved_errno, result < 0, EIO);
    c->eax = (uint32_t)(int32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_fflush(CPU *__restrict c)
{
    uint32_t token = vita_crt_arg(c, 0U);
    FILE *stream = NULL;
    int result;
    int index = -1;
    int saved_errno = errno;
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    int exact_logger = 0;
    int tracked;
#endif

    vita_crt_file_lock();
    if (token && !vita_crt_find_file_token_unlocked(
                     token, &stream, &index, NULL)) {
        vita_crt_file_unlock();
        guest_fault(c, token, "fflush received an unknown FILE token");
        return;
    }
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    if (vita_crt_log_batch_classify_locked(c, token, &exact_logger) ==
            VITA_CRT_LOG_BATCH_DEFER) {
        vita_crt_file_unlock();
        errno = saved_errno;
        c->eax = 0U;
        vita_crt_cdecl_return(c);
        return;
    }
    tracked = vita_crt_log_batch_tracks_flush_locked(token, exact_logger);
#endif
#if defined(ISAAC_VITA_CRT_SEEK_SHADOW)
    if (!token || index < 0 || !s_file_tokens[index].seek_shadow.read_only)
        vita_crt_seek_shadow_note_write();
#endif
    errno = 0;
#if defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    result = token ? vita_crt_file_fflush(stream, index) : fflush(NULL);
#else
    result = fflush(stream);
#endif
#if defined(ISAAC_VITA_GAME_LOG_BATCH)
    vita_crt_log_batch_native_result_locked(tracked, result);
#endif
#if defined(ISAAC_VITA_IO_PROFILE) && \
    defined(ISAAC_VITA_ARCHIVE_FILE_CACHE)
    if (s_io_profile_active) {
        if (!token) {
            vita_crt_io_shadow_fflush_all();
        } else if (index >= 0 && s_file_tokens[index].file.key[0] &&
                   !s_file_tokens[index].raw_archive.live) {
            vita_crt_io_shadow_fflush(
                &s_file_tokens[index].io_shadow);
        }
    }
#endif
    vita_crt_file_unlock();
    vita_crt_file_errno_restore(saved_errno, result == EOF, EIO);
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_acrt_iob_func(CPU *__restrict c)
{
    uint32_t index = vita_crt_arg(c, 0U);
    FILE *stream = vita_crt_standard_stream(index);
    uint32_t token;

    if (!stream) {
        guest_fault(c, index, "__acrt_iob_func index outside 0..2");
        return;
    }
    if (!vita_crt_guest_pointer(c, stream, &token))
        return;
    c->eax = token;
    vita_crt_cdecl_return(c);
}

int isaac_vita_crt_osfhandle_is_owned(uint32_t handle)
{
    int descriptor = (int32_t)handle;
    uint32_t i;
    int found = 0;
    int saved_errno = errno;

    if (descriptor < 0)
        return 0;
    vita_crt_file_lock();
    for (i = 0U; i < ISAAC_VITA_CRT_STANDARD_STREAM_COUNT && !found; ++i)
        found = fileno(vita_crt_standard_stream(i)) == descriptor;
    for (i = 0U; i < ISAAC_VITA_CRT_FILE_TOKEN_COUNT && !found; ++i) {
        FILE *dynamic = vita_crt_dynamic_stream(&s_file_tokens[i]);
        found = dynamic && fileno(dynamic) == descriptor;
#if defined(ISAAC_VITA_ASYNC_SAVE_WRITE)
        if (!found && s_file_tokens[i].async_write.live &&
            (uint32_t)descriptor ==
                ISAAC_VITA_CRT_ASYNC_WRITE_FD_BASE + i)
            found = 1;
#endif
    }
    vita_crt_file_unlock();
    errno = saved_errno;
    return found;
}

static void vita_crt_get_osfhandle(CPU *__restrict c)
{
    uint32_t descriptor = vita_crt_arg(c, 0U);
    int found;
    int saved_errno = errno;

    /* Vita newlib has no distinct Win32 HANDLE layer.  Preserve one libc
     * family by mapping only an owned live newlib descriptor to itself. */
    errno = 0;
    found = isaac_vita_crt_osfhandle_is_owned(descriptor);
    if (!found)
        errno = EBADF;
    vita_crt_file_errno_restore(saved_errno, !found, EBADF);
    c->eax = found ? descriptor : UINT32_MAX;
    vita_crt_cdecl_return(c);
}

/* The complete frozen-PE narrow string family uses the x86 UCRT cdecl ABI.
 * These implementations are kept mechanically equivalent to host_win32.c:
 * all strings stay guest-addressed and the C-locale ctype functions return
 * UCRT classification bits rather than invented Boolean values. */
static void vita_crt_isdigit(CPU *__restrict c)
{
    uint32_t value = vita_crt_arg(c, 0U);
    c->eax = value - (uint32_t)'0' <= 9U ? 0x04U : 0U;
    vita_crt_cdecl_return(c);
}

static int vita_crt_ascii_space(uint32_t value)
{
    return value == 0x20U || value - 0x09U <= 4U;
}

static int vita_crt_ascii_punct(uint32_t value)
{
    return (value - 0x21U <= 0x0EU) ||
           (value - 0x3AU <= 0x06U) ||
           (value - 0x5BU <= 0x05U) ||
           (value - 0x7BU <= 0x03U);
}

static void vita_crt_isspace(CPU *__restrict c)
{
    c->eax = vita_crt_ascii_space(vita_crt_arg(c, 0U)) ? 0x08U : 0U;
    vita_crt_cdecl_return(c);
}

static void vita_crt_ispunct(CPU *__restrict c)
{
    c->eax = vita_crt_ascii_punct(vita_crt_arg(c, 0U)) ? 0x10U : 0U;
    vita_crt_cdecl_return(c);
}

static void vita_crt_tolower(CPU *__restrict c)
{
    uint32_t value = vita_crt_arg(c, 0U);
    c->eax = value - (uint32_t)'A' <= 25U ? value + 0x20U : value;
    vita_crt_cdecl_return(c);
}

static void vita_crt_toupper(CPU *__restrict c)
{
    uint32_t value = vita_crt_arg(c, 0U);
    c->eax = value - (uint32_t)'a' <= 25U ? value - 0x20U : value;
    vita_crt_cdecl_return(c);
}

static void vita_crt_iswspace(CPU *__restrict c)
{
    uint32_t value = vita_crt_arg(c, 0U);
    int yes = vita_crt_ascii_space(value) || value == 0x85U ||
              value == 0xA0U || value == 0x1680U || value == 0x180EU ||
              value - 0x2000U <= 0x0AU || value - 0x2028U <= 1U ||
              value == 0x202FU || value == 0x205FU || value == 0x3000U;
    c->eax = yes ? 0x08U : 0U;
    vita_crt_cdecl_return(c);
}

static void vita_crt_strpbrk(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    uint32_t accept = vita_crt_arg(c, 1U);
    uint32_t cursor;

    if (!source || !accept) {
        guest_fault(c, !source ? source : accept,
                    "strpbrk received a null guest pointer");
        return;
    }
    for (cursor = source; ld8(cursor); ++cursor) {
        uint32_t candidate;
        uint8_t value = ld8(cursor);
        for (candidate = accept; ld8(candidate); ++candidate) {
            if (value == ld8(candidate)) {
                c->eax = cursor;
                vita_crt_cdecl_return(c);
                return;
            }
        }
    }
    c->eax = 0U;
    vita_crt_cdecl_return(c);
}

static uint8_t vita_crt_ascii_lower_byte(uint8_t value)
{
    return (uint32_t)value - (uint32_t)'A' <= 25U
        ? (uint8_t)(value + 0x20U) : value;
}

static void vita_crt_strnicmp(CPU *__restrict c)
{
    uint32_t left = vita_crt_arg(c, 0U);
    uint32_t right = vita_crt_arg(c, 1U);
    uint32_t count = vita_crt_arg(c, 2U);
    uint32_t i;
    int result = 0;

    if (count && (!left || !right)) {
        guest_fault(c, !left ? left : right,
                    "_strnicmp received a null guest pointer");
        return;
    }
    for (i = 0U; i < count; ++i) {
        uint8_t original = ld8(left + i);
        uint8_t a = vita_crt_ascii_lower_byte(original);
        uint8_t b = vita_crt_ascii_lower_byte(ld8(right + i));
        if (a != b) {
            result = (int)a - (int)b;
            break;
        }
        if (!original)
            break;
    }
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_strncmp(CPU *__restrict c)
{
    uint32_t left = vita_crt_arg(c, 0U);
    uint32_t right = vita_crt_arg(c, 1U);
    uint32_t count = vita_crt_arg(c, 2U);
    uint32_t i;
    int result = 0;

    if (count && (!left || !right)) {
        guest_fault(c, !left ? left : right,
                    "strncmp received a null guest pointer");
        return;
    }
    for (i = 0U; i < count; ++i) {
        uint8_t a = ld8(left + i);
        uint8_t b = ld8(right + i);
        if (a != b) {
            result = (int)a - (int)b;
            break;
        }
        if (!a)
            break;
    }
    c->eax = (uint32_t)result;
    vita_crt_cdecl_return(c);
}

static void vita_crt_strncpy(CPU *__restrict c)
{
    uint32_t dest = vita_crt_arg(c, 0U);
    uint32_t source = vita_crt_arg(c, 1U);
    uint32_t count = vita_crt_arg(c, 2U);
    uint32_t i;
    int terminated = 0;

    if ((!dest || !source) && count) {
        guest_fault(c, !dest ? dest : source,
                    "strncpy received a null measured range");
        return;
    }
    for (i = 0U; i < count; ++i) {
        uint8_t value = terminated ? 0U : ld8(source + i);
        st8(dest + i, value);
        if (!value)
            terminated = 1;
    }
    c->eax = dest;
    vita_crt_cdecl_return(c);
}

static void vita_crt_strncpy_s(CPU *__restrict c)
{
    uint32_t dest = vita_crt_arg(c, 0U);
    uint32_t capacity = vita_crt_arg(c, 1U);
    uint32_t source = vita_crt_arg(c, 2U);
    uint32_t count = vita_crt_arg(c, 3U);
    uint32_t i;

    if (!dest || !capacity) {
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    if (!source) {
        st8(dest, 0U);
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    if (!count) {
        st8(dest, 0U);
        c->eax = 0U;
        vita_crt_cdecl_return(c);
        return;
    }
    for (i = 0U; count == UINT32_MAX || i < count; ++i) {
        uint8_t value = ld8(source + i);
        if (!value) {
            st8(dest + i, 0U);
            c->eax = 0U;
            vita_crt_cdecl_return(c);
            return;
        }
        if (i + 1U >= capacity) {
            if (count == UINT32_MAX) {
                st8(dest + capacity - 1U, 0U);
                c->eax = 80U;       /* _STRUNCATE */
            } else {
                st8(dest, 0U);
                c->eax = ERANGE;
            }
            vita_crt_cdecl_return(c);
            return;
        }
        st8(dest + i, value);
    }
    st8(dest + i, 0U);
    c->eax = 0U;
    vita_crt_cdecl_return(c);
}

static void vita_crt_strcpy_s(CPU *__restrict c)
{
    uint32_t dest = vita_crt_arg(c, 0U);
    uint32_t capacity = vita_crt_arg(c, 1U);
    uint32_t source = vita_crt_arg(c, 2U);
    uint32_t i;

    if (!dest) {
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    if (!capacity) {
        c->eax = ERANGE;
        vita_crt_cdecl_return(c);
        return;
    }
    if (!source) {
        st8(dest, 0U);
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    for (i = 0U; i < capacity; ++i) {
        uint8_t value = ld8(source + i);
        st8(dest + i, value);
        if (!value) {
            c->eax = 0U;
            vita_crt_cdecl_return(c);
            return;
        }
    }
    st8(dest, 0U);
    c->eax = ERANGE;
    vita_crt_cdecl_return(c);
}

static void vita_crt_strcat_s(CPU *__restrict c)
{
    uint32_t dest = vita_crt_arg(c, 0U);
    uint32_t capacity = vita_crt_arg(c, 1U);
    uint32_t source = vita_crt_arg(c, 2U);
    uint32_t used = 0U;
    uint32_t i = 0U;

    if (!dest || !capacity) {
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    if (!source) {
        st8(dest, 0U);
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    while (used < capacity && ld8(dest + used))
        ++used;
    if (used == capacity) {
        st8(dest, 0U);
        c->eax = ERANGE;
        vita_crt_cdecl_return(c);
        return;
    }
    for (;;) {
        uint8_t value = ld8(source + i++);
        if (used >= capacity - 1U && value) {
            st8(dest, 0U);
            c->eax = ERANGE;
            vita_crt_cdecl_return(c);
            return;
        }
        st8(dest + used++, value);
        if (!value)
            break;
    }
    c->eax = 0U;
    vita_crt_cdecl_return(c);
}

void isaac_vita_crt_strdup(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    uint32_t length = 0U;
    void *allocation;
    uint32_t result;

    if (!source) {
        guest_fault(c, source, "_strdup received a null guest pointer");
        return;
    }
    while (ld8(source + length)) {
        if (length == UINT32_MAX - 1U) {
            guest_fault(c, source, "_strdup source is not terminated");
            return;
        }
        ++length;
    }
    allocation = isaac_vita_guest_malloc((size_t)length + 1U);
    if (isaac_vita_guest_heap_terminal()) {
        if (allocation)
            (void)isaac_vita_guest_free(allocation);
        guest_fault(c, source, "_strdup guest heap is terminal");
        return;
    }
    if (!allocation) {
        c->eax = 0U;
        vita_crt_cdecl_return(c);
        return;
    }
    if (!vita_crt_guest_pointer(c, allocation, &result)) {
        (void)isaac_vita_guest_free(allocation);
        return;
    }
    for (uint32_t i = 0U; i <= length; ++i)
        st8(result + i, ld8(source + i));
    c->eax = result;
    vita_crt_cdecl_return(c);
}

/* `void __cdecl qsort(void *, size_t, size_t, int (__cdecl *)(...))`.
 *
 * This is copied from the already-oracled PC host boundary.  A native qsort
 * cannot call a translated comparator: its function pointer is a guest
 * address and its arguments belong on the emulated x86 stack.  An in-place
 * heapsort keeps every byte in guest storage, allocates nothing, and has a
 * deterministic comparison sequence.
 *
 * There is intentionally no CRT/file lock here.  guest_call is synchronous
 * on the owning guest-dispatch thread, and a comparator may re-enter this
 * handler (or another import) before it returns.  All sort state therefore
 * lives in the native activation and CPU supplied by that caller; adding a
 * process lock across guest_call would deadlock valid nested qsort use.
 */
static int vita_crt_qsort_compare(CPU *__restrict c, uint32_t comparator,
                                  uint32_t left, uint32_t right,
                                  int32_t *order)
{
    uint32_t saved = c->esp;

    gpush(c, right);                  /* cdecl: rightmost argument first */
    gpush(c, left);
    gpush(c, ISAAC_VITA_CRT_QSORT_CALLBACK_RETURN);
    guest_call(c, comparator);
    if (c->fault)
        return 0;
    if (c->esp != saved - 8U) {
        (void)guest_stack_set(c, saved, comparator);
        guest_fault(c, comparator,
                    "qsort comparator did not preserve its cdecl stack");
        return 0;
    }
    if (!guest_stack_adjust(c, 8U, comparator))
        return 0;                     /* qsort is the comparator's caller */
    *order = (int32_t)c->eax;
    return 1;
}

static void vita_crt_qsort_swap(uint32_t left, uint32_t right,
                                uint32_t width)
{
    uint32_t i;

    if (left == right)
        return;
    for (i = 0U; i < width; ++i) {
        uint8_t temporary = ld8(left + i);
        st8(left + i, ld8(right + i));
        st8(right + i, temporary);
    }
}

static int vita_crt_qsort_sift_down(CPU *__restrict c, uint32_t base,
                                    uint32_t width, uint32_t comparator,
                                    uint32_t root, uint32_t end)
{
    for (;;) {
        uint32_t child;
        uint32_t root_address;
        uint32_t child_address;
        int32_t order;

        /* Check before doubling: a valid width-1 array can contain nearly
         * UINT32_MAX elements, and a leaf index must not wrap into a child. */
        if (end < 2U || root > (end - 2U) / 2U)
            return 1;
        child = root * 2U + 1U;
        if (child + 1U < end) {
            uint32_t left = base + child * width;
            uint32_t right = left + width;
            if (!vita_crt_qsort_compare(c, comparator, left, right, &order))
                return 0;
            if (order < 0)
                ++child;             /* choose the strictly larger child */
        }
        root_address = base + root * width;
        child_address = base + child * width;
        if (!vita_crt_qsort_compare(c, comparator, root_address,
                                    child_address, &order))
            return 0;
        if (order >= 0)
            return 1;
        vita_crt_qsort_swap(root_address, child_address, width);
        root = child;
    }
}

void isaac_vita_crt_qsort(CPU *__restrict c)
{
    uint32_t base = vita_crt_arg(c, 0U);
    uint32_t count = vita_crt_arg(c, 1U);
    uint32_t width = vita_crt_arg(c, 2U);
    uint32_t comparator = vita_crt_arg(c, 3U);
    uint64_t span;
    uint32_t start;
    uint32_t end;

    /* No element is compared or touched in these cases.  In particular, a
     * zero-length container may use the conventional null data pointer. */
    if (count < 2U) {
        vita_crt_cdecl_return(c);
        return;
    }
    if (!base) {
        guest_fault(c, base, "qsort received a null base");
        return;
    }
    if (!width) {
        guest_fault(c, width, "qsort received a zero element width");
        return;
    }
    if (!comparator) {
        guest_fault(c, comparator, "qsort received a null comparator");
        return;
    }
    span = (uint64_t)count * (uint64_t)width;
    if (span > (uint64_t)UINT32_MAX - (uint64_t)base + 1U) {
        guest_fault(c, base,
                    "qsort guest range overflows 32-bit address space");
        return;
    }

    for (start = count / 2U; start != 0U; --start) {
        if (!vita_crt_qsort_sift_down(c, base, width, comparator,
                                      start - 1U, count))
            return;
    }
    for (end = count; end > 1U; --end) {
        vita_crt_qsort_swap(base, base + (end - 1U) * width, width);
        if (!vita_crt_qsort_sift_down(c, base, width, comparator,
                                      0U, end - 1U))
            return;
    }
    vita_crt_cdecl_return(c);
}

static int vita_crt_convert_base_valid(int base)
{
    return base == 0 || (base >= 2 && base <= 36);
}

static int vita_crt_convert_endptr_valid(CPU *__restrict c,
                                         uint32_t endptr,
                                         const char *what)
{
    if (endptr && endptr > UINT32_MAX - 3U) {
        guest_fault(c, endptr, what);
        return 0;
    }
    return 1;
}

static void vita_crt_strtoull(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    uint32_t endptr = vita_crt_arg(c, 1U);
    int base = (int)vita_crt_arg(c, 2U);
    char text[ISAAC_VITA_CRT_CONVERT_TEXT_MAX + 1U];
    char *native_end;
    unsigned long long value;
    ptrdiff_t end_offset;
    int conversion_errno;
    int saved_errno = errno;

    if (!vita_crt_convert_base_valid(base)) {
        guest_fault(c, (uint32_t)base,
                    "strtoull received an invalid base");
        return;
    }
    if (!vita_crt_convert_endptr_valid(
            c, endptr, "strtoull end pointer range overflow") ||
        !vita_crt_copy_ascii(
            c, source, text, ISAAC_VITA_CRT_CONVERT_TEXT_MAX,
            "strtoull received a null guest pointer",
            "strtoull input is unreadable or exceeds 4095 bytes"))
        return;
    errno = 0;
    value = strtoull(text, &native_end, base);
    conversion_errno = errno;
    errno = saved_errno;
    end_offset = native_end - text;
    if (end_offset < 0 ||
        (uint32_t)end_offset > ISAAC_VITA_CRT_CONVERT_TEXT_MAX) {
        guest_fault(c, source,
                    "strtoull native end pointer escaped copied input");
        return;
    }
    if (conversion_errno)
        g_isaac_vita_crt_errno = conversion_errno;
    if (endptr)
        st32(endptr, source + (uint32_t)end_offset);
    c->eax = (uint32_t)value;
    c->edx = (uint32_t)(value >> 32U);
    vita_crt_cdecl_return(c);
}

static void vita_crt_strtol(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    uint32_t endptr = vita_crt_arg(c, 1U);
    int base = (int)vita_crt_arg(c, 2U);
    char text[ISAAC_VITA_CRT_CONVERT_TEXT_MAX + 1U];
    char *native_end;
    long value;
    ptrdiff_t end_offset;
    int conversion_errno;
    int saved_errno = errno;

    if (!vita_crt_convert_base_valid(base)) {
        guest_fault(c, (uint32_t)base, "strtol received an invalid base");
        return;
    }
    if (!vita_crt_convert_endptr_valid(
            c, endptr, "strtol end pointer range overflow") ||
        !vita_crt_copy_ascii(
            c, source, text, ISAAC_VITA_CRT_CONVERT_TEXT_MAX,
            "strtol received a null guest pointer",
            "strtol input is unreadable or exceeds 4095 bytes"))
        return;
    errno = 0;
    value = strtol(text, &native_end, base);
    conversion_errno = errno;
    errno = saved_errno;
    end_offset = native_end - text;
    if (end_offset < 0 ||
        (uint32_t)end_offset > ISAAC_VITA_CRT_CONVERT_TEXT_MAX) {
        guest_fault(c, source,
                    "strtol native end pointer escaped copied input");
        return;
    }
    if (conversion_errno)
        g_isaac_vita_crt_errno = conversion_errno;
    if (endptr)
        st32(endptr, source + (uint32_t)end_offset);
    c->eax = (uint32_t)(int32_t)value;
    vita_crt_cdecl_return(c);
}

static void vita_crt_atoi(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    char text[ISAAC_VITA_CRT_CONVERT_TEXT_MAX + 1U];
    long value;
    int conversion_errno;
    int saved_errno = errno;

    if (!vita_crt_copy_ascii(
            c, source, text, ISAAC_VITA_CRT_CONVERT_TEXT_MAX,
            "atoi received a null guest pointer",
            "atoi input is unreadable or exceeds 4095 bytes"))
        return;
    errno = 0;
    value = strtol(text, NULL, 10);
    conversion_errno = errno;
    errno = saved_errno;
    if (conversion_errno == ERANGE) {
        g_isaac_vita_crt_errno = ERANGE;
    } else if (conversion_errno) {
        guest_fault(c, (uint32_t)conversion_errno,
                    "atoi produced an unexpected conversion error");
        return;
    }
    c->eax = (uint32_t)(int32_t)value;
    vita_crt_cdecl_return(c);
}

static void vita_crt_atof(CPU *__restrict c)
{
    uint32_t source = vita_crt_arg(c, 0U);
    char text[ISAAC_VITA_CRT_CONVERT_TEXT_MAX + 1U];
    double value;
    int saved_errno = errno;

    if (!vita_crt_copy_ascii(
            c, source, text, ISAAC_VITA_CRT_CONVERT_TEXT_MAX,
            "atof received a null guest pointer",
            "atof input is unreadable or exceeds 4095 bytes"))
        return;
#if defined(ISAAC_VITA_CRT_ATOF_SMALLINT) && ISAAC_VITA_CRT_ATOF_SMALLINT
    if (!isaac_vita_atof_smallint(text, &value))
#endif
    value = strtod(text, NULL);
    errno = saved_errno;
    fpush(c, value);
    vita_crt_cdecl_return(c);
}

static void vita_crt_mbstowcs_s(CPU *__restrict c)
{
    uint32_t converted_out = vita_crt_arg(c, 0U);
    uint32_t dest = vita_crt_arg(c, 1U);
    uint32_t capacity = vita_crt_arg(c, 2U);
    uint32_t source = vita_crt_arg(c, 3U);
    uint32_t maximum = vita_crt_arg(c, 4U);
    uint32_t limit;
    uint32_t length = 0U;
    uint32_t i;
    int truncated = 0;

    if (converted_out)
        st32(converted_out, 0U);
    if (!source || (!dest && capacity)) {
        if (dest && capacity)
            st16(dest, 0U);
        c->eax = EINVAL;
        vita_crt_cdecl_return(c);
        return;
    }
    limit = maximum == UINT32_MAX
        ? (capacity ? capacity - 1U : 0U) : maximum;
    while (length < limit) {
        uint8_t value = ld8(source + length);
        if (!value)
            break;
        ++length;
    }
    if (maximum == UINT32_MAX && ld8(source + length) != 0U)
        truncated = 1;
    if (length + 1U > capacity && dest) {
        if (capacity)
            st16(dest, 0U);
        c->eax = ERANGE;
        vita_crt_cdecl_return(c);
        return;
    }
    if (dest) {
        for (i = 0U; i < length; ++i)
            st16(dest + i * 2U, (uint16_t)ld8(source + i));
        st16(dest + length * 2U, 0U);
    }
    if (converted_out)
        st32(converted_out, length + 1U);
    c->eax = truncated ? 80U : 0U;
    vita_crt_cdecl_return(c);
}

static int vita_crt_wcstombs_load(CPU *__restrict c, uint32_t source,
                                  uint32_t index, uint16_t *value)
{
    uint32_t offset;

    if (index >= ISAAC_VITA_CRT_WCSTOMBS_S_SOURCE_UNITS_MAX) {
        guest_fault(c, source,
                    "wcstombs_s source exceeds WIN32_FIND_DATAW bound");
        return 0;
    }
    if (source > UINT32_MAX - 1U ||
        index > (UINT32_MAX - source - 1U) / 2U) {
        guest_fault(c, source, "wcstombs_s source range overflow");
        return 0;
    }
    offset = index * 2U;
    *value = ld16(source + offset);
    return 1;
}

static void vita_crt_wcstombs_error(CPU *__restrict c,
                                    uint32_t converted_out,
                                    uint32_t dest, uint32_t capacity,
                                    uint32_t error)
{
    if (converted_out)
        st32(converted_out, 0U);
    if (dest && capacity)
        st8(dest, 0U);
    c->eax = error;
    vita_crt_cdecl_return(c);
}

static void vita_crt_wcstombs_s(CPU *__restrict c)
{
    uint32_t converted_out = vita_crt_arg(c, 0U);
    uint32_t dest = vita_crt_arg(c, 1U);
    uint32_t capacity = vita_crt_arg(c, 2U);
    uint32_t source = vita_crt_arg(c, 3U);
    uint32_t count = vita_crt_arg(c, 4U);
    uint32_t length = 0U;
    uint32_t limit;
    uint32_t i;
    int truncate = count == UINT32_MAX;
    int truncated = 0;

    if (converted_out && converted_out > UINT32_MAX - 3U) {
        guest_fault(c, converted_out,
                    "wcstombs_s converted-count range overflow");
        return;
    }
    if (dest && capacity && dest > UINT32_MAX - (capacity - 1U)) {
        guest_fault(c, dest, "wcstombs_s destination range overflow");
        return;
    }
    if (converted_out)
        st32(converted_out, 0U);
    if (!source || (!dest && capacity)) {
        vita_crt_wcstombs_error(c, converted_out, dest, capacity, EINVAL);
        return;
    }

    /* UCRT query mode ignores count and measures the complete source. */
    if (!dest) {
        for (;;) {
            uint16_t value;
            if (!vita_crt_wcstombs_load(c, source, length, &value))
                return;
            if (!value)
                break;
            if (value > 0xffU) {
                vita_crt_wcstombs_error(
                    c, converted_out, dest, capacity, EILSEQ);
                return;
            }
            ++length;
        }
        if (converted_out)
            st32(converted_out, length + 1U);
        c->eax = 0U;
        vita_crt_cdecl_return(c);
        return;
    }

    if (!capacity) {
        vita_crt_wcstombs_error(c, converted_out, dest, capacity, ERANGE);
        return;
    }
    limit = truncate ? capacity - 1U : count;
    while (length < limit) {
        uint16_t value;
        if (!vita_crt_wcstombs_load(c, source, length, &value))
            return;
        if (!value)
            break;
        if (value > 0xffU) {
            vita_crt_wcstombs_error(
                c, converted_out, dest, capacity, EILSEQ);
            return;
        }
        ++length;
    }
    if (truncate && length == limit) {
        uint16_t next;
        if (!vita_crt_wcstombs_load(c, source, length, &next))
            return;
        truncated = next != 0U;
    }
    if (length + 1U > capacity) {
        vita_crt_wcstombs_error(c, converted_out, dest, capacity, ERANGE);
        return;
    }
    for (i = 0U; i < length; ++i)
        st8(dest + i, (uint8_t)ld16(source + i * 2U));
    st8(dest + length, 0U);
    if (converted_out)
        st32(converted_out, length + 1U);
    c->eax = truncated ? ISAAC_VITA_CRT_WCSTOMBS_S_STRUNCATE : 0U;
    vita_crt_cdecl_return(c);
}

enum {
    VITA_CRT_FREAD_DISPATCH_INDEX = 0,
    VITA_CRT_FSEEK_DISPATCH_INDEX = 1
};

/* Archive startup performs about 2,070,715 fread calls and 21,301 fseek
 * calls.  Pin those two immutable-name handlers at one and two comparisons;
 * a mutable hot cache would add a race to the file lock it is meant to feed.
 * The remaining entries retain their earlier logical family order. */
static const vita_crt_import_entry s_vita_crt_imports[] = {
    [VITA_CRT_FREAD_DISPATCH_INDEX] =
        { ISAAC_VITA_CRT_FREAD_NAME, vita_crt_fread },
    [VITA_CRT_FSEEK_DISPATCH_INDEX] =
        { ISAAC_VITA_CRT_FSEEK_NAME, vita_crt_fseek },
    { ISAAC_VITA_CRT_INITTERM_E_NAME,            vita_crt_initterm_e },
    { ISAAC_VITA_CRT_SET_APP_TYPE_NAME,          vita_crt_set_app_type },
    { ISAAC_VITA_CRT_SET_FMODE_NAME,             vita_crt_set_fmode },
    { ISAAC_VITA_CRT_P_COMMODE_NAME,             vita_crt_p_commode },
    { ISAAC_VITA_CRT_ATEXIT_NAME,                 vita_crt_atexit },
    { ISAAC_VITA_CRT_CONFIGURE_ARGV_NAME,         vita_crt_configure_narrow_argv },
    { ISAAC_VITA_CRT_INITIALIZE_SLIST_NAME,       vita_crt_InitializeSListHead },
    { ISAAC_VITA_CRT_CONTROLFP_S_NAME,            vita_crt_controlfp_s },
    { ISAAC_VITA_CRT_CONFIGTHREADLOCALE_NAME,     vita_crt_configthreadlocale },
    { ISAAC_VITA_CRT_INITIALIZE_ENVIRONMENT_NAME, vita_crt_initialize_narrow_environment },
    { ISAAC_VITA_CRT_INITTERM_NAME,              vita_crt_initterm },
    { ISAAC_VITA_CRT_GET_INITIAL_ENVIRONMENT_NAME, vita_crt_get_initial_narrow_environment },
    { ISAAC_VITA_CRT_P_ARGV_NAME,                vita_crt_p_argv },
    { ISAAC_VITA_CRT_P_ARGC_NAME,                vita_crt_p_argc },
    { ISAAC_VITA_CRT_GETENV_NAME,                vita_crt_getenv },
    { ISAAC_VITA_CRT_VSPRINTF_NAME,              vita_crt_stdio_common_vsprintf },
    { ISAAC_VITA_CRT_FOPEN_NAME,                 vita_crt_fopen },
    { ISAAC_VITA_CRT_VFPRINTF_NAME,              vita_crt_stdio_common_vfprintf },
    { ISAAC_VITA_CRT_FCLOSE_NAME,                vita_crt_fclose },
    { ISAAC_VITA_CRT_STRNCPY_NAME,               vita_crt_strncpy },
    { ISAAC_VITA_CRT_STRNCPY_S_NAME,             vita_crt_strncpy_s },
    { ISAAC_VITA_CRT_STRDUP_NAME,                isaac_vita_crt_strdup },
    { ISAAC_VITA_CRT_MBSTOWCS_S_NAME,            vita_crt_mbstowcs_s },
    { ISAAC_VITA_CRT_WCSTOMBS_S_NAME,            vita_crt_wcstombs_s },
    { ISAAC_VITA_CRT_VSPRINTF_S_NAME,             vita_crt_stdio_common_vsprintf_s },
    { ISAAC_VITA_CRT_STRNCMP_NAME,                vita_crt_strncmp },
    { ISAAC_VITA_CRT_STRPBRK_NAME,                vita_crt_strpbrk },
    { ISAAC_VITA_CRT_ISPUNCT_NAME,                vita_crt_ispunct },
    { ISAAC_VITA_CRT_TOLOWER_NAME,                vita_crt_tolower },
    { ISAAC_VITA_CRT_STRNICMP_NAME,               vita_crt_strnicmp },
    { ISAAC_VITA_CRT_ISDIGIT_NAME,                vita_crt_isdigit },
    { ISAAC_VITA_CRT_ISWSPACE_NAME,               vita_crt_iswspace },
    { ISAAC_VITA_CRT_TOUPPER_NAME,                vita_crt_toupper },
    { ISAAC_VITA_CRT_STRCAT_S_NAME,               vita_crt_strcat_s },
    { ISAAC_VITA_CRT_STRCPY_S_NAME,               vita_crt_strcpy_s },
    { ISAAC_VITA_CRT_ISSPACE_NAME,                vita_crt_isspace },
    { ISAAC_VITA_CRT_VSSCANF_NAME,                vita_crt_stdio_common_vsscanf },
    { ISAAC_VITA_CRT_ERRNO_NAME,                  vita_crt_errno },
    { ISAAC_VITA_CRT_SET_ERRNO_NAME,              vita_crt_set_errno },
    { ISAAC_VITA_CRT_STRFTIME_NAME,               vita_crt_strftime },
    { ISAAC_VITA_CRT_GMTIME64_NAME,               vita_crt_gmtime64 },
    { ISAAC_VITA_CRT_TIME64_NAME,                 vita_crt_time64 },
    { ISAAC_VITA_CRT_MKGMTIME64_NAME,             vita_crt_mkgmtime64 },
    { ISAAC_VITA_CRT_LOCALTIME64_NAME,            vita_crt_localtime64 },
    { ISAAC_VITA_CRT_QSORT_NAME,                  isaac_vita_crt_qsort },
    { ISAAC_VITA_CRT_FILENO_NAME,                 vita_crt_fileno },
    { ISAAC_VITA_CRT_FWRITE_NAME,                 vita_crt_fwrite },
    { ISAAC_VITA_CRT_GET_OSFHANDLE_NAME,          vita_crt_get_osfhandle },
    { ISAAC_VITA_CRT_FTELL_NAME,                  vita_crt_ftell },
    { ISAAC_VITA_CRT_FFLUSH_NAME,                 vita_crt_fflush },
    { ISAAC_VITA_CRT_ACRT_IOB_NAME,               vita_crt_acrt_iob_func },
    { ISAAC_VITA_CRT_STRTOULL_NAME,               vita_crt_strtoull },
    { ISAAC_VITA_CRT_STRTOL_NAME,                 vita_crt_strtol },
    { ISAAC_VITA_CRT_ATOI_NAME,                   vita_crt_atoi },
    { ISAAC_VITA_CRT_ATOF_NAME,                   vita_crt_atof },
    { ISAAC_VITA_CRT_TLS_EXIT_REGISTER_NAME,      vita_crt_register_tls_atexit },
    { ISAAC_VITA_CRT__EXIT_NAME,                  vita_crt__exit },
    { ISAAC_VITA_CRT_EXIT_NAME,                   vita_crt_exit }
};

_Static_assert(sizeof s_vita_crt_imports / sizeof s_vita_crt_imports[0] ==
               ISAAC_VITA_CRT_IMPORT_COUNT,
               "Vita CRT import count drifted from measured batch");
_Static_assert(VITA_CRT_FREAD_DISPATCH_INDEX == 0 &&
               VITA_CRT_FSEEK_DISPATCH_INDEX == 1,
               "archive hot stdio handlers left the dispatch prefix");
_Static_assert(ISAAC_VITA_CRT_SCAN_IMPORT_COUNT == 1U &&
               ISAAC_VITA_CRT_VSSCANF_IAT_RVA ==
                   ISAAC_VITA_CRT_SET_FMODE_IAT_RVA + 4U &&
               ISAAC_VITA_CRT_VSPRINTF_IAT_RVA ==
                   ISAAC_VITA_CRT_VSSCANF_IAT_RVA + 4U,
               "frozen PE scanf-family IAT map drifted");
_Static_assert(ISAAC_VITA_CRT_ERRNO_FAMILY_IMPORT_COUNT == 2U &&
               ISAAC_VITA_CRT_DOSERRNO_IMPORT_COUNT == 0U &&
               ISAAC_VITA_CRT_ERRNO_DIRECT_CALL_COUNT == 2U &&
               ISAAC_VITA_CRT_SET_ERRNO_DIRECT_CALL_COUNT == 5U &&
               ISAAC_VITA_CRT_ERRNO_DIRECT_CALL_COUNT +
                   ISAAC_VITA_CRT_SET_ERRNO_DIRECT_CALL_COUNT ==
                   ISAAC_VITA_CRT_ERRNO_FAMILY_DIRECT_CALL_COUNT,
               "frozen PE errno/doserrno census drifted");
_Static_assert(ISAAC_VITA_CRT_TIME_IMPORT_COUNT == 5U &&
               ISAAC_VITA_CRT_STRFTIME_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_GMTIME64_IAT_RVA &&
               ISAAC_VITA_CRT_GMTIME64_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_TIME64_IAT_RVA &&
               ISAAC_VITA_CRT_TIME64_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_MKGMTIME64_IAT_RVA &&
               ISAAC_VITA_CRT_MKGMTIME64_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_LOCALTIME64_IAT_RVA &&
               ISAAC_VITA_CRT_STRFTIME_CALL_COUNT +
                   ISAAC_VITA_CRT_GMTIME64_CALL_COUNT +
                   ISAAC_VITA_CRT_TIME64_CALL_COUNT +
                   ISAAC_VITA_CRT_MKGMTIME64_CALL_COUNT +
                   ISAAC_VITA_CRT_LOCALTIME64_CALL_COUNT ==
                   ISAAC_VITA_CRT_TIME_CALL_COUNT &&
               ISAAC_VITA_CRT_TIME64_NULL_CALL_COUNT ==
                   ISAAC_VITA_CRT_TIME64_CALL_COUNT &&
               ISAAC_VITA_CRT_TIME64_FRONTIER_BEFORE_COUNT + 1U ==
               ISAAC_VITA_CRT_TIME64_FRONTIER_ORDINAL,
               "frozen PE time-family census drifted");
_Static_assert(ISAAC_VITA_CRT_QSORT_IMPORT_COUNT == 1U &&
               ISAAC_VITA_CRT_QSORT_DIRECT_CALL_COUNT == 7U &&
               ISAAC_VITA_CRT_QSORT_REGISTER_LOAD_COUNT == 0U &&
               ISAAC_VITA_CRT_QSORT_REGISTER_CALL_COUNT == 0U &&
               ISAAC_VITA_CRT_QSORT_DIRECT_CALL_COUNT +
                   ISAAC_VITA_CRT_QSORT_REGISTER_CALL_COUNT ==
                   ISAAC_VITA_CRT_QSORT_PHYSICAL_CALL_COUNT &&
               ISAAC_VITA_CRT_QSORT_COMPARATOR_COUNT == 6U &&
               ISAAC_VITA_CRT_QSORT_IAT_RVA == 0x00606678U &&
               ISAAC_VITA_CRT_QSORT_IAT_VA ==
                   GUEST_IMAGE_BASE + ISAAC_VITA_CRT_QSORT_IAT_RVA,
               "frozen PE qsort census drifted");
_Static_assert(ISAAC_VITA_CRT_STDIO_IMPORT_COUNT == 16U &&
               ISAAC_VITA_CRT_FILE_IO_IMPORT_COUNT == 8U &&
               ISAAC_VITA_CRT_FILE_TOKEN_COUNT == 16U &&
               ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE == 16U * 1024U &&
               ISAAC_VITA_CRT_FILENO_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FREAD_IAT_RVA &&
               ISAAC_VITA_CRT_FREAD_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FWRITE_IAT_RVA &&
               ISAAC_VITA_CRT_FWRITE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FSEEK_IAT_RVA &&
               ISAAC_VITA_CRT_FSEEK_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_GET_OSFHANDLE_IAT_RVA &&
               ISAAC_VITA_CRT_GET_OSFHANDLE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FTELL_IAT_RVA &&
               ISAAC_VITA_CRT_FTELL_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_P_COMMODE_IAT_RVA &&
               ISAAC_VITA_CRT_P_COMMODE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VFPRINTF_IAT_RVA &&
               ISAAC_VITA_CRT_VFPRINTF_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FFLUSH_IAT_RVA &&
               ISAAC_VITA_CRT_FFLUSH_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_ACRT_IOB_IAT_RVA &&
               ISAAC_VITA_CRT_ACRT_IOB_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FOPEN_IAT_RVA &&
               ISAAC_VITA_CRT_FOPEN_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_SET_FMODE_IAT_RVA &&
               ISAAC_VITA_CRT_SET_FMODE_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VSSCANF_IAT_RVA &&
               ISAAC_VITA_CRT_VSSCANF_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VSPRINTF_IAT_RVA &&
               ISAAC_VITA_CRT_VSPRINTF_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_VSPRINTF_S_IAT_RVA &&
               ISAAC_VITA_CRT_VSPRINTF_S_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_FCLOSE_IAT_RVA &&
               ISAAC_VITA_CRT_FILENO_CALL_COUNT +
                   ISAAC_VITA_CRT_FREAD_CALL_COUNT +
                   ISAAC_VITA_CRT_FWRITE_CALL_COUNT +
                   ISAAC_VITA_CRT_FSEEK_CALL_COUNT +
                   ISAAC_VITA_CRT_GET_OSFHANDLE_CALL_COUNT +
                   ISAAC_VITA_CRT_FTELL_CALL_COUNT +
                   ISAAC_VITA_CRT_FFLUSH_CALL_COUNT +
                   ISAAC_VITA_CRT_ACRT_IOB_CALL_COUNT ==
                   ISAAC_VITA_CRT_FILE_IO_CALL_COUNT,
               "frozen PE stdio/file-I/O census drifted");
_Static_assert(
               ISAAC_VITA_CRT_FREAD_RETURN_RVA == 0x00596581U &&
               ISAAC_VITA_CRT_ARCHIVE_TYPE1_HEADER_RETURN_RVA ==
                   0x0059c2ceU &&
               ISAAC_VITA_CRT_ARCHIVE_TYPE1_PAYLOAD_RETURN_RVA ==
                   0x0059c300U &&
               ISAAC_VITA_CRT_ARCHIVE_TYPE2_HEADER_RETURN_RVA ==
                   0x0059c33aU &&
               ISAAC_VITA_CRT_ARCHIVE_TYPE2_PAYLOAD_RETURN_RVA ==
                   0x0059c383U &&
               ISAAC_VITA_CRT_ARCHIVE_FSEEK_RETURN_RVA == 0x0059654dU &&
               ISAAC_VITA_CRT_ARCHIVE_SEEK_PARENT_RETURN_RVA ==
                   0x0059c269U &&
               ISAAC_VITA_CRT_ARCHIVE_BLOCK_MAX_BYTES == 0x800U,
               "frozen ArchivedFile adapter/caller contract drifted");
_Static_assert(ISAAC_VITA_CRT_CONVERT_IMPORT_COUNT == 6U &&
               ISAAC_VITA_CRT_NUMERIC_CONVERT_IMPORT_COUNT == 4U &&
               ISAAC_VITA_CRT_STRTOULL_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_MBSTOWCS_S_IAT_RVA &&
               ISAAC_VITA_CRT_MBSTOWCS_S_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_WCSTOMBS_S_IAT_RVA &&
               ISAAC_VITA_CRT_WCSTOMBS_S_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_STRTOL_IAT_RVA &&
               ISAAC_VITA_CRT_STRTOL_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_ATOI_IAT_RVA &&
               ISAAC_VITA_CRT_ATOI_IAT_RVA + 4U ==
                   ISAAC_VITA_CRT_ATOF_IAT_RVA &&
               ISAAC_VITA_CRT_ATOI_DIRECT_CALL_COUNT +
                   ISAAC_VITA_CRT_ATOI_REGISTER_CALL_COUNT ==
                   ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_COUNT &&
               ISAAC_VITA_CRT_STRTOULL_DIRECT_CALL_COUNT +
                   ISAAC_VITA_CRT_STRTOL_DIRECT_CALL_COUNT +
                   ISAAC_VITA_CRT_ATOI_PHYSICAL_CALL_COUNT +
                   ISAAC_VITA_CRT_ATOF_DIRECT_CALL_COUNT ==
                   ISAAC_VITA_CRT_NUMERIC_CONVERT_CALL_COUNT,
               "frozen PE convert-DLL census drifted");
_Static_assert(ISAAC_VITA_CRT_FIXED_FIRST_ORDINAL +
               ISAAC_VITA_CRT_FIXED_CALL_COUNT ==
               ISAAC_VITA_CRT_FIXED_NEXT_ATTEMPT_ORDINAL,
               "Vita fixed save/walk denominator drifted");
_Static_assert(ISAAC_VITA_CRT_FIXED_OWNED_CALL_COUNT +
               ISAAC_VITA_CRT_FIXED_STARTUP_CALL_COUNT +
               ISAAC_VITA_CRT_FIXED_HEAP_CALL_COUNT +
               ISAAC_VITA_CRT_FIXED_MEMORY_CALL_COUNT +
               ISAAC_VITA_CRT_FIXED_SYNC_CALL_COUNT +
               ISAAC_VITA_CRT_FIXED_OTHER_CALL_COUNT ==
               ISAAC_VITA_CRT_FIXED_CALL_COUNT,
               "Vita fixed save/walk category count drifted");

const char *isaac_vita_crt_import_name(uint32_t index)
{
    return index < ISAAC_VITA_CRT_IMPORT_COUNT
        ? s_vita_crt_imports[index].name : NULL;
}

int isaac_vita_crt_import_indexed(CPU *__restrict c, uint32_t index,
                                  unsigned *call_count)
{
    if (index >= ISAAC_VITA_CRT_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count;
    s_vita_crt_imports[index].fn(c);
    return 1;
}

static int vita_crt_dispatch(CPU *__restrict c, const char *name,
                             unsigned *call_count)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_CRT_IMPORT_COUNT; ++i) {
        if (strcmp(s_vita_crt_imports[i].name, name) == 0)
            return isaac_vita_crt_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_crt_import(CPU *__restrict c, const char *name)
{
    return vita_crt_dispatch(c, name, NULL);
}

int isaac_vita_crt_import_counted(CPU *__restrict c, const char *name,
                                  unsigned *call_count)
{
    return vita_crt_dispatch(c, name, call_count);
}
