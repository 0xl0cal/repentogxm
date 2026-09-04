/* Native stb_vorbis mirror decoder for the frozen KAGE OGG path.
 * See host_vita_native_vorbis.h for the mechanism, thread and memory rules,
 * fallbacks and receipts.  This file is the only owner of the vendored
 * decoder: it is included below after the I/O seam so that the game's
 * FILE * -> KAGE stream edit is reproduced without touching upstream text. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "guest.h"
#include "host_vita_native_vorbis.h"

void isaac_vita_log(const char *format, ...);

#if defined(ISAAC_VITA_NATIVE_VORBIS_ORACLE) || !defined(__vita__)
#define NV_HOST_BUILD 1
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#else
#define NV_HOST_BUILD 0
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif

#ifndef ISAAC_VITA_NATIVE_VORBIS_BUILD_ID
#define ISAAC_VITA_NATIVE_VORBIS_BUILD_ID "native-vorbis:unstamped"
#endif

/* ISAAC_VITA_NATIVE_VORBIS_ASYNC: partial slots, producer-side receipt CRC,
 * early clone at stb_vorbis_open_file (header, ASYNC).  Every ASYNC block is
 * a strict addition: with the option off this unit is byte-identical. */
#if defined(ISAAC_VITA_NATIVE_VORBIS_ASYNC)
#define NV_ASYNC 1
#else
#define NV_ASYNC 0
#endif
#if NV_ASYNC && defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
#error "ISAAC_VITA_NATIVE_VORBIS_ASYNC serves partial slots; VERIFY compares per-call frame counts -- run VERIFY with ASYNC off"
#endif
#if NV_ASYNC && !NV_HOST_BUILD && !defined(ISAAC_VITA_NATIVE_VORBIS_WORKER)
#error "ISAAC_VITA_NATIVE_VORBIS_ASYNC requires ISAAC_VITA_NATIVE_VORBIS_WORKER"
#endif
#if defined(ISAAC_VITA_NATIVE_VORBIS_RECEIPT) || defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY) || NV_HOST_BUILD
#define NV_RECEIPT 1
#else
#define NV_RECEIPT 0
#endif

/* ------------------------------------------------------------------------
 * Decoder I/O seam.  The vendored v1.04 keeps its stdio calls; these macros
 * redirect them to the mirror-owned byte ring identified by the FILE * token
 * the runtime stores in state->f.  Memory-mode states (whole samples) never
 * reach these functions because USE_MEMORY(z) is true for them. */
struct nv_mirror;
static int nv_io_getc(FILE *token);
static size_t nv_io_fread(void *buffer, size_t size, size_t count, FILE *token);
static int nv_io_fseek(FILE *token, long offset, int whence);
static long nv_io_ftell(FILE *token);
static int nv_io_fclose(FILE *token);
static FILE *nv_io_fopen(const char *name, const char *mode);

#define fgetc(token) nv_io_getc(token)
#define fread(buffer, size, count, token) nv_io_fread(buffer, size, count, token)
#define fseek(token, offset, whence) nv_io_fseek(token, offset, whence)
#define ftell(token) nv_io_ftell(token)
#define fclose(token) nv_io_fclose(token)
#define fopen(name, mode) nv_io_fopen(name, mode)

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif
#define ISAAC_NV_ASSERT_HEADER "../../host_vita_native_vorbis_assert.h"

/* Allocation seam of the vendored decoder.  Every state this module decodes
 * owns a stb_vorbis_alloc buffer, so these are never reached on the Vita;
 * they fail closed (NULL) there and are counted.  The host oracle needs the
 * real allocators for its malloc-mode reference decode. */
#if NV_HOST_BUILD
#define ISAAC_NV_MALLOC(size) malloc(size)
#define ISAAC_NV_REALLOC(pointer, size) realloc(pointer, size)
#define ISAAC_NV_FREE(pointer) free(pointer)
#else
static uint32_t s_vendored_alloc_refused;
static void *nv_vendored_alloc_refuse(size_t size)
{
    (void)size;
    if (s_vendored_alloc_refused != UINT32_MAX)
        ++s_vendored_alloc_refused;
    return NULL;
}
static void nv_vendored_free_ignore(void *pointer)
{
    (void)pointer;
}
#define ISAAC_NV_MALLOC(size) nv_vendored_alloc_refuse(size)
#define ISAAC_NV_REALLOC(pointer, size) nv_vendored_alloc_refuse(size)
#define ISAAC_NV_FREE(pointer) nv_vendored_free_ignore(pointer)
#endif

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
#undef fclose
#undef fopen
/* v1.04 leaves its channel-position helper macros defined. */
#undef C
#undef L
#undef R
#undef BUFFER_SIZE

/* The frozen guest layout.  These hold on every ILP32 target with natural
 * alignment, which is what the x86 PE and the Vita ARM EABI share. */
#if UINTPTR_MAX == 0xffffffffU
_Static_assert(sizeof(stb_vorbis) == ISAAC_NV_STATE_BYTES,
               "stb_vorbis state must be the guest's 0x5f8 bytes");
_Static_assert(offsetof(stb_vorbis, f) == ISAAC_NV_STATE_STREAM_OFFSET,
               "stream pointer must sit where the guest's FILE * was");
_Static_assert(offsetof(stb_vorbis, setup_temp_memory_required) ==
                   ISAAC_NV_STATE_MARKER_OFFSET,
               "marker slot drifted");
_Static_assert(offsetof(stb_vorbis, stream) == 0x20U, "stream drifted");
_Static_assert(offsetof(stb_vorbis, push_mode) == 0x30U, "push_mode drifted");
_Static_assert(offsetof(stb_vorbis, alloc) == 0x60U, "alloc drifted");
_Static_assert(offsetof(stb_vorbis, setup_offset) == 0x68U, "setup_offset");
_Static_assert(offsetof(stb_vorbis, eof) == ISAAC_NV_STATE_EOF_OFFSET,
               "eof drifted");
_Static_assert(offsetof(stb_vorbis, codebooks) == 0x8cU, "codebooks drifted");
_Static_assert(offsetof(stb_vorbis, channel_buffers) == 0x330U,
               "channel_buffers drifted");
_Static_assert(offsetof(stb_vorbis, page_crc_tests) == 0x59cU,
               "page_crc_tests drifted");
_Static_assert(offsetof(stb_vorbis, channel_buffer_start) == 0x5f0U,
               "channel_buffer_start drifted");
_Static_assert(sizeof(Codebook) == 0x830U,
               "Codebook stride must match the guest's FAST_HUFFMAN_SHORT build");
_Static_assert(sizeof(codetype) == 4U, "codebooks must be float (v1.04 default)");
#endif

/* ------------------------------------------------------------------------ */

