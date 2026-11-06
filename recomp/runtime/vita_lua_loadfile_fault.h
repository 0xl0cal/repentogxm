/* Host oracle only (recomp/test_vita_lua_loadfile_retry.py): a newlib-shaped
 * stdio over a fault-injecting fake sceIo kernel, for ISAAC_VITA_LUA_LOADFILE_RETRY.
 *
 * Three layers, each modelled on the vitasdk newlib 0da3cb1 sources fetched
 * for the read-failure diagnosis:
 *   stdio    lfo_fopen/freopen/fclose/fread/getc/feof/ferror/stdin: one
 *            BUFSIZ buffer per FILE; the refill (refill.c:52, :117-133)
 *            returns EOF early only on __SEOF, so a FILE that already has
 *            __SERR reads again; fclose runs _close_r; freopen closes the
 *            old descriptor and keeps the open's errno on failure.
 *   syscalls lfo_open_r/read_r/close_r (libc/sys/vita/syscalls.c): a
 *            directory opens without the kernel (sceIoDopen) and its reads
 *            return __vita_make_sce_errno(EBADF) (:336-337); a negative read
 *            sets errno = code & SCE_ERRNO_MASK unless it is -1 (:350-355),
 *            a successful read and a successful open or close set errno = 0
 *            (:357, :309, :97).
 *   kernel   __real_sceIoOpen/Read/Close over host files, with the faults of
 *            lfo_fault_add().  In ISAAC_VITA_LUA_LOADFILE_IO_PROBE builds the
 *            syscall layer calls __wrap_sceIo* (host_vita_lua_loadfile.c's
 *            hooks), exactly as GNU ld --wrap routes newlib's calls on the
 *            Vita.
 *
 * Use:
 *   - force-included into lauxlib.c with VITA_LUA_LOADFILE_FAULT_STDIO
 *     defined, so luaL_loadfilex's FILE calls land here (the header includes
 *     the standard headers first, then renames);
 *   - included by host_vita_lua_loadfile.c (ISAAC_VITA_LUA_LOADFILE_ORACLE)
 *     for SceUID/SceMode, the __real_sceIo* prototypes and stat/S_ISDIR;
 *   - included once with VITA_LUA_LOADFILE_FAULT_IMPL by the oracle main,
 *     which then holds the definitions. */
#ifndef VITA_LUA_LOADFILE_FAULT_H
#define VITA_LUA_LOADFILE_FAULT_H

#if defined(__vita__)
#error "vita_lua_loadfile_fault.h is a host-oracle header (test_vita_lua_loadfile_retry.py)"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#if defined(_MSC_VER)
/* /std:c11 sets __STDC__, which hides the POSIX names of <sys/stat.h>. */
#define stat _stat
#if !defined(S_ISDIR)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#endif

/* vitasdk newlib libc/sys/vita/vitaerror.h:29-30 (copied from newlib
 * 0da3cb1). */
#define LFO_SCE_ERRNO_MASK 0xFF
#define LFO_SCE_ERRNO_NONE 0x80010000

typedef int SceUID;
typedef int SceMode;

SceUID __real_sceIoOpen(const char *path, int flags, SceMode mode);
int __real_sceIoRead(SceUID uid, void *buffer, unsigned int size);
int __real_sceIoClose(SceUID uid);
int __vita_make_sce_errno(int posix_errno);

typedef struct lfo_FILE lfo_FILE;
lfo_FILE *lfo_fopen(const char *path, const char *mode);
lfo_FILE *lfo_freopen(const char *path, const char *mode, lfo_FILE *file);
int lfo_fclose(lfo_FILE *file);
size_t lfo_fread(void *buffer, size_t size, size_t count, lfo_FILE *file);
int lfo_getc(lfo_FILE *file);
int lfo_feof(lfo_FILE *file);
int lfo_ferror(lfo_FILE *file);
lfo_FILE *lfo_stdin(void);

/* Faults, matched by path suffix and counted per sceIoOpen of a matching
 * path since the last lfo_fault_clear().  An entry applies to the open with
 * 1-based ordinal >= first_open while it has fires left (-1 = unlimited):
 *   LFO_READ_AT k     that open's reads deliver bytes [0, k) and every read
 *                     at position >= k returns __vita_make_sce_errno(err)
 *                     (one fire per open that reaches k);
 *   LFO_OPEN_FAIL     sceIoOpen returns __vita_make_sce_errno(err);
 *   LFO_TRUNCATE_AT k that open sees end of file at k (one fire per open). */
