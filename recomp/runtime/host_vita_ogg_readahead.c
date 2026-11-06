/* ISAAC_VITA_OGG_READAHEAD: music stream read-ahead below newlib on CPU 2.
 *
 * 1. WHERE THE TIME GOES
 *
 * nv_topup (host_vita_native_vorbis.c) tops a mirror's OGG ring up with one
 * guest stream read per wrapped call.  That read is the KAGE ArchivedFile:
 * translated block reader 0x59c270 -> fread adapter 0x596560 -> vita_crt_fread
 * -> newlib fread, whose 16 KiB setvbuf buffer (ISAAC_VITA_CRT_FILE_READ_
 * BUFFER_SIZE) is refilled by newlib's __srefill_r -> __sread -> _read_r ->
 * sceIoRead.  The P2b device session (2026-10-17, p2b-native.log ph120.hotc
 * phase=svc) put 6557 of 8178 sound-service samples under that fread, and the
 * ph120.ioo windows measure a 16 KiB sceIoRead at 2.5 ms p50.  This module
 * takes those sceIoRead calls off the game thread for the descriptors the
 * native Vorbis top-ups read.
 *
 * 2. THE CUT AND WHY IT IS EXACT
 *
 * GNU ld --wrap=_read_r/_lseek_r/_close_r routes newlib's own references
 * (lib_a-stdio.o __sread/__sseek/__sclose, and the archive cache's direct
 * _close_r) here.  newlib's FILE state is a pure function of what those
 * three calls return, so the wrapper keeps every FILE field, cursor and guest
 * byte identical by returning exactly what the real call would return:
 *
 *  - read hit: [vpos, vpos + n) is covered by READY chunks the worker read
 *    from the same immutable archive file at the same offsets, so the bytes
 *    are the bytes sceIoRead would return; the count is n, or size - vpos at
 *    the end.  That count assumes the kernel reads a regular file short only
 *    at its end.  One device short fread is on record (recomp/vita/README.md,
 *    the ISAAC_VITA_CRT_FREAD_COMPLETE entry: tour 1, 2026-09-17,
 *    repentance.a Mines music, 48 bytes short, cause unknown); if it was a
 *    transient short sceIoRead, a hit returns the full count where the real
 *    call read short.  The worker's own short pread turns the entry
 *    passthrough (WORKER short=); the device A/B compares the CRT's
 *    KAGE VITA CRT FREAD SHORT receipts with the knob OFF and ON; errno is cleared
 *    as newlib's _read_r does on success (vitasdk/newlib branch vita,
 *    newlib/libc/sys/vita/syscalls.c, _read_r tail: "reent->_errno = 0;
 *    return ret;"; __sread/__sseek/__sclose in newlib/libc/stdio/stdio.c
 *    call _read_r/_lseek_r/_close_r with fp->_file).
 *  - read at or after the end: 0 and errno 0, as sceIoRead + _read_r.
 *  - lseek SEEK_SET/SEEK_CUR to a target inside [0, size]: the target, errno
 *    untouched (syscalls.c _lseek_r sets errno only for ret < 0).  The kernel
 *    position is then stale; vpos is the position the kernel would hold.
 *  - everything else (SEEK_END, targets outside the file, n == 0, n larger
 *    than the read-ahead window, an uncovered range): the real call, after a
 *    real SEEK_SET to vpos when the kernel position is stale.
 *
 * Unregistered descriptors go straight to the real call.  A registered one
 * used by another thread (never guest stdio, see ra_foreign) is handed back
 * at newlib's position and stays real from then on.
 *
 * The one difference is on a failure path: after an application suspend
 * (PS button) SceIofilemgr invalidates every descriptor and the real
 * sceIoRead/sceIoLseek32 fail with ENODEV (host_vita_crt.c, DESCRIPTOR_
 * RECOVER).  Chunks already READY are still the right bytes, so reads served
 * from them succeed where the real call would already fail; the worker's own
 * sceIoPread then fails on its dead descriptor, the entry turns passthrough,
 * the next call goes to the real descriptor and fails with the same ENODEV,
 * and the CRT's existing recovery reopens the token at its logical cursor.
 * The guest sees the same bytes and counts; only the point at which the dead
 * descriptor is noticed moves later.  That is at most the read-ahead window
 * when the worker's descriptor predates the suspend (it dies too).  When the
 * worker opens its descriptor after the suspend (note() ran just before it),
 * that descriptor is live: hits continue to the end of the stream, and the
 * dead game descriptor surfaces only at the next forwarded call (SEEK_END,
 * an uncovered read) or at forget/close, where the archive cache discards it
 * as with the knob OFF.  The bytes are the file's bytes either way (oracle
 * scenario suspend-late-open).
 *
 * 3. CONCURRENCY
 *
 * One worker thread (CPU 2) owns a private SceIo descriptor per entry and
 * fills chunks FREE -> FILLING -> READY with sceIoPread outside any lock.  The
 * game thread owns the newlib descriptor, vpos and the synced flag.  A
 * per-entry spinlock covers chunk state changes and the game's coverage
 * check; the game copies READY bytes after unlocking, which is safe because
 * the worker only reuses a chunk that does not intersect
 * [vpos, vpos + window) and the game moves vpos only after its copy.  The
 * game never waits on the lock for long: a busy lock is a miss.  The worker
 * never touches guest memory, the guest heap, a newlib FILE or a game
 * descriptor; its buffers are one USER_RW memblock per entry, allocated on
 * the game thread at the first registration and kept for the process. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_vita_ogg_readahead.h"

#if defined(ISAAC_VITA_OGG_READAHEAD_ORACLE) || !defined(__vita__)
#define RA_HOST 1
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "host_vita_ogg_readahead_oracle.h"
typedef isaac_ogg_ra_off_t ra_off_t;
typedef isaac_ogg_ra_ssize_t ra_ssize_t;
#else
#define RA_HOST 0
#include <reent.h>
#include <sys/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
typedef _off_t ra_off_t;
typedef _ssize_t ra_ssize_t;
void isaac_vita_log(const char *format, ...);
_ssize_t __real__read_r(struct _reent *r, int fd, void *buf, size_t n);
_off_t __real__lseek_r(struct _reent *r, int fd, _off_t off, int dir);
int __real__close_r(struct _reent *r, int fd);
_ssize_t __wrap__read_r(struct _reent *r, int fd, void *buf, size_t n);
_off_t __wrap__lseek_r(struct _reent *r, int fd, _off_t off, int dir);
int __wrap__close_r(struct _reent *r, int fd);
#endif

#if !defined(SEEK_SET) || SEEK_SET != 0 || SEEK_CUR != 1 || SEEK_END != 2
#error "newlib whence values drifted"
#endif

/* ------------------------------------------------------------------------
 * Geometry (CMake passes the cache values; these are its defaults). */
