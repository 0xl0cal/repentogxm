/* Hostile deterministic oracle for the aligned whole-image PNG cohort. */
#include "kage_vita_png_decode_profile.h"
#if defined(ISAAC_PNG_OUTER_PROFILE_ORACLE)
#include "host_vita_png_outer_profile.h"
#include <setjmp.h>
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "PNG profile oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint64_t s_now;
static uint64_t s_clock_step;
static uint32_t s_clock_calls;
static uint32_t s_log_calls;
static uint32_t s_log_embedded_newlines;
static size_t s_log_lengths[64];
static char s_logs[32768];
static size_t s_log_size;
static int s_clock_errno;

uint64_t sceKernelGetProcessTimeWide(void)
{
    uint64_t result = s_now;

    ++s_clock_calls;
    s_now += s_clock_step;
    if (s_clock_errno)
        errno = ERANGE;
    return result;
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    char rendered[512];
    int written;
    size_t slot = s_log_calls;

    ++s_log_calls;
    va_start(arguments, format);
    written = vsnprintf(rendered, sizeof rendered, format, arguments);
    va_end(arguments);
    if (written > 0 && (size_t)written < sizeof rendered) {
        size_t length = (size_t)written;

        if (slot < sizeof s_log_lengths / sizeof s_log_lengths[0])
            s_log_lengths[slot] = length;
        if (memchr(rendered, '\n', length) != NULL)
            ++s_log_embedded_newlines;
        /* Model isaac_vita_log's one-record-per-call terminator. */
        if (length + 1u < sizeof s_logs - s_log_size) {
            memcpy(s_logs + s_log_size, rendered, length);
            s_log_size += length;
            s_logs[s_log_size++] = '\n';
            s_logs[s_log_size] = '\0';
        }
    }
    return written;
}

#if !defined(ISAAC_PNG_OUTER_PROFILE_ORACLE)
static KageVitaPngDecodeProfileSnapshot snapshot(void)
{
    KageVitaPngDecodeProfileSnapshot result;

    memset(&result, 0xa5, sizeof result);
    if (!kage_vita_png_profile_snapshot_get(&result))
        memset(&result, 0, sizeof result);
    return result;
}

static void reset(void)
{
    kage_vita_png_profile_reset();
    s_now = 1000u;
    s_clock_step = 0u;
    s_log_calls = 0u;
    s_log_embedded_newlines = 0u;
    s_clock_calls = 0u;
    s_log_size = 0u;
    memset(s_log_lengths, 0, sizeof s_log_lengths);
    memset(s_logs, 0, sizeof s_logs);
}

static int cold_cohort_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;
    uint32_t image;

    /* Deliberately no reset: production also starts from zero-initialized
     * state plus the explicit 0x01234567 LCG seed. */
    for (image = 1u; image <= 64u; ++image) {
        uint32_t before = s_clock_calls;
        uint32_t expected;

        kage_vita_png_profile_image_begin();
        ++s_now;
        kage_vita_png_profile_image_end();
        expected = image == 1u ? 67u : (image == 36u ? 2u : 0u);
        CHECK(s_clock_calls - before == expected);
    }
    got = snapshot();
    CHECK(got.images_started == 64u && got.images_completed == 64u);
    CHECK(got.image_samples == 2u);
    CHECK(got.inflate_fast_residual_samples == 2u);
    CHECK(got.profile_clock_calls == 4u);
    CHECK(got.timed_regions_started == 2u &&
          got.timed_regions_completed == 2u);
    CHECK(got.calibration_clock_calls == KAGE_VITA_PNG_CALIBRATION_CALLS);
    CHECK(got.calibration_intervals == KAGE_VITA_PNG_CALIBRATION_INTERVALS);
    CHECK(s_clock_calls == 69u);
    CHECK(got.bad == 0u);
    return 0;
}

