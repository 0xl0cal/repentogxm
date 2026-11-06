/* ISAAC_VITA_LUA_LOADFILE_RETRY: retry a luaL_loadfilex read error.
 *
 * Bench run D4A4Pc (2026-10-26): one read of EID's
 * descriptions/ab+/cs_cz.lua failed inside pristine Lua 5.3.3
 * luaL_loadfilex, on a newlib FILE the guest CRT never sees.  lauxlib.c
 * reads strerror(errno) after fclose (:730, :733 -> :662) and newlib's
 * _close_r writes errno = 0 on success, so the game log said "cannot read
 * <path>: Success"; the ab+ language pack was never built and EID died at
 * eid_mcm.lua:81 for the whole run.
 *
 * GNU ld --wrap=luaL_loadfilex (recomp/vita/CMakeLists.txt) routes every
 * reference from another object here: the bridge import
 * (host_vita_lua.c vita_lua_loadfilex_import, which serves require, include
 * and LuaEngine::RunScript), lbaselib.c loadfile/dofile and loadlib.c
 * searcher_Lua.  lauxlib.c has no internal caller.  Attempt 1 is the
 * unchanged pristine call; when it succeeds, or fails with anything but
 * LUA_ERRFILE, its result is returned as is.  A LUA_ERRFILE whose message is
 * lauxlib's own "cannot read <fn>: <strerror>" (lauxlib.c:664, :733) is
 * retried up to ISAAC_VITA_LUA_LOADFILE_RETRY_MAX times, a transient
 * "cannot open <fn>: " (any strerror but ENOENT/ENOTDIR/EACCES/
 * ENAMETOOLONG) once; never a directory, never "cannot reopen".  Delays are
 * 2, 16 and 128 ms, inside a per-boot budget of 16 retries and 1 s of sleep.
 * errfile leaves exactly one value above the caller's top (lauxlib.c:732
 * settop(fnameindex), :664 push, :665 remove), so one lua_pop undoes a
 * failed attempt.  Every event is counted; the first
 * VITA_LUA_LOADFILE_LOG_FIRST get a receipt line.
 *
 * ISAAC_VITA_LUA_LOADFILE_IO_PROBE (test eboots only) adds thread-armed
 * --wrap hooks of sceIoOpen/sceIoRead/sceIoClose that record the raw SCE
 * return codes of the files the wrapped call opens, which newlib reduces to
 * errno = code & 0xFF and fclose then erases.  ISAAC_VITA_LUA_LOADFILE_IO_INJECT
 * makes the n-th sceIoRead of a matching file fail with
 * __vita_make_sce_errno(EIO) on attempt 1, so the whole newlib path
 * (_read_r, __srefill_r __SERR, fclose/_close_r, errfile) runs for real. */
#include "host_vita_lua_loadfile.h"

#if !defined(ISAAC_VITA_LUA_LOADFILE_RETRY)
#error "host_vita_lua_loadfile.c is compiled only for ISAAC_VITA_LUA_LOADFILE_RETRY"
#endif
#if !defined(ISAAC_VITA_LUA_LOADFILE_RETRY_MAX)
#error "ISAAC_VITA_LUA_LOADFILE_RETRY_MAX (0..3) is set by recomp/vita/CMakeLists.txt"
#endif
#if ISAAC_VITA_LUA_LOADFILE_RETRY_MAX > 3
#error "ISAAC_VITA_LUA_LOADFILE_RETRY_MAX must be 0..3"
#endif
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX) && \
    !defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
#error "ISAAC_VITA_LUA_LOADFILE_IO_INJECT requires ISAAC_VITA_LUA_LOADFILE_IO_PROBE"
#endif

/* Platform headers first, as host_vita_crt.c does: psp2/io's SceIoStat has
 * members named st_ctime/st_atime/st_mtime, which a libc <sys/stat.h> may
 * define as macros. */
#if defined(ISAAC_VITA_LUA_LOADFILE_ORACLE)
/* Includes <sys/stat.h> itself, then maps stat/S_ISDIR on MSVC. */
#include "vita_lua_loadfile_fault.h"
#elif defined(__vita__)
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
#include <psp2/io/fcntl.h>
#endif
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#error "ISAAC_VITA_LUA_LOADFILE_RETRY needs the Vita kernel or the host oracle"
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "lua.h"
#include "lauxlib.h" /* LUA_ERRFILE */