#ifndef ISAAC_OGG_RA_CHUNK_BYTES
#define ISAAC_OGG_RA_CHUNK_BYTES 16384U
#endif
#ifndef ISAAC_OGG_RA_CHUNKS
#define ISAAC_OGG_RA_CHUNKS 4U
#endif
#ifndef ISAAC_OGG_RA_WAIT_US
#define ISAAC_OGG_RA_WAIT_US 20000U
#endif
#ifndef ISAAC_OGG_RA_PRIORITY
#define ISAAC_OGG_RA_PRIORITY 168
#endif
#define ISAAC_OGG_RA_ENTRIES 4U
/* host_vita_startup.h ISAAC_VITA_STARTUP_PATH_MAX (1024) + NUL: the CRT's
 * recover.path capacity. */
#define ISAAC_OGG_RA_PATH_CAPACITY 1025U
#define ISAAC_OGG_RA_IDLE_US 20000U
#define ISAAC_OGG_RA_POLL_US 100U
#ifndef ISAAC_OGG_RA_GAME_SPINS
#define ISAAC_OGG_RA_GAME_SPINS 256U
#endif
/* Receipt cadence in registered _read_r calls: a steady refill is one
 * 16 KiB newlib buffer, so 16 calls is about 256 KiB of OGG between lines.
 * The device gate G3 takes the hit ratio over STATS differences that lie
 * inside gameplay windows only (recomp/vita/ogg_readahead_gates.py), so a
 * line pair should come about once per 2 s ph120 window.  At an estimated
 * 6 refills a second (P2b gameplay fread time, 0.27 ms per loop, at 2.5 ms
 * per 16 KiB read) the P2b route model in test_ogg_readahead_gates.py has
 * 18 game-only intervals at 16 reads per line and 3 at 64. */
#define ISAAC_OGG_RA_REPORT_PERIOD 16U
#define ISAAC_OGG_RA_STACK_BYTES 0x4000U
#define RA_WINDOW (ISAAC_OGG_RA_CHUNK_BYTES * ISAAC_OGG_RA_CHUNKS)
/* WAIT_US 0 never waits (and an unsigned `>= 0` would not compile under
 * -Werror=type-limits). */
#if ISAAC_OGG_RA_WAIT_US == 0U
#define RA_WAIT_DONE(waited) ((void)(waited), 1)
#else
#define RA_WAIT_DONE(waited) ((waited) >= ISAAC_OGG_RA_WAIT_US)
#endif

_Static_assert(ISAAC_OGG_RA_CHUNK_BYTES % 4096U == 0U &&
               ISAAC_OGG_RA_CHUNK_BYTES >= 4096U &&
               ISAAC_OGG_RA_CHUNK_BYTES <= 65536U,
               "chunk must be 4..64 KiB in whole pages (memblock granularity)");
/* With one chunk the window is the game's own next 16 KiB refill, which the
 * worker can only race (measured on the host oracle: zero fills). */
_Static_assert(ISAAC_OGG_RA_CHUNKS >= 2U && ISAAC_OGG_RA_CHUNKS <= 16U,
               "2..16 chunks per entry");
_Static_assert(RA_WINDOW <= 1048576U, "window bounded to 1 MiB per entry");

enum { RA_FREE = 0U, RA_LIVE = 1U, RA_RETIRING = 2U };
enum { CH_FREE = 0U, CH_FILLING = 1U, CH_READY = 2U };
enum { RA_COVERED = 0, RA_PENDING, RA_GAP, RA_BUSY };

typedef struct ra_chunk {
    uint32_t state;     /* entry lock */
    uint32_t off;
    uint32_t len;
    uint8_t *data;      /* fixed once the entry buffer exists */
} ra_chunk;

