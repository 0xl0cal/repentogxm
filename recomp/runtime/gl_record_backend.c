/* Typed-shim recorder. Inline mode: same-thread drains (stage 1).
 * ISAAC_VITA_GL_THREAD_WORKER: one consumer thread owns every replayed call
 * (host stage 2 only; the device worker is not wired). No allocator. */
#include "gl_record_backend.h"
#include "gl_record_ring.h"
#include "gl_record_locations.h"
#include "gl_owner_check.h"
#include <string.h>

#if !defined(ISAAC_VITA_GL_THREAD_RECORD) || !ISAAC_VITA_GL_THREAD_RECORD
#error Compile the recorder only when explicitly enabled
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__vita__)
#include <psp2/kernel/sysmem.h>
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
# ifndef ISAAC_GL_RENDER_THREAD_PRIORITY
/* The default user priority every other runtime worker uses (0x10000100,
 * e.g. host_vita_async_write.c:54); the game thread runs at the same level. */
#  define ISAAC_GL_RENDER_THREAD_PRIORITY 0x10000100
# endif
# ifndef ISAAC_GL_RENDER_THREAD_STACK
/* shortcut: 2 MiB without a measured high-water; the start-up/present
 * receipts log free stack so a session can size it. */
#  define ISAAC_GL_RENDER_THREAD_STACK (2u * 1024u * 1024u)
# endif
#endif
#else
#error Stage-1 range admission is implemented only for the Windows oracle and Vita
#endif

static IsaacGlRecordRing s_ring;
static IsaacGlRecordLocations s_locations;
static guest_gl_backend s_target;
static IsaacGlRecordRboNative s_rbo;
static uint32_t s_result, s_rva, s_syncs, s_location_cold;
/* NO thread-local storage: on the device __thread compiles to libgcc emutls,
 * whose __gthread_active_p() is a weak symbol resolved to 0 in this link, so
 * emutls takes its single-thread path and ONE copy is shared by every thread
 * (measured on int4-glthread-proof-20261013: the render thread's flag made the
 * game thread run init/present closures inline).  "Am I the render thread"
 * is an identity compare instead: TPIDRURO on the device, the thread id on
 * the host.  s_replaying is written by the consumer only and is meaningful
 * only together with that identity. */
static int s_active, s_rbo_sync;
static int s_replaying;
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
static volatile uint32_t s_render_identity;  /* 0 until the render thread runs */
static uint32_t record_thread_identity(void)
{
# if defined(_WIN32)
    return (uint32_t)GetCurrentThreadId();
# else
    uint32_t value;
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
# endif
}
static int record_on_consumer(void)
{
# if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_SHARED_FLAG)
    /* oracle self-test: the device's emutls behaviour (one flag, all threads) */
    return s_render_identity != 0u;
# else
#  if defined(_WIN32)
    uint32_t id = s_render_identity;
#  else
    uint32_t id = __atomic_load_n(&s_render_identity, __ATOMIC_ACQUIRE);
#  endif
    return id != 0u && id == record_thread_identity();
# endif
}
#else
static int record_on_consumer(void) { return 1; } /* inline: one thread */
#endif
static int record_replaying(void) { return s_replaying && record_on_consumer(); }
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
# if defined(ISAAC_GL_RECORD_ORACLE_EAGER)
#  error "the worker has no eager oracle layer"
# endif
static uint32_t s_last_pos;           /* producer: end of its last record */
static volatile uint32_t s_worker_stop;
static int s_worker;                  /* started (host or device) */
static int s_worker_thread_id;        /* resolved render thread id, for logs */
static void (*s_worker_init)(void);

/* ph120.rt: s_pw is producer-owned, s_rw render-thread-owned. */
static struct {
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    /* ph120.ro, game thread: the admission-check clock of the next sampled
     * append (0 = none), the sampler (LCG state, appends left until the
     * next sampled one) and the opcode of the last append (record_sync
     * charges its wait to it). */
    uint64_t census_stamp;
    uint32_t census_lcg, census_left, census_last_op;
#endif
    uint32_t syncs, sync_waits, sync_wait_us, held, held_us, call_waits, call_wait_us;
    uint32_t records_base, ring_full_base, wakes_base, copied_base;
    /* ph120.rw: memory admission calls (1/64 timed) and appends (1/64 timed) */
    uint32_t mb_calls, mb_samples, mb_us, mb_queries, ap_tick, ap_samples, ap_us;
    uint32_t clock_us;      /* the clock pair's own cost on the same samples */
    uint32_t render_skips;  /* interpolated renders declined: render thread behind */
} s_pw;
#define RECORD_SAMPLE_MASK 63u
static struct {
    uint64_t busy_us, busy_since, frame_busy_mark, window_start, window_busy_mark;
    uint32_t busy, frames, frame_overflow, swap_us, sleeps_base, timeouts_base;
    uint32_t frame_us[ISAAC_GL_RECORD_FRAME_SAMPLES];
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    uint32_t census_lcg, census_left;   /* ph120.ro: random replay timing */
#endif
} s_rw;
static uint64_t record_now_us(void)
{
# if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)(counter.QuadPart / (frequency.QuadPart / 1000000));
# else
    return sceKernelGetProcessTimeWide();
# endif
}
/* Producer: wait for `position`; count only waits that actually block. */
static void record_wait(uint32_t position, uint32_t *count, uint32_t *us)
{
    uint64_t started;
    if (isaac_gl_record_spsc_reached(&s_ring, position)) return;
    started = record_now_us();
    isaac_gl_record_spsc_wait_done(&s_ring, position);
    ++*count;
    *us += (uint32_t)(record_now_us() - started);
}
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
/* ph120.ro live counts: s_census_produce is game-thread-owned,
 * s_census_replay render-thread-owned; s_census is the window the last
 * take closed (each half fills its own fields inside the take). */
typedef struct RecordCensusProduce { uint32_t ps, pst, pus, sy, sw, swus; } RecordCensusProduce;
typedef struct RecordCensusReplay { uint32_t n, b, e, eus, rs, rus; } RecordCensusReplay;
static RecordCensusProduce s_census_produce[ISAAC_GL_RECORD_CENSUS_OPS];
static RecordCensusReplay s_census_replay[ISAAC_GL_RECORD_CENSUS_OPS];
static IsaacGlRecordOpcodeCensus s_census;
/* Sampler: every event counts *left down; the one that finds it at 1 (or
 * 0, the static start) is sampled and draws the next gap, uniform 1..127
 * (mean 64, so P = 1/64), from the Numerical Recipes LCG.  Unlike the
 * ap_tick stride it cannot lock onto a periodic command sequence (a stride
 * of 64 against a frame of 16 draws times one draw slot only), and unlike
 * an LCG step per event an unsampled event costs one decrement, the same
 * as ap_tick.  RECORD_CENSUS_NEXT(left): the next event is sampled. */
#  define RECORD_CENSUS_LCG(x) ((x) * 1664525u + 1013904223u)
#  define RECORD_CENSUS_GAP(x) (1u + ((((x) >> 16) * 127u) >> 16))
#  define RECORD_CENSUS_NEXT(left) ((left) <= 1u)
static inline int record_census_tick(uint32_t *left, uint32_t *lcg)
{
    if (!RECORD_CENSUS_NEXT(*left)) {
        --*left;
        return 0;
    }
    *lcg = RECORD_CENSUS_LCG(*lcg);
    *left = RECORD_CENSUS_GAP(*lcg);
    return 1;
}
static uint32_t record_census_slot(uint32_t opcode)
{
    return opcode < ISAAC_GL_RECORD_CENSUS_OPS ? opcode : 0u;
}
/* Game thread, after one S wait (or one closure call for REC_WORKER_CALL). */
static void record_census_wait(uint32_t opcode, uint32_t waits, uint32_t us)
{
    RecordCensusProduce *p = &s_census_produce[record_census_slot(opcode)];
    ++p->sy;
    p->sw += waits;
    p->swus += us;
}
# endif
#endif
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS) && \
    !(defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER)
#error "the opcode census counts render-thread replays (GL_THREAD_WORKER)"
#endif

/* Constants copied from gl_vita_backend.c / the patched framebuffers.c contract. */
#define RECORD_FRAMEBUFFER 0x8d40u
#define RECORD_DRAW_FRAMEBUFFER 0x8ca9u
#define RECORD_COLOR_ATTACHMENT0 0x8ce0u
#define RECORD_TEXTURE_2D 0x0de1u
#define RECORD_FRAMEBUFFER_COMPLETE 0x8cd5u
#define RECORD_STATUS_CAPACITY 64u
typedef struct RecordStatus { uint32_t name, complete; } RecordStatus;
static RecordStatus s_status[RECORD_STATUS_CAPACITY];
static uint32_t s_bound_fbo;
static int s_bound_fbo_known;

static void record_dispatch(const IsaacGlRecordHeader *, const void *);
static void record_glBindRenderbuffer(guest_gl_enum, guest_gl_uint);
static void record_glDeleteRenderbuffers(guest_gl_sizei, guest_gl_addr);
static void record_glGenRenderbuffers(guest_gl_sizei, guest_gl_addr);
static void record_glRenderbufferStorage(guest_gl_enum, guest_gl_enum, guest_gl_sizei, guest_gl_sizei);
static void record_glGetRenderbufferParameteriv(guest_gl_enum, guest_gl_enum, guest_gl_addr);
static guest_gl_int record_glGetAttribLocation(guest_gl_uint, guest_gl_addr);
static guest_gl_int record_glGetUniformLocation(guest_gl_uint, guest_gl_addr);
static guest_gl_enum record_glCheckFramebufferStatus(guest_gl_enum);

static GUEST_NORETURN void record_fault(const char *message)
{
    guest_gl_backend_fault(0u, message);
}

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER && defined(__vita__)
/* Memory admission cache (producer only).  The last block that answered
 * readable is kept with the free generation read BEFORE its query; any
 * sceKernelFreeMemBlock in this link (the one --wrap: the debug overlay's,
 * or gl_record_free_wrap.c with GL_RECORD_ADMISSION_CACHE_STANDALONE;
 * --wrap reaches vitaGL and every static library) bumps the generation
 * first.  The premise, stated narrowly: cached entries come only from the
 * producer's own client spans (guest heap, guest stack, image), and those
 * blocks stop being mapped only through a wrapped free, so a hit gives the
 * answer the query would give.  Unmaps that bypass the wrap (a deleted
 * thread's stack, frees inside system PRXs) are invisible to the import
 * census and are outside the premise.  Negative answers are never cached. */
