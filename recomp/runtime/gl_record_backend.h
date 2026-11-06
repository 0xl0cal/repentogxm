#ifndef ISAAC_GL_RECORD_BACKEND_H
#define ISAAC_GL_RECORD_BACKEND_H

#include "gl_bridge.h"

/* Raw vitaGL endpoints below the shim (the shim's own calls are interposed). */
typedef struct IsaacGlRecordRboNative {
    void (*bind)(uint32_t, uint32_t);
    void (*store)(uint32_t, uint32_t, int32_t, int32_t);
    void (*remove)(int32_t, uint32_t);
    void (*gen)(int32_t, uint32_t);
    void (*attrib_pointer)(uint32_t, int32_t, uint32_t, uint8_t, int32_t, uint32_t);
    void (*draw)(uint32_t, int32_t, uint32_t, uint32_t);
    int (*canonical)(int32_t); /* NULL without the canonical quad path */
} IsaacGlRecordRboNative;

/* Native draw interposers: a replayed snapshot draw rebinds the enabled
 * attributes onto the ring copy around the one native draw, then restores
 * the guest pointers, so vitaGL state after the draw equals direct. */
void isaac_gl_record_native_draw(uint32_t mode, int32_t count, uint32_t type, uint32_t indices);
int isaac_gl_record_native_canonical(int32_t count);

/* Inline stage only. All rendering continues on the caller's original thread. */
void isaac_gl_record_wrap(guest_gl_backend *backend, const IsaacGlRecordRboNative *rbo);
void isaac_gl_record_drain(void);
void isaac_gl_record_stop(void);
void isaac_gl_record_external_begin(void);
void isaac_gl_record_before_fault(void);
int isaac_gl_record_return_rva(uint32_t *rva);
void isaac_gl_record_rbo_bind(uint32_t target, uint32_t name);
void isaac_gl_record_rbo_store(uint32_t target, uint32_t format, int32_t w, int32_t h);
void isaac_gl_record_rbo_delete(int32_t count, uint32_t names);
void isaac_gl_record_rbo_gen(int32_t count, uint32_t names);

/* Recorder opcode slots: slot 0 (wrap/out of range) plus one per opcode.
 * The generated surface ends at REC_WORKER_CALL = 78; RENDER60 S4
 * (ISAAC_VITA_GL_RECORD_ATTRIB_BATCH) appends REC_ATTRIBS_ENABLE = 79 and
 * REC_ATTRIBS_DISABLE = 80 in gl_record_backend.c, and RENDER60 W-pkt
 * (ISAAC_VITA_KAGE_BIND_PACKET) REC_KAGE_BIND_PACKET = 81; 79/80 stay
 * reserved when S4 is OFF, so numbers never depend on the knob mix.
 * gl_record_backend.c static-asserts the bound in every shape.  Any TU that
 * sizes a per-opcode table from it (the census reader
 * kage_vita_phase_profile.c) must see the same defines. */
#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
# define ISAAC_GL_RECORD_OPCODE_SLOTS 82u
#elif defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
# define ISAAC_GL_RECORD_OPCODE_SLOTS 81u
#else
# define ISAAC_GL_RECORD_OPCODE_SLOTS 79u
#endif

#if defined(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)
/* RENDER60 S4: one ring record per Shader::EnableAttribs/DisableAttribs
 * replay (host_vita_shader_attrib_fastpath.c) on a location-memo hit.
 * ready() is 1 only while the recorder is active, 1 <= count <= 16 and the
 * installed table's lookup and toggle (+ pointer for Enable) members are the
 * recorder's own; enable()/disable() then apply exactly the producer shadow
 * notes the per-call record_gl* members would, in the per-call order, and
 * publish ONE record that the consumer unpacks into the same s_target member
 * calls, with the same arguments, in the same order (shadow update before
 * each call, as record_replay does per record).  Locations are passed as the
 * wrappers receive them (guest_gl_uint), -1 included.  The type constant is
 * a checked copy of ISAAC_VITA_SHADER_ATTRIB_GL_FLOAT (static-asserted in
 * the fastpath TU). */
#define ISAAC_GL_RECORD_ATTRIB_BATCH_TYPE 0x1406u   /* GL_FLOAT */
#define ISAAC_GL_RECORD_ATTRIB_BATCH_MAX 16u        /* == RECORD_ATTRIBS */
/* REC_WORKER_CALL + 1 / + 2 (static-asserted in gl_record_backend.c); the
 * replay TU prints them in its one-shot banner. */
#define ISAAC_GL_RECORD_ATTRIBS_ENABLE_OPCODE 79u
#define ISAAC_GL_RECORD_ATTRIBS_DISABLE_OPCODE 80u
int isaac_gl_record_attribs_batch_ready(const guest_gl_backend *installed,
    uint32_t count, int enable);
void isaac_gl_record_attribs_enable(uint32_t count, const guest_gl_int *locations,
    const uint8_t *components, int32_t stride, uint32_t base);