enum { LFO_READ_AT = 1, LFO_OPEN_FAIL = 2, LFO_TRUNCATE_AT = 3 };
void lfo_fault_clear(void);
void lfo_fault_add(const char *suffix, int kind, long at, int err,
                   unsigned first_open, int fires);
unsigned lfo_fault_fired(void);
unsigned lfo_kernel_opens(void);
unsigned lfo_kernel_live(void);
/* Called (when set) after each successful kernel close of a uid whose
 * fault fired: the oracle's stale-arm case arms an allocation failure. */
void lfo_set_close_hook(void (*hook)(void));

#if defined(VITA_LUA_LOADFILE_FAULT_STDIO)
#undef FILE
#undef fopen
#undef freopen
#undef fclose
#undef fread
#undef getc
#undef feof
#undef ferror
#undef stdin
#define FILE lfo_FILE
#define fopen lfo_fopen
#define freopen lfo_freopen
#define fclose lfo_fclose
#define fread lfo_fread
#define getc lfo_getc
#define feof lfo_feof
#define ferror lfo_ferror
#define stdin lfo_stdin()
#endif

#if defined(VITA_LUA_LOADFILE_FAULT_IMPL)
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
SceUID __wrap_sceIoOpen(const char *path, int flags, SceMode mode);
int __wrap_sceIoRead(SceUID uid, void *buffer, unsigned int size);
int __wrap_sceIoClose(SceUID uid);
#define LFO_IO_OPEN __wrap_sceIoOpen
#define LFO_IO_READ __wrap_sceIoRead
#define LFO_IO_CLOSE __wrap_sceIoClose
#else
#define LFO_IO_OPEN __real_sceIoOpen
#define LFO_IO_READ __real_sceIoRead
#define LFO_IO_CLOSE __real_sceIoClose
#endif

#define LFO_FAULTS 8
#define LFO_UIDS 16
#define LFO_FDS 16
#define LFO_UID_BASE 0x00410000
#define LFO_SCE_O_RDONLY 0x0001

int __vita_make_sce_errno(int posix_errno)
{
    return (int)(LFO_SCE_ERRNO_NONE | (unsigned)posix_errno);
}

/* ---- kernel ---- */
static struct {
    char suffix[160];
    int kind;
    long at;
    int err;
    unsigned first_open;
    int fires;
    unsigned seen;
} s_lfo_fault[LFO_FAULTS];
static unsigned s_lfo_faults;
static unsigned s_lfo_fired;
static unsigned s_lfo_opens;
static void (*s_lfo_close_hook)(void);

static struct {
    FILE *host;
    long pos;
    long read_at;      /* -1: none */
    int read_err;
    int read_fault;    /* index into s_lfo_fault, -1: none */
    int read_fired;
    long truncate_at;  /* -1: none */
} s_lfo_uid[LFO_UIDS];

void lfo_fault_clear(void)
{
    memset(s_lfo_fault, 0, sizeof s_lfo_fault);
    s_lfo_faults = 0U;
    s_lfo_fired = 0U;
}

void lfo_fault_add(const char *suffix, int kind, long at, int err,
                   unsigned first_open, int fires)
{
    if (s_lfo_faults >= LFO_FAULTS || strlen(suffix) >= 160U)
        abort();
    strcpy(s_lfo_fault[s_lfo_faults].suffix, suffix);
    s_lfo_fault[s_lfo_faults].kind = kind;
    s_lfo_fault[s_lfo_faults].at = at;
    s_lfo_fault[s_lfo_faults].err = err;
    s_lfo_fault[s_lfo_faults].first_open = first_open;
    s_lfo_fault[s_lfo_faults].fires = fires;
    s_lfo_fault[s_lfo_faults].seen = 0U;
    ++s_lfo_faults;
}

unsigned lfo_fault_fired(void) { return s_lfo_fired; }
unsigned lfo_kernel_opens(void) { return s_lfo_opens; }
void lfo_set_close_hook(void (*hook)(void)) { s_lfo_close_hook = hook; }

unsigned lfo_kernel_live(void)
{
    unsigned live = 0U, index;
    for (index = 0U; index < LFO_UIDS; ++index)
        live += s_lfo_uid[index].host != NULL;
    return live;
}

static int lfo_suffix_matches(const char *path, const char *suffix)
{
    size_t length = strlen(path), tail = strlen(suffix);
    return length >= tail && strcmp(path + length - tail, suffix) == 0;
}

static void lfo_fire(unsigned index)
{
    ++s_lfo_fired;
    if (s_lfo_fault[index].fires > 0)
        --s_lfo_fault[index].fires;
}