static volatile uint32_t s_memblock_generation;
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE)
/* Four blocks: client spans alternate between the guest stack, the heap and
 * the image (ab2 on the device: 17 % misses with one entry). */
#  define RECORD_MB_CACHE_ENTRIES 4u
static struct { uint32_t base, size, generation, valid; } s_mb_cache[RECORD_MB_CACHE_ENTRIES];
static uint32_t s_mb_next;
# endif
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
#  if !defined(ISAAC_GL_RECORD_ADMISSION_CACHE)
#   error "GL_RECORD_ADMISSION_CACHE_VERIFY checks the cache: CMake defines both"
#  endif
/* VERIFY (measurement builds): producer-side counts since the boot self-test.
 * misses = queries the production build would make; hits = cache hits that
 * were re-queried and agreed; generation0 = the free generation then. */
static struct { uint32_t misses, hits, generation0; } s_mb_verify;
# endif
void isaac_gl_record_memblock_freeing(void)
{
    __atomic_add_fetch(&s_memblock_generation, 1u, __ATOMIC_SEQ_CST);
}
#endif
static int record_readable_query(uint32_t address, uint32_t bytes);
static int record_readable(uint32_t address, uint32_t bytes)
{
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    /* Every check is followed by its wrapper's append, with no other append
     * between: when that append will be sampled, its span starts here. */
    if (RECORD_CENSUS_NEXT(s_pw.census_left) && !s_pw.census_stamp &&
            !record_on_consumer())
        s_pw.census_stamp = record_now_us();
# endif
    if (bytes && (++s_pw.mb_calls & RECORD_SAMPLE_MASK) == 0u) {
        uint64_t started = record_now_us();
        int ok = record_readable_query(address, bytes);
        s_pw.mb_us += (uint32_t)(record_now_us() - started);
        ++s_pw.mb_samples;
        return ok;
    }
#endif
    return record_readable_query(address, bytes);
}

static int record_readable_query(uint32_t address, uint32_t bytes)
{
    if (!bytes) return 1;
    if (!address || address > UINT32_MAX - (bytes - 1u)) return 0;
#if defined(_WIN32)
    {
        uintptr_t cursor = address, end = cursor + bytes;
# if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
        ++s_pw.mb_queries;   /* the host has no cache: every call queries */
# endif
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info;
            uintptr_t limit;
            if (!VirtualQuery((const void *)cursor, &info, sizeof info) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                  PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
                return 0;
            limit = (uintptr_t)info.BaseAddress + info.RegionSize;
            if (limit <= cursor) return 0;
            cursor = limit < end ? limit : end;
        }
        return 1;
    }
#else
    {
        /* ByAddr/full-span/access rules copied from host_vita_memory.c:127..165.
         * No claim about the cost of this new record-side admission syscall. */
        SceKernelMemBlockInfo info = {0};
        uintptr_t base;
        int readable;
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE)
        /* Only with a free wrap linked (CMake); else every call queries. */
        int cacheable = !record_on_consumer();
        uint32_t generation = __atomic_load_n(&s_memblock_generation, __ATOMIC_SEQ_CST);
#  if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
        int hit = 0;   /* VERIFY: a hit still queries, and must agree */
#  endif
        if (cacheable) {
            uint32_t i;
            for (i = 0u; i < RECORD_MB_CACHE_ENTRIES; ++i)
                if (s_mb_cache[i].valid && s_mb_cache[i].generation == generation &&
                        address >= s_mb_cache[i].base && bytes <= s_mb_cache[i].size &&
                        address - s_mb_cache[i].base <= s_mb_cache[i].size - bytes)
#  if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
                {
                    hit = 1;
                    break;
                }
#  else
                    return 1;
#  endif
        }
# endif
# if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
#  if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
        /* mq keeps its production meaning: the queries a cached build makes. */
        if (!hit)
#  endif
        ++s_pw.mb_queries;
# endif
        info.size = sizeof info;
        if (sceKernelGetMemBlockInfoByAddr((void *)(uintptr_t)address, &info) < 0)
#  if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
        {
            if (hit)
                record_fault("GL record admission cache hit on an unreadable span");
            return 0;
        }
#  else
            return 0;
#  endif
        base = (uintptr_t)info.mappedBase;
        readable = base && address >= base && bytes <= info.mappedSize &&
            address - base <= info.mappedSize - bytes &&
            (!info.access || (info.access & SCE_KERNEL_MEMORY_ACCESS_R));
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
        if (hit) {
            if (!readable)
                record_fault("GL record admission cache hit on an unreadable span");
            ++s_mb_verify.hits;
            return 1;   /* the store below runs only on a miss, as in production */
        }
        if (cacheable)
            ++s_mb_verify.misses;
# endif
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE)
        if (readable && cacheable) {
            uint32_t slot = s_mb_next++ % RECORD_MB_CACHE_ENTRIES;  /* round robin */
            s_mb_cache[slot].base = (uint32_t)base;
            s_mb_cache[slot].size = (uint32_t)info.mappedSize;
            s_mb_cache[slot].generation = generation;
            s_mb_cache[slot].valid = 1u;
        }
# endif
        return readable;
    }
#endif
}

#if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY) && defined(__vita__)
/* Valid entries covering `address` at the current free generation. */
static uint32_t record_mb_cached(uint32_t address)
{
    uint32_t i, n = 0u;
    uint32_t generation = __atomic_load_n(&s_memblock_generation, __ATOMIC_SEQ_CST);
    for (i = 0u; i < RECORD_MB_CACHE_ENTRIES; ++i)
        if (s_mb_cache[i].valid && s_mb_cache[i].generation == generation &&
                address >= s_mb_cache[i].base && address - s_mb_cache[i].base < s_mb_cache[i].size)
            ++n;
    return n;
}

/* Boot self-test, producer thread: cache one
 * page, free it through the link's wrap, re-query.  out = {entries covering
 * the page after the first query (1), generation steps across the free (1),
 * entries covering it after the free (0), readable after the free (0, or 1
 * if another allocation took the range: read with out[2])}.  The cache and
 * the VERIFY counters are cleared afterwards, so the run starts as a build
 * without VERIFY would: empty entries, round robin at slot 0. */
int isaac_gl_record_admission_selftest(uint32_t out[4])
{
    SceUID uid;
    void *base = NULL;
    uint32_t address, generation0, mb_queries0;
    int ok;

    out[0] = out[1] = out[2] = out[3] = 0u;
    if (record_on_consumer())
        return -1;
    /* The two queries below are the self-test's, not the game's: ph120.rw
     * mq is restored so the first window keeps its production meaning. */
    mb_queries0 = s_pw.mb_queries;
    /* Call shape copied from host_vita_anm2_scratch.c (USER_RW, NULL opt);
     * one 0x1000 page as host_vita_room_entry_external.c sizes its pages. */
    uid = sceKernelAllocMemBlock("isaac_glrec_selftest", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                 0x1000, NULL);
    if (uid < 0)
        return -1;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        return -1;
    }
    address = (uint32_t)(uintptr_t)base;
    (void)record_readable_query(address, 4u);
    out[0] = record_mb_cached(address);
    generation0 = __atomic_load_n(&s_memblock_generation, __ATOMIC_SEQ_CST);
    ok = sceKernelFreeMemBlock(uid) >= 0;   /* through the --wrap: bumps first */
    out[1] = __atomic_load_n(&s_memblock_generation, __ATOMIC_SEQ_CST) - generation0;
    out[2] = record_mb_cached(address);
    out[3] = (uint32_t)record_readable_query(address, 4u);
    memset(s_mb_cache, 0, sizeof s_mb_cache);
    s_mb_next = 0u;
    s_pw.mb_queries = mb_queries0;
    s_mb_verify.misses = s_mb_verify.hits = 0u;
    s_mb_verify.generation0 = __atomic_load_n(&s_memblock_generation, __ATOMIC_SEQ_CST);
    if (!ok)
        return -1;
    return out[0] == 1u && out[1] == 1u && out[2] == 0u && out[3] == 0u;
}

/* Producer thread.  out = {wraps (free generation steps), misses (queries
 * the production build makes), hits re-queried and agreed}, all since the
 * self-test. */
void isaac_gl_record_admission_verify_stats(uint32_t out[3])
{
    out[0] = __atomic_load_n(&s_memblock_generation, __ATOMIC_SEQ_CST) - s_mb_verify.generation0;
    out[1] = s_mb_verify.misses;
    out[2] = s_mb_verify.hits;
}
#endif

static uint32_t record_client_bytes(int32_t count, uint32_t stride, uint32_t address)
{
    if (count <= 0 || !address || (uint32_t)count > (256u * 1024u) / stride)
        return 0u; /* explicit synchronous fallback, never a truncated copy */
    return (uint32_t)count * stride;
}