typedef struct ra_entry {
    /* Discovery: fd + 1 (0 = none); written by the game thread, read by
     * every thread's wrapper scan. */
    int fd_plus1;
    /* Lifecycle: FREE -> LIVE (game), LIVE -> RETIRING (game),
     * RETIRING -> FREE (worker). */
    uint32_t state;
    /* Set by the game (failed sync/read) or the worker (failed pread/open):
     * every call is forwarded, kernel position synced first. */
    uint32_t passthrough;
    uint32_t vpos;      /* game writes; worker reads */
    uint32_t miss_end;  /* game's own real read in flight: [vpos, miss_end) */
    uint32_t size;      /* fixed while LIVE */
    uint32_t lock;
    /* game thread only */
    uint32_t owner;
    uint32_t synced;
    /* worker only */
    int wfd;
    char path[ISAAC_OGG_RA_PATH_CAPACITY];
    ra_chunk chunk[ISAAC_OGG_RA_CHUNKS];
    uint8_t *buffer;
#if !RA_HOST
    SceUID block;
#endif
} ra_entry;

int isaac_vita_ogg_ra_context;

static ra_entry s_entries[ISAAC_OGG_RA_ENTRIES];
static isaac_vita_ogg_ra_stats s_stats;
static uint32_t s_report_due;
static uint32_t s_reads_since_report;
static uint32_t s_reports;
static uint32_t s_worker_attempted;
static uint32_t s_worker_ok;
/* A memblock refusal is remembered: note() runs on every top-up fread and
 * must not retry the allocation each time. */
static uint32_t s_alloc_failed;
#if RA_HOST
static uint32_t s_worker_stop;  /* oracle shutdown; the Vita worker lives on */
#endif

/* ------------------------------------------------------------------------
 * Atomics and platform. */
