/* Aggregate timing for exact ImagePng, row-filter, inflate, and inflate_fast
 * seams.  No per-call record is emitted and no decoder state is modified.
 *
 * The frozen owner/xref/ABI proof lives in gen_all.py.  In particular, all
 * libpng longjmps return inside ImagePng, so its outer end hook always runs.
 * That hook explicitly abandons any inner region whose end hook was skipped.
 * Translated guest execution is assumed to be serialized on one owner thread;
 * this profiler is deliberately not a synchronization primitive.
 */
#include "kage_vita_png_decode_profile.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
#include "host_vita_png_outer_profile.h"
#endif

#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
int sceClibPrintf(const char *format, ...);
uint64_t sceKernelGetProcessTimeWide(void);
# define KAGE_VITA_PNG_LOG sceClibPrintf
#else
# include <psp2/kernel/processmgr.h>
void isaac_vita_log(const char *format, ...);
# define KAGE_VITA_PNG_LOG isaac_vita_log
#endif

#ifndef ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID
# define ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID "png:unstamped"
#endif

_Static_assert(KAGE_VITA_PNG_IMAGE_SAMPLE_STRIDE == 32u,
               "PNG image cohort stride drifted");
_Static_assert(KAGE_VITA_PNG_CALIBRATION_CALLS == 65u &&
                   KAGE_VITA_PNG_CALIBRATION_INTERVALS == 64u,
               "PNG clock calibration census drifted");

#define KAGE_VITA_PNG_BUILD_ID_MAX 64u
#define KAGE_VITA_PNG_LOG_A \
    "pngprof2.a %s i=%u/%u is=%u/%u/%u ib=%u/%u " \
    "r=%u/%u rs=%u/%u/%u rb=%u/%u bad=%u"
#define KAGE_VITA_PNG_LOG_B \
    "pngprof2.b %s f=%u,%u,%u,%u,%u bpp=%u,%u,%u,%u,%u " \
    "filtbad=%u meta=%u bad=%u"
#define KAGE_VITA_PNG_LOG_C \
    "pngprof2.c %s ic=%u/%u,%u/%u,%u/%u outside=%u uncaptured=%u ik=%u " \
    "bytes=%u/%u wrap=%u sample=%u/%u/%u depth=%u nest=%u " \
    "abandoned=%u bad=%u"
#define KAGE_VITA_PNG_LOG_D \
    "pngprof2.d %s fast=%u/%u outside=%u noinf=%u sample=%u/%u/%u " \
    "residual=%u/%u/%u depth=%u nest=%u abandoned=%u bad=%u"
#define KAGE_VITA_PNG_LOG_E \
    "pngprof2.e %s depth=%u,%u nest=%u,%u abandoned_row=%u mm=%u " \
    "clock=%u/%u/%u cal=%u/%u/%u/%u back=%u bad=%u"
#define KAGE_VITA_PNG_LOG_F \
    "pngprof2.f %s native=%u/%u/%u reject=%u,%u,%u,%u,%u,%u,%u,%u " \
    "invalid=%u bad=%u"
#define KAGE_VITA_PNG_U32_TEXT_GROWTH 8u
#define KAGE_VITA_PNG_LOG_BOUND(format, fields) \
    (sizeof(format) - 1u + (fields) * KAGE_VITA_PNG_U32_TEXT_GROWTH + \
     (KAGE_VITA_PNG_BUILD_ID_MAX - 2u))
_Static_assert(sizeof(ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID) - 1u <=
                   KAGE_VITA_PNG_BUILD_ID_MAX,
               "PNG profile build ID exceeds its complete-log contract");
_Static_assert(KAGE_VITA_PNG_LOG_BOUND(KAGE_VITA_PNG_LOG_A, 15u) < 384u,
               "PNG profile A record exceeds isaac_vita_log body[384]");
_Static_assert(KAGE_VITA_PNG_LOG_BOUND(KAGE_VITA_PNG_LOG_B, 13u) < 384u,
               "PNG profile B record exceeds isaac_vita_log body[384]");
_Static_assert(KAGE_VITA_PNG_LOG_BOUND(KAGE_VITA_PNG_LOG_C, 19u) < 384u,
               "PNG profile C record exceeds isaac_vita_log body[384]");
_Static_assert(KAGE_VITA_PNG_LOG_BOUND(KAGE_VITA_PNG_LOG_D, 14u) < 384u,
               "PNG profile D record exceeds isaac_vita_log body[384]");
