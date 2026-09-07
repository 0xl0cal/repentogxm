#ifndef HOST_VITA_PNG_WINDOW_PROFILE_H
#define HOST_VITA_PNG_WINDOW_PROFILE_H
#include <stdint.h>

/* Read-only snapshot of counters the native decoder already maintains. No
 * extra per-row clocks, no receipt logging, no reset of decoder state.
 * Decode includes stream I/O; serve and translated describe row work, NOT
 * the surrounding image loader, palette expansion or GL upload. Completed
 * sessions contribute their full time in the window where they finish.
 * Failed native decode attempts before a translated fallback are NOT timed
 * by these existing totals; fallback classifications expose that coverage gap.
 * Owner-thread only, like the decoder's existing singleton session.
 */
typedef struct {
    uint32_t images, native, fallbacks, passthrough, busy, abandoned;
    uint32_t rows, kib, alloc_refused, reserved, oversize, rewind_unsafe;
    uint32_t fb_shape, fb_state, fb_stream, fb_memory, fb_decode;
    uint32_t decode_max_ever, translated_max_ever, active, mode;
    uint64_t decode_us, io_us, serve_us, translated_us;
#if defined(ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT) && ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
    /* Modular cumulative counters; success means exact filtered-raw decode,
     * not complete PNG/GL publication. Refusal resumes original tinfl. */
    uint32_t strict_attempts, strict_successes, strict_refusals;
#endif
#if defined(ISAAC_VITA_NATIVE_PNG_REUSE) && ISAAC_VITA_NATIVE_PNG_REUSE
    uint32_t reuse_lookups, reuse_hits, reuse_stores, reuse_evictions;
    uint32_t reuse_skipped, reuse_hit_kib;
#endif
} IsaacVitaPngWindowSnapshot;
void isaac_vita_png_window_snapshot(IsaacVitaPngWindowSnapshot *out);
#if defined(ISAAC_VITA_DEEP_PROFILE)
/* Eight longest native decode attempts, plus totals for ALL attempts grouped
 * by strict-inflater outcome. Image IDs/dimensions are not resource filenames.
 * Includes failed attempts; excludes shape/stream rejects before decode. */
void isaac_vita_png_deep_report(const char *bid, uint32_t window);
#endif
#endif