uint32_t isaac_nv_crc32(uint32_t crc, const void *data, uint32_t bytes)
{
    static uint32_t table[256];
    static int table_ready;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    if (!table_ready) {
        for (i = 0U; i < 256U; ++i) {
            uint32_t v = i;
            unsigned k;
            for (k = 0U; k < 8U; ++k)
                v = (v & 1U) ? (v >> 1) ^ 0xedb88320U : v >> 1;
            table[i] = v;
        }
        table_ready = 1;
    }
    crc = ~crc;
    for (i = 0U; i < bytes; ++i)
        crc = table[(crc ^ p[i]) & 0xffU] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------------
 * Platform seam. */
#if NV_HOST_BUILD
/* Oracle clock seam (game thread only): a frozen or stepping clock makes
 * the ASYNC budget and wait arithmetic deterministic. */
static uint32_t s_oracle_fake_clock_on;
static uint64_t s_oracle_fake_now;
static uint32_t s_oracle_fake_step_us;
#endif

static uint64_t nv_now_us(void)
{
#if NV_HOST_BUILD
    struct timespec ts;
    if (s_oracle_fake_clock_on) {
        s_oracle_fake_now += s_oracle_fake_step_us;
        return s_oracle_fake_now;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    return (uint64_t)sceKernelGetProcessTimeWide();
#endif
}

static void nv_delay_us(uint32_t us)
{
#if NV_HOST_BUILD
    usleep(us);
#else
    sceKernelDelayThread(us);
#endif
}

#define nv_load_acq(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define nv_store_rel(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* All mirror memory is allocated on the game thread, as SceKernel user
 * memory blocks like the async save writer's arena: it is host-owned, never
 * touches the tracked guest heap, and is 32-bit addressable so the OGG ring
 * can be handed to the guest stream's read.  The host oracle supplies
 * 32-bit-addressable memory itself. */
#if NV_HOST_BUILD
#define nv_malloc(bytes) isaac_nv_oracle_malloc(bytes)
#define nv_free(pointer) isaac_nv_oracle_free(pointer)
#else
#define NV_MEMBLOCKS_MAX 128U
typedef struct nv_memblock {
    void *base;
    SceUID uid;
} nv_memblock;
static nv_memblock s_memblocks[NV_MEMBLOCKS_MAX];

static void *nv_malloc(size_t bytes)
{
    SceSize size = (SceSize)((bytes + 4095U) & ~(size_t)4095U);
    SceUID uid;
    void *base = NULL;
    uint32_t i;

    for (i = 0U; i < NV_MEMBLOCKS_MAX; ++i)
        if (!s_memblocks[i].base)
            break;
    if (i == NV_MEMBLOCKS_MAX)
        return NULL;
    uid = sceKernelAllocMemBlock("isaac_nv_vorbis",
                                 SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (uid < 0)
        return NULL;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        return NULL;
    }
    s_memblocks[i].base = base;
    s_memblocks[i].uid = uid;
    return base;
}

static void nv_free(void *pointer)
{
    uint32_t i;

    if (!pointer)
        return;
    for (i = 0U; i < NV_MEMBLOCKS_MAX; ++i)
        if (s_memblocks[i].base == pointer) {
            (void)sceKernelFreeMemBlock(s_memblocks[i].uid);
            s_memblocks[i].base = NULL;
            return;
        }
}
#endif

/* ------------------------------------------------------------------------
 * Rings: single producer, single consumer, power-of-two capacity,
 * free-running counters. */
typedef struct nv_ring {
    uint8_t *data;
    uint32_t capacity;      /* power of two */
    uint32_t head;          /* producer */
    uint32_t tail;          /* consumer */
} nv_ring;

static uint32_t nv_ring_avail(const nv_ring *r)
{
    return nv_load_acq(&r->head) - nv_load_acq(&r->tail);
}

static uint32_t nv_ring_free(const nv_ring *r)
{
    return r->capacity - nv_ring_avail(r);
}

/* Producer: copy `bytes` (<= free) in and publish. */
static void nv_ring_write(nv_ring *r, const uint8_t *src, uint32_t bytes)
{
    uint32_t head = r->head;
    uint32_t mask = r->capacity - 1U;
    uint32_t first = r->capacity - (head & mask);

    if (first > bytes)
        first = bytes;
    memcpy(r->data + (head & mask), src, first);
    if (bytes > first)
        memcpy(r->data, src + first, bytes - first);
    nv_store_rel(&r->head, head + bytes);
}

/* Consumer: copy `bytes` (<= avail) out and retire. */
static void nv_ring_read(nv_ring *r, uint8_t *dst, uint32_t bytes)
{
    uint32_t tail = r->tail;
    uint32_t mask = r->capacity - 1U;
    uint32_t first = r->capacity - (tail & mask);

    if (first > bytes)
        first = bytes;
    memcpy(dst, r->data + (tail & mask), first);
    if (bytes > first)
        memcpy(dst + first, r->data, bytes - first);
    nv_store_rel(&r->tail, tail + bytes);
}

static void nv_ring_reset(nv_ring *r)
{
    nv_store_rel(&r->tail, 0U);
    nv_store_rel(&r->head, 0U);
}

/* ------------------------------------------------------------------------
 * Mirrors. */
enum {
    NV_MODE_NONE = 0,
    NV_MODE_STREAM = 1,     /* KAGE stream I/O: rings + worker */
    NV_MODE_MEMORY = 2      /* whole sample in guest memory: sync only */
};

enum {
    NV_OWNER_NONE = 0,
    NV_OWNER_GAME = 1,
    NV_OWNER_WORKER = 2
};

typedef struct nv_mirror {
    /* identity, game thread */
    uint32_t guest_f;
    uint32_t guest_stream;
    uint32_t guest_alloc_buffer;
    uint32_t guest_alloc_len;
    uint32_t snap_serial;
    uint32_t snap_codebooks;
    uint32_t snap_sample_rate;
    uint32_t snap_channels;
    uint32_t snap_first_page;
    uint32_t snap_stream_len;
    uint32_t marker;
    uint32_t id;
    int mode;
    int channels_arg;
    uint32_t frame_bytes;       /* channels_arg * 2 */
    uint32_t frame_max_bytes;   /* blocksize_1 * frame_bytes */
    uint32_t dirty;
    uint32_t phys_valid;        /* KAGE stream position known */
    uint32_t phys_pos;
    uint32_t guest_logical_pos; /* VERIFY: where the guest state reads next */
    uint32_t ogg_head_pos;      /* stream position of the next byte to ring */
    uint32_t ogg_tail_pos;      /* stream position of the next byte to decode */
    uint64_t last_call_us;
    uint32_t poisoned;
    /* Bumped (RELEASE) by the game thread every time the slot changes hands
     * ((re)start, retire, re-open).  The worker samples it before taking
     * busy and refuses the slot when it changed underneath, so a slot that
     * was retired and re-purposed as a whole-sample mirror between the
     * worker's pre-check and its acquire is never decoded by the worker. */
    volatile uint32_t generation;
    volatile uint32_t game_waiting; /* game thread needs this mirror now */
    uint32_t io_us;             /* guest stream read time, this call */
    uint32_t leftover_frames;   /* emitted from the cloned channel buffers */
#if NV_ASYNC
    /* game thread: a wrapped call gave up waiting for the worker's frame
     * (ISAAC_NV_ACQUIRE_TIMEOUT_US).  The call then hands the state to the
     * translated decoder instead of reporting an end of stream, because an
     * OGG ring that already holds the file's tail (ogg_eof) says nothing
     * about the PCM the stalled worker still owes. */
    uint32_t timed_out;
#endif

    /* decoder state, exclusive to the busy holder */
    stb_vorbis state;
    uint8_t *clone;
    uint32_t clone_capacity;
    uint32_t clone_len;
    uint8_t *scratch;
    uint32_t scratch_capacity;
    jmp_buf assert_jmp;
    uint32_t assert_armed;
    uint32_t underrun;
    uint32_t broken;
    uint32_t assert_line;
    const char *assert_expr;
    uint32_t frames_worker;     /* written by the worker only */
    uint32_t frames_sync;       /* written by the game thread only */
    uint32_t frames_worker_seen;
    uint32_t frames_sync_seen;

    /* cross-thread */
    volatile uint32_t busy;
    volatile uint32_t active;
    volatile uint32_t retiring;
    volatile uint32_t pcm_eof;
    volatile uint32_t pcm_error;
    volatile uint32_t ogg_eof;
    nv_ring ogg;
    nv_ring pcm;

    /* receipts, game thread (ASYNC: crc_run is folded in by the decoder
     * that writes the PCM ring, i.e. by the busy holder) */
    uint32_t slots;
    uint32_t frames_out;
    uint32_t crc_run;
    uint32_t waits;
    uint32_t wait_us_max;
    uint32_t asserts;
    uint32_t underruns;
    uint32_t reclones;
    uint32_t verify_slots;
    uint32_t verify_diff_slots;
    uint32_t verify_diff_samples;
    uint32_t verify_maxabs;
    uint32_t verify_lines;
} nv_mirror;

static nv_mirror s_mirrors[ISAAC_NV_MIRRORS_MAX];

typedef struct nv_stats {
    uint32_t mirrors_created;
    uint32_t clone_rejected;
    uint32_t fallbacks;
    uint32_t slots;
    uint32_t frames;
    uint32_t frames_worker;
    uint32_t frames_sync;
    uint32_t waits;
    uint32_t wait_us_max;
    uint32_t reclones;
    uint32_t evictions;
    uint32_t asserts;
    uint32_t underruns;
    uint32_t pool_high_water;
    uint32_t verify_diff_slots;
    uint32_t io_us_total;
    uint32_t io_us_max;
    uint32_t freed_retires;
    uint32_t zero_fallbacks;
    uint32_t next_id;
    uint32_t init_logged;
#if NV_ASYNC
    uint32_t partial_slots;     /* got < req without an end of stream */
    uint32_t budget_hits;       /* sync path stopped by SYNC_BUDGET_US */
    uint32_t early_clones;      /* mirrors cloned at stb_vorbis_open_file */
    uint32_t early_rejected;
#endif
} nv_stats;

static nv_stats s_stats;

/* Worker. */
static volatile uint32_t s_worker_running;
static volatile uint32_t s_worker_stop;
static volatile uint32_t s_worker_refused;   /* slot changed under the worker */
#if NV_HOST_BUILD
static volatile uint32_t s_oracle_worker_frame_delay_us;
#endif
#if NV_HOST_BUILD
static pthread_t s_worker_thread;
static pthread_mutex_t s_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake_cond = PTHREAD_COND_INITIALIZER;
static unsigned s_wake_pending;
static int s_worker_thread_valid;
#else
static SceUID s_worker_thread = -1;
static SceUID s_worker_flag = -1;
#endif
static uint32_t s_worker_start_attempted;
static uint32_t s_worker_start_result;

static int nv_is_worker_thread(void)
{
#if NV_HOST_BUILD
    return s_worker_thread_valid &&
           pthread_equal(pthread_self(), s_worker_thread);
#else
    return s_worker_thread >= 0 && sceKernelGetThreadId() == s_worker_thread;
#endif
}

/* The mirror each thread is currently decoding, for the assertion hook. */
static nv_mirror *s_current_mirror[2];

static nv_mirror *nv_token_mirror(FILE *token)
{
    uintptr_t p = (uintptr_t)token;
    uintptr_t base = (uintptr_t)s_mirrors;

    if (p < base || p - base >= sizeof s_mirrors)
        return NULL;
    if ((p - base) % sizeof(nv_mirror) != 0U)
        return NULL;
    return (nv_mirror *)p;
}

/* ------------------------------------------------------------------------
 * Decoder-side I/O: consumes the OGG ring.  Only the busy holder runs these,
 * on whichever thread that is. */
static int nv_io_getc(FILE *token)
{
    nv_mirror *m = nv_token_mirror(token);
    uint8_t byte;

    if (!m)
        return EOF;
    if (nv_ring_avail(&m->ogg) == 0U) {
        if (!nv_load_acq(&m->ogg_eof))
            m->underrun = 1U;
        return EOF;
    }
    nv_ring_read(&m->ogg, &byte, 1U);
    m->ogg_tail_pos += 1U;
    return (int)byte;
}

static size_t nv_io_fread(void *buffer, size_t size, size_t count, FILE *token)
{
    nv_mirror *m = nv_token_mirror(token);
    uint32_t bytes = (uint32_t)(size * count);

    if (!m || bytes == 0U)
        return 0U;
    if (nv_ring_avail(&m->ogg) < bytes) {
        if (!nv_load_acq(&m->ogg_eof))
            m->underrun = 1U;
        return 0U;
    }
    nv_ring_read(&m->ogg, (uint8_t *)buffer, bytes);
    m->ogg_tail_pos += bytes;
    return count;
}

static long nv_io_ftell(FILE *token)
{
    nv_mirror *m = nv_token_mirror(token);

    return m ? (long)m->ogg_tail_pos : -1L;
}

/* Only forward seeks inside the ring window are meaningful for a pull
 * decoder that never rewinds during decode (skip()).  Anything else fails
 * exactly like the game's seek+tell check would. */
static int nv_io_fseek(FILE *token, long offset, int whence)
{
    nv_mirror *m = nv_token_mirror(token);
    uint32_t target;
    uint32_t delta;

    if (!m || whence != SEEK_SET || offset < 0)
        return -1;
    target = (uint32_t)offset;
    if (target == m->ogg_tail_pos)
        return 0;
    delta = target - m->ogg_tail_pos;
    if (target < m->ogg_tail_pos || delta > nv_ring_avail(&m->ogg)) {
        if (target > m->ogg_tail_pos && !nv_load_acq(&m->ogg_eof))
            m->underrun = 1U;
        return -1;
    }
    /* consume without copying */
    nv_store_rel(&m->ogg.tail, m->ogg.tail + delta);
    m->ogg_tail_pos = target;
    return 0;
}

static int nv_io_fclose(FILE *token)
{
    (void)token;
    return 0;
}

static FILE *nv_io_fopen(const char *name, const char *mode)
{
    (void)name;
    (void)mode;
    return NULL;
}

void isaac_nv_assert_fail(const char *expression, int line)
{
    nv_mirror *m = s_current_mirror[nv_is_worker_thread() ? 1 : 0];

    if (!m)
        return;
    m->assert_expr = expression;
    m->assert_line = (uint32_t)line;
    if (m->asserts != UINT32_MAX)
        ++m->asserts;
    m->broken = 1U;
    if (m->assert_armed) {
        m->assert_armed = 0U;
        longjmp(m->assert_jmp, 1);
    }
}

/* ------------------------------------------------------------------------
 * Ownership. */
static int nv_try_acquire(nv_mirror *m, uint32_t owner)
{
    uint32_t expected = NV_OWNER_NONE;

    return __atomic_compare_exchange_n(&m->busy, &expected, owner, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void nv_release(nv_mirror *m)
{
    nv_store_rel(&m->busy, NV_OWNER_NONE);
}

/* Game thread: wait for an in-flight worker frame.  Returns 1 when acquired. */
static int nv_acquire_wait(nv_mirror *m, uint32_t timeout_us, uint32_t *waited_us)
{
    uint64_t start = 0U;
    uint32_t waited = 0U;

    while (!nv_try_acquire(m, NV_OWNER_GAME)) {
        uint64_t now = nv_now_us();

        if (!start)
            start = now;
        waited = (uint32_t)(now - start);
        if (waited >= timeout_us) {
            *waited_us = waited;
            return 0;
        }
        nv_delay_us(50U);
    }
    *waited_us = waited;
    return 1;
}

/* ------------------------------------------------------------------------
 * Worker signalling. */
#if NV_HOST_BUILD || defined(ISAAC_VITA_NATIVE_VORBIS_WORKER)
#define NV_WORKER_CODE 1
#else
#define NV_WORKER_CODE 0
#endif

static void nv_signal_worker(void)
{
#if NV_HOST_BUILD
    if (!s_worker_thread_valid)
        return;
    pthread_mutex_lock(&s_wake_mutex);
    s_wake_pending = 1U;
    pthread_cond_signal(&s_wake_cond);
    pthread_mutex_unlock(&s_wake_mutex);
#else
    if (s_worker_flag >= 0)
        (void)sceKernelSetEventFlag(s_worker_flag, 1U);
#endif
}

#if NV_WORKER_CODE
static void nv_worker_wait(void)
{
#if NV_HOST_BUILD
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)ISAAC_NV_WORKER_IDLE_US * 1000L;
    while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        ++deadline.tv_sec;
    }
    pthread_mutex_lock(&s_wake_mutex);
    while (!s_wake_pending && !nv_load_acq(&s_worker_stop)) {
        if (pthread_cond_timedwait(&s_wake_cond, &s_wake_mutex, &deadline) != 0)
            break;
    }
    s_wake_pending = 0U;
    pthread_mutex_unlock(&s_wake_mutex);
#else
    unsigned int bits = 0U;
    SceUInt timeout = ISAAC_NV_WORKER_IDLE_US;

    if (s_worker_flag < 0) {
        nv_delay_us(ISAAC_NV_WORKER_IDLE_US);
        return;
    }
    (void)sceKernelWaitEventFlag(s_worker_flag, 1U,
                                 SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR,
                                 &bits, &timeout);
#endif
}
#endif

/* ------------------------------------------------------------------------
 * Decode one frame from the mirror into its PCM ring.  Caller holds busy
 * and has checked ring space and OGG guard.  Returns frames produced. */
static void nv_publish_end(nv_mirror *m)
{
    nv_store_rel(&m->pcm_error, (uint32_t)m->state.error);
    nv_store_rel(&m->pcm_eof, 1U);
}

static uint32_t nv_decode_frame(nv_mirror *m, int on_worker)
{
    float **outputs = NULL;
    int n;
    uint32_t bytes;

    if (m->broken || nv_load_acq(&m->pcm_eof))
        return 0U;
    s_current_mirror[on_worker ? 1 : 0] = m;
    if (setjmp(m->assert_jmp)) {
        s_current_mirror[on_worker ? 1 : 0] = NULL;
        m->broken = 1U;
        nv_publish_end(m);
        return 0U;
    }
    m->assert_armed = 1U;
    n = stb_vorbis_get_frame_float(&m->state, NULL, &outputs);
    if (n <= 0 || m->broken) {
        m->assert_armed = 0U;
        s_current_mirror[on_worker ? 1 : 0] = NULL;
        if (m->underrun)
            m->broken = 1U;
        nv_publish_end(m);
        return 0U;
    }
    if ((uint32_t)n > (uint32_t)m->state.blocksize_1 || !outputs) {
        m->assert_armed = 0U;
        s_current_mirror[on_worker ? 1 : 0] = NULL;
        m->broken = 1U;
        nv_publish_end(m);
        return 0U;
    }
    bytes = (uint32_t)n * m->frame_bytes;
#if NV_HOST_BUILD
    if (on_worker && nv_load_acq(&s_oracle_worker_frame_delay_us))
        nv_delay_us(nv_load_acq(&s_oracle_worker_frame_delay_us));
#endif
    /* still armed: a channel-layout assertion inside the conversion must
     * unwind before it can write past the scratch buffer */
    convert_channels_short_interleaved(m->channels_arg, (short *)m->scratch,
                                       m->state.channels, outputs, 0, n);
    m->assert_armed = 0U;
    s_current_mirror[on_worker ? 1 : 0] = NULL;
    if (m->broken) {
        nv_publish_end(m);
        return 0U;
    }
#if NV_ASYNC && NV_RECEIPT
    /* receipt CRC on the producer side: the worker hashes its own frames
     * on CPU 2, the game thread only the few it decodes itself */
    m->crc_run = isaac_nv_crc32(m->crc_run, m->scratch, bytes);
#endif
    nv_ring_write(&m->pcm, m->scratch, bytes);
    if (on_worker)
        m->frames_worker += (uint32_t)n;
    else
        m->frames_sync += (uint32_t)n;
    return (uint32_t)n;
}

#if NV_HOST_BUILD || defined(ISAAC_VITA_NATIVE_VORBIS_WORKER)
/* Worker-side fill: decode while there is room and enough input. */
static int nv_fill(nv_mirror *m, uint32_t max_frames, int on_worker)
{
    uint32_t frames = 0U;

    if (m->mode != NV_MODE_STREAM || !m->pcm.data || !m->ogg.data)
        return 0;
    while (frames < max_frames && !m->broken && !nv_load_acq(&m->pcm_eof)) {
        if (nv_load_acq(&m->game_waiting) || nv_load_acq(&m->retiring))
            break;
        if (nv_ring_free(&m->pcm) < m->frame_max_bytes)
            break;
        if (nv_ring_avail(&m->ogg) < ISAAC_NV_OGG_GUARD_BYTES &&
                !nv_load_acq(&m->ogg_eof))
            break;
        if (!nv_decode_frame(m, on_worker))
            break;
        ++frames;
    }
    return frames != 0U;
}
#endif

/* ------------------------------------------------------------------------
 * Worker thread. */
#if NV_WORKER_CODE
static void nv_worker_pass(int *progressed)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_NV_MIRRORS_MAX; ++i) {
        nv_mirror *m = &s_mirrors[i];

        uint32_t generation = nv_load_acq(&m->generation);

        if (!nv_load_acq(&m->active) || nv_load_acq(&m->retiring) ||
                nv_load_acq(&m->game_waiting) || m->mode != NV_MODE_STREAM)
            continue;
        if (!nv_try_acquire(m, NV_OWNER_WORKER))
            continue;
        /* Re-validate under busy: the game thread may have retired and
         * re-purposed the slot between the pre-check and the acquire. */
        if (nv_load_acq(&m->generation) == generation &&
                !nv_load_acq(&m->retiring) && nv_load_acq(&m->active) &&
                m->mode == NV_MODE_STREAM && m->pcm.data && m->ogg.data)
            *progressed |= nv_fill(m, ISAAC_NV_WORKER_FRAMES_PER_PASS, 1);
        else
            __atomic_fetch_add(&s_worker_refused, 1U, __ATOMIC_RELAXED);
        nv_release(m);
    }
}
#endif

