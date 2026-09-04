/* Executable host oracle for the async display callback, owner-thread cadence,
 * cross-thread handoff and bounded A8B8G8R8 writes. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kage_vita_loading.h"
#include "kage_vita_stall_probe.h"
#include "kage_vita_loading_test_vitagl.h"

#ifndef ISAAC_KAGE_VITA_LOADING_PRESENTATION
#define ISAAC_KAGE_VITA_LOADING_PRESENTATION 1
#endif

#define ORACLE_WIDTH  960u
#define ORACLE_HEIGHT 544u
#define ORACLE_PIXELS (ORACLE_WIDTH * ORACLE_HEIGHT)
#define ORACLE_FREAD_TOTAL 2070715u
#define ORACLE_BORDER     0xff000000u
#define ORACLE_BACKGROUND 0xff120c12u
#define ORACLE_PANEL      0xff1e1723u
#define ORACLE_TRACK      0xff322d37u
#define ORACLE_FILL       0xffc3d2dcu
#if defined(ISAAC_KAGE_VITA_LOADING_SPECIALIST)
#define ORACLE_DANCE_SKIN 0xffc6c7e7u
#endif
#define ORACLE_ANIMATION_X 368u
#define ORACLE_ANIMATION_Y 46u
#define ORACLE_FRAME_INTERVAL_US 233854u
#define ORACLE_FREAD_GRANULARITY 512u
#define ORACLE_TRACE_DURATION_US UINT64_C(59317000)

unsigned kage_vita_loading_oracle_animation_frame(void);

static uint32_t s_guarded_framebuffer[ORACLE_PIXELS + 2u];
static void (*s_display_callback)(void *framebuffer);
static void (*s_captured_display_callback)(void *framebuffer);
static uint64_t s_time;
static int s_thread = 7;
static unsigned s_pending_swaps;
static unsigned s_swap_calls;
static unsigned s_set_callback_calls;
static unsigned s_clear_callback_calls;
static unsigned s_queue_finish_calls;
static int s_queue_finish_result;
static int s_active_during_queue_finish;
static unsigned s_stage_during_queue_finish;
static unsigned s_callback_runs;
static unsigned s_log_calls;
static unsigned s_recorded_swaps;
static unsigned s_stall_published_swaps;
static unsigned s_stall_swap_publications;
static uint32_t s_sync_order[8];
static uint32_t s_sync_completed[8];
static unsigned s_sync_order_count;
static uint64_t s_previous_swap_time;
static uint64_t s_min_swap_interval;
static uint64_t s_max_swap_interval;
static int s_record_swap_times;
static int s_inside_callback;
static int s_api_called_inside_callback;
static char s_last_log[192];
static char s_last_activity_log[192];
static unsigned s_profile_begin_calls;
static unsigned s_profile_report_calls;
static char s_last_profile_reason[32];

void kage_vita_io_profile_begin(void)
{
    ++s_profile_begin_calls;
}

void kage_vita_io_profile_report(const char *reason)
{
    ++s_profile_report_calls;
    (void)snprintf(s_last_profile_reason, sizeof s_last_profile_reason,
                   "%s", reason ? reason : "");
}

void kage_vita_stall_note_loading_swap(uint32_t completed_swaps)
{
    s_stall_published_swaps = completed_swaps;
    ++s_stall_swap_publications;
}

void kage_vita_stall_trace_sync(uint32_t edge, uint32_t completed_calls)
{
    if (s_sync_order_count < 8u) {
        s_sync_order[s_sync_order_count] = edge;
        s_sync_completed[s_sync_order_count] = completed_calls;
        ++s_sync_order_count;
    }
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Vita loading oracle failed at line %d: %s\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void oracle_note_api_call(void)
{
    if (s_inside_callback)
        s_api_called_inside_callback = 1;
}

int sceKernelGetThreadId(void)
{
    oracle_note_api_call();
    return s_thread;
}

uint64_t sceKernelGetProcessTimeWide(void)
{
    oracle_note_api_call();
    return s_time;
}

int sceClibPrintf(const char *format, ...)
{
    va_list arguments;
    int result;

    oracle_note_api_call();
    ++s_log_calls;
    va_start(arguments, format);
    result = vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    if (strstr(s_last_log, "[kage-vita-loading] activity:") != NULL)
        (void)snprintf(s_last_activity_log, sizeof s_last_activity_log,
                       "%s", s_last_log);
    return result;
}

void vglSetDisplayCallback(void (*callback)(void *framebuffer))
{
    oracle_note_api_call();
    s_display_callback = callback;
    if (callback) {
        s_captured_display_callback = callback;
        ++s_set_callback_calls;
    } else {
        ++s_clear_callback_calls;
    }
}

void vglSwapBuffers(GLboolean has_common_dialog)
{
    uint64_t interval;

    oracle_note_api_call();
    if (has_common_dialog != GL_FALSE)
        s_api_called_inside_callback = 1;
    if (s_sync_order_count < 8u) {
        s_sync_order[s_sync_order_count] = 0x100u;
        s_sync_completed[s_sync_order_count] = UINT32_MAX;
        ++s_sync_order_count;
    }
    if (s_record_swap_times) {
        if (s_recorded_swaps) {
            interval = s_time - s_previous_swap_time;
            if (interval < s_min_swap_interval)
                s_min_swap_interval = interval;
            if (interval > s_max_swap_interval)
                s_max_swap_interval = interval;
        }
        s_previous_swap_time = s_time;
        ++s_recorded_swaps;
    }
    ++s_swap_calls;
    ++s_pending_swaps;
}

static void oracle_dispatch_one(void)
{
    if (!s_pending_swaps)
        return;
    --s_pending_swaps;
    if (s_display_callback) {
        s_inside_callback = 1;
        s_display_callback(&s_guarded_framebuffer[1]);
        s_inside_callback = 0;
        ++s_callback_runs;
    }
}

int sceGxmDisplayQueueFinish(void)
{
    oracle_note_api_call();
    ++s_queue_finish_calls;
    s_active_during_queue_finish = kage_vita_loading_active();
    s_stage_during_queue_finish = kage_vita_loading_stage();
    while (s_pending_swaps)
        oracle_dispatch_one();
    return s_queue_finish_result;
}

static uint32_t oracle_pixel(unsigned x, unsigned y)
{
    return s_guarded_framebuffer[1u + y * ORACLE_WIDTH + x];
}

static void oracle_reset_frame(uint32_t value)
{
    unsigned index;
    s_guarded_framebuffer[0] = 0x13579bdfu;
    for (index = 0u; index < ORACLE_PIXELS; ++index)
        s_guarded_framebuffer[index + 1u] = value;
    s_guarded_framebuffer[ORACLE_PIXELS + 1u] = 0x2468ace0u;
}

#if ISAAC_KAGE_VITA_LOADING_PRESENTATION
int main(void)
{
    uint32_t completed;
    unsigned swap_before;
    unsigned runs_before;
    unsigned trace_swap_base;

    oracle_reset_frame(0xdeadbeefu);
    CHECK(!kage_vita_loading_active());
    kage_vita_loading_start();
    CHECK(kage_vita_loading_active());
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_BOOT);
    CHECK(kage_vita_loading_elapsed_seconds() == 0u);
    CHECK(kage_vita_loading_swap_count() == 1u);
    CHECK(s_swap_calls == 1u && s_pending_swaps == 1u);
    CHECK(s_stall_published_swaps == 1u);
    CHECK(s_stall_swap_publications == 1u);
    CHECK(s_sync_order_count == 3u);
    CHECK(s_sync_order[0] == KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER);
    CHECK(s_sync_order[1] == 0x100u);
    CHECK(s_sync_order[2] == KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN);
    CHECK(s_sync_completed[0] == 0u && s_sync_completed[2] == 0u);
    CHECK(s_set_callback_calls == 1u && s_clear_callback_calls == 0u);
    CHECK(s_log_calls == 1u &&
          strstr(s_last_log, "activity=indeterminate") != NULL &&
          strstr(s_last_log, "denominator") == NULL &&
          strstr(s_last_log, "%") == NULL);
    CHECK(s_profile_begin_calls == 1u && s_profile_report_calls == 0u);
    kage_vita_loading_start();
    CHECK(s_swap_calls == 1u && s_set_callback_calls == 1u);
    CHECK(s_stall_swap_publications == 1u);
    CHECK(s_log_calls == 1u);

    kage_vita_loading_note_verify();
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_VERIFY);
    CHECK(s_swap_calls == 1u);

    oracle_dispatch_one();
    CHECK(s_callback_runs == 1u && s_pending_swaps == 0u);
    CHECK(!s_api_called_inside_callback);
    CHECK(s_guarded_framebuffer[0] == 0x13579bdfu);
    CHECK(s_guarded_framebuffer[ORACLE_PIXELS + 1u] == 0x2468ace0u);
    CHECK(oracle_pixel(0u, 0u) == ORACLE_BORDER);
    CHECK(oracle_pixel(959u, 1u) == ORACLE_BORDER);
    CHECK(oracle_pixel(0u, 542u) == ORACLE_BORDER);
    CHECK(oracle_pixel(959u, 543u) == ORACLE_BORDER);
    CHECK(oracle_pixel(0u, 100u) == ORACLE_BACKGROUND);
    CHECK(oracle_pixel(160u, 354u) == ORACLE_TRACK);
    CHECK(oracle_pixel(164u, 358u) == ORACLE_FILL);
    CHECK(oracle_pixel(283u, 358u) == ORACLE_FILL);
    CHECK(oracle_pixel(284u, 358u) == ORACLE_TRACK);
#if defined(ISAAC_KAGE_VITA_LOADING_SPECIALIST)
    /* Optional local mode: frame zero retains the pinned RGB565/RLE proof. */
    CHECK(oracle_pixel(ORACLE_ANIMATION_X + 10u * 4u,
                       ORACLE_ANIMATION_Y + 10u * 4u) == ORACLE_PANEL);
    CHECK(oracle_pixel(ORACLE_ANIMATION_X + 28u * 4u,
                       ORACLE_ANIMATION_Y + 20u * 4u) ==
          ORACLE_DANCE_SKIN);