void isaac_vita_log(const char *format, ...);

#define VITA_LUA_LOADFILE_LOG_FIRST 8U
#define LF_DELAY_FIRST_US 2000U
#define LF_DELAY_CAP_US 128000U
#define LF_BUDGET_RETRIES 16U
#define LF_BUDGET_SLEEP_US 1000000U
#define LF_WHY_BYTES 256U
#define LF_PATH_SHOWN 120U

enum { LF_READ = 0, LF_OPEN = 1, LF_OTHER = 2 };
enum {
    LF_RECOVERED = 0,
    LF_UNRECOVERED,
    LF_CHANGED,
    LF_OBSERVED,
    LF_BUDGET,
    LF_ISDIR,
    LF_OPEN_ERROR
};

static const char *const k_lf_kind[] = { "read", "open", "other" };
static const char *const k_lf_result[] = {
    "recovered", "unrecovered", "changed", "observed", "budget", "isdir",
    "open-error"
};

/* Plain counters: only the game thread loads Lua, under the bridge's owner
 * slot (host_vita_lua.c); the STATS line is a racy snapshot. */
static struct {
    uint32_t banner;
    uint32_t loads;
    uint32_t read_err;
    uint32_t open_transient;
    uint32_t open_err;
    uint32_t isdir;
    uint32_t retries;
    uint32_t recovered;
    uint32_t unrecovered;
    uint32_t changed;
    uint32_t observed;
    uint32_t budget_hit;
    uint32_t sleep_us;
    uint32_t events;
    uint32_t logged;
    /* budget use (equal to retries/sleep_us except after an oracle reset) */
    uint32_t budget_retries;
    uint32_t budget_sleep_us;
} s_lf;

int __real_luaL_loadfilex(lua_State *L, const char *filename,
                          const char *mode);
int __wrap_luaL_loadfilex(lua_State *L, const char *filename,
                          const char *mode);

static uint64_t lf_now_us(void)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_ORACLE)
    return isaac_vita_lua_loadfile_oracle_now_us();
#else
    return sceKernelGetProcessTimeWide();
#endif
}

static void lf_sleep_us(uint32_t us)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_ORACLE)
    isaac_vita_lua_loadfile_oracle_sleep_us(us);
#else
    sceKernelDelayThread(us);
#endif
}

static uint32_t lf_elapsed(uint64_t from, uint64_t to)
{
    uint64_t d = to > from ? to - from : 0U;
    return d > UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}

/* min(2000 << 3*(attempt-1), 128000): 2000, 16000, 128000, then the cap.
 * The shift stops at the cap, so no attempt number overflows. */
static uint32_t lf_delay_us(uint32_t attempt)
{
    uint32_t delay = LF_DELAY_FIRST_US;
    while (--attempt != 0U && delay < LF_DELAY_CAP_US)
        delay <<= 3;
    return delay < LF_DELAY_CAP_US ? delay : LF_DELAY_CAP_US;
}

static int lf_budget_take(uint32_t delay)
{
    if (s_lf.budget_retries + 1U > LF_BUDGET_RETRIES ||
        delay > LF_BUDGET_SLEEP_US - s_lf.budget_sleep_us)
        return 0;
    ++s_lf.budget_retries;
    s_lf.budget_sleep_us += delay;
    return 1;
}

/* ------------------------------------------------------------------ */
/* IO probe (test eboots only)                                         */
/* ------------------------------------------------------------------ */
#define LF_PROBE_UIDS 4U
#define LF_PROBE_DEPTH 4U

typedef struct lf_probe_rec {
    int32_t uid[LF_PROBE_UIDS];
    uint32_t uid_reads[LF_PROBE_UIDS];
    uint32_t uids;
    uint32_t reads;          /* sceIoRead calls on recorded uids */
    int32_t target;          /* index of the IO_INJECT uid, or -1 */
    uint32_t inject_ok;      /* attempt 1 only */
    uint32_t injected;
    uint32_t has_sce;
    int32_t sce;             /* first negative sceIoRead return, all 32 bits */
    uint32_t sce_at;         /* its 1-based index among `reads` */
    uint32_t sce_len;
    uint32_t sce_us;
    uint32_t has_close;
    int32_t close;           /* first negative sceIoClose, else the last */
} lf_probe_rec;