_Static_assert(KAGE_VITA_PNG_LOG_BOUND(KAGE_VITA_PNG_LOG_E, 15u) < 384u,
               "PNG profile E record exceeds isaac_vita_log body[384]");
_Static_assert(KAGE_VITA_PNG_LOG_BOUND(KAGE_VITA_PNG_LOG_F, 13u) < 384u,
               "PNG profile F record exceeds isaac_vita_log body[384]");

static KageVitaPngDecodeProfileSnapshot s_png;
static uint64_t s_image_started_at;
static uint64_t s_row_started_at;
static uint64_t s_inflate_started_at;
static uint64_t s_fast_started_at;
static uint32_t s_image_bytes;
static uint32_t s_rowbytes;
static uint32_t s_image_timed;
static uint32_t s_row_timed;
static uint32_t s_inflate_timed;
static uint32_t s_fast_timed;
static uint32_t s_inflate_class;
static uint32_t s_inflate_zstream_capture_valid;
static uint32_t s_inflate_total_in_before;
static uint32_t s_inflate_total_out_before;
static uint32_t s_image_inflate_raw_us;
static uint32_t s_image_fast_raw_us;
static uint32_t s_image_inner_abandoned;
static uint32_t s_image_block_position;
static uint32_t s_image_sample_phase;
static uint32_t s_image_sample_lcg = 0x01234567u;
static uint32_t s_clock_calibrated;
static uint32_t s_logged_bad;

static void png_bad(void)
{
    if (s_png.bad != UINT32_MAX)
        ++s_png.bad;
}

static void png_inc(uint32_t *value)
{
    if (*value == UINT32_MAX) {
        png_bad();
        return;
    }
    ++*value;
}

static void png_add(uint32_t *value, uint32_t amount)
{
    if (UINT32_MAX - *value < amount) {
        *value = UINT32_MAX;
        png_bad();
        return;
    }
    *value += amount;
}

static uint32_t png_delta(uint64_t started_at, uint64_t now)
{
    uint64_t elapsed;

    if (now < started_at) {
        png_inc(&s_png.clock_backwards);
        png_bad();
        return 0u;
    }
    elapsed = now - started_at;
    if (elapsed > UINT32_MAX) {
        png_bad();
        return UINT32_MAX;
    }
    return (uint32_t)elapsed;
}

static uint64_t png_profile_clock(void)
{
    png_inc(&s_png.profile_clock_calls);
    return sceKernelGetProcessTimeWide();
}

static uint64_t png_region_begin(void)
{
    png_inc(&s_png.timed_regions_started);
    return png_profile_clock();
}

static uint32_t png_region_end(uint64_t started_at)
{
    uint64_t now;

    png_inc(&s_png.timed_regions_completed);
    now = png_profile_clock();
    return png_delta(started_at, now);
}

static uint64_t png_calibration_clock(void)
{
    png_inc(&s_png.calibration_clock_calls);
    return sceKernelGetProcessTimeWide();
}

static void png_calibrate_clock(void)
{
    uint64_t previous;
    uint32_t index;

    if (s_clock_calibrated != 0u)
        return;
    s_clock_calibrated = 1u;
    previous = png_calibration_clock();
    for (index = 0u; index < KAGE_VITA_PNG_CALIBRATION_INTERVALS; ++index) {
        const uint64_t now = png_calibration_clock();
        const uint32_t elapsed = png_delta(previous, now);

        png_inc(&s_png.calibration_intervals);
        png_add(&s_png.calibration_raw_total_us, elapsed);
        if (elapsed > s_png.calibration_raw_max_us)
            s_png.calibration_raw_max_us = elapsed;
        previous = now;
    }
}

static void png_check_clock_invariant(void)
{
    const uint64_t expected =
        (uint64_t)s_png.timed_regions_started +
        (uint64_t)s_png.timed_regions_completed;
    const uint32_t expected_calibration =
        s_clock_calibrated != 0u ? KAGE_VITA_PNG_CALIBRATION_CALLS : 0u;
    const uint32_t expected_intervals =
        s_clock_calibrated != 0u ? KAGE_VITA_PNG_CALIBRATION_INTERVALS : 0u;

    if (s_png.bad == 0u &&
            (expected > UINT32_MAX ||
             s_png.profile_clock_calls != (uint32_t)expected ||
             s_png.calibration_clock_calls != expected_calibration ||
             s_png.calibration_intervals != expected_intervals)) {
        png_inc(&s_png.mismatches);
        png_bad();
    }
}

