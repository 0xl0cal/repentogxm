/* Host oracle for ISAAC_VITA_OGG_READAHEAD (host_vita_ogg_readahead.c).
 *
 * Linux/glibc only (fopencookie, pread, pthreads).  Built together with the
 * module under -DISAAC_VITA_OGG_READAHEAD_ORACLE=1; this file supplies the
 * __real_ side of the three newlib wraps (over POSIX descriptors, with the
 * Vita newlib errno contract: _read_r clears errno on success, _lseek_r
 * leaves it alone, _close_r clears it), the worker's descriptor calls with
 * fault and delay injection, and four scenario families:
 *
 *   trace   random newlib-shaped _read_r/_lseek_r sequences on a registered
 *           descriptor against the same sequence on an unwrapped reference
 *           descriptor of the same file: every return value, errno and byte
 *           must match, under random worker and game delays;
 *   faults  worker pread error, short pread, open failure, a dead
 *           descriptor (suspend) followed by the CRT-style recovery, a
 *           suspend whose worker descriptor stays live (late open), a held
 *           entry lock (lock busy), fd reuse after close, foreign threads,
 *           table full, refused registrations, a retire while a pread is in
 *           flight, forget;
 *   stdio   stb_vorbis v1.04 (the vendored copy) in FILE mode on a 16 KiB
 *           setvbuf stdio stream over the wrapped descriptor versus an
 *           unwrapped one, OGGs embedded at odd offsets of one archive file,
 *           full decode plus random sample seeks: PCM byte-identical, and
 *           equal to stb_vorbis_decode_memory of the original file;
 *   blocks  an ArchivedFile-shaped reader (4-byte header + <= 0x800 payload
 *           blocks, Seek = re-read from the entry start) feeding 24 KiB
 *           top-ups like nv_topup, two loops with rewinds and a mid-stream
 *           seek, decoded from memory: PCM byte-identical on both paths.
 *
 * Usage: oracle <scratch-dir> [<file.ogg>...]; without OGG files the stdio
 * and blocks families have nothing to decode (the run says SKIPPED).
 * RA_TRACE_SEEDS (default 12) sets the number of trace seeds;
 * RA_CENSUS_EXEMPT names extra census rows allowed to stay zero (a
 * comma-separated list, for non-default geometries). */

#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "host_vita_ogg_readahead.h"
#include "host_vita_ogg_readahead_oracle.h"

#ifndef ISAAC_OGG_RA_CHUNK_BYTES
#define ISAAC_OGG_RA_CHUNK_BYTES 16384U
#endif
#ifndef ISAAC_OGG_RA_CHUNKS
#define ISAAC_OGG_RA_CHUNKS 4U
#endif
#ifndef ISAAC_OGG_RA_WAIT_US
#define ISAAC_OGG_RA_WAIT_US 20000U
#endif
#define WINDOW (ISAAC_OGG_RA_CHUNK_BYTES * ISAAC_OGG_RA_CHUNKS)
/* host_vita_crt.h ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE: the setvbuf size
 * every CRT FILE token gets. */
#define CRT_BUFFER 16384U

static int s_failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++s_failures; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ------------------------------------------------------------------------
 * Deterministic PRNG (xorshift64*). */
static uint64_t s_rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rng_next(void)
{
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return s_rng * 0x2545f4914f6cdd1dULL;
}
static uint32_t rng_below(uint32_t n) { return n ? (uint32_t)(rng_next() % n) : 0U; }

/* ------------------------------------------------------------------------
 * Fake newlib (the __real_ side). */
#define MAX_FD 1024
static uint8_t s_dead[MAX_FD];          /* descriptor answers ENODEV */
static unsigned s_real_reads;
static unsigned s_real_seeks;
static unsigned s_real_read_delay_us;   /* emulated card latency */
static __thread struct _reent s_tls_reent;
static __thread uint32_t s_tls_tid;
static uint32_t s_next_tid;

struct _reent *isaac_ogg_ra_oracle_reent(void) { return &s_tls_reent; }

uint32_t isaac_ogg_ra_oracle_thread_id(void)
{
    if (!s_tls_tid)
        s_tls_tid = __atomic_add_fetch(&s_next_tid, 1U, __ATOMIC_RELAXED);
    return s_tls_tid;
}

isaac_ogg_ra_ssize_t __real__read_r(struct _reent *r, int fd, void *buf,
                                    size_t n)
{
    ssize_t got;

    __atomic_fetch_add(&s_real_reads, 1U, __ATOMIC_RELAXED);
    if (fd >= 0 && fd < MAX_FD && s_dead[fd]) {
        r->_errno = ENODEV;
        return -1;
    }
    if (s_real_read_delay_us)
        usleep(s_real_read_delay_us);
    got = read(fd, buf, n);
    if (got < 0) {
        r->_errno = errno;
        return -1;
    }
    r->_errno = 0;
    return (isaac_ogg_ra_ssize_t)got;
}

isaac_ogg_ra_off_t __real__lseek_r(struct _reent *r, int fd,
                                   isaac_ogg_ra_off_t off, int dir)
{
    off_t got;

    __atomic_fetch_add(&s_real_seeks, 1U, __ATOMIC_RELAXED);
    if (fd >= 0 && fd < MAX_FD && s_dead[fd]) {
        r->_errno = ENODEV;
        return -1;
    }
    got = lseek(fd, (off_t)off, dir);
    if (got < 0) {
        r->_errno = errno;
        return -1;
    }
    if (got > INT32_MAX) {
        /* sceIoLseek32 cannot answer past 2 GiB; never reached here. */
        r->_errno = EOVERFLOW;
        return -1;
    }
    return (isaac_ogg_ra_off_t)got;
}

int __real__close_r(struct _reent *r, int fd)
{
    if (fd >= 0 && fd < MAX_FD)
        s_dead[fd] = 0U;
    if (close(fd) != 0) {
        r->_errno = errno;
        return -1;
    }
    r->_errno = 0;
    return 0;
}

/* fstat failure injection for one descriptor. */
static int s_fstat_fail_fd = -1;
static int fstat_fail_once(int fd)
{
    if (fd == s_fstat_fail_fd) {
        s_fstat_fail_fd = -1;
        return 1;
    }
    return 0;
}

int isaac_ogg_ra_oracle_fstat_size(int fd, uint32_t *size)
{
    struct stat st;

    if (fstat_fail_once(fd))
        return -1;
    if (fstat(fd, &st) != 0)
        return -1;
    *size = (uint32_t)st.st_size;
    return 0;
}

/* ------------------------------------------------------------------------
 * Worker side, with injection. */
static unsigned s_pread_calls;          /* per scenario (injection index) */
static unsigned s_pread_total;
static unsigned s_pread_fail_at;        /* 1-based call number, 0 = never */
static unsigned s_pread_short_at;
static unsigned s_pread_delay_max_us;
static unsigned s_pread_delay_fixed_us;
static unsigned s_workers_dead;         /* every pread fails ENODEV */
static unsigned s_open_fail;
static unsigned s_open_calls;

int isaac_ogg_ra_oracle_open(const char *path)
{
    __atomic_fetch_add(&s_open_calls, 1U, __ATOMIC_RELAXED);
    if (__atomic_load_n(&s_open_fail, __ATOMIC_ACQUIRE))
        return -1;
    return open(path, O_RDONLY);
}

int32_t isaac_ogg_ra_oracle_pread(int wfd, void *buf, uint32_t n,
                                  uint32_t offset)
{
    unsigned call = __atomic_add_fetch(&s_pread_calls, 1U, __ATOMIC_RELAXED);
    unsigned delay = __atomic_load_n(&s_pread_delay_fixed_us, __ATOMIC_RELAXED);
    unsigned dmax = __atomic_load_n(&s_pread_delay_max_us, __ATOMIC_RELAXED);
    ssize_t got;

    if (dmax)
        delay += (unsigned)(((uint64_t)call * 2654435761ULL) % dmax);
    if (delay)
        usleep(delay);
    if (__atomic_load_n(&s_workers_dead, __ATOMIC_ACQUIRE))
        return -19;
    if (call == __atomic_load_n(&s_pread_fail_at, __ATOMIC_RELAXED))
        return -5;
    got = pread(wfd, buf, n, (off_t)offset);
    if (got < 0)
        return -errno;
    if (call == __atomic_load_n(&s_pread_short_at, __ATOMIC_RELAXED) &&
        got > 1)
        return (int32_t)(got / 2);
    return (int32_t)got;
}

