#ifndef ISAAC_HOST_VITA_NATIVE_VORBIS_H
#define ISAAC_HOST_VITA_NATIVE_VORBIS_H

#include <stddef.h>
#include <stdint.h>

/* Native stb_vorbis for the frozen KAGE::Sound OGG path (ISAAC_VITA_NATIVE_VORBIS).
 *
 * MECHANISM.  The generated caller (StreamSourceOgg::Decode, sub_005a20e0 in
 * guest_0174.c, and the whole-sample loader sub_005a36a0) and the callee
 * stb_vorbis_get_samples_short_interleaved (sub_005bdf80, guest_0177.c) live
 * in different translation units, so the eboot link wraps the callee with
 * GNU ld --wrap=sub_005bdf80: every cross-unit reference (the three direct
 * call sites and the guest_table.c dispatch slot) resolves to
 * __wrap_sub_005bdf80 below, and __real_sub_005bdf80 remains the untouched
 * translated body used for every fallback.  The same mechanism observes the
 * three sibling entries the guest uses around a stream:
 *   0x005b7740 set_file_offset      (Decode's inlined stb_vorbis_seek_start)
 *   0x005bb260 vorbis_pump_first_frame
 *   0x005bd310 vorbis_deinit        (whole-sample path teardown)
 * Their wrappers only mark the mirrored stream dirty/retired and then call
 * the real body; nothing about the guest is changed.  Wrapping is exact
 * because the generated bodies are plain C functions taking the CPU context:
 * the wrapper decodes the MSVC/LTCG custom convention read off the
 * translated prologue (`mov ebx, ecx` = stb_vorbis *f, `mov esi, edx` =
 * channels, [ebp+8] = short *buffer, [ebp+0xc] = num_shorts; caller cleans
 * the two stack words with `add esp, 8`; result in eax; ebx/esi/edi/ebp
 * preserved because the wrapper never writes them) and performs exactly the
 * translated epilogue's `ret`: eax = frames, pop the return word.
 *
 * DECODER.  recomp/runtime/third_party/stb_vorbis/stb_vorbis.c is upstream
 * v1.04 (commit b8e0530fdfbe), the exact revision the PE links (see the
 * README next to it).  It is compiled natively for Cortex-A9 with the guest's
 * compile-time options and with fgetc/fread/fseek/ftell routed to this
 * module, which reproduces the game's own edit (FILE * -> KAGE stream
 * object).  No translated code runs inside the native decode.
 *
 * MODEL (b), host-owned mirrors.  The guest's state object (0x5f8 bytes,
 * allocated together with every table inside the guest's 0x4b000-byte
 * stb_vorbis_alloc buffer, StreamSourceOgg+0x94) is never decoded in place.
 * On the first wrapped call for a state the game thread CLONES it: the state
 * and the used prefix of its alloc buffer are memcpy'd into host memory and
 * every internal pointer (codebooks and their five arrays, floor/residue/
 * mapping configs, residue classdata rows, channel/previous/finalY buffers,
 * twiddles, windows, bit-reverse tables) is relocated by the buffer delta.
 * A pointer that is neither NULL nor inside the guest buffer rejects the
 * clone and the translated decoder is used unchanged.  Because the clone is
 * bit-identical to the guest decoder state, including the sin/cos-derived
 * tables the translated start_decoder computed, the native decode consumes
 * the same bytes and the same tables as the translated one; the VERIFY
 * build proves the PCM equality on device.
 *
 * The OGG bytes are read ahead on the GAME THREAD through the guest's own
 * stream object (guest_call into vtable slots +0x08 tell, +0x0c seek,
 * +0x14 read, i.e. the same calls the translated get8/getn/set_file_offset
 * make, but in ring-sized blocks instead of one byte per virtual call) into
 * a per-stream host ring.  A SceKernel worker pinned to CPU 2 decodes from
 * that ring into a per-stream host PCM ring ahead of time.  The wrapped call
 * then only memcpy's ready PCM into the guest buffer.  If the PCM ring is
 * short the game thread decodes the missing frames itself, natively, on the
 * same mirror (ownership below), so no call ever waits for the worker beyond
 * one in-flight frame.  Whole-sample (memory-mode) states are decoded
 * synchronously and natively on the game thread from their clone; they never
 * involve the worker.
 *
 * THREADS AND MEMORY.  The worker touches only host memory: the mirror
 * (state, clone buffer, rings, scratch) and the pool bookkeeping.  It never
 * dereferences a guest address, never sees a CPU context and calls no CRT
 * or guest function; it makes no allocation.  All allocations happen on the
 * game thread.  Ordering:
 *   - rings are single-producer/single-consumer byte rings with free-running
 *     32-bit head/tail counters; the producer publishes with a RELEASE store
 *     of head after writing bytes, the consumer reads head with ACQUIRE and
 *     retires bytes with a RELEASE store of tail, which the producer reads
 *     with ACQUIRE (OGG ring: game thread -> decoder; PCM ring: decoder ->
 *     game thread);
 *   - the decoder role is exclusive: `busy` is taken with a compare-exchange
 *     (ACQUIRE) and dropped with a RELEASE store, so the mirror state written
 *     by one decoder is visible to the next; the game thread waits for an
 *     in-flight worker frame with 50 us delays, bounded;
 *   - `active`/`retiring`/`pcm_eof`/`ogg_eof` are RELEASE-stored by their
 *     writer and ACQUIRE-loaded by the other thread;
 *   - the worker is woken through a SceKernel event flag and otherwise polls
 *     every ISAAC_NV_WORKER_IDLE_US.
 *
 * FALLBACKS (all keep the guest state authoritative):
 *   - clone rejected (no alloc buffer, out-of-buffer pointer, implausible
 *     header, pool exhausted and nothing evictable, allocation failure):
 *     __real_sub_005bdf80 runs; counted in the receipt;
 *   - channels <= 0: __real (the translated body faults on the idiv exactly
 *     as before);
 *   - the guest re-opened a stream at the same address: detected through the
 *     marker written into the guest state's setup_temp_memory_required and
 *     the identity fields; the stale mirror is re-cloned;
 *   - seek_start/pump on a mirrored state: the mirror is marked dirty and
 *     re-cloned on the next call after the translated pump (loop restart);
 *   - decoder assertion or ring underrun inside a mirror: that mirror
 *     reports end of stream (n < requested), the guest loops as it does at a
 *     real end, and the next call re-clones; never abort().
 *
 * VERIFY (ISAAC_VITA_NATIVE_VORBIS_VERIFY).  Each wrapped call first
 * produces the native PCM into host scratch, then seeks the guest stream to
 * the guest state's own position and runs __real_sub_005bdf80 on the real
 * guest state, compares frame counts and int16 samples, logs bounded
 * `KAGE VITA NATIVE VORBIS VERIFY` lines and returns the TRANSLATED result
 * to the guest, so a mismatch can never reach the speakers.
 *
 * ASYNC (ISAAC_VITA_NATIVE_VORBIS_ASYNC, default OFF; requires the worker).
 * Measured on perf:wf-opt-v7: the first four slots of every
 * stream are served synchronously at 47-61 ms each (the mirror is created
 * inside the first Decode, the rings are empty, the game thread decodes the
 * whole 16384-frame slot itself), and every steady slot pays ~2.8 ms for
 * two byte-wise CRC32 passes over 64 KiB on top of its guest stream read.
 * Three changes, all behind the knob, none touching the guest:
 *   D. the receipt CRCs leave the serving path: the running CRC is folded
 *      in by whichever decoder writes a frame into the PCM ring (the worker
 *      for worker frames), the per-slot CRC is computed only for the slots
 *      whose SLOT line is actually logged, and only under RECEIPT/VERIFY;
 *      `run=` is therefore the CRC of the PCM produced into the ring, which
 *      equals the CRC of the PCM delivered once the stream has drained;
 *   A. partial slots: a wrapped call returns whatever PCM is ready when that
 *      is at least ISAAC_NV_SLOT_MIN_FRAMES (2048 = 43 ms at 48 kHz); below
 *      that the game thread decodes on the mirror only up to MIN_FRAMES and
 *      only inside ISAAC_NV_SYNC_BUDGET_US (3000), always at least one
 *      frame, so 0 frames is returned only at a real end of stream.  The
 *      worker is excluded (game_waiting) only inside that bounded path.
 *      StreamSourceOgg::Decode accepts any positive count (jg at 0x5a2154)
 *      and hands it to alBufferData, so only buffer boundaries move; the
 *      PCM order is unchanged and the queue returns to 4 x 341 ms as soon
 *      as the worker is two slots ahead.  A call that gave up waiting for
 *      the worker's frame (ISAAC_NV_ACQUIRE_TIMEOUT_US, nothing ready)
 *      drops the mirror and runs the translated body from the guest state,
 *      whatever the OGG ring says about the file's end: 0 frames reach the
 *      guest only at a real end of stream;
 *   B. lead time: stb_vorbis_open_file (sub_005bd740, StreamSourceOgg::Open
 *      at 0x5a2346 / 0x5a240e) is wrapped too; after the translated body
 *      returns the state, the mirror is cloned right there, the OGG ring is
 *      primed with ISAAC_NV_EARLY_TOPUP_CHUNKS guest reads and the worker
 *      is signalled, so the first Decode (the next loop's service phase)
 *      finds PCM ready instead of an empty ring.  A guest seek between the
 *      open and the first Decode goes through the wrapped set_file_offset
 *      (dirty -> RECLONE-seek), exactly like a loop restart.  The only stb
 *      entries StreamSourceOgg (guest_0174.c) calls are open_file,
 *      open_memory, get_samples, set_file_offset+pump and deinit.
 * ASYNC and VERIFY are mutually exclusive: VERIFY compares per-call frame
 * counts against the translated body, which partial slots break by design;
 * run VERIFY with ASYNC off.  With ASYNC off every object is byte-identical
 * to the build without this option. */