void isaac_gl_record_attribs_disable(uint32_t count, const guest_gl_int *locations);
# if defined(ISAAC_GL_VITA_BACKEND_ORACLE) || defined(ORACLE_ATTRIB_RECORDER)
/* Host oracles only: the table isaac_gl_record_wrap installed (what
 * guest_gl_installed_backend() returns once the bridge copied it), the
 * batch counters {enable records, disable records, attributes, batches
 * that started after a ring wrap marker (worker builds)}, a raw copy
 * of the producer/consumer attribute shadows (RecordAttribShadow), and a
 * poison fill of one of them (the producer's on the game thread, the
 * consumer's on the render thread, each by its owner), and one shadow slot
 * as {pointer, type, size, stride, normalized, enabled, enable known,
 * pointer known} (0 for index >= 16). */
const guest_gl_backend *isaac_gl_record_oracle_installed(void);
void isaac_gl_record_oracle_batch_stats(uint32_t out[4]);
uint32_t isaac_gl_record_oracle_shadow(int consumer, void *out, uint32_t capacity);
void isaac_gl_record_oracle_shadow_poison(int consumer, uint8_t byte);
int isaac_gl_record_oracle_shadow_slot(int consumer, uint32_t index, uint32_t out[8]);
# endif
#endif

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* Host stage 2: one consumer thread replays every record; thread_init runs
 * first on it (the render thread owns vglInit). worker_call runs fn on the
 * consumer and waits (install/uninstall/state reads belong to that thread). */
int isaac_gl_record_worker_start(void (*thread_init)(void));
void isaac_gl_record_worker_call(void (*fn)(void *), void *context);
/* Async closure (e.g. PRESENT): returns its ring position for a later wait. */
uint32_t isaac_gl_record_worker_post(void (*fn)(void *), void *context);
void isaac_gl_record_worker_wait(uint32_t position);
int isaac_gl_record_worker_active(void);
#if defined(ISAAC_VITA_GL_THREAD_WORKER_PROOF) && ISAAC_VITA_GL_THREAD_WORKER_PROOF
# define ISAAC_GL_RECORD_WORKER_MODE_NAME "ownership-proof"
#else
# define ISAAC_GL_RECORD_WORKER_MODE_NAME "async"
#endif
int isaac_gl_record_worker_thread_id(void);
int isaac_gl_record_on_render_thread(void);
void isaac_gl_record_worker_stop(void);
uint32_t isaac_gl_record_worker_producer_waits(void);
/* Per-window render-thread figures (ph120.rt).  Waits are the game thread's
 * blocked time only (a wait that found its position done costs no clock
 * read): s = value-returning calls (S), h = present waiting for the
 * previous present (frame held), c = closures and drains (includes this
 * take, one per window), rf = ring-full sleeps.  The render-thread half is
 * taken BY the render thread inside a closure, so take() also waits for
 * every earlier record: after it returns the render thread is idle and the
 * caller may read vitaGL statistics.  NULL discards. */
#define ISAAC_GL_RECORD_FRAME_SAMPLES 256u
typedef struct IsaacGlRecordWorkerWindow {
    uint32_t wall_us, busy_us;              /* render thread */
    uint32_t frames, frame_overflow;        /* present closures; beyond the cap */
    uint32_t frame_p50_us, frame_p95_us, frame_max_us; /* busy per present */
    uint32_t swap_us;                       /* vglSwapBuffers inside them */
    uint32_t ring_high_water, records;
    uint32_t syncs, sync_waits, sync_wait_us;
    uint32_t held, held_us, call_waits, call_wait_us, ring_full_waits;
    /* ph120.rw: wake policy and producer cost.  wakes = producer semaphore
     * signals; consumer sleeps / timeouts that found unkicked work; memory
     * admission calls, of which 1/64 timed (samples, µs); appends 1/64
     * timed (samples, µs); bytes copied into the ring. */
    uint32_t wakes, consumer_sleeps, consumer_timeouts;
    uint32_t mb_calls, mb_samples, mb_us, ap_samples, ap_us, copied_bytes;
    uint32_t mb_queries;                    /* admission syscalls (cache misses) */
    uint32_t clock_us;      /* clock-pair cost on the ap samples: true append = ap.us - clock_us */
    uint32_t render_skips;  /* interpolated renders declined, render thread behind */
} IsaacGlRecordWorkerWindow;
int isaac_gl_record_worker_behind(uint32_t position);
void isaac_gl_record_worker_note_render_skip(void);
#if defined(__vita__)
/* Every sceKernelFreeMemBlock wrapper calls this before the real free. */
void isaac_gl_record_memblock_freeing(void);
# if defined(ISAAC_GL_RECORD_ADMISSION_CACHE_VERIFY)
/* Measurement builds (GL_RECORD_ADMISSION_CACHE_VERIFY), producer thread:
 * the boot self-test (1 = {1,1,0,0}, 0 = other values, -1 = no page) and
 * the counts since it: {wraps, misses, verified hits}. */
