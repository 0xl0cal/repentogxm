/* Bounded raw-SceGxm return-code probe for the first post-loading frames.
 *
 * The five wrappers below are selected only by diagnostic GNU ld --wrap
 * options.  They always call the real import exactly once, then append its
 * int32 result to fixed storage.  Reporting is invoked only by the native
 * owner-thread frame seam; no wrapper logs, allocates, waits or calls another
 * GXM API.
 */
#include "kage_vita_raw_gxm_probe.h"

#include <stddef.h>
#include <stdint.h>

#include <psp2/gxm.h>

#ifndef ISAAC_VITA_RAW_GXM_BUILD_ID
# error ISAAC_VITA_RAW_GXM_BUILD_ID must identify the diagnostic binary
#endif

#define KAGE_VITA_RAW_GXM_SURFACE_CAPACITY 4u
#define KAGE_VITA_RAW_GXM_FNV_OFFSET 2166136261u
#define KAGE_VITA_RAW_GXM_FNV_PRIME 16777619u

_Static_assert(KAGE_VITA_RAW_GXM_FRAME_LIMIT == 4u,
               "raw GXM physical diagnostic must remain four frames");
_Static_assert(KAGE_VITA_RAW_GXM_RUN_CAPACITY == 8u,
               "raw GXM result trace log format assumes eight runs");
_Static_assert(sizeof(int) == sizeof(int32_t),
               "raw GXM return codes must remain exact 32-bit values");

void isaac_vita_log(const char *format, ...);

int __real_sceGxmColorSurfaceInit(
    SceGxmColorSurface *surface, SceGxmColorFormat color_format,
    SceGxmColorSurfaceType surface_type,
    SceGxmColorSurfaceScaleMode scale_mode,
    SceGxmOutputRegisterSize output_register_size,
    unsigned int width, unsigned int height, unsigned int stride_in_pixels,
    void *data);
int __real_sceGxmBeginScene(
    SceGxmContext *context, unsigned int flags,
    const SceGxmRenderTarget *render_target,
    const SceGxmValidRegion *valid_region,
    SceGxmSyncObject *vertex_sync_object,
    SceGxmSyncObject *fragment_sync_object,
    const SceGxmColorSurface *color_surface,
    const SceGxmDepthStencilSurface *depth_stencil);
int __real_sceGxmDraw(
    SceGxmContext *context, SceGxmPrimitiveType primitive_type,
    SceGxmIndexFormat index_type, const void *index_data,
    unsigned int index_count);
int __real_sceGxmEndScene(
    SceGxmContext *context,
    const SceGxmNotification *vertex_notification,
    const SceGxmNotification *fragment_notification);
int __real_sceGxmDisplayQueueAddEntry(
    SceGxmSyncObject *old_buffer, SceGxmSyncObject *new_buffer,
    const void *callback_data);

typedef struct kage_vita_raw_gxm_run {
    int32_t result;
    uint32_t count;
} kage_vita_raw_gxm_run;

typedef struct kage_vita_raw_gxm_trace {
    uint32_t calls;
    uint32_t captured_calls;
    uint32_t run_count;
    uint32_t overflow_calls;
    uint32_t overflow_runs;
    uint32_t sequence_hash;
    int32_t last_result;
    int32_t first_overflow_result;
    int32_t overflow_last_result;
    kage_vita_raw_gxm_run runs[KAGE_VITA_RAW_GXM_RUN_CAPACITY];
} kage_vita_raw_gxm_trace;

typedef struct kage_vita_raw_gxm_frame {
    uint32_t present_index;
    uint32_t nested_begin_count;
    kage_vita_raw_gxm_trace begin_scene;
    kage_vita_raw_gxm_trace draw;
    kage_vita_raw_gxm_trace end_scene;
    kage_vita_raw_gxm_trace display_queue;
} kage_vita_raw_gxm_frame;

