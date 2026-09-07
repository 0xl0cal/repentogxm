/* Native whole-image PNG decode for the frozen KAGE ImagePng loader.  See
 * host_vita_native_png.h for the seam, the convention and the exactness
 * argument.  The first half of this file is pure host code (the decoder core
 * the oracle drives); the second half is the guest-facing wrapper compiled
 * only for the Vita eboot. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_native_png.h"
#include "host_vita_archive_miniz_native.h"
#include "host_vita_native_png_rgba_neon.h"
#include "host_vita_native_png_adler.h"
#if ISAAC_VITA_NATIVE_PNG_REUSE
#include "host_vita_png_reuse.h"
#endif
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
#include "host_vita_native_png_libdeflate.h"
#endif

#if ISAAC_VITA_NATIVE_PNG_ADLER_NEON
#define NP_MINIZ_DECODE isaac_vita_png_miniz_native
#else
#define NP_MINIZ_DECODE isaac_vita_archive_miniz_native
#endif

#if defined(ISAAC_VITA_NATIVE_PNG_ORACLE) || !defined(__vita__)
#define NP_HOST_BUILD 1
#else
#define NP_HOST_BUILD 0
#include "guest.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#endif

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !NP_HOST_BUILD
#include <arm_neon.h>
#define NP_HAS_NEON 1
#else
#define NP_HAS_NEON 0
#endif

#ifndef ISAAC_VITA_NATIVE_PNG_BUILD_ID
#define ISAAC_VITA_NATIVE_PNG_BUILD_ID "native-png:unstamped"
#endif

/* tinfl flags (miniz v1.15). */
enum {
    NP_TINFL_PARSE_ZLIB_HEADER = 1,
    NP_TINFL_HAS_MORE_INPUT = 2,
    NP_TINFL_NON_WRAPPING_OUTPUT = 4,
    NP_TINFL_COMPUTE_ADLER32 = 8
};

/* ------------------------------------------------------------------------
 * Core. */

uint32_t isaac_np_crc32(uint32_t crc, const void *data, uint32_t bytes)
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

uint32_t isaac_np_channels_for(uint32_t color_type)
{
    switch (color_type) {
    case 0U: return 1U;   /* gray */
    case 2U: return 3U;   /* RGB */
    case 3U: return 1U;   /* palette indices */
    case 4U: return 2U;   /* gray + alpha */
    case 6U: return 4U;   /* RGBA */
    default: return 0U;
    }
}

const char *isaac_np_status_name(int status)
{
    switch (status) {
    case ISAAC_NP_OK: return "ok";
    case ISAAC_NP_REJECT_READ: return "read";
    case ISAAC_NP_REJECT_CRC: return "crc";
    case ISAAC_NP_REJECT_NOT_IDAT: return "not-idat";
    case ISAAC_NP_REJECT_LENGTH: return "length";
    case ISAAC_NP_REJECT_INFLATE: return "inflate";
    case ISAAC_NP_REJECT_EXTRA: return "extra";
    case ISAAC_NP_REJECT_TRUNCATED: return "truncated";
    case ISAAC_NP_REJECT_FILTER: return "filter";
    case ISAAC_NP_REJECT_PARAM: return "param";
    default: return "?";
    }
}

static uint32_t np_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* PNG filter reconstruction, the exact sequential equations of
 * png_read_filter_row: Sub adds the reconstructed byte bpp to the left, Up
 * the byte above, Average floors (left + above) / 2, Paeth keeps the tie
 * order left, above, upper-left; everything modulo 256.  `prev` is NULL on
 * the first row, which the specification defines as an all-zero row. */
static void np_unfilter_sub(uint8_t *row, uint32_t n, uint32_t bpp)
{
    uint32_t i = bpp;
#if ISAAC_NP_RGBA_NEON_AVAILABLE
    if (bpp == 4U && n >= 16U)
        i = np_rgba_sub_blocks(row, NULL, n);
#endif
    for (; i < n; ++i)
        row[i] = (uint8_t)(row[i] + row[i - bpp]);
}

static void np_unfilter_up(uint8_t *row, const uint8_t *prev, uint32_t n)
{
    uint32_t i = 0U;
#if NP_HAS_NEON
    for (; i + 16U <= n; i += 16U) {
        uint8x16_t a = vld1q_u8(row + i);
        uint8x16_t b = vld1q_u8(prev + i);
        vst1q_u8(row + i, vaddq_u8(a, b));
    }
#endif
    for (; i < n; ++i)
        row[i] = (uint8_t)(row[i] + prev[i]);
}

static void np_unfilter_avg(uint8_t *row, const uint8_t *prev, uint32_t n,
                            uint32_t bpp)
{
    uint32_t i = 0U;
#if ISAAC_NP_RGBA_NEON_AVAILABLE
    if (bpp == 4U && n >= 16U)
        i = np_rgba_avg_blocks(row, prev, n);
#endif
    if (prev) {
        for (; i < bpp && i < n; ++i)
            row[i] = (uint8_t)(row[i] + (prev[i] >> 1));
        for (; i < n; ++i)
            row[i] = (uint8_t)(row[i] +
                               (((uint32_t)row[i - bpp] + prev[i]) >> 1));
    } else {
        if (i < bpp)
            i = bpp;
        for (; i < n; ++i)
            row[i] = (uint8_t)(row[i] + (row[i - bpp] >> 1));
    }
}

static void np_unfilter_paeth(uint8_t *row, const uint8_t *prev, uint32_t n,
                              uint32_t bpp)
{
    uint32_t i = 0U;
    if (!prev) {
        /* above and upper-left are zero: the predictor is always `left` */
        np_unfilter_sub(row, n, bpp);
        return;
    }
#if ISAAC_NP_RGBA_NEON_AVAILABLE
    if (bpp == 4U && n >= 16U)
        i = np_rgba_paeth_blocks(row, prev, n);
#endif
    for (; i < bpp && i < n; ++i)
        row[i] = (uint8_t)(row[i] + prev[i]);
    for (; i < n; ++i) {
        int a = row[i - bpp];
        int b = prev[i];
        int c = prev[i - bpp];
        int p = a + b - c;
        int pa = p > a ? p - a : a - p;
        int pb = p > b ? p - b : b - p;
        int pc = p > c ? p - c : c - p;
        int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
        row[i] = (uint8_t)(row[i] + pred);
    }
}

/* png_do_gamma for 8-bit rows: every colour byte through the table, alpha
 * untouched, palette indices untouched (the caller never passes a table for
 * colour type 3 because png_do_read_transformations skips it). */
static void np_gamma_row(uint8_t *row, uint32_t rowbytes, uint32_t color_type,
                         const uint8_t *table)
{
    uint32_t i;
    switch (color_type) {
    case 0U:
    case 2U:
        for (i = 0U; i < rowbytes; ++i)
            row[i] = table[row[i]];
        break;
    case 4U:
        for (i = 0U; i + 1U < rowbytes; i += 2U)
            row[i] = table[row[i]];
        break;
    case 6U:
        for (i = 0U; i + 3U < rowbytes; i += 4U) {
            row[i] = table[row[i]];
            row[i + 1U] = table[row[i + 1U]];
            row[i + 2U] = table[row[i + 2U]];
        }
        break;
    default:
        break;
    }
}

#if ISAAC_VITA_NATIVE_PNG_REUSE
int isaac_np_decode_reusing(const isaac_np_params *p, isaac_np_read_fn read,
    void *ctx, isaac_np_work *work, isaac_np_result *out, IsaacNpReuse *reuse)
#else
int isaac_np_decode(const isaac_np_params *p, isaac_np_read_fn read,
                    void *ctx, isaac_np_work *work, isaac_np_result *out)