#else
    /* Clean-clone mode: frame zero is the source-controlled procedural ring.
     * It leaves the unused origin as panel, highlights dot zero, and renders
     * the opposite dot with the track color. */
    CHECK(oracle_pixel(ORACLE_ANIMATION_X, ORACLE_ANIMATION_Y) ==
          ORACLE_PANEL);
    CHECK(oracle_pixel(ORACLE_ANIMATION_X + 26u * 4u,
                       ORACLE_ANIMATION_Y + 7u * 4u) == ORACLE_FILL);
    CHECK(oracle_pixel(ORACLE_ANIMATION_X + 26u * 4u,
                       ORACLE_ANIMATION_Y + 33u * 4u) == ORACLE_TRACK);
#endif

    /* Activity publication and animation presentation are independent.  An
     * archive-stage change before the first cadence deadline updates the
     * callback snapshot but does not queue an early swap. */
    s_time = 50000u;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_elapsed_seconds() == 0u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 0u);
    CHECK(s_swap_calls == 1u);

    /* Read-count magnitude has no UI meaning and cannot cause a present. */
    s_time = 60000u;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 4u);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_oracle_animation_frame() == 0u);
    CHECK(s_swap_calls == 1u);

    s_time = ORACLE_FRAME_INTERVAL_US;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 1u);
    CHECK(kage_vita_loading_swap_count() == 2u && s_swap_calls == 2u);
    CHECK(s_sync_order_count == 6u);
    CHECK(s_sync_order[3] == KAGE_VITA_STALL_SYNC_LOADING_SWAP_ENTER);
    CHECK(s_sync_order[4] == 0x100u);
    CHECK(s_sync_order[5] == KAGE_VITA_STALL_SYNC_LOADING_SWAP_RETURN);
    CHECK(s_sync_completed[3] == ORACLE_FREAD_TOTAL / 2u &&
          s_sync_completed[5] == ORACLE_FREAD_TOTAL / 2u);
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_swap_count() == 2u && s_swap_calls == 2u);
    oracle_reset_frame(0xdeadbeefu);
    oracle_dispatch_one();
    CHECK(oracle_pixel(197u, 358u) == ORACLE_TRACK);
    CHECK(oracle_pixel(198u, 358u) == ORACLE_FILL);
    CHECK(oracle_pixel(317u, 358u) == ORACLE_FILL);
    CHECK(oracle_pixel(318u, 358u) == ORACLE_TRACK);