int isaac_ogg_ra_oracle_close(int wfd) { return close(wfd); }

static unsigned s_log_lines;
static unsigned s_log_max;
void isaac_vita_log(const char *format, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    ++s_log_lines;
    if (n > 0 && (unsigned)n > s_log_max)
        s_log_max = (unsigned)n;
    /* The durable logger keeps 383 body bytes (host_vita_crt.c). */
    CHECK(n > 0 && n < 383, "log line too long (%d): %s", n, line);
    printf("  log: %s\n", line);
}

/* ------------------------------------------------------------------------
 * Stats accumulation across scenarios (branch census). */
static isaac_vita_ogg_ra_stats s_total;

static void stats_fold(const char *name)
{
    isaac_vita_ogg_ra_stats s;
    unsigned *dst = (unsigned *)&s_total;
    const unsigned *src = (const unsigned *)&s;
    size_t i;

    isaac_vita_ogg_ra_snapshot(&s);
    printf("  [%s] reads=%u hits=%u eof=%u miss=%u waits=%u wait_to=%u "
           "busy=%u vseek=%u fseek=%u sync=%u sync_fail=%u pass=%u "
           "real_fail=%u foreign=%u reg=%u full=%u reg_fail=%u retired=%u "
           "fsync=%u fills=%u fill_err=%u short=%u open_fail=%u dropped=%u\n",
           name, s.reads, s.hits, s.hit_eof, s.misses, s.waits,
           s.wait_timeouts, s.lock_busy, s.vseeks, s.fseeks, s.syncs,
           s.sync_fail, s.pass_reads, s.real_fail, s.foreign, s.registered,
           s.reg_full, s.reg_fail, s.retired, s.forget_sync, s.fills,
           s.fill_err, s.fill_short, s.open_fail, s.fill_dropped);
    for (i = 0U; i < sizeof s / sizeof(unsigned); ++i)
        dst[i] += src[i];
}

static void wait_idle(void)
{
    int i;

    for (i = 0; i < 2000 && isaac_vita_ogg_ra_oracle_busy_entries(); ++i)
        usleep(1000);
}

static void scenario_end(const char *name)
{
    wait_idle();
    CHECK(isaac_vita_ogg_ra_oracle_busy_entries() == 0U,
          "%s: entries still busy after close", name);
    isaac_vita_ogg_ra_report_if_due();
    stats_fold(name);
    isaac_vita_ogg_ra_oracle_shutdown();
    s_pread_fail_at = s_pread_short_at = 0U;
    s_pread_delay_max_us = s_pread_delay_fixed_us = 0U;
    s_workers_dead = s_open_fail = 0U;
    s_pread_total += s_pread_calls;
    s_pread_calls = 0U;
    s_real_read_delay_us = 0U;
    memset(s_dead, 0, sizeof s_dead);
}

/* ------------------------------------------------------------------------
 * Files. */
static uint8_t *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t *)malloc((size_t)n + 1U);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return data;
}

static void put_random(FILE *f, size_t n)
{
    while (n--)
        fputc((int)(rng_next() >> 56), f);
}

typedef struct entry {
    uint32_t off;       /* raw archive: OGG bytes; block archive: first block */
    uint32_t len;       /* OGG length */
    uint8_t *ogg;
} entry;

#define MAX_OGG 8
static entry s_raw[MAX_OGG];
static entry s_blk[MAX_OGG];
static unsigned s_ogg_count;
static char s_raw_path[1024];
static char s_blk_path[1024];
static uint32_t s_raw_size;

static int build_archives(const char *dir, char **oggs, unsigned count)
{
    FILE *raw;
    FILE *blk;
    unsigned i;

    snprintf(s_raw_path, sizeof s_raw_path, "%s/ra-raw.a", dir);
    snprintf(s_blk_path, sizeof s_blk_path, "%s/ra-blk.a", dir);
    raw = fopen(s_raw_path, "wb");
    blk = fopen(s_blk_path, "wb");
    if (!raw || !blk)
        return 0;
    put_random(raw, 1048576U + 777U);
    put_random(blk, 307200U + 13U);
    for (i = 0U; i < count && i < MAX_OGG; ++i) {
        size_t len = 0U;
        uint8_t *ogg = read_whole(oggs[i], &len);
        size_t pos = 0U;

        if (!ogg || !len)
            return 0;
        s_raw[i].off = (uint32_t)ftell(raw);
        s_raw[i].len = (uint32_t)len;
        s_raw[i].ogg = ogg;
        fwrite(ogg, 1, len, raw);
        put_random(raw, 4097U + 31U * i);
        s_blk[i].off = (uint32_t)ftell(blk);
        s_blk[i].len = (uint32_t)len;
        s_blk[i].ogg = ogg;
        while (pos < len) {
            uint32_t n = 1U + rng_below(0x800U);
            uint8_t h[4];
            if (n > len - pos)
                n = (uint32_t)(len - pos);
            h[0] = (uint8_t)n; h[1] = (uint8_t)(n >> 8);
            h[2] = (uint8_t)(n >> 16); h[3] = (uint8_t)(n >> 24);
            fwrite(h, 1, 4, blk);
            fwrite(ogg + pos, 1, n, blk);
            pos += n;
        }
        fwrite("\0\0\0\0", 1, 4, blk);
        put_random(blk, 2048U + 7U * i);
    }
    s_raw_size = (uint32_t)ftell(raw);
    fclose(raw);
    fclose(blk);
    s_ogg_count = i;
    return 1;
}

/* ------------------------------------------------------------------------
 * Scenario: trace equivalence. */
typedef struct pair {
    int wfd;            /* registered, through the wraps */
    int rfd;            /* reference, direct */
    struct _reent wr;
    struct _reent rr;
    uint8_t *wbuf;
    uint8_t *rbuf;
    unsigned ops;
} pair;

#define SENTINEL 4242

static void pair_read(pair *p, size_t n, const char *tag)
{
    isaac_ogg_ra_ssize_t a;
    isaac_ogg_ra_ssize_t b;

    p->wr._errno = SENTINEL;
    p->rr._errno = SENTINEL;
    a = __wrap__read_r(&p->wr, p->wfd, p->wbuf, n);
    b = __real__read_r(&p->rr, p->rfd, p->rbuf, n);
    ++p->ops;
    CHECK(a == b, "%s op %u read(%zu): wrapped %d reference %d", tag, p->ops,
          n, (int)a, (int)b);
    CHECK(p->wr._errno == p->rr._errno, "%s op %u read errno %d vs %d", tag,
          p->ops, p->wr._errno, p->rr._errno);
    if (a == b && a > 0)
        CHECK(memcmp(p->wbuf, p->rbuf, (size_t)a) == 0,
              "%s op %u read bytes differ", tag, p->ops);
}

static void pair_seek(pair *p, isaac_ogg_ra_off_t off, int dir,
                      const char *tag)
{
    isaac_ogg_ra_off_t a;
    isaac_ogg_ra_off_t b;

    p->wr._errno = SENTINEL;
    p->rr._errno = SENTINEL;
    a = __wrap__lseek_r(&p->wr, p->wfd, off, dir);
    b = __real__lseek_r(&p->rr, p->rfd, off, dir);
    ++p->ops;
    CHECK(a == b, "%s op %u lseek(%d,%d): wrapped %d reference %d", tag,
          p->ops, (int)off, dir, (int)a, (int)b);
    CHECK(p->wr._errno == p->rr._errno, "%s op %u lseek errno %d vs %d", tag,
          p->ops, p->wr._errno, p->rr._errno);
}