/* Frozen RVAs (8,650,240-byte Repentance PE, image base GUEST_IMAGE_BASE). */
#define ISAAC_NV_GET_SAMPLES_RVA        0x005bdf80U
#define ISAAC_NV_SET_FILE_OFFSET_RVA    0x005b7740U
#define ISAAC_NV_PUMP_FIRST_FRAME_RVA   0x005bb260U
#define ISAAC_NV_DEINIT_RVA             0x005bd310U
#define ISAAC_NV_OPEN_STREAM_RVA        0x005bd740U
#define ISAAC_NV_OPEN_MEMORY_RVA        0x005bd8b0U
#define ISAAC_NV_DECODE_RVA             0x005a20e0U

/* Return markers pushed before the guest stream virtual calls: the exact
 * translated call sites inside get8 (read), getn (read) and set_file_offset
 * (seek, tell), so a host adapter that keys on return addresses sees the
 * same words the translated decoder would have pushed. */
#define ISAAC_NV_SITE_GET8_READ         0x005b7582U
#define ISAAC_NV_SITE_GETN_READ         0x005b772cU
#define ISAAC_NV_SITE_SEEK              0x005b77aeU
#define ISAAC_NV_SITE_TELL              0x005b77b6U

/* KAGE stream vtable slots used by the game's stb_vorbis edit. */
#define ISAAC_NV_STREAM_VT_LENGTH       0x04U
#define ISAAC_NV_STREAM_VT_TELL         0x08U
#define ISAAC_NV_STREAM_VT_SEEK         0x0cU
#define ISAAC_NV_STREAM_VT_READ         0x14U