static int aligned_nested_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;

    reset();
    CHECK(kage_vita_png_profile_snapshot_get(NULL) == 0);
    s_clock_step = 1u;
    kage_vita_png_profile_image_begin();
    s_clock_step = 0u;
    /* Calibration: 65 consecutive calls => 64 one-us intervals.  The image
     * begin clock is the 66th call and advances once more before we stop it. */
    kage_vita_png_profile_row_begin(20u, 32u, 4u);
    s_now += 3u;
    kage_vita_png_profile_row_end();

    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_ROW, 1u, 10u, 20u);
    s_now += 2u;
    kage_vita_png_profile_fast_begin();
    s_now += 5u;
    kage_vita_png_profile_fast_end();
    ++s_now;
    kage_vita_png_profile_inflate_end(1u, 110u, 220u);

    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_FINISH, 1u, 110u, 220u);
    s_now += 4u;
    kage_vita_png_profile_inflate_end(1u, 112u, 223u);
    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_ANCILLARY, 1u, 112u, 223u);
    s_now += 6u;
    kage_vita_png_profile_inflate_end(1u, 119u, 232u);
    s_now += 2u;
    kage_vita_png_profile_image_end();

    got = snapshot();
    CHECK(got.images_started == 1u && got.images_completed == 1u);
    CHECK(got.image_samples == 1u);
    CHECK(got.image_sample_raw_total_us == 24u &&
          got.image_sample_raw_max_us == 24u);
    CHECK(got.rows_started == 1u && got.rows_completed == 1u);
    CHECK(got.row_samples == 1u && got.row_sample_raw_total_us == 3u);
    CHECK(got.image_total_bytes == 20u && got.row_total_bytes == 20u);
    CHECK(got.filters[4] == 1u && got.bpp[3] == 1u);
    CHECK(got.inflate_calls[0] == 1u && got.inflate_completed[0] == 1u);
    CHECK(got.inflate_calls[1] == 1u && got.inflate_completed[1] == 1u);
    CHECK(got.inflate_calls[2] == 1u && got.inflate_completed[2] == 1u);
    CHECK(got.inflate_total_in == 109u && got.inflate_total_out == 212u);
    CHECK(got.inflate_samples == 3u);
    CHECK(got.inflate_sample_raw_total_us == 18u &&
          got.inflate_sample_raw_max_us == 8u);
    CHECK(got.fast_calls == 1u && got.fast_completed == 1u);
    CHECK(got.fast_samples == 1u && got.fast_sample_raw_total_us == 5u);
    CHECK(got.inflate_fast_residual_samples == 1u);
    CHECK(got.inflate_fast_residual_raw_total_us == 13u &&
          got.inflate_fast_residual_raw_max_us == 13u);
    CHECK(got.timed_regions_started == 6u &&
          got.timed_regions_completed == 6u);
    CHECK(got.profile_clock_calls == 12u);
    CHECK(got.calibration_clock_calls == 65u &&
          got.calibration_intervals == 64u);
    CHECK(got.calibration_raw_total_us == 64u &&
          got.calibration_raw_max_us == 1u);
    CHECK(s_clock_calls == got.profile_clock_calls +
          got.calibration_clock_calls);
    CHECK(got.bad == 0u && s_log_calls == 6u);
    CHECK(strstr(s_logs, "pngprof2.a ") != NULL);
    CHECK(strstr(s_logs, "pngprof2.c ") != NULL);
    CHECK(strstr(s_logs, "pngprof2.e ") != NULL);
    CHECK(strstr(s_logs, "pngprof2.f ") != NULL);
    return 0;
}

static int unsampled_block_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;
    uint32_t image;

    reset();
    kage_vita_png_profile_image_begin();
    kage_vita_png_profile_image_end();
    CHECK(s_clock_calls == 67u);
    for (image = 1u; image < KAGE_VITA_PNG_IMAGE_SAMPLE_STRIDE; ++image) {
        uint32_t before = s_clock_calls;

        kage_vita_png_profile_image_begin();
        kage_vita_png_profile_row_begin(8u, 8u, 0u);
        kage_vita_png_profile_row_end();
        kage_vita_png_profile_inflate_begin(
            KAGE_VITA_PNG_INFLATE_ROW, 1u, image, image);
        kage_vita_png_profile_fast_begin();
        kage_vita_png_profile_fast_end();
        kage_vita_png_profile_inflate_end(1u, image + 1u, image + 2u);
        kage_vita_png_profile_image_end();
        CHECK(s_clock_calls == before);
    }
    got = snapshot();
    CHECK(got.images_completed == 32u && got.image_samples == 1u);
    CHECK(got.rows_completed == 31u && got.row_samples == 0u);
    CHECK(got.inflate_calls[0] == 31u && got.inflate_samples == 0u);
    CHECK(got.fast_calls == 31u && got.fast_samples == 0u);
    CHECK(got.profile_clock_calls == 2u);
    CHECK(got.bad == 0u);
    return 0;
}