#if NV_HOST_BUILD
static void *nv_worker_main(void *arg)
{
    (void)arg;
    nv_store_rel(&s_worker_running, 1U);
    while (!nv_load_acq(&s_worker_stop)) {
        int progressed = 0;
        nv_worker_pass(&progressed);
        if (!progressed)
            nv_worker_wait();
    }
    nv_store_rel(&s_worker_running, 0U);
    return NULL;
}
#elif defined(ISAAC_VITA_NATIVE_VORBIS_WORKER)
static int nv_worker_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    nv_store_rel(&s_worker_running, 1U);
    for (;;) {
        int progressed = 0;
        nv_worker_pass(&progressed);
        if (!progressed)
            nv_worker_wait();
    }
    return 0;
}
#endif

static int nv_worker_start(void)
{
    if (s_worker_start_attempted)
        return s_worker_start_result == 0U;
    s_worker_start_attempted = 1U;
#if !defined(ISAAC_VITA_NATIVE_VORBIS_WORKER) && !NV_HOST_BUILD
    s_worker_start_result = 1U;
    return 0;
#else
#if NV_ASYNC && NV_RECEIPT
    /* The worker folds the receipt CRC itself (nv_decode_frame): build the
     * lazily initialised table on this thread before the worker exists, so
     * no second core can ever read a half-built table. */
    (void)isaac_nv_crc32(0U, "", 0U);
#endif
#if NV_HOST_BUILD
    if (pthread_create(&s_worker_thread, NULL, nv_worker_main, NULL) != 0) {
        s_worker_start_result = 1U;
        return 0;
    }
    s_worker_thread_valid = 1;
    s_worker_start_result = 0U;
    return 1;
#else
    s_worker_flag = sceKernelCreateEventFlag("isaac_nv_wake", 0, 0, NULL);
    if (s_worker_flag < 0) {
        s_worker_start_result = (uint32_t)s_worker_flag;
        return 0;
    }
    s_worker_thread = sceKernelCreateThread(
        "isaac_nv_vorbis", nv_worker_main, ISAAC_NV_WORKER_PRIORITY,
        ISAAC_NV_WORKER_STACK_BYTES, 0U, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (s_worker_thread < 0) {
        s_worker_start_result = (uint32_t)s_worker_thread;
        return 0;
    }
    {
        int rc = sceKernelStartThread(s_worker_thread, 0U, NULL);
        if (rc < 0) {
            (void)sceKernelDeleteThread(s_worker_thread);
            s_worker_thread = -1;
            s_worker_start_result = (uint32_t)rc;
            return 0;
        }
    }
    s_worker_start_result = 0U;
    return 1;
#endif
#endif
}

/* ------------------------------------------------------------------------
 * Receipts (game thread only). */
static int nv_power_of_two(uint32_t v)
{
    return v != 0U && (v & (v - 1U)) == 0U;
}

static void nv_log_init(void)
{
    if (s_stats.init_logged)
        return;
    s_stats.init_logged = 1U;
    isaac_vita_log(
        "KAGE VITA NATIVE VORBIS: build=%s decoder=stb_vorbis-v1.04/b8e0530f "
        "layout=0x%x worker=%s thread=0x%08x affinity=0x%08x prio=%d rc=0x%08x "
        "mirrors=%u ogg_ring=%u pcm_ring=%u guard=%u reserve=%u verify=%s "
        "receipt=%s",
        ISAAC_VITA_NATIVE_VORBIS_BUILD_ID, (unsigned)ISAAC_NV_STATE_BYTES,
#if defined(ISAAC_VITA_NATIVE_VORBIS_WORKER) || NV_HOST_BUILD
        s_worker_start_result == 0U ? "on" : "start-failed",
#else
        "off(sync)",
#endif
#if NV_HOST_BUILD
        0U, 0U,
#else
        (unsigned)s_worker_thread, (unsigned)SCE_KERNEL_CPU_MASK_USER_2,
#endif
        (int)ISAAC_NV_WORKER_PRIORITY, (unsigned)s_worker_start_result,
        (unsigned)ISAAC_NV_MIRRORS_MAX, (unsigned)ISAAC_NV_OGG_RING_BYTES,
        (unsigned)ISAAC_NV_PCM_RING_BYTES, (unsigned)ISAAC_NV_OGG_GUARD_BYTES,
        (unsigned)ISAAC_NV_TEMP_RESERVE_BYTES,
#if defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
        "on",
#else
        "off",
#endif
#if defined(ISAAC_VITA_NATIVE_VORBIS_RECEIPT)
        "on");
#else
        "off");
#endif
}

#if NV_RECEIPT
static void nv_log_stats(void)
{
    isaac_vita_log(
        "KAGE VITA NATIVE VORBIS STATS: slots=%u frames=%u worker_frames=%u "
        "sync_frames=%u waits=%u wait_max_us=%u fallbacks=%u rejected=%u "
        "reclones=%u evictions=%u asserts=%u underruns=%u pool_hw=%u "
        "verify_diff_slots=%u alloc_refused=%u worker_refused=%u "
        "io_ms=%u io_max_us=%u freed_retires=%u zero_fallbacks=%u"
#if NV_ASYNC
        " partial=%u budget_hits=%u early=%u early_rejected=%u"
#endif
        ,
        (unsigned)s_stats.slots, (unsigned)s_stats.frames,
        (unsigned)s_stats.frames_worker, (unsigned)s_stats.frames_sync,
        (unsigned)s_stats.waits, (unsigned)s_stats.wait_us_max,
        (unsigned)s_stats.fallbacks, (unsigned)s_stats.clone_rejected,
        (unsigned)s_stats.reclones, (unsigned)s_stats.evictions,
        (unsigned)s_stats.asserts, (unsigned)s_stats.underruns,
        (unsigned)s_stats.pool_high_water,
        (unsigned)s_stats.verify_diff_slots,
#if NV_HOST_BUILD
        0U,
#else
        (unsigned)s_vendored_alloc_refused,
#endif
        (unsigned)nv_load_acq(&s_worker_refused),
        (unsigned)(s_stats.io_us_total / 1000U), (unsigned)s_stats.io_us_max,
        (unsigned)s_stats.freed_retires, (unsigned)s_stats.zero_fallbacks
#if NV_ASYNC
        , (unsigned)s_stats.partial_slots, (unsigned)s_stats.budget_hits,
        (unsigned)s_stats.early_clones, (unsigned)s_stats.early_rejected
#endif
        );
}
#endif

static void nv_log_fallback(uint32_t f, const char *reason)
{
    uint32_t n = s_stats.fallbacks == UINT32_MAX ? UINT32_MAX
                                                  : ++s_stats.fallbacks;

    if (nv_power_of_two(n) || (n & 0xffU) == 0U)
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS FALLBACK: f=%08x reason=%s n=%u "
            "(translated decoder used)", (unsigned)f, reason, (unsigned)n);
}

/* ------------------------------------------------------------------------
 * Guest stream calls (game thread, CPU context required).  Argument order is
 * the translated one: thiscall, arguments pushed right to left, callee
 * cleans (`ret 0xc`/`ret 8`/`ret 0`), result in eax. */
static uint32_t nv_guest_vcall(CPU *__restrict c, uint32_t object,
                               uint32_t slot, uint32_t site,
                               const uint32_t *args, unsigned count, int *ok)
{
    uint32_t vtable;
    uint32_t target;
    uint32_t saved_esp = c->esp;
    unsigned i;

    *ok = 0;
    if (!object || (object & 3U))
        return 0U;
    vtable = ld32(object);
    if (!vtable || (vtable & 3U))
        return 0U;
    target = ld32(vtable + slot);
    if (!target)
        return 0U;
    for (i = count; i-- > 0U;)
        gpush(c, args[i]);
    gpush(c, site);
    c->ecx = object;
    guest_call(c, target);
    if (c->esp != saved_esp) {
        /* Not the frozen convention: restore and refuse further I/O. */
        c->esp = saved_esp;
        return 0U;
    }
    *ok = 1;
    return c->eax;
}

static int nv_guest_tell(CPU *__restrict c, nv_mirror *m, uint32_t *pos)
{
    int ok;
    uint32_t v = nv_guest_vcall(c, m->guest_stream, ISAAC_NV_STREAM_VT_TELL,
                                ISAAC_NV_SITE_TELL, NULL, 0U, &ok);
    if (ok)
        *pos = v;
    return ok;
}

static int nv_guest_seek(CPU *__restrict c, nv_mirror *m, uint32_t pos)
{
    uint32_t args[2];
    uint32_t got = 0U;
    int ok;

    if (m->phys_valid && m->phys_pos == pos)
        return 1;
    args[0] = pos;
    args[1] = 0U;   /* whence: SEEK_SET, as set_file_offset pushes */
    (void)nv_guest_vcall(c, m->guest_stream, ISAAC_NV_STREAM_VT_SEEK,
                         ISAAC_NV_SITE_SEEK, args, 2U, &ok);
    if (!ok || !nv_guest_tell(c, m, &got) || got != pos) {
        m->phys_valid = 0U;
        return 0;
    }
    m->phys_valid = 1U;
    m->phys_pos = pos;
    return 1;
}