/* A freshly created worker thread can take tens of milliseconds to run on a
 * loaded host (measured under WSL2 with strace: 30 ms from clone3 to the
 * worker's first syscall), longer than several fault scenarios last.  The
 * scenarios wait for the worker's own calls so that they exercise the
 * interleaving they are named after rather than thread start-up; the trace
 * and stdio families keep their random timing. */
static void wait_counter(const unsigned *counter, unsigned target)
{
    unsigned i;

    for (i = 0U; i < 2000U &&
                 __atomic_load_n(counter, __ATOMIC_ACQUIRE) < target; ++i)
        usleep(1000U);
    CHECK(__atomic_load_n(counter, __ATOMIC_ACQUIRE) >= target,
          "worker never reached call %u", target);
}

/* Register `fd` as the CRT would and, when the registration took, wait for
 * the worker to open its own descriptor (the open hook counts attempts, so
 * an injected open failure counts too). */
static void note_and_wait(int fd, const char *path)
{
    isaac_vita_ogg_ra_stats before;
    isaac_vita_ogg_ra_stats after;
    unsigned opens = __atomic_load_n(&s_open_calls, __ATOMIC_ACQUIRE);

    isaac_vita_ogg_ra_snapshot(&before);
    isaac_vita_ogg_ra_context = 1;
    isaac_vita_ogg_ra_note(fd, path);
    isaac_vita_ogg_ra_context = 0;
    isaac_vita_ogg_ra_snapshot(&after);
    if (after.registered > before.registered)
        wait_counter(&s_open_calls, opens + 1U);
}

static int pair_open(pair *p, const char *path, int register_it)
{
    memset(p, 0, sizeof *p);
    p->wfd = open(path, O_RDONLY);
    p->rfd = open(path, O_RDONLY);
    p->wbuf = (uint8_t *)malloc(200000U);
    p->rbuf = (uint8_t *)malloc(200000U);
    if (p->wfd < 0 || p->rfd < 0 || !p->wbuf || !p->rbuf)
        return 0;
    if (register_it)
        note_and_wait(p->wfd, path);
    return 1;
}

static void pair_close(pair *p, const char *tag)
{
    int a;
    int b;

    p->wr._errno = SENTINEL;
    p->rr._errno = SENTINEL;
    a = __wrap__close_r(&p->wr, p->wfd);
    b = __real__close_r(&p->rr, p->rfd);
    CHECK(a == b && p->wr._errno == p->rr._errno, "%s close %d/%d errno %d/%d",
          tag, a, b, p->wr._errno, p->rr._errno);
    free(p->wbuf);
    free(p->rbuf);
}

/* newlib-shaped operation mix: 16 KiB refills, 1 KiB-aligned optimized
 * seeks, SEEK_CUR 0 tells, SEEK_END size probes, out-of-range targets. */
static void random_op(pair *p, uint32_t size, uint32_t *logical,
                      const char *tag)
{
    uint32_t pick = rng_below(100U);

    if (pick < 50U) {
        pair_read(p, CRT_BUFFER, tag);
    } else if (pick < 58U) {
        unsigned burst = 2U + rng_below(10U);
        while (burst--) {
            pair_read(p, CRT_BUFFER, tag);
            usleep(rng_below(2500U));
        }
    } else if (pick < 64U) {
        pair_read(p, 1U + rng_below(70000U), tag);
    } else if (pick < 65U) {
        pair_read(p, 0U, tag);
    } else if (pick < 73U) {
        int64_t t = (int64_t)rng_below(size + 60000U) - 200;
        pair_seek(p, (isaac_ogg_ra_off_t)t, SEEK_SET, tag);
    } else if (pick < 81U) {
        uint32_t t = *logical > 4096U ? (*logical - rng_below(4096U)) & ~1023U
                                      : 0U;
        pair_seek(p, (isaac_ogg_ra_off_t)t, SEEK_SET, tag);
    } else if (pick < 88U) {
        pair_seek(p, 0, SEEK_CUR, tag);
    } else if (pick < 92U) {
        pair_seek(p, (isaac_ogg_ra_off_t)((int32_t)rng_below(200000U) - 100000),
                  SEEK_CUR, tag);
    } else if (pick < 95U) {
        pair_seek(p, (isaac_ogg_ra_off_t)((int32_t)rng_below(60000U) - 50000),
                  SEEK_END, tag);
    } else {
        usleep(rng_below(4000U));
    }
    {
        off_t cur = lseek(p->rfd, 0, SEEK_CUR);
        *logical = cur > 0 ? (uint32_t)cur : 0U;
    }
}

static void scenario_trace(unsigned seed, unsigned ops, unsigned pread_delay,
                           unsigned card_delay)
{
    pair p;
    uint32_t logical = 0U;
    char tag[64];
    unsigned i;

    snprintf(tag, sizeof tag, "trace-%u", seed);
    s_rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)seed * 0x100000001b3ULL);
    s_pread_delay_max_us = pread_delay;
    s_real_read_delay_us = card_delay;
    CHECK(pair_open(&p, s_raw_path, 1), "%s open", tag);
    /* Start inside the first OGG so the head of the file is exercised. */
    pair_seek(&p, (isaac_ogg_ra_off_t)s_raw[0].off, SEEK_SET, tag);
    for (i = 0U; i < ops; ++i)
        random_op(&p, s_raw_size, &logical, tag);
    pair_close(&p, tag);
    scenario_end(tag);
}

/* ------------------------------------------------------------------------
 * Scenario: faults. */
static void sequential(pair *p, unsigned reads, unsigned pause_us,
                       const char *tag)
{
    while (reads--) {
        pair_read(p, CRT_BUFFER, tag);
        if (pause_us)
            usleep(pause_us);
    }
}

static void scenario_pread_error(void)
{
    pair p;

    /* armed before the worker starts: its third pread fails */
    s_pread_fail_at = 3U;
    CHECK(pair_open(&p, s_raw_path, 1), "pread-error open");
    sequential(&p, 40U, 1500U, "pread-error");
    pair_seek(&p, 1000, SEEK_SET, "pread-error");
    sequential(&p, 8U, 0U, "pread-error");
    pair_close(&p, "pread-error");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.fill_err == 1U && s.pass_reads > 0U,
              "pread-error: fill_err=%u pass=%u", s.fill_err, s.pass_reads);
    }
    scenario_end("pread-error");
}

static void scenario_pread_short(void)
{
    pair p;

    s_pread_short_at = 2U;
    CHECK(pair_open(&p, s_raw_path, 1), "pread-short open");
    sequential(&p, 30U, 1500U, "pread-short");
    pair_close(&p, "pread-short");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.fill_short == 1U, "pread-short: short=%u", s.fill_short);
    }
    scenario_end("pread-short");
}

static void scenario_open_fail(void)
{
    pair p;

    s_open_fail = 1U;
    CHECK(pair_open(&p, s_raw_path, 1), "open-fail open");
    sequential(&p, 20U, 1000U, "open-fail");
    pair_seek(&p, 12345, SEEK_SET, "open-fail");
    sequential(&p, 4U, 0U, "open-fail");
    pair_close(&p, "open-fail");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.open_fail == 1U && s.hits == 0U,
              "open-fail: open_fail=%u hits=%u", s.open_fail, s.hits);
    }
    scenario_end("open-fail");
}

/* Suspend: both the game descriptor and the worker's descriptors die.  The
 * wrapped side may keep answering from READY chunks (the documented
 * difference) but every byte it returns must be the file's byte at that
 * position, at most one window of bytes may be served after the death, and
 * the first failure must be the real descriptor's ENODEV; then the CRT
 * recovery (close the dead FILE, reopen by path, seek to the logical cursor)
 * continues exactly. */