#endif
{
    const uint32_t flags = NP_TINFL_PARSE_ZLIB_HEADER |
                           NP_TINFL_HAS_MORE_INPUT |
                           NP_TINFL_NON_WRAPPING_OUTPUT |
                           NP_TINFL_COMPUTE_ADLER32;
    uint32_t stride;
    uint64_t total;
    uint8_t *cur;
    uint8_t *end;
    uint32_t remaining;
    uint32_t crc;
    uint32_t y;
    int done = 0;
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
    uint32_t history_unsafe = 0U;
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    uint32_t read_limit;
    int retain_first;
#endif

    memset(out, 0, sizeof(*out));
    out->status = ISAAC_NP_REJECT_PARAM;
    if (!p || !read || !work || !work->raw || !work->staging ||
            !work->tinfl_state || work->staging_bytes < 16U)
        return out->status;
    if (p->width == 0U || p->height == 0U ||
            p->width > ISAAC_NP_MAX_DIMENSION ||
            p->height > ISAAC_NP_MAX_DIMENSION ||
            p->channels == 0U || p->channels > 4U ||
            isaac_np_channels_for(p->color_type) != p->channels ||
            p->rowbytes != p->width * p->channels)
        return out->status;
    stride = p->rowbytes + 1U;
    total = (uint64_t)p->height * stride;
    if (total > 0xffffffffULL)
        return out->status;
#if ISAAC_VITA_NATIVE_PNG_REUSE
    /* Reuse storage must be independent from ALL bytes of decoder scratch,
     * including bytes a later failed speculative decode could observe. */
    if (!isaac_np_reuse_begin(reuse, work->raw, (uint32_t)total) ||
        !isaac_np_reuse_disjoint(reuse, work->staging, work->staging_bytes) ||
        !isaac_np_reuse_disjoint(reuse, work->tinfl_state, ISAAC_NP_TINFL_STATE_BYTES) ||
        !isaac_np_reuse_disjoint(reuse, work->last_row_pre_gamma, p->rowbytes) ||
        (p->gamma_table && !isaac_np_reuse_disjoint(reuse, p->gamma_table, 256U)))
        reuse = NULL;
#endif
    cur = work->raw;
    end = work->raw + (uint32_t)total;
    remaining = p->first_remaining;
    crc = p->crc_entry;
    out->chunks = 1U;
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    /* Retain a larger compressed key without changing cold/error read or
     * inflater-call boundaries. The first prefix is ALWAYS decoded normally;
     * a late hit can skip only the tail inflate and whole unfilter/gamma. */
    read_limit = work->staging_bytes < 65536U ? work->staging_bytes : 65536U;
    retain_first = p->first_remaining > 65536U &&
        p->first_remaining <= ISAAC_NP_REUSE_INPUT_BYTES &&
        p->first_remaining <= work->staging_bytes;
#endif
#if ISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET
    /* State zero starts a new stream. The local tinfl adaptation preloads
     * scalar coroutine fields, so initialize the whole asserted header;
     * its table builders initialize every lookup/tree/code byte they read.
     * No archive caller or externally visible state blob uses this reset. */
    memset(work->tinfl_state, 0, ISAAC_VITA_ARCHIVE_MINIZ_HEADER_BYTES);
#else
    memset(work->tinfl_state, 0, ISAAC_NP_TINFL_STATE_BYTES);
#endif

#define NP_READ(dst, n)                                                   \
    do {                                                                  \
        uint64_t t0_ = work->now_us ? work->now_us() : 0U;                \
        uint32_t got_ = read(ctx, (dst), (n));                            \
        if (work->now_us)                                                 \
            out->io_us += (uint32_t)(work->now_us() - t0_);               \
        out->stream_bytes += got_;                                        \
        if (got_ != (n)) {                                                \
            out->status = ISAAC_NP_REJECT_READ;                           \
            return out->status;                                           \
        }                                                                 \
    } while (0)

    while (!done) {
        uint32_t n;
        uint32_t in_pos = 0U;
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
        uint8_t *input = work->staging;
#endif

        if (remaining == 0U) {
            /* The translated loop: png_crc_finish (read + compare the
             * CRC), then the next chunk header, which must be IDAT. */
            uint8_t hdr[8];
            NP_READ(hdr, 4U);
            if (np_be32(hdr) != crc) {
                out->status = ISAAC_NP_REJECT_CRC;
                return out->status;
            }
            NP_READ(hdr, 8U);
            remaining = np_be32(hdr);
            if (remaining > 0x7fffffffU) {
                out->status = ISAAC_NP_REJECT_LENGTH;
                return out->status;
            }
            if (memcmp(hdr + 4, "IDAT", 4) != 0) {
                out->status = ISAAC_NP_REJECT_NOT_IDAT;
                return out->status;
            }
            crc = isaac_np_crc32(0U, hdr + 4, 4U);
            ++out->chunks;
            continue;
        }
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
        n = remaining < read_limit ? remaining : read_limit;
        if (retain_first && out->chunks == 1U)
            input += out->idat_bytes;
        NP_READ(input, n);
        crc = isaac_np_crc32(crc, input, n);
#else
        n = remaining < work->staging_bytes ? remaining : work->staging_bytes;
        NP_READ(work->staging, n);
        crc = isaac_np_crc32(crc, work->staging, n);
#endif
        remaining -= n;
        out->idat_bytes += n;
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
        /* The earlier prefix has already run through unchanged tinfl. On a
         * short tail read NP_READ returns with precisely those prefix writes.
         * No speculative read/decode replay or hash-only match is involved. */
        if (retain_first && out->chunks == 1U && remaining == 0U &&
            out->idat_bytes == p->first_remaining &&
            isaac_np_reuse_load(reuse, p, work->staging, p->first_remaining,
                                crc, work, out))
            return out->status;
#endif
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
        /* Only the original first IDAT, wholly staged, before any tinfl use.
         * The decoder context is typed local stack storage. Refusal does not
         * reread input, recount CRC/IO, or mutate the virgin tinfl state. */
        if (out->chunks == 1U && remaining == 0U &&
            out->idat_bytes == p->first_remaining && n == p->first_remaining &&
            n <= 65536U && cur == work->raw) {
#if ISAAC_VITA_NATIVE_PNG_REUSE
            if (isaac_np_reuse_load(reuse, p, work->staging, n, crc, work, out))
                return out->status;
#endif
            ++out->strict_attempts;
            if (isaac_np_libdeflate_try(work->staging, n, work->raw,
                                      (uint32_t)total)) {
                ++out->strict_successes;
                cur = end;
                done = 1;
                break;
            }
            ++out->strict_refusals;
        }
#endif
        while (in_pos < n) {
            uint32_t in_size = n - in_pos;
            uint32_t out_size = (uint32_t)(end - cur);
            int st =
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
                isaac_vita_png_miniz_reuse(
#else
                NP_MINIZ_DECODE(
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
                work->tinfl_state, input + in_pos, &in_size,
#else
                work->tinfl_state, work->staging + in_pos, &in_size,
#endif
                work->raw, cur, &out_size, flags
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
                , &history_unsafe
#endif
                );
            in_pos += in_size;
            cur += out_size;
            if (st == ISAAC_VITA_ARCHIVE_MINIZ_DONE) {
                if (cur != end) {
                    out->status = ISAAC_NP_REJECT_TRUNCATED;
                    return out->status;
                }
                if (in_pos != n || remaining != 0U) {
                    out->status = ISAAC_NP_REJECT_EXTRA;
                    return out->status;
                }
                done = 1;
                break;
            }
            if (st == ISAAC_VITA_ARCHIVE_MINIZ_NEEDS_MORE_INPUT) {
                if (in_pos < n && in_size == 0U && out_size == 0U) {
                    out->status = ISAAC_NP_REJECT_INFLATE;
                    return out->status;
                }
                continue;
            }
            if (st == ISAAC_VITA_ARCHIVE_MINIZ_HAS_MORE_OUTPUT) {
                out->status = ISAAC_NP_REJECT_EXTRA;
                return out->status;
            }
            out->status = ISAAC_NP_REJECT_INFLATE;
            return out->status;
        }
    }
#undef NP_READ

    /* Unfilter in place; the filter byte stays in front of every row. */
    for (y = 0U; y < p->height; ++y) {
        uint8_t *row = work->raw + (size_t)y * stride;
        const uint8_t *prev = y ? row - stride + 1 : NULL;
        uint32_t filter = row[0];
        if (filter > 4U) {
            out->status = ISAAC_NP_REJECT_FILTER;
            return out->status;
        }
        ++out->filters[filter];
        switch (filter) {
        case 1U: np_unfilter_sub(row + 1, p->rowbytes, p->channels); break;
        case 2U: if (prev) np_unfilter_up(row + 1, prev, p->rowbytes); break;
        case 3U: np_unfilter_avg(row + 1, prev, p->rowbytes, p->channels); break;
        case 4U: np_unfilter_paeth(row + 1, prev, p->rowbytes, p->channels); break;
        default: break;
        }
        out->last_filter = filter;
    }
    if (work->last_row_pre_gamma)
        memcpy(work->last_row_pre_gamma,
               work->raw + (size_t)(p->height - 1U) * stride + 1, p->rowbytes);
    if (p->gamma_table && p->color_type != 3U) {
        for (y = 0U; y < p->height; ++y)
            np_gamma_row(work->raw + (size_t)y * stride + 1, p->rowbytes,
                         p->color_type, p->gamma_table);
    }
    out->crc_final = crc;
    out->status = ISAAC_NP_OK;
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
    /* The entire original stream has produced exact output and passed Adler,
     * unfilter and shape checks. Complete tables + positive produced history
     * make that output independent of the scratch contents at entry. */
    out->reuse_history_safe = history_unsafe == 0U;
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE
    if ((out->strict_successes == 1U && out->strict_refusals == 0U)
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
        || out->reuse_history_safe
#endif
        )
        isaac_np_reuse_store(reuse, p, work->staging, p->first_remaining, work, out);
#endif
    return out->status;
}

#if ISAAC_VITA_NATIVE_PNG_REUSE
int isaac_np_decode(const isaac_np_params *p, isaac_np_read_fn read,
    void *ctx, isaac_np_work *work, isaac_np_result *out)
{
    return isaac_np_decode_reusing(p, read, ctx, work, out, NULL);
}
#endif

/* ------------------------------------------------------------------------
 * Guest wrapper (Vita eboot only). */
#if !NP_HOST_BUILD

#include "kage_vita_deep_profile.h"
#if defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
#include "host_vita_heap.h"
#include "host_vita_texel_scratch.h"
#include <errno.h>
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE
static IsaacNpReuse s_reuse;
#endif

void isaac_vita_log(const char *format, ...);
void __real_sub_005b1500(CPU *__restrict c);

#if defined(ISAAC_VITA_NATIVE_PNG_RECEIPT) || defined(ISAAC_VITA_NATIVE_PNG_VERIFY)
#define NP_RECEIPT 1
#else
#define NP_RECEIPT 0
#endif

/* Durations are consumed only by WINDOW, RECEIPT or VERIFY. Keep the
 * default path exact, and retain timing whenever any consumer is compiled
 * in. The optional core clock ABI below remains available to other callers;
 * only this private Vita wrapper supplies NULL in the consumer-free mode. */
#if !defined(ISAAC_VITA_NATIVE_PNG_CLOCK_ELISION) || \
    !ISAAC_VITA_NATIVE_PNG_CLOCK_ELISION || NP_RECEIPT || \
    defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
#define NP_TIMING 1
#else
#define NP_TIMING 0
#endif

enum {
    NP_MODE_NONE = 0,
    NP_MODE_SERVE,       /* rows come from the native scratch */
    NP_MODE_TRANSLATED,  /* __real for every row, bookkeeping only */
    NP_MODE_VERIFY       /* __real for every row, compared with the scratch */
};

typedef struct np_session {
    uint32_t active;
    uint32_t mode;
    uint32_t id;
    uint32_t png;
    uint32_t stream;
    uint32_t width, height, channels, color_type, rowbytes;
    uint32_t next_row;
    uint32_t gamma;
    uint32_t p0;
    uint32_t end_pos;
    uint32_t crc_final;
    uint32_t chunks, idat_bytes;
    uint32_t last_filter;
    uint32_t filters[5];
    uint32_t decode_us, io_us, serve_us, translated_us;
    const char *reason;
    /* decoded rows: the reserve or the per-image scratch block */
    const uint8_t *raw;
    /* verify */
    uint32_t v_diff_rows, v_diff_bytes;
    int32_t v_first_row, v_first_off;
    uint32_t v_state_bad;
    uint32_t v_pos_bad;
    uint32_t v_rowbuf_bad;
} np_session;

static np_session s_ses;
static uint8_t s_staging[ISAAC_NP_STAGING_BYTES] __attribute__((aligned(16)));
static uint32_t s_tinfl_state[ISAAC_NP_TINFL_STATE_BYTES / 4U];
static uint8_t s_last_row[ISAAC_NP_MAX_DIMENSION * 4U];
static int s_init_logged;

static struct np_stats {
    uint32_t images;          /* first-row entries seen */
    uint32_t native;          /* images served natively */
    uint32_t fallbacks;       /* images that ran translated */
    uint32_t passthrough;     /* rows passed to __real without a session */
    uint32_t sites;           /* return word was not the loader's */
    uint32_t busy;            /* another image arrived mid-session */
    uint32_t abandoned;
    uint32_t rows, kib;
    uint64_t decode_us, io_us, serve_us, translated_us;
    uint32_t decode_us_max, translated_us_max;
    uint32_t alloc_refused;
    uint32_t reserved;        /* images served from the startup reserve */
    uint32_t oversize;        /* images above the reserve (per-image block) */
    uint32_t rewind_unsafe;
    uint32_t fb_shape, fb_state, fb_stream, fb_memory, fb_decode;
    uint32_t verify_images, verify_diff_images, verify_diff_bytes;
    uint32_t verify_state_bad, verify_pos_bad;
    uint32_t gamma_images;
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
    uint32_t strict_attempts, strict_successes, strict_refusals;
#endif
} s_stats;

#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
#include "host_vita_png_window_profile.h"
void isaac_vita_png_window_snapshot(IsaacVitaPngWindowSnapshot *out)
{
    if (!out)
        return;
#define NP_SNAPSHOT(f) out->f = s_stats.f
    NP_SNAPSHOT(images); NP_SNAPSHOT(native); NP_SNAPSHOT(fallbacks);
    NP_SNAPSHOT(passthrough); NP_SNAPSHOT(busy); NP_SNAPSHOT(abandoned);
    NP_SNAPSHOT(rows); NP_SNAPSHOT(kib); NP_SNAPSHOT(alloc_refused);
    NP_SNAPSHOT(reserved); NP_SNAPSHOT(oversize); NP_SNAPSHOT(rewind_unsafe);
    NP_SNAPSHOT(fb_shape); NP_SNAPSHOT(fb_state); NP_SNAPSHOT(fb_stream);
    NP_SNAPSHOT(fb_memory); NP_SNAPSHOT(fb_decode);
    NP_SNAPSHOT(decode_us); NP_SNAPSHOT(io_us); NP_SNAPSHOT(serve_us);
    NP_SNAPSHOT(translated_us);
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
    NP_SNAPSHOT(strict_attempts); NP_SNAPSHOT(strict_successes);
    NP_SNAPSHOT(strict_refusals);
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE
    out->reuse_lookups = s_reuse.lookups;
    out->reuse_hits = s_reuse.hits;
    out->reuse_stores = s_reuse.stores;
    out->reuse_evictions = s_reuse.evictions;
    out->reuse_skipped = s_reuse.skipped;
    out->reuse_hit_kib = s_reuse.hit_kib;
#endif
#undef NP_SNAPSHOT
    out->decode_max_ever = s_stats.decode_us_max;
    out->translated_max_ever = s_stats.translated_us_max;
    out->active = s_ses.active;
    out->mode = s_ses.mode;
}
#endif

#if NP_TIMING
static uint64_t np_now_us(void)
{
    return (uint64_t)sceKernelGetProcessTimeWide();
}
#endif

#if defined(ISAAC_VITA_DEEP_PROFILE)
#include <errno.h>
/* Bounded owner-thread cards, emitted only at a phase report. The normal PNG
 * singleton already has that ownership contract. No filenames/guest pointers
 * are retained. Outcome groups: 0 no strict attempt, 1 success-only,
 * 2 refusal-only, 3 mixed/other. Their time is the WHOLE native PNG attempt,
 * not time in libdeflate alone, and includes nested physical reads. */
typedef struct {
    uint64_t us, raw, compressed;
    uint32_t id, loop, width, height, channels, status, crc, group;
} np_deep_card;
static struct {
    np_deep_card top[8];
    struct { uint64_t us, raw, compressed; uint32_t calls, failed; } group[4];
    uint32_t count, seen, omitted, bad_clock, saturated;
} s_np_deep;
static uint64_t np_deep_clock(void)
{
    const int saved_errno = errno;
    const uint64_t now = np_now_us();
    errno = saved_errno;
    return now;
}
static void np_deep_add(uint64_t *value, uint64_t add)
{
    if (add > UINT64_MAX - *value) { *value = UINT64_MAX; s_np_deep.saturated = 1u; }
    else *value += add;
}
static void np_deep_inc(uint32_t *value)
{
    if (*value == UINT32_MAX) s_np_deep.saturated = 1u;
    else ++*value;
}
static void np_deep_note(const np_session *s, const isaac_np_result *result,
                         uint64_t begin, uint64_t end)
{
    uint32_t group = 0u, slot = 0u, i;
    np_deep_card card = {0};
    np_deep_inc(&s_np_deep.seen);
    if (end < begin) { np_deep_inc(&s_np_deep.bad_clock); return; }
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
    if (result->strict_attempts)
        group = result->strict_successes && !result->strict_refusals ? 1u :
            (!result->strict_successes && result->strict_refusals ? 2u : 3u);
#endif
    card.us = end - begin;
    card.raw = (uint64_t)s->height * (s->rowbytes + 1u);
    card.compressed = result->stream_bytes;
    card.id = s_stats.images; card.loop = kage_vita_deep_loop();
    card.width = s->width; card.height = s->height; card.channels = s->channels;
    card.status = (uint32_t)result->status; card.crc = result->crc_final; card.group = group;
    np_deep_inc(&s_np_deep.group[group].calls);
    if (result->status != ISAAC_NP_OK) np_deep_inc(&s_np_deep.group[group].failed);
    np_deep_add(&s_np_deep.group[group].us, card.us);
    np_deep_add(&s_np_deep.group[group].raw, card.raw);
    np_deep_add(&s_np_deep.group[group].compressed, card.compressed);
    if (s_np_deep.count < 8u) slot = s_np_deep.count++;
    else {
        np_deep_inc(&s_np_deep.omitted);
        for (i = 1u; i < 8u; ++i)
            if (s_np_deep.top[i].us < s_np_deep.top[slot].us) slot = i;
        if (card.us <= s_np_deep.top[slot].us) return;
    }
    s_np_deep.top[slot] = card;
}
#define NP_DEEP_U64(v) (uint32_t)((v) >> 32), (uint32_t)(v)
void isaac_vita_png_deep_report(const char *bid, uint32_t window)
{
    uint32_t i;
    int saved_errno = errno;
    isaac_vita_log("[kage-vita] dp.pngh bid=%.32s win=%u bytes=%u records=%u attempts=%u retained=%u omitted=%u clock=%u sat=%u extra_clocks=%u:%u\n",
        bid, window, (unsigned)sizeof s_np_deep, 5u + s_np_deep.count, s_np_deep.seen, s_np_deep.count,
        s_np_deep.omitted, s_np_deep.bad_clock, s_np_deep.saturated,
        NP_DEEP_U64((uint64_t)s_np_deep.seen * 2u));
    for (i = 0u; i < 4u; ++i)
        isaac_vita_log("[kage-vita] dp.pngg bid=%.32s win=%u group=%u n=%u failed=%u us=%u:%u raw=%u:%u stream=%u:%u\n",
            bid, window, i, s_np_deep.group[i].calls, s_np_deep.group[i].failed,
            NP_DEEP_U64(s_np_deep.group[i].us), NP_DEEP_U64(s_np_deep.group[i].raw),
            NP_DEEP_U64(s_np_deep.group[i].compressed));
    for (i = 0u; i < s_np_deep.count; ++i) {
        const np_deep_card *p = &s_np_deep.top[i];
        isaac_vita_log("[kage-vita] dp.png bid=%.32s win=%u slot=%u image=%u loop=%u whc=%u,%u,%u status=%u crc=%08x group=%u us=%u:%u raw=%u:%u stream=%u:%u\n",
            bid, window, i, p->id, p->loop, p->width, p->height, p->channels,
            p->status, p->crc, p->group, NP_DEEP_U64(p->us), NP_DEEP_U64(p->raw),
            NP_DEEP_U64(p->compressed));
    }
    memset(&s_np_deep, 0, sizeof s_np_deep);
    errno = saved_errno;
}
#undef NP_DEEP_U64
#endif

/* Scratch: host-owned SceKernel user memory, never the tracked guest heap.
 * The reserve (ISAAC_NP_RESERVE_BYTES, see the header) is one memblock taken
 * from entry_vita.c before vitaGL init and kept for the whole session; it
 * serves every image whose raw bytes fit.  Larger images take a per-image
 * block, released after the image when it is above the retain bound so a
 * single huge sheet does not pin memory -- that per-image request is what
 * vitaGL's RAM pool starves once it has taken the USER_RW above its
 * threshold, hence the reserve. */
static struct np_scratch {
    void *base;
    SceUID uid;
    uint32_t bytes;
} s_scratch;

static struct np_reserve {
    void *base;
    SceUID uid;
    uint32_t bytes;
    int tried;
} s_reserve;
#if ISAAC_VITA_NATIVE_PNG_REUSE
#ifndef ISAAC_VITA_NATIVE_PNG_REUSE_MB
#define ISAAC_VITA_NATIVE_PNG_REUSE_MB 4U
#endif
/* Optional memory cost is explicit and independent of the existing raw
 * scratch. Allocation happens before vitaGL partitions the remaining RAM;
 * refusal disables reuse without changing the original decoder path. */
static void np_reuse_reserve(void)
{
    const SceSize size = ISAAC_VITA_NATIVE_PNG_REUSE_MB * 1024U * 1024U;
    void *base = NULL;
    SceUID uid = sceKernelAllocMemBlock("isaac_np_reuse",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (uid < 0) {
        isaac_vita_log("KAGE VITA PNG REUSE: disabled alloc=0x%08x bytes=%u",
                      (unsigned)uid, (unsigned)size);
        return;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        isaac_vita_log("KAGE VITA PNG REUSE: disabled no-base bytes=%u", (unsigned)size);
        return;
    }
    isaac_np_reuse_init(&s_reuse, (uint8_t *)base, (uint32_t)size);
    isaac_vita_log("KAGE VITA PNG REUSE: ready bytes=%u slots=%u "
        "key=exact-single-idat+geometry+gamma admit=strict-success"
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
        "+observed-safe-tinfl"
#endif
        ,
        (unsigned)size, (unsigned)ISAAC_NP_REUSE_SLOTS);
}
#endif

_Static_assert(ISAAC_NP_RESERVE_BYTES <= ISAAC_NP_SCRATCH_MAX_BYTES,
               "native PNG reserve above the decoder's scratch bound");

int isaac_vita_native_png_reserve(void)
{
#if ISAAC_VITA_NATIVE_PNG_RESERVE_MB > 0
    SceSize size = (SceSize)((ISAAC_NP_RESERVE_BYTES + 4095U) & ~4095U);
    SceUID uid;
    void *base = NULL;

    if (s_reserve.base)
        return 1;
    if (s_reserve.tried)
        return 0;
    s_reserve.tried = 1;
    uid = sceKernelAllocMemBlock("isaac_np_reserve",
                                 SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (uid < 0) {
        isaac_vita_log(
            "KAGE VITA NATIVE PNG RESERVE: status=alloc-failed result=0x%08x "
            "bytes=%u (per-image scratch blocks)",
            (unsigned)uid, (unsigned)size);
        return 0;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        isaac_vita_log(
            "KAGE VITA NATIVE PNG RESERVE: status=no-base uid=0x%08x bytes=%u",
            (unsigned)uid, (unsigned)size);
        return 0;
    }
    s_reserve.uid = uid;
    s_reserve.base = base;
    s_reserve.bytes = (uint32_t)size;
#if ISAAC_VITA_NATIVE_PNG_REUSE
    np_reuse_reserve();
#endif
    isaac_vita_log(
        "KAGE VITA NATIVE PNG RESERVE: status=ready base=0x%08x bytes=%u "
        "rgba_rows@1024=%u rgba_rows@2048=%u (height*(rowbytes+1) <= bytes)",
        (unsigned)(uintptr_t)base, (unsigned)size,
        (unsigned)(size / (1024U * 4U + 1U)),
        (unsigned)(size / (2048U * 4U + 1U)));
    return 1;
#else
    return 0;
#endif
}

static void np_scratch_release(void)
{
    if (s_scratch.base) {
        (void)sceKernelFreeMemBlock(s_scratch.uid);
        s_scratch.base = NULL;
        s_scratch.uid = 0;
        s_scratch.bytes = 0U;
    }
}

static uint8_t *np_scratch_acquire(uint32_t bytes)
{
    SceSize size;
    SceUID uid;
    void *base = NULL;

    if (bytes == 0U || bytes > ISAAC_NP_SCRATCH_MAX_BYTES)
        return NULL;
    if (s_reserve.base && bytes <= s_reserve.bytes) {
        ++s_stats.reserved;
        return (uint8_t *)s_reserve.base;
    }
    ++s_stats.oversize;
    if (s_scratch.base && s_scratch.bytes >= bytes)
        return (uint8_t *)s_scratch.base;
    np_scratch_release();
    size = (SceSize)((bytes + 4095U) & ~4095U);
    uid = sceKernelAllocMemBlock("isaac_np_scratch",
                                 SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (uid < 0) {
        ++s_stats.alloc_refused;
        return NULL;
    }
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        (void)sceKernelFreeMemBlock(uid);
        ++s_stats.alloc_refused;
        return NULL;
    }
    s_scratch.base = base;
    s_scratch.uid = uid;
    s_scratch.bytes = (uint32_t)size;
    return (uint8_t *)base;
}

static void np_scratch_trim(void)
{
    if (s_scratch.base && s_scratch.bytes > ISAAC_NP_SCRATCH_RETAIN_BYTES)
        np_scratch_release();
}

#if NP_RECEIPT
static int np_power_of_two(uint32_t v)
{
    return v && !(v & (v - 1U));
}

static void np_log_stats(void)
{
    isaac_vita_log(
        "KAGE VITA NATIVE PNG STATS: images=%u native=%u fallbacks=%u "
        "gamma=%u rows=%u kib=%u decode_ms=%u max_us=%u io_ms=%u "
        "serve_ms=%u translated_ms=%u tmax_us=%u fb_shape=%u fb_state=%u "
        "fb_stream=%u fb_memory=%u fb_decode=%u sites=%u busy=%u "
        "abandoned=%u passthrough=%u alloc_refused=%u reserved=%u "
        "oversize=%u rewind_unsafe=%u "
        "verify=%u/%u diff_bytes=%u state_bad=%u pos_bad=%u",
        (unsigned)s_stats.images, (unsigned)s_stats.native,
        (unsigned)s_stats.fallbacks, (unsigned)s_stats.gamma_images,
        (unsigned)s_stats.rows, (unsigned)s_stats.kib,
        (unsigned)(s_stats.decode_us / 1000U), (unsigned)s_stats.decode_us_max,
        (unsigned)(s_stats.io_us / 1000U), (unsigned)(s_stats.serve_us / 1000U),
        (unsigned)(s_stats.translated_us / 1000U),
        (unsigned)s_stats.translated_us_max, (unsigned)s_stats.fb_shape,
        (unsigned)s_stats.fb_state, (unsigned)s_stats.fb_stream,
        (unsigned)s_stats.fb_memory, (unsigned)s_stats.fb_decode,
        (unsigned)s_stats.sites, (unsigned)s_stats.busy,
        (unsigned)s_stats.abandoned, (unsigned)s_stats.passthrough,
        (unsigned)s_stats.alloc_refused, (unsigned)s_stats.reserved,
        (unsigned)s_stats.oversize, (unsigned)s_stats.rewind_unsafe,
        (unsigned)s_stats.verify_diff_images, (unsigned)s_stats.verify_images,
        (unsigned)s_stats.verify_diff_bytes, (unsigned)s_stats.verify_state_bad,
        (unsigned)s_stats.verify_pos_bad);
}

static void np_maybe_stats(void)
{
    uint32_t n = s_stats.images;
    if ((n >= 16U && np_power_of_two(n)) || (n % ISAAC_NP_STATS_EVERY) == 0U)
        np_log_stats();
}

static int np_dense(uint32_t n, uint32_t dense)
{
    return n <= dense || np_power_of_two(n);
}
#endif

static void np_log_init(void)
{
    if (s_init_logged)
        return;
    s_init_logged = 1;
    isaac_vita_log(
        "KAGE VITA NATIVE PNG: seam=png_read_row@%08x decoder=tinfl-v1.15 "
        "unfilter=native gamma=guest-table scratch_max=%u retain=%u "
        "staging=%u reserve=%u rgba-neon=%u adler-neon=%u tinfl-reset=%s receipt=%s verify=%s build=%s"
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
        " libdeflate-strict=1"
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE
        " reuse=exact-independent-reserve"
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
        " reuse-large=late-128k/read-64k"
#endif
#if defined(ISAAC_VITA_NATIVE_PNG_LZ_NEON) && ISAAC_VITA_NATIVE_PNG_LZ_NEON
        " lz-neon=1"
#endif
        ,
        (unsigned)ISAAC_NP_READ_ROW_RVA,
        (unsigned)ISAAC_NP_SCRATCH_MAX_BYTES,
        (unsigned)ISAAC_NP_SCRATCH_RETAIN_BYTES,
        (unsigned)ISAAC_NP_STAGING_BYTES,
        (unsigned)(s_reserve.base ? s_reserve.bytes : 0U),
        (unsigned)ISAAC_NP_RGBA_NEON_AVAILABLE,
        (unsigned)ISAAC_NP_ADLER_NEON_AVAILABLE,
        ISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET ? "header" : "full",
#if defined(ISAAC_VITA_NATIVE_PNG_RECEIPT)
        "on",
#else
        "off",
#endif
#if defined(ISAAC_VITA_NATIVE_PNG_VERIFY)
        "on",
#else
        "off",
#endif
        ISAAC_VITA_NATIVE_PNG_BUILD_ID);
}

/* ------------------------------------------------------------------------
 * Guest stream calls (game thread, CPU context).  Same convention as the
 * translated default read callback: thiscall, arguments pushed right to
 * left, callee cleans, result in eax. */
static uint32_t np_guest_vcall(CPU *__restrict c, uint32_t object,
                               uint32_t slot, const uint32_t *args,
                               unsigned count, int *ok)
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
    /* Generated call sites push the plain RVA as the return word. */
    gpush(c, ISAAC_NP_SITE_STREAM_READ_RVA);
    c->ecx = object;
    guest_call(c, target);
    if (c->esp != saved_esp) {
        c->esp = saved_esp;
        return 0U;
    }
    *ok = 1;
    return c->eax;
}

static int np_guest_tell(CPU *__restrict c, uint32_t stream, uint32_t *pos)
{
    int ok;
    uint32_t v = np_guest_vcall(c, stream, ISAAC_NP_STREAM_VT_TELL, NULL, 0U,
                                &ok);
    if (ok)
        *pos = v;
    return ok;
}

static int np_guest_seek(CPU *__restrict c, uint32_t stream, uint32_t pos)
{
    uint32_t args[2];
    uint32_t got = 0U;
    int ok;

    args[0] = pos;
    args[1] = 0U;   /* SEEK_SET */
    (void)np_guest_vcall(c, stream, ISAAC_NP_STREAM_VT_SEEK, args, 2U, &ok);
    if (!ok || !np_guest_tell(c, stream, &got) || got != pos)
        return 0;
    return 1;
}

typedef struct np_guest_io {
    CPU *c;
    uint32_t stream;
} np_guest_io;

/* read(buffer, 1, n) -> bytes read; exactly one stream call per request,
 * as the translated callback makes (it errors on a short count). */
static uint32_t np_guest_read(void *ctx, uint8_t *dst, uint32_t n)
{
    np_guest_io *io = (np_guest_io *)ctx;
    uint32_t args[3];
    uint32_t got;
    int ok;

    args[0] = (uint32_t)(uintptr_t)dst;
    args[1] = 1U;
    args[2] = n;
    got = np_guest_vcall(io->c, io->stream, ISAAC_NP_STREAM_VT_READ, args, 3U,
                         &ok);
    if (!ok)
        return 0U;
    return got > n ? n : got;
}

/* ------------------------------------------------------------------------
 * Sessions. */
static void np_session_reset(void)
{
    memset(&s_ses, 0, sizeof(s_ses));
}

static void np_call_real(CPU *__restrict c, uint32_t eax, uint32_t png,
                         uint32_t row)
{
    c->eax = eax;
    c->ecx = png;
    c->edx = row;
    __real_sub_005b1500(c);
}

static void np_note_fallback_class(const char *reason)
{
    if (!strcmp(reason, "site"))
        ++s_stats.sites;
    else if (!strcmp(reason, "memory"))
        ++s_stats.fb_memory;
    else if (!strncmp(reason, "stream", 6))
        ++s_stats.fb_stream;
    else if (!strncmp(reason, "decode", 6))
        ++s_stats.fb_decode;
    else if (!strcmp(reason, "row-init") || !strcmp(reason, "mode") ||
             !strcmp(reason, "flags") || !strcmp(reason, "zlib") ||
             !strcmp(reason, "transform") || !strcmp(reason, "read-fn") ||
             !strcmp(reason, "read-row-fn") || !strcmp(reason, "io-ptr") ||
             !strcmp(reason, "row-buf"))
        ++s_stats.fb_state;
    else
        ++s_stats.fb_shape;
}

#if NP_RECEIPT
static void np_log_fallback(const np_session *s)
{
    if (!np_dense(s_stats.fallbacks, ISAAC_NP_FALLBACK_LINES_DENSE))
        return;
    if (!s->png || (s->png & 3U)) {
        isaac_vita_log(
            "KAGE VITA NATIVE PNG FALLBACK: n=%u png=%08x reason=%s "
            "(translated decoder used)",
            (unsigned)s->id, (unsigned)s->png, s->reason);
        return;
    }
    isaac_vita_log(
        "KAGE VITA NATIVE PNG FALLBACK: n=%u png=%08x reason=%s %ux%u ct=%u "
        "depth=%u il=%u tr=%08x mode=%02x flags=%03x rows=%u translated_us=%u "
        "(translated decoder used)",
        (unsigned)s->id, (unsigned)s->png, s->reason, (unsigned)s->width,
        (unsigned)s->height, (unsigned)ld8(s->png + ISAAC_NP_PNG_COLOR_TYPE),
        (unsigned)ld8(s->png + ISAAC_NP_PNG_BIT_DEPTH),
        (unsigned)ld8(s->png + ISAAC_NP_PNG_INTERLACED),
        (unsigned)ld32(s->png + ISAAC_NP_PNG_TRANSFORMATIONS),
        (unsigned)ld32(s->png + ISAAC_NP_PNG_MODE),
        (unsigned)ld32(s->png + ISAAC_NP_PNG_FLAGS),
        (unsigned)s->next_row, (unsigned)s->translated_us);
}

static void np_log_image(const np_session *s)
{
    if (!np_dense(s_stats.native, ISAAC_NP_RECEIPT_IMAGES_DENSE))
        return;
    isaac_vita_log(
        "KAGE VITA NATIVE PNG IMAGE: n=%u png=%08x %ux%u ct=%u ch=%u idat=%u "
        "chunks=%u gamma=%u filters=%u/%u/%u/%u/%u decode_us=%u io_us=%u "
        "serve_us=%u crc=%08x result=native",
        (unsigned)s->id, (unsigned)s->png, (unsigned)s->width,
        (unsigned)s->height, (unsigned)s->color_type, (unsigned)s->channels,
        (unsigned)s->idat_bytes, (unsigned)s->chunks, (unsigned)s->gamma,
        (unsigned)s->filters[0], (unsigned)s->filters[1],
        (unsigned)s->filters[2], (unsigned)s->filters[3],
        (unsigned)s->filters[4], (unsigned)s->decode_us, (unsigned)s->io_us,
        (unsigned)s->serve_us, (unsigned)s->crc_final);
}
#endif

static void np_finish_translated(np_session *s)
{
    ++s_stats.fallbacks;
    s_stats.translated_us += s->translated_us;
    if (s->translated_us > s_stats.translated_us_max)
        s_stats.translated_us_max = s->translated_us;
    np_note_fallback_class(s->reason);
#if NP_RECEIPT
    np_log_fallback(s);
    np_maybe_stats();
#endif
    np_session_reset();
}

/* Translated bookkeeping mode: __real serves the row; the session only
 * follows row_number so the image is counted once. */
static void np_translated_row(CPU *__restrict c, np_session *s, uint32_t row,
                              uint32_t eax0)
{
#if NP_TIMING
    uint64_t t0 = np_now_us();
#endif
    uint32_t rn;

    np_call_real(c, eax0, s->png, row);
#if NP_TIMING
    s->translated_us += (uint32_t)(np_now_us() - t0);
#endif
    rn = ld32(s->png + ISAAC_NP_PNG_ROW_NUMBER);
    if (rn >= s->height || rn <= s->next_row) {
        s->next_row = rn;
        np_finish_translated(s);
        return;
    }
    s->next_row = rn;
}

static void np_begin_translated(CPU *__restrict c, uint32_t png, uint32_t row,
                                uint32_t eax0, const char *reason)
{
    np_session *s = &s_ses;
    uint32_t height;

    np_session_reset();
    s->active = 1U;
    s->mode = NP_MODE_TRANSLATED;
    s->id = s_stats.images;
    s->png = png;
    s->reason = reason;
    if (!png || (png & 3U)) {
        /* the translated body faults on this exactly as before */
        np_call_real(c, eax0, png, row);
        np_finish_translated(s);
        return;
    }
    height = ld32(png + ISAAC_NP_PNG_HEIGHT);
    s->width = ld32(png + ISAAC_NP_PNG_WIDTH);
    s->height = height;
    s->next_row = 0U;
    if (height == 0U) {
        /* nothing to follow; count it and pass through */
        np_call_real(c, eax0, png, row);
        np_finish_translated(s);
        return;
    }
    np_translated_row(c, s, row, eax0);
}

/* Write the end-of-image state the translated png_read_row +
 * png_read_finish_row leave behind for a well-formed, non-interlaced 8-bit
 * image: idat_size consumed, running CRC of the last chunk, AFTER_IDAT and
 * ZLIB_FINISHED, the reset zstream counters, row_number == height, the
 * row_info of the last row, prev_row = the last unfiltered row, row_buf =
 * the last transformed row (both with the filter byte in front). */
static void np_write_final_state(const np_session *s)
{
    uint32_t png = s->png;
    uint32_t row_buf = ld32(png + ISAAC_NP_PNG_ROW_BUF);
    uint32_t prev_row = ld32(png + ISAAC_NP_PNG_PREV_ROW);
    uint32_t stride = s->rowbytes + 1U;
    const uint8_t *last = s->raw +
                          (size_t)(s->height - 1U) * stride;

    st32(png + ISAAC_NP_PNG_IDAT_SIZE, 0U);
    st32(png + ISAAC_NP_PNG_CRC, s->crc_final);
    st32(png + ISAAC_NP_PNG_MODE,
         ld32(png + ISAAC_NP_PNG_MODE) | ISAAC_NP_MODE_AFTER_IDAT);
    st32(png + ISAAC_NP_PNG_FLAGS,
         ld32(png + ISAAC_NP_PNG_FLAGS) | ISAAC_NP_FLAG_ZLIB_FINISHED);
    st32(png + ISAAC_NP_PNG_Z_AVAIL_IN, 0U);
    st32(png + ISAAC_NP_PNG_Z_AVAIL_OUT, 0U);
    st32(png + ISAAC_NP_PNG_Z_NEXT_OUT, row_buf);
    st32(png + ISAAC_NP_PNG_Z_TOTAL_IN, 0U);
    st32(png + ISAAC_NP_PNG_Z_TOTAL_OUT, 0U);
    st32(png + ISAAC_NP_PNG_ROW_NUMBER, s->height);
    if (row_buf) {
        st8(row_buf, (uint8_t)s->last_filter);
        memcpy((void *)(uintptr_t)(row_buf + 1U), last + 1, s->rowbytes);
    }
    if (prev_row) {
        st8(prev_row, (uint8_t)s->last_filter);
        memcpy((void *)(uintptr_t)(prev_row + 1U), s_last_row, s->rowbytes);
    }
}

static void np_write_row_info(const np_session *s)
{
    uint32_t png = s->png;
    st32(png + ISAAC_NP_PNG_RI_WIDTH, s->width);
    st32(png + ISAAC_NP_PNG_RI_ROWBYTES, s->rowbytes);
    st8(png + ISAAC_NP_PNG_RI_COLOR_TYPE, (uint8_t)s->color_type);
    st8(png + ISAAC_NP_PNG_RI_BIT_DEPTH, 8U);
    st8(png + ISAAC_NP_PNG_RI_CHANNELS, (uint8_t)s->channels);
    st8(png + ISAAC_NP_PNG_RI_PIXEL_DEPTH, (uint8_t)(8U * s->channels));
}

static void np_finish_native(np_session *s)
{
    ++s_stats.native;
    s_stats.rows += s->height;
    s_stats.kib += (s->height * s->rowbytes) >> 10;
    s_stats.decode_us += s->decode_us;
    s_stats.io_us += s->io_us;
    s_stats.serve_us += s->serve_us;
    if (s->decode_us > s_stats.decode_us_max)
        s_stats.decode_us_max = s->decode_us;
    if (s->gamma)
        ++s_stats.gamma_images;
#if NP_RECEIPT
    np_log_image(s);
    np_maybe_stats();
#endif
    np_scratch_trim();
    np_session_reset();
}

/* Native serve: one row per call, exactly the translated epilogue. */
static void np_serve_row(CPU *__restrict c, np_session *s, uint32_t row)
{
#if NP_TIMING
    uint64_t t0 = np_now_us();
#endif
    uint32_t k = s->next_row;
    uint32_t stride = s->rowbytes + 1U;
    const uint8_t *src = s->raw + (size_t)k * stride + 1;

    if (k == 0U)
        np_write_row_info(s);
    if (row)
        memcpy((void *)(uintptr_t)row, src, s->rowbytes);
    st32(s->png + ISAAC_NP_PNG_ROW_NUMBER, k + 1U);
    s->next_row = k + 1U;
    (void)gpop(c);
    if (s->next_row == s->height) {
        np_write_final_state(s);
#if NP_TIMING
        s->serve_us += (uint32_t)(np_now_us() - t0);
#endif
        np_finish_native(s);
        return;
    }
#if NP_TIMING
    s->serve_us += (uint32_t)(np_now_us() - t0);
#endif
}

#if defined(ISAAC_VITA_NATIVE_PNG_ROW_BATCH)
/* Frozen ImagePng 005a13a2, immediately after the first original read-row
 * call. The decoder has already accepted the complete image; this is never
 * used by translated/VERIFY/interlaced/error paths. Keep the original final
 * iteration so the caller restores its exact registers, flags, stack and
 * libpng final state. No guest callback is skipped on an eligible session. */
int isaac_vita_native_png_middle_rows_try(CPU *__restrict c)
{
    static unsigned engaged_logged;
    const int saved_errno = errno;
    np_session *s = &s_ses;
    uint32_t token = 0u, base, stride, y, skipped;
    int result = 0;
#if NP_TIMING
    uint64_t t0;
#endif
    if (!c || c->fault || !s->active || s->mode != NP_MODE_SERVE ||
        s->next_row != 1u || s->height < 3u || c->ebx != s->png ||
        c->edi != s->height || !s->raw ||
        c->ebp < 0x43cu ||
        !guest_stack_contains(c, c->ebp - 0x43cu, 0x444u) ||
        !guest_stack_contains(c, c->esp, 4u) ||
        ld32(c->ebp - 0x43cu) != c->esi ||
        ld32(c->ebp - 0x438u) != s->height ||
        ld32(c->ebp - 0x418u) != s->png ||
        ld32(c->ebp - 0x410u) != 1u ||
        (ld32(c->ebp + 4u) != 0x005a0c7eu &&
         ld32(c->ebp + 4u) != 0x005a0d62u) ||
        ld32(s->png + ISAAC_NP_PNG_ROW_NUMBER) != 1u)
        goto done;
    token = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)c->esi, (const void *)(uintptr_t)c->esi,
        (size_t)s->height * 4u);
    if (!token)
        goto done;
    base = ld32(c->esi);
    stride = ld32(c->esi + 4u) - base;
    if (base != c->edx || !base || stride < s->rowbytes ||
        (uint64_t)base + (uint64_t)(s->height - 1u) * stride + s->rowbytes > UINT32_MAX)
        goto release;
    /* The table is leased and read-only for the whole batch. Reject aliases,
     * null/nonlinear destinations and altered row pointers before any copy. */
    for (y = 1u; y < s->height; ++y)
        if (ld32(c->esi + y * 4u) != base + y * stride)
            goto release;
#if NP_TIMING
    t0 = np_now_us();
#endif
    if (!isaac_vita_texel_scratch_png_middle_rows(base, stride,
            s->rowbytes, s->height, s->raw))
        goto release;
    skipped = s->height - 2u;
    s->next_row += skipped;
    st32(s->png + ISAAC_NP_PNG_ROW_NUMBER, s->next_row);
    c->esi += skipped * 4u;
    c->edi -= skipped;
#if NP_TIMING
    s->serve_us += (uint32_t)(np_now_us() - t0);
#endif
    result = 1;
release:
    if (!isaac_vita_guest_heap_lease_release(token)) {
        errno = saved_errno;
        guest_fault(c, c->esi, "PNG row batch table lease release failed");
        result = -1;
    }
done:
    if (result > 0 && !engaged_logged) {
        engaged_logged = 1u;
        isaac_vita_log("KAGE VITA PNG ROW BATCH: engaged middle=%u "
            "height=%u rowbytes=%u first-final=original",
            (unsigned)(s->height - 2u), (unsigned)s->height,
            (unsigned)s->rowbytes);
    }
    errno = saved_errno;
    return result;
}
#endif