#if !defined(ISAAC_KAGE_VITA_LOADING_SPECIALIST)
    /* Frame one is a distinct visible spinner phase, not one of the legacy
     * repeated phases. */
    CHECK(oracle_pixel(ORACLE_ANIMATION_X + 35u * 4u,
                       ORACLE_ANIMATION_Y + 11u * 4u) == ORACLE_FILL);
    CHECK(oracle_pixel(ORACLE_ANIMATION_X + 26u * 4u,
                       ORACLE_ANIMATION_Y + 7u * 4u) == ORACLE_TRACK);
#endif
    CHECK(s_guarded_framebuffer[0] == 0x13579bdfu &&
          s_guarded_framebuffer[ORACLE_PIXELS + 1u] == 0x2468ace0u);

    /* A delayed poll advances three logical frames but queues only one swap;
     * this prevents a load stall from causing a burst of GL presents. */
    swap_before = s_swap_calls;
    s_time = 4u * ORACLE_FRAME_INTERVAL_US;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 4u);
    CHECK(s_swap_calls == swap_before + 1u && s_pending_swaps == 1u);
    oracle_dispatch_one();

    /* A worker can report fread traffic, but only the context owner swaps. */
    swap_before = s_swap_calls;
    s_thread = 99;
    s_time = 10u * ORACLE_FRAME_INTERVAL_US;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_oracle_animation_frame() == 4u);
    CHECK(s_swap_calls == swap_before);

    s_thread = 7;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_elapsed_seconds() == 2u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 10u);
    CHECK(s_swap_calls == swap_before + 1u && s_pending_swaps == 1u);

    /* Shader compilation gets one immediate same-context presentation so a
     * long compiler call cannot leave the archive label frozen on-screen. */
    kage_vita_loading_note_shader();
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_SHADERS);
    CHECK(kage_vita_loading_swap_count() == 5u);
    CHECK(s_pending_swaps == 2u);

    /* Finish removes the callback before draining.  A queued frame therefore
     * cannot overwrite a genuine game buffer after the handoff. */
    oracle_reset_frame(0xa5a5a5a5u);
    runs_before = s_callback_runs;
    /* A defensive finish from another thread must still disable/drain the
     * callback, and its owner mismatch must be visible in the boot log. */
    s_thread = 99;
    s_queue_finish_result = (int32_t)0x805b0003u;
    kage_vita_loading_finish();
    CHECK(!kage_vita_loading_active());
    CHECK(s_active_during_queue_finish);
    CHECK(s_stage_during_queue_finish == KAGE_VITA_LOADING_GAME);
    CHECK(s_clear_callback_calls == 1u && s_queue_finish_calls == 1u);
    CHECK(s_pending_swaps == 0u && s_callback_runs == runs_before);
    CHECK(oracle_pixel(480u, 272u) == 0xa5a5a5a5u);
    CHECK(s_log_calls == 3u && strstr(s_last_log, "stage=game") != NULL);
    CHECK(strstr(s_last_log, "previous_stage=4") != NULL);
    CHECK(strstr(s_last_log, "elapsed_s=2") != NULL);
    CHECK(strstr(s_last_log, "loading_swaps=5") != NULL);
    CHECK(strstr(s_last_activity_log, "verify_notes=1") != NULL);
    CHECK(strstr(s_last_activity_log, "archive_notes=") != NULL);
    CHECK(strstr(s_last_activity_log, "shader_notes=1") != NULL);
    CHECK(strstr(s_last_log, "%") == NULL);
    CHECK(strstr(s_last_log, "owner_match=0") != NULL);
    CHECK(strstr(s_last_log, "foreign_notes=1") != NULL);
    CHECK(strstr(s_last_log, "queue_finish=0x805b0003") != NULL);
    CHECK(s_profile_begin_calls == 1u && s_profile_report_calls == 1u);
    CHECK(strcmp(s_last_profile_reason, "loading-complete") == 0);

    /* Model the narrow race where vitaGL's display thread captured the old
     * callback pointer just before vglSetDisplayCallback(NULL).  The inactive
     * acquire snapshot must make that stale invocation a no-op. */
    CHECK(s_captured_display_callback != NULL);
    s_inside_callback = 1;
    s_captured_display_callback(&s_guarded_framebuffer[1]);
    s_inside_callback = 0;
    CHECK(oracle_pixel(480u, 272u) == 0xa5a5a5a5u);
    CHECK(s_guarded_framebuffer[0] == 0x13579bdfu &&
          s_guarded_framebuffer[ORACLE_PIXELS + 1u] == 0x2468ace0u);

    swap_before = s_swap_calls;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL);
    kage_vita_loading_finish();
    CHECK(s_swap_calls == swap_before && s_queue_finish_calls == 1u);
    CHECK(!s_api_called_inside_callback);

    /* Uniformly replay the accepted 59.317 s / 2,070,715-fread evidence at
     * the exact production 512-call polling boundary.  The initial swap plus
     * 253 anchored deadlines is 254 swaps; 253 frame advances end at 29/32. */
    s_thread = 7;
    s_time = 0u;
    trace_swap_base = s_swap_calls;
    s_recorded_swaps = 0u;
    s_min_swap_interval = UINT64_MAX;
    s_max_swap_interval = 0u;
    s_record_swap_times = 1;
    kage_vita_loading_start();
    for (completed = ORACLE_FREAD_GRANULARITY;
         completed <= ORACLE_FREAD_TOTAL;
         completed += ORACLE_FREAD_GRANULARITY) {
        s_time = (uint64_t)completed * ORACLE_TRACE_DURATION_US /
            ORACLE_FREAD_TOTAL;
        kage_vita_loading_note_fread(completed);
    }
    CHECK(ORACLE_FREAD_TOTAL / ORACLE_FREAD_GRANULARITY == 4044u);
    CHECK(kage_vita_loading_swap_count() == 254u);
    CHECK(s_stall_published_swaps == 254u);
    CHECK(s_swap_calls - trace_swap_base == 254u);
    CHECK(s_recorded_swaps == 254u);
    CHECK(s_min_swap_interval == 219998u);
    CHECK(s_max_swap_interval == 234666u);
    CHECK(s_pending_swaps == 254u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 29u);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_elapsed_seconds() == 59u);
    runs_before = s_callback_runs;
    s_queue_finish_result = 0;
    kage_vita_loading_finish();
    CHECK(!kage_vita_loading_active());
    CHECK(s_pending_swaps == 0u && s_callback_runs == runs_before);
    CHECK(s_clear_callback_calls == 2u && s_queue_finish_calls == 2u);
    CHECK(strstr(s_last_log, "previous_stage=3") != NULL);
    CHECK(strstr(s_last_log, "elapsed_s=59") != NULL);
    CHECK(strstr(s_last_activity_log, "verify_notes=0") != NULL);
    CHECK(strstr(s_last_activity_log, "archive_notes=") != NULL);
    CHECK(strstr(s_last_activity_log, "shader_notes=0") != NULL);
    CHECK(strstr(s_last_log, "%") == NULL);
    CHECK(strstr(s_last_log, "loading_swaps=254") != NULL);
    CHECK(strstr(s_last_log, "owner_match=1") != NULL);
    CHECK(strstr(s_last_log, "queue_finish=0x00000000") != NULL);
    CHECK(s_profile_begin_calls == 2u && s_profile_report_calls == 2u);
    CHECK(strcmp(s_last_profile_reason, "loading-complete") == 0);
    s_record_swap_times = 0;

    puts("Vita loading recorded trace: duration=59.317s activity=fread "
         "polls=4044 swaps=254 frame_advances=253 final_frame=29 "
         "target_cadence_hz=4.276169 interval_us=219998..234666");
    puts("Vita loading async-callback/cadence/handoff/redzone oracle: PASS");
    return 0;
}
#else
int main(void)
{
    uint32_t completed;
    unsigned index;
    unsigned publications_before;

    oracle_reset_frame(0xdeadbeefu);
    CHECK(!kage_vita_loading_active());
    kage_vita_loading_start();
    CHECK(kage_vita_loading_active());
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_BOOT);
    CHECK(kage_vita_loading_elapsed_seconds() == 0u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 0u);
    CHECK(kage_vita_loading_swap_count() == 0u);
    CHECK(s_swap_calls == 0u && s_pending_swaps == 0u);
    CHECK(s_set_callback_calls == 0u && s_clear_callback_calls == 0u);
    CHECK(s_queue_finish_calls == 0u && s_callback_runs == 0u);
    CHECK(s_display_callback == NULL && s_captured_display_callback == NULL);
    CHECK(s_stall_published_swaps == 0u);
    CHECK(s_stall_swap_publications == 1u && s_sync_order_count == 0u);
    CHECK(s_profile_begin_calls == 1u && s_profile_report_calls == 0u);
    CHECK(s_log_calls == 1u &&
          strstr(s_last_log, "activity=indeterminate") != NULL &&
          strstr(s_last_log, "denominator") == NULL &&
          strstr(s_last_log, "%") == NULL);

    /* Re-entering the active lifecycle remains idempotent while presentation
     * is disabled; in particular it cannot install a callback later. */
    kage_vita_loading_start();
    CHECK(s_profile_begin_calls == 1u && s_log_calls == 1u);
    CHECK(s_stall_swap_publications == 1u);

    kage_vita_loading_note_verify();
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_VERIFY);

    /* Preserve truthful archive activity and the cooperative clock, but never
     * turn either an initial or an elapsed deadline into a display action. */
    s_time = 50000u;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_oracle_animation_frame() == 0u);
    s_time = ORACLE_FRAME_INTERVAL_US;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 1u);
    s_time = 4u * ORACLE_FRAME_INTERVAL_US;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL / 2u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 4u);
    CHECK(kage_vita_loading_swap_count() == 0u && s_swap_calls == 0u);
    CHECK(s_set_callback_calls == 0u && s_queue_finish_calls == 0u);
    CHECK(s_sync_order_count == 0u);

    /* Foreign fread notes retain their accounting/owner rule without gaining
     * permission to touch the display. */
    publications_before = s_stall_swap_publications;
    s_thread = 99;
    s_time = 10u * ORACLE_FRAME_INTERVAL_US;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_oracle_animation_frame() == 4u);
    CHECK(s_stall_swap_publications == publications_before);
    s_thread = 7;
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL);
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_elapsed_seconds() == 2u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 10u);
    CHECK(s_stall_published_swaps == 0u);
    kage_vita_loading_note_shader();
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_SHADERS);
    CHECK(kage_vita_loading_swap_count() == 0u);

    s_thread = 99;
    s_queue_finish_result = (int32_t)0x805b0003u;
    kage_vita_loading_finish();
    CHECK(!kage_vita_loading_active());
    CHECK(kage_vita_loading_swap_count() == 0u && s_swap_calls == 0u);
    CHECK(s_set_callback_calls == 0u && s_clear_callback_calls == 0u);
    CHECK(s_queue_finish_calls == 0u && s_pending_swaps == 0u);
    CHECK(s_callback_runs == 0u && s_sync_order_count == 0u);
    CHECK(s_profile_begin_calls == 1u && s_profile_report_calls == 1u);
    CHECK(strcmp(s_last_profile_reason, "loading-complete") == 0);
    CHECK(s_log_calls == 3u && strstr(s_last_log, "stage=game") != NULL);
    CHECK(strstr(s_last_log, "previous_stage=4") != NULL);
    CHECK(strstr(s_last_log, "elapsed_s=2") != NULL);
    CHECK(strstr(s_last_activity_log, "verify_notes=1") != NULL);
    CHECK(strstr(s_last_activity_log, "archive_notes=") != NULL);
    CHECK(strstr(s_last_activity_log, "shader_notes=1") != NULL);
    CHECK(strstr(s_last_log, "%") == NULL);
    CHECK(strstr(s_last_log, "loading_swaps=0") != NULL);
    CHECK(strstr(s_last_log, "owner_match=0") != NULL);
    CHECK(strstr(s_last_log, "foreign_notes=1") != NULL);
    CHECK(strstr(s_last_log, "queue_finish=skipped") != NULL);

    /* With no callback installed and no CPU presenter compiled, every pixel
     * and both redzones stay byte-for-byte untouched. */
    CHECK(s_guarded_framebuffer[0] == 0x13579bdfu);
    for (index = 0u; index < ORACLE_PIXELS; ++index)
        CHECK(s_guarded_framebuffer[index + 1u] == 0xdeadbeefu);
    CHECK(s_guarded_framebuffer[ORACLE_PIXELS + 1u] == 0x2468ace0u);
    CHECK(oracle_pixel(480u, 272u) == 0xdeadbeefu);

    /* Inactive calls remain no-ops, including the profile finish hook. */
    kage_vita_loading_note_fread(ORACLE_FREAD_TOTAL);
    kage_vita_loading_finish();
    CHECK(s_profile_report_calls == 1u && s_log_calls == 3u);

    /* Replay the measured load to cover every periodic presentation point. */
    s_thread = 7;
    s_time = 0u;
    kage_vita_loading_start();
    for (completed = ORACLE_FREAD_GRANULARITY;
         completed <= ORACLE_FREAD_TOTAL;
         completed += ORACLE_FREAD_GRANULARITY) {
        s_time = (uint64_t)completed * ORACLE_TRACE_DURATION_US /
            ORACLE_FREAD_TOTAL;
        kage_vita_loading_note_fread(completed);
    }
    CHECK(kage_vita_loading_stage() == KAGE_VITA_LOADING_ARCHIVES);
    CHECK(kage_vita_loading_elapsed_seconds() == 59u);
    CHECK(kage_vita_loading_oracle_animation_frame() == 29u);
    CHECK(kage_vita_loading_swap_count() == 0u && s_swap_calls == 0u);
    CHECK(s_set_callback_calls == 0u && s_queue_finish_calls == 0u);
    kage_vita_loading_finish();
    CHECK(!kage_vita_loading_active());
    CHECK(s_profile_begin_calls == 2u && s_profile_report_calls == 2u);
    CHECK(strstr(s_last_log, "previous_stage=3") != NULL);
    CHECK(strstr(s_last_log, "elapsed_s=59") != NULL);
    CHECK(strstr(s_last_activity_log, "verify_notes=0") != NULL);
    CHECK(strstr(s_last_activity_log, "archive_notes=") != NULL);
    CHECK(strstr(s_last_activity_log, "shader_notes=0") != NULL);
    CHECK(strstr(s_last_log, "%") == NULL);
    CHECK(strstr(s_last_log, "loading_swaps=0") != NULL);
    CHECK(strstr(s_last_log, "queue_finish=skipped") != NULL);
    CHECK(s_stall_published_swaps == 0u && s_sync_order_count == 0u);

    puts("Vita loading presentation-OFF lifecycle/fread/stall/profile/log "
         "oracle: PASS (callback=0 swaps=0 queue_finish=0 writes=0)");
    return 0;
}
#endif