static void scenario_suspend(void)
{
    int fd = open(s_raw_path, O_RDONLY);
    int live = open(s_raw_path, O_RDONLY);
    struct _reent r = { 0 };
    uint8_t *buf = (uint8_t *)malloc(CRT_BUFFER);
    uint8_t *ref = (uint8_t *)malloc(CRT_BUFFER);
    uint32_t logical = s_raw[0].off;
    uint32_t after_death = 0U;
    int failed = 0;
    unsigned i;
    isaac_ogg_ra_ssize_t got;

    lseek(fd, (off_t)logical, SEEK_SET);
    note_and_wait(fd, s_raw_path);
    for (i = 0U; i < 12U; ++i) {
        got = __wrap__read_r(&r, fd, buf, CRT_BUFFER);
        CHECK(got == (isaac_ogg_ra_ssize_t)CRT_BUFFER, "suspend pre read %d",
              (int)got);
        CHECK(pread(live, ref, CRT_BUFFER, logical) == (ssize_t)CRT_BUFFER &&
              memcmp(buf, ref, CRT_BUFFER) == 0, "suspend pre bytes");
        logical += (uint32_t)got;
        usleep(3000U);
    }
    /* Power button. */
    s_dead[fd] = 1U;
    s_workers_dead = 1U;
    for (i = 0U; i < 64U && !failed; ++i) {
        r._errno = SENTINEL;
        got = __wrap__read_r(&r, fd, buf, CRT_BUFFER);
        if (got < 0) {
            CHECK(r._errno == ENODEV, "suspend: first failure errno %d",
                  r._errno);
            failed = 1;
            break;
        }
        CHECK(pread(live, ref, (size_t)got, logical) == got &&
              memcmp(buf, ref, (size_t)got) == 0,
              "suspend: post-death bytes differ at %u", logical);
        logical += (uint32_t)got;
        after_death += (uint32_t)got;
        usleep(2000U);
    }
    CHECK(failed, "suspend: dead descriptor never surfaced");
    CHECK(after_death <= WINDOW, "suspend: %u bytes served after death "
          "(window %u)", after_death, (unsigned)WINDOW);
    r._errno = SENTINEL;
    CHECK(__wrap__lseek_r(&r, fd, 0, SEEK_CUR) == -1 && r._errno == ENODEV,
          "suspend: seek after failure must hit the dead descriptor");
    printf("  suspend: %u bytes served from chunks after the descriptor died "
           "(window %u)\n", after_death, (unsigned)WINDOW);
    /* CRT recovery: close the dead FILE (the refused close is the archive
     * cache's business), reopen by path, seek to the cursor, redo. */
    (void)__wrap__close_r(&r, fd);
    s_workers_dead = 0U;
    fd = open(s_raw_path, O_RDONLY);
    CHECK(__wrap__lseek_r(&r, fd, (isaac_ogg_ra_off_t)logical, SEEK_SET) ==
          (isaac_ogg_ra_off_t)logical, "suspend: reopen seek");
    note_and_wait(fd, s_raw_path);
    for (i = 0U; i < 30U; ++i) {
        /* The file's own answer at the cursor (0 at the end of a short
         * archive, when only a few OGGs were given). */
        ssize_t want = pread(live, ref, CRT_BUFFER, logical);
        got = __wrap__read_r(&r, fd, buf, CRT_BUFFER);
        CHECK(got >= 0 && got == want &&
              (got == 0 || memcmp(buf, ref, (size_t)got) == 0),
              "suspend: recovered read %u: %d, file says %d", i, (int)got,
              (int)want);
        if (got > 0)
            logical += (uint32_t)got;
        usleep(1500U);
    }
    (void)__wrap__close_r(&r, fd);
    close(live);
    free(buf);
    free(ref);
    scenario_end("suspend");
}

/* Suspend with a late worker open: note() ran just before the suspend, so
 * the worker opens its own descriptor only after the resume and that
 * descriptor is live while the game's is dead.  The oracle models it by
 * killing only the game descriptor (s_workers_dead stays 0).  Hits then
 * continue past one window: the dead game descriptor surfaces only at the
 * next forwarded call, here a SEEK_END, which must answer the real
 * descriptor's ENODEV.  Every byte served after the death must still be the
 * file's byte at that position. */
static void scenario_suspend_late_open(void)
{
    int fd = open(s_raw_path, O_RDONLY);
    int live = open(s_raw_path, O_RDONLY);
    struct _reent r = { 0 };
    uint8_t *buf = (uint8_t *)malloc(CRT_BUFFER);
    uint8_t *ref = (uint8_t *)malloc(CRT_BUFFER);
    uint32_t logical = s_raw[0].off;
    uint32_t after_death = 0U;
    unsigned busy_before;
    unsigned i;
    isaac_ogg_ra_ssize_t got;
    isaac_vita_ogg_ra_stats s;

    lseek(fd, (off_t)logical, SEEK_SET);
    note_and_wait(fd, s_raw_path);
    s_dead[fd] = 1U;
    isaac_vita_ogg_ra_snapshot(&s);
    busy_before = s.lock_busy;
    for (i = 0U; i < 4U * (WINDOW / CRT_BUFFER) + 4U; ++i) {
        /* the worker refills behind the reads; give it time so that every
         * read is a hit and the death stays hidden */
        unsigned spin;
        for (spin = 0U; spin < 400U; ++spin) {
            isaac_vita_ogg_ra_snapshot(&s);
            if (s.fills * ISAAC_OGG_RA_CHUNK_BYTES >= after_death + 2U * CRT_BUFFER)
                break;
            usleep(500U);
        }
        r._errno = SENTINEL;
        got = __wrap__read_r(&r, fd, buf, CRT_BUFFER);
        if (got <= 0)
            break;
        CHECK(pread(live, ref, (size_t)got, logical) == got &&
              memcmp(buf, ref, (size_t)got) == 0,
              "suspend-late-open: post-death bytes differ at %u", logical);
        logical += (uint32_t)got;
        after_death += (uint32_t)got;
    }
    isaac_vita_ogg_ra_snapshot(&s);
    /* A read that met a held lock (the spin1 geometry, by chance) is a
     * forwarded call: it surfaces the death early, which is also exact. */
    CHECK(after_death > WINDOW || s.lock_busy > busy_before,
          "suspend-late-open: only %u bytes served after death (window %u): "
          "the late-open case was not reached", after_death,
          (unsigned)WINDOW);
    r._errno = SENTINEL;
    CHECK(__wrap__lseek_r(&r, fd, 0, SEEK_END) == -1 && r._errno == ENODEV,
          "suspend-late-open: SEEK_END must answer the dead descriptor's "
          "ENODEV (errno %d)", r._errno);
    printf("  suspend-late-open: %u bytes served from chunks after the "
           "descriptor died (window %u)\n", after_death, (unsigned)WINDOW);
    isaac_vita_ogg_ra_snapshot(&s);
    CHECK(s.sync_fail >= 1U && s.real_fail == 0U && s.fill_err == 0U,
          "suspend-late-open: sync_fail=%u real_fail=%u fill_err=%u",
          s.sync_fail, s.real_fail, s.fill_err);
    (void)__wrap__close_r(&r, fd);
    close(live);
    free(buf);
    free(ref);
    scenario_end("suspend-late-open");
}

/* Lock busy: the game's bounded trylock gives up because the worker holds
 * the entry lock.  The read must then be the real call at newlib's
 * position, i.e. a sync SEEK_SET (hits left the kernel position stale) and
 * a real read, exact in bytes, count and errno.  The oracle holds the lock
 * itself, so the branch runs by construction in every geometry (a contended
 * run reaches it only by chance: 481 busy misses in one spin1 run, 0 in the
 * next). */