#if defined(ISAAC_VITA_NATIVE_PNG_VERIFY)
static void np_verify_finish(CPU *__restrict c, np_session *s)
{
    uint32_t png = s->png;
    uint32_t pos = 0U;
    uint32_t row_buf = ld32(png + ISAAC_NP_PNG_ROW_BUF);
    uint32_t prev_row = ld32(png + ISAAC_NP_PNG_PREV_ROW);
    uint32_t stride = s->rowbytes + 1U;
    const uint8_t *last = s->raw +
                          (size_t)(s->height - 1U) * stride;
    const char *result;

    if (ld32(png + ISAAC_NP_PNG_IDAT_SIZE) != 0U) s->v_state_bad |= 1U;
    if (ld32(png + ISAAC_NP_PNG_CRC) != s->crc_final) s->v_state_bad |= 2U;
    if (!(ld32(png + ISAAC_NP_PNG_MODE) & ISAAC_NP_MODE_AFTER_IDAT))
        s->v_state_bad |= 4U;
    if (!(ld32(png + ISAAC_NP_PNG_FLAGS) & ISAAC_NP_FLAG_ZLIB_FINISHED))
        s->v_state_bad |= 8U;
    if (ld32(png + ISAAC_NP_PNG_ROW_NUMBER) != s->height) s->v_state_bad |= 16U;
    if (ld32(png + ISAAC_NP_PNG_Z_AVAIL_IN) != 0U) s->v_state_bad |= 32U;
    if (ld32(png + ISAAC_NP_PNG_Z_AVAIL_OUT) != 0U) s->v_state_bad |= 64U;
    if (row_buf && (ld8(row_buf) != s->last_filter ||
                    memcmp((const void *)(uintptr_t)(row_buf + 1U), last + 1,
                           s->rowbytes) != 0))
        s->v_rowbuf_bad |= 1U;
    if (prev_row && (ld8(prev_row) != s->last_filter ||
                     memcmp((const void *)(uintptr_t)(prev_row + 1U),
                            s_last_row, s->rowbytes) != 0))
        s->v_rowbuf_bad |= 2U;
    if (!np_guest_tell(c, s->stream, &pos) || pos != s->end_pos)
        s->v_pos_bad = 1U;

    ++s_stats.verify_images;
    s_stats.verify_diff_bytes += s->v_diff_bytes;
    if (s->v_diff_rows) ++s_stats.verify_diff_images;
    if (s->v_state_bad || s->v_rowbuf_bad) ++s_stats.verify_state_bad;
    if (s->v_pos_bad) ++s_stats.verify_pos_bad;
    result = (s->v_diff_rows == 0U && !s->v_state_bad && !s->v_rowbuf_bad &&
              !s->v_pos_bad) ? "MATCH" : "DIFF";
    if (np_dense(s_stats.verify_images, ISAAC_NP_RECEIPT_IMAGES_DENSE) ||
            strcmp(result, "MATCH") != 0) {
        isaac_vita_log(
            "KAGE VITA NATIVE PNG VERIFY: n=%u png=%08x %ux%u ct=%u gamma=%u "
            "rows=%u diff_rows=%u diff_bytes=%u first=%d/%d state_bad=%02x "
            "rowbuf_bad=%u pos=%s(%u/%u) native_us=%u translated_us=%u "
            "result=%s",
            (unsigned)s->id, (unsigned)s->png, (unsigned)s->width,
            (unsigned)s->height, (unsigned)s->color_type, (unsigned)s->gamma,
            (unsigned)s->height, (unsigned)s->v_diff_rows,
            (unsigned)s->v_diff_bytes, (int)s->v_first_row,
            (int)s->v_first_off, (unsigned)s->v_state_bad,
            (unsigned)s->v_rowbuf_bad, s->v_pos_bad ? "DIFF" : "ok",
            (unsigned)pos, (unsigned)s->end_pos, (unsigned)s->decode_us,
            (unsigned)s->translated_us, result);
    }
    s_stats.translated_us += s->translated_us;
    if (s->translated_us > s_stats.translated_us_max)
        s_stats.translated_us_max = s->translated_us;
    np_finish_native(s);
}