static void png_emit(void)
{
    int saved_errno = errno;

    png_check_clock_invariant();
    KAGE_VITA_PNG_LOG(
        KAGE_VITA_PNG_LOG_A,
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID,
        s_png.images_completed, s_png.images_started,
        s_png.image_samples, s_png.image_sample_raw_total_us,
        s_png.image_sample_raw_max_us,
        s_png.image_total_bytes, s_png.image_max_bytes,
        s_png.rows_completed, s_png.rows_started,
        s_png.row_samples, s_png.row_sample_raw_total_us,
        s_png.row_sample_raw_max_us,
        s_png.row_total_bytes, s_png.row_max_bytes, s_png.bad);
    KAGE_VITA_PNG_LOG(
        KAGE_VITA_PNG_LOG_B,
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID,
        s_png.filters[0], s_png.filters[1], s_png.filters[2],
        s_png.filters[3], s_png.filters[4],
        s_png.bpp[0], s_png.bpp[1], s_png.bpp[2], s_png.bpp[3],
        s_png.bpp[4], s_png.invalid_filters, s_png.invalid_metadata,
        s_png.bad);
    KAGE_VITA_PNG_LOG(
        KAGE_VITA_PNG_LOG_C,
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID,
        s_png.inflate_calls[0], s_png.inflate_completed[0],
        s_png.inflate_calls[1], s_png.inflate_completed[1],
        s_png.inflate_calls[2], s_png.inflate_completed[2],
        s_png.inflate_outside_image, s_png.inflate_uncaptured_streams,
        s_png.inflate_invalid_class, s_png.inflate_total_in,
        s_png.inflate_total_out, s_png.inflate_total_wraps,
        s_png.inflate_samples, s_png.inflate_sample_raw_total_us,
        s_png.inflate_sample_raw_max_us, s_png.inflate_depth,
        s_png.nested_inflates, s_png.abandoned_inflates, s_png.bad);
    KAGE_VITA_PNG_LOG(
        KAGE_VITA_PNG_LOG_D,
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID,
        s_png.fast_calls, s_png.fast_completed,
        s_png.fast_outside_image, s_png.fast_without_inflate,
        s_png.fast_samples, s_png.fast_sample_raw_total_us,
        s_png.fast_sample_raw_max_us,
        s_png.inflate_fast_residual_samples,
        s_png.inflate_fast_residual_raw_total_us,
        s_png.inflate_fast_residual_raw_max_us,
        s_png.fast_depth, s_png.nested_fast, s_png.abandoned_fast,
        s_png.bad);
    KAGE_VITA_PNG_LOG(
        KAGE_VITA_PNG_LOG_E,
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID,
        s_png.image_depth, s_png.row_depth,
        s_png.nested_images, s_png.nested_rows,
        s_png.abandoned_rows, s_png.mismatches,
        s_png.profile_clock_calls, s_png.timed_regions_started,
        s_png.timed_regions_completed,
        s_png.calibration_clock_calls, s_png.calibration_intervals,
        s_png.calibration_raw_total_us, s_png.calibration_raw_max_us,
        s_png.clock_backwards, s_png.bad);
    KAGE_VITA_PNG_LOG(
        KAGE_VITA_PNG_LOG_F,
        ISAAC_VITA_PNG_DECODE_PROFILE_BUILD_ID,
        s_png.native_attempts, s_png.native_handled,
        s_png.native_fallbacks,
        s_png.native_rejects[0], s_png.native_rejects[1],
        s_png.native_rejects[2], s_png.native_rejects[3],
        s_png.native_rejects[4], s_png.native_rejects[5],
        s_png.native_rejects[6], s_png.native_rejects[7],
        s_png.native_invalid_outcomes, s_png.bad);
    errno = saved_errno;
}

#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
void kage_vita_png_profile_reset(void)
{
    memset(&s_png, 0, sizeof s_png);
    s_image_started_at = 0u;
    s_row_started_at = 0u;
    s_inflate_started_at = 0u;
    s_fast_started_at = 0u;
    s_image_bytes = 0u;
    s_rowbytes = 0u;
    s_image_timed = 0u;
    s_row_timed = 0u;
    s_inflate_timed = 0u;
    s_fast_timed = 0u;
    s_inflate_class = 0u;
    s_inflate_zstream_capture_valid = 0u;
    s_inflate_total_in_before = 0u;
    s_inflate_total_out_before = 0u;
    s_image_inflate_raw_us = 0u;
    s_image_fast_raw_us = 0u;
    s_image_inner_abandoned = 0u;
    s_image_block_position = 0u;
    s_image_sample_phase = 0u;
    s_image_sample_lcg = 0x01234567u;
    s_clock_calibrated = 0u;
    s_logged_bad = 0u;
}
#endif