static void scenario_lock_busy(void)
{
    pair p;
    isaac_vita_ogg_ra_stats s0;
    isaac_vita_ogg_ra_stats s1;
    unsigned held = 0U;
    unsigned i;

    CHECK(pair_open(&p, s_raw_path, 1), "lock-busy open");
    for (i = 0U; i < 2000U; ++i) {
        isaac_vita_ogg_ra_snapshot(&s0);
        if (s0.fills >= 2U)
            break;
        usleep(1000U);
    }
    CHECK(s0.fills >= 2U, "lock-busy: worker filled %u chunks", s0.fills);
    sequential(&p, 2U, 0U, "lock-busy-pre");
    isaac_vita_ogg_ra_snapshot(&s0);
    CHECK(s0.hits >= 1U, "lock-busy: no hit before the held lock (hits=%u)",
          s0.hits);
    CHECK(isaac_vita_ogg_ra_oracle_hold_lock(p.wfd, 1),
          "lock-busy: descriptor not registered");
    pair_read(&p, CRT_BUFFER, "lock-busy");
    ++held;
    pair_seek(&p, 4096, SEEK_CUR, "lock-busy");
    pair_read(&p, 5000U, "lock-busy");
    ++held;
    pair_read(&p, CRT_BUFFER, "lock-busy");
    ++held;
    (void)isaac_vita_ogg_ra_oracle_hold_lock(p.wfd, 0);
    isaac_vita_ogg_ra_snapshot(&s1);
    CHECK(s1.lock_busy - s0.lock_busy == held && s1.hits == s0.hits &&
          s1.misses - s0.misses == held && s1.syncs > s0.syncs,
          "lock-busy: busy %u->%u hits %u->%u misses %u->%u syncs %u->%u "
          "(held reads %u)", s0.lock_busy, s1.lock_busy, s0.hits, s1.hits,
          s0.misses, s1.misses, s0.syncs, s1.syncs, held);
    sequential(&p, 8U, 2000U, "lock-busy-post");
    pair_close(&p, "lock-busy");
    scenario_end("lock-busy");
}

/* fd reuse: a retired number handed to an unregistered file passes through. */
static void scenario_fd_reuse(void)
{
    pair p;
    pair q;
    int number;

    CHECK(pair_open(&p, s_raw_path, 1), "fd-reuse open");
    sequential(&p, 10U, 1000U, "fd-reuse");
    number = p.wfd;
    pair_close(&p, "fd-reuse");
    CHECK(pair_open(&q, s_blk_path, 0), "fd-reuse reopen");
    CHECK(q.wfd == number || q.rfd == number,
          "fd-reuse: number %d not reused (%d/%d)", number, q.wfd, q.rfd);
    sequential(&q, 10U, 0U, "fd-reuse-other");
    pair_seek(&q, 77777, SEEK_SET, "fd-reuse-other");
    sequential(&q, 3U, 0U, "fd-reuse-other");
    pair_close(&q, "fd-reuse-other");
    scenario_end("fd-reuse");
}

typedef struct foreign_job {
    int fd;
    const char *path;
    unsigned reads;
    int ok;
} foreign_job;

/* Another thread reading an unregistered descriptor through the wraps. */
static void *foreign_thread(void *arg)
{
    foreign_job *job = (foreign_job *)arg;
    int ref = open(job->path, O_RDONLY);
    uint8_t *a = (uint8_t *)malloc(CRT_BUFFER);
    uint8_t *b = (uint8_t *)malloc(CRT_BUFFER);
    struct _reent r = { 0 };
    unsigned i;

    job->ok = ref >= 0 && a && b;
    for (i = 0U; i < job->reads && job->ok; ++i) {
        isaac_ogg_ra_ssize_t x = __wrap__read_r(&r, job->fd, a, CRT_BUFFER);
        ssize_t y = read(ref, b, CRT_BUFFER);
        if (x != y || (x > 0 && memcmp(a, b, (size_t)x) != 0))
            job->ok = 0;
    }
    if (ref >= 0)
        close(ref);
    free(a);
    free(b);
    return NULL;
}

/* Another thread using the game's registered descriptor itself (the pair's
 * reference descriptor follows the same calls), while the owner waits. */
static void *foreign_pair_thread(void *arg)
{
    pair *p = (pair *)arg;

    pair_seek(p, 0, SEEK_CUR, "foreign-thread");
    pair_read(p, CRT_BUFFER, "foreign-thread");
    pair_read(p, 3000U, "foreign-thread");
    return NULL;
}

/* Foreign threads.  (1) The registered descriptor used from another thread
 * after the owner's hits left the kernel position stale: the module hands
 * the position back, the foreign calls are the real calls at newlib's
 * position, and the owner's later calls stay real and exact.  (2) An
 * unregistered descriptor read concurrently with the game's registered one
 * is never touched. */
static void scenario_foreign(void)
{
    pair p;
    foreign_job job;
    pthread_t t;
    int other = open(s_blk_path, O_RDONLY);
    isaac_vita_ogg_ra_stats s;

    CHECK(pair_open(&p, s_raw_path, 1), "foreign open");
    sequential(&p, 8U, 2000U, "foreign-owner");
    pair_seek(&p, 70000, SEEK_SET, "foreign-owner");
    sequential(&p, 2U, 0U, "foreign-owner");
    /* a virtual seek: the kernel position is now certainly stale */
    pair_seek(&p, 200000, SEEK_SET, "foreign-owner");
    pthread_create(&t, NULL, foreign_pair_thread, &p);
    pthread_join(t, NULL);
    sequential(&p, 6U, 1000U, "foreign-owner-after");
    pair_seek(&p, 1234, SEEK_SET, "foreign-owner-after");
    sequential(&p, 3U, 0U, "foreign-owner-after");
    isaac_vita_ogg_ra_snapshot(&s);
    CHECK(s.foreign == 3U && s.pass_reads >= 9U,
          "foreign: foreign=%u pass=%u", s.foreign, s.pass_reads);
    pair_close(&p, "foreign-registered");
    wait_idle();
    CHECK(pair_open(&p, s_raw_path, 1), "foreign open 2");
    job.fd = other;
    job.path = s_blk_path;
    job.reads = 200U;
    pthread_create(&t, NULL, foreign_thread, &job);
    sequential(&p, 60U, 300U, "foreign");
    pthread_join(t, NULL);
    CHECK(job.ok, "foreign: unregistered descriptor on another thread");
    pair_close(&p, "foreign");
    close(other);
    isaac_vita_ogg_ra_snapshot(&s);
    CHECK(s.foreign == 3U, "foreign counter %u", s.foreign);
    scenario_end("foreign");
}

/* Five registrations on four entries; refused registrations (path too long,
 * fstat failure, unregistered context) stay plain descriptors. */
static void scenario_table(void)
{
    pair p[5];
    char long_path[1100];
    pair q;
    unsigned i;

    for (i = 0U; i < 5U; ++i)
        CHECK(pair_open(&p[i], s_raw_path, 1), "table open %u", i);
    for (i = 0U; i < 5U; ++i)
        sequential(&p[i], 6U, 500U, "table");
    memset(long_path, 'a', sizeof long_path - 1U);
    long_path[sizeof long_path - 1U] = '\0';
    isaac_vita_ogg_ra_note(p[4].wfd, long_path);
    for (i = 0U; i < 5U; ++i)
        pair_close(&p[i], "table");
    wait_idle();
    CHECK(pair_open(&q, s_raw_path, 0), "table fstat open");
    s_fstat_fail_fd = q.wfd;
    isaac_vita_ogg_ra_note(q.wfd, s_raw_path);
    sequential(&q, 4U, 0U, "table-fstat");
    pair_close(&q, "table-fstat");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.reg_full >= 1U && s.reg_fail >= 1U && s.registered == 4U,
              "table: full=%u fail=%u reg=%u", s.reg_full, s.reg_fail,
              s.registered);
    }
    scenario_end("table");
}

/* Retire while a pread is in flight: the late chunk is dropped. */
static void scenario_retire_inflight(void)
{
    pair p;

    s_pread_delay_fixed_us = 60000U;
    CHECK(pair_open(&p, s_raw_path, 1), "retire open");
    /* the worker's first pread is in flight */
    wait_counter(&s_pread_calls, 1U);
    pair_read(&p, CRT_BUFFER, "retire");
    usleep(5000U);
    pair_close(&p, "retire");
    usleep(120000U);
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.fill_dropped >= 1U, "retire: dropped=%u", s.fill_dropped);
    }
    scenario_end("retire-inflight");
}

