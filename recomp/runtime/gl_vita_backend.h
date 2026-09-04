#ifndef GL_VITA_BACKEND_H
#define GL_VITA_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE) && \
    !defined(ISAAC_VITA_PHASE_PROFILE)
#error "texture-churn profile requires the aggregate phase profile"
#endif

/* Driver-facing calls made by the translated game during one aggregate
 * profile window.  The counters live at the typed GL boundary, so internal
 * setup/diagnostic GL calls are not misattributed to game rendering.  The
 * suppressed counters are requests deliberately stopped at that boundary;
 * add them to the matching driver-facing count to recover guest requests. */
typedef struct IsaacVitaGlPhaseProfileCounters {
    uint32_t draw_elements;
    uint32_t clear;
    uint32_t bind_texture;
    uint32_t use_program;
    uint32_t tex_image;
    uint32_t tex_sub_image;
    uint32_t uniform;
    uint32_t attrib_pointer;
    uint32_t attrib_toggle;
    uint32_t bind_framebuffer;
    uint32_t state;
    uint32_t use_program_suppressed;
    uint32_t uniform_suppressed;
    uint32_t typed_state_hit_active_texture;
    uint32_t typed_state_hit_bind_texture;
    uint32_t typed_state_hit_blend;
    uint32_t typed_state_hit_depth;
    uint32_t typed_state_hit_viewport;
    uint32_t typed_state_hit_attrib_toggle;
    uint32_t typed_state_hit_attrib_pointer;
    uint32_t typed_state_miss;
    uint32_t typed_state_reject_invalid;
    uint32_t typed_state_reject_unknown;
    uint32_t canonical_quad_hits;
    uint32_t canonical_quad_reject_callsite;
    uint32_t canonical_quad_reject_shape;
    uint32_t canonical_quad_reject_bounds;
    uint32_t canonical_quad_reject_pointer;
    uint32_t canonical_quad_driver_fallback;
    uint32_t canonical_quad_index_bytes_saved;
    uint32_t fusion_index_bytes;
    uint32_t fusion_blend_additive;
    uint32_t fusion_blend_alpha;
    uint32_t fusion_blend_other;
    uint32_t fusion_shader_same;
    uint32_t fusion_shader_change;
    uint32_t fusion_fbo_default;
    uint32_t fusion_fbo_offscreen;
    uint32_t fusion_atlas_same;
    uint32_t fusion_atlas_change;
    uint32_t fusion_additive_draws;
    uint32_t fusion_additive_runs;
    /* Offscreen render-target boundary (FBO_CLEAR_ELISION / FBO_RASTER_SCALE):
     * guest glClear requests absorbed at the call (colour part a no-op,
     * depth/stencil part owed), colour targets allocated at the reduced
     * raster, native viewports rewritten for a scaled draw target, the
     * permanent elision poison (guest enabled GL_SCISSOR_TEST), owed
     * depth/stencil clears issued as their own native glClear (before the
     * first draw or ahead of an unmodelled call; folds into a later native
     * clear are free and not counted), and scaled textures re-specified at
     * full size because the guest sub-imaged them. */
    uint32_t clear_suppressed;
    uint32_t fbo_raster_textures;
    uint32_t fbo_raster_viewports;
    uint32_t fbo_elision_poison;
    uint32_t clear_replayed;
    uint32_t fbo_raster_respecified;
    /* Location queries (ph120.a loc(a,u,h,m)): guest glGetAttribLocation and
     * glGetUniformLocation requests, and how many of them the shim-side
     * (program, generation, name) cache answered without entering vitaGL
     * (ISAAC_VITA_GL_LOCATION_CACHE; zero otherwise). */
    uint32_t attrib_location;
    uint32_t uniform_location;
    uint32_t location_cache_hit;
    /* ISAAC_VITA_GL_LOCATION_CACHE_VERIFY only: hits whose cached answer
     * differed from a shadow native query (must stay zero; zero otherwise). */
    uint32_t location_cache_mismatch;
} IsaacVitaGlPhaseProfileCounters;