typedef struct kage_vita_raw_gxm_surface_event {
    int32_t result;
    uintptr_t surface;
    uint32_t color_format;
    uint32_t surface_type;
    uint32_t scale_mode;
    uint32_t output_register_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_in_pixels;
    uintptr_t data;
} kage_vita_raw_gxm_surface_event;

static kage_vita_raw_gxm_frame s_frame;
static uint32_t s_frame_active;
static uint32_t s_started_frames;
static kage_vita_raw_gxm_surface_event
    s_surface_events[KAGE_VITA_RAW_GXM_SURFACE_CAPACITY];
static uint32_t s_surface_calls;
static uint32_t s_surface_captured;
static uint32_t s_surface_overflow;
static uint32_t s_surface_hash = KAGE_VITA_RAW_GXM_FNV_OFFSET;
static uint32_t s_surface_reported;

static uint32_t kage_vita_raw_gxm_hash_result(
    uint32_t hash, int32_t result)
{
    uint32_t value = (uint32_t)result;
    unsigned int byte;

    for (byte = 0u; byte < 4u; ++byte) {
        hash ^= value & 0xffu;
        hash *= KAGE_VITA_RAW_GXM_FNV_PRIME;
        value >>= 8u;
    }
    return hash;
}

static void kage_vita_raw_gxm_reset_trace(kage_vita_raw_gxm_trace *trace)
{
    unsigned int index;

    trace->calls = 0u;
    trace->captured_calls = 0u;
    trace->run_count = 0u;
    trace->overflow_calls = 0u;
    trace->overflow_runs = 0u;
    trace->sequence_hash = KAGE_VITA_RAW_GXM_FNV_OFFSET;
    trace->last_result = 0;
    trace->first_overflow_result = 0;
    trace->overflow_last_result = 0;
    for (index = 0u; index < KAGE_VITA_RAW_GXM_RUN_CAPACITY; ++index) {
        trace->runs[index].result = 0;
        trace->runs[index].count = 0u;
    }
}

static void kage_vita_raw_gxm_reset_frame(uint32_t present_index)
{
    s_frame.present_index = present_index;
    s_frame.nested_begin_count = 0u;
    kage_vita_raw_gxm_reset_trace(&s_frame.begin_scene);
    kage_vita_raw_gxm_reset_trace(&s_frame.draw);
    kage_vita_raw_gxm_reset_trace(&s_frame.end_scene);
    kage_vita_raw_gxm_reset_trace(&s_frame.display_queue);
}

static void kage_vita_raw_gxm_note_result(
    kage_vita_raw_gxm_trace *trace, int32_t result)
{
    kage_vita_raw_gxm_run *run;

    ++trace->calls;
    trace->last_result = result;
    trace->sequence_hash = kage_vita_raw_gxm_hash_result(
        trace->sequence_hash, result);

    if (trace->overflow_calls) {
        ++trace->overflow_calls;
        if (result != trace->overflow_last_result)
            ++trace->overflow_runs;
        trace->overflow_last_result = result;
        return;
    }
    if (trace->run_count) {
        run = &trace->runs[trace->run_count - 1u];
        if (run->result == result) {
            ++run->count;
            ++trace->captured_calls;
            return;
        }
    }
    if (trace->run_count < KAGE_VITA_RAW_GXM_RUN_CAPACITY) {
        run = &trace->runs[trace->run_count++];
        run->result = result;
        run->count = 1u;
        ++trace->captured_calls;
        return;
    }

    trace->overflow_calls = 1u;
    trace->overflow_runs = 1u;
    trace->first_overflow_result = result;
    trace->overflow_last_result = result;
}