/* Verify mode: the translated body serves the row, the wrapper compares it
 * with the native scratch and, after the last row, the final state. */
static void np_verify_row(CPU *__restrict c, np_session *s, uint32_t row,
                          uint32_t eax0)
{
    uint64_t t0 = np_now_us();
    uint32_t k = s->next_row;
    uint32_t stride = s->rowbytes + 1U;
    const uint8_t *src = s->raw + (size_t)k * stride + 1;
    uint32_t rn;

    np_call_real(c, eax0, s->png, row);
    s->translated_us += (uint32_t)(np_now_us() - t0);
    if (row) {
        const uint8_t *dst = (const uint8_t *)(uintptr_t)row;
        if (memcmp(dst, src, s->rowbytes) != 0) {
            uint32_t i;
            uint32_t diff = 0U;
            int32_t first = -1;
            for (i = 0U; i < s->rowbytes; ++i)
                if (dst[i] != src[i]) {
                    if (first < 0) first = (int32_t)i;
                    ++diff;
                }
            if (s->v_first_row < 0) {
                s->v_first_row = (int32_t)k;
                s->v_first_off = first;
            }
            ++s->v_diff_rows;
            s->v_diff_bytes += diff;
        }
    }
    rn = ld32(s->png + ISAAC_NP_PNG_ROW_NUMBER);
    s->next_row = k + 1U;
    if (rn != s->next_row)
        s->v_state_bad |= 0x100U;
    if (s->next_row >= s->height || rn >= s->height || rn <= k) {
        np_verify_finish(c, s);
        return;
    }
}
#endif

