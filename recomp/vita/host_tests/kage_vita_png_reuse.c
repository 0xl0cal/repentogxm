/* Actual-core differential bridge for test_vita_native_png_reuse.py.
 * No substitute inflater/unfilter/cache algorithm lives in this harness. */
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "host_vita_native_png.h"
#include "host_vita_png_reuse.h"
#include "host_vita_archive_miniz_native.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    uint8_t *raw_allocation, *cache_allocation;
    uint32_t raw_bytes, cache_bytes;
    uint32_t next_alias;
    uint32_t *read_trace;
    uint32_t read_count, read_capacity, trace_failed;
    uint32_t *feed_trace;
    uint32_t feed_count, feed_capacity;
    uint32_t state[ISAAC_NP_TINFL_STATE_BYTES / sizeof(uint32_t)];
    IsaacNpReuse reuse;
} Probe;

typedef struct {
    const uint8_t *data;
    uint32_t bytes, pos, calls, request_hash;
    Probe *probe;
} Reader;

static uint64_t fake_time;
static uint64_t now_us(void) { return ++fake_time; }
static Probe *active_probe;

/* Only the test's separately compiled miniz object renames these symbols.
 * These wrappers call that actual production inflater, never a replacement. */
int probe_actual_png_miniz_native(void *, const uint8_t *, uint32_t *,
    uint8_t *, uint8_t *, uint32_t *, uint32_t);
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
int probe_actual_png_miniz_reuse(void *, const uint8_t *, uint32_t *,
    uint8_t *, uint8_t *, uint32_t *, uint32_t, uint32_t *);
#endif

static void record_feed(uint32_t input, uint32_t consumed, uint32_t capacity,
                        uint32_t produced, int status)
{
    Probe *p = active_probe;
    uint32_t *dst;
    if (p->feed_count == p->feed_capacity) {
        uint32_t count = p->feed_capacity ? p->feed_capacity * 2U : 64U;
        uint32_t *trace = (uint32_t *)realloc(p->feed_trace,
            (size_t)count * 6U * sizeof(uint32_t));
        if (!trace) { p->trace_failed = 1U; return; }
        p->feed_trace = trace; p->feed_capacity = count;
    }
    dst = p->feed_trace + p->feed_count++ * 6U;
    dst[0] = p->read_count; dst[1] = input; dst[2] = consumed;
    dst[3] = capacity; dst[4] = produced; dst[5] = (uint32_t)status;
}

int isaac_vita_png_miniz_native(void *state, const uint8_t *input,
    uint32_t *input_size, uint8_t *start, uint8_t *next,
    uint32_t *output_size, uint32_t flags)
{
    uint32_t in = *input_size, out = *output_size;
    int st = probe_actual_png_miniz_native(state, input, input_size, start,
        next, output_size, flags);
    record_feed(in, *input_size, out, *output_size, st);
    return st;
}

#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
int isaac_vita_png_miniz_reuse(void *state, const uint8_t *input,
    uint32_t *input_size, uint8_t *start, uint8_t *next,
    uint32_t *output_size, uint32_t flags, uint32_t *history)
{
    uint32_t in = *input_size, out = *output_size;
    int st = probe_actual_png_miniz_reuse(state, input, input_size, start,
        next, output_size, flags, history);
    record_feed(in, *input_size, out, *output_size, st);
    return st;
}
#endif

static uint32_t read_bytes(void *context, uint8_t *dst, uint32_t n)
{
    Reader *r = (Reader *)context;
    uint32_t remain = r->bytes - r->pos;
    uint32_t got = n < remain ? n : remain;
    Probe *p = r->probe;
    if (p->read_count == p->read_capacity) {
        uint32_t capacity = p->read_capacity ? p->read_capacity * 2U : 64U;
        uint32_t *trace = (uint32_t *)realloc(p->read_trace,
            (size_t)capacity * 2U * sizeof(uint32_t));
        if (!trace) { p->trace_failed = 1U; return 0U; }
        p->read_trace = trace; p->read_capacity = capacity;
    }
    p->read_trace[p->read_count * 2U] = n;
    p->read_trace[p->read_count * 2U + 1U] = got;
    ++p->read_count;
    if (got) memcpy(dst, r->data + r->pos, got);
    r->pos += got;
    ++r->calls;
    r->request_hash = r->request_hash * 33U + n;
    return got;
}