static void record_enqueue2(uint32_t opcode, const void *args, uint32_t arg_bytes,
    uint32_t client, uint32_t client_bytes, uint32_t client2, uint32_t client2_bytes)
{
    uint32_t rva = guest_gl_backend_return_rva();
#if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_NO_COPY)
    client_bytes = 0u; /* oracle self-test: replay reads the live guest buffer */
#endif
#if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_NO_RVA)
    rva = 0u; /* oracle self-test: stack provenance lost */
#endif
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    s_pw.census_last_op = opcode;
    if (record_census_tick(&s_pw.census_left, &s_pw.census_lcg)) {
# else
    if ((++s_pw.ap_tick & RECORD_SAMPLE_MASK) == 0u) {
# endif
        uint64_t started = record_now_us();
        s_last_pos = isaac_gl_record_spsc_append(&s_ring, opcode, rva, args, arg_bytes,
            (const void *)(uintptr_t)client, client_bytes,
            (const void *)(uintptr_t)client2, client2_bytes);
        {
            uint64_t ended = record_now_us(), again;
            s_pw.ap_us += (uint32_t)(ended - started);
            again = record_now_us();
            s_pw.clock_us += (uint32_t)(again - ended);
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
            {
                RecordCensusProduce *p = &s_census_produce[record_census_slot(opcode)];
                ++p->ps;
                if (s_pw.census_stamp) {
                    ++p->pst;
                    p->pus += (uint32_t)(ended - s_pw.census_stamp);
                    s_pw.census_stamp = 0u;
                } else {
                    p->pus += (uint32_t)(ended - started);
                }
            }
# endif
        }
        ++s_pw.ap_samples;
    } else {
        s_last_pos = isaac_gl_record_spsc_append(&s_ring, opcode, rva, args, arg_bytes,
            (const void *)(uintptr_t)client, client_bytes,
            (const void *)(uintptr_t)client2, client2_bytes);
    }
    if (!s_last_pos)
        record_fault("GL recorder could not publish a bounded command");
# if defined(ISAAC_VITA_GL_THREAD_WORKER_PROOF) && ISAAC_VITA_GL_THREAD_WORKER_PROOF
    /* Ownership proof: never overlap; the render thread still makes every call. */
    isaac_gl_record_spsc_wait_done(&s_ring, s_last_pos);
# endif
#else
    if (!isaac_gl_record_ring_append_client2(&s_ring, opcode,
            rva, args, arg_bytes,
            (const void *)(uintptr_t)client, client_bytes,
            (const void *)(uintptr_t)client2, client2_bytes))
        record_fault("GL recorder could not publish a bounded command");
#endif
#if defined(ISAAC_GL_RECORD_ORACLE_EAGER)
    /* Oracle layer 1: replay from the ring copy at once, whatever the entry
     * path (dispatch, replay helper, direct table member). */
    isaac_gl_record_drain();
#endif
}

static void record_enqueue(uint32_t opcode, const void *args, uint32_t arg_bytes,
    uint32_t client, uint32_t client_bytes)
{
    record_enqueue2(opcode, args, arg_bytes, client, client_bytes, 0u, 0u);
}

static void record_sync(void)
{
    ++s_syncs;
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    ++s_pw.syncs;
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    {
        uint32_t waits = s_pw.sync_waits, us = s_pw.sync_wait_us;
        record_wait(s_last_pos, &s_pw.sync_waits, &s_pw.sync_wait_us);
        record_census_wait(s_pw.census_last_op, s_pw.sync_waits - waits,
            s_pw.sync_wait_us - us);
    }
# else
    record_wait(s_last_pos, &s_pw.sync_waits, &s_pw.sync_wait_us);
# endif
#else
    isaac_gl_record_drain();
#endif
}

static void record_forget_status(void)
{
    memset(s_status, 0, sizeof s_status);
    s_bound_fbo_known = 0;
}

static void record_note_bind_fbo(uint32_t target, uint32_t name)
{
    if (target == RECORD_FRAMEBUFFER || target == RECORD_DRAW_FRAMEBUFFER) {
        s_bound_fbo = name;
        s_bound_fbo_known = 1;
    }
}

static void record_note_attach_fbo(uint32_t target, uint32_t attachment,
    uint32_t textarget, uint32_t name, int32_t level)
{
    uint32_t i;
    if (!s_bound_fbo_known || !s_bound_fbo ||
        (target != RECORD_FRAMEBUFFER && target != RECORD_DRAW_FRAMEBUFFER) ||
        attachment != RECORD_COLOR_ATTACHMENT0) return;
    for (i = 0u; i < RECORD_STATUS_CAPACITY; ++i) {
        if (!s_status[i].name || s_status[i].name == s_bound_fbo) {
            s_status[i].name = s_bound_fbo;
            /* A zero detach/other shape is not modelled: retain exact S fallback. */
            s_status[i].complete = name && textarget == RECORD_TEXTURE_2D && level == 0;
            return;
        }
    }
}

static void record_sync(void);
static void record_note_attrib_enable(uint32_t index, int on);
static void record_note_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
    uint8_t normalized, int32_t stride, uint32_t pointer);
static void record_glDrawElements(guest_gl_enum, guest_gl_sizei, guest_gl_enum, guest_gl_addr);
static void record_custom_replay(const IsaacGlRecordHeader *, const void *);
#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
/* The bind-packet location memo (isaac_gl_record_bind_packet_uniform_location)
 * remembers producer location-cache HITS.  The cache has no eviction and a
 * present key is never stored again (stores follow misses only), so a hit
 * keeps answering the same value until the cache is reset.  Every reset in
 * this TU - isaac_gl_record_wrap and the seven program-state mutators of the
 * generated surface below - goes through this macro and advances the epoch
 * the memo entries are keyed on.  Epoch 0 is never current. */
static uint32_t s_location_epoch = 1u;
static void record_locations_reset_epoch(IsaacGlRecordLocations *memo)
{
    if (++s_location_epoch == 0u) s_location_epoch = 1u;
    isaac_gl_record_locations_reset(memo);
}
# define isaac_gl_record_locations_reset record_locations_reset_epoch
#endif
#include "gl_record_surface.inc"
#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
/* RENDER60 W-pkt: one record per native KAGE Shader::Bind (its staged typed
 * calls, see isaac_gl_record_bind_packet_*), appended after the generated
 * surface (0..78) and S4's 79/80, which stay reserved when S4 is OFF.
 * Analyzers map it by name (tools/bench/bench_run.py). */
enum { REC_KAGE_BIND_PACKET = REC_WORKER_CALL + 3 };
_Static_assert(REC_KAGE_BIND_PACKET + 1u == ISAAC_GL_RECORD_OPCODE_SLOTS,
    "W-pkt: ISAAC_GL_RECORD_OPCODE_SLOTS (gl_record_backend.h) must end at REC_KAGE_BIND_PACKET");
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
_Static_assert(REC_KAGE_BIND_PACKET + 1u == ISAAC_GL_RECORD_CENSUS_OPS,
    "census: one slot per opcode, REC_KAGE_BIND_PACKET last (gl_record_backend.h)");
# endif
#endif
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS) && \
    !defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH) && !defined(ISAAC_VITA_KAGE_BIND_PACKET)
_Static_assert(REC_WORKER_CALL + 1u == ISAAC_GL_RECORD_CENSUS_OPS,
    "census: one slot per opcode, REC_WORKER_CALL last (bump the bound with the enum)");
#endif
#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
/* RENDER60 S4 opcodes, appended after the generated surface's last opcode so
 * gl_record_surface.py and its .inc stay untouched (79 and 80 while the
 * surface ends at REC_WORKER_CALL = 78).  Analyzers map them by name
 * (tools/bench/bench_run.py reads this enum next to the surface's). */
enum { REC_ATTRIBS_ENABLE = REC_WORKER_CALL + 1, REC_ATTRIBS_DISABLE = REC_WORKER_CALL + 2 };
# if !defined(ISAAC_VITA_KAGE_BIND_PACKET)
_Static_assert(REC_ATTRIBS_DISABLE + 1u == ISAAC_GL_RECORD_OPCODE_SLOTS,
    "S4: ISAAC_GL_RECORD_OPCODE_SLOTS (gl_record_backend.h) must end at REC_ATTRIBS_DISABLE");
# endif
_Static_assert(REC_ATTRIBS_ENABLE == ISAAC_GL_RECORD_ATTRIBS_ENABLE_OPCODE &&
    REC_ATTRIBS_DISABLE == ISAAC_GL_RECORD_ATTRIBS_DISABLE_OPCODE,
    "S4: the published opcode numbers (gl_record_backend.h) are the enum's");
# if defined(ISAAC_GL_RECORD_OPCODE_CENSUS) && !defined(ISAAC_VITA_KAGE_BIND_PACKET)
_Static_assert(REC_ATTRIBS_DISABLE + 1u == ISAAC_GL_RECORD_CENSUS_OPS,
    "census: one slot per opcode, REC_ATTRIBS_DISABLE last (bump the bound with the enum)");
# endif
#endif

/* ---- attribute shadows and draw snapshots ------------------------------
 * Producer shadow: guest requests in call order (span computation).
 * Consumer shadow: the same requests in replay order (rebinding). vitaGL's
 * glVertexAttribPointer is pure state (custom_shaders.c:4743..4794): pointer,
 * vbo, format, stride (0 = bpe * size). Enable state is process-wide like the
 * native VAO, starting all-disabled (vitaGL's zero-initialized default VAO;
 * assumption, not read in full). Out-of-range indices make all unknown. */
_Static_assert(sizeof(void *) == 4u, "draw relocation assumes 32-bit pointers");
#define RECORD_ATTRIBS 16u
#define RECORD_DRAW_SPAN_MAX (256u * 1024u)
typedef struct RecordAttrib {
    uint32_t pointer, type;
    int32_t size, stride;
    uint8_t normalized;
} RecordAttrib;
typedef struct RecordAttribShadow {
    RecordAttrib attrib[RECORD_ATTRIBS];
    uint16_t enable_known, enabled, pointer_known;
} RecordAttribShadow;
static RecordAttribShadow s_produce = {{{0}}, 0xffffu, 0u, 0u};
static RecordAttribShadow s_consume = {{{0}}, 0xffffu, 0u, 0u};
static struct { int active; uint32_t delta; } s_reloc;
static uint32_t s_snap_draws, s_sync_draws;

static void shadow_enable(RecordAttribShadow *s, uint32_t index, int on)
{
    uint16_t bit;
    if (index >= RECORD_ATTRIBS) { s->enable_known = 0u; return; }
    bit = (uint16_t)(1u << index);
    s->enable_known |= bit;
    if (on) s->enabled |= bit; else s->enabled &= (uint16_t)~bit;
}

static void shadow_pointer(RecordAttribShadow *s, uint32_t index, int32_t size,
    uint32_t type, uint8_t normalized, int32_t stride, uint32_t pointer)
{
    RecordAttrib *a;
    if (index >= RECORD_ATTRIBS) { s->pointer_known = 0u; return; }
    a = &s->attrib[index];
    a->pointer = pointer; a->type = type; a->size = size;
    a->stride = stride; a->normalized = normalized;
    s->pointer_known |= (uint16_t)(1u << index);
}

static void record_note_attrib_enable(uint32_t index, int on)
{ shadow_enable(&s_produce, index, on); }
static void record_note_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
    uint8_t normalized, int32_t stride, uint32_t pointer)
{ shadow_pointer(&s_produce, index, size, type, normalized, stride, pointer); }

#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
/* ---- RENDER60 S4: one record per Shader attribute replay ----------------
 * Contract in gl_record_backend.h.  Producer: the per-call members
 * record_glEnableVertexAttribArray + record_glVertexAttribPointer (resp.
 * record_glDisableVertexAttribArray), gl_record_surface.inc, are a shadow
 * note followed by an enqueue; the batch makes the same notes with the same
 * arguments in the same order and ONE enqueue.  s_produce is read only by
 * record_draw_span, in a later draw record, so the notes need not be
 * interleaved with enqueues.  The return RVA is captured once; every
 * per-call record of one fastpath replay captured the same [esp] word.
 * Consumer: per element, record_replay's shadow update, then the s_target
 * call record_dispatch makes for that per-call record. */
_Static_assert(ISAAC_GL_RECORD_ATTRIB_BATCH_MAX == RECORD_ATTRIBS,
    "S4: the batch bound is the shadow width");