/* A slow card: the game waits for the worker's pending chunk, and with a
 * pread slower than ISAAC_OGG_RA_WAIT_US it gives up and reads itself.
 * With WAIT_US 0 it never waits: every PENDING chunk is a timeout. */
#define SLOW_BASE_US (ISAAC_OGG_RA_WAIT_US ? ISAAC_OGG_RA_WAIT_US : 20000U)
static void scenario_slow_worker(void)
{
    pair p;

    s_pread_delay_fixed_us = SLOW_BASE_US / 4U;
    CHECK(pair_open(&p, s_raw_path, 1), "slow open");
    /* the worker's first pread is in flight: the first read finds it
     * FILLING */
    wait_counter(&s_pread_calls, 1U);
    sequential(&p, 16U, 0U, "slow");
    s_pread_delay_fixed_us = SLOW_BASE_US * 2U;
    sequential(&p, 6U, 0U, "slow");
    s_pread_delay_fixed_us = 0U;
    pair_close(&p, "slow");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK((ISAAC_OGG_RA_WAIT_US == 0U ? s.waits == 0U : s.waits > 0U) &&
              s.wait_timeouts > 0U,
              "slow: waits=%u timeouts=%u", s.waits, s.wait_timeouts);
    }
    scenario_end("slow-worker");
}

/* Forget (CRT fclose into the idle cache): the kernel position goes back to
 * newlib's view, so plain reads continue exactly afterwards. */
static void scenario_forget(void)
{
    pair p;
    uint8_t a[4096];
    uint8_t b[4096];
    ssize_t x;
    ssize_t y;

    CHECK(pair_open(&p, s_raw_path, 1), "forget open");
    sequential(&p, 10U, 2000U, "forget");
    pair_seek(&p, 300000, SEEK_SET, "forget");
    isaac_vita_ogg_ra_forget(p.wfd);
    x = read(p.wfd, a, sizeof a);
    y = read(p.rfd, b, sizeof b);
    CHECK(x == y && x > 0 && memcmp(a, b, (size_t)x) == 0,
          "forget: kernel position not handed back");
    pair_close(&p, "forget");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.forget_sync == 1U, "forget: fsync=%u", s.forget_sync);
    }
    scenario_end("forget");
}

/* A forwarded read that fails on a live registration (EFAULT: a request
 * larger than the window into an invalid buffer, refused by the kernel on
 * both descriptors alike) turns the entry passthrough; every later call is
 * then the real descriptor's own answer. */
static void scenario_real_fail(void)
{
    pair p;
    isaac_ogg_ra_ssize_t a;
    isaac_ogg_ra_ssize_t b;

    CHECK(pair_open(&p, s_raw_path, 1), "real-fail open");
    sequential(&p, 6U, 2000U, "real-fail");
    pair_seek(&p, 500000, SEEK_SET, "real-fail");
    p.wr._errno = SENTINEL;
    p.rr._errno = SENTINEL;
    a = __wrap__read_r(&p.wr, p.wfd, NULL, WINDOW + 1U);
    b = __real__read_r(&p.rr, p.rfd, NULL, WINDOW + 1U);
    CHECK(a == b && a < 0 && p.wr._errno == p.rr._errno,
          "real-fail: %d/%d errno %d/%d", (int)a, (int)b, p.wr._errno,
          p.rr._errno);
    sequential(&p, 6U, 2000U, "real-fail-pass");
    pair_seek(&p, 0, SEEK_CUR, "real-fail-pass");
    pair_seek(&p, 1000, SEEK_SET, "real-fail-pass");
    sequential(&p, 3U, 0U, "real-fail-pass");
    pair_close(&p, "real-fail");
    {
        isaac_vita_ogg_ra_stats s;
        isaac_vita_ogg_ra_snapshot(&s);
        CHECK(s.real_fail == 1U && s.pass_reads >= 9U,
              "real-fail: real_fail=%u pass=%u", s.real_fail, s.pass_reads);
    }
    scenario_end("real-fail");
}

/* ------------------------------------------------------------------------
 * stdio streams over the wraps (fopencookie, 16 KiB setvbuf). */
typedef struct cookie {
    int fd;
    int wrapped;
    struct _reent r;
} cookie;

static ssize_t ck_read(void *c, char *buf, size_t n)
{
    cookie *k = (cookie *)c;
    isaac_ogg_ra_ssize_t got = k->wrapped
        ? __wrap__read_r(&k->r, k->fd, buf, n)
        : __real__read_r(&k->r, k->fd, buf, n);
    if (got < 0) {
        errno = k->r._errno;
        return -1;
    }
    return got;
}

static int ck_seek(void *c, off64_t *off, int whence)
{
    cookie *k = (cookie *)c;
    isaac_ogg_ra_off_t got;

    if (*off > INT32_MAX || *off < INT32_MIN) {
        errno = EOVERFLOW;
        return -1;
    }
    got = k->wrapped
        ? __wrap__lseek_r(&k->r, k->fd, (isaac_ogg_ra_off_t)*off, whence)
        : __real__lseek_r(&k->r, k->fd, (isaac_ogg_ra_off_t)*off, whence);
    if (got < 0) {
        errno = k->r._errno;
        return -1;
    }
    *off = got;
    return 0;
}

static int ck_close(void *c)
{
    cookie *k = (cookie *)c;
    int rc = k->wrapped ? __wrap__close_r(&k->r, k->fd)
                        : __real__close_r(&k->r, k->fd);
    free(k);
    return rc;
}

typedef struct stream {
    FILE *f;
    int fd;
    int wrapped;
    char *vbuf;
} stream;

static int stream_open(stream *s, const char *path, int wrapped)
{
    cookie_io_functions_t io = { ck_read, NULL, ck_seek, ck_close };
    cookie *k = (cookie *)calloc(1, sizeof *k);

    memset(s, 0, sizeof *s);
    if (!k)
        return 0;
    k->fd = open(path, O_RDONLY);
    k->wrapped = wrapped;
    s->fd = k->fd;
    s->wrapped = wrapped;
    s->f = fopencookie(k, "rb", io);
    s->vbuf = (char *)malloc(CRT_BUFFER);
    if (!s->f || !s->vbuf || k->fd < 0)
        return 0;
    setvbuf(s->f, s->vbuf, _IOFBF, CRT_BUFFER);
    return 1;
}

static void stream_close(stream *s)
{
    if (s->wrapped)
        isaac_vita_ogg_ra_forget(s->fd);
    fclose(s->f);
    free(s->vbuf);
}

/* The CRT seam: vita_crt_fread registers the descriptor of a read-only
 * token when the native Vorbis context is set, then reads. */
static stream *s_crt_stream;
static const char *s_crt_path;

static void crt_note(FILE *f)
{
    if (isaac_vita_ogg_ra_context && s_crt_stream && f == s_crt_stream->f &&
        s_crt_stream->wrapped)
        isaac_vita_ogg_ra_note(s_crt_stream->fd, s_crt_path);
}

static int crt_getc(FILE *f) { crt_note(f); return fgetc(f); }
static size_t crt_fread(void *b, size_t s, size_t n, FILE *f)
{
    crt_note(f);
    return fread(b, s, n, f);
}
static int crt_fseek(FILE *f, long o, int w) { return fseek(f, o, w); }
static long crt_ftell(FILE *f) { return ftell(f); }

#define fgetc(f) crt_getc(f)
#define fread(b, s, n, f) crt_fread(b, s, n, f)
#define fseek(f, o, w) crt_fseek(f, o, w)
#define ftell(f) crt_ftell(f)

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wempty-body"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "third_party/stb_vorbis/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#undef fgetc
#undef fread
#undef fseek
#undef ftell

typedef struct pcm {
    uint8_t *data;
    size_t len;
    size_t cap;
} pcm;

static void pcm_add(pcm *p, const void *src, size_t n)
{
    if (p->len + n > p->cap) {
        size_t cap = p->cap ? p->cap * 2U : 1U << 20;
        while (cap < p->len + n)
            cap *= 2U;
        p->data = (uint8_t *)realloc(p->data, cap);
        p->cap = cap;
    }
    memcpy(p->data + p->len, src, n);
    p->len += n;
}