EXPORT Probe *probe_create(uint32_t raw_bytes, uint32_t cache_bytes, uint32_t seed)
{
    Probe *p = (Probe *)calloc(1U, sizeof(*p));
    if (!p) return NULL;
    p->raw_allocation = (uint8_t *)malloc((size_t)raw_bytes + 64U);
    p->cache_allocation = (uint8_t *)malloc((size_t)cache_bytes + 64U);
    if (!p->raw_allocation || !p->cache_allocation) {
        free(p->raw_allocation); free(p->cache_allocation); free(p);
        return NULL;
    }
    memset(p->raw_allocation, 0xa5, (size_t)raw_bytes + 64U);
    memset(p->raw_allocation + 32U, (int)seed, raw_bytes);
    memset(p->cache_allocation, 0xa5, (size_t)cache_bytes + 64U);
    p->raw_bytes = raw_bytes; p->cache_bytes = cache_bytes;
    memset(p->state, 0x39, sizeof p->state);
#if ISAAC_VITA_NATIVE_PNG_REUSE
    isaac_np_reuse_init(&p->reuse, p->cache_allocation + 32U, cache_bytes);
#endif
    return p;
}

EXPORT void probe_destroy(Probe *p)
{
    if (!p) return;
    free(p->read_trace);
    free(p->feed_trace);
    free(p->raw_allocation); free(p->cache_allocation); free(p);
}

EXPORT int probe_canaries(const Probe *p)
{
    uint32_t i;
    for (i = 0; i < 32U; ++i)
        if (p->raw_allocation[i] != 0xa5U ||
            p->raw_allocation[p->raw_bytes + 32U + i] != 0xa5U ||
            p->cache_allocation[i] != 0xa5U ||
            p->cache_allocation[p->cache_bytes + 32U + i] != 0xa5U)
            return 0;
    return 1;
}

EXPORT void probe_stats(const Probe *p, uint32_t *out)
{
    out[0] = p->reuse.lookups; out[1] = p->reuse.hits;
    out[2] = p->reuse.stores; out[3] = p->reuse.evictions;
    out[4] = p->reuse.skipped; out[5] = p->reuse.hit_kib;
}

EXPORT const uint8_t *probe_scratch(const Probe *p)
{
    return p->raw_allocation + 32U;
}

EXPORT int probe_scratch_equal(const Probe *p, const uint8_t *other, uint32_t n)
{
    return n == p->raw_bytes && !memcmp(p->raw_allocation + 32U, other, n);
}

EXPORT const uint32_t *probe_read_trace(const Probe *p) { return p->read_trace; }
EXPORT const uint32_t *probe_feed_trace(const Probe *p) { return p->feed_trace; }
EXPORT uint32_t probe_feed_count(const Probe *p) { return p->feed_count; }

/* Exact projection onto legacy class 1/2 state, not a model allocator.
 * Invalid/class-3 metadata and unused arena bytes are deliberately excluded;
 * every live old entry's complete metadata, payload and slot are retained. */
EXPORT uint32_t probe_legacy_projection(const Probe *p, uint8_t *dst, uint32_t capacity)
{
    uint32_t i, size = 2U * sizeof(uint32_t);
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i) {
        const IsaacNpReuseEntry *e = &p->reuse.entries[i];
        size += sizeof(*e);
        if (e->valid == 1U || e->valid == 2U) size += e->bytes;
    }
    if (!dst || capacity < size) return size;
    memcpy(dst, &p->reuse.cursor, sizeof(uint32_t)); dst += sizeof(uint32_t);
    memcpy(dst, &p->reuse.next_slot, sizeof(uint32_t)); dst += sizeof(uint32_t);
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i) {
        const IsaacNpReuseEntry *e = &p->reuse.entries[i];
        if (e->valid == 1U || e->valid == 2U) {
            IsaacNpReuseEntry relevant = *e;
            /* Inactive gamma bytes are not part of the live key. */
            if (!e->gamma) memset(relevant.gamma_table, 0, sizeof(relevant.gamma_table));
            memcpy(dst, &relevant, sizeof relevant); dst += sizeof(*e);
            memcpy(dst, p->reuse.arena + e->offset, e->bytes); dst += e->bytes;
        } else { memset(dst, 0, sizeof(*e)); dst += sizeof(*e); }
    }
    return size;
}

EXPORT uint32_t probe_entry_count(const Probe *p, uint32_t kind)
{
    uint32_t i, count = 0U;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i)
        count += p->reuse.entries[i].valid == kind;
    return count;
}

/* Keep the cached CRC/geometry unchanged while corrupting only a key byte
 * AFTER the first 64KiB. A subsequent hit must do full-tail memcmp too. */
EXPORT int probe_corrupt_large_tail(Probe *p)
{
    uint32_t i;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i) {
        IsaacNpReuseEntry *e = &p->reuse.entries[i];
        if (e->valid == 3U && e->input_bytes > 65536U) {
            p->reuse.arena[e->offset + e->input_bytes - 1U] ^= 1U;
            return 1;
        }
    }
    return 0;
}

/* Deliberately damaged cache payload with unchanged candidate metadata tests
 * that equality is not inferred from geometry/CRC. Not a production mutation. */