/* Hand-off: put the KAGE stream where the guest state expects it.  While a
 * mirror serves a state, that state is frozen at the clone position
 * (guest_logical_pos) and the stream rests wherever the read-ahead stopped
 * (ogg_head_pos), so the next top-up continues forward without a seek.  The
 * state goes back to the guest only at a hand-off -- translated fallback, a
 * re-clone that reads the position, eviction, a pump without a preceding
 * seek -- and only there is the stream seeked back.
 *
 * This is the one backward seek in the module and it must never run per
 * slot: KAGE streams are LZ4 archive entries whose seek re-decodes from the
 * entry start, so its cost is the stream position.  perf:wf-flags-v4-audio2
 * (2026-09-03) had a seek-back after every wrapped call and measured the
 * game thread's io_us at 28/39/58/105/182/361 ms for slots 4/8/16/32/64/128
 * of the two 2.7 MB music layers (wait_us=0, PCM ring full: the core-2
 * decoder was idle), 100 s of stream I/O on the frame thread in 165 s and
 * ~1 FPS; the build before that seek-back sat at 1.5-4.4 ms per slot. */
static int nv_guest_sync(CPU *__restrict c, nv_mirror *m)
{
    if (m->mode != NV_MODE_STREAM || !m->guest_stream)
        return 1;
    return nv_guest_seek(c, m, m->guest_logical_pos);
}

/* read(buffer, 1, n) -> bytes read */
static uint32_t nv_guest_read(CPU *__restrict c, nv_mirror *m,
                              uint8_t *buffer, uint32_t n, int *ok)
{
    uint32_t args[3];
    uint32_t got;

    args[0] = (uint32_t)(uintptr_t)buffer;
    args[1] = 1U;
    args[2] = n;
    got = nv_guest_vcall(c, m->guest_stream, ISAAC_NV_STREAM_VT_READ,
                         ISAAC_NV_SITE_GETN_READ, args, 3U, ok);
    if (!*ok)
        return 0U;
    if (got > n)
        got = n;
    if (m->phys_valid)
        m->phys_pos += got;
    return got;
}

/* Game thread producer: top the OGG ring up from the guest stream with one
 * block read into a staging buffer (game thread only), then publish. */
static uint8_t *s_staging;
static uint32_t s_staging_capacity;
static int nv_grow(uint8_t **buffer, uint32_t *capacity, uint32_t needed);

static void nv_topup(CPU *__restrict c, nv_mirror *m)
{
    uint32_t free_bytes;
    uint32_t got;
    int ok;

    uint64_t t0;

    if (nv_load_acq(&m->ogg_eof))
        return;
    free_bytes = nv_ring_free(&m->ogg);
    if (free_bytes < 4096U)
        return;
    /* One bounded block per call: the guest stream read runs translated
     * archive code (LZ4 in the KAGE file system), so a whole-ring read at
     * stream open would stall the frame; a chunk per call keeps the ring
     * ahead of the 7-20 KiB one slot consumes. */
    if (free_bytes > ISAAC_NV_TOPUP_CHUNK_BYTES)
        free_bytes = ISAAC_NV_TOPUP_CHUNK_BYTES;
    if (!nv_grow(&s_staging, &s_staging_capacity, m->ogg.capacity)) {
        nv_store_rel(&m->ogg_eof, 1U);
        return;
    }
    t0 = nv_now_us();
    if (!nv_guest_seek(c, m, m->ogg_head_pos)) {
        /* Cannot position the stream: treat as the end so the mirror
         * drains and the guest's own seek_start recovers. */
        nv_store_rel(&m->ogg_eof, 1U);
        return;
    }
    got = nv_guest_read(c, m, s_staging, free_bytes, &ok);
    m->io_us += (uint32_t)(nv_now_us() - t0);
    if (!ok) {
        nv_store_rel(&m->ogg_eof, 1U);
        return;
    }
    nv_ring_write(&m->ogg, s_staging, got);
    m->ogg_head_pos += got;
    if (got < free_bytes)
        nv_store_rel(&m->ogg_eof, 1U);
}

/* ------------------------------------------------------------------------
 * Clone: copy the guest state and the used prefix of its alloc buffer and
 * relocate every internal pointer.  Pure host-pointer code; the oracle
 * drives it with native states. */
enum {
    NV_CLONE_OK = 0,
    NV_CLONE_REJECT_HEADER,
    NV_CLONE_REJECT_ALLOC,
    NV_CLONE_REJECT_POINTER,
    NV_CLONE_REJECT_MEMORY,
    NV_CLONE_REJECT_MODE
};

static const char *nv_clone_reason(int code)
{
    switch (code) {
    case NV_CLONE_REJECT_HEADER: return "header";
    case NV_CLONE_REJECT_ALLOC: return "alloc";
    case NV_CLONE_REJECT_POINTER: return "pointer";
    case NV_CLONE_REJECT_MEMORY: return "memory";
    case NV_CLONE_REJECT_MODE: return "mode";
    default: return "ok";
    }
}

static int nv_pow2_in(uint32_t v, uint32_t lo, uint32_t hi)
{
    return nv_power_of_two(v) && v >= lo && v <= hi;
}

static int nv_inspect(const stb_vorbis *g, int *mode)
{
    if (g->push_mode)
        return NV_CLONE_REJECT_MODE;
    if (g->channels < 1 || g->channels > STB_VORBIS_MAX_CHANNELS)
        return NV_CLONE_REJECT_HEADER;
    if (g->sample_rate < 8000U || g->sample_rate > 192000U)
        return NV_CLONE_REJECT_HEADER;
    if (!nv_pow2_in((uint32_t)g->blocksize_0, 64U, 8192U) ||
            !nv_pow2_in((uint32_t)g->blocksize_1, 64U, 8192U) ||
            g->blocksize_0 > g->blocksize_1)
        return NV_CLONE_REJECT_HEADER;
    if (g->codebook_count < 0 || g->codebook_count > 256 ||
            g->floor_count < 0 || g->floor_count > 64 ||
            g->residue_count < 0 || g->residue_count > 64 ||
            g->mapping_count < 0 || g->mapping_count > 64 ||
            g->mode_count < 0 || g->mode_count > 64)
        return NV_CLONE_REJECT_HEADER;
    if (!g->alloc.alloc_buffer || g->alloc.alloc_buffer_length_in_bytes < 0x1000 ||
            g->alloc.alloc_buffer_length_in_bytes > 0x2000000)
        return NV_CLONE_REJECT_ALLOC;
    if (g->setup_offset <= 0 ||
            g->setup_offset > g->alloc.alloc_buffer_length_in_bytes ||
            g->temp_offset != g->alloc.alloc_buffer_length_in_bytes)
        return NV_CLONE_REJECT_ALLOC;
    if (g->stream) {
        if (!g->stream_start || g->stream_start > g->stream ||
                g->stream > g->stream_end)
            return NV_CLONE_REJECT_MODE;
        *mode = NV_MODE_MEMORY;
    } else {
        if (!g->f)
            return NV_CLONE_REJECT_MODE;
        *mode = NV_MODE_STREAM;
    }
    return NV_CLONE_OK;
}

static int nv_fix_pointer(void *slot, uintptr_t base, uintptr_t len,
                          intptr_t delta)
{
    void *value;
    uintptr_t p;

    memcpy(&value, slot, sizeof value);
    p = (uintptr_t)value;
    if (!p)
        return 1;
    if (p - base > len)
        return 0;
    value = (void *)(p + (uintptr_t)delta);
    memcpy(slot, &value, sizeof value);
    return 1;
}

#define NV_FIX(field) \
    do { \
        if (!nv_fix_pointer(&(field), base, setup_len, delta)) \
            return NV_CLONE_REJECT_POINTER; \
    } while (0)

static int nv_clone_relocate(stb_vorbis *s, uintptr_t base, uintptr_t setup_len,
                             intptr_t delta)
{
    int i, j;

    NV_FIX(s->codebooks);
    for (i = 0; i < s->codebook_count && s->codebooks; ++i) {
        Codebook *cb = &s->codebooks[i];
        NV_FIX(cb->codeword_lengths);
        NV_FIX(cb->multiplicands);
        NV_FIX(cb->codewords);
        NV_FIX(cb->sorted_codewords);
        NV_FIX(cb->sorted_values);
    }
    NV_FIX(s->floor_config);
    NV_FIX(s->residue_config);
    for (i = 0; i < s->residue_count && s->residue_config; ++i) {
        Residue *r = &s->residue_config[i];
        NV_FIX(r->classdata);
        NV_FIX(r->residue_books);
        if (r->classdata) {
            int entries;
            if (r->classbook >= s->codebook_count || !s->codebooks)
                return NV_CLONE_REJECT_POINTER;
            entries = s->codebooks[r->classbook].entries;
            if (entries < 0 || entries > (1 << 24))
                return NV_CLONE_REJECT_POINTER;
            for (j = 0; j < entries; ++j)
                NV_FIX(r->classdata[j]);
        }
    }
    NV_FIX(s->mapping);
    for (i = 0; i < s->mapping_count && s->mapping; ++i)
        NV_FIX(s->mapping[i].chan);
    for (i = 0; i < STB_VORBIS_MAX_CHANNELS; ++i) {
        NV_FIX(s->channel_buffers[i]);
        NV_FIX(s->outputs[i]);
        NV_FIX(s->previous_window[i]);
        NV_FIX(s->finalY[i]);
    }
    for (i = 0; i < 2; ++i) {
        NV_FIX(s->A[i]);
        NV_FIX(s->B[i]);
        NV_FIX(s->C[i]);
        NV_FIX(s->window[i]);
        NV_FIX(s->bit_reverse[i]);
    }
    return NV_CLONE_OK;
}

#undef NV_FIX

static int nv_grow(uint8_t **buffer, uint32_t *capacity, uint32_t needed)
{
    uint8_t *next;

    if (*capacity >= needed && *buffer)
        return 1;
    next = (uint8_t *)nv_malloc(needed);
    if (!next)
        return 0;
    if (*buffer)
        nv_free(*buffer);
    *buffer = next;
    *capacity = needed;
    return 1;
}

/* Clone `g` into `m` (state + tables), token the stream, size the scratch.
 * Game thread.  Rings and identity are handled by the caller. */
static int nv_clone_into(nv_mirror *m, const stb_vorbis *g, int channels_arg)
{
    int mode = NV_MODE_NONE;
    int rc = nv_inspect(g, &mode);
    uintptr_t base;
    uint32_t setup_len;
    uint32_t clone_len;
    uint32_t frame_max;
    intptr_t delta;

    if (rc != NV_CLONE_OK)
        return rc;
    base = (uintptr_t)g->alloc.alloc_buffer;
    setup_len = (uint32_t)g->setup_offset;
    clone_len = setup_len + ISAAC_NV_TEMP_RESERVE_BYTES;
    if (clone_len > (uint32_t)g->alloc.alloc_buffer_length_in_bytes)
        clone_len = (uint32_t)g->alloc.alloc_buffer_length_in_bytes;
    clone_len = (clone_len + 3U) & ~3U;
    if (!nv_grow(&m->clone, &m->clone_capacity, clone_len))
        return NV_CLONE_REJECT_MEMORY;
    frame_max = (uint32_t)g->blocksize_1 * (uint32_t)channels_arg * 2U;
    if (!nv_grow(&m->scratch, &m->scratch_capacity, frame_max))
        return NV_CLONE_REJECT_MEMORY;
    memcpy(&m->state, g, sizeof m->state);
    memcpy(m->clone, (const void *)base, setup_len);
    delta = (intptr_t)((uintptr_t)m->clone - base);
    rc = nv_clone_relocate(&m->state, base, setup_len, delta);
    if (rc != NV_CLONE_OK)
        return rc;
    m->state.alloc.alloc_buffer = (char *)m->clone;
    m->state.alloc.alloc_buffer_length_in_bytes = (int)clone_len;
    m->state.temp_offset = (int)clone_len;
    m->state.f = (mode == NV_MODE_STREAM) ? (FILE *)m : NULL;
    m->mode = mode;
    m->channels_arg = channels_arg;
    m->frame_bytes = (uint32_t)channels_arg * 2U;
    m->frame_max_bytes = frame_max;
    m->clone_len = clone_len;
    m->broken = 0U;
    m->underrun = 0U;
    m->assert_armed = 0U;
    m->frames_worker = 0U;
    m->frames_sync = 0U;
    m->frames_worker_seen = 0U;
    m->frames_sync_seen = 0U;
    nv_store_rel(&m->pcm_eof, 0U);
    nv_store_rel(&m->pcm_error, 0U);
    nv_store_rel(&m->ogg_eof, 0U);
    return NV_CLONE_OK;
}

/* ------------------------------------------------------------------------
 * Pool. */