static uint32_t crc32_of(const uint8_t *d, size_t n)
{
    uint32_t c = 0xffffffffU;
    size_t i;
    int k;

    for (i = 0U; i < n; ++i) {
        c ^= d[i];
        for (k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

/* Decode entry `i` of the raw archive in FILE mode, with seeks. */
static int decode_file_mode(unsigned i, int wrapped, pcm *out,
                            unsigned *seeks_done)
{
    stream s;
    stb_vorbis *v;
    int err = 0;
    short chunk[4096];
    int got;
    unsigned seeks = 0U;
    unsigned total;
    int channels;

    if (!stream_open(&s, s_raw_path, wrapped))
        return 0;
    s_crt_stream = &s;
    s_crt_path = s_raw_path;
    isaac_vita_ogg_ra_context = 1;
    fseek(s.f, (long)s_raw[i].off, SEEK_SET);
    v = stb_vorbis_open_file_section(s.f, 0, &err, NULL, s_raw[i].len);
    if (!v) {
        isaac_vita_ogg_ra_context = 0;
        stream_close(&s);
        return 0;
    }
    channels = v->channels;
    total = stb_vorbis_stream_length_in_samples(v);
    s_rng = 0x51ed2701ULL + i;
    for (;;) {
        got = stb_vorbis_get_samples_short_interleaved(v, channels, chunk,
                                                       4096 / channels * channels);
        if (got <= 0)
            break;
        pcm_add(out, chunk, (size_t)got * (size_t)channels * sizeof(short));
        if (seeks < 6U && rng_below(40U) == 0U && total > 1000U) {
            unsigned target = rng_below(total - 1U);
            int ok = stb_vorbis_seek(v, target);
            uint32_t tag[2] = { target, (uint32_t)ok };
            pcm_add(out, tag, sizeof tag);
            ++seeks;
        }
        usleep(rng_below(400U));
    }
    stb_vorbis_close(v);
    isaac_vita_ogg_ra_context = 0;
    stream_close(&s);
    s_crt_stream = NULL;
    *seeks_done = seeks;
    return 1;
}

static void scenario_stdio(void)
{
    unsigned i;

    for (i = 0U; i < s_ogg_count; ++i) {
        pcm a = { 0 };
        pcm b = { 0 };
        unsigned sa = 0U;
        unsigned sb = 0U;
        short *mem = NULL;
        int ch = 0;
        int rate = 0;
        int frames;

        CHECK(decode_file_mode(i, 1, &a, &sa), "stdio %u: wrapped decode", i);
        CHECK(decode_file_mode(i, 0, &b, &sb), "stdio %u: reference decode", i);
        CHECK(a.len == b.len && sa == sb &&
              (a.len == 0U || memcmp(a.data, b.data, a.len) == 0),
              "stdio %u: PCM differs (%zu vs %zu bytes)", i, a.len, b.len);
        frames = stb_vorbis_decode_memory(s_raw[i].ogg, (int)s_raw[i].len, &ch,
                                          &rate, &mem);
        printf("  stdio ogg %u: %zu PCM+tag bytes, %u seeks, crc %08x/%08x, "
               "memory decode %d frames x %d ch\n", i, a.len, sa,
               crc32_of(a.data, a.len), crc32_of(b.data, b.len), frames, ch);
        free(mem);
        free(a.data);
        free(b.data);
    }
    scenario_end("stdio");
}

/* Plain sequential FILE-mode decode, no seeks: equals the memory decode. */
static void scenario_stdio_plain(void)
{
    unsigned i;

    for (i = 0U; i < s_ogg_count; ++i) {
        stream s;
        stb_vorbis *v;
        int err = 0;
        short *mem = NULL;
        int ch = 0;
        int rate = 0;
        int frames = stb_vorbis_decode_memory(s_raw[i].ogg, (int)s_raw[i].len,
                                              &ch, &rate, &mem);
        pcm a = { 0 };
        short chunk[4096];
        int got;

        CHECK(stream_open(&s, s_raw_path, 1), "plain open");
        s_crt_stream = &s;
        s_crt_path = s_raw_path;
        isaac_vita_ogg_ra_context = 1;
        fseek(s.f, (long)s_raw[i].off, SEEK_SET);
        v = stb_vorbis_open_file_section(s.f, 0, &err, NULL, s_raw[i].len);
        CHECK(v != NULL, "plain %u open err %d", i, err);
        while (v && (got = stb_vorbis_get_samples_short_interleaved(
                         v, ch, chunk, 4096 / ch * ch)) > 0)
            pcm_add(&a, chunk, (size_t)got * (size_t)ch * sizeof(short));
        if (v)
            stb_vorbis_close(v);
        isaac_vita_ogg_ra_context = 0;
        stream_close(&s);
        s_crt_stream = NULL;
        CHECK(frames > 0 && a.len == (size_t)frames * (size_t)ch * 2U &&
              memcmp(a.data, mem, a.len) == 0,
              "plain %u: FILE-mode PCM over the read-ahead differs from the "
              "memory decode (%zu vs %d frames)", i, a.len, frames);
        free(mem);
        free(a.data);
    }
    scenario_end("stdio-plain");
}

/* ------------------------------------------------------------------------
 * ArchivedFile-shaped block reader + nv_topup-shaped consumer. */
typedef struct kfile {
    stream s;
    uint32_t start;     /* first block header */
    uint32_t logical;   /* decoded position */
    uint8_t block[0x800];
    uint32_t have;
    uint32_t used;
    int eof;
} kfile;

static int k_next_block(kfile *k)
{
    uint8_t h[4];
    uint32_t n;

    if (crt_fread(h, 1, 4, k->s.f) != 4U)
        return 0;
    n = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) |
        ((uint32_t)h[3] << 24);
    if (n == 0U) {
        k->eof = 1;
        return 0;
    }
    if (n > 0x800U || crt_fread(k->block, 1, n, k->s.f) != n)
        return 0;
    k->have = n;
    k->used = 0U;
    return 1;
}

static uint32_t k_read(kfile *k, uint8_t *dst, uint32_t n)
{
    uint32_t done = 0U;

    while (done < n) {
        uint32_t take;
        if (k->used == k->have && (k->eof || !k_next_block(k)))
            break;
        take = k->have - k->used;
        if (take > n - done)
            take = n - done;
        memcpy(dst + done, k->block + k->used, take);
        k->used += take;
        done += take;
    }
    k->logical += done;
    return done;
}

/* ArchivedFile::Seek: back to the entry start and decode forward. */
static int k_seek(kfile *k, uint32_t pos)
{
    uint8_t sink[4096];

    if (fseek(k->s.f, (long)k->start, SEEK_SET) != 0)
        return 0;
    k->logical = 0U;
    k->have = k->used = 0U;
    k->eof = 0;
    while (k->logical < pos) {
        uint32_t step = pos - k->logical;
        if (step > sizeof sink)
            step = sizeof sink;
        if (k_read(k, sink, step) != step)
            return 0;
    }
    return 1;
}

static int topup_stream(unsigned i, int wrapped, pcm *ogg_out)
{
    kfile k;
    uint8_t chunk[24576];
    uint32_t got;
    unsigned loop;

    memset(&k, 0, sizeof k);
    if (!stream_open(&k.s, s_blk_path, wrapped))
        return 0;
    s_crt_stream = &k.s;
    s_crt_path = s_blk_path;
    k.start = s_blk[i].off;
    /* The guest's own header parse happens outside the native context. */
    isaac_vita_ogg_ra_context = 0;
    if (!k_seek(&k, 0U))
        return 0;
    got = k_read(&k, chunk, 4096U);
    pcm_add(ogg_out, chunk, got);
    s_rng = 0x70b0ULL + i;
    for (loop = 0U; loop < 2U; ++loop) {
        unsigned slot = 0U;
        for (;;) {
            isaac_vita_ogg_ra_context = 1;
            got = k_read(&k, chunk, sizeof chunk);
            isaac_vita_ogg_ra_context = 0;
            pcm_add(ogg_out, chunk, got);
            if (got < sizeof chunk)
                break;
            if (++slot == 5U) {
                /* a hand-off seek back to the current position */
                isaac_vita_ogg_ra_context = 1;
                CHECK(k_seek(&k, k.logical), "blocks: mid seek");
                isaac_vita_ogg_ra_context = 0;
            }
            usleep(rng_below(3000U));
        }
        isaac_vita_ogg_ra_context = 1;
        CHECK(k_seek(&k, 0U), "blocks: loop rewind");
        isaac_vita_ogg_ra_context = 0;
    }
    stream_close(&k.s);
    s_crt_stream = NULL;
    return 1;
}

static void scenario_blocks(void)
{
    unsigned i;

    for (i = 0U; i < s_ogg_count; ++i) {
        pcm a = { 0 };
        pcm b = { 0 };
        short *pa = NULL;
        short *pb = NULL;
        int ch = 0;
        int rate = 0;
        int fa;
        int fb;
        size_t one = 4096U < s_blk[i].len ? 4096U : s_blk[i].len;

        CHECK(topup_stream(i, 1, &a), "blocks %u: wrapped", i);
        CHECK(topup_stream(i, 0, &b), "blocks %u: reference", i);
        CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0,
              "blocks %u: OGG byte streams differ", i);
        CHECK(a.len == one + 2U * s_blk[i].len - one &&
              memcmp(a.data + one, s_blk[i].ogg + one, s_blk[i].len - one) == 0,
              "blocks %u: stream is not the entry (%zu bytes)", i, a.len);
        fa = stb_vorbis_decode_memory(a.data, (int)s_blk[i].len, &ch, &rate,
                                      &pa);
        fb = stb_vorbis_decode_memory(b.data, (int)s_blk[i].len, &ch, &rate,
                                      &pb);
        CHECK(fa > 0 && fa == fb &&
              memcmp(pa, pb, (size_t)fa * (size_t)ch * 2U) == 0,
              "blocks %u: PCM differs", i);
        printf("  blocks ogg %u: %zu stream bytes (2 loops), %d frames, "
               "pcm crc %08x\n", i, a.len, fa,
               fa > 0 ? crc32_of((const uint8_t *)pa,
                                 (size_t)fa * (size_t)ch * 2U) : 0U);
        free(pa);
        free(pb);
        free(a.data);
        free(b.data);
    }
    scenario_end("blocks");
}

/* ------------------------------------------------------------------------ */
/* Every row must be reached in the production geometry; non-default
 * geometries name the rows they cannot reach.  lock_busy is reached by
 * construction (scenario_lock_busy holds the entry lock). */
static int census_exempt(const char *name)
{
    const char *list = getenv("RA_CENSUS_EXEMPT");
    size_t n = strlen(name);

    while (list && *list) {
        const char *comma = strchr(list, ',');
        size_t len = comma ? (size_t)(comma - list) : strlen(list);
        if (len == n && strncmp(list, name, n) == 0)
            return 1;
        list = comma ? comma + 1 : NULL;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned seed;
    static const char *const census[] = {
        "reads", "hits", "hit_eof", "misses", "waits", "wait_timeouts",
        "lock_busy", "vseeks", "fseeks", "syncs", "sync_fail", "pass_reads",
        "foreign", "registered", "reg_full", "reg_fail", "retired",
        "forget_sync", "real_fail", "fills", "fill_err", "fill_short",
        "open_fail", "fill_dropped",
    };
    const unsigned *t = (const unsigned *)&s_total;
    static const size_t offsets[] = {
        offsetof(isaac_vita_ogg_ra_stats, reads),
        offsetof(isaac_vita_ogg_ra_stats, hits),
        offsetof(isaac_vita_ogg_ra_stats, hit_eof),
        offsetof(isaac_vita_ogg_ra_stats, misses),
        offsetof(isaac_vita_ogg_ra_stats, waits),
        offsetof(isaac_vita_ogg_ra_stats, wait_timeouts),
        offsetof(isaac_vita_ogg_ra_stats, lock_busy),
        offsetof(isaac_vita_ogg_ra_stats, vseeks),
        offsetof(isaac_vita_ogg_ra_stats, fseeks),
        offsetof(isaac_vita_ogg_ra_stats, syncs),
        offsetof(isaac_vita_ogg_ra_stats, sync_fail),
        offsetof(isaac_vita_ogg_ra_stats, pass_reads),
        offsetof(isaac_vita_ogg_ra_stats, foreign),
        offsetof(isaac_vita_ogg_ra_stats, registered),
        offsetof(isaac_vita_ogg_ra_stats, reg_full),
        offsetof(isaac_vita_ogg_ra_stats, reg_fail),
        offsetof(isaac_vita_ogg_ra_stats, retired),
        offsetof(isaac_vita_ogg_ra_stats, forget_sync),
        offsetof(isaac_vita_ogg_ra_stats, real_fail),
        offsetof(isaac_vita_ogg_ra_stats, fills),
        offsetof(isaac_vita_ogg_ra_stats, fill_err),
        offsetof(isaac_vita_ogg_ra_stats, fill_short),
        offsetof(isaac_vita_ogg_ra_stats, open_fail),
        offsetof(isaac_vita_ogg_ra_stats, fill_dropped),
    };
    unsigned trace_seeds = getenv("RA_TRACE_SEEDS")
        ? (unsigned)atoi(getenv("RA_TRACE_SEEDS")) : 12U;
    unsigned i;
    unsigned missing = 0U;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch-dir> [<file.ogg>...]\n", argv[0]);
        return 2;
    }
    if (!build_archives(argv[1], argv + 2, (unsigned)(argc - 2))) {
        fprintf(stderr, "cannot build test archives in %s\n", argv[1]);
        return 2;
    }
    printf("ogg readahead oracle: chunk=%u chunks=%u window=%u wait_us=%u "
           "raw=%u bytes, %u oggs\n", (unsigned)ISAAC_OGG_RA_CHUNK_BYTES,
           (unsigned)ISAAC_OGG_RA_CHUNKS, (unsigned)WINDOW,
           (unsigned)ISAAC_OGG_RA_WAIT_US, (unsigned)s_raw_size, s_ogg_count);

    for (seed = 1U; seed <= trace_seeds; ++seed)
        scenario_trace(seed, 400U, seed % 3U == 0U ? 3000U : 800U,
                       seed % 4U == 0U ? 500U : 0U);
    scenario_pread_error();
    scenario_pread_short();
    scenario_open_fail();
    scenario_suspend();
    scenario_suspend_late_open();
    scenario_lock_busy();
    scenario_fd_reuse();
    scenario_foreign();
    scenario_table();
    scenario_retire_inflight();
    scenario_slow_worker();
    scenario_forget();
    scenario_real_fail();
    scenario_stdio_plain();
    scenario_stdio();
    scenario_blocks();

    printf("ogg readahead oracle: branch census (sum over scenarios)\n");
    for (i = 0U; i < sizeof census / sizeof census[0]; ++i) {
        unsigned v = t[offsets[i] / sizeof(unsigned)];
        printf("  %-14s %u%s\n", census[i], v, v ? "" : "   <-- never taken");
        if (!v && !census_exempt(census[i]))
            ++missing;
    }
    printf("ogg readahead oracle: real reads %u, real seeks %u, preads %u, "
           "log lines %u (max %u bytes)\n", s_real_reads, s_real_seeks,
           s_pread_total, s_log_lines, s_log_max);
    CHECK(missing == 0U, "%u census branches never taken", missing);
    if (s_failures) {
        printf("ogg readahead oracle: FAIL (%d)\n", s_failures);
        return 1;
    }
    printf("ogg readahead oracle: PASS%s\n",
           s_ogg_count ? "" : " (stdio/blocks SKIPPED: no OGG files)");
    return 0;
}