/* Diagnostic-only texture lifecycle and upload-path census.  Gen timing
 * brackets native glGenTextures after its safe pre-read and before lifecycle
 * classification.  Image timing brackets IO/FXRay/native dispatch but ends
 * before classification.  Delete-native timing brackets only native
 * glDeleteTextures; delete-post timing brackets only the profiler's lifecycle
 * bookkeeping after that call.  Every interval uses CPU process time, includes
 * two clock calls, and excludes asynchronous GPU completion; timings are
 * screening upper bounds, not CPU-cycle measurements.  First/redefine and byte
 * fields classify requests, not successful native allocations; logical-live
 * counts guest name lifetime, not deferred native-slot availability.  A
 * nonzero gen_ambiguous makes subsequent lifecycle, first/redefine and
 * recycled-name counts lower bounds: an unadopted successful same-ID recycle
 * remains unknown until the next full profiler reset.  Later windows retain a
 * nonzero bad marker even though gen_ambiguous itself belongs to the event
 * window.  The frozen guest GL dispatcher serializes wrapper
 * mutations; the phase-loop snapshot shares the existing main-thread
 * ownership assumption. */
typedef struct IsaacVitaTextureChurnProfile {
    uint32_t gen_calls;
    uint32_t gen_names;
    uint32_t gen_ambiguous;
    uint32_t gen_observed_us;
    uint32_t gen_scan_slots;
    uint32_t delete_calls;
    uint32_t delete_names;
    uint32_t delete_native_observed_us;
    uint32_t delete_native_max_observed_us;
    uint32_t delete_post_observed_us;
    uint32_t delete_post_max_observed_us;
    uint32_t image_first;
    uint32_t image_redefine;
    uint32_t image_unknown;
    uint32_t image_other_level;
    uint32_t path_linear;
    uint32_t path_converted;
    uint32_t path_other;
    uint32_t known_pixels;
    uint32_t known_alloc_bytes;
    uint32_t image_observed_us;
    uint32_t image_max_observed_us;
    uint32_t logical_live;
    uint32_t window_peak_live;
    uint32_t recycled_names;
    uint32_t bad;
    uint32_t clock_calls;
    uint32_t clock_pair_max_us;
} IsaacVitaTextureChurnProfile;

_Static_assert(sizeof(IsaacVitaTextureChurnProfile) == 28u * sizeof(uint32_t),
               "texture-churn profile ABI drifted");

#if defined(ISAAC_VITA_PHASE_PROFILE)
extern IsaacVitaGlPhaseProfileCounters
    g_isaac_vita_gl_phase_profile_counters;
#endif

#if defined(ISAAC_VITA_GL_TIME_PROFILE)
/* Wall time spent inside native vitaGL calls, split by what the call makes
 * GXM do.  Measured on the device: Game::Render is a 7 ms CPU floor that does
 * not move with display resolution, so the only way to attribute it is to
 * bracket every native boundary call with two process-clock reads (about
 * 0.3 us each on the 500 MHz A9; the bracket itself is charged to the bucket,
 * never to the guest).  rnd minus the sum of the six buckets is therefore the
 * translated guest code plus our own boundary bookkeeping.
 *   draw      glDrawElements / vglIsaacDrawCanonicalQuads
 *   clear     glClear (a full-target quad in vitaGL; may open a scene)
 *   bind_fb   glBindFramebuffer + glFramebufferTexture2D
 *   tex_upload glTexImage2D + glTexSubImage2D
 *   state     program/uniform/attrib/texture/blend/depth/viewport/enable
 *   present   vglSwapBuffers (also the ph120.t swp phase)
 * Totals are per 120-loop window and are zeroed by the phase profiler when
 * it reads them; a 2 s window cannot overflow 32 bits of microseconds. */
typedef struct IsaacVitaGlTimeBucket {
    uint32_t calls;
    uint32_t total_us;
    uint32_t max_us;
} IsaacVitaGlTimeBucket;

typedef struct IsaacVitaGlTimeProfile {
    IsaacVitaGlTimeBucket draw;
    IsaacVitaGlTimeBucket clear;
    IsaacVitaGlTimeBucket bind_fb;
    IsaacVitaGlTimeBucket tex_upload;
    IsaacVitaGlTimeBucket state;
    IsaacVitaGlTimeBucket present;
    uint32_t bad_clock;
} IsaacVitaGlTimeProfile;