static nv_mirror *nv_lookup(uint32_t f)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_NV_MIRRORS_MAX; ++i)
        if (s_mirrors[i].active && s_mirrors[i].guest_f == f)
            return &s_mirrors[i];
    return NULL;
}

/* Stop the decoder role and make the mirror inert.  Returns 0 when a
 * worker frame did not finish inside the bound; the slot is then poisoned. */
static int nv_quiesce(nv_mirror *m)
{
    uint32_t waited = 0U;

    nv_store_rel(&m->retiring, 1U);
    if (!nv_acquire_wait(m, ISAAC_NV_ACQUIRE_TIMEOUT_US * 2U, &waited)) {
        m->poisoned = 1U;
        nv_store_rel(&m->active, 0U);
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS ERR: id=%u f=%08x worker held the "
            "mirror for %u us; slot poisoned", (unsigned)m->id,
            (unsigned)m->guest_f, (unsigned)waited);
        return 0;
    }
    /* holding busy as the game thread: nothing else can decode */
    return 1;
}

static void nv_log_close(nv_mirror *m, const char *reason)
{
#if NV_RECEIPT
    isaac_vita_log(
        "KAGE VITA NATIVE VORBIS CLOSE: id=%u f=%08x reason=%s slots=%u "
        "frames=%u run=%08x worker_frames=%u sync_frames=%u waits=%u "
        "wait_max_us=%u asserts=%u underruns=%u reclones=%u "
        "verify_slots=%u verify_diff_slots=%u verify_diff_samples=%u "
        "verify_maxabs=%u",
        (unsigned)m->id, (unsigned)m->guest_f, reason, (unsigned)m->slots,
        (unsigned)m->frames_out, (unsigned)m->crc_run,
        (unsigned)m->frames_worker, (unsigned)m->frames_sync,
        (unsigned)m->waits, (unsigned)m->wait_us_max, (unsigned)m->asserts,
        (unsigned)m->underruns, (unsigned)m->reclones,
        (unsigned)m->verify_slots, (unsigned)m->verify_diff_slots,
        (unsigned)m->verify_diff_samples, (unsigned)m->verify_maxabs);
#else
    (void)m;
    (void)reason;
#endif
}

static void nv_retire(nv_mirror *m, const char *reason)
{
    if (!m->active)
        return;
    nv_log_close(m, reason);
    if (!nv_quiesce(m))
        return;
    nv_store_rel(&m->active, 0U);
    m->guest_f = 0U;
    m->mode = NV_MODE_NONE;
    nv_ring_reset(&m->ogg);
    nv_ring_reset(&m->pcm);
    nv_store_rel(&m->generation, m->generation + 1U);
    nv_release(m);
    nv_store_rel(&m->retiring, 0U);
}

static nv_mirror *nv_pool_take(CPU *__restrict c, uint64_t now)
{
    uint32_t i;
    nv_mirror *victim = NULL;
    uint32_t live = 0U;

    for (i = 0U; i < ISAAC_NV_MIRRORS_MAX; ++i) {
        nv_mirror *m = &s_mirrors[i];
        if (m->poisoned)
            continue;
        if (!m->active)
            return m;
        ++live;
        if (now - m->last_call_us >= ISAAC_NV_EVICT_IDLE_US &&
                (!victim || m->last_call_us < victim->last_call_us))
            victim = m;
    }
    if (victim) {
        if (s_stats.evictions != UINT32_MAX)
            ++s_stats.evictions;
        /* The evicted state stays alive in the guest and is re-cloned (or
         * decoded translated) at its next call from its stream position. */
        (void)nv_guest_sync(c, victim);
        nv_retire(victim, "evicted");
        if (!victim->active)
            return victim;
    }
    (void)live;
    return NULL;
}

static int nv_ring_alloc(nv_ring *r, uint32_t capacity)
{
    if (r->data && r->capacity == capacity)
        return 1;
    if (r->data)
        nv_free(r->data);
    r->data = (uint8_t *)nv_malloc(capacity);
    r->capacity = capacity;
    r->head = 0U;
    r->tail = 0U;
    return r->data != NULL;
}

static uint32_t nv_pool_live(void)
{
    uint32_t i, live = 0U;
    for (i = 0U; i < ISAAC_NV_MIRRORS_MAX; ++i)
        if (s_mirrors[i].active)
            ++live;
    return live;
}

/* Snapshot the identity fields used to detect a guest re-open at the same
 * address, and write the marker into the guest state. */
static void nv_bind_identity(nv_mirror *m, uint32_t f, const stb_vorbis *g)
{
    m->guest_f = f;
    m->guest_stream = (uint32_t)(uintptr_t)g->f;
    m->guest_alloc_buffer = (uint32_t)(uintptr_t)g->alloc.alloc_buffer;
    m->guest_alloc_len = (uint32_t)g->alloc.alloc_buffer_length_in_bytes;
    m->snap_serial = g->serial;
    m->snap_codebooks = (uint32_t)(uintptr_t)g->codebooks;
    m->snap_sample_rate = g->sample_rate;
    m->snap_channels = (uint32_t)g->channels;
    m->snap_first_page = g->first_audio_page_offset;
    m->snap_stream_len = g->stream_len;
    m->marker = 0x4e560000U | (m->id & 0xffffU);
    st32(f + ISAAC_NV_STATE_MARKER_OFFSET, m->marker);
}

static int nv_identity_holds(const nv_mirror *m, const stb_vorbis *g)
{
    return ld32(m->guest_f + ISAAC_NV_STATE_MARKER_OFFSET) == m->marker &&
           (uint32_t)(uintptr_t)g->f == m->guest_stream &&
           (uint32_t)(uintptr_t)g->alloc.alloc_buffer == m->guest_alloc_buffer &&
           g->serial == m->snap_serial &&
           (uint32_t)(uintptr_t)g->codebooks == m->snap_codebooks &&
           g->sample_rate == m->snap_sample_rate &&
           (uint32_t)g->channels == m->snap_channels &&
           g->first_audio_page_offset == m->snap_first_page &&
           g->stream_len == m->snap_stream_len;
}

static void nv_log_open(nv_mirror *m, const stb_vorbis *g, const char *kind)
{
#if NV_RECEIPT
    isaac_vita_log(
        "KAGE VITA NATIVE VORBIS %s: id=%u f=%08x mode=%s sr=%u ch=%u "
        "arg_ch=%d bs=%d/%d setup=%u clone=%u alloc=%08x/%u stream=%08x "
        "pos=%u len=%u serial=%08x first_page=%u temp_req=%u leftover=%u "
        "pool=%u",
        kind, (unsigned)m->id, (unsigned)m->guest_f,
        m->mode == NV_MODE_STREAM ? "stream" : "memory",
        (unsigned)g->sample_rate, (unsigned)g->channels, m->channels_arg,
        g->blocksize_0, g->blocksize_1, (unsigned)g->setup_offset,
        (unsigned)m->clone_len, (unsigned)m->guest_alloc_buffer,
        (unsigned)m->guest_alloc_len, (unsigned)m->guest_stream,
        (unsigned)m->ogg_tail_pos, (unsigned)g->stream_len,
        (unsigned)g->serial, (unsigned)g->first_audio_page_offset,
        (unsigned)g->temp_memory_required, (unsigned)m->leftover_frames,
        (unsigned)nv_pool_live());
#else
    (void)m;
    (void)g;
    (void)kind;
#endif
}

/* (Re)start a mirror for guest state f.  Game thread.  Returns NULL and the
 * reason when the translated decoder must be used. */
static nv_mirror *nv_start(CPU *__restrict c, nv_mirror *m, uint32_t f,
                           int channels_arg, const char **reason,
                           const char *reclone)
{
    const stb_vorbis *g = (const stb_vorbis *)(uintptr_t)f;
    uint64_t now = nv_now_us();
    int rc;

    if (!m) {
        m = nv_pool_take(c, now);
        if (!m) {
            *reason = "pool-full";
            return NULL;
        }
        m->id = ++s_stats.next_id;
        m->slots = 0U;
        m->frames_out = 0U;
        m->crc_run = 0U;
        m->waits = 0U;
        m->wait_us_max = 0U;
        m->asserts = 0U;
        m->underruns = 0U;
        m->reclones = 0U;
        m->verify_slots = 0U;
        m->verify_diff_slots = 0U;
        m->verify_diff_samples = 0U;
        m->verify_maxabs = 0U;
        m->verify_lines = 0U;
        m->phys_valid = 0U;
        /* the slot is inert: take busy so no worker can start on it */
        if (!nv_try_acquire(m, NV_OWNER_GAME)) {
            *reason = "pool-busy";
            return NULL;
        }
    }
    /* caller holds busy here in both paths; the slot changes hands */
    nv_store_rel(&m->generation, m->generation + 1U);
    m->mode = NV_MODE_NONE;
    rc = nv_clone_into(m, g, channels_arg);
    if (rc != NV_CLONE_OK) {
        if (s_stats.clone_rejected != UINT32_MAX)
            ++s_stats.clone_rejected;
        *reason = nv_clone_reason(rc);
        nv_store_rel(&m->active, 0U);
        m->guest_f = 0U;
        m->mode = NV_MODE_NONE;
        nv_release(m);
        nv_store_rel(&m->retiring, 0U);
        return NULL;
    }
    if (m->mode == NV_MODE_STREAM &&
            m->frame_max_bytes * 2U > ISAAC_NV_PCM_RING_BYTES) {
        if (s_stats.clone_rejected != UINT32_MAX)
            ++s_stats.clone_rejected;
        *reason = "frame-too-large";
        nv_store_rel(&m->active, 0U);
        m->guest_f = 0U;
        m->mode = NV_MODE_NONE;
        nv_release(m);
        nv_store_rel(&m->retiring, 0U);
        return NULL;
    }
    nv_bind_identity(m, f, g);
    m->dirty = 0U;
#if NV_ASYNC
    m->timed_out = 0U;
#endif
    nv_ring_reset(&m->pcm);
    nv_ring_reset(&m->ogg);
#if !NV_HOST_BUILD
    if (!s_worker_start_attempted) {
        (void)nv_worker_start();
        nv_log_init();
#if NV_ASYNC
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS ASYNC: on min_frames=%u "
            "sync_budget_us=%u early_chunks=%u topup_chunk=%u",
            (unsigned)ISAAC_NV_SLOT_MIN_FRAMES,
            (unsigned)ISAAC_NV_SYNC_BUDGET_US,
            (unsigned)ISAAC_NV_EARLY_TOPUP_CHUNKS,
            (unsigned)ISAAC_NV_TOPUP_CHUNK_BYTES);
#endif
    }
#endif
    if (m->mode == NV_MODE_STREAM) {
        uint32_t pos = 0U;

        if (!nv_ring_alloc(&m->ogg, ISAAC_NV_OGG_RING_BYTES) ||
                !nv_ring_alloc(&m->pcm, ISAAC_NV_PCM_RING_BYTES)) {
            *reason = "memory";
            nv_store_rel(&m->active, 0U);
            m->guest_f = 0U;
            m->mode = NV_MODE_NONE;
            nv_release(m);
            nv_store_rel(&m->retiring, 0U);
            return NULL;
        }
        m->phys_valid = 0U;
        if (!nv_guest_tell(c, m, &pos)) {
            *reason = "tell";
            nv_store_rel(&m->active, 0U);
            m->guest_f = 0U;
            m->mode = NV_MODE_NONE;
            nv_release(m);
            nv_store_rel(&m->retiring, 0U);
            return NULL;
        }
        m->phys_valid = 1U;
        m->phys_pos = pos;
        m->ogg_head_pos = pos;
        m->ogg_tail_pos = pos;
        m->guest_logical_pos = pos;
        {
            int pending = m->state.channel_buffer_end -
                          m->state.channel_buffer_start;

            if (pending > 0 && pending <= m->state.blocksize_1 &&
                    m->state.channel_buffer_start >= 0) {
                convert_channels_short_interleaved(
                    m->channels_arg, (short *)m->scratch, m->state.channels,
                    m->state.channel_buffers, m->state.channel_buffer_start,
                    pending);
#if NV_ASYNC && NV_RECEIPT
                m->crc_run = isaac_nv_crc32(m->crc_run, m->scratch,
                                            (uint32_t)pending * m->frame_bytes);
#endif
                nv_ring_write(&m->pcm, m->scratch,
                              (uint32_t)pending * m->frame_bytes);
                m->state.channel_buffer_start = m->state.channel_buffer_end;
                m->leftover_frames = (uint32_t)pending;
            } else {
                m->leftover_frames = 0U;
            }
        }
    }
    m->last_call_us = now;
    nv_store_rel(&m->retiring, 0U);
    nv_store_rel(&m->active, 1U);
    nv_release(m);
    if (reclone) {
        if (m->reclones != UINT32_MAX)
            ++m->reclones;
        if (s_stats.reclones != UINT32_MAX)
            ++s_stats.reclones;
    } else {
        uint32_t live = nv_pool_live();
        if (s_stats.mirrors_created != UINT32_MAX)
            ++s_stats.mirrors_created;
        if (live > s_stats.pool_high_water)
            s_stats.pool_high_water = live;
    }
    nv_log_open(m, g, reclone ? reclone : "OPEN");
    return m;
}