SceUID __real_sceIoOpen(const char *path, int flags, SceMode mode)
{
    long read_at = -1, truncate_at = -1;
    int read_err = 0, read_fault = -1;
    unsigned index;
    FILE *host;

    (void)flags;
    (void)mode;
    ++s_lfo_opens;
    for (index = 0U; index < s_lfo_faults; ++index) {
        if (!lfo_suffix_matches(path, s_lfo_fault[index].suffix))
            continue;
        ++s_lfo_fault[index].seen;
        if (s_lfo_fault[index].seen < s_lfo_fault[index].first_open ||
            s_lfo_fault[index].fires == 0)
            continue;
        switch (s_lfo_fault[index].kind) {
        case LFO_OPEN_FAIL:
            lfo_fire(index);
            return __vita_make_sce_errno(s_lfo_fault[index].err);
        case LFO_READ_AT:
            read_at = s_lfo_fault[index].at;
            read_err = s_lfo_fault[index].err;
            read_fault = (int)index;
            break;
        case LFO_TRUNCATE_AT:
            truncate_at = s_lfo_fault[index].at;
            lfo_fire(index);
            break;
        default:
            abort();
        }
    }
    host = fopen(path, "rb");
    if (!host)
        return __vita_make_sce_errno(errno);
    for (index = 0U; index < LFO_UIDS; ++index) {
        if (!s_lfo_uid[index].host) {
            s_lfo_uid[index].host = host;
            s_lfo_uid[index].pos = 0;
            s_lfo_uid[index].read_at = read_at;
            s_lfo_uid[index].read_err = read_err;
            s_lfo_uid[index].read_fault = read_fault;
            s_lfo_uid[index].read_fired = 0;
            s_lfo_uid[index].truncate_at = truncate_at;
            return (SceUID)(LFO_UID_BASE + (int)index);
        }
    }
    fclose(host);
    return __vita_make_sce_errno(EMFILE);
}

static int lfo_uid_index(SceUID uid)
{
    int index = (int)uid - LFO_UID_BASE;
    if (index < 0 || index >= LFO_UIDS || !s_lfo_uid[index].host)
        return -1;
    return index;
}

int __real_sceIoRead(SceUID uid, void *buffer, unsigned int size)
{
    int index = lfo_uid_index(uid);
    long want = (long)size;
    size_t got;

    if (index < 0)
        return __vita_make_sce_errno(EBADF);
    if (s_lfo_uid[index].read_at >= 0) {
        if (s_lfo_uid[index].pos >= s_lfo_uid[index].read_at) {
            if (!s_lfo_uid[index].read_fired) {
                s_lfo_uid[index].read_fired = 1;
                lfo_fire((unsigned)s_lfo_uid[index].read_fault);
            }
            return __vita_make_sce_errno(s_lfo_uid[index].read_err);
        }
        if (s_lfo_uid[index].pos + want > s_lfo_uid[index].read_at)
            want = s_lfo_uid[index].read_at - s_lfo_uid[index].pos;
    }
    if (s_lfo_uid[index].truncate_at >= 0) {
        if (s_lfo_uid[index].pos >= s_lfo_uid[index].truncate_at)
            return 0;
        if (s_lfo_uid[index].pos + want > s_lfo_uid[index].truncate_at)
            want = s_lfo_uid[index].truncate_at - s_lfo_uid[index].pos;
    }
    got = fread(buffer, 1, (size_t)want, s_lfo_uid[index].host);
    if (ferror(s_lfo_uid[index].host))
        return __vita_make_sce_errno(EIO);
    s_lfo_uid[index].pos += (long)got;
    return (int)got;
}

int __real_sceIoClose(SceUID uid)
{
    int index = lfo_uid_index(uid);
    int fired;

    if (index < 0)
        return __vita_make_sce_errno(EBADF);
    fired = s_lfo_uid[index].read_fired;
    fclose(s_lfo_uid[index].host);
    memset(&s_lfo_uid[index], 0, sizeof s_lfo_uid[index]);
    if (fired && s_lfo_close_hook)
        s_lfo_close_hook();
    return 0;
}

/* ---- syscalls (newlib libc/sys/vita/syscalls.c) ---- */
enum { LFO_FD_FREE = 0, LFO_FD_FILE, LFO_FD_DIR, LFO_FD_STDIN };
static struct {
    int type;
    SceUID uid;
} s_lfo_fd[LFO_FDS];