/* Frozen v1.04 state layout (also enforced by _Static_assert on Vita). */
#define ISAAC_NV_STATE_BYTES            0x5f8U
#define ISAAC_NV_STATE_STREAM_OFFSET    0x14U   /* FILE *f -> KAGE stream */
#define ISAAC_NV_STATE_EOF_OFFSET       0x70U
#define ISAAC_NV_STATE_MARKER_OFFSET    0x10U   /* setup_temp_memory_required */
#define ISAAC_NV_GUEST_ALLOC_BYTES      0x4b000U

/* Pool and ring geometry (bytes; rings are powers of two). */
#ifndef ISAAC_NV_MIRRORS_MAX
#define ISAAC_NV_MIRRORS_MAX            20U
#endif
#ifndef ISAAC_NV_OGG_RING_BYTES
#define ISAAC_NV_OGG_RING_BYTES         (64U * 1024U)
#endif
#ifndef ISAAC_NV_PCM_RING_BYTES
#define ISAAC_NV_PCM_RING_BYTES         (128U * 1024U)
#endif
/* Bytes that must be in the OGG ring (or end of stream) before the decoder
 * starts one more frame; one Vorbis packet is a few KiB. */
#ifndef ISAAC_NV_OGG_GUARD_BYTES
#define ISAAC_NV_OGG_GUARD_BYTES        (24U * 1024U)
#endif
/* Decode-time temp headroom appended to the cloned setup prefix. */
#ifndef ISAAC_NV_TEMP_RESERVE_BYTES
#define ISAAC_NV_TEMP_RESERVE_BYTES     (96U * 1024U)
#endif
/* Largest guest stream read per wrapped call (the read runs translated
 * archive code on the game thread). */