/* ------------------------------------------------------------------------
 * Serving. */
typedef struct nv_serve_result {
    int32_t frames;
    uint32_t wait_us;
    uint32_t sync_frames;
    uint32_t worker_frames;
    int ended;
    const char *source;
#if NV_ASYNC
    uint32_t sync_us;       /* time inside the bounded game-thread path */
#endif
} nv_serve_result;

#if NV_ASYNC
/* Move up to `len - *n` ready frames from the PCM ring into dst. */
static void nv_take_ready(nv_mirror *m, uint8_t *dst, int32_t len, int32_t *n)
{
    uint32_t avail = nv_ring_avail(&m->pcm) / m->frame_bytes;
    uint32_t take = avail;

    if (!avail || *n >= len)
        return;
    if ((int32_t)take > len - *n)
        take = (uint32_t)(len - *n);
    nv_ring_read(&m->pcm, dst + (uint32_t)*n * m->frame_bytes,
                 take * m->frame_bytes);
    *n += (int32_t)take;
}

/* Stream mode, partial slots (ASYNC).  Copy what the worker has ready; that
 * is the slot when it is at least MIN_FRAMES (the guest accepts any positive
 * count and only buffer boundaries move).  Below MIN_FRAMES the game thread
 * decodes on the mirror itself, one frame at a time, until MIN_FRAMES are
 * ready or SYNC_BUDGET_US is spent -- and never returns 0 frames before the
 * stream has ended.  The worker is excluded (game_waiting) only while this
 * thread wants the decoder role. */
static void nv_serve_stream(CPU *__restrict c, nv_mirror *m, uint8_t *dst,
                            int32_t len, nv_serve_result *out)
{
    int32_t n = 0;
    int32_t min_frames = (int32_t)ISAAC_NV_SLOT_MIN_FRAMES;
    uint32_t worker_now;
    uint64_t wait_start = 0U;
    uint64_t sync_start = 0U;

    if (min_frames > len)
        min_frames = len;
    if (min_frames < 1)
        min_frames = 1;
    out->wait_us = 0U;
    out->ended = 0;
    out->sync_us = 0U;
    nv_topup(c, m);
    nv_signal_worker();
    for (;;) {
        uint64_t now;
        uint32_t waited;
        int decoded;

        nv_take_ready(m, dst, len, &n);
        if (n >= min_frames)
            break;
        if (nv_load_acq(&m->pcm_eof)) {
            /* the last frame is published before pcm_eof: take it first */
            if (nv_ring_avail(&m->pcm) >= m->frame_bytes)
                continue;
            out->ended = 1;
            break;
        }
        now = nv_now_us();
        if (!sync_start) {
            sync_start = now;
        } else if (n > 0 && now - sync_start >= ISAAC_NV_SYNC_BUDGET_US) {
            if (s_stats.budget_hits != UINT32_MAX)
                ++s_stats.budget_hits;
            break;
        }
        nv_store_rel(&m->game_waiting, 1U);
        if (!nv_try_acquire(m, NV_OWNER_GAME)) {
            /* The worker is inside a frame of this mirror.  It stops at the
             * frame boundary because game_waiting is set; meanwhile the PCM
             * it publishes is exactly what we need, so wait for either. */
            if (!wait_start)
                wait_start = now;
            waited = (uint32_t)(now - wait_start);
            if (waited >= ISAAC_NV_ACQUIRE_TIMEOUT_US) {
                nv_store_rel(&m->game_waiting, 0U);
                out->wait_us += waited;
                out->ended = 1;
                /* n == 0 here (n > 0 leaves through the budget): the caller
                 * drops the mirror and runs the translated body rather than
                 * telling the guest the stream ended */
                m->timed_out = 1U;
                isaac_vita_log(
                    "KAGE VITA NATIVE VORBIS ERR: id=%u worker held the "
                    "mirror for %u us; returning %d of %d frames",
                    (unsigned)m->id, (unsigned)waited, (int)n, (int)len);
                break;
            }
            nv_delay_us(50U);
            continue;
        }
        if (wait_start) {
            waited = (uint32_t)(now - wait_start);
            wait_start = 0U;
            out->wait_us += waited;
            if (m->waits != UINT32_MAX)
                ++m->waits;
            if (waited > m->wait_us_max)
                m->wait_us_max = waited;
        }
        /* one frame on this thread */
        decoded = 0;
        if (!m->broken && !nv_load_acq(&m->pcm_eof) &&
                nv_ring_free(&m->pcm) >= m->frame_max_bytes) {
            if (nv_ring_avail(&m->ogg) < ISAAC_NV_OGG_GUARD_BYTES &&
                    !nv_load_acq(&m->ogg_eof))
                nv_topup(c, m);
            decoded = nv_decode_frame(m, 0) != 0U;
        }
        nv_release(m);
        nv_store_rel(&m->game_waiting, 0U);
        if (m->broken && nv_ring_avail(&m->pcm) == 0U)
            nv_publish_end(m);
        if (!decoded && !nv_load_acq(&m->pcm_eof) &&
                nv_ring_avail(&m->pcm) < m->frame_bytes)
            break;  /* nothing more can come from this thread (ring full) */
    }
    /* the loop also leaves through take_ready/pcm_eof while a wait is
     * pending: the worker must never stay excluded between calls */
    nv_store_rel(&m->game_waiting, 0U);
    if (sync_start)
        out->sync_us = (uint32_t)(nv_now_us() - sync_start);
    nv_signal_worker();
    out->frames = n;
    if (n < len && !out->ended && s_stats.partial_slots != UINT32_MAX)
        ++s_stats.partial_slots;
    worker_now = nv_load_acq(&m->frames_worker);
    out->worker_frames = worker_now - m->frames_worker_seen;
    m->frames_worker_seen = worker_now;
    out->sync_frames = m->frames_sync - m->frames_sync_seen;
    m->frames_sync_seen = m->frames_sync;
    out->source = out->sync_frames == 0U ? "worker"
                  : out->worker_frames == 0U ? "sync" : "mixed";
}
#else
/* Stream mode: copy `len` frames into `dst` (host or guest memory) from the
 * PCM ring, decoding on this thread whatever the worker has not produced. */
static void nv_serve_stream(CPU *__restrict c, nv_mirror *m, uint8_t *dst,
                            int32_t len, nv_serve_result *out)
{
    int32_t n = 0;
    uint32_t worker_now;
    uint64_t wait_start = 0U;

    out->wait_us = 0U;
    out->ended = 0;
    nv_topup(c, m);
    nv_signal_worker();
    while (n < len) {
        uint32_t avail = nv_ring_avail(&m->pcm) / m->frame_bytes;
        uint32_t waited = 0U;

        if (avail) {
            uint32_t take = avail;
            if ((int32_t)take > len - n)
                take = (uint32_t)(len - n);
            nv_ring_read(&m->pcm, dst + (uint32_t)n * m->frame_bytes,
                         take * m->frame_bytes);
            n += (int32_t)take;
            nv_signal_worker();
            continue;
        }
        if (nv_load_acq(&m->pcm_eof)) {
            out->ended = 1;
            break;
        }
        nv_store_rel(&m->game_waiting, 1U);
        if (!nv_try_acquire(m, NV_OWNER_GAME)) {
            /* The worker is inside a frame of this mirror.  It stops at the
             * frame boundary because game_waiting is set; meanwhile the PCM
             * it publishes is exactly what we need, so wait for either. */
            if (!wait_start)
                wait_start = nv_now_us();
            waited = (uint32_t)(nv_now_us() - wait_start);
            if (waited >= ISAAC_NV_ACQUIRE_TIMEOUT_US) {
                nv_store_rel(&m->game_waiting, 0U);
                out->wait_us += waited;
                out->ended = 1;
                isaac_vita_log(
                    "KAGE VITA NATIVE VORBIS ERR: id=%u worker held the "
                    "mirror for %u us; returning %d of %d frames",
                    (unsigned)m->id, (unsigned)waited, (int)n, (int)len);
                break;
            }
            nv_delay_us(50U);
            continue;
        }
        nv_store_rel(&m->game_waiting, 0U);
        if (wait_start) {
            waited = (uint32_t)(nv_now_us() - wait_start);
            wait_start = 0U;
            out->wait_us += waited;
            if (m->waits != UINT32_MAX)
                ++m->waits;
            if (waited > m->wait_us_max)
                m->wait_us_max = waited;
        }
        /* decode until the request can be met or the stream ends */
        while (!m->broken && !nv_load_acq(&m->pcm_eof) &&
               nv_ring_avail(&m->pcm) / m->frame_bytes <
                   (uint32_t)(len - n) &&
               nv_ring_free(&m->pcm) >= m->frame_max_bytes) {
            if (nv_ring_avail(&m->ogg) < ISAAC_NV_OGG_GUARD_BYTES &&
                    !nv_load_acq(&m->ogg_eof))
                nv_topup(c, m);
            if (!nv_decode_frame(m, 0))
                break;
        }
        nv_release(m);
        if (m->broken && nv_ring_avail(&m->pcm) == 0U)
            nv_publish_end(m);
    }
    out->frames = n;
    /* frames the worker produced since the last call, including the ones
     * it decoded ahead between calls */
    worker_now = nv_load_acq(&m->frames_worker);
    out->worker_frames = worker_now - m->frames_worker_seen;
    m->frames_worker_seen = worker_now;
    out->sync_frames = m->frames_sync - m->frames_sync_seen;
    m->frames_sync_seen = m->frames_sync;
    out->source = out->sync_frames == 0U ? "worker"
                  : out->worker_frames == 0U ? "sync" : "mixed";
}
#endif

/* Memory mode: the vendored public entry on the mirror, synchronously. */
static void nv_serve_memory(nv_mirror *m, uint8_t *dst, int channels,
                            int num_shorts, nv_serve_result *out)
{
    int n;

    out->wait_us = 0U;
    out->ended = 0;
    out->worker_frames = 0U;
    out->source = "memory";
#if NV_ASYNC
    out->sync_us = 0U;
#endif
    if (m->broken) {
        out->frames = 0;
        out->sync_frames = 0U;
        out->ended = 1;
        return;
    }
    /* The worker never selects a whole-sample mirror, but the decoder role
     * is exclusive by construction: take busy like every other decoder. */
    if (!nv_acquire_wait(m, ISAAC_NV_ACQUIRE_TIMEOUT_US, &out->wait_us)) {
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS ERR: id=%u memory mirror busy for %u us; "
            "returning 0 frames", (unsigned)m->id, (unsigned)out->wait_us);
        out->frames = 0;
        out->sync_frames = 0U;
        out->ended = 1;
        return;
    }
    s_current_mirror[0] = m;
    if (setjmp(m->assert_jmp)) {
        s_current_mirror[0] = NULL;
        m->broken = 1U;
        nv_release(m);
        out->frames = 0;
        out->sync_frames = 0U;
        out->ended = 1;
        return;
    }
    m->assert_armed = 1U;
    n = stb_vorbis_get_samples_short_interleaved(&m->state, channels,
                                                 (short *)dst, num_shorts);
    m->assert_armed = 0U;
    s_current_mirror[0] = NULL;
    if (n < 0)
        n = 0;
    out->frames = n;
    out->sync_frames = (uint32_t)n;
    m->frames_sync += (uint32_t)n;
    if (n < num_shorts / channels || m->state.eof)
        out->ended = 1;
#if NV_ASYNC && NV_RECEIPT
    /* whole samples never pass through a ring: fold the receipt here */
    m->crc_run = isaac_nv_crc32(m->crc_run, dst, (uint32_t)n * m->frame_bytes);
#endif
    nv_release(m);
}

#if NV_ASYNC
/* ASYNC: the per-slot receipt CRC is computed here, only for the slots
 * whose line is logged and after serve_us has been sampled. */
static void nv_account(nv_mirror *m, const nv_serve_result *r,
                       const uint8_t *dst, int32_t len, uint64_t t0)
#else
static void nv_account(nv_mirror *m, const nv_serve_result *r, uint32_t crc,
                       int32_t len, uint64_t t0)