static int outside_and_uncaptured_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;

    reset();
    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_ANCILLARY, 0u, 0u, 0u);
    kage_vita_png_profile_fast_begin();
    kage_vita_png_profile_fast_end();
    kage_vita_png_profile_inflate_end(0u, 0u, 0u);
    /* The generated non-wrapping gate also maps a nonzero z_stream near
     * UINT32_MAX to capture_valid=0.  Sentinel totals prove the runtime does
     * not infer a delta from data that the generated seam did not read. */
    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_ROW, 0u, UINT32_MAX, UINT32_MAX);
    kage_vita_png_profile_inflate_end(0u, 1u, 2u);
    kage_vita_png_profile_fast_begin();
    kage_vita_png_profile_fast_end();
    got = snapshot();
    CHECK(got.inflate_outside_image == 2u);
    CHECK(got.inflate_uncaptured_streams == 2u);
    CHECK(got.inflate_total_in == 0u && got.inflate_total_out == 0u);
    CHECK(got.fast_outside_image == 2u);
    CHECK(got.fast_without_inflate == 1u);
    CHECK(got.profile_clock_calls == 0u && s_clock_calls == 0u);
    CHECK(got.bad >= 1u);
    return 0;
}

static int longjmp_cleanup_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;

    reset();
    kage_vita_png_profile_image_begin();
    kage_vita_png_profile_row_begin(8u, 8u, 0u);
    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_ROW, 1u, 0u, 0u);
    kage_vita_png_profile_fast_begin();
    s_now += 9u;
    kage_vita_png_profile_image_end(); /* local libpng longjmp cleanup */
    got = snapshot();
    CHECK(got.abandoned_rows == 1u);
    CHECK(got.abandoned_inflates == 1u);
    CHECK(got.abandoned_fast == 1u);
    CHECK(got.row_depth == 0u && got.inflate_depth == 0u &&
          got.fast_depth == 0u && got.image_depth == 0u);
    CHECK(got.timed_regions_started == 4u);
    CHECK(got.timed_regions_completed == 1u);
    CHECK(got.profile_clock_calls == 5u);
    CHECK(got.profile_clock_calls == got.timed_regions_started +
          got.timed_regions_completed);
    CHECK(s_clock_calls == 70u);
    CHECK(got.inflate_fast_residual_samples == 0u);
    CHECK(got.bad >= 3u && s_log_calls == 6u);
    return 0;
}

static int wrap_backwards_and_saturation_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;

    reset();
    kage_vita_png_profile_inflate_begin(
        KAGE_VITA_PNG_INFLATE_ROW, 1u, UINT32_MAX - 2u, UINT32_MAX);
    kage_vita_png_profile_inflate_end(1u, 1u, 0u);
    got = snapshot();
    CHECK(got.inflate_total_wraps == 2u);
    CHECK(got.inflate_total_in == 4u && got.inflate_total_out == 1u);

    reset();
    kage_vita_png_profile_image_begin();
    kage_vita_png_profile_row_begin(UINT32_MAX, 8u, 0u);
    s_now = 900u;
    kage_vita_png_profile_row_end();
    kage_vita_png_profile_row_begin(1u, 8u, 0u);
    kage_vita_png_profile_row_end();
    s_now = 800u;
    kage_vita_png_profile_image_end();
    got = snapshot();
    CHECK(got.clock_backwards == 2u);
    CHECK(got.row_total_bytes == UINT32_MAX);
    CHECK(got.image_total_bytes == UINT32_MAX);
    CHECK(got.row_max_bytes == UINT32_MAX);
    CHECK(got.bad >= 4u);
    return 0;
}

static int complete_record_oracle(void)
{
    const char *line;
    const char *newline;
    uint32_t lines = 0u;
    uint32_t index;

    reset();
    kage_vita_png_profile_oracle_emit_max_records();
    line = s_logs;
    while ((newline = strchr(line, '\n')) != NULL) {
        CHECK((size_t)(newline - line + 1) < 384u);
        ++lines;
        line = newline + 1;
    }
    CHECK(lines == 6u && s_log_calls == 6u);
    CHECK(s_log_embedded_newlines == 0u);
    for (index = 0u; index < s_log_calls; ++index)
        CHECK(s_log_lengths[index] < 384u);
    CHECK(*line == '\0');
    CHECK(strstr(s_logs, "4294967295") != NULL);
    return 0;
}

