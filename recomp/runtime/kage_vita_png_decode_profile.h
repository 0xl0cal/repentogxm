#ifndef KAGE_VITA_PNG_DECODE_PROFILE_H
#define KAGE_VITA_PNG_DECODE_PROFILE_H

#include <stdint.h>

/* Cumulative, saturating counters for exact frozen libpng/zlib seams.
 *
 * One whole image per 32-image block is selected.  Image, row-filter,
 * inflate, and nested inflate_fast clocks are enabled only for that same
 * cohort; every unsampled call still contributes the cheap counters and byte
 * deltas.  All raw timings are observed upper bounds.  The outer image time
 * includes all nested profiler clock overhead, inflate includes inflate_fast,
 * and `inflate_fast_residual_*` is sampled-image inflate minus fast time.  The
 * one-time clock calibration is reported separately and is never subtracted.
 * None of these figures may be reported as pure PNG, DEFLATE, or unfilter CPU
 * share.  A high signal only justifies a later native A/B with differential
 * proof; this diagnostic profiler replaces no decoder code. */
typedef struct KageVitaPngDecodeProfileSnapshot {
    uint32_t images_started;
    uint32_t images_completed;
    uint32_t image_samples;
    uint32_t image_sample_raw_total_us;
    uint32_t image_sample_raw_max_us;
    uint32_t image_total_bytes;
    uint32_t image_max_bytes;

    uint32_t rows_started;
    uint32_t rows_completed;
    uint32_t row_samples;
    uint32_t row_sample_raw_total_us;
    uint32_t row_sample_raw_max_us;
    uint32_t row_total_bytes;
    uint32_t row_max_bytes;
    uint32_t filters[5];
    uint32_t bpp[5];              /* 1, 2, 3, 4, other/zero. */

    /* Inflate classes: png_read_row, png_read_finish_row, ancillary IDAT. */
    uint32_t inflate_calls[3];
    uint32_t inflate_completed[3];
    uint32_t inflate_outside_image;
    uint32_t inflate_invalid_class;
    /* NULL or arithmetic-wrap-risk streams: call counted, totals unread. */
    uint32_t inflate_uncaptured_streams;
    uint32_t inflate_total_in;
    uint32_t inflate_total_out;
    uint32_t inflate_total_wraps;
    uint32_t inflate_samples;
    uint32_t inflate_sample_raw_total_us;
    uint32_t inflate_sample_raw_max_us;

    uint32_t fast_calls;
    uint32_t fast_completed;
    uint32_t fast_outside_image;
    uint32_t fast_without_inflate;
    uint32_t fast_samples;
    uint32_t fast_sample_raw_total_us;
    uint32_t fast_sample_raw_max_us;
    uint32_t inflate_fast_residual_samples;
    uint32_t inflate_fast_residual_raw_total_us;
    uint32_t inflate_fast_residual_raw_max_us;

    uint32_t image_depth;
    uint32_t row_depth;
    uint32_t inflate_depth;
    uint32_t fast_depth;
    uint32_t nested_images;
    uint32_t nested_rows;
    uint32_t nested_inflates;
    uint32_t nested_fast;
    uint32_t abandoned_rows;
    uint32_t abandoned_inflates;
    uint32_t abandoned_fast;
    uint32_t mismatches;
    uint32_t invalid_filters;
    uint32_t invalid_metadata;

    /* Healthy exact census:
     * profile_clock_calls == timed_regions_started + timed_regions_completed.
     * An abandoned longjmp region has a start clock but deliberately no
     * invented end clock.  Calibration is exactly 65 calls / 64 intervals. */
    uint32_t profile_clock_calls;
    uint32_t timed_regions_started;
    uint32_t timed_regions_completed;
    uint32_t calibration_clock_calls;
    uint32_t calibration_intervals;
    uint32_t calibration_raw_total_us;
    uint32_t calibration_raw_max_us;
    uint32_t clock_backwards;

    /* Default-off native-unfilter A/B census.  Every attempted entry is
     * exactly one handled row or one original-body fallback. */
    uint32_t native_attempts;
    uint32_t native_handled;
    uint32_t native_fallbacks;
    uint32_t native_rejects[8];
    uint32_t native_invalid_outcomes;
    uint32_t bad;
} KageVitaPngDecodeProfileSnapshot;

#define KAGE_VITA_PNG_IMAGE_SAMPLE_STRIDE 32u
#define KAGE_VITA_PNG_INFLATE_ROW 0u
#define KAGE_VITA_PNG_INFLATE_FINISH 1u
#define KAGE_VITA_PNG_INFLATE_ANCILLARY 2u
#define KAGE_VITA_PNG_INFLATE_CLASSES 3u
#define KAGE_VITA_PNG_CALIBRATION_CALLS 65u
#define KAGE_VITA_PNG_CALIBRATION_INTERVALS 64u

enum {
    KAGE_VITA_PNG_NATIVE_HANDLED = 0u,
    KAGE_VITA_PNG_NATIVE_REJECT_STACK = 1u,
    KAGE_VITA_PNG_NATIVE_REJECT_ABI = 2u,
    KAGE_VITA_PNG_NATIVE_REJECT_FILTER = 3u,
    KAGE_VITA_PNG_NATIVE_REJECT_METADATA = 4u,
    KAGE_VITA_PNG_NATIVE_REJECT_RANGE = 5u,
    KAGE_VITA_PNG_NATIVE_REJECT_ALIAS = 6u,
    KAGE_VITA_PNG_NATIVE_REJECT_HELPER = 7u,
    KAGE_VITA_PNG_NATIVE_REJECT_RELEASE = 8u,
    KAGE_VITA_PNG_NATIVE_OUTCOME_COUNT = 9u
};

void kage_vita_png_profile_image_begin(void);
void kage_vita_png_profile_image_end(void);
void kage_vita_png_profile_row_begin(
    uint32_t rowbytes, uint32_t pixel_depth, uint32_t filter);
void kage_vita_png_profile_row_end(void);
void kage_vita_png_profile_inflate_begin(
    uint32_t caller_class, uint32_t zstream_capture_valid,
    uint32_t total_in, uint32_t total_out);
void kage_vita_png_profile_inflate_end(
    uint32_t zstream_capture_valid, uint32_t total_in, uint32_t total_out);
void kage_vita_png_profile_fast_begin(void);
void kage_vita_png_profile_fast_end(void);
void kage_vita_png_profile_native_result(uint32_t outcome);
#if defined(ISAAC_KAGE_VITA_PNG_DECODE_PROFILE_ORACLE)
void kage_vita_png_profile_reset(void);
int kage_vita_png_profile_snapshot_get(
    KageVitaPngDecodeProfileSnapshot *snapshot);
void kage_vita_png_profile_oracle_emit_max_records(void);
#endif

#endif