int isaac_gl_record_admission_selftest(uint32_t out[4]);
void isaac_gl_record_admission_verify_stats(uint32_t out[3]);
# endif
#endif
void isaac_gl_record_worker_take(IsaacGlRecordWorkerWindow *out);
#if defined(ISAAC_GL_RECORD_OPCODE_CENSUS)
/* Measurement builds (GL_RECORD_OPCODE_CENSUS): per-opcode figures of the
 * window the last isaac_gl_record_worker_take(non-NULL) closed; a NULL take
 * discards the live counts and leaves this copy alone.  Slot = opcode
 * (REC_WORKER_CALL = 78 is the last generated one, RENDER60 S4 adds 79/80,
 * W-pkt 81; slot 0 = out of range, expected 0).
 * Render thread: n records replayed, b their payload bytes; e/eus exactly
 * timed replays (the first ISAAC_GL_RECORD_CENSUS_EXACT of each opcode in
 * the window, and every closure); rs/rus randomly timed replays among the
 * rest (P = 1/64).  Game thread: ps sampled appends (P = 1/64, the ph120.rw
 * ap samples), pus their microseconds from the admission check (pst of them
 * started there) or else the append start, to the append end; sy/sw/swus =
 * S waits after this opcode (op 78: closure calls), of which blocked, and
 * blocked microseconds.  ck16_us = 16 back-to-back clock intervals on the
 * render thread (its per-read cost).  Every timed span holds one clock read
 * (two when stamped at the admission check): subtract it when estimating. */
#define ISAAC_GL_RECORD_CENSUS_OPS ISAAC_GL_RECORD_OPCODE_SLOTS
#define ISAAC_GL_RECORD_CENSUS_EXACT 32u
typedef struct IsaacGlRecordOpcodeStats {
    uint32_t n, b, e, eus, rs, rus;         /* render thread */
    uint32_t ps, pst, pus, sy, sw, swus;    /* game thread */
} IsaacGlRecordOpcodeStats;
typedef struct IsaacGlRecordOpcodeCensus {
    uint32_t ck16_us;
    IsaacGlRecordOpcodeStats op[ISAAC_GL_RECORD_CENSUS_OPS];
} IsaacGlRecordOpcodeCensus;
const IsaacGlRecordOpcodeCensus *isaac_gl_record_opcode_census(void);
#endif
/* Wait until the render thread ran everything published (no-op without a
 * running worker or on the render thread itself). */
void isaac_gl_record_worker_quiesce(void);
/* Render thread, at the end of every present closure. */
void isaac_gl_record_worker_frame_end(uint32_t swap_us);
#endif

#if defined(ISAAC_VITA_KAGE_BIND_PACKET)
/* ISAAC_VITA_KAGE_BIND_PACKET (host_vita_kage_bind_packet.c): the native
 * KAGE Shader::Bind stages its typed GL calls here, game thread only, and
 * publishes them as ONE ring record (REC_KAGE_BIND_PACKET) whose replay
 * runs each staged call through record_dispatch with its own return RVA,
 * exactly as the separate records would have run.
 *
 * admit: 1 when the recorder is installed, the caller is not the render
 * thread and the six entry points of `installed` (the guest bridge's
 * table) are this recorder's own wrappers; 0 = the caller must not stage.
 * The stage functions append one call each; uniform_location answers from
 * the per-name memo or the producer location cache (staging the original
 * lookup with its name copy, as record_location's hit path enqueues it)
 * and returns 0 without staging on a cache miss: the caller then flushes
 * and makes the ordinary synchronous call.  flush publishes what is staged
 * (nothing staged: no record).  `slot` selects the memo entry (0..3).
 * stats: {packets, staged calls, memo hits, cache hits, cold misses}. */
int isaac_gl_record_bind_packet_admit(const guest_gl_backend *installed);
void isaac_gl_record_bind_packet_use_program(uint32_t rva, guest_gl_uint program);
void isaac_gl_record_bind_packet_active_texture(uint32_t rva, guest_gl_enum texture);
void isaac_gl_record_bind_packet_bind_texture(uint32_t rva, guest_gl_enum target,
    guest_gl_uint texture);
int isaac_gl_record_bind_packet_uniform_location(uint32_t rva, uint32_t slot,
    guest_gl_uint program, guest_gl_addr name, guest_gl_int *location);
void isaac_gl_record_bind_packet_uniform1i(uint32_t rva, guest_gl_int location,
    guest_gl_int v0);
void isaac_gl_record_bind_packet_uniform_matrix4fv(uint32_t rva,
    guest_gl_int location, guest_gl_sizei count, guest_gl_boolean transpose,
    guest_gl_addr value, const uint32_t matrix[16]);
void isaac_gl_record_bind_packet_flush(void);
void isaac_gl_record_bind_packet_stats(uint32_t out[5]);
#endif

#if defined(ISAAC_GL_VITA_BACKEND_ORACLE)
void isaac_gl_record_oracle_stats(uint32_t out[8]);
#endif

#endif