#endif
{
    uint32_t slot = m->slots == UINT32_MAX ? UINT32_MAX : ++m->slots;
    uint64_t now = nv_now_us();
    uint32_t serve_us = (uint32_t)(now - t0);

    (void)slot;

    m->frames_out += (uint32_t)r->frames;
    m->last_call_us = now;
    s_stats.io_us_total += m->io_us;
    if (m->io_us > s_stats.io_us_max)
        s_stats.io_us_max = m->io_us;
    if (s_stats.slots != UINT32_MAX)
        ++s_stats.slots;
    s_stats.frames += (uint32_t)r->frames;
    s_stats.frames_worker += r->worker_frames;
    s_stats.frames_sync += r->sync_frames;
    if (r->wait_us) {
        if (s_stats.waits != UINT32_MAX)
            ++s_stats.waits;
        if (r->wait_us > s_stats.wait_us_max)
            s_stats.wait_us_max = r->wait_us;
    }
    if (m->underrun && m->underruns != UINT32_MAX) {
        ++m->underruns;
        ++s_stats.underruns;
        m->underrun = 0U;
    }
    if (m->assert_line) {
        if (s_stats.asserts != UINT32_MAX)
            ++s_stats.asserts;
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS ASSERT: id=%u f=%08x stb_vorbis.c:%u "
            "'%s' (mirror ends the stream)", (unsigned)m->id,
            (unsigned)m->guest_f, (unsigned)m->assert_line,
            m->assert_expr ? m->assert_expr : "?");
        m->assert_line = 0U;
    }
#if NV_RECEIPT
    if (slot <= ISAAC_NV_RECEIPT_SLOTS_DENSE || nv_power_of_two(slot) ||
            r->ended || (slot & 0x3fU) == 0U) {
#if NV_ASYNC
        uint32_t crc = isaac_nv_crc32(0U, dst,
                                      (uint32_t)r->frames * m->frame_bytes);

        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS SLOT: id=%u n=%u req=%d got=%d "
            "ended=%d crc=%08x run=%08x src=%s wait_us=%u serve_us=%u "
            "io_us=%u pcm_ahead=%u ogg_ahead=%u ogg_eof=%u sync_frames=%u "
            "worker_frames=%u pool=%u sync_us=%u",
            (unsigned)m->id, (unsigned)slot, (int)len, (int)r->frames,
            r->ended, (unsigned)crc, (unsigned)m->crc_run, r->source,
            (unsigned)r->wait_us, (unsigned)serve_us, (unsigned)m->io_us,
            (unsigned)(m->mode == NV_MODE_STREAM
                           ? nv_ring_avail(&m->pcm) / m->frame_bytes : 0U),
            (unsigned)(m->mode == NV_MODE_STREAM ? nv_ring_avail(&m->ogg) : 0U),
            (unsigned)nv_load_acq(&m->ogg_eof), (unsigned)r->sync_frames,
            (unsigned)r->worker_frames, (unsigned)nv_pool_live(),
            (unsigned)r->sync_us);
#else
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS SLOT: id=%u n=%u req=%d got=%d "
            "ended=%d crc=%08x run=%08x src=%s wait_us=%u serve_us=%u "
            "io_us=%u pcm_ahead=%u ogg_ahead=%u ogg_eof=%u sync_frames=%u "
            "worker_frames=%u pool=%u",
            (unsigned)m->id, (unsigned)slot, (int)len, (int)r->frames,
            r->ended, (unsigned)crc, (unsigned)m->crc_run, r->source,
            (unsigned)r->wait_us, (unsigned)serve_us, (unsigned)m->io_us,
            (unsigned)(m->mode == NV_MODE_STREAM
                           ? nv_ring_avail(&m->pcm) / m->frame_bytes : 0U),
            (unsigned)(m->mode == NV_MODE_STREAM ? nv_ring_avail(&m->ogg) : 0U),
            (unsigned)nv_load_acq(&m->ogg_eof), (unsigned)r->sync_frames,
            (unsigned)r->worker_frames, (unsigned)nv_pool_live());
#endif
    }
    if ((s_stats.slots & 0x3fU) == 0U)
        nv_log_stats();
#else
    (void)serve_us;
#if NV_ASYNC
    (void)dst;
#else
    (void)crc;
#endif
    (void)len;
#endif
}

#if !defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
/* Write the observable end-of-stream fields back into the guest state, as
 * the translated decoder would have left them. */
static void nv_writeback(nv_mirror *m, const nv_serve_result *r)
{
    if (!r->ended)
        return;
    st32(m->guest_f + ISAAC_NV_STATE_EOF_OFFSET, 1U);
    if (m->mode == NV_MODE_STREAM) {
        uint32_t err = nv_load_acq(&m->pcm_error);
        if (err)
            st32(m->guest_f + ISAAC_NV_STATE_EOF_OFFSET + 4U, err);
    } else if (m->state.error) {
        st32(m->guest_f + ISAAC_NV_STATE_EOF_OFFSET + 4U,
             (uint32_t)m->state.error);
    }
}
#endif

/* ------------------------------------------------------------------------
 * The wrapped guest entries. */
void __real_sub_005bdf80(CPU *__restrict c);
void __real_sub_005b7740(CPU *__restrict c);
void __real_sub_005bb260(CPU *__restrict c);
void __real_sub_005bd310(CPU *__restrict c);
static void nv_call_real(CPU *__restrict c, uint32_t eax, uint32_t ecx,
                         uint32_t edx);
static void nv_retire(nv_mirror *m, const char *reason);

static nv_mirror *nv_resolve(CPU *__restrict c, uint32_t f, int channels,
                             const char **reason)
{
    nv_mirror *m = nv_lookup(f);
    const stb_vorbis *g = (const stb_vorbis *)(uintptr_t)f;

    if (m) {
        int stale = !nv_identity_holds(m, g);
        int mismatch = m->channels_arg != channels;

        if (m->dirty || stale || mismatch) {
            /* The guest itself moved the stream (seek_start/pump) or opened
             * a new state at this address: its position is authoritative.
             * Otherwise the state is still frozen at the clone position and
             * the stream rests at the read-ahead head; put it back before
             * the re-clone's tell, or before the translated body runs on a
             * quiesce failure. */
            if (!m->dirty && !stale)
                (void)nv_guest_sync(c, m);
            if (!nv_quiesce(m)) {
                *reason = "quiesce";
                return NULL;
            }
            /* busy held; identity/rings are rebuilt by nv_start */
            m->phys_valid = 0U;
            if (stale) {
                nv_log_close(m, "reopened");
                nv_store_rel(&m->active, 0U);
                m->guest_f = 0U;
                m->mode = NV_MODE_NONE;
                nv_ring_reset(&m->ogg);
                nv_ring_reset(&m->pcm);
                nv_store_rel(&m->generation, m->generation + 1U);
                nv_release(m);
                nv_store_rel(&m->retiring, 0U);
                return nv_start(c, NULL, f, channels, reason, NULL);
            }
            return nv_start(c, m, f, channels, reason,
                            m->dirty ? "RECLONE-seek" : "RECLONE-channels");
        }
        return m;
    }
    return nv_start(c, NULL, f, channels, reason, NULL);
}

#if defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
static uint8_t *s_verify_buffer;
static uint32_t s_verify_capacity;

static void nv_verify(CPU *__restrict c, nv_mirror *m, uint32_t f,
                      uint32_t buffer, int32_t len, int32_t n_native,
                      uint32_t crc_native, const nv_serve_result *r,
                      uint32_t entry_eax, uint32_t channels)
{
    int32_t n_real;
    uint32_t real_eax;
    uint32_t crc_real;
    uint32_t common;
    uint32_t diff = 0U;
    uint32_t maxabs = 0U;
    int32_t first = -1;
    const int16_t *a;
    const int16_t *b;
    uint32_t i;
    const char *result;

    if (m->mode == NV_MODE_STREAM)
        (void)nv_guest_seek(c, m, m->guest_logical_pos);
    nv_call_real(c, entry_eax, f, channels);
    real_eax = c->eax;
    n_real = (int32_t)real_eax;
    if (m->mode == NV_MODE_STREAM) {
        uint32_t pos = 0U;
        m->phys_valid = 0U;
        if (nv_guest_tell(c, m, &pos)) {
            m->guest_logical_pos = pos;
            m->phys_valid = 1U;
            m->phys_pos = pos;
        }
    }
    /* the guest reads the translated frame count from eax */
    c->eax = real_eax;
    crc_real = isaac_nv_crc32(0U, (const void *)(uintptr_t)buffer,
                              (uint32_t)(n_real > 0 ? n_real : 0) *
                                  m->frame_bytes);
    common = (uint32_t)(n_native < n_real ? n_native : n_real);
    if (n_native < 0)
        common = 0U;
    a = (const int16_t *)s_verify_buffer;
    b = (const int16_t *)(uintptr_t)buffer;
    for (i = 0U; i < common * (uint32_t)m->channels_arg; ++i) {
        if (a[i] != b[i]) {
            int32_t d = (int32_t)a[i] - (int32_t)b[i];
            if (d < 0)
                d = -d;
            if (first < 0)
                first = (int32_t)i;
            ++diff;
            if ((uint32_t)d > maxabs)
                maxabs = (uint32_t)d;
        }
    }
    result = (n_native == n_real && diff == 0U) ? "MATCH"
             : (n_native == n_real ? "DIFF" : "COUNT");
    if (m->verify_slots != UINT32_MAX)
        ++m->verify_slots;
    if (strcmp(result, "MATCH") != 0) {
        if (m->verify_diff_slots != UINT32_MAX)
            ++m->verify_diff_slots;
        if (s_stats.verify_diff_slots != UINT32_MAX)
            ++s_stats.verify_diff_slots;
    }
    m->verify_diff_samples += diff;
    if (maxabs > m->verify_maxabs)
        m->verify_maxabs = maxabs;
    if (m->verify_slots <= 8U || strcmp(result, "MATCH") != 0 ||
            nv_power_of_two(m->verify_slots)) {
        if (m->verify_lines < ISAAC_NV_VERIFY_LINES_PER_MIRROR ||
                nv_power_of_two(m->verify_slots)) {
            ++m->verify_lines;
            isaac_vita_log(
                "KAGE VITA NATIVE VORBIS VERIFY: id=%u n=%u req=%d native=%d "
                "translated=%d crc_n=%08x crc_t=%08x diff=%u maxabs=%u "
                "first=%d src=%s result=%s mirror_diff_slots=%u "
                "mirror_diff_samples=%u",
                (unsigned)m->id, (unsigned)m->slots, (int)len, (int)n_native,
                (int)n_real, (unsigned)crc_native, (unsigned)crc_real,
                (unsigned)diff, (unsigned)maxabs, (int)first, r->source,
                result, (unsigned)m->verify_diff_slots,
                (unsigned)m->verify_diff_samples);
        }
    }
}
#endif

/* The guest stream calls made while resolving a mirror clobber the caller-
 * saved registers, so the translated body must always see the entry values
 * of ecx (f) and edx (channels). */
static void nv_call_real(CPU *__restrict c, uint32_t eax, uint32_t ecx,
                         uint32_t edx)
{
    c->eax = eax;
    c->ecx = ecx;
    c->edx = edx;
    __real_sub_005bdf80(c);
}

/* ASYNC: a worker timeout is a dropped mirror, never an end of stream. */
#if NV_ASYNC
#define NV_TIMED_OUT(m) ((m)->timed_out != 0U)
#else
#define NV_TIMED_OUT(m) 0
#endif