typedef struct lf_arm {
    int32_t thread;          /* arming thread; meaningful while depth > 0 */
    uint32_t depth;
    uintptr_t marker;        /* address of a local in the arming frame */
    lf_probe_rec *rec;       /* NULL past LF_PROBE_DEPTH */
} lf_arm;

#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
/* Signatures copied from vita-headers psp2/io/fcntl.h and common/types.h
 * (as kage_vita_io_profile.c): SceMode/SceUID are int, SceSize unsigned. */
extern SceUID __real_sceIoOpen(const char *, int, SceMode);
extern int __real_sceIoClose(SceUID);
extern int __real_sceIoRead(SceUID, void *, unsigned int);
SceUID __wrap_sceIoOpen(const char *path, int flags, SceMode mode);
int __wrap_sceIoClose(SceUID descriptor);
int __wrap_sceIoRead(SceUID descriptor, void *buffer, unsigned int size);
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX)
/* newlib libc/sys/vita/vitaerror.h:39 (error.c: SCE_ERRNO_NONE | errno). */
int __vita_make_sce_errno(int posix_errno);
#endif

static lf_arm s_arm;
static lf_probe_rec s_rec[LF_PROBE_DEPTH];
static uint32_t s_stale_clears;

static int32_t lf_thread_id(void)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_ORACLE)
    return isaac_vita_lua_loadfile_oracle_thread_id();
#else
    return (int32_t)sceKernelGetThreadId();
#endif
}

static void lf_rec_reset(lf_probe_rec *rec, uint32_t inject_ok)
{
    if (!rec)
        return;
    memset(rec, 0, sizeof *rec);
    rec->target = -1;
    rec->inject_ok = inject_ok;
}

/* A frame address at or above the arming frame's marker means that frame is
 * gone (the stack grows down): __real_luaL_loadfilex longjmped out
 * (LUA_ERRMEM in errfile's lua_pushfstring, or a __gc error rethrown there
 * as LUA_ERRGCMM) and never disarmed.  While the arming frame is live, every
 * hook and nested wrapper runs strictly below the marker; an address equal
 * to it is a new wrapper frame in the dead one's slot, e.g. the next
 * pcall(require) from the same caller. */
static void lf_arm_drop_stale(int32_t thread, const void *frame)
{
    if (s_arm.depth != 0U && s_arm.thread == thread &&
        (uintptr_t)frame >= s_arm.marker) {
        memset(&s_arm, 0, sizeof s_arm);
        ++s_stale_clears;
    }
}

static lf_probe_rec *lf_arm_enter(lf_arm *saved, const void *frame)
{
    int32_t thread = lf_thread_id();

    lf_arm_drop_stale(thread, frame);
    *saved = s_arm;
    s_arm.thread = thread;
    s_arm.depth = saved->depth + 1U;
    s_arm.marker = (uintptr_t)frame;
    s_arm.rec = s_arm.depth <= LF_PROBE_DEPTH ? &s_rec[s_arm.depth - 1U]
                                              : NULL;
    lf_rec_reset(s_arm.rec, 1U);
    return s_arm.rec;
}

static void lf_arm_leave(const lf_arm *saved)
{
    s_arm = *saved;
}

/* The hook's own record, or NULL: armed, on the arming thread, and the
 * arming frame still live. */
static lf_probe_rec *lf_probe_mine(const void *frame)
{
    int32_t thread;

    if (s_arm.depth == 0U)
        return NULL;
    thread = lf_thread_id();
    if (s_arm.thread != thread)
        return NULL;
    lf_arm_drop_stale(thread, frame);
    return s_arm.depth != 0U ? s_arm.rec : NULL;
}