/* Enable: {u32 count; i32 stride; u32 base; i32 loc[count];
 * u8 components[count] zero-padded to 4}.  Disable: {u32 count; i32 loc[count]}. */
#define RECORD_BATCH_ENABLE_BYTES(n) (12u + 4u * (n) + (((n) + 3u) & ~3u))
#define RECORD_BATCH_DISABLE_BYTES(n) (4u + 4u * (n))
#define RECORD_BATCH_WORDS (3u + ISAAC_GL_RECORD_ATTRIB_BATCH_MAX + \
    ISAAC_GL_RECORD_ATTRIB_BATCH_MAX / 4u)
# if defined(ISAAC_GL_VITA_BACKEND_ORACLE) || defined(ORACLE_ATTRIB_RECORDER)
static guest_gl_backend s_oracle_installed;
/* enable records, disable records, attributes, batches that started after
 * a ring wrap marker (worker builds) */
static uint32_t s_batch_stats[4];
#  define RECORD_BATCH_COUNT(slot, n) (++s_batch_stats[(slot)], s_batch_stats[2] += (n))
# else
#  define RECORD_BATCH_COUNT(slot, n) ((void)0)
# endif

static void record_batch_publish(uint32_t opcode, const uint32_t *payload, uint32_t bytes)
{
# if (defined(ISAAC_GL_VITA_BACKEND_ORACLE) || defined(ORACLE_ATTRIB_RECORDER)) && \
     defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    /* Oracles only: a batch that started after a wrap marker moved the
     * producer's write position by more than its own record (record size
     * as ring_layout and isaac_gl_record_spsc_append compute it,
     * gl_record_ring.c:58 and :197). */
    uint32_t from = s_ring.write_pos;
    record_enqueue(opcode, payload, bytes, 0u, 0u);
    if (s_ring.write_pos - from >
            (((((uint32_t)sizeof(IsaacGlRecordHeader) + bytes + 7u) & ~7u) + 15u) & ~15u))
        ++s_batch_stats[3];
# else
    record_enqueue(opcode, payload, bytes, 0u, 0u);
# endif
}

int isaac_gl_record_attribs_batch_ready(const guest_gl_backend *installed,
    uint32_t count, int enable)
{
    if (!s_active || count == 0u || count > RECORD_ATTRIBS ||
            installed->glGetAttribLocation != record_glGetAttribLocation)
        return 0;
    if (enable)
        return installed->glEnableVertexAttribArray == record_glEnableVertexAttribArray &&
            installed->glVertexAttribPointer == record_glVertexAttribPointer;
    return installed->glDisableVertexAttribArray == record_glDisableVertexAttribArray;
}

void isaac_gl_record_attribs_enable(uint32_t count, const guest_gl_int *locations,
    const uint8_t *components, int32_t stride, uint32_t base)
{
    uint32_t payload[RECORD_BATCH_WORDS];
    uint8_t *packed;
    uint32_t i, pointer = base, padded = (count + 3u) & ~3u;
    if (count == 0u || count > RECORD_ATTRIBS)   /* callers passed ready() */
        record_fault("GL recorder attribute batch out of range");
    packed = (uint8_t *)&payload[3u + count];
    payload[0] = count;
    payload[1] = (uint32_t)stride;
    payload[2] = base;
    for (i = 0u; i < count; ++i) {
        /* record_glEnableVertexAttribArray(loc), then
         * record_glVertexAttribPointer(loc, ncomp, GL_FLOAT, 0, stride, p). */
        record_note_attrib_enable((guest_gl_uint)locations[i], 1);
        record_note_attrib_pointer((guest_gl_uint)locations[i],
            (guest_gl_int)components[i], ISAAC_GL_RECORD_ATTRIB_BATCH_TYPE,
            (guest_gl_boolean)0u, (guest_gl_sizei)stride, (guest_gl_addr)pointer);
#if !defined(ISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_NO_ADVANCE)
        pointer += (uint32_t)components[i] * 4u;   /* oracle self-test skips it */
#endif
        payload[3u + i] = (uint32_t)locations[i];
    }
    for (i = 0u; i < padded; ++i)   /* the ring copies the padding verbatim */
        packed[i] = (uint8_t)(i < count ? components[i] : 0u);
    RECORD_BATCH_COUNT(0u, count);
    record_batch_publish(REC_ATTRIBS_ENABLE, payload, RECORD_BATCH_ENABLE_BYTES(count));
}

void isaac_gl_record_attribs_disable(uint32_t count, const guest_gl_int *locations)
{
    uint32_t payload[1u + ISAAC_GL_RECORD_ATTRIB_BATCH_MAX];
    uint32_t i;
    if (count == 0u || count > RECORD_ATTRIBS)   /* callers passed ready() */
        record_fault("GL recorder attribute batch out of range");
    payload[0] = count;
    for (i = 0u; i < count; ++i) {
        /* record_glDisableVertexAttribArray(loc) */
        record_note_attrib_enable((guest_gl_uint)locations[i], 0);
        payload[1u + i] = (uint32_t)locations[i];
    }
    RECORD_BATCH_COUNT(1u, count);
    record_batch_publish(REC_ATTRIBS_DISABLE, payload, RECORD_BATCH_DISABLE_BYTES(count));
}

static void record_replay_attribs(const IsaacGlRecordHeader *h, const void *payload)
{
    const uint32_t *a = (const uint32_t *)payload;
    uint32_t n = h->payload_bytes >= 4u ? a[0] : 0u, i;
    if (h->opcode == REC_ATTRIBS_ENABLE) {
        const uint8_t *components;
        uint32_t pointer;
        int32_t stride;
        if (n == 0u || n > RECORD_ATTRIBS || h->payload_bytes != RECORD_BATCH_ENABLE_BYTES(n))
            record_fault("GL recorder malformed attribute batch");
        stride = (int32_t)a[1];
        pointer = a[2];
        components = (const uint8_t *)(a + 3u + n);
#if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_ORDER)
        /* oracle self-test: all enables, then all pointers */
        for (i = 0u; i < n; ++i) {
            shadow_enable(&s_consume, a[3u + i], 1);
            s_target.glEnableVertexAttribArray((guest_gl_uint)a[3u + i]);
        }
        for (i = 0u; i < n; ++i) {
            shadow_pointer(&s_consume, a[3u + i], (int32_t)components[i],
                ISAAC_GL_RECORD_ATTRIB_BATCH_TYPE, 0u, stride, pointer);
            s_target.glVertexAttribPointer((guest_gl_uint)a[3u + i],
                (guest_gl_int)components[i], ISAAC_GL_RECORD_ATTRIB_BATCH_TYPE,
                (guest_gl_boolean)0u, (guest_gl_sizei)stride, (guest_gl_addr)pointer);
            pointer += (uint32_t)components[i] * 4u;
        }
#else
        for (i = 0u; i < n; ++i) {
            guest_gl_uint location = (guest_gl_uint)a[3u + i];
# if !defined(ISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_NO_CONSUME)
            shadow_enable(&s_consume, location, 1);   /* oracle self-test skips it */
# endif
            s_target.glEnableVertexAttribArray(location);
# if !defined(ISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_NO_CONSUME)
            shadow_pointer(&s_consume, location, (int32_t)components[i],
                ISAAC_GL_RECORD_ATTRIB_BATCH_TYPE, 0u, stride, pointer);
# endif
            s_target.glVertexAttribPointer(location, (guest_gl_int)components[i],
                ISAAC_GL_RECORD_ATTRIB_BATCH_TYPE, (guest_gl_boolean)0u,
                (guest_gl_sizei)stride, (guest_gl_addr)pointer);
            pointer += (uint32_t)components[i] * 4u;
        }
#endif
        return;
    }
    if (n == 0u || n > RECORD_ATTRIBS || h->payload_bytes != RECORD_BATCH_DISABLE_BYTES(n))
        record_fault("GL recorder malformed attribute batch");
    for (i = 0u; i < n; ++i) {
        guest_gl_uint location = (guest_gl_uint)a[1u + i];
#if !defined(ISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_NO_CONSUME)
        shadow_enable(&s_consume, location, 0);
#endif
        s_target.glDisableVertexAttribArray(location);
    }
}

# if defined(ISAAC_GL_VITA_BACKEND_ORACLE) || defined(ORACLE_ATTRIB_RECORDER)
const guest_gl_backend *isaac_gl_record_oracle_installed(void)
{
    return &s_oracle_installed;
}
void isaac_gl_record_oracle_batch_stats(uint32_t out[4])
{
    out[0] = s_batch_stats[0];
    out[1] = s_batch_stats[1];
    out[2] = s_batch_stats[2];
    out[3] = s_batch_stats[3];
}
uint32_t isaac_gl_record_oracle_shadow(int consumer, void *out, uint32_t capacity)
{
    const RecordAttribShadow *s = consumer ? &s_consume : &s_produce;
    if (capacity >= sizeof *s) memcpy(out, s, sizeof *s);
    return (uint32_t)sizeof *s;
}
void isaac_gl_record_oracle_shadow_poison(int consumer, uint8_t byte)
{
    memset(consumer ? &s_consume : &s_produce, byte, sizeof s_consume);
}
int isaac_gl_record_oracle_shadow_slot(int consumer, uint32_t index, uint32_t out[8])
{
    const RecordAttribShadow *s = consumer ? &s_consume : &s_produce;
    const RecordAttrib *a;
    if (index >= RECORD_ATTRIBS) return 0;
    a = &s->attrib[index];
    out[0] = a->pointer;
    out[1] = a->type;
    out[2] = (uint32_t)a->size;
    out[3] = (uint32_t)a->stride;
    out[4] = a->normalized;
    out[5] = (s->enabled >> index) & 1u;
    out[6] = (s->enable_known >> index) & 1u;
    out[7] = (s->pointer_known >> index) & 1u;
    return 1;
}
# endif
#endif

/* vitaGL bytes per element, copied from custom_shaders.c:4760..4791. */
static uint32_t record_attrib_bpe(uint32_t type)
{
    switch (type) {
    case 0x140bu: case 0x8d61u: return 2u;  /* HALF_FLOAT, HALF_FLOAT_OES */
    case 0x1406u: return 4u;                 /* FLOAT */
    case 0x1402u: case 0x1403u: return 2u;   /* SHORT, UNSIGNED_SHORT */
    case 0x1400u: case 0x1401u: return 1u;   /* BYTE, UNSIGNED_BYTE */
    default: return 0u;
    }
}