/* Validation of the frozen png_struct at the first row.  Every rejection
 * leaves the guest untouched; the reason names the field class. */
static const char *np_validate(uint32_t png, np_session *s)
{
    uint32_t mode, flags, tr, ct, ch, width, height, rowbytes;

    if (!png || (png & 3U))
        return "png-null";
    flags = ld32(png + ISAAC_NP_PNG_FLAGS);
    if (!(flags & ISAAC_NP_FLAG_ROW_INIT))
        return "row-init";
    if (flags & (ISAAC_NP_FLAG_ZLIB_FINISHED | ISAAC_NP_FLAG_CRC_MASK))
        return "flags";
    mode = ld32(png + ISAAC_NP_PNG_MODE);
    if (!(mode & ISAAC_NP_MODE_HAVE_IDAT) ||
            (mode & (ISAAC_NP_MODE_AFTER_IDAT | ISAAC_NP_MODE_HAVE_IEND)))
        return "mode";
    if (ld8(png + ISAAC_NP_PNG_INTERLACED) != 0U)
        return "interlaced";
    tr = ld32(png + ISAAC_NP_PNG_TRANSFORMATIONS);
    if (tr & ~ISAAC_NP_TRANSFORM_GAMMA)
        return "transform";
    if (ld32(png + ISAAC_NP_PNG_Z_AVAIL_IN) != 0U)
        return "zlib";
    if (ld32(png + ISAAC_NP_PNG_ROW_NUMBER) != 0U)
        return "row-number";
    if (ld8(png + ISAAC_NP_PNG_PASS) != 0U)
        return "pass";
    if (ld8(png + ISAAC_NP_PNG_BIT_DEPTH) != 8U)
        return "depth";
    ct = ld8(png + ISAAC_NP_PNG_COLOR_TYPE);
    ch = isaac_np_channels_for(ct);
    if (ch == 0U)
        return "color-type";
    if (ld8(png + ISAAC_NP_PNG_CHANNELS) != ch)
        return "channels";
    if (ld8(png + ISAAC_NP_PNG_PIXEL_DEPTH) != 8U * ch)
        return "pixel-depth";
    width = ld32(png + ISAAC_NP_PNG_WIDTH);
    height = ld32(png + ISAAC_NP_PNG_HEIGHT);
    if (width == 0U || width > ISAAC_NP_MAX_DIMENSION)
        return "width";
    if (height == 0U || height > ISAAC_NP_MAX_DIMENSION)
        return "height";
    if (ld32(png + ISAAC_NP_PNG_NUM_ROWS) != height)
        return "num-rows";
    rowbytes = ld32(png + ISAAC_NP_PNG_ROWBYTES);
    if (rowbytes != width * ch)
        return "rowbytes";
    if (ld32(png + ISAAC_NP_PNG_IROWBYTES) != rowbytes + 1U)
        return "irowbytes";
    if (ld32(png + ISAAC_NP_PNG_IWIDTH) != width)
        return "iwidth";
    if (!ld32(png + ISAAC_NP_PNG_ROW_BUF) || !ld32(png + ISAAC_NP_PNG_PREV_ROW))
        return "row-buf";
    if (ld32(png + ISAAC_NP_PNG_READ_DATA_FN) !=
            (uint32_t)GUEST_IMAGE_BASE + ISAAC_NP_DEFAULT_READ_FN_RVA)
        return "read-fn";
    if (ld32(png + ISAAC_NP_PNG_READ_ROW_FN) != 0U)
        return "read-row-fn";
    s->stream = ld32(png + ISAAC_NP_PNG_IO_PTR);
    if (!s->stream || (s->stream & 3U) || !ld32(s->stream))
        return "io-ptr";
    if ((uint64_t)height * (rowbytes + 1U) > ISAAC_NP_SCRATCH_MAX_BYTES)
        return "size";
    s->width = width;
    s->height = height;
    s->channels = ch;
    s->color_type = ct;
    s->rowbytes = rowbytes;
    s->gamma = (tr & ISAAC_NP_TRANSFORM_GAMMA) &&
               ld32(png + ISAAC_NP_PNG_GAMMA_TABLE) != 0U && ct != 3U;
    return NULL;
}