void __wrap_sub_005bdf80(CPU *__restrict c)
{
    uint32_t entry_eax = c->eax;
    uint32_t f = c->ecx;
    int32_t channels = (int32_t)c->edx;
    uint32_t buffer;
    int32_t num_shorts;
    int32_t len;
    nv_mirror *m;
    const char *reason = "?";
    nv_serve_result r;
    uint64_t t0;
#if !NV_ASYNC
    uint32_t crc;
#endif
    uint8_t *dst;

    if (!f || (f & 3U) || channels <= 0 || channels > STB_VORBIS_MAX_CHANNELS) {
        if (f && channels > 0)
            nv_log_fallback(f, "arguments");
        __real_sub_005bdf80(c);
        return;
    }
    buffer = ld32(guest_stack_address(c, c->esp + 4U, 4U,
                                      ISAAC_NV_GET_SAMPLES_RVA));
    num_shorts = (int32_t)ld32(guest_stack_address(
        c, c->esp + 8U, 4U, ISAAC_NV_GET_SAMPLES_RVA));
    len = num_shorts / channels;
    if (!buffer || (buffer & 1U) || len <= 0) {
        __real_sub_005bdf80(c);
        return;
    }
    t0 = nv_now_us();
    m = nv_resolve(c, f, channels, &reason);
    if (!m) {
        nv_log_fallback(f, reason);
        nv_call_real(c, entry_eax, f, (uint32_t)channels);
        return;
    }
#if defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
    if (!nv_grow(&s_verify_buffer, &s_verify_capacity,
                 (uint32_t)len * m->frame_bytes)) {
        nv_log_fallback(f, "verify-memory");
        nv_call_real(c, entry_eax, f, (uint32_t)channels);
        return;
    }
    dst = s_verify_buffer;
#else
    dst = (uint8_t *)(uintptr_t)buffer;
#endif
    m->io_us = 0U;
    if (m->mode == NV_MODE_STREAM)
        nv_serve_stream(c, m, dst, len, &r);
    else
        nv_serve_memory(m, dst, channels, num_shorts, &r);
#if NV_ASYNC
    nv_account(m, &r, dst, len, t0);
#else
    crc = isaac_nv_crc32(0U, dst, (uint32_t)r.frames * m->frame_bytes);
    m->crc_run = isaac_nv_crc32(m->crc_run, dst,
                                (uint32_t)r.frames * m->frame_bytes);
    nv_account(m, &r, crc, len, t0);
#endif
    /* A mirror that produced nothing although the stream has not ended, or
     * that broke (assertion, underrun, worker timeout), is not an end of
     * stream: drop it and let the translated decoder continue from the
     * guest state, which is still exactly where the mirror was cloned. */
    if (r.frames == 0 && r.ended &&
            (m->broken || NV_TIMED_OUT(m) ||
             (m->mode == NV_MODE_STREAM && !nv_load_acq(&m->ogg_eof)) ||
             (m->mode == NV_MODE_MEMORY && !m->state.eof))) {
        if (s_stats.zero_fallbacks != UINT32_MAX)
            ++s_stats.zero_fallbacks;
        (void)nv_guest_sync(c, m);
        nv_retire(m, "native-zero");
        nv_log_fallback(f, "native-zero");
        nv_call_real(c, entry_eax, f, (uint32_t)channels);
        return;
    }
    /* Between calls the KAGE stream stays where the read-ahead left it: the
     * next top-up continues forward without a seek.  The guest state is
     * frozen at the clone position and gets the stream back there only at a
     * hand-off (nv_guest_sync: fallback, re-clone, eviction, pump), never
     * per slot -- see nv_guest_sync for the measured cost of doing so.  The
     * translated Decode (sub_005a20e0) never asks the stream where it is:
     * its virtual calls go to the lock at this+0x9c and to its own vtable. */
#if defined(ISAAC_VITA_NATIVE_VORBIS_VERIFY)
    /* The translated body pops the return word and sets eax itself. */
    nv_verify(c, m, f, buffer, len, r.frames, crc, &r, entry_eax,
              (uint32_t)channels);
#else
    nv_writeback(m, &r);
    c->eax = (uint32_t)r.frames;
    (void)gpop(c);
#endif
}

void isaac_nv_guest_buffer_freed(void *pointer)
{
    uint32_t address = (uint32_t)(uintptr_t)pointer;
    uint32_t i;

    if (!address)
        return;
    for (i = 0U; i < ISAAC_NV_MIRRORS_MAX; ++i) {
        nv_mirror *m = &s_mirrors[i];

        if (!m->active)
            continue;
        /* the freed block is the mirror's alloc buffer, or it contains
         * the state (StreamSourceOgg frees the 0x4b000 buffer that holds
         * every table and the state itself) */
        if (m->guest_alloc_buffer == address ||
                (address - m->guest_alloc_buffer) < m->guest_alloc_len) {
            if (s_stats.freed_retires != UINT32_MAX)
                ++s_stats.freed_retires;
            nv_retire(m, "freed");
        }
    }
}

/* set_file_offset(f = ecx, loc = edx): Decode's inlined seek_start. */
void __wrap_sub_005b7740(CPU *__restrict c)
{
    nv_mirror *m = nv_lookup(c->ecx);

    if (m) {
        m->dirty = 1U;
        m->phys_valid = 0U;
    }
    __real_sub_005b7740(c);
}

/* vorbis_pump_first_frame(f = ecx). */
void __wrap_sub_005bb260(CPU *__restrict c)
{
    nv_mirror *m = nv_lookup(c->ecx);

    if (m) {
        /* seek_start pumps right after set_file_offset (dirty already set,
         * the guest's own seek is authoritative).  A pump without that seek
         * decodes from where the state stands, not from the read-ahead. */
        if (!m->dirty)
            (void)nv_guest_sync(c, m);
        m->dirty = 1U;
        m->phys_valid = 0U;
    }
    __real_sub_005bb260(c);
}

/* vorbis_deinit(f = ecx). */
void __wrap_sub_005bd310(CPU *__restrict c)
{
    nv_mirror *m = nv_lookup(c->ecx);

    if (m)
        nv_retire(m, "deinit");
    __real_sub_005bd310(c);
}

#if NV_ASYNC
void __real_sub_005bd740(CPU *__restrict c);

/* stb_vorbis_open_file(stream = ecx, [ebp+8] = int *error, [ebp+0xc] =
 * stb_vorbis_alloc *), StreamSourceOgg::Open at 0x5a2346 / 0x5a240e; eax =
 * the state or 0, the caller cleans the two words.  The translated body runs
 * first and unchanged.  Its result is cloned right here and the OGG ring is
 * primed, so the worker has the whole gap until the first Decode (the next
 * loop's service phase) to get ahead.  The layout Decode will request is
 * not known yet; every OPEN receipt measured arg_ch == ch, and a different
 * argument simply re-clones (RECLONE-channels).  A guest seek before the
 * first Decode goes through the wrapped set_file_offset (RECLONE-seek). */
void __wrap_sub_005bd740(CPU *__restrict c)
{
    uint32_t f;
    uint32_t ecx;
    uint32_t edx;
    const stb_vorbis *g;
    const char *reason = "?";
    nv_mirror *m;
    uint64_t t0;
    uint32_t i;

    __real_sub_005bd740(c);
    f = c->eax;
    if (c->fault || !f || (f & 3U))
        return;
    g = (const stb_vorbis *)(uintptr_t)f;
    if (g->channels <= 0 || g->channels > STB_VORBIS_MAX_CHANNELS ||
            g->stream || nv_lookup(f))
        return;
    /* The guest stream calls below (tell/seek/read) return through eax and
     * clobber ecx/edx; the caller stores eax as the state pointer right
     * after the call (`mov [esi+0x90], eax`), so the translated body's
     * result registers are restored before returning. */
    ecx = c->ecx;
    edx = c->edx;
    t0 = nv_now_us();
    m = nv_start(c, NULL, f, g->channels, &reason, NULL);
    if (!m) {
        /* the first Decode tries again and logs the fallback if it must */
        (void)reason;
        if (s_stats.early_rejected != UINT32_MAX)
            ++s_stats.early_rejected;
    } else {
        m->io_us = 0U;
        for (i = 0U; i < ISAAC_NV_EARLY_TOPUP_CHUNKS; ++i)
            nv_topup(c, m);
        nv_signal_worker();
        if (s_stats.early_clones != UINT32_MAX)
            ++s_stats.early_clones;
#if NV_RECEIPT
        isaac_vita_log(
            "KAGE VITA NATIVE VORBIS EARLY: id=%u f=%08x ogg_ahead=%u "
            "ogg_eof=%u leftover=%u io_us=%u total_us=%u",
            (unsigned)m->id, (unsigned)f, (unsigned)nv_ring_avail(&m->ogg),
            (unsigned)nv_load_acq(&m->ogg_eof), (unsigned)m->leftover_frames,
            (unsigned)m->io_us, (unsigned)(nv_now_us() - t0));
#else
        (void)t0;
#endif
    }
    c->eax = f;
    c->ecx = ecx;
    c->edx = edx;
}
#endif

/* ------------------------------------------------------------------------
 * Oracle seam. */
#if defined(ISAAC_VITA_NATIVE_VORBIS_ORACLE)
/* The oracle runs on an LP64 host where the raw guest offsets do not apply;
 * these helpers poke the fields through the real struct. */
uint32_t isaac_nv_oracle_to_stream_mode(void *state, uint32_t stream_object)
{
    stb_vorbis *v = (stb_vorbis *)state;
    uint32_t consumed = (uint32_t)(v->stream - v->stream_start);

    v->stream = NULL;
    v->stream_start = NULL;
    v->stream_end = NULL;
    v->f = (FILE *)(uintptr_t)stream_object;
    v->f_start = 0U;
    v->close_on_free = 1;
    return consumed;
}

uint32_t isaac_nv_oracle_first_page(const void *state)
{
    return ((const stb_vorbis *)state)->first_audio_page_offset;
}

void isaac_nv_oracle_set_push_mode(void *state, int on)
{
    ((stb_vorbis *)state)->push_mode = (uint8)(on ? 1 : 0);
}

void *isaac_nv_oracle_swap_alloc_buffer(void *state, void *buffer)
{
    stb_vorbis *v = (stb_vorbis *)state;
    void *old = v->alloc.alloc_buffer;

    v->alloc.alloc_buffer = (char *)buffer;
    return old;
}

uint32_t isaac_nv_oracle_state_bytes(void)
{
    return (uint32_t)sizeof(stb_vorbis);
}

/* seek_start+pump rewrites the decode fields but never the marker slot. */
int isaac_nv_oracle_seek(void *state, uint32_t sample)
{
    return stb_vorbis_seek((stb_vorbis *)state, sample);
}

/* Emulate a state that expects a page boundary next (what a translated
 * seek_start leaves behind) so a misplaced stream fails immediately. */
void isaac_nv_oracle_force_page_resync(void *state)
{
    ((stb_vorbis *)state)->next_seg = -1;
}

uint32_t isaac_nv_oracle_marker_get(const void *state)
{
    return ((const stb_vorbis *)state)->setup_temp_memory_required;
}

void isaac_nv_oracle_marker_set(void *state, uint32_t marker)
{
    ((stb_vorbis *)state)->setup_temp_memory_required = marker;
}

void isaac_nv_oracle_stats_get(isaac_nv_oracle_stats *out)
{
    memset(out, 0, sizeof *out);
    out->mirrors_created = s_stats.mirrors_created;
    out->clone_rejected = s_stats.clone_rejected;
    out->fallbacks = s_stats.fallbacks;
    out->frames_worker = s_stats.frames_worker;
    out->frames_sync = s_stats.frames_sync;
    out->waits = s_stats.waits;
    out->reclones = s_stats.reclones;
    out->evictions = s_stats.evictions;
    out->asserts = s_stats.asserts;
    out->underruns = s_stats.underruns;
    out->worker_refused = nv_load_acq(&s_worker_refused);
    out->freed_retires = s_stats.freed_retires;
    out->zero_fallbacks = s_stats.zero_fallbacks;
    out->frames = s_stats.frames;
    out->slots = s_stats.slots;
#if NV_ASYNC
    out->partial_slots = s_stats.partial_slots;
    out->budget_hits = s_stats.budget_hits;
    out->early_clones = s_stats.early_clones;
    out->early_rejected = s_stats.early_rejected;
#endif
}

void isaac_nv_oracle_set_fake_clock(int on, uint32_t step_us)
{
    if (on && !s_oracle_fake_clock_on)
        s_oracle_fake_now = nv_now_us();
    s_oracle_fake_step_us = step_us;
    s_oracle_fake_clock_on = on ? 1U : 0U;
}

int32_t isaac_nv_oracle_pcm_ahead_frames(uint32_t f)
{
    nv_mirror *m = nv_lookup(f);

    if (!m || m->mode != NV_MODE_STREAM || !m->frame_bytes)
        return -1;
    return (int32_t)(nv_ring_avail(&m->pcm) / m->frame_bytes);
}

void isaac_nv_oracle_set_worker_frame_delay_us(uint32_t us)
{
    nv_store_rel(&s_oracle_worker_frame_delay_us, us);
}

/* 0 = free, 1 = game thread, 2 = worker; -1 = no mirror for f */
int isaac_nv_oracle_mirror_busy(uint32_t f)
{
    nv_mirror *m = nv_lookup(f);

    return m ? (int)nv_load_acq(&m->busy) : -1;
}

uint32_t isaac_nv_oracle_worker_refused(void)
{
    return nv_load_acq(&s_worker_refused);
}

int isaac_nv_oracle_worker_start(void)
{
    int started = nv_worker_start();

    s_stats.init_logged = 0U;
    nv_log_init();
    return started;
}

void isaac_nv_oracle_worker_stop(void)
{
    if (!s_worker_thread_valid)
        return;
    nv_store_rel(&s_worker_stop, 1U);
    nv_signal_worker();
    pthread_join(s_worker_thread, NULL);
    s_worker_thread_valid = 0;
    nv_store_rel(&s_worker_stop, 0U);
    s_worker_start_attempted = 0U;
}
#endif