static void record_rebind(int relocated)
{
    uint32_t i;
    for (i = 0u; i < RECORD_ATTRIBS; ++i) {
        const RecordAttrib *a = &s_consume.attrib[i];
        if (!(s_consume.enabled & (1u << i))) continue;
        s_rbo.attrib_pointer(i, a->size, a->type, a->normalized, a->stride,
            a->pointer + (relocated ? s_reloc.delta : 0u));
    }
}

void isaac_gl_record_native_draw(uint32_t mode, int32_t count, uint32_t type, uint32_t indices)
{
    int relocated = s_reloc.active;
    if (relocated) record_rebind(1);
    s_rbo.draw(mode, count, type, indices);
    if (relocated) record_rebind(0);
}

int isaac_gl_record_native_canonical(int32_t count)
{
    int relocated = s_reloc.active, drawn;
    if (!s_rbo.canonical) record_fault("GL recorder canonical draw without endpoint");
    if (relocated) record_rebind(1);
    drawn = s_rbo.canonical(count);
    if (relocated) record_rebind(0);
    return drawn;
}

typedef struct RecordDraw {
    Record_glDrawElements a;
    uint32_t snapshot, base, bytes, index_bytes;
} RecordDraw;

static uint32_t record_index_at(uint32_t indices, uint32_t size, uint32_t i)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)indices + (size_t)i * size;
    uint16_t v16;
    uint32_t v32;
    if (size == 1u) return *p;
    if (size == 2u) { memcpy(&v16, p, 2u); return v16; }
    memcpy(&v32, p, 4u); return v32;
}

/* Exact copy of every byte vitaGL may read for this draw, or zero (S). */
static int record_draw_span(const RecordDraw *d, uint32_t index_size,
    uint32_t *base, uint32_t *bytes)
{
    uint64_t lo = UINT64_MAX, hi = 0u;
    uint32_t top = 0u, i, count = (uint32_t)d->a.count;
#if defined(ISAAC_GL_RECORD_ORACLE_EAGER)
    (void)index_size; (void)base; (void)bytes; (void)i; (void)count; (void)top;
    (void)lo; (void)hi;
    return 0; /* layer 1 keeps raw-pointer draws; layer 2 tests relocation */
#else
    if (d->a.count <= 0 || !index_size || !d->a.indices ||
            count > RECORD_DRAW_SPAN_MAX / index_size ||
            s_produce.enable_known != 0xffffu ||
            !record_readable(d->a.indices, count * index_size))
        return 0;
    for (i = 0u; i < count; ++i) {
        uint32_t v = record_index_at(d->a.indices, index_size, i);
        if (v >= top) top = v + 1u;
    }
    /* Canonical path draws count/6*4 vertices regardless of index values. */
    if (d->a.mode == 4u && d->a.type == 0x1403u && count % 6u == 0u &&
            count / 6u * 4u > top)
        top = count / 6u * 4u;
    for (i = 0u; i < RECORD_ATTRIBS; ++i) {
        const RecordAttrib *a = &s_produce.attrib[i];
        uint64_t element, stride, start, end;
        if (!(s_produce.enabled & (1u << i))) continue;
        if (!(s_produce.pointer_known & (1u << i)) || a->size < 1 || a->size > 4 ||
                a->stride < 0 || !record_attrib_bpe(a->type) || !a->pointer)
            return 0;
        element = (uint64_t)a->size * record_attrib_bpe(a->type);
        stride = a->stride ? (uint64_t)a->stride : element;
        start = a->pointer;
        /* Per-vertex reads and the coalesced top*stride bulk copy
         * (custom_shaders.c:2843..2861, correction 1). */
        end = start + ((uint64_t)(top - 1u) * stride + element > (uint64_t)top * stride ?
            (uint64_t)(top - 1u) * stride + element : (uint64_t)top * stride);
        if (start < lo) lo = start;
        if (end > hi) hi = end;
    }
    if (hi == 0u) { *base = 0u; *bytes = 0u; return 1; } /* no enabled array */
    if (hi > UINT32_MAX || hi - lo > RECORD_DRAW_SPAN_MAX ||
            !record_readable((uint32_t)lo, (uint32_t)(hi - lo)))
        return 0;
    *base = (uint32_t)lo;
    *bytes = (uint32_t)(hi - lo);
    return 1;
#endif
}

static void record_glDrawElements(guest_gl_enum mode, guest_gl_sizei count,
    guest_gl_enum type, guest_gl_addr indices)
{
    RecordDraw d;
    uint32_t index_size = type == 0x1401u ? 1u : type == 0x1403u ? 2u :
        type == 0x1405u ? 4u : 0u;
    memset(&d, 0, sizeof d);
    d.a.mode = mode; d.a.count = count; d.a.type = type; d.a.indices = indices;
    if (!record_draw_span(&d, index_size, &d.base, &d.bytes)) {
        ++s_sync_draws;
        record_enqueue(REC_glDrawElements, &d, sizeof d, 0u, 0u);
        record_sync();
        return;
    }
    ++s_snap_draws;
    d.snapshot = 1u;
    d.index_bytes = (uint32_t)count * index_size;
    record_enqueue2(REC_glDrawElements, &d, sizeof d, indices, d.index_bytes,
        d.base, d.bytes);
}

static void record_replay_draw(const IsaacGlRecordHeader *h, const void *payload)
{
    const RecordDraw *d = (const RecordDraw *)payload;
    Record_glDrawElements a = d->a;
    (void)h;
    if (d->snapshot) {
        const uint8_t *copy = (const uint8_t *)payload + sizeof *d;
        a.indices = (guest_gl_addr)(uintptr_t)copy;
        s_reloc.delta = (uint32_t)(uintptr_t)(copy +
            ((sizeof *d + d->index_bytes + 7u) & ~7u) - sizeof *d) - d->base;
        s_reloc.active = 1;
#if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_NO_REBIND)
        s_reloc.active = 0; /* oracle self-test: vitaGL reads the live guest arrays */
#endif
    }
    s_target.glDrawElements(a.mode, a.count, a.type, a.indices);
    s_reloc.active = 0;
}


static void record_replay(void *context, const IsaacGlRecordHeader *h, const void *payload)
{
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    uint32_t slot = record_census_slot(h->opcode), payload_bytes = h->payload_bytes;
    uint32_t timed = 0u;   /* 1 = exact, 2 = random */
    uint64_t started = 0u;
#endif
    (void)context;
    s_replaying = 1;
    s_rva = h->return_rva;
    if (h->opcode == REC_glEnableVertexAttribArray)
        shadow_enable(&s_consume, ((const Record_glEnableVertexAttribArray *)payload)->index, 1);
    else if (h->opcode == REC_glDisableVertexAttribArray)
        shadow_enable(&s_consume, ((const Record_glDisableVertexAttribArray *)payload)->index, 0);
    else if (h->opcode == REC_glVertexAttribPointer) {
        const Record_glVertexAttribPointer *p = (const Record_glVertexAttribPointer *)payload;
        shadow_pointer(&s_consume, p->index, p->size, p->type, p->normalized, p->stride, p->pointer);
    }
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    /* Decided before the call: a take closure resets the counts inside it,
     * and its own record is counted afterwards, in the new window. */
    if (slot == REC_WORKER_CALL || s_census_replay[slot].n < ISAAC_GL_RECORD_CENSUS_EXACT)
        timed = 1u;
    else if (record_census_tick(&s_rw.census_left, &s_rw.census_lcg))
        timed = 2u;
    if (timed) started = record_now_us();
#endif
    record_dispatch(h, payload);
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    {
        RecordCensusReplay *r = &s_census_replay[slot];
        if (timed) {
            uint32_t us = (uint32_t)(record_now_us() - started);
            if (timed == 1u) { ++r->e; r->eus += us; }
            else { ++r->rs; r->rus += us; }
        }
        ++r->n;
        r->b += payload_bytes;
    }
#endif
    s_replaying = 0;
}

void isaac_gl_record_drain(void)
{
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    /* On the consumer everything earlier already ran (records are ordered). */
    if (!record_on_consumer() && s_worker)
        record_wait(s_ring.write_pos, &s_pw.call_waits, &s_pw.call_wait_us);
#else
    if (s_active && !isaac_gl_record_ring_drain(&s_ring))
        record_fault("GL recorder nested drain");
#endif
}

static uint32_t s_total_records, s_total_bytes, s_total_syncs, s_total_cold;