static void np_start(CPU *__restrict c, uint32_t png, uint32_t row,
                     uint32_t eax0, uint32_t site_ok)
{
    np_session *s = &s_ses;
    const char *reason;
    isaac_np_params params;
    isaac_np_work work;
    isaac_np_result result;
    np_guest_io io;
    uint8_t *raw;
    uint32_t p0 = 0U;
#if NP_TIMING
    uint64_t t0;
#endif

    ++s_stats.images;
    np_session_reset();
    if (!site_ok) {
        np_begin_translated(c, png, row, eax0, "site");
        return;
    }
    reason = np_validate(png, s);
    if (reason) {
        np_begin_translated(c, png, row, eax0, reason);
        return;
    }
#if NP_TIMING
    t0 = np_now_us();
#endif
    /* The stream must be seekable for the rewind; probe before any byte is
     * consumed (a seek to the current position is a no-op). */
    if (!np_guest_tell(c, s->stream, &p0)) {
        np_begin_translated(c, png, row, eax0, "stream-tell");
        return;
    }
    if (!np_guest_seek(c, s->stream, p0)) {
        np_begin_translated(c, png, row, eax0, "stream-seek");
        return;
    }
    raw = np_scratch_acquire(s->height * (s->rowbytes + 1U));
    if (!raw) {
        np_begin_translated(c, png, row, eax0, "memory");
        return;
    }
    params.width = s->width;
    params.height = s->height;
    params.channels = s->channels;
    params.color_type = s->color_type;
    params.rowbytes = s->rowbytes;
    params.first_remaining = ld32(png + ISAAC_NP_PNG_IDAT_SIZE);
    params.crc_entry = ld32(png + ISAAC_NP_PNG_CRC);
    params.gamma_table = s->gamma
        ? (const uint8_t *)(uintptr_t)ld32(png + ISAAC_NP_PNG_GAMMA_TABLE)
        : NULL;
    work.raw = raw;
    work.staging = s_staging;
    work.staging_bytes = ISAAC_NP_STAGING_BYTES;
    work.tinfl_state = s_tinfl_state;
    work.last_row_pre_gamma = s_last_row;
#if NP_TIMING
    work.now_us = np_now_us;
#else
    work.now_us = NULL;
#endif
    io.c = c;
    io.stream = s->stream;
    {
        KAGE_VITA_DEEP_SCOPE_BYTES(KVD_PNG_DECODE,
            (uint64_t)s->height * (s->rowbytes + 1u));
#if defined(ISAAC_VITA_DEEP_PROFILE)
        uint64_t deep_png_begin = np_deep_clock();
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE
        (void)isaac_np_decode_reusing(&params, np_guest_read, &io, &work, &result,
                                    &s_reuse);
#else
        (void)isaac_np_decode(&params, np_guest_read, &io, &work, &result);
#endif
#if defined(ISAAC_VITA_DEEP_PROFILE)
        np_deep_note(s, &result, deep_png_begin, np_deep_clock());
#endif
    }
#if ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
    s_stats.strict_attempts += result.strict_attempts;
    s_stats.strict_successes += result.strict_successes;
    s_stats.strict_refusals += result.strict_refusals;
#endif
    if (result.status != ISAAC_NP_OK) {
        static const char *const names[ISAAC_NP_STATUS_COUNT] = {
            "decode-ok", "decode-read", "decode-crc", "decode-not-idat",
            "decode-length", "decode-inflate", "decode-extra",
            "decode-truncated", "decode-filter", "decode-param"
        };
        const char *why = (result.status >= 0 &&
                           result.status < ISAAC_NP_STATUS_COUNT)
                          ? names[result.status] : "decode-?";
        if (!np_guest_seek(c, s->stream, p0)) {
            ++s_stats.rewind_unsafe;
            isaac_vita_log(
                "KAGE VITA NATIVE PNG REWIND FAILED: png=%08x pos=%u after %s "
                "(translated decoder continues from a moved stream)",
                (unsigned)png, (unsigned)p0, why);
        }
        np_scratch_trim();
        np_begin_translated(c, png, row, eax0, why);
        return;
    }
    s->active = 1U;
    s->raw = raw;
    s->id = s_stats.images;
    s->png = png;
    s->p0 = p0;
    s->end_pos = p0 + result.stream_bytes;
    s->crc_final = result.crc_final;
    s->chunks = result.chunks;
    s->idat_bytes = result.idat_bytes;
    s->last_filter = result.last_filter;
    memcpy(s->filters, result.filters, sizeof(s->filters));
    s->io_us = result.io_us;
#if NP_TIMING
    s->decode_us = (uint32_t)(np_now_us() - t0);
#endif
    s->next_row = 0U;
    s->v_first_row = -1;
    s->v_first_off = -1;
#if defined(ISAAC_VITA_NATIVE_PNG_VERIFY)
    if (!np_guest_seek(c, s->stream, p0)) {
        ++s_stats.rewind_unsafe;
        isaac_vita_log(
            "KAGE VITA NATIVE PNG REWIND FAILED: png=%08x pos=%u before verify",
            (unsigned)png, (unsigned)p0);
    }
    s->mode = NP_MODE_VERIFY;
    np_verify_row(c, s, row, eax0);
#else
    s->mode = NP_MODE_SERVE;
    np_serve_row(c, s, row);
#endif
}