void kage_vita_png_profile_image_begin(void)
{
    png_inc(&s_png.images_started);
    if (s_png.image_depth != 0u) {
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
        isaac_vita_png_outer_begin();
#endif
        png_inc(&s_png.nested_images);
        png_bad();
        png_inc(&s_png.image_depth);
        return;
    }

    s_png.image_depth = 1u;
    s_image_bytes = 0u;
    s_image_inflate_raw_us = 0u;
    s_image_fast_raw_us = 0u;
    s_image_inner_abandoned = 0u;
    s_image_timed = s_image_block_position == s_image_sample_phase;
    ++s_image_block_position;
    if (s_image_block_position == KAGE_VITA_PNG_IMAGE_SAMPLE_STRIDE) {
        s_image_block_position = 0u;
        s_image_sample_lcg =
            s_image_sample_lcg * 1664525u + 1013904223u;
        s_image_sample_phase = s_image_sample_lcg >> 27;
    }
    if (s_image_timed != 0u) {
        png_calibrate_clock();
        s_image_started_at = png_region_begin();
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
        isaac_vita_png_outer_begin_at(s_image_started_at);
#endif
    }
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
    else {
        isaac_vita_png_outer_begin();
    }
#endif
}

static void png_abandon_inner_regions(void)
{
    if (s_png.fast_depth != 0u) {
        png_add(&s_png.abandoned_fast, s_png.fast_depth);
        s_png.fast_depth = 0u;
        s_fast_timed = 0u;
        s_image_inner_abandoned = 1u;
        png_inc(&s_png.mismatches);
        png_bad();
    }
    if (s_png.inflate_depth != 0u) {
        png_add(&s_png.abandoned_inflates, s_png.inflate_depth);
        s_png.inflate_depth = 0u;
        s_inflate_timed = 0u;
        s_image_inner_abandoned = 1u;
        png_inc(&s_png.mismatches);
        png_bad();
    }
    if (s_png.row_depth != 0u) {
        png_add(&s_png.abandoned_rows, s_png.row_depth);
        s_png.row_depth = 0u;
        s_row_timed = 0u;
        s_image_inner_abandoned = 1u;
        png_inc(&s_png.mismatches);
        png_bad();
    }
}

void kage_vita_png_profile_image_end(void)
{
    uint32_t elapsed = 0u;

    if (s_png.image_depth == 0u) {
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
        isaac_vita_png_outer_end();
#endif
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_png.image_depth > 1u) {
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
        isaac_vita_png_outer_end();
#endif
        --s_png.image_depth;
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }

    png_abandon_inner_regions();
    if (s_image_timed != 0u) {
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
        uint64_t now;
        png_inc(&s_png.timed_regions_completed);
        now = png_profile_clock();
        elapsed = png_delta(s_image_started_at, now);
        isaac_vita_png_outer_end_at(now);
#else
        elapsed = png_region_end(s_image_started_at);
#endif
    }
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
    else {
        isaac_vita_png_outer_end();
    }
#endif
    s_png.image_depth = 0u;
    png_inc(&s_png.images_completed);
    if (s_image_timed != 0u) {
        uint32_t residual = 0u;

        png_inc(&s_png.image_samples);
        png_add(&s_png.image_sample_raw_total_us, elapsed);
        if (elapsed > s_png.image_sample_raw_max_us)
            s_png.image_sample_raw_max_us = elapsed;
        if (s_image_inner_abandoned == 0u) {
            if (s_image_fast_raw_us <= s_image_inflate_raw_us)
                residual = s_image_inflate_raw_us - s_image_fast_raw_us;
            else {
                png_inc(&s_png.mismatches);
                png_bad();
            }
            png_inc(&s_png.inflate_fast_residual_samples);
            png_add(&s_png.inflate_fast_residual_raw_total_us, residual);
            if (residual > s_png.inflate_fast_residual_raw_max_us)
                s_png.inflate_fast_residual_raw_max_us = residual;
        }
    }
    s_image_timed = 0u;
    png_add(&s_png.image_total_bytes, s_image_bytes);
    if (s_image_bytes > s_png.image_max_bytes)
        s_png.image_max_bytes = s_image_bytes;

    if (s_png.images_completed <= 4u ||
            (s_png.images_completed & 15u) == 0u ||
            (s_logged_bad == 0u && s_png.bad != 0u)) {
        png_emit();
        if (s_png.bad != 0u)
            s_logged_bad = 1u;
    }
}