void isaac_gl_record_stop(void)
{
    isaac_gl_record_drain();
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    if (0) {   /* the worker ring persists: its counters are process totals */
#else
    if (s_active) {
#endif
        s_total_records += s_ring.records;
        s_total_bytes += s_ring.copied_bytes;
        s_total_syncs += s_syncs;
        s_total_cold += s_location_cold;
    }
    s_active = 0;
    s_replaying = s_rbo_sync = 0;
}

void isaac_gl_record_external_begin(void)
{
    isaac_gl_record_drain();
    record_forget_status();
}

void isaac_gl_record_before_fault(void)
{
    if (!s_active) return;
    if (!s_ring.draining) isaac_gl_record_drain();
    /* The synchronous failing command terminates this batch. Earlier commands
     * already executed; no later commands can have been published behind it. */
    s_ring.used = s_ring.draining = 0u;
    s_replaying = s_rbo_sync = 0;
}

int isaac_gl_record_return_rva(uint32_t *rva)
{
    if (!record_replaying()) return 0;
    *rva = s_rva;
    return 1;
}

void isaac_gl_record_wrap(guest_gl_backend *backend, const IsaacGlRecordRboNative *rbo)
{
    s_target = *backend;
    s_rbo = *rbo;
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    if (!record_on_consumer())
        record_fault("GL recorder worker: install must run on the consumer");
#else
    isaac_gl_record_ring_init(&s_ring, record_replay, NULL);
#endif
    isaac_gl_record_locations_reset(&s_locations);
    s_syncs = s_location_cold = 0u;
    s_replaying = s_rbo_sync = 0;
    record_forget_status();
    s_bound_fbo = 0u; s_bound_fbo_known = 1;
    record_table(backend);
#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH) && \
    (defined(ISAAC_GL_VITA_BACKEND_ORACLE) || defined(ORACLE_ATTRIB_RECORDER))
    s_oracle_installed = *backend;
#endif
    s_active = 1;
}

static guest_gl_int record_location(uint32_t kind, uint32_t program, uint32_t name)
{
    uint32_t args[2] = {program, name};
    uint32_t op = kind == ISAAC_GL_RECORD_LOCATION_ATTRIB ?
        REC_glGetAttribLocation : REC_glGetUniformLocation;
    IsaacGlRecordLocationKey key;
    int32_t answer;
    int cacheable = record_readable(name, ISAAC_GL_RECORD_LOCATION_NAME_MAX + 1u) &&
        isaac_gl_record_location_key(kind, program, (const char *)(uintptr_t)name,
            ISAAC_GL_RECORD_LOCATION_NAME_MAX + 1u, &key);
    if (cacheable && isaac_gl_record_locations_lookup(&s_locations, &key, &answer)) {
        /* Replay the ORIGINAL lookup too: preserve cache/phase/location-observed
         * transitions in command order, but the game does not wait for it. */
        record_enqueue(op, args, sizeof args, name, (uint32_t)key.length + 1u);
        return answer;
    }
    ++s_location_cold;
#if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_PRODUCER_S)
    /* oracle self-test: the S call runs on the producer thread */
    (void)op;
    answer = kind == ISAAC_GL_RECORD_LOCATION_ATTRIB ?
        s_target.glGetAttribLocation(program, name) : s_target.glGetUniformLocation(program, name);
#else
    record_enqueue(op, args, sizeof args, 0u, 0u);
    record_sync();
    answer = (int32_t)s_result;
#endif
    if (cacheable) (void)isaac_gl_record_locations_store(&s_locations, &key, answer);
    return answer;
}

static guest_gl_int record_glGetAttribLocation(guest_gl_uint program, guest_gl_addr name)
{ return record_location(ISAAC_GL_RECORD_LOCATION_ATTRIB, program, name); }
static guest_gl_int record_glGetUniformLocation(guest_gl_uint program, guest_gl_addr name)
{ return record_location(ISAAC_GL_RECORD_LOCATION_UNIFORM, program, name); }

static guest_gl_enum record_glCheckFramebufferStatus(guest_gl_enum target)
{
    Record_glCheckFramebufferStatus a = {target};
    uint32_t i;
    int known = s_bound_fbo_known && !s_bound_fbo;
    if (s_bound_fbo_known && s_bound_fbo)
        for (i = 0u; i < RECORD_STATUS_CAPACITY; ++i)
            if (s_status[i].name == s_bound_fbo && s_status[i].complete) known = 1;
    record_enqueue(REC_glCheckFramebufferStatus, &a, sizeof a, 0u, 0u);
    if (known && (target == RECORD_FRAMEBUFFER || target == RECORD_DRAW_FRAMEBUFFER))
        return RECORD_FRAMEBUFFER_COMPLETE;
    record_sync();
    return (guest_gl_enum)s_result;
}

/* Reuse the ORIGINAL producer registry/validation; replay only its native API. */
static void record_glBindRenderbuffer(guest_gl_enum target, guest_gl_uint name)
{ s_target.glBindRenderbuffer(target, name); }
static void record_glDeleteRenderbuffers(guest_gl_sizei count, guest_gl_addr names)
{ s_target.glDeleteRenderbuffers(count, names); }
static void record_glRenderbufferStorage(guest_gl_enum target, guest_gl_enum format,
    guest_gl_sizei w, guest_gl_sizei h)
{ s_target.glRenderbufferStorage(target, format, w, h); }
static void record_glGetRenderbufferParameteriv(guest_gl_enum target, guest_gl_enum name, guest_gl_addr out)
{ s_target.glGetRenderbufferParameteriv(target, name, out); }
static void record_glGenRenderbuffers(guest_gl_sizei count, guest_gl_addr names)
{
    s_rbo_sync = 1;
    s_target.glGenRenderbuffers(count, names);
    s_rbo_sync = 0;
}

void isaac_gl_record_rbo_bind(uint32_t target, uint32_t name)
{
    uint32_t a[4] = {target, name, 0u, 0u};
    if (record_replaying() || !s_active) { s_rbo.bind(target, name); return; }
    record_enqueue(REC_NATIVE_BIND_RBO, a, sizeof a, 0u, 0u);
}
void isaac_gl_record_rbo_store(uint32_t target, uint32_t format, int32_t w, int32_t h)
{
    uint32_t a[4] = {target, format, (uint32_t)w, (uint32_t)h};
    if (record_replaying() || !s_active) { s_rbo.store(target, format, w, h); return; }
    record_enqueue(REC_NATIVE_STORE_RBO, a, sizeof a, 0u, 0u);
}
void isaac_gl_record_rbo_delete(int32_t count, uint32_t names)
{
    uint32_t a[4] = {(uint32_t)count, names, 0u, 0u};
    uint32_t bytes;
    if (record_replaying() || !s_active) {
        s_rbo.remove(count, names); return;
    }
    bytes = record_client_bytes(count, 4u, names);
    if (!s_rbo_sync && bytes && record_readable(names, bytes))
        record_enqueue(REC_NATIVE_DELETE_RBO, a, sizeof a, names, bytes);
    else {
        record_enqueue(REC_NATIVE_DELETE_RBO, a, sizeof a, 0u, 0u);
        record_sync();
    }
}
void isaac_gl_record_rbo_gen(int32_t count, uint32_t names)
{
    uint32_t a[4] = {(uint32_t)count, names, 0u, 0u};
    if (record_replaying() || !s_active) { s_rbo.gen(count, names); return; }
    record_enqueue(REC_NATIVE_GEN_RBO, a, sizeof a, 0u, 0u);
    record_sync();
}

#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
/* One staged call inside a REC_KAGE_BIND_PACKET payload: this header, then
 * the call's own record payload (arguments, then the client copy exactly
 * where record_enqueue2 would have put it), zero-padded to 8 bytes. */
typedef struct RecordBindPacketOp {
    uint16_t opcode, payload_bytes;
    uint32_t return_rva;
} RecordBindPacketOp;
_Static_assert(sizeof(RecordBindPacketOp) == 8u, "bind packet op header is 8 bytes");
static int record_bind_packet_opcode(uint32_t opcode)
{
    return opcode == REC_glUseProgram || opcode == REC_glActiveTexture ||
        opcode == REC_glBindTexture || opcode == REC_glGetUniformLocation ||
        opcode == REC_glUniform1i || opcode == REC_glUniformMatrix4fv;
}
/* Render thread: each staged call runs through record_dispatch with its own
 * return RVA, in staging order, i.e. exactly what its separate record would
 * have run (record_replay sets s_replaying around the whole packet; none of
 * the six opcodes touches the attribute shadows record_replay maintains).
 * The opcode census counts the packet, not the calls inside it. */
static void record_replay_bind_packet(const IsaacGlRecordHeader *h, const void *payload)
{
    const uint8_t *cursor = (const uint8_t *)payload;
    const uint8_t *end = cursor + h->payload_bytes;
    while (cursor < end) {
        RecordBindPacketOp op;
        IsaacGlRecordHeader sub;
        uint32_t step;
        if ((uint32_t)(end - cursor) < sizeof op)
            record_fault("GL recorder: truncated bind packet");
        memcpy(&op, cursor, sizeof op);
        step = (uint32_t)sizeof op + (((uint32_t)op.payload_bytes + 7u) & ~7u);
        if (step > (uint32_t)(end - cursor) || !record_bind_packet_opcode(op.opcode))
            record_fault("GL recorder: malformed bind packet");
        sub.bytes = step;
        sub.opcode = op.opcode;
        sub.return_rva = op.return_rva;
        sub.payload_bytes = op.payload_bytes;
        s_rva = op.return_rva;
        record_dispatch(&sub, cursor + sizeof op);
        cursor += step;
    }
}
#endif

static void record_custom_replay(const IsaacGlRecordHeader *h, const void *payload)
{
    const uint32_t *a = (const uint32_t *)payload;
    switch (h->opcode) {
#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
    case REC_KAGE_BIND_PACKET: record_replay_bind_packet(h, payload); break;
#endif
    case REC_glDrawElements: record_replay_draw(h, payload); break;
    case REC_NATIVE_BIND_RBO: s_rbo.bind(a[0], a[1]); break;
    case REC_NATIVE_STORE_RBO: s_rbo.store(a[0], a[1], (int32_t)a[2], (int32_t)a[3]); break;
    case REC_NATIVE_DELETE_RBO:
        s_rbo.remove((int32_t)a[0], h->payload_bytes > 16u ?
            (uint32_t)(uintptr_t)(a + 4) : a[1]); break;
    case REC_NATIVE_GEN_RBO: s_rbo.gen((int32_t)a[0], a[1]); break;
    case REC_WORKER_CALL: {
        void (*fn)(void *);
        void *context;
        memcpy(&fn, payload, sizeof fn);
        memcpy(&context, (const uint8_t *)payload + sizeof fn, sizeof context);
        fn(context);
        break;
    }
#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
    case REC_ATTRIBS_ENABLE: case REC_ATTRIBS_DISABLE: record_replay_attribs(h, payload); break;
#endif
    default: record_fault("GL recorder unknown opcode");
    }
}

#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
/* ---- RENDER60 W-pkt: producer staging of one native Shader::Bind -------
 * Game thread only.  Each stage call appends exactly the payload its typed
 * wrapper (gl_record_surface.inc / record_location) would have enqueued,
 * with the return RVA the translated call site would have pushed; flush
 * publishes the staged calls as one REC_KAGE_BIND_PACKET record. */
#define RECORD_BIND_PACKET_BYTES 512u
#define RECORD_BIND_MEMO_SLOTS 4u
#define RECORD_BIND_MEMO_NAME 16u   /* names up to 15 bytes are memoised */
static struct {
    uint64_t storage[RECORD_BIND_PACKET_BYTES / sizeof(uint64_t)];
    uint32_t used;
} s_bind_packet;
typedef struct RecordBindMemo {
    uint32_t epoch;                  /* 0 = empty */
    uint32_t program, name;
    int32_t location;
    uint32_t length;                 /* bytes before the NUL */
    char bytes[RECORD_BIND_MEMO_NAME];
} RecordBindMemo;
static RecordBindMemo s_bind_memo[RECORD_BIND_MEMO_SLOTS];
static uint32_t s_bind_stats[5];     /* packets, staged, memo, cache, cold */

int isaac_gl_record_bind_packet_admit(const guest_gl_backend *installed)
{
    if (!s_active || !installed || s_bind_packet.used)
        return 0;
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
    if (record_on_consumer())
        return 0;
#else
    if (record_replaying())
        return 0;
#endif
    /* The guest dispatch of each of the six slots ends in exactly these. */
    return installed->glUseProgram == record_glUseProgram &&
        installed->glActiveTexture == record_glActiveTexture &&
        installed->glBindTexture == record_glBindTexture &&
        installed->glGetUniformLocation == record_glGetUniformLocation &&
        installed->glUniform1i == record_glUniform1i &&
        installed->glUniformMatrix4fv == record_glUniformMatrix4fv;
}

static void record_bind_packet_stage(uint32_t opcode, uint32_t rva,
    const void *args, uint32_t arg_bytes, uint32_t client, uint32_t client_bytes)
{
    RecordBindPacketOp op;
    uint32_t payload = arg_bytes + client_bytes;
    uint32_t step = (uint32_t)sizeof op + ((payload + 7u) & ~7u);
    uint8_t *at;
    if (step > RECORD_BIND_PACKET_BYTES)
        record_fault("GL recorder: bind packet call too large");
    if (s_bind_packet.used + step > RECORD_BIND_PACKET_BYTES)
        isaac_gl_record_bind_packet_flush();
    at = (uint8_t *)s_bind_packet.storage + s_bind_packet.used;
    op.opcode = (uint16_t)opcode;
    op.payload_bytes = (uint16_t)payload;
    op.return_rva = rva;
    memcpy(at, &op, sizeof op);
    memcpy(at + sizeof op, args, arg_bytes);
    if (client_bytes)
        memcpy(at + sizeof op + arg_bytes, (const void *)(uintptr_t)client, client_bytes);
    memset(at + sizeof op + payload, 0, step - (uint32_t)sizeof op - payload);
    s_bind_packet.used += step;
    ++s_bind_stats[1];
}

void isaac_gl_record_bind_packet_use_program(uint32_t rva, guest_gl_uint program)
{
    Record_glUseProgram a;
    memset(&a, 0, sizeof a);
    a.program = program;
    record_bind_packet_stage(REC_glUseProgram, rva, &a, sizeof a, 0u, 0u);
}

void isaac_gl_record_bind_packet_active_texture(uint32_t rva, guest_gl_enum texture)
{
    Record_glActiveTexture a;
    memset(&a, 0, sizeof a);
    a.texture = texture;
    record_bind_packet_stage(REC_glActiveTexture, rva, &a, sizeof a, 0u, 0u);
}

void isaac_gl_record_bind_packet_bind_texture(uint32_t rva, guest_gl_enum target,
    guest_gl_uint texture)
{
    Record_glBindTexture a;
    memset(&a, 0, sizeof a);
    a.target = target;
    a.texture = texture;
    record_bind_packet_stage(REC_glBindTexture, rva, &a, sizeof a, 0u, 0u);
}

/* Remembered bytes are never NUL: a shorter current string mismatches at its
 * terminator, a longer one at the final NUL test; reads stop at the first
 * mismatch. */
static int record_bind_memo_bytes(const RecordBindMemo *memo, uint32_t name)
{
    const uint8_t *text = (const uint8_t *)(uintptr_t)name;
    uint32_t i;
    for (i = 0u; i < memo->length; ++i)
        if (text[i] != (uint8_t)memo->bytes[i])
            return 0;
    return text[i] == 0u;
}

/* record_location's hit path, staged: the same admission, key and cache
 * lookup, and the same record (arguments + the name copy of key.length + 1
 * bytes).  A memo hit stands for that lookup: same epoch (no reset since
 * the hit that filled it), same program, name address and name bytes, so
 * the cache would answer the same key with the same value.  A cache miss
 * stages nothing and returns 0; the caller flushes and calls the table's
 * glGetUniformLocation, whose record_location takes the synchronous path. */
int isaac_gl_record_bind_packet_uniform_location(uint32_t rva, uint32_t slot,
    guest_gl_uint program, guest_gl_addr name, guest_gl_int *location)
{
    uint32_t args[2];
    RecordBindMemo *memo = &s_bind_memo[slot & (RECORD_BIND_MEMO_SLOTS - 1u)];
    IsaacGlRecordLocationKey key;
    int32_t answer;
    args[0] = program;
    args[1] = name;
    if (memo->epoch == s_location_epoch && memo->program == program &&
            memo->name == name && record_bind_memo_bytes(memo, name)) {
        record_bind_packet_stage(REC_glGetUniformLocation, rva, args, sizeof args,
            name, memo->length + 1u);
        *location = memo->location;
        ++s_bind_stats[2];
        return 1;
    }
    if (record_readable(name, ISAAC_GL_RECORD_LOCATION_NAME_MAX + 1u) &&
            isaac_gl_record_location_key(ISAAC_GL_RECORD_LOCATION_UNIFORM, program,
                (const char *)(uintptr_t)name, ISAAC_GL_RECORD_LOCATION_NAME_MAX + 1u,
                &key) &&
            isaac_gl_record_locations_lookup(&s_locations, &key, &answer)) {
        record_bind_packet_stage(REC_glGetUniformLocation, rva, args, sizeof args,
            name, (uint32_t)key.length + 1u);
        if (key.length < RECORD_BIND_MEMO_NAME) {
            memo->program = program;
            memo->name = name;
            memo->location = answer;
            memo->length = key.length;
            memcpy(memo->bytes, key.name, (size_t)key.length + 1u);
            memo->epoch = s_location_epoch;
        } else {
            memo->epoch = 0u;
        }
        *location = answer;
        ++s_bind_stats[3];
        return 1;
    }
    ++s_bind_stats[4];
    return 0;
}

void isaac_gl_record_bind_packet_uniform1i(uint32_t rva, guest_gl_int location,
    guest_gl_int v0)
{
    Record_glUniform1i a;
    memset(&a, 0, sizeof a);
    a.location = location;
    a.v0 = v0;
    record_bind_packet_stage(REC_glUniform1i, rva, &a, sizeof a, 0u, 0u);
}

/* record_glUniformMatrix4fv's copy path with the copy taken from `matrix`
 * (the 64 bytes the translated body stores at `value`); the arguments keep
 * the guest address, which the replay replaces by the copy as before. */
void isaac_gl_record_bind_packet_uniform_matrix4fv(uint32_t rva,
    guest_gl_int location, guest_gl_sizei count, guest_gl_boolean transpose,
    guest_gl_addr value, const uint32_t matrix[16])
{
    Record_glUniformMatrix4fv a;
    if (count != 1)
        record_fault("GL recorder: bind packet matrix count must be 1");
    memset(&a, 0, sizeof a);
    a.location = location;
    a.count = count;
    a.transpose = transpose;
    a.value = value;
    record_bind_packet_stage(REC_glUniformMatrix4fv, rva, &a, sizeof a,
        (uint32_t)(uintptr_t)matrix, 64u);
}

void isaac_gl_record_bind_packet_flush(void)
{
    if (!s_bind_packet.used)
        return;
    record_enqueue(REC_KAGE_BIND_PACKET, s_bind_packet.storage, s_bind_packet.used, 0u, 0u);
    s_bind_packet.used = 0u;
    ++s_bind_stats[0];
}

void isaac_gl_record_bind_packet_stats(uint32_t out[5])
{
    memcpy(out, s_bind_stats, sizeof s_bind_stats);
}
#endif

#if defined(ISAAC_GL_VITA_BACKEND_ORACLE)
void isaac_gl_record_oracle_stats(uint32_t out[8])
{
    /* Process totals: every install restarts the ring. */
    out[0] = s_total_records + (s_active ? s_ring.records : 0u);
    out[1] = s_total_bytes + (s_active ? s_ring.copied_bytes : 0u);
    out[2] = s_ring.used;
    out[3] = s_total_syncs + (s_active ? s_syncs : 0u);
    out[4] = s_total_cold + (s_active ? s_location_cold : 0u);
    out[5] = s_locations.used;
    out[6] = s_snap_draws;
    out[7] = s_sync_draws;
}
#endif

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
#if defined(ISAAC_GL_RECORD_WORKER_JITTER)
/* Host oracle only: random consumer delays expose any missing copy/order. */
static uint32_t s_jitter = 0x2545f491u;
static void record_jitter(void)
{
    s_jitter = s_jitter * 1103515245u + 12345u;
    if (((s_jitter >> 16) & 255u) == 0u) Sleep(1); /* host-only (Windows) */
    else if (((s_jitter >> 16) & 7u) == 0u) SwitchToThread();
}
#else
# define record_jitter() ((void)0)
#endif

#if defined(_WIN32)
static HANDLE s_worker_handle;
#endif

static void record_worker_loop(void)
{
#if defined(_WIN32)
    s_render_identity = record_thread_identity();
#else
    __atomic_store_n(&s_render_identity, record_thread_identity(), __ATOMIC_RELEASE);
#endif
#if defined(ISAAC_VITA_GL_OWNER_CHECK) && ISAAC_VITA_GL_OWNER_CHECK
    isaac_gl_owner_claim();               /* before the first vitaGL call */
#endif
    s_rw.window_start = record_now_us();
    if (s_worker_init) s_worker_init();     /* the render thread owns vglInit */
    for (;;) {
        record_jitter();
        /* Busy = first record seen to idle: two clock reads per wake, none
         * per record; it starts before that record runs (closures read it). */
        if (!s_rw.busy && s_ring.read_pos != s_ring.write_pos) {
            s_rw.busy = 1u;
            s_rw.busy_since = record_now_us();
        }
        if (isaac_gl_record_spsc_consume_one(&s_ring)) continue;
        if (s_rw.busy) {
            s_rw.busy_us += record_now_us() - s_rw.busy_since;
            s_rw.busy = 0u;
        }
#if defined(_WIN32)
        if (InterlockedCompareExchange((volatile LONG *)&s_worker_stop, 0, 0)) break;
        isaac_gl_record_spsc_wait_work(&s_ring);
#else
        if (__atomic_load_n(&s_worker_stop, __ATOMIC_ACQUIRE)) break;
        isaac_gl_record_spsc_wait_work(&s_ring);
#endif
    }
}

#if defined(_WIN32)
static DWORD WINAPI record_worker_main(LPVOID unused)
{
    (void)unused;
    s_worker_thread_id = (int)GetCurrentThreadId();
    record_worker_loop();
    return 0;
}
#else
static int record_worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    s_worker_thread_id = sceKernelGetThreadId();
    record_worker_loop();
    return 0;
}
#endif