static int lf_probe_index(const lf_probe_rec *rec, SceUID uid)
{
    uint32_t index;

    for (index = 0U; index < rec->uids; ++index) {
        if (rec->uid[index] == (int32_t)uid)
            return (int)index;
    }
    return -1;
}

#if defined(ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX)
static int lf_path_matches(const char *path)
{
    static const char suffix[] = ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX;
    size_t length;

    if (!path)
        return 0;
    length = strlen(path);
    return length >= sizeof suffix - 1U &&
           memcmp(path + length - (sizeof suffix - 1U), suffix,
                  sizeof suffix - 1U) == 0;
}
#endif

static void lf_probe_opened(const char *path, SceUID uid)
{
    char frame;
    lf_probe_rec *rec = lf_probe_mine(&frame);

    if (!rec || uid < 0 || rec->uids >= LF_PROBE_UIDS)
        return;
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX)
    if (rec->inject_ok && rec->target < 0 && lf_path_matches(path))
        rec->target = (int32_t)rec->uids;
#else
    (void)path;
#endif
    rec->uid[rec->uids] = (int32_t)uid;
    rec->uid_reads[rec->uids] = 0U;
    ++rec->uids;
}

SceUID __wrap_sceIoOpen(const char *path, int flags, SceMode mode)
{
    SceUID uid = __real_sceIoOpen(path, flags, mode);

    if (s_arm.depth != 0U)
        lf_probe_opened(path, uid);
    return uid;
}

static int lf_probe_read(SceUID descriptor, void *buffer, unsigned int size)
{
    char frame;
    lf_probe_rec *rec = lf_probe_mine(&frame);
    uint64_t begin;
    uint32_t took;
    int index;
    int result;

    index = rec ? lf_probe_index(rec, descriptor) : -1;
    if (index < 0)
        return __real_sceIoRead(descriptor, buffer, size);
    ++rec->reads;
    ++rec->uid_reads[index];
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX)
    if (rec->inject_ok && !rec->injected && rec->target == index &&
        rec->uid_reads[index] == ISAAC_VITA_LUA_LOADFILE_IO_INJECT_READ) {
        rec->injected = 1U;
        result = __vita_make_sce_errno(EIO);
        took = 0U;
    } else
#endif
    {
        begin = lf_now_us();
        result = __real_sceIoRead(descriptor, buffer, size);
        took = lf_elapsed(begin, lf_now_us());
    }
    if (result < 0 && !rec->has_sce) {
        rec->has_sce = 1U;
        rec->sce = (int32_t)result;
        rec->sce_at = rec->reads;
        rec->sce_len = size;
        rec->sce_us = took;
    }
    return result;
}

int __wrap_sceIoRead(SceUID descriptor, void *buffer, unsigned int size)
{
    if (s_arm.depth == 0U)
        return __real_sceIoRead(descriptor, buffer, size);
    return lf_probe_read(descriptor, buffer, size);
}

static int lf_probe_close(SceUID descriptor)
{
    char frame;
    lf_probe_rec *rec = lf_probe_mine(&frame);
    int index = rec ? lf_probe_index(rec, descriptor) : -1;
    int result = __real_sceIoClose(descriptor);

    if (index >= 0 && (!rec->has_close || rec->close >= 0)) {
        rec->has_close = 1U;
        rec->close = (int32_t)result;
    }
    return result;
}

int __wrap_sceIoClose(SceUID descriptor)
{
    if (s_arm.depth == 0U)
        return __real_sceIoClose(descriptor);
    return lf_probe_close(descriptor);
}

#define LF_PROBE_ON 1
#else
static lf_probe_rec *lf_arm_enter(lf_arm *saved, const void *frame)
{
    (void)saved;
    (void)frame;
    return NULL;
}

static void lf_arm_leave(const lf_arm *saved)
{
    (void)saved;
}

static void lf_rec_reset(lf_probe_rec *rec, uint32_t inject_ok)
{
    (void)rec;
    (void)inject_ok;
}

#define LF_PROBE_ON 0
#endif

/* ------------------------------------------------------------------ */
/* Receipts                                                            */
/* ------------------------------------------------------------------ */
/* "<suffix>@<n>" as configured, or "-". */
static void lf_inject_text(char *out, size_t size)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX)
    snprintf(out, size, "%.64s@%u", ISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX,
             (unsigned)ISAAC_VITA_LUA_LOADFILE_IO_INJECT_READ);