static uint32_t ra_load(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void ra_store(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static void ra_add(unsigned *p, unsigned v)
{
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static unsigned ra_peek(const unsigned *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static int ra_trylock(ra_entry *e)
{
    uint32_t expected = 0U;
    return __atomic_compare_exchange_n(&e->lock, &expected, 1U, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void ra_unlock(ra_entry *e)
{
    __atomic_store_n(&e->lock, 0U, __ATOMIC_RELEASE);
}

static uint64_t ra_now_us(void)
{
#if RA_HOST
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    return (uint64_t)sceKernelGetProcessTimeWide();
#endif
}

static void ra_delay_us(uint32_t us)
{
#if RA_HOST
    usleep(us);
#else
    sceKernelDelayThread(us);
#endif
}

static uint32_t ra_self(void)
{
#if RA_HOST
    return isaac_ogg_ra_oracle_thread_id();
#else
    return (uint32_t)sceKernelGetThreadId();
#endif
}

static struct _reent *ra_reent(void)
{
#if RA_HOST
    return isaac_ogg_ra_oracle_reent();
#else
    return _REENT;
#endif
}

/* Worker lock: the game holds it for a scan of at most CHUNKS entries, so
 * spin, and yield the core now and then in case the game was preempted. */
static void ra_lock_worker(ra_entry *e)
{
    uint32_t spins = 0U;

    while (!ra_trylock(e)) {
        if (++spins == 64U) {
            spins = 0U;
            ra_delay_us(20U);
        }
    }
}

static int ra_lock_game(ra_entry *e)
{
    uint32_t spins;

    for (spins = 0U; spins < ISAAC_OGG_RA_GAME_SPINS; ++spins)
        if (ra_trylock(e))
            return 1;
    return 0;
}

static int ra_fstat_size(int fd, uint32_t *size)
{
#if RA_HOST
    return isaac_ogg_ra_oracle_fstat_size(fd, size) == 0;
#else
    struct stat st;

    if (fstat(fd, &st) != 0 || st.st_size < 0)
        return 0;
    *size = (uint32_t)st.st_size;
    return 1;
#endif
}

static int ra_open(const char *path)
{
#if RA_HOST
    return isaac_ogg_ra_oracle_open(path);
#else
    /* The raw lane's own call (host_vita_crt.c vita_crt_raw_sce_open). */
    SceUID uid = sceIoOpen(path, SCE_O_RDONLY, 0);
    return uid < 0 ? -1 : (int)uid;
#endif
}

static int32_t ra_pread(int wfd, void *buf, uint32_t n, uint32_t off)
{
#if RA_HOST
    return isaac_ogg_ra_oracle_pread(wfd, buf, n, off);
#else
    return (int32_t)sceIoPread((SceUID)wfd, buf, (SceSize)n, (SceOff)off);
#endif
}

static void ra_close(int wfd)
{
#if RA_HOST
    (void)isaac_ogg_ra_oracle_close(wfd);
#else
    (void)sceIoClose((SceUID)wfd);
#endif
}

static int ra_alloc(ra_entry *e)
{
    uint32_t i;
#if RA_HOST
    e->buffer = (uint8_t *)malloc(RA_WINDOW);
    if (!e->buffer)
        return 0;
#else
    void *base = NULL;
    SceUID uid = sceKernelAllocMemBlock("isaac_ogg_ra",
                                        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                        RA_WINDOW, NULL);
    if (uid < 0)
        return 0;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        return 0;
    }
    e->block = uid;
    e->buffer = (uint8_t *)base;
#endif
    for (i = 0U; i < ISAAC_OGG_RA_CHUNKS; ++i)
        e->chunk[i].data = e->buffer + (size_t)i * ISAAC_OGG_RA_CHUNK_BYTES;
    return 1;
}

/* ------------------------------------------------------------------------
 * Worker thread and its wake-up. */
#if RA_HOST
static pthread_t s_worker_thread;
static pthread_mutex_t s_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake_cond = PTHREAD_COND_INITIALIZER;
static uint32_t s_wake_pending;
#else
static SceUID s_worker_flag = -1;
static SceUID s_worker_thread = -1;
#endif

static void ra_signal(void)
{
    if (!ra_load(&s_worker_ok))
        return;
#if RA_HOST
    pthread_mutex_lock(&s_wake_mutex);
    s_wake_pending = 1U;
    pthread_cond_signal(&s_wake_cond);
    pthread_mutex_unlock(&s_wake_mutex);
#else
    (void)sceKernelSetEventFlag(s_worker_flag, 1U);
#endif
}

static void ra_wait(void)
{
#if RA_HOST
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)ISAAC_OGG_RA_IDLE_US * 1000L;
    while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        ++deadline.tv_sec;
    }
    pthread_mutex_lock(&s_wake_mutex);
    while (!s_wake_pending && !ra_load(&s_worker_stop)) {
        if (pthread_cond_timedwait(&s_wake_cond, &s_wake_mutex,
                                   &deadline) != 0)
            break;
    }
    s_wake_pending = 0U;
    pthread_mutex_unlock(&s_wake_mutex);
#else
    unsigned int bits = 0U;
    SceUInt timeout = ISAAC_OGG_RA_IDLE_US;

    (void)sceKernelWaitEventFlag(s_worker_flag, 1U,
                                 SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
                                 &bits, &timeout);
#endif
}

/* First position >= from not covered by a READY/FILLING chunk (lock held). */
static uint32_t ra_first_uncovered(const ra_entry *e, uint32_t from)
{
    uint32_t q = from;
    int moved;

    do {
        uint32_t i;
        moved = 0;
        for (i = 0U; i < ISAAC_OGG_RA_CHUNKS; ++i) {
            const ra_chunk *k = &e->chunk[i];
            if (k->state != CH_FREE && k->off <= q && q - k->off < k->len) {
                q = k->off + k->len;
                moved = 1;
            }
        }
    } while (moved);
    return q;
}

/* A chunk the game cannot be reading: FREE, or READY and entirely outside
 * [vpos, vpos + window).  Consumed chunks go first, then the farthest one
 * ahead (lock held). */
static ra_chunk *ra_victim(ra_entry *e, uint32_t vpos)
{
    ra_chunk *best = NULL;
    uint64_t window_end = (uint64_t)vpos + RA_WINDOW;
    uint32_t i;
    int best_rank = 3;
    uint32_t best_key = 0U;

    for (i = 0U; i < ISAAC_OGG_RA_CHUNKS; ++i) {
        ra_chunk *k = &e->chunk[i];
        int rank;
        uint32_t key;

        if (k->state == CH_FREE)
            return k;
        if (k->state != CH_READY)
            continue;
        if ((uint64_t)k->off + k->len <= vpos) {
            rank = 0;                   /* consumed: lowest end first */
            key = UINT32_MAX - (k->off + k->len);
        } else if ((uint64_t)k->off >= window_end) {
            rank = 1;                   /* beyond: farthest first */
            key = k->off;
        } else {
            continue;
        }
        if (rank < best_rank || (rank == best_rank && key > best_key)) {
            best = k;
            best_rank = rank;
            best_key = key;
        }
    }
    return best;
}

/* Bytes of read-ahead the entry holds beyond vpos (lock held). */
static uint32_t ra_lead(const ra_entry *e, uint32_t vpos, uint32_t miss_end)
{
    uint32_t from = miss_end > vpos ? miss_end : vpos;
    return ra_first_uncovered(e, from) - vpos;
}

static void ra_worker_retire(ra_entry *e)
{
    uint32_t i;

    if (e->wfd >= 0) {
        ra_close(e->wfd);
        e->wfd = -1;
    }
    ra_lock_worker(e);
    for (i = 0U; i < ISAAC_OGG_RA_CHUNKS; ++i)
        e->chunk[i].state = CH_FREE;
    ra_unlock(e);
    ra_store(&e->state, RA_FREE);
}

/* One sceIoPread for the most urgent LIVE entry.  Returns 1 if it did any
 * work (a fill, a retirement, an open attempt). */
static int ra_worker_pass(void)
{
    ra_entry *best = NULL;
    uint32_t best_lead = UINT32_MAX;
    int worked = 0;
    uint32_t i;

    for (i = 0U; i < ISAAC_OGG_RA_ENTRIES; ++i) {
        ra_entry *e = &s_entries[i];
        uint32_t state = ra_load(&e->state);
        uint32_t vpos;
        uint32_t lead;
        uint32_t from;

        if (state == RA_RETIRING) {
            ra_worker_retire(e);
            worked = 1;
            continue;
        }
        if (state != RA_LIVE || ra_load(&e->passthrough))
            continue;
        if (e->wfd < 0) {
            e->wfd = ra_open(e->path);
            worked = 1;
            if (e->wfd < 0) {
                ra_add(&s_stats.open_fail, 1U);
                ra_store(&e->passthrough, 1U);
                continue;
            }
        }
        ra_lock_worker(e);
        vpos = ra_load(&e->vpos);
        from = ra_load(&e->miss_end);
        lead = ra_lead(e, vpos, from);
        from = vpos + lead;
        if (lead < RA_WINDOW && from < e->size && ra_victim(e, vpos) &&
            lead < best_lead) {
            best = e;
            best_lead = lead;
        }
        ra_unlock(e);
    }
    if (best) {
        ra_entry *e = best;
        ra_chunk *k = NULL;
        uint32_t vpos;
        uint32_t p = 0U;
        uint32_t len = 0U;
        int32_t got;
        uint64_t t0;

        ra_lock_worker(e);
        if (ra_load(&e->state) == RA_LIVE && !ra_load(&e->passthrough)) {
            uint32_t lead;
            vpos = ra_load(&e->vpos);
            lead = ra_lead(e, vpos, ra_load(&e->miss_end));
            p = vpos + lead;
            if (lead < RA_WINDOW && p < e->size)
                k = ra_victim(e, vpos);
            if (k) {
                len = e->size - p;
                if (len > ISAAC_OGG_RA_CHUNK_BYTES)
                    len = ISAAC_OGG_RA_CHUNK_BYTES;
                k->state = CH_FILLING;
                k->off = p;
                k->len = len;
            }
        }
        ra_unlock(e);
        if (k) {
            t0 = ra_now_us();
            got = ra_pread(e->wfd, k->data, len, p);
            ra_add(&s_stats.fill_us, (unsigned)(ra_now_us() - t0));
            ra_lock_worker(e);
            if (ra_load(&e->state) != RA_LIVE) {
                k->state = CH_FREE;
                ra_add(&s_stats.fill_dropped, 1U);
            } else if (got >= 0 && (uint32_t)got == len) {
                k->state = CH_READY;
                ra_add(&s_stats.fills, 1U);
                ra_add(&s_stats.fill_kb, len / 1024U);
            } else {
                /* A failed or short pread (a regular file reads short only
                 * at its end, and len stops there): stop serving this entry
                 * so the next call meets the real descriptor's own answer. */
                k->state = CH_FREE;
                ra_add(got < 0 ? &s_stats.fill_err : &s_stats.fill_short, 1U);
                ra_store(&e->passthrough, 1U);
            }
            ra_unlock(e);
            worked = 1;
        }
    }
    return worked;
}

#if RA_HOST
static void *ra_worker_main(void *arg)
{
    (void)arg;
    while (!ra_load(&s_worker_stop)) {
        if (!ra_worker_pass())
            ra_wait();
    }
    return NULL;
}
#else
static int ra_worker_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        if (!ra_worker_pass())
            ra_wait();
    }
    return 0;
}
#endif

static int ra_worker_start(void)
{
    if (s_worker_attempted)
        return (int)ra_load(&s_worker_ok);
    s_worker_attempted = 1U;
    s_report_due = 1U;
#if RA_HOST
    ra_store(&s_worker_stop, 0U);
    if (pthread_create(&s_worker_thread, NULL, ra_worker_main, NULL) != 0) {
        s_stats.worker_rc = -1;
        return 0;
    }
#else
    s_worker_flag = sceKernelCreateEventFlag("isaac_ogg_ra_wake", 0, 0, NULL);
    if (s_worker_flag < 0) {
        s_stats.worker_rc = (int)s_worker_flag;
        return 0;
    }
    s_worker_thread = sceKernelCreateThread(
        "isaac_ogg_ra", ra_worker_main, ISAAC_OGG_RA_PRIORITY,
        ISAAC_OGG_RA_STACK_BYTES, 0U, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_worker_thread < 0) {
        s_stats.worker_rc = (int)s_worker_thread;
        return 0;
    }
    {
        int rc = sceKernelStartThread(s_worker_thread, 0U, NULL);
        if (rc < 0) {
            (void)sceKernelDeleteThread(s_worker_thread);
            s_worker_thread = -1;
            s_stats.worker_rc = rc;
            return 0;
        }
    }
#endif
    s_stats.worker_rc = 0;
    s_stats.worker_started = 1U;
    ra_store(&s_worker_ok, 1U);
    return 1;
}

/* ------------------------------------------------------------------------
 * Game thread. */
static ra_entry *ra_find(int fd)
{
    uint32_t i;
    int key = fd + 1;

    for (i = 0U; i < ISAAC_OGG_RA_ENTRIES; ++i)
        if (__atomic_load_n(&s_entries[i].fd_plus1, __ATOMIC_RELAXED) == key)
            return &s_entries[i];
    return NULL;
}

static void ra_retire(ra_entry *e)
{
    __atomic_store_n(&e->fd_plus1, 0, __ATOMIC_RELAXED);
    ra_store(&e->state, RA_RETIRING);
    ra_add(&s_stats.retired, 1U);
    ra_signal();
}

static void ra_count_read(void)
{
    ++s_stats.reads;
    if (++s_reads_since_report >= ISAAC_OGG_RA_REPORT_PERIOD) {
        s_reads_since_report = 0U;
        s_report_due = 1U;
    }
}

/* Put the kernel position where newlib believes it is before a real call.
 * A SEEK_SET inside the file fails only on a dead descriptor, whose real
 * read/seek fails with the same ENODEV (host_vita_crt.c DESCRIPTOR_RECOVER
 * comment), so that answer is returned without touching the descriptor
 * again.  Never reads at an unknown position. */
static int ra_sync(ra_entry *e, struct _reent *r, int fd)
{
    ra_off_t got;

    if (e->synced)
        return 1;
    ++s_stats.syncs;
    got = __real__lseek_r(r, fd, (ra_off_t)e->vpos, SEEK_SET);
    if (got < 0 || (uint32_t)got != e->vpos) {
        ++s_stats.sync_fail;
        if (got >= 0)
            r->_errno = EIO;
        ra_store(&e->passthrough, 1U);
        return 0;
    }
    e->synced = 1U;
    return 1;
}

static ra_ssize_t ra_forward_read(ra_entry *e, struct _reent *r, int fd,
                                  void *buf, size_t n)
{
    ra_ssize_t got;
    uint64_t t0;

    if (!ra_sync(e, r, fd))
        return -1;
    ++s_stats.misses;
    if (n <= RA_WINDOW) {
        uint64_t end = (uint64_t)e->vpos + n;
        ra_store(&e->miss_end, end > e->size ? e->size : (uint32_t)end);
        ra_signal();
    }
    t0 = ra_now_us();
    got = __real__read_r(r, fd, buf, n);
    s_stats.miss_us += (unsigned)(ra_now_us() - t0);
    if (got > 0) {
        ra_store(&e->vpos, e->vpos + (uint32_t)got);
        s_stats.miss_kb += (unsigned)got / 1024U;
    } else if (got < 0) {
        /* The real descriptor answered an error: from here on it answers
         * every call itself. */
        ++s_stats.real_fail;
        ra_store(&e->passthrough, 1U);
    }
    ra_store(&e->miss_end, 0U);
    return got;
}

typedef struct ra_seg {
    const uint8_t *src;
    uint32_t len;
} ra_seg;

/* Coverage of [from, to) by READY chunks (to > from, to - from <= window). */
static int ra_cover(ra_entry *e, uint32_t from, uint32_t to, ra_seg *segs,
                    unsigned *nsegs)
{
    uint32_t q = from;
    unsigned count = 0U;
    int verdict = RA_COVERED;

    if (!ra_lock_game(e))
        return RA_BUSY;
    while (q < to) {
        const ra_chunk *hit = NULL;
        int pending = 0;
        uint32_t i;

        for (i = 0U; i < ISAAC_OGG_RA_CHUNKS; ++i) {
            const ra_chunk *k = &e->chunk[i];
            if (k->off <= q && q - k->off < k->len) {
                if (k->state == CH_READY) {
                    hit = k;
                    break;
                }
                if (k->state == CH_FILLING)
                    pending = 1;
            }
        }
        if (!hit) {
            verdict = pending ? RA_PENDING : RA_GAP;
            break;
        }
        {
            uint32_t end = hit->off + hit->len;
            if (end > to)
                end = to;
            segs[count].src = hit->data + (q - hit->off);
            segs[count].len = end - q;
            ++count;
            q = end;
        }
    }
    ra_unlock(e);
    *nsegs = count;
    return verdict;
}

static ra_ssize_t ra_read(ra_entry *e, struct _reent *r, int fd, void *buf,
                          size_t n)
{
    ra_seg segs[ISAAC_OGG_RA_CHUNKS];
    unsigned nsegs = 0U;
    uint32_t vpos = e->vpos;
    uint32_t end;
    uint32_t waited = 0U;
    uint64_t wait_t0 = 0U;
    int verdict;

    ra_count_read();
    if (ra_load(&e->passthrough)) {
        ++s_stats.pass_reads;
        return ra_forward_read(e, r, fd, buf, n);
    }
    if (n == 0U || n > RA_WINDOW)
        return ra_forward_read(e, r, fd, buf, n);
    if (vpos >= e->size) {
        /* sceIoRead at or past the end of an immutable file reads 0 bytes
         * and newlib's _read_r clears errno on success. */
        ++s_stats.hit_eof;
        r->_errno = 0;
        return 0;
    }
    end = e->size - vpos < n ? e->size : vpos + (uint32_t)n;
    for (;;) {
        verdict = ra_cover(e, vpos, end, segs, &nsegs);
        if (verdict != RA_PENDING || RA_WAIT_DONE(waited))
            break;
        if (!waited) {
            ++s_stats.waits;
            wait_t0 = ra_now_us();
        }
        ra_delay_us(ISAAC_OGG_RA_POLL_US);
        waited = (uint32_t)(ra_now_us() - wait_t0);
        if (!waited)
            waited = 1U;
    }
    s_stats.wait_us += waited;
    if (verdict == RA_COVERED) {
        uint8_t *dst = (uint8_t *)buf;
        unsigned i;

        for (i = 0U; i < nsegs; ++i) {
            memcpy(dst, segs[i].src, segs[i].len);
            dst += segs[i].len;
        }
        /* The kernel position stayed behind: the next forwarded call
         * seeks it to vpos first. */
        ra_store(&e->vpos, end);
        e->synced = 0U;
        ++s_stats.hits;
        s_stats.hit_kb += (end - vpos) / 1024U;
        r->_errno = 0;
        ra_signal();
        return (ra_ssize_t)(end - vpos);
    }
    if (verdict == RA_PENDING)
        ++s_stats.wait_timeouts;
    else if (verdict == RA_BUSY)
        ++s_stats.lock_busy;
    return ra_forward_read(e, r, fd, buf, n);
}

/* A registered descriptor used by a thread other than the one that
 * registered it.  Guest stdio never does that (every guest FILE call runs on
 * the game thread, the only owner of host_vita_crt.c s_file_lock), so this
 * is host code reaching the same descriptor; it is ordered with the owner's
 * calls only as far as that code orders itself, exactly as on the unwrapped
 * descriptor.  The descriptor leaves the read-ahead for good: the kernel
 * position goes back to newlib's view first, then this call and every later
 * one, the owner's included, is the real call. */
static void ra_foreign(ra_entry *e, struct _reent *r, int fd)
{
    ra_add(&s_stats.foreign, 1U);
    if (!e->synced) {
        int saved_errno = r->_errno;
        /* A dead descriptor fails here; the owner's next call retries the
         * sync through ra_sync and answers that failure. */
        if (__real__lseek_r(r, fd, (ra_off_t)e->vpos, SEEK_SET) ==
            (ra_off_t)e->vpos)
            e->synced = 1U;
        r->_errno = saved_errno;
    }
    ra_store(&e->passthrough, 1U);
}

ra_ssize_t __wrap__read_r(struct _reent *r, int fd, void *buf, size_t n)
{
    ra_entry *e = fd >= 0 ? ra_find(fd) : NULL;

    if (!e)
        return __real__read_r(r, fd, buf, n);
    if (e->owner != ra_self()) {
        ra_foreign(e, r, fd);
        return __real__read_r(r, fd, buf, n);
    }
    return ra_read(e, r, fd, buf, n);
}

ra_off_t __wrap__lseek_r(struct _reent *r, int fd, ra_off_t off, int dir)
{
    ra_entry *e = fd >= 0 ? ra_find(fd) : NULL;
    ra_off_t got;

    if (!e)
        return __real__lseek_r(r, fd, off, dir);
    if (e->owner != ra_self()) {
        ra_foreign(e, r, fd);
        return __real__lseek_r(r, fd, off, dir);
    }
    if (!ra_load(&e->passthrough) && (dir == SEEK_SET || dir == SEEK_CUR)) {
        int64_t target = (int64_t)off;
        if (dir == SEEK_CUR)
            target += (int64_t)e->vpos;
        if (target >= 0 && target <= (int64_t)e->size) {
            /* sceIoLseek32 answers any in-file target with the target and
             * newlib's _lseek_r leaves errno alone on success. */
            ++s_stats.vseeks;
            if ((uint32_t)target != e->vpos) {
                ra_store(&e->vpos, (uint32_t)target);
                e->synced = 0U;
                ra_signal();
            }
            return (ra_off_t)target;
        }
    }
    ++s_stats.fseeks;
    if (!ra_sync(e, r, fd))
        return -1;
    got = __real__lseek_r(r, fd, off, dir);
    if (got >= 0) {
        ra_store(&e->vpos, (uint32_t)got);
        ra_signal();
    }
    return got;
}

int __wrap__close_r(struct _reent *r, int fd)
{
    ra_entry *e = fd >= 0 ? ra_find(fd) : NULL;

    if (e) {
        if (e->owner != ra_self())
            ra_add(&s_stats.foreign, 1U);
        /* Before the real close: once newlib releases the number, the next
         * open may reuse it. */
        ra_retire(e);
    }
    return __real__close_r(r, fd);
}

void isaac_vita_ogg_ra_note(int fd, const char *path)
{
    struct _reent *r;
    ra_entry *e = NULL;
    size_t length;
    ra_off_t pos;
    uint32_t size = 0U;
    int saved_errno;
    uint32_t i;

    if (fd < 0 || !path || ra_find(fd))
        return;
    length = strlen(path);
    if (length == 0U || length >= ISAAC_OGG_RA_PATH_CAPACITY) {
        ++s_stats.reg_fail;
        return;
    }
    if (!ra_worker_start()) {
        ++s_stats.reg_fail;
        return;
    }
    for (i = 0U; i < ISAAC_OGG_RA_ENTRIES; ++i)
        if (ra_load(&s_entries[i].state) == RA_FREE) {
            e = &s_entries[i];
            break;
        }
    if (!e) {
        ++s_stats.reg_full;
        return;
    }
    if (!e->buffer && (s_alloc_failed || !ra_alloc(e))) {
        s_alloc_failed = 1U;
        ++s_stats.reg_fail;
        return;
    }
    r = ra_reent();
    saved_errno = r->_errno;
    pos = __real__lseek_r(r, fd, 0, SEEK_CUR);
    if (pos < 0 || !ra_fstat_size(fd, &size) || (uint32_t)pos > size) {
        r->_errno = saved_errno;
        ++s_stats.reg_fail;
        return;
    }
    r->_errno = saved_errno;
    /* The entry is FREE: the worker does not look at it until LIVE. */
    memcpy(e->path, path, length + 1U);
    e->size = size;
    e->vpos = (uint32_t)pos;
    e->miss_end = 0U;
    e->synced = 1U;
    e->owner = ra_self();
    e->wfd = -1;
    ra_store(&e->passthrough, 0U);
    for (i = 0U; i < ISAAC_OGG_RA_CHUNKS; ++i)
        e->chunk[i].state = CH_FREE;
    ra_store(&e->state, RA_LIVE);
    __atomic_store_n(&e->fd_plus1, fd + 1, __ATOMIC_RELAXED);
    ++s_stats.registered;
    ra_signal();
}

void isaac_vita_ogg_ra_forget(int fd)
{
    ra_entry *e = fd >= 0 ? ra_find(fd) : NULL;

    if (!e || e->owner != ra_self())
        return;
    if (!e->synced) {
        struct _reent *r = ra_reent();
        int saved_errno = r->_errno;
        /* An idle archive FILE is reset by newlib calls that may refill
         * without a seek: hand it back at newlib's own position.  A dead
         * descriptor fails here and fails every later call as well. */
        ++s_stats.forget_sync;
        if (__real__lseek_r(r, fd, (ra_off_t)e->vpos, SEEK_SET) ==
            (ra_off_t)e->vpos)
            e->synced = 1U;
        r->_errno = saved_errno;
    }
    ra_retire(e);
}

void isaac_vita_ogg_ra_snapshot(isaac_vita_ogg_ra_stats *out)
{
    const unsigned *src = (const unsigned *)&s_stats;
    unsigned *dst = (unsigned *)out;
    size_t i;

    _Static_assert(sizeof(isaac_vita_ogg_ra_stats) % sizeof(unsigned) == 0U,
                   "stats are whole words");
    for (i = 0U; i < sizeof(isaac_vita_ogg_ra_stats) / sizeof(unsigned); ++i)
        dst[i] = ra_peek(&src[i]);
}

void isaac_vita_ogg_ra_report_if_due(void)
{
    isaac_vita_ogg_ra_stats s;
    int saved_errno;
    unsigned live = 0U;
    uint32_t i;

    if (!s_report_due)
        return;
    s_report_due = 0U;
    saved_errno = errno;
    isaac_vita_ogg_ra_snapshot(&s);
    for (i = 0U; i < ISAAC_OGG_RA_ENTRIES; ++i)
        if (ra_load(&s_entries[i].state) == RA_LIVE)
            ++live;
    if (s_reports++ == 0U)
        isaac_vita_log(
            "KAGE VITA OGG READAHEAD: worker=%s rc=0x%08x prio=%d cpu=2 "
            "entries=%u chunks=%u chunk=%u window=%u wait_us=%u",
            s.worker_started ? "on" : "off", (unsigned)s.worker_rc,
            (int)ISAAC_OGG_RA_PRIORITY, (unsigned)ISAAC_OGG_RA_ENTRIES,
            (unsigned)ISAAC_OGG_RA_CHUNKS, (unsigned)ISAAC_OGG_RA_CHUNK_BYTES,
            (unsigned)RA_WINDOW, (unsigned)ISAAC_OGG_RA_WAIT_US);
    isaac_vita_log(
        "KAGE VITA OGG READAHEAD STATS: reads=%u hits=%u eof=%u miss=%u "
        "hit_kb=%u miss_kb=%u miss_ms=%u waits=%u wait_ms=%u wait_to=%u "
        "busy=%u vseek=%u fseek=%u sync=%u sync_fail=%u pass=%u real_fail=%u "
        "foreign=%u reg=%u full=%u reg_fail=%u retired=%u fsync=%u live=%u",
        s.reads, s.hits, s.hit_eof, s.misses, s.hit_kb, s.miss_kb,
        s.miss_us / 1000U, s.waits, s.wait_us / 1000U, s.wait_timeouts,
        s.lock_busy, s.vseeks, s.fseeks, s.syncs, s.sync_fail, s.pass_reads,
        s.real_fail, s.foreign, s.registered, s.reg_full, s.reg_fail,
        s.retired, s.forget_sync, live);
    isaac_vita_log(
        "KAGE VITA OGG READAHEAD WORKER: fills=%u fill_kb=%u fill_ms=%u "
        "fill_err=%u short=%u open_fail=%u dropped=%u",
        s.fills, s.fill_kb, s.fill_us / 1000U, s.fill_err, s.fill_short,
        s.open_fail, s.fill_dropped);
    errno = saved_errno;
}

#if RA_HOST
/* Take (hold != 0) or release the entry lock of a registered descriptor the
 * way the worker takes it, so the game's bounded trylock in ra_cover gives
 * up by construction rather than by a lucky race. */
int isaac_vita_ogg_ra_oracle_hold_lock(int fd, int hold)
{
    ra_entry *e = fd >= 0 ? ra_find(fd) : NULL;

    if (!e)
        return 0;
    if (hold)
        ra_lock_worker(e);
    else
        ra_unlock(e);
    return 1;
}

unsigned isaac_vita_ogg_ra_oracle_busy_entries(void)
{
    unsigned busy = 0U;
    uint32_t i;

    for (i = 0U; i < ISAAC_OGG_RA_ENTRIES; ++i)
        if (ra_load(&s_entries[i].state) != RA_FREE)
            ++busy;
    return busy;
}

/* Join the worker and return the module to its initial state (buffers are
 * kept, as on the target). */
void isaac_vita_ogg_ra_oracle_shutdown(void)
{
    uint32_t i;

    if (ra_load(&s_worker_ok)) {
        ra_store(&s_worker_stop, 1U);
        ra_store(&s_worker_ok, 0U);
        pthread_mutex_lock(&s_wake_mutex);
        s_wake_pending = 1U;
        pthread_cond_signal(&s_wake_cond);
        pthread_mutex_unlock(&s_wake_mutex);
        pthread_join(s_worker_thread, NULL);
    }
    for (i = 0U; i < ISAAC_OGG_RA_ENTRIES; ++i) {
        ra_entry *e = &s_entries[i];
        if (e->state != RA_FREE && e->wfd >= 0)
            ra_close(e->wfd);
        e->fd_plus1 = 0;
        e->state = RA_FREE;
        e->passthrough = 0U;
        e->lock = 0U;
        e->wfd = -1;
    }
    memset(&s_stats, 0, sizeof s_stats);
    s_report_due = 0U;
    s_reads_since_report = 0U;
    s_reports = 0U;
    s_worker_attempted = 0U;
    s_alloc_failed = 0U;
    s_wake_pending = 0U;
    isaac_vita_ogg_ra_context = 0;
}
#endif