void kage_vita_png_profile_row_begin(
    uint32_t rowbytes, uint32_t pixel_depth, uint32_t filter)
{
    uint32_t bytes_per_pixel;

    png_inc(&s_png.rows_started);
    if (filter < 5u)
        png_inc(&s_png.filters[filter]);
    else {
        png_inc(&s_png.invalid_filters);
        png_bad();
    }
    bytes_per_pixel = pixel_depth / 8u +
        ((pixel_depth & 7u) != 0u ? 1u : 0u);
    if (bytes_per_pixel >= 1u && bytes_per_pixel <= 4u)
        png_inc(&s_png.bpp[bytes_per_pixel - 1u]);
    else
        png_inc(&s_png.bpp[4]);
    if (rowbytes == 0u || pixel_depth == 0u) {
        png_inc(&s_png.invalid_metadata);
        png_bad();
    }
    if (s_png.image_depth != 1u) {
        png_inc(&s_png.mismatches);
        png_bad();
    }
    if (s_png.row_depth != 0u) {
        png_inc(&s_png.nested_rows);
        png_bad();
        png_inc(&s_png.row_depth);
        return;
    }

    s_png.row_depth = 1u;
    s_rowbytes = rowbytes;
    s_row_timed = s_png.image_depth == 1u && s_image_timed != 0u;
    if (s_row_timed != 0u)
        s_row_started_at = png_region_begin();
}

void kage_vita_png_profile_row_end(void)
{
    uint32_t elapsed = 0u;

    if (s_png.row_depth == 0u) {
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_png.row_depth > 1u) {
        --s_png.row_depth;
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_row_timed != 0u)
        elapsed = png_region_end(s_row_started_at);
    s_png.row_depth = 0u;
    png_inc(&s_png.rows_completed);
    if (s_row_timed != 0u) {
        png_inc(&s_png.row_samples);
        png_add(&s_png.row_sample_raw_total_us, elapsed);
        if (elapsed > s_png.row_sample_raw_max_us)
            s_png.row_sample_raw_max_us = elapsed;
    }
    s_row_timed = 0u;
    png_add(&s_png.row_total_bytes, s_rowbytes);
    if (s_rowbytes > s_png.row_max_bytes)
        s_png.row_max_bytes = s_rowbytes;
    if (s_png.image_depth == 1u)
        png_add(&s_image_bytes, s_rowbytes);
    else {
        png_inc(&s_png.mismatches);
        png_bad();
    }
}

void kage_vita_png_profile_inflate_begin(
    uint32_t caller_class, uint32_t zstream_capture_valid,
    uint32_t total_in, uint32_t total_out)
{
    if (caller_class < KAGE_VITA_PNG_INFLATE_CLASSES)
        png_inc(&s_png.inflate_calls[caller_class]);
    else {
        png_inc(&s_png.inflate_invalid_class);
        png_bad();
    }
    if (zstream_capture_valid == 0u)
        png_inc(&s_png.inflate_uncaptured_streams);
    if (s_png.image_depth != 1u)
        png_inc(&s_png.inflate_outside_image);
    if (s_png.inflate_depth != 0u) {
        png_inc(&s_png.nested_inflates);
        png_bad();
        png_inc(&s_png.inflate_depth);
        return;
    }

    s_png.inflate_depth = 1u;
    s_inflate_class = caller_class;
    s_inflate_zstream_capture_valid = zstream_capture_valid != 0u;
    s_inflate_total_in_before = total_in;
    s_inflate_total_out_before = total_out;
    s_inflate_timed = s_png.image_depth == 1u && s_image_timed != 0u;
    if (s_inflate_timed != 0u)
        s_inflate_started_at = png_region_begin();
}