#else
    snprintf(out, size, "-");
#endif
}

void isaac_vita_lua_loadfile_report(const char *why)
{
    int saved_errno = errno;
    char delays[48];
    char inject[80];
    size_t used = 0U;
    uint32_t attempt;

    lf_inject_text(inject, sizeof inject);
    delays[0] = '-';
    delays[1] = '\0';
    for (attempt = 1U; attempt <= ISAAC_VITA_LUA_LOADFILE_RETRY_MAX;
         ++attempt) {
        int n = snprintf(delays + used, sizeof delays - used, "%s%u",
                         attempt == 1U ? "" : ",",
                         (unsigned)lf_delay_us(attempt));
        if (n < 0 || (size_t)n >= sizeof delays - used)
            break;
        used += (size_t)n;
    }
    isaac_vita_log(
        "KAGE VITA LUA LOADFILE STATS: why=%s max=%u delay_us=%s "
        "budget=%u/%u probe=%d inject=%s loads=%u read_err=%u "
        "open_transient=%u open_err=%u isdir=%u retries=%u recovered=%u "
        "unrecovered=%u changed=%u observed=%u budget_hit=%u sleep_us=%u "
        "logged=%u",
        why ? why : "-", (unsigned)ISAAC_VITA_LUA_LOADFILE_RETRY_MAX, delays,
        (unsigned)LF_BUDGET_RETRIES, (unsigned)LF_BUDGET_SLEEP_US,
        LF_PROBE_ON, inject, (unsigned)s_lf.loads,
        (unsigned)s_lf.read_err, (unsigned)s_lf.open_transient,
        (unsigned)s_lf.open_err, (unsigned)s_lf.isdir,
        (unsigned)s_lf.retries, (unsigned)s_lf.recovered,
        (unsigned)s_lf.unrecovered, (unsigned)s_lf.changed,
        (unsigned)s_lf.observed, (unsigned)s_lf.budget_hit,
        (unsigned)s_lf.sleep_us, (unsigned)s_lf.logged);
    errno = saved_errno;
}

static void lf_event(uint32_t result, int kind, uint32_t attempts,
                     int status, uint32_t fail_us, uint32_t retry_us,
                     const lf_probe_rec *probe, const char *fn,
                     const char *why)
{
    char sce[16] = "-";
    char sce_at[24] = "-";
    char sce_len[12] = "-";
    char sce_us[12] = "-";
    char close_text[16] = "-";
    size_t fn_length = strlen(fn);
    uint32_t n = ++s_lf.events;

    if (n > VITA_LUA_LOADFILE_LOG_FIRST)
        return;
    ++s_lf.logged;
    if (probe) {
        if (probe->has_sce) {
            snprintf(sce, sizeof sce, "0x%08x", (unsigned)probe->sce);
            snprintf(sce_at, sizeof sce_at, "%u/%u",
                     (unsigned)probe->sce_at, (unsigned)probe->reads);
            snprintf(sce_len, sizeof sce_len, "%u",
                     (unsigned)probe->sce_len);
            snprintf(sce_us, sizeof sce_us, "%u", (unsigned)probe->sce_us);
        } else {
            snprintf(sce, sizeof sce, "none");
            snprintf(sce_at, sizeof sce_at, "0/%u", (unsigned)probe->reads);
            snprintf(sce_len, sizeof sce_len, "0");
            snprintf(sce_us, sizeof sce_us, "0");
        }
        if (probe->has_close)
            snprintf(close_text, sizeof close_text, "0x%08x",
                     (unsigned)probe->close);
    }
    /* why= last: the strerror tail holds spaces.  No errno= text: a
     * low-byte errno is not the SCE code (bench_run.py RE_ENODEV). */
    isaac_vita_log(
        "KAGE VITA LUA LOADFILE RETRY: n=%u result=%s kind=%s attempts=%u "
        "status=%d fail_us=%u retry_us=%u sce=%s sce_at=%s sce_len=%s "
        "sce_us=%s close=%s inj=%u path=%s%s why=%s",
        (unsigned)n, k_lf_result[result], k_lf_kind[kind],
        (unsigned)attempts, status, (unsigned)fail_us, (unsigned)retry_us,
        sce, sce_at, sce_len, sce_us, close_text,
        (unsigned)(probe ? probe->injected : 0U),
        fn_length > LF_PATH_SHOWN ? "..." : "",
        fn_length > LF_PATH_SHOWN ? fn + fn_length - LF_PATH_SHOWN : fn,
        why);
}