#ifndef ISAAC_NV_TOPUP_CHUNK_BYTES
#define ISAAC_NV_TOPUP_CHUNK_BYTES      (24U * 1024U)
#endif
#ifndef ISAAC_NV_WORKER_IDLE_US
#define ISAAC_NV_WORKER_IDLE_US         20000U
#endif
#ifndef ISAAC_NV_WORKER_FRAMES_PER_PASS
#define ISAAC_NV_WORKER_FRAMES_PER_PASS 8U
#endif
#ifndef ISAAC_NV_WORKER_PRIORITY
#define ISAAC_NV_WORKER_PRIORITY        170
#endif
#ifndef ISAAC_NV_WORKER_STACK_BYTES
#define ISAAC_NV_WORKER_STACK_BYTES     (128U * 1024U)
#endif
/* Bounded wait for an in-flight worker frame before the game thread gives up
 * and returns the frames it has. */
#ifndef ISAAC_NV_ACQUIRE_TIMEOUT_US
#define ISAAC_NV_ACQUIRE_TIMEOUT_US     250000U
#endif
/* Mirrors idle longer than this are evictable when the pool is full. */
#ifndef ISAAC_NV_EVICT_IDLE_US
#define ISAAC_NV_EVICT_IDLE_US          2000000U
#endif

/* ISAAC_VITA_NATIVE_VORBIS_ASYNC knobs (see ASYNC above). */
/* A wrapped call returns a partial slot once this many frames are ready;
 * below it the game thread decodes on the mirror up to this count. */
#ifndef ISAAC_NV_SLOT_MIN_FRAMES
#define ISAAC_NV_SLOT_MIN_FRAMES        2048U
#endif
/* Bound on the game thread's own decoding per wrapped call (checked between
 * frames; the call still returns at least one frame). */
#ifndef ISAAC_NV_SYNC_BUDGET_US
#define ISAAC_NV_SYNC_BUDGET_US         3000U
#endif
/* Guest stream reads issued at stb_vorbis_open_file to prime the OGG ring
 * (each <= ISAAC_NV_TOPUP_CHUNK_BYTES); the worker decodes only while the
 * ring holds more than ISAAC_NV_OGG_GUARD_BYTES, so one chunk yields a
 * single frame of lead and two chunks let it fill the PCM ring.  0 clones
 * without reading. */