void kage_vita_png_profile_inflate_end(
    uint32_t zstream_capture_valid, uint32_t total_in, uint32_t total_out)
{
    uint32_t elapsed = 0u;

    if (s_png.inflate_depth == 0u) {
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_png.inflate_depth > 1u) {
        --s_png.inflate_depth;
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_inflate_timed != 0u)
        elapsed = png_region_end(s_inflate_started_at);
    s_png.inflate_depth = 0u;
    if (s_inflate_class < KAGE_VITA_PNG_INFLATE_CLASSES)
        png_inc(&s_png.inflate_completed[s_inflate_class]);
    if (s_inflate_zstream_capture_valid !=
            (zstream_capture_valid != 0u)) {
        png_inc(&s_png.mismatches);
        png_bad();
        if (zstream_capture_valid == 0u)
            png_inc(&s_png.inflate_uncaptured_streams);
    } else if (s_inflate_zstream_capture_valid != 0u) {
        if (total_in < s_inflate_total_in_before)
            png_inc(&s_png.inflate_total_wraps);
        if (total_out < s_inflate_total_out_before)
            png_inc(&s_png.inflate_total_wraps);
        png_add(&s_png.inflate_total_in,
                total_in - s_inflate_total_in_before);
        png_add(&s_png.inflate_total_out,
                total_out - s_inflate_total_out_before);
    }
    if (s_inflate_timed != 0u) {
        png_inc(&s_png.inflate_samples);
        png_add(&s_png.inflate_sample_raw_total_us, elapsed);
        if (elapsed > s_png.inflate_sample_raw_max_us)
            s_png.inflate_sample_raw_max_us = elapsed;
        png_add(&s_image_inflate_raw_us, elapsed);
    }
    s_inflate_timed = 0u;
}

void kage_vita_png_profile_fast_begin(void)
{
    png_inc(&s_png.fast_calls);
    if (s_png.image_depth != 1u)
        png_inc(&s_png.fast_outside_image);
    if (s_png.inflate_depth != 1u) {
        png_inc(&s_png.fast_without_inflate);
        png_bad();
    }
    if (s_png.fast_depth != 0u) {
        png_inc(&s_png.nested_fast);
        png_bad();
        png_inc(&s_png.fast_depth);
        return;
    }

    s_png.fast_depth = 1u;
    s_fast_timed = s_png.image_depth == 1u &&
        s_png.inflate_depth == 1u && s_image_timed != 0u;
    if (s_fast_timed != 0u)
        s_fast_started_at = png_region_begin();
}

void kage_vita_png_profile_fast_end(void)
{
    uint32_t elapsed = 0u;

    if (s_png.fast_depth == 0u) {
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_png.fast_depth > 1u) {
        --s_png.fast_depth;
        png_inc(&s_png.mismatches);
        png_bad();
        return;
    }
    if (s_fast_timed != 0u)
        elapsed = png_region_end(s_fast_started_at);
    s_png.fast_depth = 0u;
    png_inc(&s_png.fast_completed);
    if (s_fast_timed != 0u) {
        png_inc(&s_png.fast_samples);
        png_add(&s_png.fast_sample_raw_total_us, elapsed);
        if (elapsed > s_png.fast_sample_raw_max_us)
            s_png.fast_sample_raw_max_us = elapsed;
        png_add(&s_image_fast_raw_us, elapsed);
    }
    s_fast_timed = 0u;
}

void kage_vita_png_profile_native_result(uint32_t outcome)
{
    png_inc(&s_png.native_attempts);
    if (outcome == KAGE_VITA_PNG_NATIVE_HANDLED) {
        png_inc(&s_png.native_handled);
        return;
    }

    png_inc(&s_png.native_fallbacks);
    if (outcome > KAGE_VITA_PNG_NATIVE_HANDLED &&
            outcome < KAGE_VITA_PNG_NATIVE_OUTCOME_COUNT) {
        png_inc(&s_png.native_rejects[outcome - 1u]);
        return;
    }
    png_inc(&s_png.native_invalid_outcomes);
    png_bad();
}

#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
void kage_vita_png_profile_oracle_emit_max_records(void)
{
    KageVitaPngDecodeProfileSnapshot saved = s_png;
    uint32_t saved_calibrated = s_clock_calibrated;

    memset(&s_png, 0xff, sizeof s_png);
    png_emit();
    s_png = saved;
    s_clock_calibrated = saved_calibrated;
}

int kage_vita_png_profile_snapshot_get(
    KageVitaPngDecodeProfileSnapshot *snapshot)
{
    if (!snapshot)
        return 0;
    *snapshot = s_png;
    return 1;
}
#endif