static int native_result_oracle(void)
{
    KageVitaPngDecodeProfileSnapshot got;
    uint32_t outcome;

    reset();
    kage_vita_png_profile_native_result(KAGE_VITA_PNG_NATIVE_HANDLED);
    for (outcome = KAGE_VITA_PNG_NATIVE_REJECT_STACK;
         outcome < KAGE_VITA_PNG_NATIVE_OUTCOME_COUNT; ++outcome)
        kage_vita_png_profile_native_result(outcome);
    got = snapshot();
    CHECK(got.native_attempts == KAGE_VITA_PNG_NATIVE_OUTCOME_COUNT);
    CHECK(got.native_handled == 1u && got.native_fallbacks == 8u);
    for (outcome = 0u; outcome < 8u; ++outcome)
        CHECK(got.native_rejects[outcome] == 1u);
    CHECK(got.native_invalid_outcomes == 0u);
    CHECK(got.bad == 0u && s_clock_calls == 0u && s_log_calls == 0u);

    kage_vita_png_profile_native_result(UINT32_MAX);
    got = snapshot();
    CHECK(got.native_attempts == KAGE_VITA_PNG_NATIVE_OUTCOME_COUNT + 1u);
    CHECK(got.native_fallbacks == 9u);
    CHECK(got.native_invalid_outcomes == 1u && got.bad == 1u);
    return 0;
}

int main(void)
{
    CHECK(cold_cohort_oracle() == 0);
    CHECK(aligned_nested_oracle() == 0);
    CHECK(unsampled_block_oracle() == 0);
    CHECK(outside_and_uncaptured_oracle() == 0);
    CHECK(longjmp_cleanup_oracle() == 0);
    CHECK(wrap_backwards_and_saturation_oracle() == 0);
    CHECK(native_result_oracle() == 0);
    CHECK(complete_record_oracle() == 0);
    puts("Vita PNG decode aligned cohort profiler oracle: PASS");
    return 0;
}
#else
static IsaacVitaPngOuterSnapshot outer_snapshot(void)
{
    IsaacVitaPngOuterSnapshot value;
    memset(&value, 0xa5, sizeof value);
    isaac_vita_png_outer_snapshot(&value);
    return value;
}

static void outer_reset(void)
{
    isaac_vita_png_outer_oracle_reset();
#if defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    kage_vita_png_profile_reset();
#endif
    s_now = 1000u;
    s_clock_step = 0u;
    s_clock_calls = s_log_calls = 0u;
    s_clock_errno = 0;
    s_log_size = 0u;
    memset(s_logs, 0, sizeof s_logs);
}

static int outer_hook_modes(void)
{
    IsaacVitaPngOuterSnapshot got;
    uint32_t i;
    outer_reset();
    for (i = 0u; i < 64u; ++i) {
        kage_vita_png_profile_image_begin();
        s_now += 3u;
        kage_vita_png_profile_image_end();
    }
    got = outer_snapshot();
    CHECK(got.started == 64u && got.completed == 64u && got.timed == 64u);
    CHECK(got.total_us == 192u && got.max_us == 3u);
    CHECK(got.depth == 0u && got.nested == 0u && !got.bad_clock &&
          !got.bad_sequence && !got.saturated);
#if defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    {
        KageVitaPngDecodeProfileSnapshot legacy;
        CHECK(kage_vita_png_profile_snapshot_get(&legacy));
        CHECK(legacy.image_samples == 2u &&
              legacy.image_sample_raw_total_us == 6u);
        CHECK(legacy.profile_clock_calls == 4u &&
              legacy.calibration_clock_calls == 65u && !legacy.bad);
        CHECK(s_clock_calls == 128u + 65u); /* shared, not 128 + 69 */
        CHECK(s_log_calls != 0u); /* only the explicitly enabled old mode */
    }
#else
    CHECK(s_clock_calls == 128u && s_log_calls == 0u);
#endif
    outer_reset();
    kage_vita_png_profile_image_begin();
    s_now += 1u;
    kage_vita_png_profile_image_begin();
    s_now += 2u;
    kage_vita_png_profile_image_end();
    s_now += 7u;
    kage_vita_png_profile_image_end();
    got = outer_snapshot();
    CHECK(got.started == 1u && got.completed == 1u && got.timed == 1u &&
          got.nested == 1u && got.depth == 0u && got.total_us == 10u);