void __wrap_sub_005b1500(CPU *__restrict c)
{
    uint32_t eax0 = c->eax;
    uint32_t png = c->ecx;
    uint32_t row = c->edx;
    uint32_t ret;
    np_session *s = &s_ses;

    np_log_init();
    ret = ld32(guest_stack_address(c, c->esp, 4U, ISAAC_NP_READ_ROW_RVA));
    if (s->active) {
        if (s->png != png && png && !(png & 3U) &&
                ld32(png + ISAAC_NP_PNG_ROW_NUMBER) != 0U) {
            /* A different image in mid-flight while ours is unfinished
             * (the loader never interleaves; keep both exact regardless):
             * that one runs translated, ours goes on. */
            ++s_stats.busy;
            ++s_stats.passthrough;
            np_call_real(c, eax0, png, row);
            return;
        }
        if (s->png == png &&
                ld32(png + ISAAC_NP_PNG_ROW_NUMBER) == s->next_row &&
                s->next_row < s->height) {
            switch (s->mode) {
            case NP_MODE_SERVE:
                np_serve_row(c, s, row);
                return;
            case NP_MODE_TRANSLATED:
                np_translated_row(c, s, row, eax0);
                return;
#if defined(ISAAC_VITA_NATIVE_PNG_VERIFY)
            case NP_MODE_VERIFY:
                np_verify_row(c, s, row, eax0);
                return;
#endif
            default:
                break;
            }
        }
        /* The loader left this image (error longjmp) and a new one starts,
         * on the same or another png_struct: drop the stale session. */
        ++s_stats.abandoned;
        np_scratch_trim();
        np_session_reset();
    }
    if (!png || (png & 3U)) {
        ++s_stats.passthrough;
        np_call_real(c, eax0, png, row);
        return;
    }
    if (ld32(png + ISAAC_NP_PNG_ROW_NUMBER) != 0U) {
        ++s_stats.passthrough;
        np_call_real(c, eax0, png, row);
        return;
    }
    /* The loop's `call 0x5b1500` is translated as gpush_generated(0x5a13a2):
     * return words are plain RVAs, unlike code pointers stored in guest
     * memory (read_data_fn above), which carry the image base. */
    np_start(c, png, row, eax0, ret == ISAAC_NP_READ_ROW_SITE_RVA);
}

#endif /* !NP_HOST_BUILD */