extern IsaacVitaGlTimeProfile g_isaac_vita_gl_time_profile;

static inline void isaac_vita_gl_time_add(
    IsaacVitaGlTimeBucket *bucket, uint64_t started_at, uint64_t ended_at)
{
    uint64_t elapsed;

    if (ended_at < started_at) {
        ++g_isaac_vita_gl_time_profile.bad_clock;
        return;
    }
    elapsed = ended_at - started_at;
    if (elapsed > UINT32_MAX)
        elapsed = UINT32_MAX;
    ++bucket->calls;
    bucket->total_us += (uint32_t)elapsed;
    if ((uint32_t)elapsed > bucket->max_us)
        bucket->max_us = (uint32_t)elapsed;
}
#endif

/* Preserve current requested GL state but start a fresh adjacency/run census. */
void gl_vita_backend_phase_profile_window_boundary(void);

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
/* Call immediately before vglSwapBuffers: the present ends the GXM scene, so
 * a depth/stencil clear still owed to an offscreen target is dropped exactly
 * as stock vitaGL would have lost it. */
void gl_vita_backend_fbo_present(void);
#endif

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE) || \
    defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
/* Copies and clears the completed-window counters while preserving the
 * logical texture-name lifecycle needed to classify the next window. */
void gl_vita_backend_texture_churn_profile_take_window(
    IsaacVitaTextureChurnProfile *profile);
#endif