int isaac_gl_record_worker_start(void (*thread_init)(void))
{
    if (s_worker) return 0;
    memset(&s_ring, 0, sizeof s_ring);
    s_ring.replay = record_replay;
    s_worker_init = thread_init;
    s_worker_stop = 0;
#if defined(_WIN32)
    s_ring.producer_sema = (intptr_t)CreateSemaphoreA(NULL, 0, 1, NULL);
    s_ring.consumer_sema = (intptr_t)CreateSemaphoreA(NULL, 0, 1, NULL);
    if (!s_ring.producer_sema || !s_ring.consumer_sema) return 0;
    s_worker_handle = CreateThread(NULL, 0, record_worker_main, NULL, 0, NULL);
    s_worker = s_worker_handle != NULL;
#else
    {
        int thread;
        s_ring.producer_sema = sceKernelCreateSema("isaac_gl_produce", 0, 0, 1, NULL);
        s_ring.consumer_sema = sceKernelCreateSema("isaac_gl_consume", 0, 0, 1, NULL);
        if (s_ring.producer_sema < 0 || s_ring.consumer_sema < 0) return 0;
        /* USER_1: USER_2 carries the mixer, Vorbis, async log and trophy
         * workers; the game thread moves to USER_0. */
        thread = sceKernelCreateThread("isaac_gl_render", record_worker_main,
            ISAAC_GL_RENDER_THREAD_PRIORITY, ISAAC_GL_RENDER_THREAD_STACK,
            0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
        if (thread < 0) return 0;
        if (sceKernelStartThread(thread, 0, NULL) < 0) return 0;
        s_worker = 1;
    }
#endif
    return s_worker;
}

int isaac_gl_record_worker_active(void) { return s_worker; }
int isaac_gl_record_worker_thread_id(void) { return s_worker_thread_id; }
int isaac_gl_record_on_render_thread(void) { return record_on_consumer(); }

static uint32_t record_post_closure(void (*fn)(void *), void *context)
{
    uint8_t a[sizeof fn + sizeof context];
    uint32_t position;
    memcpy(a, &fn, sizeof fn);
    memcpy(a + sizeof fn, &context, sizeof context);
    position = isaac_gl_record_spsc_append(&s_ring, REC_WORKER_CALL, 0u, a, sizeof a,
        NULL, 0u, NULL, 0u);
    if (!position) record_fault("GL recorder could not publish a closure");
    s_last_pos = position;
    return position;
}

void isaac_gl_record_worker_call(void (*fn)(void *), void *context)
{
    if (record_on_consumer()) { fn(context); return; } /* already ordered here */
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    {
        uint32_t waits = s_pw.call_waits, us = s_pw.call_wait_us;
        record_wait(record_post_closure(fn, context), &s_pw.call_waits, &s_pw.call_wait_us);
        record_census_wait(REC_WORKER_CALL, s_pw.call_waits - waits, s_pw.call_wait_us - us);
    }
#else
    record_wait(record_post_closure(fn, context), &s_pw.call_waits, &s_pw.call_wait_us);
#endif
}

uint32_t isaac_gl_record_worker_post(void (*fn)(void *), void *context)
{
    if (record_on_consumer()) { fn(context); return 0u; }
#if defined(ISAAC_VITA_GL_THREAD_WORKER_PROOF) && ISAAC_VITA_GL_THREAD_WORKER_PROOF
    isaac_gl_record_spsc_wait_done(&s_ring, record_post_closure(fn, context));
    return 0u;
#else
    {
        /* PRESENT: the frame is complete, let the render thread run it now. */
        uint32_t position = record_post_closure(fn, context);
        isaac_gl_record_spsc_kick(&s_ring);
        return position;
    }
#endif
}

void isaac_gl_record_worker_wait(uint32_t position)
{
    if (position && !record_on_consumer())
        record_wait(position, &s_pw.held, &s_pw.held_us);
}

void isaac_gl_record_worker_stop(void)
{
    if (!s_worker) return;
#if defined(_WIN32)
    InterlockedExchange((volatile LONG *)&s_worker_stop, 1);
    isaac_gl_record_spsc_kick(&s_ring);
    WaitForSingleObject(s_worker_handle, INFINITE);
    CloseHandle(s_worker_handle);
    s_worker_handle = NULL;
    s_worker = 0;
#else
    /* The device render thread lives as long as the vitaGL context, which
     * has no teardown (kage_vita_backend.c header). */
#endif
}

uint32_t isaac_gl_record_worker_producer_waits(void) { return s_ring.producer_waits; }

static uint64_t record_busy_now(void)
{
    return s_rw.busy_us + (s_rw.busy ? record_now_us() - s_rw.busy_since : 0u);
}

void isaac_gl_record_worker_frame_end(uint32_t swap_us)
{
    uint64_t busy = record_busy_now();
    if (s_rw.frames < ISAAC_GL_RECORD_FRAME_SAMPLES)
        s_rw.frame_us[s_rw.frames++] = (uint32_t)(busy - s_rw.frame_busy_mark);
    else
        ++s_rw.frame_overflow;
    s_rw.frame_busy_mark = busy;
    s_rw.swap_us += swap_us;
}

/* Render thread: its half of the window, then reset it. */
static void record_take_render(void *context)
{
    IsaacGlRecordWorkerWindow *out = (IsaacGlRecordWorkerWindow *)context;
    uint64_t now = record_now_us(), busy = record_busy_now();
    uint32_t n = s_rw.frames, i, j;
    if (out) {
        uint32_t *f = s_rw.frame_us;
        for (i = 1u; i < n; ++i) {          /* insertion sort, n <= 256 */
            uint32_t v = f[i];
            for (j = i; j && f[j - 1u] > v; --j) f[j] = f[j - 1u];
            f[j] = v;
        }
        out->wall_us = (uint32_t)(now - s_rw.window_start);
        out->busy_us = (uint32_t)(busy - s_rw.window_busy_mark);
        out->frames = n;
        out->frame_overflow = s_rw.frame_overflow;
        /* nearest rank */
        out->frame_p50_us = n ? f[(n + 1u) / 2u - 1u] : 0u;
        out->frame_p95_us = n ? f[(n * 95u + 99u) / 100u - 1u] : 0u;
        out->frame_max_us = n ? f[n - 1u] : 0u;
        out->swap_us = s_rw.swap_us;
        out->consumer_sleeps = s_ring.consumer_sleeps - s_rw.sleeps_base;
        out->consumer_timeouts = s_ring.consumer_timeouts - s_rw.timeouts_base;
    }
    s_rw.sleeps_base = s_ring.consumer_sleeps;
    s_rw.timeouts_base = s_ring.consumer_timeouts;
    s_rw.window_start = now;
    s_rw.window_busy_mark = busy;
    s_rw.frames = s_rw.frame_overflow = s_rw.swap_us = 0u;
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    if (out) {
        uint64_t first = record_now_us(), last = first;
        for (i = 0u; i < 16u; ++i) last = record_now_us();
        s_census.ck16_us = (uint32_t)(last - first);
        for (i = 0u; i < ISAAC_GL_RECORD_CENSUS_OPS; ++i) {
            const RecordCensusReplay *r = &s_census_replay[i];
            IsaacGlRecordOpcodeStats *o = &s_census.op[i];
            o->n = r->n; o->b = r->b; o->e = r->e; o->eus = r->eus;
            o->rs = r->rs; o->rus = r->rus;
        }
    }
    memset(s_census_replay, 0, sizeof s_census_replay);
#endif
}

void isaac_gl_record_worker_take(IsaacGlRecordWorkerWindow *out)
{
    if (!s_worker || record_on_consumer()) {
        if (out) memset(out, 0, sizeof *out);
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
        if (out) memset(&s_census, 0, sizeof s_census);
#endif
        return;
    }
    /* Producer half first: this take's own wait lands in the next window. */
    if (out) {
        memset(out, 0, sizeof *out);
        out->ring_high_water = s_ring.high_water;
        out->records = s_ring.records - s_pw.records_base;
        out->syncs = s_pw.syncs;
        out->sync_waits = s_pw.sync_waits;
        out->sync_wait_us = s_pw.sync_wait_us;
        out->held = s_pw.held;
        out->held_us = s_pw.held_us;
        out->call_waits = s_pw.call_waits;
        out->call_wait_us = s_pw.call_wait_us;
        out->ring_full_waits = s_ring.producer_waits - s_pw.ring_full_base;
        out->wakes = s_ring.wakes - s_pw.wakes_base;
        out->mb_calls = s_pw.mb_calls;
        out->mb_samples = s_pw.mb_samples;
        out->mb_us = s_pw.mb_us;
        out->ap_samples = s_pw.ap_samples;
        out->ap_us = s_pw.ap_us;
        out->copied_bytes = s_ring.copied_bytes - s_pw.copied_base;
        out->mb_queries = s_pw.mb_queries;
        out->clock_us = s_pw.clock_us;
        out->render_skips = s_pw.render_skips;
    }
    s_ring.high_water = 0u;
    s_pw.records_base = s_ring.records;
    s_pw.ring_full_base = s_ring.producer_waits;
    s_pw.syncs = s_pw.sync_waits = s_pw.sync_wait_us = s_pw.held = s_pw.held_us = 0u;
    s_pw.call_waits = s_pw.call_wait_us = 0u;
    s_pw.wakes_base = s_ring.wakes;
    s_pw.copied_base = s_ring.copied_bytes;
    s_pw.mb_calls = s_pw.mb_samples = s_pw.mb_us = s_pw.ap_samples = s_pw.ap_us = 0u;
    s_pw.mb_queries = s_pw.clock_us = s_pw.render_skips = 0u;
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
    if (out) {
        uint32_t i;
        for (i = 0u; i < ISAAC_GL_RECORD_CENSUS_OPS; ++i) {
            const RecordCensusProduce *p = &s_census_produce[i];
            IsaacGlRecordOpcodeStats *o = &s_census.op[i];
            o->ps = p->ps; o->pst = p->pst; o->pus = p->pus;
            o->sy = p->sy; o->sw = p->sw; o->swus = p->swus;
        }
    }
    /* No stamp can be pending: takes run between wrappers, never inside. */
    memset(s_census_produce, 0, sizeof s_census_produce);
#endif
    isaac_gl_record_worker_call(record_take_render, out);
}

#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
const IsaacGlRecordOpcodeCensus *isaac_gl_record_opcode_census(void) { return &s_census; }
#endif

/* Producer: has the render thread NOT yet finished the record ending at
 * position (0 = nothing posted)?  Never waits. */
int isaac_gl_record_worker_behind(uint32_t position)
{
    return s_worker && position && !record_on_consumer() &&
        !isaac_gl_record_spsc_reached(&s_ring, position);
}

void isaac_gl_record_worker_note_render_skip(void) { ++s_pw.render_skips; }

void isaac_gl_record_worker_quiesce(void)
{
    if (s_worker && !record_on_consumer())
        isaac_gl_record_drain();
}
#endif