/* ------------------------------------------------------------------ */
/* Classification of lauxlib's LUA_ERRFILE text                        */
/* ------------------------------------------------------------------ */
/* lauxlib.c:664 lua_pushfstring(L, "cannot %s %s: %s", what, filename,
 * serr), where what is "open" (:717), "reopen" (:723) or "read" (:733),
 * filename is luaL_loadfilex's own argument (:715 "@%s", :663 + 1) and serr
 * is strerror(errno).  *tail gets serr when the text has exactly that
 * shape. */
static int lf_classify(lua_State *L, const char *fn, const char **tail)
{
    static const char prefix_read[] = "cannot read ";
    static const char prefix_open[] = "cannot open ";
    static const char prefix_reopen[] = "cannot reopen ";
    const char *message;
    size_t skip;
    size_t fn_length;
    int kind;

    *tail = "";
    if (lua_type(L, -1) != LUA_TSTRING)
        return LF_OTHER;
    message = lua_tostring(L, -1);
    if (strncmp(message, prefix_read, sizeof prefix_read - 1U) == 0) {
        kind = LF_READ;
        skip = sizeof prefix_read - 1U;
    } else if (strncmp(message, prefix_open,
                       sizeof prefix_open - 1U) == 0) {
        kind = LF_OPEN;
        skip = sizeof prefix_open - 1U;
    } else if (strncmp(message, prefix_reopen,
                       sizeof prefix_reopen - 1U) == 0) {
        kind = LF_OTHER;
        skip = sizeof prefix_reopen - 1U;
    } else {
        *tail = message;
        return LF_OTHER;
    }
    fn_length = strlen(fn);
    if (strncmp(message + skip, fn, fn_length) != 0 ||
        message[skip + fn_length] != ':' ||
        message[skip + fn_length + 1U] != ' ') {
        *tail = message;
        return LF_OTHER;
    }
    *tail = message + skip + fn_length + 2U;
    return kind;
}

/* errno is real on the open path (lauxlib.c:717 calls errfile before any
 * fclose).  The strings come from the strerror lauxlib uses, never typed. */
static int lf_open_is_definitive(const char *tail)
{
    static const int definitive[] = { ENOENT, ENOTDIR, EACCES, ENAMETOOLONG };
    size_t index;

    for (index = 0U; index < sizeof definitive / sizeof definitive[0];
         ++index) {
        if (strcmp(tail, strerror(definitive[index])) == 0)
            return 1;
    }
    return 0;
}

/* newlib opens a directory with sceIoDopen; its _read_r then returns
 * __vita_make_sce_errno(EBADF) (old-syscalls.c:336-337) and sceIoDclose
 * succeeds, so a directory gives "cannot read <dir>: Success" every time. */
static int lf_path_is_directory(const char *fn)
{
    struct stat status;

    return stat(fn, &status) == 0 && S_ISDIR(status.st_mode);
}

static void lf_copy_why(char *out, const char *tail)
{
    size_t length = strlen(tail);

    if (length >= LF_WHY_BYTES)
        length = LF_WHY_BYTES - 1U;
    memcpy(out, tail, length);
    out[length] = '\0';
}

static void lf_banner_once(void)
{
    if (s_lf.banner)
        return;
    s_lf.banner = 1U;
    isaac_vita_lua_loadfile_report("first-load");
}