#ifndef ISAAC_VITAGL_DISPLAY_SURFACE_STATUS_DEFINED
#define ISAAC_VITAGL_DISPLAY_SURFACE_STATUS_DEFINED 1
#define ISAAC_VITAGL_DISPLAY_SURFACE_COUNT 3u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_NONE        0u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_ALLOC       1u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_GET_BASE    2u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_MAP         3u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_COLOR_INIT  4u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_SYNC_CREATE 5u
typedef struct IsaacVitaGlDisplaySurfaceStatus {
    uint32_t size;
    uint32_t display_size;
    uint32_t buffer_count;
    uint32_t dedicated_count;
    uint32_t system_app_mode;
    uint32_t failure_stage;
    uint32_t failure_index;
    int32_t alloc_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t get_base_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t map_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    uint32_t dedicated[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t color_init_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t sync_create_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
} IsaacVitaGlDisplaySurfaceStatus;
_Static_assert(sizeof(IsaacVitaGlDisplaySurfaceStatus) == 100u,
               "vitaGL display-surface status ABI drifted");
#endif

/* vitaGL calls the strong status endpoint synchronously during initialization.
 * The getter lets the KAGE owner fail before loading if the A/B did not reach
 * three dedicated, mapped, initialized scanout buffers. */
void isaac_vita_vitagl_display_surface_status(
    const IsaacVitaGlDisplaySurfaceStatus *status);
void isaac_vita_vitagl_display_surface_lifecycle_failure(
    const char *site, int32_t result);
int gl_vita_backend_get_display_surface_status(
    IsaacVitaGlDisplaySurfaceStatus *status);

#ifndef ISAAC_VITAGL_RT_TELEMETRY_DEFINED
#define ISAAC_VITAGL_RT_TELEMETRY_DEFINED 1
typedef struct IsaacVitaGlRenderTargetTelemetry {
    const char *site;
    int32_t first_result;
    int32_t retry_result;
    int32_t finish_result;
    uint32_t finish_called;
    int32_t destroy_result;
    const void *first_target;
    const void *retry_target;
    uint32_t retry_attempted;
    uint32_t pool_full;
    uint32_t pending_duplicate;
    uint32_t invariant_failure;
    uint32_t pending[4];
    uint32_t drained_targets;
    uint32_t live_targets;
    uint32_t reference_sum;
    uint32_t width;
    uint32_t height;
    uint32_t frame;
    uint32_t recovered;
} IsaacVitaGlRenderTargetTelemetry;
#endif

/* Exact 32-bit worst case for the bounded durable record emitted by the
 * project-side endpoint (42-byte site plus UINT32_MAX in every field). */
#define ISAAC_VITAGL_RT_EVENT_MAX_BODY 381u

void isaac_vita_vitagl_render_target_event(
    const IsaacVitaGlRenderTargetTelemetry *event);

/* Install every symbol from the frozen typed surface that the Vita backend
 * can implement.  The 11 remaining desktop-only symbols stay NULL and
 * therefore fault by exact name in gl_bridge.c. */
int         gl_vita_backend_install(void);
void        gl_vita_backend_uninstall(void);
int         gl_vita_backend_installed(void);
size_t      gl_vita_backend_resolved_count(void);
size_t      gl_vita_backend_missing_count(void);
const char *gl_vita_backend_missing_symbol(size_t index);
const char *gl_vita_backend_last_error(void);

#if defined(ISAAC_VITA_VITAGL_SHADER_CACHE)
/* Emit one aggregate snapshot at the next game-present boundary, never from
 * a shader compile or display callback. */
void gl_vita_backend_shader_cache_report(void);
#endif

/* High-level KAGE creates its initial depth renderbuffer through raw vitaGL
 * calls before guest GL dispatch starts.  Register that exact native object
 * here so the typed glGetRenderbufferParameteriv emulation observes the same
 * lifecycle as objects created through glGenRenderbuffers. */
int gl_vita_backend_register_renderbuffer(
    uint32_t renderbuffer, uint32_t internal_format,
    int32_t width, int32_t height);
void gl_vita_backend_unregister_renderbuffer(uint32_t renderbuffer);

#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
/* The legacy bounded presentation experiment mutates three finite windows.
 * ISAAC_VITA_DIRECT_DEFAULT instead changes the frozen Render CFG itself;
 * with that mode selected this owner retains only passive readback, queue,
 * and GL-call observations. */
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
#define ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT 120u
#define ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT  240u
#define ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT 360u

static inline int isaac_vita_first_frame_postprocess_bypass_active(
    uint32_t present_index)
{
    return present_index >= ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT &&
        present_index < ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT;
}

static inline int isaac_vita_first_frame_postprocess_bypass_complete(
    uint32_t successful_present_count)
{
    return successful_present_count ==
        ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT;
}

static inline int isaac_vita_first_frame_postprocess_restored(
    uint32_t successful_present_count)
{
    return successful_present_count ==
        ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT + 1u;
}
#endif

/* Kept byte-for-byte in sync with the optional vitaGL queue hook.  The
 * runtime header appears before vitaGL.h, so the shared guard prevents a
 * second typedef regardless of include order. */
#ifndef ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED
#define ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED 1
#define ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT 8u
#define ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT 1u
#define ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_DISPLAY_CALLBACK 2u
#define ISAAC_VITAGL_DISPLAY_LINEAGE_BEGIN_MISMATCH 0x00000001u
#define ISAAC_VITAGL_DISPLAY_LINEAGE_END_MISMATCH   0x00000002u
typedef struct IsaacVitaGlDisplayQueueProbe {
    uint32_t size;
    uint32_t stage;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    union {
        struct {
            int32_t result;
            uint32_t old_sync;
            uint32_t new_sync;
            uint32_t begin_context;
            uint32_t begin_render_target;
            uint32_t begin_fragment_sync;
            uint32_t begin_color_surface;
            uint32_t begin_color_data;
            int32_t begin_result;
            uint32_t begin_count;
            uint32_t end_context;
            int32_t end_result;
            uint32_t end_count;
            uint32_t mismatch_mask;
            int32_t back_memblock_uid;
            int32_t back_get_base_result;
            int32_t back_map_result;
            uint32_t back_dedicated;
        } add;
        struct {
            int32_t result;
            uint32_t size;
            uint32_t base;
            uint32_t pitch;
            uint32_t pixel_format;
            uint32_t width;
            uint32_t height;
            uint32_t sync;
            uint32_t sample_count;
            uint32_t sparse_hash;
            uint32_t rgba[ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT];
        } display;
    } detail;
} IsaacVitaGlDisplayQueueProbe;
#if defined(__cplusplus)
static_assert(sizeof(IsaacVitaGlDisplayQueueProbe) == 96u,
              "vitaGL display-lineage ABI drifted");
#else
_Static_assert(sizeof(IsaacVitaGlDisplayQueueProbe) == 96u,
               "vitaGL display-lineage ABI drifted");
#endif
#endif

void isaac_vitagl_display_queue_probe(
    const IsaacVitaGlDisplayQueueProbe *event);

#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
/* Kept byte-for-byte in sync with the optional vitaGL p2 transfer hook. */
#ifndef ISAAC_VITAGL_KNOWN_COLOR_PROBE_DEFINED
#define ISAAC_VITAGL_KNOWN_COLOR_PROBE_DEFINED 1
#define ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX 2u
#define ISAAC_VITAGL_KNOWN_COLOR_FILL          0xffff00ffu
#define ISAAC_VITAGL_KNOWN_COLOR_FORMAT        0x00060000u
#define ISAAC_VITAGL_KNOWN_COLOR_X             64u
#define ISAAC_VITAGL_KNOWN_COLOR_Y             64u
#define ISAAC_VITAGL_KNOWN_COLOR_WIDTH         512u
#define ISAAC_VITAGL_KNOWN_COLOR_HEIGHT        256u
#define ISAAC_VITAGL_KNOWN_COLOR_NOT_CALLED    ((int32_t)0x80000000u)
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_TAG        0x00000001u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_NON_SYSTEM 0x00000002u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_COUNT      0x00000004u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_BACK       0x00000008u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_ADDRESS    0x00000010u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_DEDICATED  0x00000020u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_BEGIN_ONE  0x00000040u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_END_ONE    0x00000080u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_BEGIN_OK   0x00000100u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_END_OK     0x00000200u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_CONTEXT    0x00000400u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_DATA       0x00000800u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_SYNC       0x00001000u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_MISMATCH   0x00002000u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_ALL        0x00003fffu
typedef struct IsaacVitaGlKnownColorProbe {
    uint32_t size;
    uint32_t present_index;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    uint32_t dedicated;
    uint32_t color;
    uint32_t format;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t sync_object;
    uint32_t sync_flags;
    uint32_t gate_mask;
    uint32_t context_finish_called;
    int32_t fill_result;
    uint32_t transfer_finish_called;
    int32_t transfer_finish_result;
    int32_t queue_result;
} IsaacVitaGlKnownColorProbe;
#if defined(__cplusplus)
static_assert(sizeof(IsaacVitaGlKnownColorProbe) == 88u,
              "vitaGL known-color ABI drifted");
#else
_Static_assert(sizeof(IsaacVitaGlKnownColorProbe) == 88u,
               "vitaGL known-color ABI drifted");
#endif
#endif

/* The query is valid only while the game owner brackets vglSwapBuffers.
 * The event endpoint is a fixed bounded copy: no logging, allocation, GL,
 * or waiting is legal inside the vitaGL hook. */
uint32_t isaac_vita_vitagl_known_color_present_index(void);
void isaac_vita_vitagl_known_color_probe(
    const IsaacVitaGlKnownColorProbe *event);
#endif

typedef struct IsaacVitaFirstFrameSnapshot {
    uint32_t current_guest_fbo;
    uint32_t manager_fbo;
    /* Last texture argument observed at the manager FBO's COLOR_ATTACHMENT0
     * boundary.  vitaGL's void call cannot prove the resulting attachment. */
    uint32_t manager_color_attach_arg;
    /* Total matching calls, including the first attachment. */
    uint32_t manager_color_attach_calls;
    uint32_t bind_zero;
    uint32_t bind_nonzero;
    uint32_t clear_default;
    uint32_t clear_offscreen;
    uint32_t draw_default;
    uint32_t draw_offscreen;
    uint32_t last_draw_fbo;
    uint32_t control_clears;
    uint32_t blit_attempts;
    uint32_t blit_successes;
    uint32_t blit_no_manager;
    uint32_t blit_incomplete;
} IsaacVitaFirstFrameSnapshot;

void gl_vita_backend_first_frame_set_manager_framebuffer(
    uint32_t framebuffer);
void gl_vita_backend_first_frame_before_present(uint32_t present_index);
void gl_vita_backend_first_frame_queue_begin(uint32_t present_index);
void gl_vita_backend_first_frame_queue_end(void);
void gl_vita_backend_first_frame_snapshot(
    IsaacVitaFirstFrameSnapshot *snapshot);
#endif

#endif