#if defined(ISAAC_VITA_PNG_DECODE_PROFILE)
    CHECK(s_clock_calls == 67u); /* old mode still rejects nested cohort */
    outer_reset();
    kage_vita_png_profile_image_begin();
    kage_vita_png_profile_row_begin(16u, 32u, 4u);
    s_now += 20u;
    /* ImagePng's error return skips a row-end hook but not the outer end. */
    kage_vita_png_profile_image_end();
    got = outer_snapshot();
    CHECK(got.completed == 1u && got.timed == 1u && got.total_us == 20u &&
          got.depth == 0u && !got.bad_sequence && s_clock_calls == 68u);
#else
    CHECK(s_clock_calls == 2u && s_log_calls == 0u);
#endif
    return 0;
}

static int outer_scope_edges(void)
{
    IsaacVitaPngOuterSnapshot got, before;
    jmp_buf png_error;
    outer_reset();
    s_clock_errno = 1;
    errno = EDOM;
    isaac_vita_png_outer_begin();
    CHECK(errno == EDOM && s_clock_calls == 1u);
    s_now += 2u;
    isaac_vita_png_outer_begin();
    s_now += 3u;
    before = outer_snapshot();
    CHECK(before.depth == 2u && before.nested == 1u && before.total_us == 0u);
    isaac_vita_png_outer_end();
    s_now += 5u;
    isaac_vita_png_outer_end();
    got = outer_snapshot();
    CHECK(got.started == 1u && got.completed == 1u && got.timed == 1u);
    CHECK(got.total_us == 10u && got.max_us == 10u && got.depth == 0u);
    CHECK(s_clock_calls == 2u && errno == EDOM);
    isaac_vita_png_outer_snapshot(NULL);
    CHECK(s_clock_calls == 2u && s_log_calls == 0u);
    isaac_vita_png_outer_end();
    CHECK(outer_snapshot().bad_sequence == 1u && s_clock_calls == 2u);

    /* The guest's png setjmp returns inside ImagePng; its caller's outer end
     * still executes. Exercise the same closure, not a per-row finally hook. */
    isaac_vita_png_outer_begin();
    if (setjmp(png_error) == 0) {
        s_now += 19u;
        longjmp(png_error, 1);
    }
    isaac_vita_png_outer_end();
    got = outer_snapshot();
    CHECK(got.completed == 2u && got.timed == 2u && got.total_us == 29u &&
          got.max_us == 19u && got.depth == 0u && s_clock_calls == 4u);

    isaac_vita_png_outer_begin();
    s_now -= 1u;
    isaac_vita_png_outer_end();
    got = outer_snapshot();
    CHECK(got.completed == 3u && got.timed == 2u && got.bad_clock == 1u &&
          got.total_us == 29u && got.depth == 0u && s_clock_calls == 6u);
    isaac_vita_png_outer_begin();
    isaac_vita_png_outer_end();
    got = outer_snapshot();
    CHECK(got.completed == 4u && got.timed == 3u && got.total_us == 29u);
    return 0;
}

static int outer_saturation(void)
{
    IsaacVitaPngOuterSnapshot seed = {0}, got;
    outer_reset();
    seed.started = seed.completed = seed.timed = UINT32_MAX;
    seed.total_us = UINT64_MAX - 1u;
    isaac_vita_png_outer_oracle_seed(&seed);
    isaac_vita_png_outer_begin();
    s_now += 2u;
    isaac_vita_png_outer_end();
    got = outer_snapshot();
    CHECK(got.started == UINT32_MAX && got.completed == UINT32_MAX &&
          got.timed == UINT32_MAX && got.total_us == UINT64_MAX &&
          got.saturated == 1u && got.depth == 0u);
    outer_reset();
    memset(&seed, 0, sizeof seed);
    seed.depth = UINT32_MAX;
    isaac_vita_png_outer_oracle_seed(&seed);
    isaac_vita_png_outer_begin();
    isaac_vita_png_outer_end();
    got = outer_snapshot();
    CHECK(got.depth == UINT32_MAX && got.nested == 1u &&
          got.bad_sequence == 1u && got.saturated == 1u);
    CHECK(got.completed == 0u && s_clock_calls == 0u);
    return 0;
}

int main(void)
{
    CHECK(ISAAC_VITA_PNG_OUTER_PROFILE_ABI == 1u);
    CHECK(outer_hook_modes() == 0);
    CHECK(outer_scope_edges() == 0);
    CHECK(outer_saturation() == 0);
    puts("Vita PNG every-image outer attribution oracle: PASS");
    return 0;
}
#endif