int __wrap_luaL_loadfilex(lua_State *L, const char *fn, const char *mode)
{
    lf_arm saved_arm;
    lf_probe_rec *rec;
    lf_probe_rec first;
    char why[LF_WHY_BYTES];
    const char *tail;
    uint64_t start;
    uint64_t failed;
    uint32_t fail_us;
    uint32_t retry_us;
    uint32_t attempts = 1U;
    uint32_t max;
    uint32_t attempt;
    uint32_t delay;
    uint32_t result = LF_UNRECOVERED;
    int budget = 0;
    int status;
    int kind;
    int saved_errno = errno;

    lf_banner_once();
    errno = saved_errno;
    rec = lf_arm_enter(&saved_arm, &saved_arm);
    start = lf_now_us();
    status = __real_luaL_loadfilex(L, fn, mode); /* attempt 1 == the OFF call */
    saved_errno = errno;
    failed = lf_now_us();
    ++s_lf.loads;
    if (fn == NULL || status != LUA_ERRFILE)
        goto out;
    fail_us = lf_elapsed(start, failed);
    retry_us = 0U;
    if (rec)
        first = *rec;
    kind = lf_classify(L, fn, &tail);
    lf_copy_why(why, tail);
    if (kind == LF_OTHER) {
        ++s_lf.open_err;
        result = LF_OPEN_ERROR;
        goto out_logged;
    }
    if (kind == LF_OPEN && lf_open_is_definitive(tail)) {
        ++s_lf.open_err;
        result = LF_OPEN_ERROR;
        goto out_logged;
    }
    if (lf_path_is_directory(fn)) {
        ++s_lf.isdir;
        result = LF_ISDIR;
        goto out_logged;
    }
    if (kind == LF_READ)
        ++s_lf.read_err;
    else
        ++s_lf.open_transient;
    max = kind == LF_READ ? (uint32_t)ISAAC_VITA_LUA_LOADFILE_RETRY_MAX
                          : (ISAAC_VITA_LUA_LOADFILE_RETRY_MAX ? 1U : 0U);
    for (attempt = 1U; attempt <= max; ++attempt) {
        delay = lf_delay_us(attempt);
        if (!lf_budget_take(delay)) {
            ++s_lf.budget_hit;
            budget = 1;
            break;
        }
        lua_pop(L, 1); /* the failed attempt's single error string */
        lf_sleep_us(delay);
        ++s_lf.retries;
        s_lf.sleep_us += delay;
        lf_rec_reset(rec, 0U); /* retries are never injected */
        status = __real_luaL_loadfilex(L, fn, mode);
        saved_errno = errno;
        ++attempts;
        if (status != LUA_ERRFILE)
            break;
        if (lf_classify(L, fn, &tail) == LF_OTHER)
            break;
    }
    retry_us = lf_elapsed(failed, lf_now_us());
    if (max == 0U) {
        result = LF_OBSERVED;
        ++s_lf.observed;
    } else if (budget) {
        result = LF_BUDGET;
    } else if (status == LUA_OK) {
        result = LF_RECOVERED;
        ++s_lf.recovered;
    } else if (status == LUA_ERRFILE &&
               lf_classify(L, fn, &tail) != LF_OTHER) {
        result = LF_UNRECOVERED;
        ++s_lf.unrecovered;
    } else {
        /* e.g. LUA_ERRSYNTAX after a truncated retry, or "cannot reopen" */
        result = LF_CHANGED;
        ++s_lf.changed;
    }
out_logged:
    lf_event(result, kind, attempts, status, fail_us, retry_us,
             rec ? &first : NULL, fn, why);
out:
    lf_arm_leave(&saved_arm);
    errno = saved_errno;
    return status;
}

#if defined(ISAAC_VITA_LUA_LOADFILE_ORACLE)
uint32_t isaac_vita_lua_loadfile_oracle_delay_us(uint32_t attempt)
{
    return lf_delay_us(attempt);
}

void isaac_vita_lua_loadfile_oracle_reset_budget(void)
{
    s_lf.budget_retries = 0U;
    s_lf.budget_sleep_us = 0U;
}

uint32_t isaac_vita_lua_loadfile_oracle_arm_depth(void)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
    return s_arm.depth;
#else
    return 0U;
#endif
}

uint32_t isaac_vita_lua_loadfile_oracle_stale_clears(void)
{
#if defined(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)
    return s_stale_clears;
#else
    return 0U;
#endif
}
#endif