EXPORT int probe_corrupt_cached_input(Probe *p)
{
#if ISAAC_VITA_NATIVE_PNG_REUSE
    uint32_t i;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i)
        if (p->reuse.entries[i].valid && p->reuse.entries[i].input_bytes) {
            p->reuse.arena[p->reuse.entries[i].offset] ^= 1U;
            return 1;
        }
#else
    (void)p;
#endif
    return 0;
}

EXPORT void probe_alias_next(Probe *p, uint32_t alias) { p->next_alias = alias; }

/* words: width,height,channels,color_type,rowbytes,first_remaining,crc_entry,
 * staging_bytes. Return all decoder result fields except opt-in diagnostics
 * through named fields, so struct-layout changes cannot invalidate ctypes. */
EXPORT int probe_decode(Probe *p, const uint32_t *words,
    const uint8_t *stream, uint32_t stream_bytes, const uint8_t *gamma,
    uint32_t *out_words, uint8_t *raw_out, uint8_t *last_out)
{
    isaac_np_params params;
    isaac_np_work work;
    isaac_np_result out;
    Reader r;
    uint8_t *staging, *last;
    uint32_t *state;
#if ISAAC_VITA_NATIVE_PNG_REUSE
    uint8_t *saved_arena;
#endif
    uint64_t total = (uint64_t)words[1] * (words[4] + 1ULL);
    uint32_t i;
    int status, decoder_errno;
    if (total > p->raw_bytes || words[7] > 131072U || words[4] > 65536U)
        return -1;
    staging = (uint8_t *)malloc((size_t)words[7] + 32U);
    last = (uint8_t *)malloc((size_t)words[4] + 32U);
    state = p->state;
    if (!staging || !last) {
        free(staging); free(last); return -2;
    }
    memset(staging, 0x6d, (size_t)words[7] + 32U);
    memset(last, 0xa7, (size_t)words[4] + 32U);
    params.width = words[0]; params.height = words[1];
    params.channels = words[2]; params.color_type = words[3];
    params.rowbytes = words[4]; params.first_remaining = words[5];
    params.crc_entry = words[6]; params.gamma_table = gamma;
    work.raw = p->raw_allocation + 32U; work.staging = staging;
    work.staging_bytes = words[7]; work.tinfl_state = state;
    work.last_row_pre_gamma = last; work.now_us = now_us;
    r.data = stream; r.bytes = stream_bytes; r.pos = r.calls = r.request_hash = 0U;
    r.probe = p;
    p->read_count = p->feed_count = p->trace_failed = 0U;
    active_probe = p;
    fake_time = 0U;
#if ISAAC_VITA_NATIVE_PNG_REUSE
    saved_arena = p->reuse.arena;
    switch (p->next_alias) {
    case 1U: p->reuse.arena = work.raw; break;
    case 2U: p->reuse.arena = staging; break;
    case 3U: p->reuse.arena = (uint8_t *)state; break;
    case 4U: p->reuse.arena = last; break;
    case 5U: p->reuse.arena = (uint8_t *)gamma; break;
    case 6U: p->reuse.arena = NULL; break;
    default: break;
    }
    errno = 123;
    status = isaac_np_decode_reusing(&params, read_bytes, &r, &work, &out, &p->reuse);
    decoder_errno = errno;
    p->reuse.arena = saved_arena;
#else
    errno = 123;
    status = isaac_np_decode(&params, read_bytes, &r, &work, &out);
    decoder_errno = errno;
#endif
    active_probe = NULL;
    p->next_alias = 0U;
    out_words[0] = (uint32_t)out.status; out_words[1] = out.chunks;
    out_words[2] = out.idat_bytes; out_words[3] = out.stream_bytes;
    out_words[4] = out.crc_final; out_words[5] = out.last_filter;
    for (i = 0U; i < 5U; ++i) out_words[6U + i] = out.filters[i];
    out_words[11] = out.io_us;
    out_words[12] = out.strict_attempts; out_words[13] = out.strict_successes;
    out_words[14] = out.strict_refusals;
    out_words[15] = r.pos; out_words[16] = r.calls; out_words[17] = r.request_hash;
    out_words[18] = (uint32_t)status; out_words[19] = (uint32_t)decoder_errno;
    memcpy(raw_out, work.raw, (size_t)total);
    memcpy(last_out, last, words[4]);
    for (i = 0U; i < 32U; ++i)
        if (staging[words[7] + i] != 0x6dU || last[words[4] + i] != 0xa7U) {
            free(staging); free(last); return -3;
        }
    free(staging); free(last);
    if (p->trace_failed || p->read_count != r.calls) return -5;
    return probe_canaries(p) ? 0 : -4;
}