static int lfo_open_r(const char *path)
{
    struct stat status;
    int is_dir = stat(path, &status) == 0 && S_ISDIR(status.st_mode);
    SceUID uid = 0;
    int fd;

    if (!is_dir) {
        uid = LFO_IO_OPEN(path, LFO_SCE_O_RDONLY, 0666);
        if (uid < 0) {
            errno = uid & LFO_SCE_ERRNO_MASK;
            return -1;
        }
    }
    for (fd = 3; fd < LFO_FDS; ++fd) {
        if (s_lfo_fd[fd].type == LFO_FD_FREE) {
            s_lfo_fd[fd].type = is_dir ? LFO_FD_DIR : LFO_FD_FILE;
            s_lfo_fd[fd].uid = uid;
            errno = 0;
            return fd;
        }
    }
    if (!is_dir)
        LFO_IO_CLOSE(uid);
    errno = EMFILE;
    return -1;
}

static int lfo_read_r(int fd, void *buffer, size_t length)
{
    int ret;

    switch (s_lfo_fd[fd].type) {
    case LFO_FD_FILE:
        ret = LFO_IO_READ(s_lfo_fd[fd].uid, buffer, (unsigned int)length);
        break;
    case LFO_FD_DIR:
        ret = __vita_make_sce_errno(EBADF);
        break;
    case LFO_FD_STDIN:
        ret = (int)fread(buffer, 1, length, stdin);
        if (ferror(stdin))
            ret = __vita_make_sce_errno(EIO);
        break;
    default:
        errno = EBADF;
        return -1;
    }
    if (ret < 0) {
        if (ret != -1)
            errno = ret & LFO_SCE_ERRNO_MASK;
        return -1;
    }
    errno = 0;
    return ret;
}

static int lfo_close_r(int fd)
{
    int ret = 0;

    if (s_lfo_fd[fd].type == LFO_FD_FILE)
        ret = LFO_IO_CLOSE(s_lfo_fd[fd].uid);
    s_lfo_fd[fd].type = LFO_FD_FREE;
    if (ret < 0) {
        errno = EBADF;
        return -1;
    }
    errno = 0;
    return 0;
}

/* ---- stdio (newlib libc/stdio) ---- */
#define LFO_SEOF 1
#define LFO_SERR 2
struct lfo_FILE {
    int fd;
    int flags;
    size_t r;
    size_t p;
    unsigned char buffer[BUFSIZ];
};

lfo_FILE *lfo_fopen(const char *path, const char *mode)
{
    lfo_FILE *file;
    int fd;

    (void)mode;
    fd = lfo_open_r(path);
    if (fd < 0)
        return NULL;
    file = (lfo_FILE *)calloc(1, sizeof *file);
    if (!file)
        abort();
    file->fd = fd;
    return file;
}

lfo_FILE *lfo_freopen(const char *path, const char *mode, lfo_FILE *file)
{
    int fd;
    int saved;

    (void)mode;
    lfo_close_r(file->fd);
    fd = lfo_open_r(path);
    if (fd < 0) {
        saved = errno;
        free(file);
        errno = saved;
        return NULL;
    }
    file->fd = fd;
    file->flags = 0;
    file->r = 0U;
    file->p = 0U;
    return file;
}

int lfo_fclose(lfo_FILE *file)
{
    int ret = lfo_close_r(file->fd);
    free(file);
    return ret < 0 ? EOF : 0;
}

static int lfo_refill(lfo_FILE *file)
{
    int got;

    file->r = 0U;
    if (file->flags & LFO_SEOF)
        return EOF;
    got = lfo_read_r(file->fd, file->buffer, sizeof file->buffer);
    if (got <= 0) {
        if (got == 0)
            file->flags |= LFO_SEOF;
        else
            file->flags |= LFO_SERR;
        return EOF;
    }
    file->r = (size_t)got;
    file->p = 0U;
    return 0;
}

int lfo_getc(lfo_FILE *file)
{
    if (file->r == 0U && lfo_refill(file))
        return EOF;
    --file->r;
    return file->buffer[file->p++];
}

size_t lfo_fread(void *buffer, size_t size, size_t count, lfo_FILE *file)
{
    size_t want = size * count, done = 0U, step;
    unsigned char *out = (unsigned char *)buffer;

    if (want == 0U)
        return 0U;
    while (done < want) {
        if (file->r == 0U && lfo_refill(file))
            break;
        step = want - done < file->r ? want - done : file->r;
        memcpy(out + done, file->buffer + file->p, step);
        file->p += step;
        file->r -= step;
        done += step;
    }
    return done / size;
}

int lfo_feof(lfo_FILE *file) { return (file->flags & LFO_SEOF) != 0; }
int lfo_ferror(lfo_FILE *file) { return (file->flags & LFO_SERR) != 0; }

lfo_FILE *lfo_stdin(void)
{
    static lfo_FILE in;

    in.fd = 0;
    s_lfo_fd[0].type = LFO_FD_STDIN;
    return &in;
}
#endif

#endif