static void kage_vita_raw_gxm_log_trace(
    uint32_t present_index, const char *api,
    const kage_vita_raw_gxm_trace *trace)
{
    isaac_vita_log(
        "KAGE VITA RAW GXM: bid40=%.40s f=%u api=%s "
        "n=%u keep=%u ov=%u/%u hash=%08x last=%08x runs=%u:"
        "%08x*%u,%08x*%u,%08x*%u,%08x*%u,"
        "%08x*%u,%08x*%u,%08x*%u,%08x*%u of=%08x",
        ISAAC_VITA_RAW_GXM_BUILD_ID, (unsigned)present_index, api,
        (unsigned)trace->calls, (unsigned)trace->captured_calls,
        (unsigned)trace->overflow_calls, (unsigned)trace->overflow_runs,
        (unsigned)trace->sequence_hash, (unsigned)trace->last_result,
        (unsigned)trace->run_count,
        (unsigned)trace->runs[0].result, (unsigned)trace->runs[0].count,
        (unsigned)trace->runs[1].result, (unsigned)trace->runs[1].count,
        (unsigned)trace->runs[2].result, (unsigned)trace->runs[2].count,
        (unsigned)trace->runs[3].result, (unsigned)trace->runs[3].count,
        (unsigned)trace->runs[4].result, (unsigned)trace->runs[4].count,
        (unsigned)trace->runs[5].result, (unsigned)trace->runs[5].count,
        (unsigned)trace->runs[6].result, (unsigned)trace->runs[6].count,
        (unsigned)trace->runs[7].result, (unsigned)trace->runs[7].count,
        (unsigned)trace->first_overflow_result);
}

static void kage_vita_raw_gxm_report_surfaces(void)
{
    uint32_t index;

    if (s_surface_reported)
        return;
    s_surface_reported = 1u;
    isaac_vita_log(
        "KAGE VITA RAW GXM: bid40=%.40s phase=surface-summary "
        "n=%u keep=%u ov=%u hash=%08x",
        ISAAC_VITA_RAW_GXM_BUILD_ID, (unsigned)s_surface_calls,
        (unsigned)s_surface_captured, (unsigned)s_surface_overflow,
        (unsigned)s_surface_hash);
    for (index = 0u; index < s_surface_captured; ++index) {
        const kage_vita_raw_gxm_surface_event *event =
            &s_surface_events[index];

        isaac_vita_log(
            "KAGE VITA RAW GXM: bid40=%.40s phase=surface i=%u "
            "rc=%08x surf=%08x data=%08x fmt=%08x type=%08x "
            "scale=%08x out=%08x wh=%u/%u stride=%u",
            ISAAC_VITA_RAW_GXM_BUILD_ID, (unsigned)index,
            (unsigned)event->result, (unsigned)event->surface,
            (unsigned)event->data, (unsigned)event->color_format,
            (unsigned)event->surface_type, (unsigned)event->scale_mode,
            (unsigned)event->output_register_size,
            (unsigned)event->width, (unsigned)event->height,
            (unsigned)event->stride_in_pixels);
    }
}

void kage_vita_raw_gxm_probe_begin_frame(uint32_t present_index)
{
    if (s_frame_active) {
        ++s_frame.nested_begin_count;
        return;
    }
    if (s_started_frames >= KAGE_VITA_RAW_GXM_FRAME_LIMIT)
        return;
    kage_vita_raw_gxm_reset_frame(present_index);
    ++s_started_frames;
    s_frame_active = 1u;
}

void kage_vita_raw_gxm_probe_end_frame(uint32_t present_index)
{
    if (!s_frame_active)
        return;
    /* Seal only here: the first post-loading Render can create/reattach its
     * own color surface after begin_frame(), and that boundary is part of the
     * diagnostic rather than startup noise to discard. */
    kage_vita_raw_gxm_report_surfaces();
    isaac_vita_log(
        "KAGE VITA RAW GXM: bid40=%.40s phase=frame begin=%u end=%u "
        "match=%u nested=%u ordinal=%u/%u",
        ISAAC_VITA_RAW_GXM_BUILD_ID, (unsigned)s_frame.present_index,
        (unsigned)present_index,
        (unsigned)(s_frame.present_index == present_index),
        (unsigned)s_frame.nested_begin_count, (unsigned)s_started_frames,
        (unsigned)KAGE_VITA_RAW_GXM_FRAME_LIMIT);
    kage_vita_raw_gxm_log_trace(
        s_frame.present_index, "b", &s_frame.begin_scene);
    kage_vita_raw_gxm_log_trace(
        s_frame.present_index, "d", &s_frame.draw);
    kage_vita_raw_gxm_log_trace(
        s_frame.present_index, "e", &s_frame.end_scene);
    kage_vita_raw_gxm_log_trace(
        s_frame.present_index, "q", &s_frame.display_queue);
    s_frame_active = 0u;
}