#ifndef ISAAC_NV_EARLY_TOPUP_CHUNKS
#define ISAAC_NV_EARLY_TOPUP_CHUNKS     2U
#endif

/* Receipt bounds. */
#define ISAAC_NV_RECEIPT_SLOTS_DENSE    4U
#define ISAAC_NV_VERIFY_LINES_PER_MIRROR 40U

/* Standard reflected CRC-32 (zlib/IEEE 802.3), exposed for the oracle. */
uint32_t isaac_nv_crc32(uint32_t crc, const void *data, uint32_t bytes);

/* Called by the guest heap (weak reference from host_vita_heap.c) before a
 * guest allocation is released.  A music stream is closed by freeing its
 * stb_vorbis_alloc buffer without vorbis_deinit, so this is the only exact
 * teardown edge for stream mirrors: the mirror is retired synchronously
 * (waits for an in-flight worker frame) before the memory can be reused. */
void isaac_nv_guest_buffer_freed(void *pointer);

#if defined(ISAAC_VITA_NATIVE_VORBIS_ORACLE)
/* Host oracle seam: the same relocation and serving code, driven by native
 * pointers instead of a translated CPU. */
typedef struct isaac_nv_oracle_stats {
    uint32_t mirrors_created;
    uint32_t clone_rejected;
    uint32_t fallbacks;
    uint32_t frames_worker;
    uint32_t frames_sync;
    uint32_t waits;
    uint32_t reclones;
    uint32_t evictions;
    uint32_t asserts;
    uint32_t underruns;
    uint32_t worker_refused;
    uint32_t freed_retires;
    uint32_t zero_fallbacks;
    uint32_t frames;
    uint32_t slots;
    /* ASYNC only (0 otherwise) */
    uint32_t partial_slots;
    uint32_t budget_hits;
    uint32_t early_clones;
    uint32_t early_rejected;
} isaac_nv_oracle_stats;
void isaac_nv_oracle_stats_get(isaac_nv_oracle_stats *out);
int isaac_nv_oracle_worker_start(void);
void isaac_nv_oracle_worker_stop(void);
uint32_t isaac_nv_oracle_to_stream_mode(void *state, uint32_t stream_object);
uint32_t isaac_nv_oracle_first_page(const void *state);
void isaac_nv_oracle_set_push_mode(void *state, int on);
void *isaac_nv_oracle_swap_alloc_buffer(void *state, void *buffer);
uint32_t isaac_nv_oracle_state_bytes(void);
void isaac_nv_oracle_set_worker_frame_delay_us(uint32_t us);
int isaac_nv_oracle_mirror_busy(uint32_t f);
uint32_t isaac_nv_oracle_worker_refused(void);
int isaac_nv_oracle_seek(void *state, uint32_t sample);
void isaac_nv_oracle_force_page_resync(void *state);
uint32_t isaac_nv_oracle_marker_get(const void *state);
void isaac_nv_oracle_marker_set(void *state, uint32_t marker);
/* Deterministic game-thread clock for nv_now_us: every read advances it by
 * step_us (0 freezes it).  Enabling continues from the real clock so idle
 * and eviction arithmetic stays monotonic.  Game thread only. */
void isaac_nv_oracle_set_fake_clock(int on, uint32_t step_us);
/* Frames ready in the PCM ring of the mirror for f; -1 when there is none. */
int32_t isaac_nv_oracle_pcm_ahead_frames(uint32_t f);
/* Supplied by the oracle: 32-bit-addressable allocations for mirror memory. */
void *isaac_nv_oracle_malloc(size_t bytes);
void isaac_nv_oracle_free(void *pointer);
#endif

#endif