int __wrap_sceGxmColorSurfaceInit(
    SceGxmColorSurface *surface, SceGxmColorFormat color_format,
    SceGxmColorSurfaceType surface_type,
    SceGxmColorSurfaceScaleMode scale_mode,
    SceGxmOutputRegisterSize output_register_size,
    unsigned int width, unsigned int height, unsigned int stride_in_pixels,
    void *data)
{
    int result = __real_sceGxmColorSurfaceInit(
        surface, color_format, surface_type, scale_mode,
        output_register_size, width, height, stride_in_pixels, data);

    if (!s_surface_reported) {
        ++s_surface_calls;
        s_surface_hash = kage_vita_raw_gxm_hash_result(
            s_surface_hash, result);
        if (s_surface_captured < KAGE_VITA_RAW_GXM_SURFACE_CAPACITY) {
            kage_vita_raw_gxm_surface_event *event =
                &s_surface_events[s_surface_captured++];
            event->result = result;
            event->surface = (uintptr_t)surface;
            event->color_format = (uint32_t)color_format;
            event->surface_type = (uint32_t)surface_type;
            event->scale_mode = (uint32_t)scale_mode;
            event->output_register_size = (uint32_t)output_register_size;
            event->width = width;
            event->height = height;
            event->stride_in_pixels = stride_in_pixels;
            event->data = (uintptr_t)data;
        } else {
            ++s_surface_overflow;
        }
    }
    return result;
}

int __wrap_sceGxmBeginScene(
    SceGxmContext *context, unsigned int flags,
    const SceGxmRenderTarget *render_target,
    const SceGxmValidRegion *valid_region,
    SceGxmSyncObject *vertex_sync_object,
    SceGxmSyncObject *fragment_sync_object,
    const SceGxmColorSurface *color_surface,
    const SceGxmDepthStencilSurface *depth_stencil)
{
    int result = __real_sceGxmBeginScene(
        context, flags, render_target, valid_region, vertex_sync_object,
        fragment_sync_object, color_surface, depth_stencil);

    if (s_frame_active)
        kage_vita_raw_gxm_note_result(&s_frame.begin_scene, result);
    return result;
}

int __wrap_sceGxmDraw(
    SceGxmContext *context, SceGxmPrimitiveType primitive_type,
    SceGxmIndexFormat index_type, const void *index_data,
    unsigned int index_count)
{
    int result = __real_sceGxmDraw(
        context, primitive_type, index_type, index_data, index_count);

    if (s_frame_active)
        kage_vita_raw_gxm_note_result(&s_frame.draw, result);
    return result;
}

int __wrap_sceGxmEndScene(
    SceGxmContext *context,
    const SceGxmNotification *vertex_notification,
    const SceGxmNotification *fragment_notification)
{
    int result = __real_sceGxmEndScene(
        context, vertex_notification, fragment_notification);

    if (s_frame_active)
        kage_vita_raw_gxm_note_result(&s_frame.end_scene, result);
    return result;
}

int __wrap_sceGxmDisplayQueueAddEntry(
    SceGxmSyncObject *old_buffer, SceGxmSyncObject *new_buffer,
    const void *callback_data)
{
    int result = __real_sceGxmDisplayQueueAddEntry(
        old_buffer, new_buffer, callback_data);

    if (s_frame_active)
        kage_vita_raw_gxm_note_result(&s_frame.display_queue, result);
    return result;
}
